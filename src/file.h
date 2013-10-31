/*
 * Copyright (c) 2013 Andrew Kelley
 *
 * This file is part of libgroove, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#ifndef GROOVE_FILE_H_INCLUDED
#define GROOVE_FILE_H_INCLUDED

#include "groove.h"

#include <libavformat/avformat.h>
#include <SDL2/SDL_thread.h>

struct GrooveFilePrivate {
    struct GrooveFile externals;
    int audio_stream_index;
    int abort_request; // true when we're closing the file
    AVFormatContext *ic;
    AVCodec *decoder;
    AVStream *audio_st;

    // this mutex protects the fields in this block
    SDL_mutex *seek_mutex;
    int64_t seek_pos; // -1 if no seek request
    int seek_flush; // whether the seek request wants us to flush the buffer

    int eof;
    double audio_clock; // position of the decode head
    AVPacket audio_pkt;

    // state while saving
    AVFormatContext *oc;
    int tempfile_exists;
};

#endif /* GROOVE_FILE_H_INCLUDED */
