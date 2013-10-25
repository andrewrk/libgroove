#include "groove.h"
#include "queue.h"

#include <libavutil/mem.h>
#include <libavutil/log.h>

typedef struct GrooveEncoderPrivate {
    GrooveQueue *audioq;
    GrooveSink *sink;
    AVCodecContext *codec_ctx;
} GrooveEncoderPrivate;

static GrooveBuffer *end_of_q_sentinel = NULL;

static int encode_thread(void *arg) {
    GrooveEncoder *encoder = arg;
    GrooveEncoderPrivate *e = encoder->internals;

    GrooveBuffer *buffer;
    for (;;) {
        int result = groove_sink_get_buffer(e->sink, &buffer, 1);

        if (result == GROOVE_BUFFER_END)
            continue;

        if (result != GROOVE_BUFFER_YES)
            break;

        GrooveBufferPrivate *b = buffer->internals;
        int got_packet = 0;
        int errcode = avcodec_encode_audio2(e->codec_ctx, &pkt, b->frame, &got_packet);
        groove_buffer_unref(buffer);
        if (errcode < 0) {
            av_log(NULL, AV_LOG_ERROR, "error encoding audio frame\n");
            continue;
        }
        if (got_packet) {
            // TODO create buffer and push onto our queue
        }
    }

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

    e->audioq = groove_queue_create();
    if (!e->audioq) {
        groove_encoder_destroy(encoder);
        av_log(NULL, AV_LOG_ERROR, "unable to allocate queue\n");
        return NULL;
    }

    e->sink = groove_sink_create();
    if (!e->sink) {
        groove_encoder_destroy(encoder);
        av_log(NULL, AV_LOG_ERROR, "unable to allocate sink\n");
        return NULL;
    }

    return encoder;
}

void groove_encoder_destroy(GrooveEncoder *encoder) {
    GrooveEncoderPrivate *e = encoder->internals;

    if (e->audioq)
        groove_queue_destroy(e->audioq);

    av_free(e);
    av_free(encoder);
}

int groove_encoder_attach(GrooveEncoder *encoder, GroovePlaylist *playlist) {
    GrooveEncoderPrivate *e = encoder->internals;

    encoder->playlist = playlist;
    groove_queue_reset(e->audioq);

    AVOutputFormat *output_fmt = av_guess_format(encoder->format_short_name,
            encoder->filename, encoder->mime_type);
    enum AVCodecID codec_id = av_guess_codec(e->output_fmt,
            encoder->codec_short_name, encoder->filename, encoder->mime_type,
            AVMEDIA_TYPE_AUDIO);
    AVCodec *codec = avcodec_find_encoder(codec_id);
    if (!codec) {
        groove_encoder_detach(encoder);
        av_log(NULL, AV_LOG_ERROR, "unable to find encoder\n");
        return -1;
    }

    encoder->actual_audio_format.sample_fmt = closest_supported_sample_fmt(
            codec, encoder->audio_format.sample_fmt);
    encoder->actual_audio_format.sample_rate = closest_supported_sample_rate(
            codec, encoder->audio_format.sample_rate);
    encoder->actual_audio_format.channel_layout = closest_supported_channel_layout(
            codec, encoder->audio_format.channel_layout);

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

    encoder->playlist = NULL;
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

}

int groove_encoder_get_trailer(GrooveEncoder *encoder, GrooveBuffer **buffer) {

}
