#include "groove.h"
#include "queue.h"

#include <libavutil/mem.h>
#include <libavutil/log.h>
#include <SDL2/SDL_audio.h>

typedef struct GroovePlayerPrivate {
    GrooveBuffer *audio_buf;
    size_t audio_buf_size; // in bytes
    size_t audio_buf_index; // in bytes

    // this mutex applies to the variables in this block
    SDL_mutex *play_head_mutex;
    // pointer to current item where the buffered audio is reaching the device
    GroovePlaylistItem *play_head;
    // number of seconds into the play_head song where the buffered audio
    // is reaching the device
    double play_pos;

    SDL_AudioDeviceID device_id;
    GrooveSink *sink;

    // only touched by sdl_audio_callback, tells whether we have reached end
    // of audio queue naturally rather than a buffer underrun
    int end_of_q;

    GrooveQueue *eventq;
} GroovePlayerPrivate;

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

static void emit_event(GrooveQueue *queue, enum GrooveEventType type) {
    GrooveEvent *evt = av_malloc(sizeof(GrooveEvent));
    if (!evt) {
        av_log(NULL, AV_LOG_ERROR, "unable to create event: out of memory\n");
        return;
    }
    evt->type = type;
    if (groove_queue_put(queue, evt) < 0)
        av_log(NULL, AV_LOG_ERROR, "unable to put event on queue: out of memory\n");
}


