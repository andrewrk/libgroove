/*
 * Copyright (c) 2013 Andrew Kelley
 *
 * This file is part of libgroove, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "file.hpp"
#include "util.hpp"

static int decode_interrupt_cb(void *ctx) {
    struct GrooveFilePrivate *f = (GrooveFilePrivate *)ctx;
    return f ? f->abort_request : 0;
}

struct GrooveFile *groove_file_open(const char *filename) {
    struct GrooveFilePrivate *f = allocate<GrooveFilePrivate>(1);
    if (!f) {
        av_log(NULL, AV_LOG_ERROR, "unable to allocate file context\n");
        return NULL;
    }
    struct GrooveFile *file = &f->externals;

    f->audio_stream_index = -1;
    f->seek_pos = -1;

    if (pthread_mutex_init(&f->seek_mutex, NULL) != 0) {
        deallocate(f);
        av_log(NULL, AV_LOG_ERROR, "unable to create seek mutex\n");
        return NULL;
    }

    f->ic = avformat_alloc_context();
    if (!f->ic) {
        groove_file_close(file);
        av_log(NULL, AV_LOG_ERROR, "unable to allocate format context\n");
        return NULL;
    }
    file->filename = f->ic->filename;
    f->ic->interrupt_callback.callback = decode_interrupt_cb;
    f->ic->interrupt_callback.opaque = file;
    int err = avformat_open_input(&f->ic, filename, NULL, NULL);
    if (err < 0) {
        groove_file_close(file);
        av_log(NULL, AV_LOG_INFO, "%s: unrecognized format\n", filename);
        return NULL;
    }

    err = avformat_find_stream_info(f->ic, NULL);
    if (err < 0) {
        groove_file_close(file);
        av_log(NULL, AV_LOG_ERROR, "%s: could not find codec parameters\n", filename);
        return NULL;
    }

    // set all streams to discard. in a few lines here we will find the audio
    // stream and cancel discarding it
    if (f->ic->nb_streams > INT_MAX) {
        groove_file_close(file);
        av_log(NULL, AV_LOG_ERROR, "too many streams\n");
        return NULL;
    }
    int stream_count = (int)f->ic->nb_streams;

    for (int i = 0; i < stream_count; i++)
        f->ic->streams[i]->discard = AVDISCARD_ALL;

    f->audio_stream_index = av_find_best_stream(f->ic, AVMEDIA_TYPE_AUDIO, -1, -1, &f->decoder, 0);

    if (f->audio_stream_index < 0) {
        groove_file_close(file);
        av_log(NULL, AV_LOG_INFO, "%s: no audio stream found\n", filename);
        return NULL;
    }

    if (!f->decoder) {
        groove_file_close(file);
        av_log(NULL, AV_LOG_ERROR, "%s: no decoder found\n", filename);
        return NULL;
    }

    f->audio_st = f->ic->streams[f->audio_stream_index];
    f->audio_st->discard = AVDISCARD_DEFAULT;

    AVCodecContext *avctx = f->audio_st->codec;

    if (avcodec_open2(avctx, f->decoder, NULL) < 0) {
        groove_file_close(file);
        av_log(NULL, AV_LOG_ERROR, "unable to open decoder\n");
        return NULL;
    }

    if (!avctx->channel_layout)
        avctx->channel_layout = av_get_default_channel_layout(avctx->channels);
    if (!avctx->channel_layout) {
        groove_file_close(file);
        av_log(NULL, AV_LOG_ERROR, "unable to guess channel layout\n");
        return NULL;
    }

    // copy the audio stream metadata to the context metadata
    av_dict_copy(&f->ic->metadata, f->audio_st->metadata, 0);

    return file;
}

// should be safe to call no matter what state the file is in
void groove_file_close(struct GrooveFile *file) {
    if (!file)
        return;

    struct GrooveFilePrivate *f = (struct GrooveFilePrivate *)file;

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

    pthread_mutex_destroy(&f->seek_mutex);

    deallocate(f);
}


const char *groove_file_short_names(struct GrooveFile *file) {
    struct GrooveFilePrivate *f = (struct GrooveFilePrivate *) file;
    return f->ic->iformat->name;
}

double groove_file_duration(struct GrooveFile *file) {
    struct GrooveFilePrivate *f = (struct GrooveFilePrivate *) file;
    double time_base = av_q2d(f->audio_st->time_base);
    return time_base * f->audio_st->duration;
}

void groove_file_audio_format(struct GrooveFile *file, struct GrooveAudioFormat *audio_format) {
    struct GrooveFilePrivate *f = (struct GrooveFilePrivate *) file;

    AVCodecContext *codec_ctx = f->audio_st->codec;
    audio_format->sample_rate = codec_ctx->sample_rate;
    from_ffmpeg_layout(codec_ctx->channel_layout, &audio_format->layout);
    audio_format->format = from_ffmpeg_format(codec_ctx->sample_fmt);
    audio_format->is_planar = from_ffmpeg_format_planar(codec_ctx->sample_fmt);
}

struct GrooveTag *groove_file_metadata_get(struct GrooveFile *file, const char *key,
        const struct GrooveTag *prev, int flags)
{
    struct GrooveFilePrivate *f = (struct GrooveFilePrivate *) file;
    const AVDictionaryEntry *e = (const AVDictionaryEntry *) prev;
    if (key && key[0] == 0)
        flags |= AV_DICT_IGNORE_SUFFIX;
    return (struct GrooveTag *) av_dict_get(f->ic->metadata, key, e, flags);
}

int groove_file_metadata_set(struct GrooveFile *file, const char *key,
        const char *value, int flags)
{
    file->dirty = 1;
    struct GrooveFilePrivate *f = (struct GrooveFilePrivate *) file;
    return av_dict_set(&f->ic->metadata, key, value, flags);
}

const char *groove_tag_key(struct GrooveTag *tag) {
    AVDictionaryEntry *e = (AVDictionaryEntry *) tag;
    return e->key;
}

const char *groove_tag_value(struct GrooveTag *tag) {
    AVDictionaryEntry *e = (AVDictionaryEntry *) tag;
    return e->value;
}

static void cleanup_save(struct GrooveFile *file) {
    struct GrooveFilePrivate *f = (struct GrooveFilePrivate *) file;

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

int groove_file_save_as(struct GrooveFile *file, const char *filename) {
    struct GrooveFilePrivate *f = (struct GrooveFilePrivate *) file;

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
    snprintf(f->oc->filename, sizeof(f->oc->filename), "%s", filename);

    // open output file if needed
    if (!(ofmt->flags & AVFMT_NOFILE)) {
        if (avio_open(&f->oc->pb, f->oc->filename, AVIO_FLAG_WRITE) < 0) {
            cleanup_save(file);
            av_log(NULL, AV_LOG_ERROR, "could not open '%s'\n", f->oc->filename);
            return -1;
        }
        f->tempfile_exists = 1;
    }

    if (f->ic->nb_streams > INT_MAX) {
        cleanup_save(file);
        av_log(NULL, AV_LOG_ERROR, "too many streams\n");
        return -1;
    }
    int stream_count = (int)f->ic->nb_streams;

    // add all the streams
    for (int i = 0; i < stream_count; i++) {
        AVStream *in_stream = f->ic->streams[i];
        AVStream *out_stream = avformat_new_stream(f->oc, NULL);
        if (!out_stream) {
            cleanup_save(file);
            av_log(NULL, AV_LOG_ERROR, "error allocating output stream\n");
            return -1;
        }
        out_stream->id = in_stream->id;
        out_stream->disposition = in_stream->disposition;
        out_stream->time_base = in_stream->time_base;

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
        ocodec->extradata      = allocate<uint8_t>(extra_size);
        if (!ocodec->extradata) {
            cleanup_save(file);
            av_log(NULL, AV_LOG_ERROR, "could not allocate codec extradata: out of memory\n");
            return AVERROR(ENOMEM);
        }
        memcpy(ocodec->extradata, icodec->extradata, icodec->extradata_size);
        ocodec->extradata_size = icodec->extradata_size;
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
                    icodec->sample_aspect_ratio : AVRational{0, 1};
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
    av_dict_copy(&f->oc->metadata, f->ic->metadata, 0);

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

    f->tempfile_exists = 0;
    cleanup_save(file);

    return 0;
}

int groove_file_save(struct GrooveFile *file) {
    if (!file->dirty)
        return 0;

    struct GrooveFilePrivate *f = (struct GrooveFilePrivate *) file;

    int temp_filename_len;
    char *temp_filename = groove_create_rand_name(&temp_filename_len, f->ic->filename, strlen(f->ic->filename));

    if (!temp_filename) {
        cleanup_save(file);
        av_log(NULL, AV_LOG_ERROR, "could not create temp file name - out of memory\n");
        return -1;
    }

    if (groove_file_save_as(file, temp_filename) < 0) {
        cleanup_save(file);
        return -1;
    }

    if (rename(f->oc->filename, f->ic->filename) != 0) {
        f->tempfile_exists = 1;
        cleanup_save(file);
        av_log(NULL, AV_LOG_ERROR, "error renaming tmp file to original file\n");
        return -1;
    }

    file->dirty = 0;
    return 0;
}
