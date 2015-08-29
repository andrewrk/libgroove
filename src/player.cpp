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

#include <soundio/soundio.h>
#include <pthread.h>
#include <time.h>
#include <stdbool.h>
#include <stdarg.h>
#include <assert.h>

struct GroovePlayerPrivate {
    struct GroovePlayer externals;

    struct GrooveBuffer *audio_buf;
    size_t audio_buf_size; // in frames
    size_t audio_buf_index; // in frames

    // this mutex applies to the variables in this block
    pthread_mutex_t play_head_mutex;
    bool play_head_mutex_inited;
    // pointer to current item where the buffered audio is reaching the device
    struct GroovePlaylistItem *play_head;
    // number of seconds into the play_head song where the buffered audio
    // is reaching the device
    double play_pos;

    struct SoundIoOutStream *outstream;
    struct GrooveSink *sink;

    struct GrooveQueue *eventq;

    // watchdog thread for opening and closing audio device
    atomic_bool abort_request;
    pthread_t device_thread_id;
    int device_thread_inited;
    pthread_cond_t device_thread_cond;
    bool device_thread_cond_inited;
    int silence_frames_left;
    atomic_bool request_device_reopen;
    struct GrooveAudioFormat device_format;
};

static int open_audio_device(struct GroovePlayer *player,
        const struct GrooveAudioFormat *target_format, struct GrooveAudioFormat *actual_format,
        bool use_exact_audio_format);

static enum SoundIoFormat prioritized_formats[] = {
    SoundIoFormatFloat32NE,
    SoundIoFormatS32NE,
    SoundIoFormatFloat64NE,
    SoundIoFormatS16NE,
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

static int find_best_format(struct SoundIoDevice *device, bool exact,
        enum SoundIoFormat target_fmt,
        enum SoundIoFormat *actual_fmt)
{
    // if exact format is supported, that's obviously the best pick
    if (soundio_device_supports_format(device, target_fmt)) {
        *actual_fmt = target_fmt;
        return 0;
    }

    if (exact)
        return -1;

    for (int i = 0; i < array_length(prioritized_formats); i += 1) {
        enum SoundIoFormat try_fmt = prioritized_formats[i];

        if (soundio_device_supports_format(device, try_fmt)) {
            *actual_fmt = try_fmt;
            return 0;
        }
    }

    return -1;
}

static int find_best_sample_rate(struct SoundIoDevice *device, bool exact,
        int target_rate, int *actual_rate)
{
    // if exact rate is supported, that's obviously the best pick
    for (int i = 0; i < device->sample_rate_count; i += 1) {
        struct SoundIoSampleRateRange *range = &device->sample_rates[i];
        if (range->min <= target_rate && target_rate <= range->max) {
            *actual_rate = target_rate;
            return 0;
        }
    }

    if (exact)
        return -1;

    *actual_rate = soundio_device_nearest_sample_rate(device, target_rate);

    return 0;
}


static int find_best_channel_layout(struct SoundIoDevice *device, bool exact,
        const struct SoundIoChannelLayout *target_layout,
        struct SoundIoChannelLayout *actual_layout)
{
    // if exact layout is supported, that's obviousy the best pick
    if (soundio_device_supports_layout(device, target_layout)) {
        *actual_layout = *target_layout;
        return 0;
    }

    if (exact)
        return -1;

    for (int i = 0; i < array_length(prioritized_layouts); i += 1) {
        enum SoundIoChannelLayoutId try_layout_id = prioritized_layouts[i];
        const struct SoundIoChannelLayout *try_layout = soundio_channel_layout_get_builtin(try_layout_id);
        if (soundio_device_supports_layout(device, try_layout)) {
            *actual_layout = *try_layout;
            return 0;
        }
    }

    return -1;
}

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
    if (p->outstream) {
        soundio_outstream_destroy(p->outstream);
        p->outstream = NULL;
    }
}

