#include "groove.h"
#include "queue.h"
#include "buffer.h"

#include <libavutil/mem.h>
#include <libavutil/log.h>
#include <libavutil/channel_layout.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_mutex.h>
#include <string.h>

typedef struct GrooveEncoderPrivate {
    GrooveQueue *audioq;
    GrooveSink *sink;
    AVFormatContext *fmt_ctx;
    AVStream *stream;
    AVPacket pkt;
    int audioq_size; // in bytes

    // set temporarily
    GroovePlaylistItem *purge_item;

    SDL_mutex *encode_head_mutex;
    GroovePlaylistItem *encode_head;
    double encode_pos;
    GrooveAudioFormat encode_format;

    SDL_Thread *thread_id;

    AVIOContext *avio;
    unsigned char *avio_buf;

    int sent_header;
    char strbuf[512];
} GrooveEncoderPrivate;

static GrooveBuffer *end_of_q_sentinel = NULL;

static int encode_buffer(GrooveEncoder *encoder, GrooveBuffer *buffer) {
    GrooveEncoderPrivate *e = encoder->internals;

    av_init_packet(&e->pkt);

    AVFrame *frame = NULL;
    if (buffer) {
        e->encode_head = buffer->item;
        e->encode_pos = buffer->pos;
        e->encode_format = buffer->format;

        GrooveBufferPrivate *b = buffer->internals;
        frame = b->frame;
    }

    int got_packet = 0;
    int errcode = avcodec_encode_audio2(e->stream->codec, &e->pkt, frame, &got_packet);
    if (errcode < 0) {
        av_strerror(errcode, e->strbuf, sizeof(e->strbuf));
        av_log(NULL, AV_LOG_ERROR, "error encoding audio frame: %s\n", e->strbuf);
        return -1;
    }
    if (!got_packet)
        return -1;

    av_write_frame(e->fmt_ctx, &e->pkt);
    av_free_packet(&e->pkt);

    return 0;
}

static int encode_thread(void *arg) {
    GrooveEncoder *encoder = arg;
    GrooveEncoderPrivate *e = encoder->internals;

    GrooveBuffer *buffer;
    for (;;) {
        if (e->audioq_size >= encoder->encoded_buffer_size) {
            // this should ideally be done with mutex cond and signals instead
            // of a delay. https://github.com/superjoe30/libgroove/issues/24
            SDL_Delay(5);
            continue;
        }
        int result = groove_sink_get_buffer(e->sink, &buffer, 1);

        if (result == GROOVE_BUFFER_END) {
            SDL_LockMutex(e->encode_head_mutex);
            // flush encoder with empty packets
            while (encode_buffer(encoder, NULL) >= 0) {}
            // then flush format context with empty packets
            while (av_write_frame(e->fmt_ctx, NULL) == 0) {}
            groove_queue_put(e->audioq, end_of_q_sentinel);
            e->encode_head = NULL;
            e->encode_pos = -1.0;
            SDL_UnlockMutex(e->encode_head_mutex);

            // send trailer
            e->sent_header = 0;
            av_log(NULL, AV_LOG_INFO, "encoder: writing trailer\n");
            if (av_write_trailer(e->fmt_ctx) < 0) {
                av_log(NULL, AV_LOG_ERROR, "could not write trailer\n");
            }

            continue;
        }

        if (result != GROOVE_BUFFER_YES)
            break;

        if (!e->sent_header) {
            av_log(NULL, AV_LOG_INFO, "encoder: writing header\n");
            if (avformat_write_header(e->fmt_ctx, NULL) < 0) {
                av_log(NULL, AV_LOG_ERROR, "could not write header\n");
            }
            e->sent_header = 1;
        }

        SDL_LockMutex(e->encode_head_mutex);
        encode_buffer(encoder, buffer);
        SDL_UnlockMutex(e->encode_head_mutex);
        groove_buffer_unref(buffer);
    }

    return 0;
}

