#include "groove.h"
#include "decode.h"

#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>

#include <SDL/SDL.h>

static int initialized = 0;
static int initialized_sdl = 0;

static void deinit_network() {
    avformat_network_deinit();
}

int maybe_init() {
    if (initialized)
        return 0;
    initialized = 1;


    srand(time(NULL));

    // register all codecs, demux and protocols
    avcodec_register_all();
    av_register_all();
    avformat_network_init();
    avfilter_register_all();

    atexit(deinit_network);
    atexit(avfilter_uninit);

    av_log_set_level(AV_LOG_QUIET);
    return 0;
}

int maybe_init_sdl() {
    if (initialized_sdl)
        return 0;
    initialized_sdl = 1;

    int flags = SDL_INIT_AUDIO;
    if (SDL_Init(flags)) {
        av_log(NULL, AV_LOG_ERROR, "Could not initialize SDL - %s\n", SDL_GetError());
        return -1;
    }
    atexit(SDL_Quit);
    return 0;
}

// decode one audio packet and return its uncompressed size
static int audio_decode_frame(DecodeContext *decode_ctx, GrooveFile *file) {
    GrooveFilePrivate * f = file->internals;

    AVPacket *pkt = &f->audio_pkt;
    AVCodecContext *dec = f->audio_st->codec;

    AVPacket *pkt_temp = &decode_ctx->audio_pkt_temp;
    *pkt_temp = *pkt;

    // update the audio clock with the pts if we can
    if (pkt->pts != AV_NOPTS_VALUE) {
        f->audio_clock = av_q2d(f->audio_st->time_base)*pkt->pts;
    }

    int data_size = 0;
    int n, len1, got_frame;
    int new_packet = 1;
    AVFrame *frame = decode_ctx->frame;

    // NOTE: the audio packet can contain several frames
    while (pkt_temp->size > 0 || (!pkt_temp->data && new_packet)) {
        avcodec_get_frame_defaults(frame);
        new_packet = 0;

        len1 = avcodec_decode_audio4(dec, frame, &got_frame, pkt_temp);
        if (len1 < 0) {
            // if error, we skip the frame
            pkt_temp->size = 0;
            return -1;
        }

        pkt_temp->data += len1;
        pkt_temp->size -= len1;

        if (!got_frame) {
            // stop sending empty packets if the decoder is finished
            if (!pkt_temp->data && dec->codec->capabilities & CODEC_CAP_DELAY)
                return 0;
            continue;
        }

        // push the audio data from decoded frame into the filtergraph
        int err = av_buffersrc_write_frame(decode_ctx->abuffer_ctx, frame);
        if (err < 0) {
            av_strerror(err, decode_ctx->args, sizeof(decode_ctx->args));
            av_log(NULL, AV_LOG_ERROR, "error writing frame to buffersrc: %s\n",
                    decode_ctx->args);
            return -1;
        }

        // pull filtered audio from the filtergraph
        AVFilterBufferRef *buf;
        for (;;) {
            int err = av_buffersink_read(decode_ctx->abuffersink_ctx, &buf);
            if (err == AVERROR_EOF || err == AVERROR(EAGAIN))
                break;
            if (err < 0) {
                av_log(NULL, AV_LOG_ERROR, "error reading buffer from buffersink\n");
                return -1;
            }
            data_size += buf->linesize[0];
            err = decode_ctx->buffer(decode_ctx, buf);
            if (err < 0)
                return err;
        }

        // if no pts, then compute it
        if (pkt->pts == AV_NOPTS_VALUE) {
            n = decode_ctx->dest_channel_count * av_get_bytes_per_sample(decode_ctx->dest_sample_fmt);
            f->audio_clock += (double)data_size / (double)(n * decode_ctx->dest_sample_rate);
        }
        return data_size;
    }
    return data_size;
}

static void cleanup_orphan_filters(DecodeContext *decode_ctx) {
    if (decode_ctx->abuffer_ctx) {
        avfilter_free(decode_ctx->abuffer_ctx);
        decode_ctx->abuffer_ctx = NULL;
    }
    if (decode_ctx->volume_ctx) {
        avfilter_free(decode_ctx->volume_ctx);
        decode_ctx->volume_ctx = NULL;
    }
    if (decode_ctx->aformat_ctx) {
        avfilter_free(decode_ctx->aformat_ctx);
        decode_ctx->aformat_ctx = NULL;
    }
    if (decode_ctx->abuffersink_ctx) {
        avfilter_free(decode_ctx->abuffersink_ctx);
        decode_ctx->abuffersink_ctx = NULL;
    }
}