static void sdl_audio_callback(void *opaque, Uint8 *stream, int len) {
    GroovePlayer *player = opaque;
    GroovePlayerPrivate *p = player->internals;

    GrooveSink *sink = p->sink;
    GroovePlaylist *playlist = sink->playlist;

    double bytes_per_sec = sink->bytes_per_sec;
    int paused = !groove_playlist_playing(playlist);

    SDL_LockMutex(p->play_head_mutex);

    while (len > 0) {
        if (!paused && p->audio_buf_index >= p->audio_buf_size) {
            groove_buffer_unref(p->audio_buf);
            p->audio_buf_index = 0;
            p->audio_buf_size = 0;

            int ret = groove_sink_get_buffer(p->sink, &p->audio_buf, 0);
            if (ret == GROOVE_BUFFER_END) {
                emit_event(p->eventq, GROOVE_EVENT_NOWPLAYING);

                p->end_of_q = 1;
                p->play_head = NULL;
                p->play_pos = -1.0;
            } else if (ret == GROOVE_BUFFER_YES) {
                if (p->play_head != p->audio_buf->item)
                    emit_event(p->eventq, GROOVE_EVENT_NOWPLAYING);

                p->end_of_q = 0;
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

    SDL_UnlockMutex(p->play_head_mutex);
}

static void sink_purge(GrooveSink *sink, GroovePlaylistItem *item) {
    GroovePlayer *player = sink->userdata;
    GroovePlayerPrivate *p = player->internals;

    SDL_LockMutex(p->play_head_mutex);

    if (p->play_head == item) {
        p->play_head = NULL;
        p->play_pos = -1.0;
        groove_buffer_unref(p->audio_buf);
        p->audio_buf = NULL;
        p->audio_buf_index = 0;
        p->audio_buf_size = 0;
        emit_event(p->eventq, GROOVE_EVENT_NOWPLAYING);
    }

    SDL_UnlockMutex(p->play_head_mutex);
}

static void sink_flush(GrooveSink *sink) {
    GroovePlayer *player = sink->userdata;
    GroovePlayerPrivate *p = player->internals;

    SDL_LockMutex(p->play_head_mutex);

    groove_buffer_unref(p->audio_buf);
    p->audio_buf = NULL;
    p->audio_buf_index = 0;
    p->audio_buf_size = 0;

    SDL_UnlockMutex(p->play_head_mutex);
}

GroovePlayer * groove_player_create() {
    GroovePlayer *player = av_mallocz(sizeof(GroovePlayer));
    GroovePlayerPrivate *p = av_mallocz(sizeof(GroovePlayerPrivate));

    if (!player || !p) {
        av_free(player);
        av_free(p);
        av_log(NULL, AV_LOG_ERROR, "unable to create player: out of memory\n");
        return NULL;
    }

    player->internals = p;

    p->sink = groove_sink_create();
    if (!p->sink) {
        groove_player_destroy(player);
        av_log(NULL, AV_LOG_ERROR,"unable to create sink: out of memory\n");
        return NULL;
    }

    p->sink->userdata = player;
    p->sink->purge = sink_purge;
    p->sink->flush = sink_flush;

    p->play_head_mutex = SDL_CreateMutex();
    if (!p->play_head_mutex) {
        groove_player_destroy(player);
        av_log(NULL, AV_LOG_ERROR,"unable to create play head mutex: out of memory\n");
        return NULL;
    }

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
    player->memory_buffer_size = 8192;

    return player;
}

void groove_player_destroy(GroovePlayer *player) {
    if (!player)
        return;

    GroovePlayerPrivate *p = player->internals;

    if (p->play_head_mutex)
        SDL_DestroyMutex(p->play_head_mutex);

    if (p->eventq)
        groove_queue_destroy(p->eventq);

    groove_sink_destroy(p->sink);

    av_free(p);
    av_free(player);
}

int groove_player_attach(GroovePlayer *player, GroovePlaylist *playlist) {
    GroovePlayerPrivate *p = player->internals;

    SDL_AudioSpec wanted_spec, spec;
    wanted_spec.format = groove_fmt_to_sdl_fmt(player->target_audio_format.sample_fmt);
    wanted_spec.freq = player->target_audio_format.sample_rate;
    wanted_spec.channels = groove_channel_layout_count(player->target_audio_format.channel_layout);
    wanted_spec.samples = player->device_buffer_size;
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = player;

    p->device_id = SDL_OpenAudioDevice(player->device_name, 0, &wanted_spec,
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
    p->sink->buffer_size = player->memory_buffer_size;
    p->sink->audio_format = player->actual_audio_format;

    if (p->sink->audio_format.sample_fmt == GROOVE_SAMPLE_FMT_NONE) {
        groove_player_detach(player);
        av_log(NULL, AV_LOG_ERROR, "unsupported audio device sample format\n");
        return -1;
    }

    int err = groove_sink_attach(p->sink, playlist);
    if (err < 0) {
        groove_player_detach(player);
        av_log(NULL, AV_LOG_ERROR, "unable to attach sink\n");
        return err;
    }

    p->play_pos = -1.0;

    groove_queue_reset(p->eventq);

    SDL_PauseAudioDevice(p->device_id, 0);

    return 0;
}

int groove_player_detach(GroovePlayer *player) {
    GroovePlayerPrivate *p = player->internals;
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
    player->playlist = NULL;

    groove_buffer_unref(p->audio_buf);
    p->audio_buf = NULL;

    return 0;
}

int groove_device_count() {
    return SDL_GetNumAudioDevices(0);
}

const char * groove_device_name(int index) {
    return SDL_GetAudioDeviceName(index, 0);
}

void groove_player_position(GroovePlayer *player,
        GroovePlaylistItem **item, double *seconds)
{
    GroovePlayerPrivate *p = player->internals;

    SDL_LockMutex(p->play_head_mutex);

    if (item)
        *item = p->play_head;

    if (seconds)
        *seconds = p->play_pos;

    SDL_UnlockMutex(p->play_head_mutex);
}

int groove_player_event_get(GroovePlayer *player, GrooveEvent *event,
        int block)
{
    GroovePlayerPrivate *p = player->internals;
    GrooveEvent *tmp;
    int err = groove_queue_get(p->eventq, (void **)&tmp, block);
    if (err > 0) {
        *event = *tmp;
        av_free(tmp);
    }
    return err;
}

int groove_player_event_peek(GroovePlayer *player, int block) {
    GroovePlayerPrivate *p = player->internals;
    return groove_queue_peek(p->eventq, block);
}
