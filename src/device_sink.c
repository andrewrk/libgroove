#include "groove.h"
#include "queue.h"

#include <libavutil/mem.h>
#include <libavutil/log.h>
#include <SDL2/SDL_audio.h>

typedef struct GrooveDeviceSinkPrivate {
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
} GrooveDeviceSinkPrivate;

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
    GrooveDeviceSink *device_sink = opaque;
    GrooveDeviceSinkPrivate *ds = device_sink->internals;

    GrooveSink *sink = ds->sink;
    GroovePlayer *player = sink->player;

    double bytes_per_sec = sink->bytes_per_sec;
    int paused = !groove_player_playing(player);

    SDL_LockMutex(ds->play_head_mutex);

    while (len > 0) {
        if (!paused && ds->audio_buf_index >= ds->audio_buf_size) {
            groove_buffer_unref(ds->audio_buf);
            ds->audio_buf_index = 0;
            ds->audio_buf_size = 0;

            int ret = groove_sink_get_buffer(ds->sink, &ds->audio_buf, 0);
            if (ret == GROOVE_BUFFER_END) {
                emit_event(ds->eventq, GROOVE_EVENT_NOWPLAYING);

                ds->end_of_q = 1;
                ds->play_head = NULL;
                ds->play_pos = -1.0;
            } else if (ret == GROOVE_BUFFER_YES) {
                if (ds->play_head != ds->audio_buf->item)
                    emit_event(ds->eventq, GROOVE_EVENT_NOWPLAYING);

                ds->end_of_q = 0;
                ds->play_head = ds->audio_buf->item;
                ds->play_pos = ds->audio_buf->pos;
                ds->audio_buf_size = ds->audio_buf->size;
            } else {
                // errors are treated the same as no buffer ready
                emit_event(ds->eventq, GROOVE_EVENT_BUFFERUNDERRUN);
            }
        }
        if (paused || !ds->audio_buf) {
            // fill with silence
            memset(stream, 0, len);
            break;
        }
        size_t len1 = ds->audio_buf_size - ds->audio_buf_index;
        if (len1 > len)
            len1 = len;
        memcpy(stream, ds->audio_buf->data[0] + ds->audio_buf_index, len1);
        len -= len1;
        stream += len1;
        ds->audio_buf_index += len1;
        ds->play_pos += len1 / bytes_per_sec;
    }

    SDL_UnlockMutex(ds->play_head_mutex);
}

static void sink_purge(GrooveSink *sink, GroovePlaylistItem *item) {
    GrooveDeviceSink *device_sink = sink->userdata;
    GrooveDeviceSinkPrivate *ds = device_sink->internals;

    SDL_LockMutex(ds->play_head_mutex);

    if (ds->play_head == item) {
        ds->play_head = NULL;
        ds->play_pos = -1.0;
        groove_buffer_unref(ds->audio_buf);
        ds->audio_buf = NULL;
        ds->audio_buf_index = 0;
        ds->audio_buf_size = 0;
        emit_event(ds->eventq, GROOVE_EVENT_NOWPLAYING);
    }

    SDL_UnlockMutex(ds->play_head_mutex);
}

static void sink_flush(GrooveSink *sink) {
    GrooveDeviceSink *device_sink = sink->userdata;
    GrooveDeviceSinkPrivate *ds = device_sink->internals;

    SDL_LockMutex(ds->play_head_mutex);

    groove_buffer_unref(ds->audio_buf);
    ds->audio_buf = NULL;
    ds->audio_buf_index = 0;
    ds->audio_buf_size = 0;

    SDL_UnlockMutex(ds->play_head_mutex);
}

GrooveDeviceSink * groove_device_sink_create() {
    GrooveDeviceSink *device_sink = av_mallocz(sizeof(GrooveDeviceSink));
    GrooveDeviceSinkPrivate *ds = av_mallocz(sizeof(GrooveDeviceSinkPrivate));

    if (!device_sink || !ds) {
        av_free(device_sink);
        av_free(ds);
        av_log(NULL, AV_LOG_ERROR, "unable to create device sink: out of memory\n");
        return NULL;
    }

    device_sink->internals = ds;

    ds->sink = groove_sink_create();
    if (!ds->sink) {
        groove_device_sink_destroy(device_sink);
        av_log(NULL, AV_LOG_ERROR,"unable to create sink: out of memory\n");
        return NULL;
    }

    ds->play_head_mutex = SDL_CreateMutex();
    if (!ds->play_head_mutex) {
        groove_device_sink_destroy(device_sink);
        av_log(NULL, AV_LOG_ERROR,"unable to create play head mutex: out of memory\n");
        return NULL;
    }

    ds->eventq = groove_queue_create();
    if (!ds->eventq) {
        groove_device_sink_destroy(device_sink);
        av_log(NULL, AV_LOG_ERROR,"unable to create event queue: out of memory\n");
        return NULL;
    }

    // set some nice defaults
    device_sink->target_audio_format.sample_rate = 44100;
    device_sink->target_audio_format.channel_layout = GROOVE_CH_LAYOUT_STEREO;
    device_sink->target_audio_format.sample_fmt = GROOVE_SAMPLE_FMT_S16;
    // small because there is no way to clear the buffer.
    device_sink->device_buffer_size = 1024;
    device_sink->memory_buffer_size = 8192;

    return device_sink;
}

