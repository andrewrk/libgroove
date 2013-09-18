#ifndef __DECODE_H__
#define __DECODE_H__

#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <SDL/SDL_thread.h>

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
    int dest_channel_count; // computed
    int dest_bytes_per_sec; // computed

    char strbuf[512];
    AVFilterGraph *filter_graph;
    AVFilterContext *abuffer_ctx;
    AVFilterContext *volume_ctx;
    AVFilterContext *aformat_ctx;
    AVFilterContext *abuffersink_ctx;

    // these are the desired settings
    enum GrooveReplayGainMode replaygain_mode;
    double replaygain_preamp;
    double replaygain_default;
    double volume;

    // these are the settings that were used to build the filter graph
    enum GrooveReplayGainMode filter_replaygain_mode;
    double filter_replaygain_preamp;
    double filter_replaygain_default;
    double filter_volume;

    GrooveFile *last_decoded_file;
} GrooveDecodeContext;

typedef struct GrooveFilePrivate {
    int audio_stream_index;
    int abort_request; // true when we're closing the file
    AVFormatContext *ic;
    AVCodec *decoder;
    AVStream *audio_st;

    // this mutex protects seek_pos
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

// call this after you have set dest_sample_rate, dest_channel_layout,
// and dest_sample_fmt
int groove_init_decode_ctx(GrooveDecodeContext *decode_ctx);

void groove_cleanup_decode_ctx(GrooveDecodeContext *decode_ctx);
int groove_decode(GrooveDecodeContext *decode_ctx, GrooveFile *file);

#endif /* __DECODE_H__ */