static void sink_purge(GrooveSink *sink, GroovePlaylistItem *item) {
    GrooveEncoder *encoder = sink->userdata;
    GrooveEncoderPrivate *e = encoder->internals;

    SDL_LockMutex(e->encode_head_mutex);
    e->purge_item = item;
    groove_queue_purge(e->audioq);
    e->purge_item = NULL;

    if (e->encode_head == item) {
        e->encode_head = NULL;
        e->encode_pos = -1.0;
    }
    SDL_UnlockMutex(e->encode_head_mutex);
}

static void sink_flush(GrooveSink *sink) {
    GrooveEncoder *encoder = sink->userdata;
    GrooveEncoderPrivate *e = encoder->internals;

    SDL_LockMutex(e->encode_head_mutex);
    groove_queue_flush(e->audioq);
    avcodec_flush_buffers(e->stream->codec);
    SDL_UnlockMutex(e->encode_head_mutex);
}

static int audioq_purge(GrooveQueue* queue, void *obj) {
    GrooveBuffer *buffer = obj;
    if (buffer == end_of_q_sentinel)
        return 0;
    GrooveEncoder *encoder = queue->context;
    GrooveEncoderPrivate *e = encoder->internals;
    return buffer->item == e->purge_item;
}

static void audioq_cleanup(GrooveQueue* queue, void *obj) {
    GrooveBuffer *buffer = obj;
    if (buffer == end_of_q_sentinel)
        return;
    GrooveEncoder *encoder = queue->context;
    GrooveEncoderPrivate *e = encoder->internals;
    e->audioq_size -= buffer->size;
    groove_buffer_unref(buffer);
}

static void audioq_put(GrooveQueue *queue, void *obj) {
    GrooveBuffer *buffer = obj;
    if (buffer == end_of_q_sentinel)
        return;
    GrooveEncoder *encoder = queue->context;
    GrooveEncoderPrivate *e = encoder->internals;
    e->audioq_size += buffer->size;
}

static void audioq_get(GrooveQueue *queue, void *obj) {
    GrooveBuffer *buffer = obj;
    if (buffer == end_of_q_sentinel)
        return;
    GrooveEncoder *encoder = queue->context;
    GrooveEncoderPrivate *e = encoder->internals;
    e->audioq_size -= buffer->size;
}

static int encoder_write_packet(void *opaque, uint8_t *buf, int buf_size) {
    GrooveEncoder *encoder = opaque;
    GrooveEncoderPrivate *e = encoder->internals;

    GrooveBuffer *buffer = av_mallocz(sizeof(GrooveBuffer));
    GrooveBufferPrivate *b = av_mallocz(sizeof(GrooveBufferPrivate));

    if (!buffer || !b) {
        av_free(buffer);
        av_free(b);
        av_log(NULL, AV_LOG_ERROR, "unable to allocate buffer\n");
        return -1;
    }

    buffer->internals = b;

    b->mutex = SDL_CreateMutex();

    if (!b->mutex) {
        av_free(buffer);
        av_free(b);
        av_log(NULL, AV_LOG_ERROR, "unable to create mutex\n");
        return -1;
    }

    buffer->item = e->encode_head;
    buffer->pos = e->encode_pos;
    buffer->format = e->encode_format;

    b->is_packet = 1;
    b->data = av_malloc(buf_size);
    if (!b->data) {
        av_free(buffer);
        av_free(b);
        SDL_DestroyMutex(b->mutex);
        av_log(NULL, AV_LOG_ERROR, "unable to create data buffer\n");
        return -1;
    }
    memcpy(b->data, buf, buf_size);

    buffer->data = &b->data;
    buffer->size = buf_size;

    b->ref_count = 1;

    groove_queue_put(e->audioq, buffer);

    return 0;
}

