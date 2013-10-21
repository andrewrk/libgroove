#include "groove.h"
#include "queue.h"

#include <libavutil/frame.h>
#include <SDL2/SDL_audio.h>

// SDL audio buffer size, in samples. Should be small because there is no way
// to clear the buffer.
#define SDL_AUDIO_BUFFER_SIZE 1024

typedef struct DeviceSinkContext {
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
    GroovePlayer *player;

    // only touched by sdl_audio_callback, tells whether we have reached end
    // of audio queue naturally rather than a buffer underrun
    int end_of_q;

    GrooveQueue *eventq;
} DeviceSinkContext;

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
    DeviceSinkContext *dsc = opaque;
    GrooveSink *sink = dsc->sink;

    double bytes_per_sec = sink->bytes_per_sec;

    SDL_LockMutex(dsc->play_head_mutex);

    while (len > 0) {
        int paused = groove_player_playing(dsc->player);
        if (!paused && dsc->audio_buf_index >= dsc->audio_buf_size) {
            groove_buffer_unref(dsc->audio_buf);
            dsc->audio_buf_index = 0;
            dsc->audio_buf_size = 0;

            int ret = groove_player_sink_get_buffer(dsc->player, dsc->sink,
                    &dsc->audio_buf, 0);
            if (ret == GROOVE_BUFFER_END) {
                emit_event(dsc->eventq, GROOVE_EVENT_NOWPLAYING);

                dsc->end_of_q = 1;
                dsc->play_head = NULL;
                dsc->play_pos = -1.0;
            } else if (ret == GROOVE_BUFFER_YES) {
                if (dsc->play_head != dsc->audio_buf->item)
                    emit_event(dsc->eventq, GROOVE_EVENT_NOWPLAYING);

                dsc->end_of_q = 0;
                dsc->play_head = dsc->audio_buf->item;
                dsc->play_pos = dsc->audio_buf->pos;
                dsc->audio_buf_size = dsc->audio_buf->size;
            } else {
                // errors are treated the same as no buffer ready
                emit_event(dsc->eventq, GROOVE_EVENT_BUFFERUNDERRUN);
            }
        }
        if (paused || !dsc->audio_buf) {
            // fill with silence
            memset(stream, 0, len);
            break;
        }
        size_t len1 = dsc->audio_buf_size - dsc->audio_buf_index;
        if (len1 > len)
            len1 = len;
        memcpy(stream, dsc->audio_buf->data[0] + dsc->audio_buf_index, len1);
        len -= len1;
        stream += len1;
        dsc->audio_buf_index += len1;
        dsc->play_pos += len1 / bytes_per_sec;
    }

    SDL_UnlockMutex(dsc->play_head_mutex);
}

static void destroy_device_sink(DeviceSinkContext *dsc) {
    if (dsc->device_id > 0) {
        SDL_CloseAudioDevice(dsc->device_id);
    }
    av_frame_free(&dsc->audio_buf);
}

static void sink_cleanup(GrooveSink *sink) {
    DeviceSinkContext *dsc = sink->userdata;
    destroy_device_sink(dsc);
}

static void sink_purge(GrooveSink *sink, GroovePlaylistItem *item) {
    DeviceSinkContext *dsc = sink->userdata;

    SDL_LockMutex(dsc->play_head_mutex);

    if (dsc->play_head == item) {
        dsc->play_head = NULL;
        dsc->play_pos = -1.0;
        groove_buffer_unref(dsc->audio_buf);
        dsc->audio_buf_index = 0;
        dsc->audio_buf_size = 0;
        emit_event(dsc->eventq, GROOVE_EVENT_NOWPLAYING);
    }

    SDL_UnlockMutex(dsc->play_head_mutex);
}

GrooveDeviceSink* groove_device_sink_create(GroovePlayer *player,
        const char *name)
{
    DeviceSinkContext *dsc = av_mallocz(sizeof(DeviceSinkContext));

    if (!dsc) {
        av_log(NULL, AV_LOG_ERROR, "unable to create device sink: out of memory\n");
        return NULL;
    }
    dsc->player = player;

    SDL_AudioSpec wanted_spec, spec;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.freq = 44100;
    wanted_spec.channels = 2;
    wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = dsc;
    dsc->device_id = SDL_OpenAudioDevice(name, 0, &wanted_spec,
            &spec, SDL_AUDIO_ALLOW_ANY_CHANGE);
    if (dsc->device_id == 0) {
        groove_device_sink_destroy(dsc);
        av_log(NULL, AV_LOG_ERROR, "unable to open audio device: %s\n", SDL_GetError());
        return NULL;
    }

    // based on spec that we got, attach a sink with those properties
    GrooveAudioFormat audio_format;
    audio_format.sample_rate = spec.freq;
    audio_format.channel_layout = groove_channel_layout_default(spec.channels);
    audio_format.sample_fmt = sdl_fmt_to_groove_fmt(spec.format);

    if (audio_format.sample_fmt == GROOVE_SAMPLE_FMT_NONE) {
        groove_device_sink_destroy(dsc);
        av_log(NULL, AV_LOG_ERROR, "unsupported audio device sample format\n");
        return NULL;
    }

    dsc->sink = groove_player_attach_sink(player, &audio_format);
    if (!dsc->sink) {
        groove_device_sink_destroy(dsc);
        av_log(NULL, AV_LOG_ERROR, "unable to attach sink for playback\n");
        return NULL;
    }

    dsc->play_pos = -1.0;

    dsc->sink->userdata = dsc;
    dsc->sink->cleanup = sink_cleanup;
    dsc->sink->purge = sink_purge;

    SDL_PauseAudioDevice(dsc->device_id, 0);
}

void groove_device_sink_destroy(GrooveDeviceSink *device_sink) {
    DeviceSinkContext *dsc = device_sink;
    if (dsc->sink) {
        groove_player_remove_sink(dsc->player, dsc->sink);
    } else {
        destroy_device_sink(dsc);
    }
    av_free(dsc);
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
    DeviceSinkContext *dsc = device_sink;

    SDL_LockMutex(dsc->play_head_mutex);
    if (item)
        *item = dsc->play_head;

    if (seconds)
        *seconds = dsc->play_pos;
    SDL_UnlockMutex(p->decode_head_mutex);
}