static void *device_thread_run(void *arg) {
    struct GroovePlayer *player = (GroovePlayer *)arg;
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;

    for (;;) {
        if (p->abort_request)
            break;

        pthread_mutex_lock(&p->play_head_mutex);
        if (!p->request_device_reopen.load()) {
            pthread_cond_wait(&p->device_thread_cond, &p->play_head_mutex);
            pthread_mutex_unlock(&p->play_head_mutex);
            continue;
        }

        p->request_device_reopen.store(false);

        close_audio_device(p);

        p->device_format = p->audio_buf->format;
        int err;
        struct GrooveAudioFormat actual_audio_format;
        if ((err = open_audio_device(player, &p->audio_buf->format,
                        &actual_audio_format, player->use_exact_audio_format)))
        {
            pthread_mutex_unlock(&p->play_head_mutex);
            emit_event(p->eventq, GROOVE_EVENT_DEVICE_REOPEN_ERROR);
        } else {
            pthread_mutex_unlock(&p->play_head_mutex);
            // unlock the mutex before calling start since start might call the audio callback
            soundio_outstream_start(p->outstream);
            emit_event(p->eventq, GROOVE_EVENT_DEVICEREOPENED);
        }
    }

    return NULL;
}

static void error_callback(struct SoundIoOutStream *outstream, int err) {
    groove_panic("stream error: %s", soundio_strerror(err));
}

static void underflow_callback(struct SoundIoOutStream *outstream) {
    struct GroovePlayerPrivate *p = (GroovePlayerPrivate *)outstream->userdata;
    emit_event(p->eventq, GROOVE_EVENT_BUFFERUNDERRUN);
}

