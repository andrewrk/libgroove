#include "groove.h"
#include "queue.h"
#include "buffer.h"

#include <libavutil/mem.h>
#include <libavutil/log.h>
#include <libavutil/channel_layout.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <SDL2/SDL_mutex.h>

typedef struct GrooveEncoderPrivate {
    GrooveQueue *audioq;
    GrooveSink *sink;
    AVCodecContext *codec_ctx;
    AVPacket *pkt;
    GrooveBuffer *dest_buf;

    // set temporarily
    GroovePlaylistItem *purge_item;

    SDL_mutex *encode_head_mutex;
    GroovePlaylistItem *encode_head;
    double encode_pos;
    GrooveAudioFormat encode_format;

    SDL_Thread *thread_id;
} GrooveEncoderPrivate;

static GrooveBuffer *end_of_q_sentinel = NULL;

static int encode_buffer(GrooveEncoder *encoder, GrooveBuffer *src_buf) {
    GrooveEncoderPrivate *e = encoder->internals;

    if (!e->pkt) {
        e->pkt = av_mallocz(sizeof(AVPacket));
        if (!e->pkt) {
            av_log(NULL, AV_LOG_ERROR, "unable to allocate packet\n");
            return -1;
        }
        av_init_packet(e->pkt);
    }

    if (src_buf) {
        e->encode_head = src_buf->item;
        e->encode_pos = src_buf->pos;
        e->encode_format = src_buf->format;
    }

    if (!e->dest_buf) {
        GrooveBuffer *dest_buf = av_mallocz(sizeof(GrooveBuffer));
        GrooveBufferPrivate *dest_b = av_mallocz(sizeof(GrooveBufferPrivate));

        if (!dest_buf || !dest_b) {
            av_free(dest_buf);
            av_free(dest_b);
            av_log(NULL, AV_LOG_ERROR, "unable to allocate buffer\n");
            return -1;
        }

        dest_buf->internals = dest_b;

        dest_b->mutex = SDL_CreateMutex();

        if (!dest_b->mutex) {
            av_free(dest_buf);
            av_free(dest_b);
            av_log(NULL, AV_LOG_ERROR, "unable to create mutex\n");
            return -1;
        }

        dest_b->is_packet = 1;

        dest_buf->item = e->encode_head;
        dest_buf->pos = e->encode_pos;
        dest_buf->format = e->encode_format;

        e->dest_buf = dest_buf;
    }

    AVFrame *frame = NULL;
    if (src_buf) {
        GrooveBufferPrivate *src_b = src_buf->internals;
        frame = src_b->frame;
    }

    int got_packet = 0;
    int errcode = avcodec_encode_audio2(e->codec_ctx, e->pkt, frame, &got_packet);
    if (errcode < 0) {
        av_log(NULL, AV_LOG_ERROR, "error encoding audio frame\n");
        return -1;
    }
    if (!got_packet)
        return -1;

    e->dest_buf->data[0] = e->pkt->data;
    e->dest_buf->size = e->pkt->size;

    GrooveBufferPrivate *dest_b = e->dest_buf->internals;
    dest_b->packet = e->pkt;
    e->pkt = NULL;

    groove_queue_put(e->audioq, e->dest_buf);
    e->dest_buf = NULL;

    return 0;
}

