#ifndef __DECODE_H__
#define __DECODE_H__

#include "libavformat/avformat.h"
#include "libavresample/avresample.h"

typedef struct BufferList {
    uint8_t * buffer;
    int size;
    struct BufferList * next;
} BufferList;

typedef struct DecodeContext {
    AVPacket audio_pkt_temp;
    AVFrame *frame;
    int paused;
    int last_paused;
    void *callback_context;
    void (*flush)(struct DecodeContext *);
    int (*buffer)(struct DecodeContext *, BufferList *);
} DecodeContext;

typedef struct GrooveFilePrivate {
    int audio_stream_index;
    int abort_request; // true when we're closing the file
    AVFormatContext *ic;
    int seek_by_bytes;
    AVCodec *decoder;
    int sdl_sample_rate;
    uint64_t sdl_channel_layout;
    int sdl_channels;
    enum AVSampleFormat sdl_sample_fmt;
    enum AVSampleFormat resample_sample_fmt;
    uint64_t resample_channel_layout;
    int resample_sample_rate;
    AVStream *audio_st;
    int seek_req;
    int seek_flags;
    int64_t seek_pos;
    int64_t seek_rel;
    int eof;
    double audio_clock;
    AVAudioResampleContext *avr;
    AVPacket audio_pkt;

    // state while saving
    AVFormatContext *oc;
    int tempfile_exists;
} GrooveFilePrivate;

int audio_decode_frame(DecodeContext *decode_ctx, GrooveFile *file);
int decode(DecodeContext *decode_ctx, GrooveFile *file);
int init_decode(GrooveFile *file);

#endif /* __DECODE_H__ */
