#ifndef __DECODE_H__
#define __DECODE_H__

#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>

typedef struct GrooveBufferList {
    AVFrame *frame;
    struct GrooveBufferList *next;
} GrooveBufferList;

typedef struct GrooveDecodeContext {
    AVPacket audio_pkt_temp;
    AVFrame *frame;
    int paused;
    int last_paused;
    void *callback_context;
    void (*flush)(struct GrooveDecodeContext *);
    int (*buffer)(struct GrooveDecodeContext *, AVFrame *);

    int in_sample_rate;
    uint64_t in_channel_layout;
    enum AVSampleFormat in_sample_fmt;
    AVRational in_time_base;

    int dest_sample_rate;
    uint64_t dest_channel_layout;
    enum AVSampleFormat dest_sample_fmt;
    int dest_channel_count;

    char strbuf[512];
    AVFilterGraph *filter_graph;
    AVFilterContext *abuffer_ctx;
    AVFilterContext *volume_ctx;
    AVFilterContext *aformat_ctx;
    AVFilterContext *abuffersink_ctx;

    enum GrooveReplayGainMode replaygain_mode;
    double replaygain_preamp;
    double replaygain_default;
    double volume;
} GrooveDecodeContext;

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

// call this after you have set dest_sample_rate, dest_channel_layout,
// and dest_sample_fmt
int groove_init_decode_ctx(GrooveDecodeContext *decode_ctx);

void groove_cleanup_decode_ctx(GrooveDecodeContext *decode_ctx);
int groove_decode(GrooveDecodeContext *decode_ctx, GrooveFile *file);
int groove_maybe_init();
int groove_maybe_init_sdl();

#endif /* __DECODE_H__ */