static void audio_callback(struct SoundIoOutStream *outstream,
        int frame_count_min, int frame_count_max)
{
    const struct SoundIoChannelLayout *layout = &outstream->layout;
    struct GroovePlayerPrivate *p = (GroovePlayerPrivate *)outstream->userdata;
    struct GrooveSink *sink = p->sink;
    struct GroovePlaylist *playlist = sink->playlist;
    struct SoundIoChannelArea *areas;

    int paused = !groove_playlist_playing(playlist);
    int err;

    int audio_buf_frames_left = p->audio_buf_size - p->audio_buf_index;
    assert(audio_buf_frames_left >= 0);
    int frames_in_queue = groove_sink_get_fill_level(sink) / outstream->bytes_per_frame;
    assert(frames_in_queue >= 0);
    int frames_available = frames_in_queue + audio_buf_frames_left;
    bool queue_contains_end = groove_sink_contains_end_of_playlist(sink);

    if (!queue_contains_end && frames_available < frame_count_min) {
        // We don't have enough frames to meet the minimum requirements.
        // Fill entire buffer with silence.
        int frames_left = frame_count_min;
        for (;;) {
            int frame_count = frames_left;

            if ((err = soundio_outstream_begin_write(outstream, &areas, &frame_count)))
                groove_panic("%s", soundio_strerror(err));

            if (!frame_count)
                return;

            for (int frame = 0; frame < frame_count; frame += 1) {
                for (int channel = 0; channel < layout->channel_count; channel += 1) {
                    memset(areas[channel].ptr, 0, outstream->bytes_per_sample);
                    areas[channel].ptr += areas[channel].step;
                }
            }

            if ((err = soundio_outstream_end_write(outstream)))
                groove_panic("%s", soundio_strerror(err));

            frames_left -= frame_count;

            if (frames_left <= 0)
                return;
        }
    }

    int frames_to_write = clamp(frame_count_min, frames_available, frame_count_max);
    int frames_left = frames_to_write;
    assert(frames_left >= 0);
    bool read_anyway = queue_contains_end && (frame_count_max > frames_available);

    pthread_mutex_lock(&p->play_head_mutex);

    while (frames_left || read_anyway) {
        int write_frame_count = frames_left;
        if (write_frame_count) {
            if ((err = soundio_outstream_begin_write(outstream, &areas, &write_frame_count)))
                groove_panic("%s", soundio_strerror(err));
        }

        int write_frames_left = write_frame_count;

        while (write_frames_left || read_anyway) {
            bool waiting_for_silence = (p->silence_frames_left > 0);
            if (!p->request_device_reopen.load() && !waiting_for_silence &&
                !paused && p->audio_buf_index >= p->audio_buf_size)
            {
                groove_buffer_unref(p->audio_buf);
                p->audio_buf_index = 0;
                p->audio_buf_size = 0;

                int ret = groove_sink_buffer_get(sink, &p->audio_buf, 0);
                if (ret == GROOVE_BUFFER_END) {
                    emit_event(p->eventq, GROOVE_EVENT_NOWPLAYING);
                    p->play_head = NULL;
                    p->play_pos = -1.0;
                } else if (ret == GROOVE_BUFFER_YES) {
                    if (p->play_head != p->audio_buf->item)
                        emit_event(p->eventq, GROOVE_EVENT_NOWPLAYING);

                    p->play_head = p->audio_buf->item;
                    p->play_pos = p->audio_buf->pos;
                    p->audio_buf_size = p->audio_buf->frame_count;

                    if (p->device_thread_inited &&
                        !groove_audio_formats_equal(&p->audio_buf->format, &p->device_format))
                    {
                        p->silence_frames_left = outstream->software_latency * outstream->sample_rate;
                        waiting_for_silence = true;
                    }
                } else {
                    groove_panic("unexpected buffer error");
                }
            }
            if (p->request_device_reopen.load() || waiting_for_silence || paused || !p->audio_buf) {
                // fill the rest with silence
                pthread_mutex_unlock(&p->play_head_mutex);

                int silence_frames_written = 0;
                while (frames_left) {
                    for (int frame = 0; frame < write_frames_left; frame += 1) {
                        for (int channel = 0; channel < layout->channel_count; channel += 1) {
                            memset(areas[channel].ptr, 0, outstream->bytes_per_sample);
                            areas[channel].ptr += areas[channel].step;
                        }
                    }
                    silence_frames_written += write_frames_left;
                    if (write_frame_count) {
                        if ((err = soundio_outstream_end_write(outstream)))
                            groove_panic("%s", soundio_strerror(err));
                    }
                    frames_left -= write_frames_left;
                    if (frames_left <= 0)
                        break;
                    write_frames_left = frames_left;
                    if (write_frames_left <= 0)
                        break;
                    if ((err = soundio_outstream_begin_write(outstream, &areas, &write_frames_left)))
                        groove_panic("%s", soundio_strerror(err));
                    assert(write_frames_left);
                }

                if (waiting_for_silence) {
                    p->silence_frames_left -= silence_frames_written;
                    if (p->silence_frames_left <= 0) {
                        p->request_device_reopen.store(true);
                        pthread_cond_signal(&p->device_thread_cond);
                    }
                }

                return;
            }
            size_t read_frame_count = p->audio_buf_size - p->audio_buf_index;
            int frame_count = min((int)read_frame_count, write_frames_left);

            if (p->audio_buf->format.is_planar) {
                size_t end_frame = p->audio_buf_index + frame_count;
                for (; p->audio_buf_index < end_frame; p->audio_buf_index += 1) {
                    for (int ch = 0; ch < layout->channel_count; ch += 1) {
                        uint8_t *source = &p->audio_buf->data[ch][p->audio_buf_index * outstream->bytes_per_sample];
                        memcpy(areas[ch].ptr, source, outstream->bytes_per_sample);
                        areas[ch].ptr += areas[ch].step;
                    }
                }
            } else {
                uint8_t *source = p->audio_buf->data[0] + p->audio_buf_index * outstream->bytes_per_frame;
                size_t end_frame = p->audio_buf_index + frame_count;
                for (; p->audio_buf_index < end_frame; p->audio_buf_index += 1) {
                    for (int ch = 0; ch < layout->channel_count; ch += 1) {
                        memcpy(areas[ch].ptr, source, outstream->bytes_per_sample);
                        source += outstream->bytes_per_sample;
                        areas[ch].ptr += areas[ch].step;
                    }
                }
            }
            p->play_pos += frame_count / (double) outstream->sample_rate;
            write_frames_left -= frame_count;
            frames_left -= frame_count;
        }

        if (write_frame_count) {
            if ((err = soundio_outstream_end_write(outstream)))
                groove_panic("%s", soundio_strerror(err));
        }
    }

    pthread_mutex_unlock(&p->play_head_mutex);
}