GrooveEncoder * groove_encoder_create() {
    GrooveEncoder *encoder = av_mallocz(sizeof(GrooveEncoder));
    GrooveEncoderPrivate *e = av_mallocz(sizeof(GrooveEncoderPrivate));

    if (!encoder || !e) {
        av_free(encoder);
        av_free(e);
        av_log(NULL, AV_LOG_ERROR, "unable to allocate encoder\n");
        return NULL;
    }
    encoder->internals = e;

    const int buffer_size = 4 * 1024;
    e->avio_buf = av_malloc(buffer_size);
    if (!e->avio_buf) {
        groove_encoder_destroy(encoder);
        av_log(NULL, AV_LOG_ERROR, "unable to allocate avio buffer\n");
        return NULL;
    }

    e->avio = avio_alloc_context(e->avio_buf, buffer_size, 1, encoder, NULL,
            encoder_write_packet, NULL);
    if (!e->avio) {
        groove_encoder_destroy(encoder);
        av_log(NULL, AV_LOG_ERROR, "unable to allocate avio context\n");
        return NULL;
    }

    e->encode_head_mutex = SDL_CreateMutex();
    if (!e->encode_head_mutex) {
        groove_encoder_destroy(encoder);
        av_log(NULL, AV_LOG_ERROR, "unable to create mutex\n");
        return NULL;
    }

    e->audioq = groove_queue_create();
    if (!e->audioq) {
        groove_encoder_destroy(encoder);
        av_log(NULL, AV_LOG_ERROR, "unable to allocate queue\n");
        return NULL;
    }
    e->audioq->context = encoder;
    e->audioq->cleanup = audioq_cleanup;
    e->audioq->put = audioq_put;
    e->audioq->get = audioq_get;
    e->audioq->purge = audioq_purge;

    e->sink = groove_sink_create();
    if (!e->sink) {
        groove_encoder_destroy(encoder);
        av_log(NULL, AV_LOG_ERROR, "unable to allocate sink\n");
        return NULL;
    }

    e->sink->userdata = encoder;
    e->sink->purge = sink_purge;
    e->sink->flush = sink_flush;

    // set some defaults
    encoder->bit_rate = 256 * 1000;
    encoder->target_audio_format.sample_rate = 44100;
    encoder->target_audio_format.sample_fmt = GROOVE_SAMPLE_FMT_S16;
    encoder->target_audio_format.channel_layout = GROOVE_CH_LAYOUT_STEREO;
    encoder->sink_buffer_size = e->sink->buffer_size;
    encoder->encoded_buffer_size = 16 * 1024;

    return encoder;
}

void groove_encoder_destroy(GrooveEncoder *encoder) {
    GrooveEncoderPrivate *e = encoder->internals;

    if (e->sink)
        groove_sink_destroy(e->sink);

    if (e->audioq)
        groove_queue_destroy(e->audioq);

    if (e->encode_head_mutex)
        SDL_DestroyMutex(e->encode_head_mutex);

    if (e->avio)
        av_free(e->avio);

    if (e->avio_buf)
        av_free(e->avio_buf);

    av_free(e);
    av_free(encoder);
}

static int abs_diff(int a, int b) {
    int n = a - b;
    return n >= 0 ? n : -n;
}

static int codec_supports_fmt(AVCodec *codec, enum GrooveSampleFormat fmt) {
    const enum GrooveSampleFormat *p = (enum GrooveSampleFormat*) codec->sample_fmts;

    while (*p != GROOVE_SAMPLE_FMT_NONE) {
        if (*p == fmt)
            return 1;
        p += 1;
    }

    return 0;
}

static enum GrooveSampleFormat closest_supported_sample_fmt(AVCodec *codec,
        enum GrooveSampleFormat target)
{
    // if we can, return exact match. otherwise, return the format with the
    // next highest sample byte count

    if (!codec->sample_fmts)
        return target;

    int target_size = av_get_bytes_per_sample(target);
    const enum GrooveSampleFormat *p = (enum GrooveSampleFormat*) codec->sample_fmts;
    enum GrooveSampleFormat best = *p;
    int best_size = av_get_bytes_per_sample(best);
    while (*p != GROOVE_SAMPLE_FMT_NONE) {
        if (*p == target)
            return target;

        int size = av_get_bytes_per_sample(*p);
        if ((best_size < target_size && size > best_size) ||
            (size >= target_size &&
             abs_diff(target_size, size) < abs_diff(target_size, best_size)))
        {
            best_size = size;
            best = *p;
        }

        p += 1;
    }

