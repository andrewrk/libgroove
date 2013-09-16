#include "groove.h"
#include "decode.h"

#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>

#include <SDL/SDL.h>

static int initialized = 0;
static int initialized_sdl = 0;

static double dB_scale;

static double dB_to_float(double dB) {
    return exp(dB * dB_scale);
}

static void deinit_network() {
    avformat_network_deinit();
}

int groove_maybe_init() {
    if (initialized)
        return 0;
    initialized = 1;

    dB_scale = log(10.0) * 0.05;

    srand(time(NULL));

    // register all codecs, demux and protocols
    avcodec_register_all();
    av_register_all();
    avformat_network_init();
    avfilter_register_all();

    atexit(deinit_network);

    av_log_set_level(AV_LOG_QUIET);
    return 0;
}

int groove_maybe_init_sdl() {
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
static int audio_decode_frame(GrooveDecodeContext *decode_ctx, GrooveFile *file) {
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
            av_strerror(err, decode_ctx->strbuf, sizeof(decode_ctx->strbuf));
            av_log(NULL, AV_LOG_ERROR, "error writing frame to buffersrc: %s\n",
                    decode_ctx->strbuf);
            return -1;
        }

        // pull filtered audio from the filtergraph
        AVFrame *oframe = av_frame_alloc();
        for (;;) {
            int err = av_buffersink_get_frame(decode_ctx->abuffersink_ctx, oframe);
            if (err == AVERROR_EOF || err == AVERROR(EAGAIN))
                break;
            if (err < 0) {
                av_log(NULL, AV_LOG_ERROR, "error reading buffer from buffersink\n");
                return -1;
            }
            data_size += oframe->linesize[0];
            err = decode_ctx->buffer(decode_ctx, oframe);
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

static double get_replaygain_adjustment(GrooveDecodeContext *decode_ctx,
        GrooveFile *file, const char *tag_name)
{
    GrooveTag *tag = groove_file_metadata_get(file, "REPLAYGAIN_TRACK_GAIN", NULL, 0);
    if (tag) {
        const char *tag_value = groove_tag_value(tag);
        double gain_value;
        if (sscanf(tag_value, "%lf", &gain_value) == 1)
            return dB_to_float(gain_value);
        GrooveFilePrivate *f = file->internals;
        av_log(NULL, AV_LOG_WARNING, "track %s lacks replaygain metadata\n", f->ic->filename);
    }
    return decode_ctx->replaygain_default;
}

static int init_filter_graph(GrooveDecodeContext *decode_ctx, GrooveFile *file) {
    GrooveFilePrivate *f = file->internals;

    decode_ctx->last_decoded_file = file;

    // destruct old graph
    avfilter_graph_free(&decode_ctx->filter_graph);

    // create new graph
    decode_ctx->filter_graph = avfilter_graph_alloc();
    if (!decode_ctx->filter_graph) {
        av_log(NULL, AV_LOG_ERROR, "unable to create filter graph: out of memory\n");
        return -1;
    }

    AVFilter *abuffer = avfilter_get_by_name("abuffer");
    AVFilter *volume = avfilter_get_by_name("volume");
    AVFilter *aformat = avfilter_get_by_name("aformat");
    AVFilter *abuffersink = avfilter_get_by_name("abuffersink");

    int err;
    // create abuffer filter
    AVCodecContext *avctx = f->audio_st->codec;
    AVRational time_base = f->audio_st->time_base;
    snprintf(decode_ctx->strbuf, sizeof(decode_ctx->strbuf),
            "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%"PRIx64, 
            time_base.num, time_base.den, avctx->sample_rate,
            av_get_sample_fmt_name(avctx->sample_fmt),
            avctx->channel_layout);
    av_log(NULL, AV_LOG_INFO, "abuffer: %s\n", decode_ctx->strbuf);
    // save these values so we can compare later and check
    // whether we have to reconstruct the graph
    decode_ctx->in_sample_rate = avctx->sample_rate;
    decode_ctx->in_channel_layout = avctx->channel_layout;
    decode_ctx->in_sample_fmt = avctx->sample_fmt;
    decode_ctx->in_time_base = time_base;
    err = avfilter_graph_create_filter(&decode_ctx->abuffer_ctx, abuffer,
            NULL, decode_ctx->strbuf, NULL, decode_ctx->filter_graph);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "error initializing abuffer filter\n");
        return err;
    }
    // create volume filter
    double vol = decode_ctx->volume;
    switch(decode_ctx->replaygain_mode) {
    case GROOVE_REPLAYGAINMODE_TRACK:
        vol *= get_replaygain_adjustment(decode_ctx, file, "REPLAYGAIN_TRACK_GAIN");
        vol *= decode_ctx->replaygain_preamp;
        break;
    case GROOVE_REPLAYGAINMODE_ALBUM:
        vol *= get_replaygain_adjustment(decode_ctx, file, "REPLAYGAIN_ALBUM_GAIN");
        vol *= decode_ctx->replaygain_preamp;
        break;
    case GROOVE_REPLAYGAINMODE_OFF:
        break;
    }
    if (vol > 1.0) vol = 1.0;
    if (vol < 0.0) vol = 0.0;
    snprintf(decode_ctx->strbuf, sizeof(decode_ctx->strbuf), "volume=%f", vol);
    av_log(NULL, AV_LOG_INFO, "volume: %s\n", decode_ctx->strbuf);
    // save these values so we can compare later and check
    // whether we have to reconstruct the graph
    decode_ctx->filter_replaygain_mode = decode_ctx->replaygain_mode;
    decode_ctx->filter_replaygain_preamp = decode_ctx->replaygain_preamp;
    decode_ctx->filter_replaygain_default = decode_ctx->replaygain_default;
    decode_ctx->filter_volume = decode_ctx->volume;
    err = avfilter_graph_create_filter(&decode_ctx->volume_ctx, volume, NULL,
            decode_ctx->strbuf, NULL, decode_ctx->filter_graph);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "error initializing volume filter\n");
        return err;
    }
    // create aformat filter
    snprintf(decode_ctx->strbuf, sizeof(decode_ctx->strbuf),
            "sample_fmts=%s:sample_rates=%d:channel_layouts=0x%"PRIx64,
            av_get_sample_fmt_name(decode_ctx->dest_sample_fmt),
            decode_ctx->dest_sample_rate,
            decode_ctx->dest_channel_layout);
    av_log(NULL, AV_LOG_INFO, "aformat: %s\n", decode_ctx->strbuf);
    err = avfilter_graph_create_filter(&decode_ctx->aformat_ctx, aformat,
            NULL, decode_ctx->strbuf, NULL, decode_ctx->filter_graph);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "unable to create aformat filter\n");
        return err;
    }
    // create abuffersink filter
    err = avfilter_graph_create_filter(&decode_ctx->abuffersink_ctx, abuffersink,
            NULL, NULL, NULL, decode_ctx->filter_graph);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "unable to create aformat filter\n");
        return err;
    }

    // connect inputs and outputs
    if (err >= 0) err = avfilter_link(decode_ctx->abuffer_ctx, 0, decode_ctx->volume_ctx, 0);
    if (err >= 0) err = avfilter_link(decode_ctx->volume_ctx, 0, decode_ctx->aformat_ctx, 0);
    if (err >= 0) err = avfilter_link(decode_ctx->aformat_ctx, 0, decode_ctx->abuffersink_ctx, 0);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "error connecting filters\n");
        return err;
    }
    err = avfilter_graph_config(decode_ctx->filter_graph, NULL);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "error configuring the filter graph\n");
        return err;
    }

    return 0;
}