void groove_device_sink_destroy(GrooveDeviceSink *device_sink) {
    if (!device_sink)
        return;

    GrooveDeviceSinkPrivate *ds = device_sink->internals;

    if (ds->play_head_mutex)
        SDL_DestroyMutex(ds->play_head_mutex);

    if (ds->eventq)
        groove_queue_destroy(ds->eventq);

    groove_sink_destroy(ds->sink);

    av_free(ds);
    av_free(device_sink);
}

int groove_device_sink_attach(GrooveDeviceSink *device_sink, GroovePlayer *player) {
    GrooveDeviceSinkPrivate *ds = device_sink->internals;

    SDL_AudioSpec wanted_spec, spec;
    wanted_spec.format = groove_fmt_to_sdl_fmt(device_sink->target_audio_format.sample_fmt);
    wanted_spec.freq = device_sink->target_audio_format.sample_rate;
    wanted_spec.channels = groove_channel_layout_count(device_sink->target_audio_format.channel_layout);
    wanted_spec.samples = device_sink->device_buffer_size;
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = device_sink;

    ds->device_id = SDL_OpenAudioDevice(device_sink->device_name, 0, &wanted_spec,
            &spec, SDL_AUDIO_ALLOW_ANY_CHANGE);
    if (ds->device_id == 0) {
        av_log(NULL, AV_LOG_ERROR, "unable to open audio device: %s\n", SDL_GetError());
        return -1;
    }

    // save the actual spec back into the struct
    device_sink->actual_audio_format.sample_rate = spec.freq;
    device_sink->actual_audio_format.channel_layout = groove_channel_layout_default(spec.channels);
    device_sink->actual_audio_format.sample_fmt = sdl_fmt_to_groove_fmt(spec.format);

    // based on spec that we got, attach a sink with those properties
    ds->sink->buffer_size = device_sink->memory_buffer_size;
    ds->sink->audio_format = device_sink->actual_audio_format;

    if (ds->sink->audio_format.sample_fmt == GROOVE_SAMPLE_FMT_NONE) {
        groove_device_sink_detach(device_sink);
        av_log(NULL, AV_LOG_ERROR, "unsupported audio device sample format\n");
        return -1;
    }

    int err = groove_sink_attach(ds->sink, player);
    if (err < 0) {
        groove_device_sink_detach(device_sink);
        av_log(NULL, AV_LOG_ERROR, "unable to attach sink\n");
        return err;
    }

    ds->play_pos = -1.0;

    ds->sink->userdata = device_sink;
    ds->sink->purge = sink_purge;
    ds->sink->flush = sink_flush;

    SDL_PauseAudioDevice(ds->device_id, 0);

    return 0;
}

int groove_device_sink_detach(GrooveDeviceSink *device_sink) {
    GrooveDeviceSinkPrivate *ds = device_sink->internals;
    if (ds->eventq) {
        groove_queue_abort(ds->eventq);
    }
    if (ds->sink->player) {
        groove_sink_detach(ds->sink);
    }
    if (ds->device_id > 0) {
        SDL_CloseAudioDevice(ds->device_id);
        ds->device_id = 0;
    }
    device_sink->player = NULL;

    groove_buffer_unref(ds->audio_buf);
    ds->audio_buf = NULL;

    return 0;
}

int groove_device_count() {
    return SDL_GetNumAudioDevices(0);
}

const char * groove_device_name(int index) {
    return SDL_GetAudioDeviceName(index, 0);
}

void groove_device_sink_position(GrooveDeviceSink *device_sink,
        GroovePlaylistItem **item, double *seconds)
{
    GrooveDeviceSinkPrivate *ds = device_sink->internals;

    SDL_LockMutex(ds->play_head_mutex);

    if (item)
        *item = ds->play_head;

    if (seconds)
        *seconds = ds->play_pos;

    SDL_UnlockMutex(ds->play_head_mutex);
}

int groove_device_sink_event_get(GrooveDeviceSink *device_sink, GrooveEvent *event,
        int block)
{
    GrooveDeviceSinkPrivate *ds = device_sink->internals;
    GrooveEvent *tmp;
    int err = groove_queue_get(ds->eventq, (void **)&tmp, block);
    if (err > 0) {
        *event = *tmp;
        av_free(tmp);
    }
    return err;
}

int groove_device_sink_event_peek(GrooveDeviceSink *device_sink, int block) {
    GrooveDeviceSinkPrivate *ds = device_sink->internals;
    return groove_queue_peek(ds->eventq, block);
}