    // prefer interleaved format
    enum GrooveSampleFormat packed_best = av_get_packed_sample_fmt(best);
    return codec_supports_fmt(codec, packed_best) ? packed_best : best;
}

static int closest_supported_sample_rate(AVCodec *codec, int target) {
    // if we can, return exact match. otherwise, return the minimum sample
    // rate >= target

    if (!codec->supported_samplerates)
        return target;

    const int *p = codec->supported_samplerates;
    int best = *p;

    while (*p) {
        if (*p == target)
            return target;

        if ((best < target && *p > best) || (*p >= target &&
                    abs_diff(target, *p) < abs_diff(target, best)))
        {
            best = *p;
        }

        p += 1;
    }

    return best;
}

static uint64_t closest_supported_channel_layout(AVCodec *codec, uint64_t target) {
    // if we can, return exact match. otherwise, return the layout with the
    // minimum number of channels >= target

    if (!codec->channel_layouts)
        return target;

    int target_count = av_get_channel_layout_nb_channels(target);
    const uint64_t *p = codec->channel_layouts;
    uint64_t best = *p;
    int best_count = av_get_channel_layout_nb_channels(best);
    while (*p) {
        if (*p == target)
            return target;

        int count = av_get_channel_layout_nb_channels(*p);
        if ((best_count < target_count && count > best_count) ||
            (count >= target_count &&
             abs_diff(target_count, count) < abs_diff(target_count, best_count)))
        {
            best_count = count;
            best = *p;
        }

        p += 1;
    }

    return best;
}

void log_audio_fmt(const GrooveAudioFormat *fmt) {
    const int buf_size = 128;
    char buf[buf_size];

    av_get_channel_layout_string(buf, buf_size, 0, fmt->channel_layout);
    av_log(NULL, AV_LOG_INFO, "encoder: using audio format: %s, %d Hz, %s\n", av_get_sample_fmt_name(fmt->sample_fmt),
            fmt->sample_rate, buf);
}

