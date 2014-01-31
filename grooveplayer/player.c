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
#include <portaudio.h>
#include <pthread.h>
#include <string.h>

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

    PaStream *stream;
    struct GrooveSink *sink;

    struct GrooveQueue *eventq;
};

static PaSampleFormat groove_fmt_to_portaudio_fmt(enum GrooveSampleFormat fmt) {
    switch (fmt) {
        case GROOVE_SAMPLE_FMT_U8:
            return paUInt8;
        case GROOVE_SAMPLE_FMT_S16:
            return paInt16;
        case GROOVE_SAMPLE_FMT_S32:
            return paInt32;
        case GROOVE_SAMPLE_FMT_FLT:
            return paFloat32;

        case GROOVE_SAMPLE_FMT_U8P:
            return paUInt8|paNonInterleaved;
        case GROOVE_SAMPLE_FMT_S16P:
            return paInt16|paNonInterleaved;
        case GROOVE_SAMPLE_FMT_S32P:
            return paInt32|paNonInterleaved;
        case GROOVE_SAMPLE_FMT_FLTP:
            return paFloat32|paNonInterleaved;

        case GROOVE_SAMPLE_FMT_DBLP:
            av_log(NULL, AV_LOG_ERROR,
                "unable to use selected format. using GROOVE_SAMPLE_FMT_S16P instead.\n");
            return paInt16|paNonInterleaved;

        default:
            av_log(NULL, AV_LOG_ERROR,
                "unable to use selected format. using GROOVE_SAMPLE_FMT_S16 instead.\n");
            return paInt16;

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

static int portaudio_audio_callback(const void *input, void *output_void,
        unsigned long len, const PaStreamCallbackTimeInfo *timeInfo,
        PaStreamCallbackFlags statusFlags, void *opaque)
{
    struct GroovePlayerPrivate *p = opaque;

    struct GrooveSink *sink = p->sink;
    struct GroovePlaylist *playlist = sink->playlist;

    double bytes_per_sec = sink->bytes_per_sec;
    int paused = !groove_playlist_playing(playlist);

    unsigned char *output = (unsigned char *) output_void;

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
            memset(output, 0, len);
            break;
        }
        size_t len1 = p->audio_buf_size - p->audio_buf_index;
        if (len1 > len)
            len1 = len;
        memcpy(output, p->audio_buf->data[0] + p->audio_buf_index, len1);
        len -= len1;
        output += len1;
        p->audio_buf_index += len1;
        p->play_pos += len1 / bytes_per_sec;
    }

    pthread_mutex_unlock(&p->play_head_mutex);
    return paContinue;
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
        emit_event(p->eventq, GROOVE_EVENT_NOWPLAYING);
    }

    pthread_mutex_unlock(&p->play_head_mutex);
}

static void sink_flush(struct GrooveSink *sink) {
    struct GroovePlayerPrivate *p = sink->userdata;

    pthread_mutex_lock(&p->play_head_mutex);

    groove_buffer_unref(p->audio_buf);
    p->audio_buf = NULL;
    p->audio_buf_index = 0;
    p->audio_buf_size = 0;

    pthread_mutex_unlock(&p->play_head_mutex);
}

struct GroovePlayer *groove_player_create(void) {
    struct GroovePlayerPrivate *p = av_mallocz(sizeof(struct GroovePlayerPrivate));

    if (!p) {
        av_log(NULL, AV_LOG_ERROR, "unable to create player: out of memory\n");
        return NULL;
    }