static int maybe_init_filter_graph(GrooveDecodeContext *decode_ctx, GrooveFile *file) {
    if (!decode_ctx->filter_graph)
        return init_filter_graph(decode_ctx, file);

    GrooveFilePrivate *f = file->internals;
    AVCodecContext *avctx = f->audio_st->codec;
    AVRational time_base = f->audio_st->time_base;

    // if the input format stuff has changed, then we need to re-build the graph
    if (decode_ctx->in_sample_rate != avctx->sample_rate ||
        decode_ctx->in_channel_layout != avctx->channel_layout ||
        decode_ctx->in_sample_fmt != avctx->sample_fmt ||
        decode_ctx->in_time_base.num != time_base.num ||
        decode_ctx->in_time_base.den != time_base.den)
    {
        return init_filter_graph(decode_ctx, file);
    }

    // if any of the volume settings have changed, we need to re-build the graph
    if (decode_ctx->replaygain_mode != decode_ctx->filter_replaygain_mode ||
        decode_ctx->replaygain_preamp != decode_ctx->filter_replaygain_preamp ||
        decode_ctx->replaygain_default != decode_ctx->filter_replaygain_default ||
        decode_ctx->volume != decode_ctx->filter_volume)
    {
        return init_filter_graph(decode_ctx, file);
    }

    if (decode_ctx->last_decoded_file != file) {
        return init_filter_graph(decode_ctx, file);
    }

    return 0;
}


int groove_decode(GrooveDecodeContext *decode_ctx, GrooveFile *file) {
    GrooveFilePrivate * f = file->internals;
    AVPacket *pkt = &f->audio_pkt;

    if (maybe_init_filter_graph(decode_ctx, file) < 0)
        return -1;
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

void groove_cleanup_decode_ctx(GrooveDecodeContext *decode_ctx) {
    decode_ctx->last_decoded_file = NULL;
    avfilter_graph_free(&decode_ctx->filter_graph);
    avcodec_free_frame(&decode_ctx->frame);
}

int groove_init_decode_ctx(GrooveDecodeContext *decode_ctx) {
    decode_ctx->frame = avcodec_alloc_frame();
    if (!decode_ctx->frame) {
        av_log(NULL, AV_LOG_ERROR, "unable to alloc frame: out of memory\n");
        return -1;
    }
    decode_ctx->dest_channel_count =
        av_get_channel_layout_nb_channels(decode_ctx->dest_channel_layout);

    decode_ctx->volume = 1.0;
    decode_ctx->replaygain_preamp = 0.75;
    decode_ctx->replaygain_default = 0.25;
    decode_ctx->replaygain_mode = GROOVE_REPLAYGAINMODE_ALBUM;
    return 0;
}