static int init_filter_graph(DecodeContext *decode_ctx) {
    decode_ctx->filter_graph = avfilter_graph_alloc();
    if (!decode_ctx->filter_graph) {
        av_log(NULL, AV_LOG_ERROR, "unable to create filter graph: out of memory\n");
        return -1;
    }

    AVFilter *abuffer = avfilter_get_by_name("abuffer");
    AVFilter *volume = avfilter_get_by_name("volume");
    AVFilter *aformat = avfilter_get_by_name("aformat");
    AVFilter *abuffersink = avfilter_get_by_name("abuffersink");

    int err = 0;
    // create filter instances
    if (err >= 0) err = avfilter_open(&decode_ctx->abuffer_ctx, abuffer, NULL);
    if (err >= 0) err = avfilter_open(&decode_ctx->volume_ctx, volume, NULL);
    if (err >= 0) err = avfilter_open(&decode_ctx->aformat_ctx, aformat, NULL);
    if (err >= 0) err = avfilter_open(&decode_ctx->abuffersink_ctx, abuffersink, NULL);
    // connect the inputs and outputs
    if (err >= 0) err = avfilter_link(decode_ctx->abuffer_ctx, 0, decode_ctx->volume_ctx, 0);
    if (err >= 0) err = avfilter_link(decode_ctx->volume_ctx, 0, decode_ctx->aformat_ctx, 0);
    if (err >= 0) err = avfilter_link(decode_ctx->aformat_ctx, 0, decode_ctx->abuffersink_ctx, 0);
    if (err < 0) {
        cleanup_orphan_filters(decode_ctx);
        av_log(NULL, AV_LOG_ERROR, "error creating filters\n");
        return err;
    }
    // initialize parameters
    snprintf(decode_ctx->args, sizeof(decode_ctx->args),
            "sample_fmts=%s:channel_layouts=0x%"PRIx64":sample_rates=%d", 
            av_get_sample_fmt_name(decode_ctx->dest_sample_fmt),
            decode_ctx->dest_channel_layout,
            decode_ctx->dest_sample_rate);
    err = avfilter_init_filter(decode_ctx->aformat_ctx, decode_ctx->args, NULL);
    if (err < 0) {
        cleanup_orphan_filters(decode_ctx);
        av_log(NULL, AV_LOG_ERROR, "error initializing aformat filter\n");
        return err;
    }

    // add them all to the filter graph
    if (avfilter_graph_add_filter(decode_ctx->filter_graph, decode_ctx->abuffer_ctx) < 0) {
        cleanup_orphan_filters(decode_ctx);
        av_log(NULL, AV_LOG_ERROR, "error adding abuffer to graph\n");
        return -1;
    }
    if (avfilter_graph_add_filter(decode_ctx->filter_graph, decode_ctx->volume_ctx) < 0) {
        avfilter_free(decode_ctx->volume_ctx);
        avfilter_free(decode_ctx->aformat_ctx);
        avfilter_free(decode_ctx->abuffersink_ctx);
        av_log(NULL, AV_LOG_ERROR, "error adding volume to graph\n");
        return -1;
    }
    if (avfilter_graph_add_filter(decode_ctx->filter_graph, decode_ctx->aformat_ctx) < 0) {
        avfilter_free(decode_ctx->aformat_ctx);
        avfilter_free(decode_ctx->abuffersink_ctx);
        av_log(NULL, AV_LOG_ERROR, "error adding aformat to graph\n");
        return -1;
    }
    if (avfilter_graph_add_filter(decode_ctx->filter_graph, decode_ctx->abuffersink_ctx) < 0) {
        avfilter_free(decode_ctx->abuffersink_ctx);
        av_log(NULL, AV_LOG_ERROR, "error adding abuffersink to graph\n");
        return -1;
    }

    return 0;
}

