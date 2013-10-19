#ifndef __FILE_H__
#define __FILE_H__

#include "groove.h"

typedef struct GrooveFilePrivate {
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

} GrooveFilePrivate;


#endif /* __FILE_H__ */
