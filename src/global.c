/*
 * Copyright (c) 2013 Andrew Kelley
 *
 * This file is part of libgroove, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "groove.h"

#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <SDL2/SDL.h>

static void deinit_network() {
    avformat_network_deinit();
}

static int my_lockmgr_cb(void **mutex, enum AVLockOp op) {
    if (mutex == NULL)
        return -1;
    switch (op) {
        case AV_LOCK_CREATE:
            *mutex = SDL_CreateMutex();
            break;
        case AV_LOCK_OBTAIN:
            SDL_LockMutex(*mutex);
            break;
        case AV_LOCK_RELEASE:
            SDL_UnlockMutex(*mutex);
            break;
        case AV_LOCK_DESTROY:
            SDL_DestroyMutex(*mutex);
            break;
    }
    return 0;
}

int groove_init() {
    av_lockmgr_register(&my_lockmgr_cb);

    srand(time(NULL));

    // register all codecs, demux and protocols
    avcodec_register_all();
    av_register_all();
    avformat_network_init();
    avfilter_register_all();

    atexit(deinit_network);

    if (SDL_Init(SDL_INIT_AUDIO)) {
        av_log(NULL, AV_LOG_ERROR, "Could not initialize SDL - %s\n", SDL_GetError());
        return -1;
    }
    atexit(SDL_Quit);

    av_log_set_level(AV_LOG_QUIET);
    return 0;
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
    return av_get_bytes_per_sample(format);
}
