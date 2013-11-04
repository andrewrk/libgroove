/*
 * Copyright (c) 2013 Andrew Kelley
 *
 * This file is part of libgroove, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "groove.h"
#include "queue.h"

#include <ebur128.h>

#include <SDL2/SDL_thread.h>

#include <libavutil/mem.h>
#include <libavutil/log.h>

#include <limits.h>

struct GrooveLoudnessDetectorPrivate {
    struct GrooveLoudnessDetector externals;

    ebur128_state *ebur_track_state;
    ebur128_state *ebur_album_state;
    struct GrooveSink *sink;
    struct GrooveQueue *info_queue;
    SDL_Thread *thread_id;

    // info_head_mutex applies to variables inside this block.
    SDL_mutex *info_head_mutex;
    // current playlist item pointer
    struct GroovePlaylistItem *info_head;
    double info_pos;
    // analyze_thread waits on this when the info queue is full
    SDL_cond *drain_cond;
    // how many items are in the queue
    int info_queue_count;
    double album_peak;
    double track_duration;
    double album_duration;

    // set temporarily
    struct GroovePlaylistItem *purge_item;

    int abort_request;
};

static int emit_track_info(struct GrooveLoudnessDetectorPrivate *d) {
    struct GrooveLoudnessDetectorInfo *info = av_mallocz(sizeof(struct GrooveLoudnessDetectorInfo));
    if (!info) {
        av_log(NULL, AV_LOG_ERROR, "unable to allocate loudness detector info\n");
        return -1;
    }
    info->item = d->info_head;
    info->duration = d->track_duration;

    ebur128_loudness_global(d->ebur_track_state, &info->loudness);
    ebur128_sample_peak(d->ebur_track_state, 0, &info->peak);
    double out;
    ebur128_sample_peak(d->ebur_track_state, 1, &out);
    if (out > info->peak) info->peak = out;
    if (info->peak > d->album_peak) d->album_peak = info->peak;

    groove_queue_put(d->info_queue, info);

    return 0;
}

static int detect_thread(void *arg) {
    struct GrooveLoudnessDetectorPrivate *d = arg;
    struct GrooveLoudnessDetector *detector = &d->externals;

    struct GrooveBuffer *buffer;
    while (!d->abort_request) {
        SDL_LockMutex(d->info_head_mutex);

        if (d->info_queue_count >= detector->info_queue_size) {
            SDL_CondWait(d->drain_cond, d->info_head_mutex);
            SDL_UnlockMutex(d->info_head_mutex);
            continue;
        }

        // we definitely want to unlock the mutex while we wait for the
        // next buffer. Otherwise there will be a deadlock when sink_flush or
        // sink_purge is called.
        SDL_UnlockMutex(d->info_head_mutex);

        int result = groove_sink_get_buffer(d->sink, &buffer, 1);

        SDL_LockMutex(d->info_head_mutex);

        if (result == GROOVE_BUFFER_END) {
            // last file info
            emit_track_info(d);

            // send album info
            struct GrooveLoudnessDetectorInfo *info = av_mallocz(sizeof(struct GrooveLoudnessDetectorInfo));
            if (info) {
                info->duration = d->album_duration;
                ebur128_loudness_global(d->ebur_album_state, &info->loudness);
                info->peak = d->album_peak;
                groove_queue_put(d->info_queue, info);
            } else {
                av_log(NULL, AV_LOG_ERROR, "unable to allocate album loudness info\n");
            }

            d->album_peak = 0.0;
            ebur128_destroy(&d->ebur_album_state);
            d->ebur_album_state = NULL;
            d->album_duration = 0.0;

            d->info_head = NULL;
            d->info_pos = -1.0;

            SDL_UnlockMutex(d->info_head_mutex);
            continue;
        }

        if (result != GROOVE_BUFFER_YES) {
            SDL_UnlockMutex(d->info_head_mutex);
            break;
        }

        if (buffer->item != d->info_head) {
            if (d->ebur_track_state) {
                emit_track_info(d);
                ebur128_destroy(&d->ebur_track_state);
            }
            d->ebur_track_state = ebur128_init(2, 44100, EBUR128_MODE_SAMPLE_PEAK|EBUR128_MODE_I);
            if (!d->ebur_track_state) {
                av_log(NULL, AV_LOG_ERROR, "unable to allocate EBU R128 track context\n");
            }
            d->track_duration = 0.0;
            d->info_head = buffer->item;
            d->info_pos = buffer->pos;
        }
        if (!d->ebur_album_state) {
            d->ebur_album_state = ebur128_init(2, 44100, EBUR128_MODE_SAMPLE_PEAK|EBUR128_MODE_I);
            if (!d->ebur_album_state) {
                av_log(NULL, AV_LOG_ERROR, "unable to allocate EBU R128 album context\n");
            }
        }

        double buffer_duration = buffer->frame_count / (double)buffer->format.sample_rate;
        d->track_duration += buffer_duration;
        d->album_duration += buffer_duration;
        ebur128_add_frames_double(d->ebur_track_state, (double*)buffer->data[0], buffer->frame_count);
        ebur128_add_frames_double(d->ebur_album_state, (double*)buffer->data[0], buffer->frame_count);

        SDL_UnlockMutex(d->info_head_mutex);
        groove_buffer_unref(buffer);
    }

    return 0;
}

static void info_queue_cleanup(struct GrooveQueue* queue, void *obj) {
    struct GrooveLoudnessDetectorInfo *info = obj;
    struct GrooveLoudnessDetectorPrivate *d = queue->context;
    d->info_queue_count -= 1;
    av_free(info);
}

static void info_queue_put(struct GrooveQueue *queue, void *obj) {
    struct GrooveLoudnessDetectorPrivate *d = queue->context;
    d->info_queue_count += 1;
}

static void info_queue_get(struct GrooveQueue *queue, void *obj) {
    struct GrooveLoudnessDetectorPrivate *d = queue->context;
    struct GrooveLoudnessDetector *detector = &d->externals;

    d->info_queue_count -= 1;

    if (d->info_queue_count < detector->info_queue_size)
        SDL_CondSignal(d->drain_cond);
}

static int info_queue_purge(struct GrooveQueue* queue, void *obj) {
    struct GrooveLoudnessDetectorInfo *info = obj;
    struct GrooveLoudnessDetectorPrivate *d = queue->context;

    return info->item == d->purge_item;
}

static void sink_purge(struct GrooveSink *sink, struct GroovePlaylistItem *item) {
    struct GrooveLoudnessDetectorPrivate *d = sink->userdata;

    SDL_LockMutex(d->info_head_mutex);
    d->purge_item = item;
    groove_queue_purge(d->info_queue);
    d->purge_item = NULL;

    if (d->info_head == item) {
        d->info_head = NULL;
        d->info_pos = -1.0;
    }
    SDL_CondSignal(d->drain_cond);
    SDL_UnlockMutex(d->info_head_mutex);
}

static void sink_flush(struct GrooveSink *sink) {
    struct GrooveLoudnessDetectorPrivate *d = sink->userdata;

    SDL_LockMutex(d->info_head_mutex);
    groove_queue_flush(d->info_queue);
    if (d->ebur_track_state) {
        ebur128_destroy(&d->ebur_track_state);
        d->ebur_track_state = NULL;
        d->track_duration = 0.0;
    }
    if (d->ebur_album_state) {
        ebur128_destroy(&d->ebur_album_state);
        d->ebur_album_state = NULL;
    }
    SDL_CondSignal(d->drain_cond);
    SDL_UnlockMutex(d->info_head_mutex);
}

struct GrooveLoudnessDetector *groove_loudness_detector_create(void) {
    struct GrooveLoudnessDetectorPrivate *d = av_mallocz(sizeof(struct GrooveLoudnessDetectorPrivate));
    if (!d) {
        av_log(NULL, AV_LOG_ERROR, "unable to allocate loudness detector\n");
        return NULL;
    }

    struct GrooveLoudnessDetector *detector = &d->externals;

    d->info_head_mutex = SDL_CreateMutex();
    if (!d->info_head_mutex) {
        groove_loudness_detector_destroy(detector);
        av_log(NULL, AV_LOG_ERROR, "unable to create mutex\n");
        return NULL;
    }

    d->drain_cond = SDL_CreateCond();
    if (!d->drain_cond) {
        groove_loudness_detector_destroy(detector);
        av_log(NULL, AV_LOG_ERROR, "unable to create mutex condition\n");
        return NULL;
    }

    d->info_queue = groove_queue_create();
    if (!d->info_queue) {
        groove_loudness_detector_destroy(detector);
        av_log(NULL, AV_LOG_ERROR, "unable to allocate queue\n");
        return NULL;
    }
    d->info_queue->context = detector;
    d->info_queue->cleanup = info_queue_cleanup;
    d->info_queue->put = info_queue_put;
    d->info_queue->get = info_queue_get;
    d->info_queue->purge = info_queue_purge;

    d->sink = groove_sink_create();
    if (!d->sink) {
        groove_loudness_detector_destroy(detector);
        av_log(NULL, AV_LOG_ERROR, "unable to allocate sink\n");
        return NULL;
    }
    d->sink->audio_format.sample_rate = 44100;
    d->sink->audio_format.channel_layout = GROOVE_CH_LAYOUT_STEREO;
    d->sink->audio_format.sample_fmt = GROOVE_SAMPLE_FMT_DBL;
    d->sink->userdata = detector;
    d->sink->purge = sink_purge;
    d->sink->flush = sink_flush;

    // set some defaults
    detector->info_queue_size = INT_MAX;
    detector->sink_buffer_size = d->sink->buffer_size;

    return detector;
}

void groove_loudness_detector_destroy(struct GrooveLoudnessDetector *detector) {
    if (!detector)
        return;

    struct GrooveLoudnessDetectorPrivate *d = (struct GrooveLoudnessDetectorPrivate *) detector;

    if (d->sink)
        groove_sink_destroy(d->sink);

    if (d->info_queue)
        groove_queue_destroy(d->info_queue);

    if (d->info_head_mutex)
        SDL_DestroyMutex(d->info_head_mutex);

    if (d->drain_cond)
        SDL_DestroyCond(d->drain_cond);

    av_free(d);
}

int groove_loudness_detector_attach(struct GrooveLoudnessDetector *detector,
        struct GroovePlaylist *playlist)
{
    struct GrooveLoudnessDetectorPrivate *d = (struct GrooveLoudnessDetectorPrivate *) detector;

    detector->playlist = playlist;
    groove_queue_reset(d->info_queue);

    if (groove_sink_attach(d->sink, playlist) < 0) {
        groove_loudness_detector_detach(detector);
        av_log(NULL, AV_LOG_ERROR, "unable to attach sink\n");
        return -1;
    }

    d->thread_id = SDL_CreateThread(detect_thread, "detect", detector);

    if (!d->thread_id) {
        groove_loudness_detector_detach(detector);
        av_log(NULL, AV_LOG_ERROR, "unable to create detector thread\n");
        return -1;
    }

    return 0;
}

int groove_loudness_detector_detach(struct GrooveLoudnessDetector *detector) {
    struct GrooveLoudnessDetectorPrivate *d = (struct GrooveLoudnessDetectorPrivate *) detector;

    d->abort_request = 1;
    groove_sink_detach(d->sink);
    groove_queue_flush(d->info_queue);
    groove_queue_abort(d->info_queue);
    SDL_CondSignal(d->drain_cond);
    SDL_WaitThread(d->thread_id, NULL);
    d->thread_id = NULL;

    detector->playlist = NULL;

    if (d->ebur_track_state)
        ebur128_destroy(&d->ebur_track_state);
    if (d->ebur_album_state)
        ebur128_destroy(&d->ebur_album_state);

    d->abort_request = 0;
    d->info_head = NULL;
    d->info_pos = 0;

    return 0;
}

int groove_loudness_detector_get_info(struct GrooveLoudnessDetector *detector,
        struct GrooveLoudnessDetectorInfo *info, int block)
{
    struct GrooveLoudnessDetectorPrivate *d = (struct GrooveLoudnessDetectorPrivate *) detector;

    struct GrooveLoudnessDetectorInfo *info_ptr;
    if (groove_queue_get(d->info_queue, (void**)&info_ptr, block) == 1) {
        *info = *info_ptr;
        av_free(info_ptr);
        return 1;
    }

    return 0;
}

void groove_loudness_detector_position(struct GrooveLoudnessDetector *detector,
        struct GroovePlaylistItem **item, double *seconds)
{
    struct GrooveLoudnessDetectorPrivate *d = (struct GrooveLoudnessDetectorPrivate *) detector;

    SDL_LockMutex(d->info_head_mutex);

    if (item)
        *item = d->info_head;

    if (seconds)
        *seconds = d->info_pos;

    SDL_UnlockMutex(d->info_head_mutex);
}
