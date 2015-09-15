/*
 * Copyright (c) 2013 Andrew Kelley
 *
 * This file is part of libgroove, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "groove_private.h"
#include "groove/player.h"
#include "queue.hpp"
#include "ffmpeg.hpp"
#include "util.hpp"
#include "atomics.hpp"
#include "atomic_value.hpp"
#include "os.hpp"

#include <soundio/soundio.h>
#include <assert.h>

// At timestamp, frame_index had delay seconds until becoming audible.
struct TimeStamp {
    long frame_index;
    double delay;
    double timestamp;
};

struct GroovePlayerPrivate {
    struct GroovePlayer externals;

    struct Groove *groove;
    struct GrooveBuffer *audio_buf;
    size_t audio_buf_size; // in frames
    size_t audio_buf_index; // in frames

    // this mutex applies to the variables in this block
    GrooveOsMutex *play_head_mutex;
    // pointer to current item where the buffered audio is reaching the device
    struct GroovePlaylistItem *play_head;
    // number of seconds into the play_head song where the buffered audio
    // is reaching the device
    double play_pos;
    long play_pos_index;

    struct SoundIoOutStream *outstream;
    struct GrooveSink *sink;

    struct GrooveQueue *eventq;

    // watchdog thread for opening and closing audio device
    bool abort_request;
    GrooveOsThread *helper_thread;
    GrooveOsCond *helper_thread_cond;
    bool request_device_reopen;
    struct GrooveAudioFormat device_format;

    bool is_paused;
    bool prebuffering;
    atomic_bool prebuf_flag;
    bool is_underrun;
    bool is_started;
    struct SoundIoRingBuffer *playback_buffer;
    int playback_buffer_cap;
    bool waiting_for_device_reopen;
    long decode_abs_index;
    long buffer_abs_index;

    AtomicValue<TimeStamp> time_stamp;
    atomic_long device_close_frame_index;
    bool waiting_to_be_closed;
    double sink_buffer_seconds;
    atomic_long skip_to_index;
};

static enum SoundIoFormat prioritized_formats[] = {
    SoundIoFormatFloat32NE,
    SoundIoFormatS32NE,
    SoundIoFormatS16NE,
    SoundIoFormatFloat64NE,
    SoundIoFormatU8,
};

static enum SoundIoChannelLayoutId prioritized_layouts[] = {
    SoundIoChannelLayoutIdOctagonal,
    SoundIoChannelLayoutId7Point1WideBack,
    SoundIoChannelLayoutId7Point1Wide,
    SoundIoChannelLayoutId7Point1,
    SoundIoChannelLayoutId7Point0Front,
    SoundIoChannelLayoutId7Point0,
    SoundIoChannelLayoutId6Point1Front,
    SoundIoChannelLayoutId6Point1Back,
    SoundIoChannelLayoutId6Point1,
    SoundIoChannelLayoutIdHexagonal,
    SoundIoChannelLayoutId6Point0Front,
    SoundIoChannelLayoutId6Point0Side,
    SoundIoChannelLayoutId5Point1Back,
    SoundIoChannelLayoutId5Point0Back,
    SoundIoChannelLayoutId5Point1,
    SoundIoChannelLayoutId5Point0Side,
    SoundIoChannelLayoutId4Point1,
    SoundIoChannelLayoutIdQuad,
    SoundIoChannelLayoutId4Point0,
    SoundIoChannelLayoutId3Point1,
    SoundIoChannelLayoutId2Point1,
    SoundIoChannelLayoutIdStereo,
    SoundIoChannelLayoutIdMono,
};

static void emit_event(struct GrooveQueue *queue, enum GroovePlayerEventType type) {
    union GroovePlayerEvent *evt = allocate_nonzero<GroovePlayerEvent>(1);
    if (!evt) {
        av_log(NULL, AV_LOG_ERROR, "unable to create event: out of memory\n");
        return;
    }
    evt->type = type;
    if (groove_queue_put(queue, evt) < 0)
        av_log(NULL, AV_LOG_ERROR, "unable to put event on queue: out of memory\n");
}

static void close_audio_device(struct GroovePlayerPrivate *p) {
    soundio_outstream_destroy(p->outstream);
    p->outstream = NULL;

    soundio_ring_buffer_destroy(p->playback_buffer);
    p->playback_buffer = NULL;
}

static void error_callback(struct SoundIoOutStream *outstream, int err) {
    groove_panic("stream error: %s", soundio_strerror(err));
}

static void set_pause_state(GroovePlayerPrivate *p) {
    if (p->is_started)
        soundio_outstream_pause(p->outstream, (p->prebuffering || p->is_paused));
}

static void handle_buffer_underrun(struct GroovePlayerPrivate *p) {
    groove_os_mutex_lock(p->play_head_mutex);
    p->is_underrun = true;
    p->prebuf_flag.store(true);
    groove_os_cond_signal(p->helper_thread_cond, p->play_head_mutex);
    groove_os_mutex_unlock(p->play_head_mutex);
}

static void underflow_callback(struct SoundIoOutStream *outstream) {
    struct GroovePlayerPrivate *p = (GroovePlayerPrivate *)outstream->userdata;
    handle_buffer_underrun(p);
}

static void audio_callback(struct SoundIoOutStream *outstream,
        int frame_count_min, int frame_count_max)
{
    struct GroovePlayerPrivate *p = (GroovePlayerPrivate *)outstream->userdata;
    const struct SoundIoChannelLayout *layout = &outstream->layout;
    struct SoundIoChannelArea *areas;
    int err;

    long device_close_frame_index = p->device_close_frame_index.load();
    int needed_frames_past_close_index = (outstream->software_latency * outstream->sample_rate);
    int frames_until_close = (device_close_frame_index >= 0) ?
        (device_close_frame_index - p->buffer_abs_index + needed_frames_past_close_index) : -1;

    bool prebuf = p->prebuf_flag.load();
    int fill_bytes = soundio_ring_buffer_fill_count(p->playback_buffer);
    int fill_frames = fill_bytes / outstream->bytes_per_frame;

    if (!prebuf && frame_count_min > fill_frames && frame_count_min > frames_until_close)
        handle_buffer_underrun(p);
    prebuf = p->prebuf_flag.load();

    if (prebuf || p->waiting_to_be_closed) {
        // Waiting for helper thread to recover from buffer underrun. In the
        // mean time, write silence.
        int frames_left = frame_count_min;
        while (frames_left > 0) {
            int frame_count = frames_left;

            if ((err = soundio_outstream_begin_write(outstream, &areas, &frame_count))) {
                error_callback(outstream, err);
                break;
            }

            if (!frame_count)
                break;

            for (int frame = 0; frame < frame_count; frame += 1) {
                for (int channel = 0; channel < layout->channel_count; channel += 1) {
                    memset(areas[channel].ptr, 0, outstream->bytes_per_sample);
                    areas[channel].ptr += areas[channel].step;
                }
            }

            if ((err = soundio_outstream_end_write(outstream))) {
                error_callback(outstream, err);
                break;
            }

            frames_left -= frame_count;
        }
        return;
    }

    char *read_ptr = soundio_ring_buffer_read_ptr(p->playback_buffer);
    int write_frames = min(max(fill_frames, frames_until_close), frame_count_max);
    int ring_buf_write_frames = min(write_frames, fill_frames);
    int frames_until_zero = ring_buf_write_frames;
    int frames_left = write_frames;
    while (frames_left > 0) {
        int frame_count = frames_left;

        if ((err = soundio_outstream_begin_write(outstream, &areas, &frame_count))) {
            error_callback(outstream, err);
            break;
        }

        if (!frame_count)
            break;

        int nonzero_write_count = min(frames_until_zero, frame_count);
        int zero_write_count = max(0, frame_count - frames_until_zero);

        for (int frame = 0; frame < nonzero_write_count; frame += 1) {
            for (int channel = 0; channel < layout->channel_count; channel += 1) {
                memcpy(areas[channel].ptr, read_ptr, outstream->bytes_per_sample);
                areas[channel].ptr += areas[channel].step;
                read_ptr += outstream->bytes_per_sample;
            }
        }
        for (int frame = 0; frame < zero_write_count; frame += 1) {
            for (int channel = 0; channel < layout->channel_count; channel += 1) {
                memset(areas[channel].ptr, 0, outstream->bytes_per_sample);
                areas[channel].ptr += areas[channel].step;
            }
        }

        frames_until_zero -= nonzero_write_count;

        if ((err = soundio_outstream_end_write(outstream))) {
            error_callback(outstream, err);
            break;
        }

        frames_left -= frame_count;
    }

    soundio_ring_buffer_advance_read_ptr(p->playback_buffer, ring_buf_write_frames * outstream->bytes_per_frame);
    p->buffer_abs_index += write_frames;

    long skip_to_index = p->skip_to_index.exchange(-1);
    if (skip_to_index > 0) {
        soundio_outstream_clear_buffer(p->outstream);
        int frames_to_skip = max(0L, skip_to_index - p->buffer_abs_index);
        int fill_bytes = soundio_ring_buffer_fill_count(p->playback_buffer);
        int fill_frames = fill_bytes / outstream->bytes_per_frame;
        int bytes_to_skip = outstream->bytes_per_frame * min(frames_to_skip, fill_frames);
        soundio_ring_buffer_advance_read_ptr(p->playback_buffer, bytes_to_skip);
    }

    if (device_close_frame_index >= 0) {
        int frames_past_close_index = p->buffer_abs_index - device_close_frame_index;
        if (frames_past_close_index >= needed_frames_past_close_index) {
            groove_os_mutex_lock(p->play_head_mutex);
            p->request_device_reopen = true;
            p->waiting_to_be_closed = true;
            groove_os_cond_signal(p->helper_thread_cond, p->play_head_mutex);
            groove_os_mutex_unlock(p->play_head_mutex);
            return;
        }
    }

    TimeStamp *time_stamp = p->time_stamp.write_begin();

    if ((err = soundio_outstream_get_latency(outstream, &time_stamp->delay))) {
        time_stamp->delay = outstream->software_latency - (frame_count_max / (double)outstream->sample_rate);
        return;
    }

    time_stamp->frame_index = p->buffer_abs_index;
    time_stamp->timestamp = groove_os_get_time();
    p->time_stamp.write_end();
}

static int open_audio_device(struct GroovePlayerPrivate *p) {
    struct GroovePlayer *player = &p->externals;
    int err;

    assert(player->device);
    assert(!p->outstream);

    // We need this defined so that we can see with what parameters to open the device.
    assert(p->audio_buf);

    struct SoundIoDevice *device = player->device;
    p->outstream = soundio_outstream_create(device);
    if (!p->outstream) {
        close_audio_device(p);
        return GrooveErrorNoMem;
    }
    p->device_format = p->audio_buf->format;

    p->outstream->format = p->audio_buf->format.format;
    p->outstream->sample_rate = p->audio_buf->format.sample_rate;
    p->outstream->layout = p->audio_buf->format.layout;

    p->outstream->userdata = player;
    p->outstream->error_callback = error_callback;
    p->outstream->underflow_callback = underflow_callback;
    p->outstream->write_callback = audio_callback;

    if ((err = soundio_outstream_open(p->outstream))) {
        close_audio_device(p);
        av_log(NULL, AV_LOG_ERROR, "unable to open audio device: %s\n", soundio_strerror(err));
        return GrooveErrorOpeningDevice;
    }

    TimeStamp *time_stamp = p->time_stamp.write_begin();
    time_stamp->frame_index = 0;
    time_stamp->delay = p->outstream->software_latency;
    time_stamp->timestamp = groove_os_get_time();
    p->time_stamp.write_end();

    p->sink_buffer_seconds = max(4.0, p->outstream->software_latency);
    p->playback_buffer_cap = p->sink_buffer_seconds * p->outstream->sample_rate * p->outstream->bytes_per_frame;
    p->sink->buffer_size_bytes = p->playback_buffer_cap / 4;
    p->playback_buffer = soundio_ring_buffer_create(device->soundio, p->playback_buffer_cap);
    if (!p->playback_buffer) {
        close_audio_device(p);
        return GrooveErrorNoMem;
    }

    return 0;
}

static bool audio_formats_equal_ignore_planar(
        const struct GrooveAudioFormat *a, const struct GrooveAudioFormat *b)
{
    return (a->sample_rate == b->sample_rate &&
            soundio_channel_layout_equal(&a->layout, &b->layout) &&
            a->format == b->format);
}

static void done_prebuffering(GroovePlayerPrivate *p) {
    if (p->prebuffering) {
        p->prebuffering = false;
        p->prebuf_flag.store(false);
        if (!p->is_started) {
            p->is_started = true;
            soundio_outstream_start(p->outstream);
        }
        set_pause_state(p);
    }
}

static void helper_thread_run(void *arg) {
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) arg;
    struct GrooveSink *sink = p->sink;
    int err;

    // This thread's job is to:
    // * Close and re-open the sound device with proper parameters.
    // * Write decoded audio to the playback ring buffer (so that the audio
    //   callback can be real time safe).
    // * Start the outstream when the playback ring buffer is full.
    // * Detect an underrun reported from the audio callback, recover by
    //   pausing the device until the playback ring buffer is full, resume the
    //   device, and then report the underrun via an event.

    groove_os_mutex_lock(p->play_head_mutex);
    while (!p->abort_request) {
        if (p->audio_buf_index >= p->audio_buf_size) {
            groove_buffer_unref(p->audio_buf);
            p->audio_buf_index = 0;
            p->audio_buf_size = 0;

            int ret = groove_sink_buffer_get(sink, &p->audio_buf, 0);

            if (ret == GROOVE_BUFFER_END) {
                emit_event(p->eventq, GROOVE_EVENT_END_OF_PLAYLIST);
                emit_event(p->eventq, GROOVE_EVENT_NOWPLAYING);
                p->play_head = NULL;
                p->play_pos = -1.0;
                p->play_pos_index = 0;
                if (p->outstream) {
                    done_prebuffering(p);
                    p->waiting_for_device_reopen = true;
                    p->device_close_frame_index.store(p->decode_abs_index);
                }
                groove_os_cond_wait(p->helper_thread_cond, p->play_head_mutex);
                continue;
            } else if (ret == GROOVE_BUFFER_YES) {
                if (p->play_head != p->audio_buf->item)
                    emit_event(p->eventq, GROOVE_EVENT_NOWPLAYING);

                p->play_head = p->audio_buf->item;
                p->play_pos = p->audio_buf->pos;
                p->play_pos_index = p->decode_abs_index;
                p->audio_buf_size = p->audio_buf->frame_count;

                if (!p->outstream) {
                    p->request_device_reopen = true;
                } else if (!audio_formats_equal_ignore_planar(&p->audio_buf->format, &p->device_format)) {
                    p->waiting_for_device_reopen = true;
                    p->device_close_frame_index.store(p->decode_abs_index);
                }
            } else if (!p->request_device_reopen) {
                groove_os_cond_wait(p->helper_thread_cond, p->play_head_mutex);
                continue;
            }
        }

        if (p->request_device_reopen) {
            close_audio_device(p);
            emit_event(p->eventq, GROOVE_EVENT_DEVICE_CLOSED);

            p->waiting_for_device_reopen = false;
            p->device_close_frame_index.store(-1);
            p->prebuffering = true;
            p->is_started = false;
            p->prebuf_flag.store(false);
            p->is_underrun = false;
            p->decode_abs_index = 0;
            p->buffer_abs_index = 0;
            p->request_device_reopen = false;
            p->waiting_to_be_closed = false;
            p->skip_to_index.store(-1);

            if (p->audio_buf) {
                if ((err = open_audio_device(p))) {
                    av_log(NULL, AV_LOG_ERROR, "unable to open device: %s\n", groove_strerror(err));
                    emit_event(p->eventq, GROOVE_EVENT_DEVICE_OPEN_ERROR);
                } else {
                    emit_event(p->eventq, GROOVE_EVENT_DEVICE_OPENED);
                }
            }
            continue;
        }
        if (p->waiting_for_device_reopen) {
            groove_os_cond_wait(p->helper_thread_cond, p->play_head_mutex);
            continue;
        }
        if (!p->prebuffering && p->prebuf_flag.load()) {
            p->prebuffering = true;
            set_pause_state(p);
            if (p->is_underrun)
                emit_event(p->eventq, GROOVE_EVENT_BUFFERUNDERRUN);
            continue;
        }
        assert(p->outstream);
        int audio_buf_frames_left = p->audio_buf_size - p->audio_buf_index;
        assert(audio_buf_frames_left > 0);

        int fill_bytes = soundio_ring_buffer_fill_count(p->playback_buffer);
        int free_bytes = p->playback_buffer_cap - fill_bytes;
        int free_frames = free_bytes / p->outstream->bytes_per_frame;

        int write_frames = min(free_frames, audio_buf_frames_left);
        if (write_frames == 0) {
            done_prebuffering(p);
            double sleep_time = p->sink_buffer_seconds / 2.0;
            groove_os_cond_timed_wait(p->helper_thread_cond, p->play_head_mutex, sleep_time);
            continue;
        }

        char *write_ptr = soundio_ring_buffer_write_ptr(p->playback_buffer);
        int channel_count = p->audio_buf->format.layout.channel_count;
        if (p->audio_buf->format.is_planar) {
            size_t end_frame = p->audio_buf_index + write_frames;
            for (; p->audio_buf_index < end_frame; p->audio_buf_index += 1) {
                for (int ch = 0; ch < channel_count; ch += 1) {
                    uint8_t *source = &p->audio_buf->data[ch][p->audio_buf_index * p->outstream->bytes_per_sample];
                    memcpy(write_ptr, source, p->outstream->bytes_per_sample);
                    write_ptr += p->outstream->bytes_per_sample;
                }
            }
        } else {
            uint8_t *source = p->audio_buf->data[0] + p->audio_buf_index * p->outstream->bytes_per_frame;
            memcpy(write_ptr, source, p->outstream->bytes_per_frame * write_frames);
            p->audio_buf_index += write_frames;
        }
        p->decode_abs_index += write_frames;
        soundio_ring_buffer_advance_write_ptr(p->playback_buffer, write_frames * p->outstream->bytes_per_frame);

    }
    groove_os_mutex_unlock(p->play_head_mutex);

    close_audio_device(p);
}

static void sink_purge(struct GrooveSink *sink, struct GroovePlaylistItem *item) {
    struct GroovePlayerPrivate *p = (GroovePlayerPrivate *)sink->userdata;

    groove_os_mutex_lock(p->play_head_mutex);

    if (p->play_head == item) {
        p->play_head = NULL;
        p->play_pos = -1.0;
        p->play_pos_index = 0;
        groove_buffer_unref(p->audio_buf);
        p->audio_buf = NULL;
        p->audio_buf_index = 0;
        p->audio_buf_size = 0;
        emit_event(p->eventq, GROOVE_EVENT_NOWPLAYING);
    }

    groove_os_mutex_unlock(p->play_head_mutex);
}

static void sink_pause(struct GrooveSink *sink) {
    struct GroovePlayer *player = (GroovePlayer *)sink->userdata;
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;

    groove_os_mutex_lock(p->play_head_mutex);
    p->is_paused = true;
    set_pause_state(p);
    groove_os_mutex_unlock(p->play_head_mutex);
}

static void sink_play(struct GrooveSink *sink) {
    struct GroovePlayer *player = (GroovePlayer *)sink->userdata;
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;

    groove_os_mutex_lock(p->play_head_mutex);
    p->is_paused = false;
    set_pause_state(p);
    groove_os_mutex_unlock(p->play_head_mutex);
}

static void sink_filled(struct GrooveSink *sink) {
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) sink->userdata;

    groove_os_mutex_lock(p->play_head_mutex);
    groove_os_cond_signal(p->helper_thread_cond, p->play_head_mutex);
    groove_os_mutex_unlock(p->play_head_mutex);
}

static void sink_flush(struct GrooveSink *sink) {
    struct GroovePlayerPrivate *p = (GroovePlayerPrivate *)sink->userdata;

    groove_os_mutex_lock(p->play_head_mutex);

    groove_buffer_unref(p->audio_buf);
    p->audio_buf = NULL;
    p->audio_buf_index = 0;
    p->audio_buf_size = 0;
    p->play_pos = -1.0;
    p->play_head = NULL;
    p->play_pos_index = 0;
    p->prebuffering = true;
    p->prebuf_flag.store(true);

    p->skip_to_index.store(p->decode_abs_index);

    groove_os_mutex_unlock(p->play_head_mutex);
}

struct GroovePlayer *groove_player_create(struct Groove *groove) {
    struct GroovePlayerPrivate *p = allocate<GroovePlayerPrivate>(1);

    if (!p) {
        av_log(NULL, AV_LOG_ERROR, "unable to create player: out of memory\n");
        return NULL;
    }
    struct GroovePlayer *player = &p->externals;

    p->time_stamp.init();

    p->groove = groove;

    p->sink = groove_sink_create(groove);
    if (!p->sink) {
        groove_player_destroy(player);
        av_log(NULL, AV_LOG_ERROR,"unable to create sink: out of memory\n");
        return NULL;
    }

    p->sink->userdata = player;
    p->sink->purge = sink_purge;
    p->sink->flush = sink_flush;

    if (!(p->play_head_mutex = groove_os_mutex_create())) {
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

    if (!(p->helper_thread_cond = groove_os_cond_create())) {
        groove_player_destroy(player);
        av_log(NULL, AV_LOG_ERROR, "unable to create mutex condition\n");
        return NULL;
    }

    // set some nice defaults
    player->gain = p->sink->gain;

    return player;
}

void groove_player_destroy(struct GroovePlayer *player) {
    if (!player)
        return;

    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;

    groove_os_cond_destroy(p->helper_thread_cond);
    groove_os_mutex_destroy(p->play_head_mutex);

    if (p->eventq)
        groove_queue_destroy(p->eventq);

    groove_sink_destroy(p->sink);

    deallocate(p);
}

static int best_supported_sample_rate(GrooveSink *sink) {
    static const int target = 44100;
    int closest = -1;
    for (int i = 0; i < sink->sample_rate_count; i += 1) {
        struct SoundIoSampleRateRange *range = &sink->sample_rates[i];
        if (range->min <= target && target <= range->max)
            return target;
        if (closest == -1 || abs(range->max - target) < abs(closest - target)) {
            closest = range->max;
        }
    }
    return closest;
}

static int best_supported_layout(SoundIoDevice *device, SoundIoChannelLayout *out_layout) {
    for (int i = 0; i < array_length(prioritized_layouts); i += 1) {
        enum SoundIoChannelLayoutId layout_id = prioritized_layouts[i];
        const struct SoundIoChannelLayout *layout = soundio_channel_layout_get_builtin(layout_id);
        if (soundio_device_supports_layout(device, layout)) {
            *out_layout = *layout;
            return 0;
        }
    }

    return GrooveErrorDeviceParams;
}

static SoundIoFormat best_supported_format(SoundIoDevice *device) {
    for (int i = 0; i < array_length(prioritized_formats); i += 1) {
        SoundIoFormat format = prioritized_formats[i];
        if (soundio_device_supports_format(device, format)) {
            return format;
        }
    }
    return SoundIoFormatInvalid;
}

int groove_player_attach(struct GroovePlayer *player, struct GroovePlaylist *playlist) {
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;
    int err;

    if (!player->device)
        return GrooveErrorInvalid;
    if (player->device->aim != SoundIoDeviceAimOutput)
        return GrooveErrorInvalid;

    soundio_device_ref(player->device);

    p->sink->gain = player->gain;
    p->sink->pause = sink_pause;
    p->sink->play = sink_play;
    p->sink->filled = sink_filled;

    p->sink->sample_rates = player->device->sample_rates;
    p->sink->sample_rate_count = player->device->sample_rate_count;
    p->sink->sample_rate_default = best_supported_sample_rate(p->sink);

    p->sink->channel_layouts = player->device->layouts;
    p->sink->channel_layout_count = player->device->layout_count;
    if ((err = best_supported_layout(player->device, &p->sink->channel_layout_default))) {
        groove_player_detach(player);
        return err;
    }

    p->sink->sample_formats = player->device->formats;
    p->sink->sample_format_count = player->device->format_count;
    p->sink->sample_format_default = best_supported_format(player->device);

    if (p->sink->sample_format_default == SoundIoFormatInvalid) {
        groove_player_detach(player);
        return GrooveErrorDeviceParams;
    }

    p->sink->flags = ((uint32_t)GrooveSinkFlagPlanarOk)|((uint32_t)GrooveSinkFlagInterleavedOk);

    if ((err = groove_sink_attach(p->sink, playlist))) {
        groove_player_detach(player);
        av_log(NULL, AV_LOG_ERROR, "unable to attach sink\n");
        return err;
    }

    p->play_pos = -1.0;
    p->is_paused = !groove_playlist_playing(playlist);
    p->request_device_reopen = true;
    p->audio_buf_size = 0;
    p->audio_buf_index = 0;
    p->play_pos_index = 0;
    assert(!p->outstream);
    p->abort_request = false;
    p->waiting_to_be_closed = false;
    p->is_underrun = false;

    if ((err = groove_os_thread_create(helper_thread_run, p, &p->helper_thread))) {
        groove_player_detach(player);
        av_log(NULL, AV_LOG_ERROR, "unable to create device thread\n");
        return err;
    }

    groove_queue_reset(p->eventq);

    return 0;
}

int groove_player_detach(struct GroovePlayer *player) {
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;

    if (p->helper_thread) {
        groove_os_mutex_lock(p->play_head_mutex);
        p->abort_request = true;
        groove_os_cond_signal(p->helper_thread_cond, p->play_head_mutex);
        groove_os_mutex_unlock(p->play_head_mutex);
        groove_os_thread_destroy(p->helper_thread);
    }

    if (p->eventq) {
        groove_queue_flush(p->eventq);
        groove_queue_abort(p->eventq);
    }
    if (p->sink->playlist) {
        groove_sink_detach(p->sink);
    }

    player->playlist = NULL;

    soundio_device_unref(player->device);
    player->device = NULL;

    groove_buffer_unref(p->audio_buf);
    p->audio_buf = NULL;

    p->abort_request = false;

    return 0;
}

void groove_player_position(struct GroovePlayer *player,
        struct GroovePlaylistItem **item, double *seconds)
{
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;

    groove_os_mutex_lock(p->play_head_mutex);

    if (item)
        *item = p->play_head;

    if (seconds && p->outstream) {
        TimeStamp *time_stamp = p->time_stamp.get_read_ptr();

        double played_timestamp_at = time_stamp->timestamp + time_stamp->delay;

        long play_pos_frames_ahead = p->play_pos_index - time_stamp->frame_index;
        double play_pos_time_ahead = play_pos_frames_ahead / (double)p->outstream->sample_rate;
        double play_pos_timestamp = played_timestamp_at + play_pos_time_ahead;
        double now = groove_os_get_time();
        double time_since_play_pos_audible = now - play_pos_timestamp;

        *seconds = p->play_pos + time_since_play_pos_audible;
    }

    groove_os_mutex_unlock(p->play_head_mutex);
}

int groove_player_event_get(struct GroovePlayer *player,
        union GroovePlayerEvent *event, int block)
{
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;
    union GroovePlayerEvent *tmp;
    int err = groove_queue_get(p->eventq, (void **)&tmp, block);
    if (err > 0) {
        *event = *tmp;
        deallocate(tmp);
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

void groove_player_get_device_audio_format(struct GroovePlayer *player,
        struct GrooveAudioFormat *out_audio_format)
{
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;
    groove_os_mutex_lock(p->play_head_mutex);
    *out_audio_format = p->device_format;
    groove_os_mutex_unlock(p->play_head_mutex);
}
