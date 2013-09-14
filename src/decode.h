#ifndef __DECODE_H__
#define __DECODE_H__

#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>

// TODO prefix these types with Groove
typedef struct BufferList {
    AVFrame *frame;
    struct BufferList *next;
} BufferList;

typedef struct DecodeContext {
    AVPacket audio_pkt_temp;
    AVFrame *frame;
    int paused;
    int last_paused;
    void *callback_context;
    void (*flush)(struct DecodeContext *);
    int (*buffer)(struct DecodeContext *, AVFrame *);

    int in_sample_rate;
    uint64_t in_channel_layout;
    enum AVSampleFormat in_sample_fmt;
    AVRational in_time_base;

    int dest_sample_rate;
    uint64_t dest_channel_layout;
    int dest_channel_count;
    enum AVSampleFormat dest_sample_fmt;

    char strbuf[512];
    AVFilterGraph *filter_graph;
    AVFilterContext *abuffer_ctx;
    AVFilterContext *volume_ctx;
    AVFilterContext *aformat_ctx;
    AVFilterContext *abuffersink_ctx;
} DecodeContext;

typedef struct GrooveFilePrivate {
    int audio_stream_index;
    int abort_request; // true when we're closing the file
    AVFormatContext *ic;
    int seek_by_bytes;
    AVCodec *decoder;
    AVStream *audio_st;
    int seek_req;
    int seek_flags;
    int64_t seek_pos;
    int64_t seek_rel;
    int eof;
    double audio_clock;
    AVPacket audio_pkt;

    // state while saving
    AVFormatContext *oc;
    int tempfile_exists;
} GrooveFilePrivate;

// TODO prefix these with groove_
void cleanup_decode_ctx(DecodeContext *decode_ctx);
int decode(DecodeContext *decode_ctx, GrooveFile *file);
int maybe_init();
int maybe_init_sdl();

#endif /* __DECODE_H__ */
