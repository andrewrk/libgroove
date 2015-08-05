/*
 * Copyright (c) 2013 Andrew Kelley
 *
 * This file is part of libgroove, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#ifndef GROOVE_FILE_HPP
#define GROOVE_FILE_HPP

#include "groove/groove.h"
#include "ffmpeg.hpp"

#include <pthread.h>

struct GrooveFilePrivate {
    struct GrooveFile externals;
    int audio_stream_index;
    int abort_request; // true when we're closing the file
    AVFormatContext *ic;
    AVCodec *decoder;
    AVStream *audio_st;

    // this mutex protects the fields in this block
    pthread_mutex_t seek_mutex;
    int64_t seek_pos; // -1 if no seek request
    int seek_flush; // whether the seek request wants us to flush the buffer

    int eof;
    double audio_clock; // position of the decode head
    AVPacket audio_pkt;

    // state while saving
    AVFormatContext *oc;
    int tempfile_exists;

    int paused;
};

#endif