int groove_encoder_attach(GrooveEncoder *encoder, GroovePlaylist *playlist) {
    GrooveEncoderPrivate *e = encoder->internals;

    encoder->playlist = playlist;
    groove_queue_reset(e->audioq);

    e->fmt_ctx = avformat_alloc_context();
    if (!e->fmt_ctx) {
        groove_encoder_detach(encoder);
        av_log(NULL, AV_LOG_ERROR, "unable to allocate format context\n");
        return -1;
    }
    e->fmt_ctx->pb = e->avio;

    e->fmt_ctx->oformat = av_guess_format(encoder->format_short_name,
            encoder->filename, encoder->mime_type);
    if (!e->fmt_ctx->oformat) {
        groove_encoder_detach(encoder);
        av_log(NULL, AV_LOG_ERROR, "unable to determine format\n");
        return -1;
    }
    // av_guess_codec ignores mime_type, filename, and codec_short_name. see
    // https://bugzilla.libav.org/show_bug.cgi?id=580
    // because of this we do a workaround to return the correct codec based on
    // the codec_short_name.
    AVCodec *codec = NULL;
    if (encoder->codec_short_name) {
        codec = avcodec_find_encoder_by_name(encoder->codec_short_name);
        if (!codec) {
            const AVCodecDescriptor *desc =
                avcodec_descriptor_get_by_name(encoder->codec_short_name);
            if (desc) {
                codec = avcodec_find_encoder(desc->id);
            }
        }
    }
    if (!codec) {
        enum AVCodecID codec_id = av_guess_codec(e->fmt_ctx->oformat,
                encoder->codec_short_name, encoder->filename, encoder->mime_type,
                AVMEDIA_TYPE_AUDIO);
        codec = avcodec_find_encoder(codec_id);
        if (!codec) {
            groove_encoder_detach(encoder);
            av_log(NULL, AV_LOG_ERROR, "unable to find encoder\n");
            return -1;
        }
    }
    av_log(NULL, AV_LOG_INFO, "encoder: using codec: %s\n", codec->long_name);

    e->stream = avformat_new_stream(e->fmt_ctx, codec);
    if (!e->stream) {
        groove_encoder_detach(encoder);
        av_log(NULL, AV_LOG_ERROR, "unable to create output stream\n");
        return -1;
    }

    encoder->actual_audio_format.sample_fmt = closest_supported_sample_fmt(
            codec, encoder->target_audio_format.sample_fmt);
    encoder->actual_audio_format.sample_rate = closest_supported_sample_rate(
            codec, encoder->target_audio_format.sample_rate);
    encoder->actual_audio_format.channel_layout = closest_supported_channel_layout(
            codec, encoder->target_audio_format.channel_layout);

    log_audio_fmt(&encoder->actual_audio_format);

    AVCodecContext *codec_ctx = e->stream->codec;
    codec_ctx->bit_rate = encoder->bit_rate;
    codec_ctx->sample_fmt = encoder->actual_audio_format.sample_fmt;
    codec_ctx->sample_rate = encoder->actual_audio_format.sample_rate;
    codec_ctx->channel_layout = encoder->actual_audio_format.channel_layout;
    codec_ctx->channels = av_get_channel_layout_nb_channels(encoder->actual_audio_format.channel_layout);
    codec_ctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

    e->stream->codec = codec_ctx;

    int errcode = avcodec_open2(codec_ctx, codec, NULL);
    if (errcode < 0) {
        groove_encoder_detach(encoder);
        av_strerror(errcode, e->strbuf, sizeof(e->strbuf));
        av_log(NULL, AV_LOG_ERROR, "unable to open codec: %s\n", e->strbuf);
        return -1;
    }

    e->sink->audio_format = encoder->actual_audio_format;
    e->sink->buffer_size = encoder->sink_buffer_size;
    e->sink->buffer_sample_count = (codec->capabilities & CODEC_CAP_VARIABLE_FRAME_SIZE) ?
        0 : codec_ctx->frame_size;

    if (groove_sink_attach(e->sink, playlist) < 0) {
        groove_encoder_detach(encoder);
        av_log(NULL, AV_LOG_ERROR, "unable to attach sink\n");
        return -1;
    }

    e->thread_id = SDL_CreateThread(encode_thread, "encode", encoder);

    if (!e->thread_id) {
        groove_encoder_detach(encoder);
        av_log(NULL, AV_LOG_ERROR, "unable to create encoder thread\n");
        return -1;
    }

    return 0;
}

int groove_encoder_detach(GrooveEncoder *encoder) {
    GrooveEncoderPrivate *e = encoder->internals;

    groove_sink_detach(e->sink);
    groove_queue_flush(e->audioq);
    groove_queue_abort(e->audioq);
    SDL_WaitThread(e->thread_id, NULL);
    e->thread_id = NULL;

    if (e->stream) {
        avcodec_close(e->stream->codec);
        // stream is freed by freeing the AVFormatContext
        e->stream = NULL;
    }

    if (e->fmt_ctx)
        avformat_free_context(e->fmt_ctx);

    e->encode_head = NULL;
    e->encode_pos = -1.0;
    e->sent_header = 0;

    encoder->playlist = NULL;
    return 0;
}

int groove_encoder_get_buffer(GrooveEncoder *encoder, GrooveBuffer **buffer,
        int block)
{
    GrooveEncoderPrivate *e = encoder->internals;

    if (groove_queue_get(e->audioq, (void**)buffer, block) == 1) {
        if (*buffer == end_of_q_sentinel) {
            *buffer = NULL;
            return GROOVE_BUFFER_END;
        } else {
            return GROOVE_BUFFER_YES;
        }
    } else {
        *buffer = NULL;
        return GROOVE_BUFFER_NO;
    }
}
