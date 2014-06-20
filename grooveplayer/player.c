/*
 * Copyright (c) 2013 Andrew Kelley
 *
 * This file is part of libgroove, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "player.h"
#include <groove/queue.h>

#include <libavutil/mem.h>
#include <libavutil/log.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_audio.h>
#include <pthread.h>
#include <time.h>

struct GroovePlayerPrivate {
    struct GroovePlayer externals;
    struct GrooveBuffer *audio_buf;
    size_t audio_buf_size; // in bytes
    size_t audio_buf_index; // in bytes

    // this mutex applies to the variables in this block
    pthread_mutex_t play_head_mutex;
    char play_head_mutex_inited;
    // pointer to current item where the buffered audio is reaching the device
    struct GroovePlaylistItem *play_head;
    // number of seconds into the play_head song where the buffered audio
    // is reaching the device
    double play_pos;

    SDL_AudioDeviceID device_id;
    struct GrooveSink *sink;

    struct GrooveQueue *eventq;

    // for dummy player
    pthread_t thread_id;
    int thread_inited;
    int abort_request;
    uint64_t start_nanos;
    uint64_t frames_consumed;
    pthread_cond_t pause_cond;
    pthread_condattr_t cond_attr;
    int pause_cond_inited;
    int paused;
};

static Uint16 groove_fmt_to_sdl_fmt(enum GrooveSampleFormat fmt) {
    switch (fmt) {
        case GROOVE_SAMPLE_FMT_U8:
            return AUDIO_U8;
        case GROOVE_SAMPLE_FMT_S16:
            return AUDIO_S16SYS;
        case GROOVE_SAMPLE_FMT_S32:
            return AUDIO_S32SYS;
        case GROOVE_SAMPLE_FMT_FLT:
            return AUDIO_F32SYS;
        default:
            av_log(NULL, AV_LOG_ERROR,
                "unable to use selected format. using GROOVE_SAMPLE_FMT_S16 instead.\n");
            return AUDIO_S16SYS;
    }
}

static enum GrooveSampleFormat sdl_fmt_to_groove_fmt(Uint16 sdl_format) {
    switch (sdl_format) {
        case AUDIO_U8:
            return GROOVE_SAMPLE_FMT_U8;
        case AUDIO_S16SYS:
            return GROOVE_SAMPLE_FMT_S16;
        case AUDIO_S32SYS:
            return GROOVE_SAMPLE_FMT_S32;
        case AUDIO_F32SYS:
            return GROOVE_SAMPLE_FMT_FLT;
        default:
            return GROOVE_SAMPLE_FMT_NONE;
    }
}

static void emit_event(struct GrooveQueue *queue, enum GroovePlayerEventType type) {
    union GroovePlayerEvent *evt = av_malloc(sizeof(union GroovePlayerEvent));
    if (!evt) {
        av_log(NULL, AV_LOG_ERROR, "unable to create event: out of memory\n");
        return;
    }
    evt->type = type;
    if (groove_queue_put(queue, evt) < 0)
        av_log(NULL, AV_LOG_ERROR, "unable to put event on queue: out of memory\n");
}

static uint64_t now_nanos(void) {
    struct timespec tms;
    clock_gettime(CLOCK_MONOTONIC, &tms);
    uint64_t tv_sec = tms.tv_sec;
    uint64_t sec_mult = 1000000000;
    uint64_t tv_nsec = tms.tv_nsec;
    return tv_sec * sec_mult + tv_nsec;
}

// this thread is started if the user selects a dummy device instead of a
// real device.
static void *dummy_thread(void *arg) {
    struct GroovePlayer *player = arg;
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;

    while (!p->abort_request) {
        pthread_mutex_lock(&p->play_head_mutex);
        if (p->paused) {
            pthread_cond_wait(&p->pause_cond, &p->play_head_mutex);
            pthread_mutex_unlock(&p->play_head_mutex);
            continue;
        }

        uint64_t now = now_nanos();
        int more = 1;
        while (more) {
            more = 0;
            if (!p->audio_buf || p->audio_buf_index >= p->audio_buf->frame_count) {
                groove_buffer_unref(p->audio_buf);
                p->audio_buf_index = 0;
                p->audio_buf_size = 0;
                int ret = groove_sink_buffer_get(p->sink, &p->audio_buf, 0);
                if (ret == GROOVE_BUFFER_END) {
                    emit_event(p->eventq, GROOVE_EVENT_NOWPLAYING);
                    p->play_head = NULL;
                    p->play_pos = -1.0;
                } else if (ret == GROOVE_BUFFER_YES) {
                    if (p->play_head != p->audio_buf->item)
                        emit_event(p->eventq, GROOVE_EVENT_NOWPLAYING);

                    p->play_head = p->audio_buf->item;
                    p->play_pos = p->audio_buf->pos;
                    p->audio_buf_size = p->audio_buf->size;
                } else {
                    // since this is a dummy player whose only job is to keep
                    // track of time, we're going to pretend that we did *not*
                    // just get a buffer underrun. Instead we'll wait patiently
                    // for the next buffer to appear and handle it appropriately.
                    pthread_mutex_unlock(&p->play_head_mutex);
                    break;
                }
            }
            if (p->audio_buf) {
                uint64_t nanos_per_frame = 1000000000 / p->audio_buf->format.sample_rate;
                uint64_t total_nanos = now - p->start_nanos;
                uint64_t total_frames = total_nanos / nanos_per_frame;
                int frames_to_kill = total_frames - p->frames_consumed;
                int new_index = p->audio_buf_index + frames_to_kill;
                if (new_index > p->audio_buf->frame_count) {
                    more = 1;
                    new_index = p->audio_buf->frame_count;
                    frames_to_kill = new_index - p->audio_buf_index;
                }
                p->frames_consumed += frames_to_kill;
                p->audio_buf_index = new_index;
                p->play_pos += frames_to_kill / (double) p->audio_buf->format.sample_rate;
            }
        }

        // sleep for a little while
        struct timespec tms;
        clock_gettime(CLOCK_MONOTONIC, &tms);
        tms.tv_nsec += 10000000;
        pthread_cond_timedwait(&p->pause_cond, &p->play_head_mutex, &tms);
        pthread_mutex_unlock(&p->play_head_mutex);
    }
    return NULL;
}

static void sdl_audio_callback(void *opaque, Uint8 *stream, int len) {
    struct GroovePlayerPrivate *p = opaque;

    struct GrooveSink *sink = p->sink;
    struct GroovePlaylist *playlist = sink->playlist;

    double bytes_per_sec = sink->bytes_per_sec;
    int paused = !groove_playlist_playing(playlist);

    pthread_mutex_lock(&p->play_head_mutex);

    while (len > 0) {
        if (!paused && p->audio_buf_index >= p->audio_buf_size) {
            groove_buffer_unref(p->audio_buf);
            p->audio_buf_index = 0;
            p->audio_buf_size = 0;

            int ret = groove_sink_buffer_get(p->sink, &p->audio_buf, 0);
            if (ret == GROOVE_BUFFER_END) {
                emit_event(p->eventq, GROOVE_EVENT_NOWPLAYING);

                p->play_head = NULL;
                p->play_pos = -1.0;
            } else if (ret == GROOVE_BUFFER_YES) {
                if (p->play_head != p->audio_buf->item)
                    emit_event(p->eventq, GROOVE_EVENT_NOWPLAYING);

                p->play_head = p->audio_buf->item;
                p->play_pos = p->audio_buf->pos;
                p->audio_buf_size = p->audio_buf->size;
            } else {
                // errors are treated the same as no buffer ready
                emit_event(p->eventq, GROOVE_EVENT_BUFFERUNDERRUN);
            }
        }
        if (paused || !p->audio_buf) {
            // fill with silence
            memset(stream, 0, len);
            break;
        }
        size_t len1 = p->audio_buf_size - p->audio_buf_index;
        if (len1 > len)
            len1 = len;
        memcpy(stream, p->audio_buf->data[0] + p->audio_buf_index, len1);
        len -= len1;
        stream += len1;
        p->audio_buf_index += len1;
        p->play_pos += len1 / bytes_per_sec;
    }

    pthread_mutex_unlock(&p->play_head_mutex);
}

static void sink_purge(struct GrooveSink *sink, struct GroovePlaylistItem *item) {
    struct GroovePlayerPrivate *p = sink->userdata;

    pthread_mutex_lock(&p->play_head_mutex);

    if (p->play_head == item) {
        p->play_head = NULL;
        p->play_pos = -1.0;
        groove_buffer_unref(p->audio_buf);
        p->audio_buf = NULL;
        p->audio_buf_index = 0;
        p->audio_buf_size = 0;
        p->start_nanos = now_nanos();
        p->frames_consumed = 0;
        emit_event(p->eventq, GROOVE_EVENT_NOWPLAYING);
    }

    pthread_mutex_unlock(&p->play_head_mutex);
}

static void sink_pause(struct GrooveSink *sink) {
    struct GroovePlayer *player = sink->userdata;

    // only the dummy device needs to handle pausing
    if (player->device_index != GROOVE_PLAYER_DUMMY_DEVICE)
        return;

    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;

    // mark the position in time that we paused at.
    // no mutex needed for simple boolean flag
    p->paused = 1;
}

static void sink_play(struct GrooveSink *sink) {
    struct GroovePlayer *player = sink->userdata;

    // only the dummy device needs to handle playing
    if (player->device_index != GROOVE_PLAYER_DUMMY_DEVICE)
        return;

    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;

    // mark the position in time that we started playing at.
    pthread_mutex_lock(&p->play_head_mutex);
    p->start_nanos = now_nanos();
    p->frames_consumed = 0;
    p->paused = 0;
    pthread_cond_signal(&p->pause_cond);
    pthread_mutex_unlock(&p->play_head_mutex);
}

static void sink_flush(struct GrooveSink *sink) {
    struct GroovePlayerPrivate *p = sink->userdata;

    pthread_mutex_lock(&p->play_head_mutex);

    groove_buffer_unref(p->audio_buf);
    p->audio_buf = NULL;
    p->audio_buf_index = 0;
    p->audio_buf_size = 0;
    p->start_nanos = now_nanos();
    p->frames_consumed = 0;
    p->play_pos = -1.0;
    p->play_head = NULL;

    pthread_mutex_unlock(&p->play_head_mutex);
}

struct GroovePlayer *groove_player_create(void) {
    struct GroovePlayerPrivate *p = av_mallocz(sizeof(struct GroovePlayerPrivate));

    if (!p) {
        av_log(NULL, AV_LOG_ERROR, "unable to create player: out of memory\n");
        return NULL;
    }

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        av_free(p);
        av_log(NULL, AV_LOG_ERROR, "unable to init SDL audio subsystem: %s\n",
                SDL_GetError());
        return NULL;
    }

    struct GroovePlayer *player = &p->externals;

    p->sink = groove_sink_create();
    if (!p->sink) {
        groove_player_destroy(player);
        av_log(NULL, AV_LOG_ERROR,"unable to create sink: out of memory\n");
        return NULL;
    }

    p->sink->userdata = player;
    p->sink->purge = sink_purge;
    p->sink->flush = sink_flush;
    p->sink->pause = sink_pause;
    p->sink->play = sink_play;

    if (pthread_mutex_init(&p->play_head_mutex, NULL) != 0) {
        groove_player_destroy(player);
        av_log(NULL, AV_LOG_ERROR,"unable to create play head mutex: out of memory\n");
        return NULL;
    }
    p->play_head_mutex_inited = 1;

    pthread_condattr_init(&p->cond_attr);
    pthread_condattr_setclock(&p->cond_attr, CLOCK_MONOTONIC);
    if (pthread_cond_init(&p->pause_cond, &p->cond_attr) != 0) {
        groove_player_destroy(player);
        av_log(NULL, AV_LOG_ERROR, "unable to create mutex condition\n");
        return NULL;
    }
    p->pause_cond_inited = 1;

    p->eventq = groove_queue_create();
    if (!p->eventq) {
        groove_player_destroy(player);
        av_log(NULL, AV_LOG_ERROR,"unable to create event queue: out of memory\n");
        return NULL;
    }

    // set some nice defaults
    player->target_audio_format.sample_rate = 44100;
    player->target_audio_format.channel_layout = GROOVE_CH_LAYOUT_STEREO;
    player->target_audio_format.sample_fmt = GROOVE_SAMPLE_FMT_S16;
    // small because there is no way to clear the buffer.
    player->device_buffer_size = 1024;
    player->sink_buffer_size = 8192;
    player->gain = p->sink->gain;
    player->device_index = -1; // default device

    return player;
}

void groove_player_destroy(struct GroovePlayer *player) {
    if (!player)
        return;

    SDL_QuitSubSystem(SDL_INIT_AUDIO);

    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;

    if (p->play_head_mutex_inited)
        pthread_mutex_destroy(&p->play_head_mutex);

    if (p->pause_cond_inited)
        pthread_cond_destroy(&p->pause_cond);

    if (p->eventq)
        groove_queue_destroy(p->eventq);

    groove_sink_destroy(p->sink);

    av_free(p);
}

int groove_player_attach(struct GroovePlayer *player, struct GroovePlaylist *playlist) {
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;

    p->sink->gain = player->gain;
    p->sink->buffer_size = player->sink_buffer_size;

    if (player->device_index == GROOVE_PLAYER_DUMMY_DEVICE) {
        // dummy device
        player->actual_audio_format = player->target_audio_format;
        p->sink->audio_format = player->actual_audio_format;
        p->sink->disable_resample = 1;
    } else {
        SDL_AudioSpec wanted_spec, spec;
        wanted_spec.format = groove_fmt_to_sdl_fmt(player->target_audio_format.sample_fmt);
        wanted_spec.freq = player->target_audio_format.sample_rate;
        wanted_spec.channels = groove_channel_layout_count(player->target_audio_format.channel_layout);
        wanted_spec.samples = player->device_buffer_size;
        wanted_spec.callback = sdl_audio_callback;
        wanted_spec.userdata = player;

        const char* device_name = NULL;

        if (player->device_index >= 0)
            device_name = SDL_GetAudioDeviceName(player->device_index, 0);
        p->device_id = SDL_OpenAudioDevice(device_name, 0, &wanted_spec,
                &spec, SDL_AUDIO_ALLOW_ANY_CHANGE);
        if (p->device_id == 0) {
            av_log(NULL, AV_LOG_ERROR, "unable to open audio device: %s\n", SDL_GetError());
            return -1;
        }

        // save the actual spec back into the struct
        player->actual_audio_format.sample_rate = spec.freq;
        player->actual_audio_format.channel_layout = groove_channel_layout_default(spec.channels);
        player->actual_audio_format.sample_fmt = sdl_fmt_to_groove_fmt(spec.format);

        // based on spec that we got, attach a sink with those properties
        p->sink->audio_format = player->actual_audio_format;

        if (p->sink->audio_format.sample_fmt == GROOVE_SAMPLE_FMT_NONE) {
            groove_player_detach(player);
            av_log(NULL, AV_LOG_ERROR, "unsupported audio device sample format\n");
            return -1;
        }
    }

    int err = groove_sink_attach(p->sink, playlist);
    if (err < 0) {
        groove_player_detach(player);
        av_log(NULL, AV_LOG_ERROR, "unable to attach sink\n");
        return err;
    }

    p->play_pos = -1.0;

    groove_queue_reset(p->eventq);

    if (player->device_index == GROOVE_PLAYER_DUMMY_DEVICE) {
        if (groove_playlist_playing(playlist))
            sink_play(p->sink);
        else
            sink_pause(p->sink);

        // set up thread to keep track of time
        if (pthread_create(&p->thread_id, NULL, dummy_thread, player) != 0) {
            groove_player_detach(player);
            av_log(NULL, AV_LOG_ERROR, "unable to create dummy player thread\n");
            return -1;
        }
        p->thread_inited = 1;
    } else {
        SDL_PauseAudioDevice(p->device_id, 0);
    }

    return 0;
}

int groove_player_detach(struct GroovePlayer *player) {
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;
    p->abort_request = 1;
    if (p->eventq) {
        groove_queue_flush(p->eventq);
        groove_queue_abort(p->eventq);
    }
    if (p->sink->playlist) {
        groove_sink_detach(p->sink);
    }
    if (p->device_id > 0) {
        SDL_CloseAudioDevice(p->device_id);
        p->device_id = 0;
    }
    if (p->thread_inited) {
        pthread_cond_signal(&p->pause_cond);
        pthread_join(p->thread_id, NULL);
        p->thread_inited = 0;
    }

    player->playlist = NULL;

    groove_buffer_unref(p->audio_buf);
    p->audio_buf = NULL;

    return 0;
}

int groove_device_count(void) {
    return SDL_GetNumAudioDevices(0);
}

const char * groove_device_name(int index) {
    return SDL_GetAudioDeviceName(index, 0);
}

void groove_player_position(struct GroovePlayer *player,
        struct GroovePlaylistItem **item, double *seconds)
{
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;

    pthread_mutex_lock(&p->play_head_mutex);

    if (item)
        *item = p->play_head;

    if (seconds)
        *seconds = p->play_pos;

    pthread_mutex_unlock(&p->play_head_mutex);
}

int groove_player_event_get(struct GroovePlayer *player,
        union GroovePlayerEvent *event, int block)
{
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;
    union GroovePlayerEvent *tmp;
    int err = groove_queue_get(p->eventq, (void **)&tmp, block);
    if (err > 0) {
        *event = *tmp;
        av_free(tmp);
    }
    return err;
}

int groove_player_event_peek(struct GroovePlayer *player, int block) {
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;
    return groove_queue_peek(p->eventq, block);
}

int groove_player_set_gain(struct GroovePlayer *player, double gain) {
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;
    player->gain = gain;
    return groove_sink_set_gain(p->sink, gain);
}
