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

static int decode_interrupt_cb(void *ctx) {
    GrooveFile *file = ctx;
    if (!file)
        return 0;
    GrooveFilePrivate *f = file->internals;
    return f->abort_request;
}

GrooveFile * groove_open(char* filename) {
    if (groove_maybe_init() < 0)
        return NULL;

    GrooveFile * file = av_mallocz(sizeof(GrooveFile));
    GrooveFilePrivate * f = av_mallocz(sizeof(GrooveFilePrivate));
    if (!file || !f) {
        av_free(file);
        av_free(f);
        av_log(NULL, AV_LOG_ERROR, "Error opening file: Out of memory\n");
        return NULL;
    }
    file->internals = f;
    f->audio_stream_index = -1;

    f->ic = avformat_alloc_context();
    if (!f->ic) {
        groove_close(file);
        av_log(NULL, AV_LOG_ERROR, "error creating format context: out of memory\n");
        return NULL;
    }
    f->ic->interrupt_callback.callback = decode_interrupt_cb;
    f->ic->interrupt_callback.opaque = file;
    int err = avformat_open_input(&f->ic, filename, NULL, NULL);
    if (err < 0) {
        groove_close(file);
        av_log(NULL, AV_LOG_ERROR, "error opening %s\n", filename);
        return NULL;
    }

    err = avformat_find_stream_info(f->ic, NULL);
    if (err < 0) {
        groove_close(file);
        av_log(NULL, AV_LOG_ERROR, "%s: could not find codec parameters\n", filename);
        return NULL;
    }

    f->seek_by_bytes = !!(f->ic->iformat->flags & AVFMT_TS_DISCONT);

    // set all streams to discard. in a few lines here we will find the audio
    // stream and cancel discarding it
    for (int i = 0; i < f->ic->nb_streams; i++)
        f->ic->streams[i]->discard = AVDISCARD_ALL;

    f->audio_stream_index = av_find_best_stream(f->ic, AVMEDIA_TYPE_AUDIO, -1, -1, &f->decoder, 0);

    if (f->audio_stream_index < 0) {
        groove_close(file);
        av_log(NULL, AV_LOG_ERROR, "%s: no audio stream found\n", f->ic->filename);
        return NULL;
    }

    if (!f->decoder) {
        groove_close(file);
        av_log(NULL, AV_LOG_ERROR, "%s: no decoder found\n", f->ic->filename);
        return NULL;
    }

    f->audio_st = f->ic->streams[f->audio_stream_index];
    f->audio_st->discard = AVDISCARD_DEFAULT;

    AVCodecContext *avctx = f->audio_st->codec;

    if (avcodec_open2(avctx, f->decoder, NULL) < 0) {
        groove_close(file);
        av_log(NULL, AV_LOG_ERROR, "unable to open decoder\n");
        return NULL;
    }

    if (!avctx->channel_layout)
        avctx->channel_layout = av_get_default_channel_layout(avctx->channels);
    if (!avctx->channel_layout) {
        groove_close(file);
        av_log(NULL, AV_LOG_ERROR, "unable to guess channel layout\n");
        return NULL;
    }

    // copy the audio stream metadata to the context metadata
    AVDictionaryEntry *tag = NULL;
    while((tag = av_dict_get(f->audio_st->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
        av_dict_set(&f->ic->metadata, tag->key, tag->value, AV_DICT_IGNORE_SUFFIX);
    }

    return file;
}

// should be safe to call no matter what state the file is in
void groove_close(GrooveFile * file) {
    if (!file)
        return;

    GrooveFilePrivate * f = file->internals;

    if (f) {
        f->abort_request = 1;

        if (f->audio_stream_index >= 0) {
            AVCodecContext *avctx = f->ic->streams[f->audio_stream_index]->codec;

            av_free_packet(&f->audio_pkt);

            f->ic->streams[f->audio_stream_index]->discard = AVDISCARD_ALL;
            avcodec_close(avctx);
            f->audio_st = NULL;
            f->audio_stream_index = -1;
        }

        // disable interrupting
        f->abort_request = 0;

        if (f->ic)
            avformat_close_input(&f->ic);

        av_free(f);
    }
    av_free(file);
}

void groove_set_logging(int level) {
    groove_maybe_init();
    av_log_set_level(level);
}

char * groove_file_filename(GrooveFile *file) {
    GrooveFilePrivate * f = file->internals;
    return f->ic->filename;
}

const char * groove_file_short_names(GrooveFile *file) {
    GrooveFilePrivate * f = file->internals;
    return f->ic->iformat->name;
}

GrooveTag *groove_file_metadata_get(GrooveFile *file, const char *key,
        const GrooveTag *prev, int flags)
{
    GrooveFilePrivate *f = file->internals;
    const AVDictionaryEntry *e = prev;
    return av_dict_get(f->ic->metadata, key, e, flags|AV_DICT_IGNORE_SUFFIX);
}

int groove_file_metadata_set(GrooveFile *file, const char *key,
        const char *value, int flags)
{
    file->dirty = 1;
    GrooveFilePrivate *f = file->internals;
    return av_dict_set(&f->ic->metadata, key, value, flags|AV_DICT_IGNORE_SUFFIX);
}

const char * groove_tag_key(GrooveTag *tag) {
    AVDictionaryEntry *e = tag;
    return e->key;
}

const char * groove_tag_value(GrooveTag *tag) {
    AVDictionaryEntry *e = tag;
    return e->value;
}

// XXX this might break for some character encodings
// would love some advice on what to do instead of this
static int tempfileify(char * str, size_t max_len) {
    size_t len = strlen(str);
    if (len + 10 > max_len)
        return -1;
    char prepend[11];
    int n = rand() % 99999;
    snprintf(prepend, 11, ".tmp%05d-", n);
    // find the last slash and insert after it
    // if no slash, insert at beginning
    char * slash = strrchr(str, '/');
    char * pos = slash ? slash + 1 : str;
    size_t orig_len = len - (pos - str);
    memmove(pos + 10, pos, orig_len);
    strncpy(pos, prepend, 10);
    return 0;
}

static void cleanup_save(GrooveFile *file) {
    GrooveFilePrivate *f = file->internals;

    av_free_packet(&f->audio_pkt);
    avio_closep(&f->oc->pb);
    if (f->tempfile_exists) {
        if (remove(f->oc->filename) != 0)
            av_log(NULL, AV_LOG_WARNING, "Error deleting temp file during cleanup\n");
        f->tempfile_exists = 0;
    }
    if (f->oc) {
        avformat_free_context(f->oc);
        f->oc = NULL;
    }
}

int groove_file_save(GrooveFile *file) {
    if (!file->dirty)
        return 0;

    GrooveFilePrivate *f = file->internals;

    // detect output format
    AVOutputFormat *ofmt = av_guess_format(f->ic->iformat->name, f->ic->filename, NULL);
    if (!ofmt) {
        av_log(NULL, AV_LOG_ERROR, "Could not deduce output format to use.\n");
        return -1;
    }

    // allocate output media context
    f->oc = avformat_alloc_context();
    if (!f->oc) {
        cleanup_save(file);
        av_log(NULL, AV_LOG_ERROR, "Could not create output context: out of memory\n");
        return -1;
    }

    f->oc->oformat = ofmt;
    snprintf(f->oc->filename, sizeof(f->oc->filename), "%s", f->ic->filename);
    if (tempfileify(f->oc->filename, sizeof(f->oc->filename)) < 0) {
        cleanup_save(file);
        av_log(NULL, AV_LOG_ERROR, "could not create temp file - filename too long\n");
        return -1;
    }

    // open output file if needed
    if (!(ofmt->flags & AVFMT_NOFILE)) {
        if (avio_open(&f->oc->pb, f->oc->filename, AVIO_FLAG_WRITE) < 0) {
            cleanup_save(file);
            av_log(NULL, AV_LOG_ERROR, "could not open '%s'\n", f->oc->filename);
            return -1;
        }
        f->tempfile_exists = 1;
    }

    // add all the streams
    for (int i = 0; i < f->ic->nb_streams; i++) {
        AVStream *in_stream = f->ic->streams[i];
        AVStream *out_stream = avformat_new_stream(f->oc, NULL);
        if (!out_stream) {
            cleanup_save(file);
            av_log(NULL, AV_LOG_ERROR, "error allocating output stream\n");
            return -1;
        }
        out_stream->id = in_stream->id;
        out_stream->disposition = in_stream->disposition;

        AVCodecContext *icodec = in_stream->codec;
        AVCodecContext *ocodec = out_stream->codec;
        ocodec->bits_per_raw_sample    = icodec->bits_per_raw_sample;
        ocodec->chroma_sample_location = icodec->chroma_sample_location;
        ocodec->codec_id   = icodec->codec_id;
        ocodec->codec_type = icodec->codec_type;
        if (!ocodec->codec_tag) {
            if (!f->oc->oformat->codec_tag ||
                 av_codec_get_id (f->oc->oformat->codec_tag, icodec->codec_tag) == ocodec->codec_id ||
                 av_codec_get_tag(f->oc->oformat->codec_tag, icodec->codec_id) <= 0)
                ocodec->codec_tag = icodec->codec_tag;
        }
        ocodec->bit_rate       = icodec->bit_rate;
        ocodec->rc_max_rate    = icodec->rc_max_rate;
        ocodec->rc_buffer_size = icodec->rc_buffer_size;
        ocodec->field_order    = icodec->field_order;

        uint64_t extra_size = (uint64_t)icodec->extradata_size + FF_INPUT_BUFFER_PADDING_SIZE;
        if (extra_size > INT_MAX) {
            cleanup_save(file);
            av_log(NULL, AV_LOG_ERROR, "codec extra size too big\n");
            return AVERROR(EINVAL);
        }
        ocodec->extradata      = av_mallocz(extra_size);
        if (!ocodec->extradata) {
            cleanup_save(file);
            av_log(NULL, AV_LOG_ERROR, "could not allocate codec extradata: out of memory\n");
            return AVERROR(ENOMEM);
        }
        memcpy(ocodec->extradata, icodec->extradata, icodec->extradata_size);
        ocodec->extradata_size = icodec->extradata_size;
        ocodec->time_base      = in_stream->time_base;
        switch (ocodec->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            ocodec->channel_layout     = icodec->channel_layout;
            ocodec->sample_rate        = icodec->sample_rate;
            ocodec->channels           = icodec->channels;
            ocodec->frame_size         = icodec->frame_size;
            ocodec->audio_service_type = icodec->audio_service_type;
            ocodec->block_align        = icodec->block_align;
            break;
        case AVMEDIA_TYPE_VIDEO:
            ocodec->pix_fmt            = icodec->pix_fmt;
            ocodec->width              = icodec->width;
            ocodec->height             = icodec->height;
            ocodec->has_b_frames       = icodec->has_b_frames;
            if (!ocodec->sample_aspect_ratio.num) {
                ocodec->sample_aspect_ratio   =
                out_stream->sample_aspect_ratio =
                    in_stream->sample_aspect_ratio.num ? in_stream->sample_aspect_ratio :
                    icodec->sample_aspect_ratio.num ?
                    icodec->sample_aspect_ratio : (AVRational){0, 1};
            }
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            ocodec->width  = icodec->width;
            ocodec->height = icodec->height;
            break;
        case AVMEDIA_TYPE_DATA:
        case AVMEDIA_TYPE_ATTACHMENT:
            break;
        default:
            cleanup_save(file);
            av_log(NULL, AV_LOG_ERROR, "unrecognized stream type\n");
            return -1;
        }
    }

    // set metadata
    AVDictionaryEntry *tag = NULL;
    while((tag = av_dict_get(f->ic->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
        av_dict_set(&f->oc->metadata, tag->key, tag->value, AV_DICT_IGNORE_SUFFIX);
    }

    if (avformat_write_header(f->oc, NULL) < 0) {
        cleanup_save(file);
        av_log(NULL, AV_LOG_ERROR, "could not write header\n");
        return -1;
    }

    AVPacket *pkt = &f->audio_pkt;
    for (;;) {
        int err = av_read_frame(f->ic, pkt);
        if (err == AVERROR_EOF) {
            break;
        } else if (err < 0) {
            cleanup_save(file);
            av_log(NULL, AV_LOG_ERROR, "error reading frame\n");
            return -1;
        }
        if (av_write_frame(f->oc, pkt) < 0) {
            cleanup_save(file);
            av_log(NULL, AV_LOG_ERROR, "error writing frame\n");
            return -1;
        }
        av_free_packet(pkt);
    }

    if (av_write_trailer(f->oc) < 0) {
        cleanup_save(file);
        av_log(NULL, AV_LOG_ERROR, "could not write trailer\n");
        return -1;
    }

    if (rename(f->oc->filename, f->ic->filename) != 0) {
        cleanup_save(file);
        av_log(NULL, AV_LOG_ERROR, "error renaming tmp file to original file\n");
        return -1;
    }
    f->tempfile_exists = 0;
    cleanup_save(file);

    file->dirty = 0;
    return 0;
}
