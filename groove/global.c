/*
 * Copyright (c) 2013 Andrew Kelley
 *
 * This file is part of libgroove, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "groove.h"
#include "config.h"

#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <pthread.h>

static int should_deinit_network = 0;

static int my_lockmgr_cb(void **mutex, enum AVLockOp op) {
    if (mutex == NULL)
        return -1;
    pthread_mutex_t *pmutex;
    switch (op) {
        case AV_LOCK_CREATE:
            pmutex = av_mallocz(sizeof(pthread_mutex_t));
            *mutex = pmutex;
            return pthread_mutex_init(pmutex, NULL);
        case AV_LOCK_OBTAIN:
            pmutex = *mutex;
            return pthread_mutex_lock(pmutex);
        case AV_LOCK_RELEASE:
            pmutex = *mutex;
            return pthread_mutex_unlock(pmutex);
        case AV_LOCK_DESTROY:
            pmutex = *mutex;
            int err = pthread_mutex_destroy(pmutex);
            av_free(pmutex);
            *mutex = NULL;
            return err;
    }
    return 0;
}

int groove_init(void) {
    av_lockmgr_register(&my_lockmgr_cb);

    srand(time(NULL));

    // register all codecs, demux and protocols
    avcodec_register_all();
    av_register_all();
    avformat_network_init();
    avfilter_register_all();

    should_deinit_network = 1;

    av_log_set_level(AV_LOG_QUIET);
    return 0;
}

void groove_finish(void) {
    if (should_deinit_network) {
        avformat_network_deinit();
        should_deinit_network = 0;
    }
}

void groove_set_logging(int level) {
    av_log_set_level(level);
}

int groove_channel_layout_count(uint64_t channel_layout) {
    return av_get_channel_layout_nb_channels(channel_layout);
}

uint64_t groove_channel_layout_default(int count) {
    return av_get_default_channel_layout(count);
}

int groove_sample_format_bytes_per_sample(enum GrooveSampleFormat format) {
    return av_get_bytes_per_sample((enum AVSampleFormat)format);
}

const char *groove_version(void) {
    return GROOVE_VERSION_STRING;
}

int groove_version_major(void) {
    return GROOVE_VERSION_MAJOR;
}

int groove_version_minor(void) {
    return GROOVE_VERSION_MINOR;
}

int groove_version_patch(void) {
    return GROOVE_VERSION_PATCH;
}