// return < 0 if error
static int init_decode(DecodeContext *decode_ctx, GrooveFile *file) {
    GrooveFilePrivate *f = file->internals;

    // set all streams to discard. in a few lines here we will find the audio
    // stream and cancel discarding it
    for (int i = 0; i < f->ic->nb_streams; i++)
        f->ic->streams[i]->discard = AVDISCARD_ALL;

    f->audio_stream_index = av_find_best_stream(f->ic, AVMEDIA_TYPE_AUDIO, -1, -1, &f->decoder, 0);

    if (f->audio_stream_index < 0) {
        av_log(NULL, AV_LOG_ERROR, "%s: no audio stream found\n", f->ic->filename);
        return -1;
    }

    if (!f->decoder) {
        av_log(NULL, AV_LOG_ERROR, "%s: no decoder found\n", f->ic->filename);
        return -1;
    }

    f->audio_st = f->ic->streams[f->audio_stream_index];
    AVCodecContext *avctx = f->audio_st->codec;

    if (avcodec_open2(avctx, f->decoder, NULL) < 0) {
        av_log(NULL, AV_LOG_ERROR, "unable to open decoder\n");
        return -1;
    }

    // prepare audio output
    if (!avctx->channel_layout)
        avctx->channel_layout = av_get_default_channel_layout(avctx->channels);
    if (!avctx->channel_layout) {
        av_log(NULL, AV_LOG_ERROR, "unable to guess channel layout\n");
        return -1;
    }

    f->audio_st->discard = AVDISCARD_DEFAULT;

    memset(&f->audio_pkt, 0, sizeof(f->audio_pkt));

    if (!decode_ctx->filter_graph) {
        int err = init_filter_graph(decode_ctx);
        if (err < 0) return err;
    }
    // configure abuffer filter with correct parameters
    AVRational time_base = f->audio_st->time_base;
    snprintf(decode_ctx->args, sizeof(decode_ctx->args),
            "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%"PRIx64, 
            time_base.num, time_base.den, avctx->sample_rate,
            av_get_sample_fmt_name(avctx->sample_fmt),
            avctx->channel_layout);
    int err = avfilter_init_filter(decode_ctx->abuffer_ctx, decode_ctx->args, NULL);
    if (err < 0) {
        // cleanup_decode_ctx will clean up for us when it is called later
        av_log(NULL, AV_LOG_ERROR, "error initializing abuffer filter\n");
        return err;
    }
    if (!decode_ctx->graph_configured) {
        decode_ctx->graph_configured = 1;
        err = avfilter_graph_config(decode_ctx->filter_graph, NULL);
        if (err < 0) {
            av_log(NULL, AV_LOG_ERROR, "error configuring the filter graph\n");
            return err;
        }
    }

    return 0;
}

int decode(DecodeContext *decode_ctx, GrooveFile *file) {
    GrooveFilePrivate * f = file->internals;
    AVPacket *pkt = &f->audio_pkt;

    // if the file has not been initialized for decoding
    if (f->audio_stream_index < 0) {
        int err = init_decode(decode_ctx, file);
        if (err < 0)
            return err;
    }
    if (f->abort_request)
        return -1;
    if (decode_ctx->paused != decode_ctx->last_paused) {
        decode_ctx->last_paused = decode_ctx->paused;
        if (decode_ctx->paused) {
            av_read_pause(f->ic);
        } else {
            av_read_play(f->ic);
        }
    }
    if (f->seek_req) {
        AVCodecContext *dec = f->audio_st->codec;
        int64_t seek_target = f->seek_pos;
        int64_t seek_min    = f->seek_rel > 0 ? seek_target - f->seek_rel + 2: INT64_MIN;
        int64_t seek_max    = f->seek_rel < 0 ? seek_target - f->seek_rel - 2: INT64_MAX;
        // FIXME the +-2 is due to rounding being not done in the correct
        // direction in generation of the seek_pos/seek_rel variables
        int err = avformat_seek_file(f->ic, -1, seek_min, seek_target, seek_max,
                f->seek_flags);
        if (err < 0) {
            av_log(NULL, AV_LOG_ERROR, "%s: error while seeking\n", f->ic->filename);
        } else {
            if (decode_ctx->flush)
                decode_ctx->flush(decode_ctx);
            avcodec_flush_buffers(dec);
        }
        f->seek_req = 0;
        f->eof = 0;
    }
    if (f->eof) {
        if (f->audio_st->codec->codec->capabilities & CODEC_CAP_DELAY) {
            av_init_packet(pkt);
            pkt->data = NULL;
            pkt->size = 0;
            pkt->stream_index = f->audio_stream_index;
            if (audio_decode_frame(decode_ctx, file) > 0) {
                // keep flushing
                return 0;
            }
        }
        // this file is complete. move on
        return -1;
    }
    int err = av_read_frame(f->ic, pkt);
    if (err < 0) {
        // treat all errors as EOF, but log non-EOF errors.
        if (err != AVERROR_EOF) {
            av_log(NULL, AV_LOG_WARNING, "error reading frames\n");
        }
        f->eof = 1;
        return 0;
    }
    if (pkt->stream_index != f->audio_stream_index) {
        // we're only interested in the One True Audio Stream
        av_free_packet(pkt);
        return 0;
    }
    audio_decode_frame(decode_ctx, file);
    av_free_packet(pkt);
    return 0;
}

void cleanup_decode_ctx(DecodeContext *decode_ctx) {
    if (decode_ctx->filter_graph)
        avfilter_graph_free(&decode_ctx->filter_graph);
    avcodec_free_frame(&decode_ctx->frame);
}
