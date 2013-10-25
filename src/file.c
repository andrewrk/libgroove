#include "file.h"

#include <libavutil/mem.h>
#include <libavutil/channel_layout.h>

static int decode_interrupt_cb(void *ctx) {
    GrooveFile *file = ctx;
    if (!file)
        return 0;
    GrooveFilePrivate *f = file->internals;
    return f->abort_request;
}

GrooveFile * groove_file_open(char* filename) {
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
    f->seek_pos = -1;

    f->seek_mutex = SDL_CreateMutex();
    if (!f->seek_mutex) {
        groove_file_close(file);
        av_log(NULL, AV_LOG_ERROR, "error creating seek mutex: out of memory\n");
        return NULL;
    }

    f->ic = avformat_alloc_context();
    if (!f->ic) {
        groove_file_close(file);
        av_log(NULL, AV_LOG_ERROR, "error creating format context: out of memory\n");
        return NULL;
    }
    file->filename = f->ic->filename;
    f->ic->interrupt_callback.callback = decode_interrupt_cb;
    f->ic->interrupt_callback.opaque = file;
    int err = avformat_open_input(&f->ic, filename, NULL, NULL);
    if (err < 0) {
        groove_file_close(file);
        av_log(NULL, AV_LOG_ERROR, "error opening %s\n", filename);
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
    for (int i = 0; i < f->ic->nb_streams; i++)
        f->ic->streams[i]->discard = AVDISCARD_ALL;

    f->audio_stream_index = av_find_best_stream(f->ic, AVMEDIA_TYPE_AUDIO, -1, -1, &f->decoder, 0);

    if (f->audio_stream_index < 0) {
        groove_file_close(file);
        av_log(NULL, AV_LOG_ERROR, "%s: no audio stream found\n", f->ic->filename);
        return NULL;
    }

    if (!f->decoder) {
        groove_file_close(file);
        av_log(NULL, AV_LOG_ERROR, "%s: no decoder found\n", f->ic->filename);
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
    AVDictionaryEntry *tag = NULL;
    while((tag = av_dict_get(f->audio_st->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
        av_dict_set(&f->ic->metadata, tag->key, tag->value, AV_DICT_IGNORE_SUFFIX);
    }

    return file;
}

// should be safe to call no matter what state the file is in
void groove_file_close(GrooveFile * file) {
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

        if (f->seek_mutex)
            SDL_DestroyMutex(f->seek_mutex);

        av_free(f);
    }
    av_free(file);
}


const char * groove_file_short_names(GrooveFile *file) {
    GrooveFilePrivate * f = file->internals;
    return f->ic->iformat->name;
}

double groove_file_duration(GrooveFile *file) {
    GrooveFilePrivate * f = file->internals;
    double time_base = av_q2d(f->audio_st->time_base);
    return time_base * f->audio_st->duration;
}

void groove_file_audio_format(GrooveFile *file, GrooveAudioFormat *audio_format) {
    GrooveFilePrivate * f = file->internals;

    AVCodecContext *codec_ctx = f->audio_st->codec;
    audio_format->sample_rate = codec_ctx->sample_rate;
    audio_format->channel_layout = codec_ctx->channel_layout;
    audio_format->sample_fmt = codec_ctx->sample_fmt;
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