static void sink_purge(struct GrooveSink *sink, struct GroovePlaylistItem *item) {
    struct GroovePlayerPrivate *p = (GroovePlayerPrivate *)sink->userdata;

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

static void sink_pause(struct GrooveSink *sink) {
    struct GroovePlayer *player = (GroovePlayer *)sink->userdata;
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;

    soundio_outstream_pause(p->outstream, true);
}

static void sink_play(struct GrooveSink *sink) {
    struct GroovePlayer *player = (GroovePlayer *)sink->userdata;
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;

    soundio_outstream_pause(p->outstream, false);
}

static void sink_flush(struct GrooveSink *sink) {
    struct GroovePlayerPrivate *p = (GroovePlayerPrivate *)sink->userdata;

    pthread_mutex_lock(&p->play_head_mutex);

    groove_buffer_unref(p->audio_buf);
    p->audio_buf = NULL;
    p->audio_buf_index = 0;
    p->audio_buf_size = 0;
    p->play_pos = -1.0;
    p->play_head = NULL;

    pthread_mutex_unlock(&p->play_head_mutex);
}

struct GroovePlayer *groove_player_create(void) {
    struct GroovePlayerPrivate *p = allocate<GroovePlayerPrivate>(1);

    if (!p) {
        av_log(NULL, AV_LOG_ERROR, "unable to create player: out of memory\n");
        return NULL;
    }
    struct GroovePlayer *player = &p->externals;

    p->request_device_reopen.store(false);

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
    p->play_head_mutex_inited = true;

    p->eventq = groove_queue_create();
    if (!p->eventq) {
        groove_player_destroy(player);
        av_log(NULL, AV_LOG_ERROR,"unable to create event queue: out of memory\n");
        return NULL;
    }

    if (pthread_cond_init(&p->device_thread_cond, NULL) != 0) {
        groove_player_destroy(player);
        av_log(NULL, AV_LOG_ERROR, "unable to create mutex condition\n");
        return NULL;
    }
    p->device_thread_cond_inited = true;

    // set some nice defaults
    player->target_audio_format.sample_rate = 44100;
    player->target_audio_format.layout = *soundio_channel_layout_get_builtin(SoundIoChannelLayoutIdStereo);
    player->target_audio_format.format = SoundIoFormatS16NE;
    player->target_audio_format.is_planar = false;
    player->gain = p->sink->gain;

    return player;
}

void groove_player_destroy(struct GroovePlayer *player) {
    if (!player)
        return;

    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;

    if (p->device_thread_cond_inited)
        pthread_cond_destroy(&p->device_thread_cond);

    if (p->play_head_mutex_inited)
        pthread_mutex_destroy(&p->play_head_mutex);

    if (p->eventq)
        groove_queue_destroy(p->eventq);

    groove_sink_destroy(p->sink);

    deallocate(p);
}

static int open_audio_device(struct GroovePlayer *player,
        const struct GrooveAudioFormat *target_format,
        struct GrooveAudioFormat *actual_format,
        bool use_exact_audio_format)
{
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;
    int err;

    assert(player->device);
    struct SoundIoDevice *device = player->device;
    soundio_device_ref(device);

    assert(!p->outstream);
    p->outstream = soundio_outstream_create(device);
    if (!p->outstream) {
        soundio_device_unref(device);
        av_log(NULL, AV_LOG_ERROR, "unable to open audio device: out of memory\n");
        return -1;
    }

    if ((err = find_best_format(device, use_exact_audio_format, target_format->format,
                    &actual_format->format)))
    {
        soundio_device_unref(device);
        av_log(NULL, AV_LOG_ERROR, "unable to open audio device: no supported sample format\n");
        return err;
    }
    p->outstream->format = actual_format->format;

    if ((err = find_best_sample_rate(device, use_exact_audio_format, target_format->sample_rate,
                    &actual_format->sample_rate)))
    {
        soundio_device_unref(device);
        av_log(NULL, AV_LOG_ERROR, "unable to open audio device: no supported sample rate\n");
        return err;
    }
    p->outstream->sample_rate = actual_format->sample_rate;

    if ((err = find_best_channel_layout(device, use_exact_audio_format, &target_format->layout,
                    &actual_format->layout)))
    {
        soundio_device_unref(device);
        av_log(NULL, AV_LOG_ERROR, "unable to open audio device: no supported channel layout\n");
        return err;
    }
    p->outstream->layout = actual_format->layout;

    soundio_device_unref(device);

    p->outstream->userdata = player;
    p->outstream->error_callback = error_callback;
    p->outstream->underflow_callback = underflow_callback;
    p->outstream->write_callback = audio_callback;

    if ((err = soundio_outstream_open(p->outstream))) {
        soundio_device_unref(device);
        av_log(NULL, AV_LOG_ERROR, "unable to open audio device: %s\n", soundio_strerror(err));
        return -1;
    }
    double sink_buffer_seconds = max(4.0, p->outstream->software_latency);
    p->sink->buffer_size_bytes = sink_buffer_seconds * p->outstream->sample_rate * p->outstream->bytes_per_frame;


    return 0;
}

int groove_player_attach(struct GroovePlayer *player, struct GroovePlaylist *playlist) {
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;
    int err;

    p->sink->gain = player->gain;
    p->sink->pause = sink_pause;
    p->sink->play = sink_play;

    if (open_audio_device(player, &player->target_audio_format, &player->actual_audio_format,
                player->use_exact_audio_format))
    {
        groove_player_detach(player);
        return -1;
    }

    // based on spec that we got, attach a sink with those properties
    p->sink->audio_format = player->actual_audio_format;

    if (p->sink->audio_format.format == SoundIoFormatInvalid) {
        groove_player_detach(player);
        av_log(NULL, AV_LOG_ERROR, "unsupported audio device sample format\n");
        return -1;
    }

    if (player->use_exact_audio_format) {
        p->sink->disable_resample = 1;

        if (pthread_create(&p->device_thread_id, NULL, device_thread_run, player) != 0) {
            groove_player_detach(player);
            av_log(NULL, AV_LOG_ERROR, "unable to create device thread\n");
            return -1;
        }
        p->device_thread_inited = true;
    }

    err = groove_sink_attach(p->sink, playlist);
    if (err < 0) {
        groove_player_detach(player);
        av_log(NULL, AV_LOG_ERROR, "unable to attach sink\n");
        return err;
    }

    p->play_pos = -1.0;

    groove_queue_reset(p->eventq);


    if (!groove_playlist_playing(playlist))
        sink_pause(p->sink);

    soundio_outstream_start(p->outstream);

    return 0;
}

int groove_player_detach(struct GroovePlayer *player) {
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;
    p->abort_request = true;
    close_audio_device(p);
    if (p->device_thread_inited) {
        pthread_mutex_lock(&p->play_head_mutex);
        pthread_cond_signal(&p->device_thread_cond);
        pthread_mutex_unlock(&p->play_head_mutex);
        pthread_join(p->device_thread_id, NULL);
        p->device_thread_inited = false;
    }
    if (p->eventq) {
        groove_queue_flush(p->eventq);
        groove_queue_abort(p->eventq);
    }
    if (p->sink->playlist) {
        groove_sink_detach(p->sink);
    }

    player->playlist = NULL;

    groove_buffer_unref(p->audio_buf);
    p->audio_buf = NULL;

    p->request_device_reopen.store(false);
    p->silence_frames_left = 0;
    p->abort_request = false;

    return 0;
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

struct GrooveAudioFormat groove_player_get_device_audio_format(struct GroovePlayer *player) {
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;
    struct GrooveAudioFormat result;
    pthread_mutex_lock(&p->play_head_mutex);
    result = p->device_format;
    pthread_mutex_unlock(&p->play_head_mutex);
    return result;
}