static int encode_thread(void *arg) {
    GrooveEncoder *encoder = arg;
    GrooveEncoderPrivate *e = encoder->internals;

    GrooveBuffer *buffer;
    for (;;) {
        int result = groove_sink_get_buffer(e->sink, &buffer, 1);

        if (result == GROOVE_BUFFER_END) {
            // flush encoder with empty packets
            SDL_LockMutex(e->encode_head_mutex);
            while (encode_buffer(encoder, NULL) >= 0) {}
            groove_queue_put(e->audioq, end_of_q_sentinel);
            SDL_UnlockMutex(e->encode_head_mutex);
            continue;
        }

        if (result != GROOVE_BUFFER_YES)
            break;

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
    avcodec_flush_buffers(e->codec_ctx);
    SDL_UnlockMutex(e->encode_head_mutex);
}

static int audioq_purge(GrooveQueue* queue, void *obj) {
    GrooveEncoder *encoder = queue->context;
    GrooveEncoderPrivate *e = encoder->internals;
    GrooveBuffer *buffer = obj;
    return buffer->item == e->purge_item;
}

static void audioq_cleanup(GrooveQueue* queue, void *obj) {
    GrooveBuffer *buffer = obj;
    groove_buffer_unref(buffer);
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
    encoder->target_audio_format.sample_rate = 44100;
    encoder->target_audio_format.sample_fmt = GROOVE_SAMPLE_FMT_S16;
    encoder->target_audio_format.channel_layout = GROOVE_CH_LAYOUT_STEREO;

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

    av_free(e);
    av_free(encoder);
}

static enum GrooveSampleFormat closest_supported_sample_fmt(AVCodec *codec, enum GrooveSampleFormat target) {
    // TODO actually check
    return target;
}

static int closest_supported_sample_rate(AVCodec *codec, int target) {
    // TODO actually check
    return target;
}

static uint64_t closest_supported_channel_layout(AVCodec *codec, uint64_t target) {
    // TODO actually check
    return target;
}

int groove_encoder_attach(GrooveEncoder *encoder, GroovePlaylist *playlist) {
    GrooveEncoderPrivate *e = encoder->internals;

    encoder->playlist = playlist;
    groove_queue_reset(e->audioq);

    AVOutputFormat *output_fmt = av_guess_format(encoder->format_short_name,
            encoder->filename, encoder->mime_type);
    enum AVCodecID codec_id = av_guess_codec(output_fmt,
            encoder->codec_short_name, encoder->filename, encoder->mime_type,
            AVMEDIA_TYPE_AUDIO);
    AVCodec *codec = avcodec_find_encoder(codec_id);
    if (!codec) {
        groove_encoder_detach(encoder);
        av_log(NULL, AV_LOG_ERROR, "unable to find encoder\n");
        return -1;
    }

    encoder->actual_audio_format.sample_fmt = closest_supported_sample_fmt(
            codec, encoder->target_audio_format.sample_fmt);
    encoder->actual_audio_format.sample_rate = closest_supported_sample_rate(
            codec, encoder->target_audio_format.sample_rate);
    encoder->actual_audio_format.channel_layout = closest_supported_channel_layout(
            codec, encoder->target_audio_format.channel_layout);

    e->codec_ctx = avcodec_alloc_context3(codec);
    e->codec_ctx->bit_rate = encoder->bit_rate;
    e->codec_ctx->sample_fmt = encoder->actual_audio_format.sample_fmt;
    e->codec_ctx->sample_rate = encoder->actual_audio_format.sample_rate;
    e->codec_ctx->channel_layout = encoder->actual_audio_format.channel_layout;
    e->codec_ctx->channels = av_get_channel_layout_nb_channels(encoder->actual_audio_format.channel_layout);

    if (avcodec_open2(e->codec_ctx, codec, NULL) < 0) {
        groove_encoder_detach(encoder);
        av_log(NULL, AV_LOG_ERROR, "unable to open codec\n");
        return -1;
    }

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

    if (e->codec_ctx) {
        avcodec_close(e->codec_ctx);
        av_free(e->codec_ctx);
        e->codec_ctx = NULL;
    }

    if (e->pkt) {
        av_free_packet(e->pkt);
        av_free(e->pkt);
        e->pkt = NULL;
    }

    if (e->dest_buf) {
        groove_buffer_unref(e->dest_buf);
        e->dest_buf = NULL;
    }

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

int groove_encoder_get_header(GrooveEncoder *encoder, GrooveBuffer **buffer) {
    // TODO implement
    return -1;
}

int groove_encoder_get_trailer(GrooveEncoder *encoder, GrooveBuffer **buffer) {
    // TODO implement
    return -1;
}