    PaError err = Pa_Initialize();
    if (err != paNoError) {
        av_free(p);
        av_log(NULL, AV_LOG_ERROR, "unable to init PortAudio: %s\n",
                Pa_GetErrorText(err));
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

    if (pthread_mutex_init(&p->play_head_mutex, NULL) != 0) {
        groove_player_destroy(player);
        av_log(NULL, AV_LOG_ERROR,"unable to create play head mutex: out of memory\n");
        return NULL;
    }
    p->play_head_mutex_inited = 1;

    p->eventq = groove_queue_create();
    if (!p->eventq) {
        groove_player_destroy(player);
        av_log(NULL, AV_LOG_ERROR,"unable to create event queue: out of memory\n");
        return NULL;
    }

    // set some nice defaults
    player->target_audio_format.sample_rate = -1; // find out default in attach
    player->target_audio_format.channel_layout = GROOVE_CH_LAYOUT_STEREO;
    player->target_audio_format.sample_fmt = GROOVE_SAMPLE_FMT_S16;
    player->device_buffer_size = -1; // find out default in attach
    player->sink_buffer_size = 8192;

    return player;
}

void groove_player_destroy(struct GroovePlayer *player) {
    if (!player)
        return;

    PaError err = Pa_Terminate();
    if (err != paNoError) {
        av_log(NULL, AV_LOG_WARNING, "unable to terminate PortAudio: %s\n",
                Pa_GetErrorText(err));
    }

    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;

    if (p->play_head_mutex_inited)
        pthread_mutex_destroy(&p->play_head_mutex);

    if (p->eventq)
        groove_queue_destroy(p->eventq);

    groove_sink_destroy(p->sink);

    av_free(p);
}

int groove_player_attach(struct GroovePlayer *player, struct GroovePlaylist *playlist) {
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;

    // deprecation: on the next version bump, replace this with an index.
    // figure out which device index has the name player->device_name
    PaDeviceIndex device_index = -1;
    const PaDeviceInfo *device_info = NULL;
    if (player->device_name) {
        int device_count = Pa_GetDeviceCount();
        for (int i = 0; i < device_count; i += 1) {
            device_info = Pa_GetDeviceInfo(i);
            if (strcmp(device_info->name, player->device_name) == 0) {
                device_index = i;
                break;
            }
        }
    }
    if (device_index == -1) {
        device_index = Pa_GetDefaultOutputDevice();
        device_info = Pa_GetDeviceInfo(device_index);
    }

    player->actual_audio_format = player->target_audio_format;

    if (player->actual_audio_format.sample_rate == -1) {
        player->actual_audio_format.sample_rate = device_info->defaultSampleRate;
    }

    // TODO switch to a fallback sample format if the device reports it cannot open it

    double latency;
    if (player->device_buffer_size == -1) {
        latency = device_info->defaultHighOutputLatency;
        player->device_buffer_size = latency * player->actual_audio_format.sample_rate;
    } else {
        latency = player->device_buffer_size / (double) player->target_audio_format.sample_rate;
    }

    PaStreamParameters params;
    params.device = device_index;
    params.channelCount = groove_channel_layout_count(player->actual_audio_format.channel_layout);
    params.sampleFormat = groove_fmt_to_portaudio_fmt(player->actual_audio_format.sample_fmt);
    params.suggestedLatency = latency;
    params.hostApiSpecificStreamInfo = NULL;

    PaStreamFlags flags = paClipOff|paDitherOff;
    int err = Pa_OpenStream(&p->stream, NULL, &params, player->actual_audio_format.sample_rate,
            paFramesPerBufferUnspecified, flags, portaudio_audio_callback, player);
    if (err != paNoError) {
        p->stream = NULL;
        av_log(NULL, AV_LOG_ERROR, "unable to open audio device: %s\n", Pa_GetErrorText(err));
        return -1;
    }

    // based on spec that we got, attach a sink with those properties
    p->sink->buffer_size = player->sink_buffer_size;
    p->sink->audio_format = player->actual_audio_format;

    if (p->sink->audio_format.sample_fmt == GROOVE_SAMPLE_FMT_NONE) {
        groove_player_detach(player);
        av_log(NULL, AV_LOG_ERROR, "unsupported audio device sample format\n");
        return -1;
    }

    err = groove_sink_attach(p->sink, playlist);
    if (err < 0) {
        groove_player_detach(player);
        av_log(NULL, AV_LOG_ERROR, "unable to attach sink\n");
        return err;
    }

    p->play_pos = -1.0;

    groove_queue_reset(p->eventq);

    Pa_StartStream(p->stream);

    return 0;
}

int groove_player_detach(struct GroovePlayer *player) {
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;
    if (p->eventq) {
        groove_queue_flush(p->eventq);
        groove_queue_abort(p->eventq);
    }
    if (p->sink->playlist) {
        groove_sink_detach(p->sink);
    }
    if (p->stream) {
        PaErrorCode err = Pa_StopStream(p->stream);
        if (err != paNoError) {
            av_log(NULL, AV_LOG_ERROR, "unable to stop stream: %s\n",
                    Pa_GetErrorText(err));
        }
        err = Pa_CloseStream(p->stream);
        if (err != paNoError) {
            av_log(NULL, AV_LOG_ERROR, "unable to close stream: %s\n",
                    Pa_GetErrorText(err));
        }
        p->stream = NULL;
    }
    player->playlist = NULL;

    groove_buffer_unref(p->audio_buf);
    p->audio_buf = NULL;

    return 0;
}

int groove_device_count(void) {
    return Pa_GetDeviceCount();
}

const char *groove_device_name(int index) {
    const PaDeviceInfo *info = Pa_GetDeviceInfo(index);
    return info->name;
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
