#include "groove.h"

#include <stdio.h>
#include <inttypes.h>

#include "libavutil/avstring.h"
#include "libavutil/dict.h"
#include "libavutil/samplefmt.h"
#include "libavformat/avformat.h"
#include "libavresample/avresample.h"
#include "libavutil/opt.h"

#include <SDL/SDL.h>
#include <SDL/SDL_thread.h>

/* SDL audio buffer size, in samples. Should be small to have precise
   A/V sync as SDL does not have hardware buffer fullness info. */
#define SDL_AUDIO_BUFFER_SIZE 1024

#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MIN_AUDIOQ_SIZE (20 * 16 * 1024)

typedef struct PacketQueue {
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    int abort_request;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;

typedef struct GrooveFilePrivate {
    int audio_stream_index;
    int abort_request; // true when we're closing the file
    char filename[1024];
    AVFormatContext *ic;
    int seek_by_bytes;
    AVCodec *decoder;
    int sdl_sample_rate;
    uint64_t sdl_channel_layout;
    int sdl_channels;
    enum AVSampleFormat sdl_sample_fmt;
    enum AVSampleFormat resample_sample_fmt;
    uint64_t resample_channel_layout;
    int resample_sample_rate;
    AVStream *audio_st;
    AVPacket audio_pkt;
    AVPacket audio_pkt_temp;
    int paused;
    int last_paused;
    int seek_req;
    int seek_flags;
    int64_t seek_pos;
    int64_t seek_rel;
    int eof;
    double audio_clock;
    AVAudioResampleContext *avr;
    AVFrame *frame;
} GrooveFilePrivate;

typedef struct GroovePlayerPrivate {
    AVPacket flush_pkt;
    SDL_Thread *thread_id;
    int abort_request;
    PacketQueue audioq;
    SDL_AudioSpec spec;
    uint8_t silence_buf[SDL_AUDIO_BUFFER_SIZE];
    uint8_t *audio_buf;
    uint8_t *audio_buf1;
    unsigned int audio_buf_size; /* in bytes */
    int audio_buf_index; /* in bytes */
} GroovePlayerPrivate;

static int initialized = 0;
static int initialized_sdl = 0;

// return < 0 if aborted, 0 if no packet and > 0 if packet.
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block) {
    AVPacketList *pkt1;
    int ret;

    SDL_LockMutex(q->mutex);

    for (;;) {
        if (q->abort_request) {
            ret = -1;
            break;
        }

        pkt1 = q->first_pkt;
        if (pkt1) {
            q->first_pkt = pkt1->next;
            if (!q->first_pkt)
                q->last_pkt = NULL;
            q->nb_packets--;
            q->size -= pkt1->pkt.size + sizeof(*pkt1);
            *pkt = pkt1->pkt;
            av_free(pkt1);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

// decode one audio frame and returns its uncompressed size
static int audio_decode_frame(GroovePlayer *player) {
    GroovePlayerPrivate * p = player->internals;
    GrooveQueueItem * item = player->queue_head;
    if (!item)
        return -1;
    GrooveFile * file = item->file;
    GrooveFilePrivate * f = file->internals;

    AVPacket *pkt_temp = &f->audio_pkt_temp;
    AVPacket *pkt = &f->audio_pkt;
    AVCodecContext *dec = f->audio_st->codec;
    int n, len1, data_size, got_frame;
    int new_packet = 0;
    int flush_complete = 0;

    for (;;) {
        /* NOTE: the audio packet can contain several frames */
        while (pkt_temp->size > 0 || (!pkt_temp->data && new_packet)) {
            if (!f->frame) {
                if (!(f->frame = avcodec_alloc_frame()))
                    return AVERROR(ENOMEM);
            } else
                avcodec_get_frame_defaults(f->frame);

            if (flush_complete)
                break;
            new_packet = 0;
            len1 = avcodec_decode_audio4(dec, f->frame, &got_frame, pkt_temp);
            if (len1 < 0) {
                /* if error, we skip the frame */
                pkt_temp->size = 0;
                break;
            }

            pkt_temp->data += len1;
            pkt_temp->size -= len1;

            if (!got_frame) {
                /* stop sending empty packets if the decoder is finished */
                if (!pkt_temp->data && dec->codec->capabilities & CODEC_CAP_DELAY)
                    flush_complete = 1;
                continue;
            }
            data_size = av_samples_get_buffer_size(NULL, dec->channels,
                                                   f->frame->nb_samples,
                                                   f->frame->format, 1);

            int audio_resample = f->frame->format         != f->sdl_sample_fmt     ||
                             f->frame->channel_layout != f->sdl_channel_layout ||
                             f->frame->sample_rate    != f->sdl_sample_rate;

            int resample_changed = f->frame->format         != f->resample_sample_fmt     ||
                               f->frame->channel_layout != f->resample_channel_layout ||
                               f->frame->sample_rate    != f->resample_sample_rate;

            if ((!f->avr && audio_resample) || resample_changed) {
                int ret;
                if (f->avr)
                    avresample_close(f->avr);
                else if (audio_resample) {
                    f->avr = avresample_alloc_context();
                    if (!f->avr) {
                        av_log(NULL, AV_LOG_ERROR, "error allocating AVAudioResampleContext\n");
                        break;
                    }
                }
                if (audio_resample) {
                    av_opt_set_int(f->avr, "in_channel_layout",  f->frame->channel_layout, 0);
                    av_opt_set_int(f->avr, "in_sample_fmt",      f->frame->format,         0);
                    av_opt_set_int(f->avr, "in_sample_rate",     f->frame->sample_rate,    0);
                    av_opt_set_int(f->avr, "out_channel_layout", f->sdl_channel_layout,    0);
                    av_opt_set_int(f->avr, "out_sample_fmt",     f->sdl_sample_fmt,        0);
                    av_opt_set_int(f->avr, "out_sample_rate",    f->sdl_sample_rate,       0);

                    if ((ret = avresample_open(f->avr)) < 0) {
                        av_log(NULL, AV_LOG_ERROR, "error initializing libavresample\n");
                        break;
                    }
                }
                f->resample_sample_fmt     = f->frame->format;
                f->resample_channel_layout = f->frame->channel_layout;
                f->resample_sample_rate    = f->frame->sample_rate;
            }

            if (audio_resample) {
                void *tmp_out;
                int out_samples, out_size, out_linesize;
                int osize      = av_get_bytes_per_sample(f->sdl_sample_fmt);
                int nb_samples = f->frame->nb_samples;

                out_size = av_samples_get_buffer_size(&out_linesize,
                                                      f->sdl_channels,
                                                      nb_samples,
                                                      f->sdl_sample_fmt, 0);
                tmp_out = av_realloc(p->audio_buf1, out_size);
                if (!tmp_out)
                    return AVERROR(ENOMEM);
                p->audio_buf1 = tmp_out;

                out_samples = avresample_convert(f->avr,
                                                 &p->audio_buf1,
                                                 out_linesize, nb_samples,
                                                 f->frame->data,
                                                 f->frame->linesize[0],
                                                 f->frame->nb_samples);
                if (out_samples < 0) {
                    av_log(NULL, AV_LOG_ERROR, "avresample_convert() failed\n");
                    break;
                }
                p->audio_buf = p->audio_buf1;
                data_size = out_samples * osize * f->sdl_channels;
            } else {
                p->audio_buf = f->frame->data[0];
            }

            // if no pts, then compute it
            n = f->sdl_channels * av_get_bytes_per_sample(f->sdl_sample_fmt);
            f->audio_clock += (double)data_size / (double)(n * f->sdl_sample_rate);
            return data_size;
        }

        // free the current packet
        if (pkt->data)
            av_free_packet(pkt);
        memset(pkt_temp, 0, sizeof(*pkt_temp));

        if (f->paused || p->audioq.abort_request) {
            return -1;
        }

        // read next packet
        if ((new_packet = packet_queue_get(&p->audioq, pkt, 1)) < 0)
            return -1;

        if (pkt->data == p->flush_pkt.data) {
            avcodec_flush_buffers(dec);
            flush_complete = 0;
        }

        *pkt_temp = *pkt;

        // if update the audio clock with the pts
        if (pkt->pts != AV_NOPTS_VALUE) {
            f->audio_clock = av_q2d(f->audio_st->time_base)*pkt->pts;
        }
    }
}

// prepare a new audio buffer
static void sdl_audio_callback(void *opaque, Uint8 *stream, int len) {
    GroovePlayer *player = opaque;
    GroovePlayerPrivate *p = player->internals;

    while (len > 0) {
        if (p->audio_buf_index >= p->audio_buf_size) {
            int audio_size = audio_decode_frame(player);
            if (audio_size < 0) {
                // if error, just output silence
                p->audio_buf      = p->silence_buf;
                p->audio_buf_size = sizeof(p->silence_buf);
            } else {
                p->audio_buf_size = audio_size;
            }
            p->audio_buf_index = 0;
        }
        int len1 = p->audio_buf_size - p->audio_buf_index;
        if (len1 > len)
            len1 = len;
        memcpy(stream, (uint8_t *)p->audio_buf + p->audio_buf_index, len1);
        len -= len1;
        stream += len1;
        p->audio_buf_index += len1;
    }
}

static int packet_queue_put(PacketQueue *q, AVPacket *pkt, AVPacket *flush_pkt) {
    AVPacketList *pkt1;

    // duplicate the packet
    if (pkt != flush_pkt && av_dup_packet(pkt) < 0)
        return -1;

    pkt1 = av_malloc(sizeof(AVPacketList));
    if (!pkt1)
        return -1;
    pkt1->pkt = *pkt;
    pkt1->next = NULL;


    SDL_LockMutex(q->mutex);

    if (!q->last_pkt)
        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;

    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size + sizeof(*pkt1);
    SDL_CondSignal(q->cond);
    SDL_UnlockMutex(q->mutex);
    return 0;
}

static void packet_queue_init(PacketQueue *q, AVPacket *pkt) {
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
    packet_queue_put(q, pkt, pkt);
}

static int decode_interrupt_cb(void *ctx) {
    GrooveFile *file = ctx;
    if (!file)
        return 0;
    GrooveFilePrivate *f = file->internals;
    return f->abort_request;
}

// open a given stream. Return < 0 if error
static int stream_component_open(GroovePlayer *player, GrooveFile *file) {
    GroovePlayerPrivate * p = player->internals;
    GrooveFilePrivate * f = file->internals;
    AVFormatContext *ic = f->ic;
    AVCodecContext *avctx = ic->streams[f->audio_stream_index]->codec;

    if (avcodec_open2(avctx, f->decoder, NULL) < 0) {
        av_log(NULL, AV_LOG_ERROR, "unable to open decoder\n");
        return -1;
    }

    // prepare audio output
    f->sdl_sample_rate = avctx->sample_rate;

    if (!avctx->channel_layout)
        avctx->channel_layout = av_get_default_channel_layout(avctx->channels);
    if (!avctx->channel_layout) {
        av_log(NULL, AV_LOG_ERROR, "unable to guess channel layout\n");
        return -1;
    }
    f->sdl_channel_layout = (avctx->channels == 1) ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO;
    f->sdl_channels = av_get_channel_layout_nb_channels(f->sdl_channel_layout);

    f->sdl_sample_fmt          = AV_SAMPLE_FMT_S16;
    f->resample_sample_fmt     = f->sdl_sample_fmt;
    f->resample_channel_layout = avctx->channel_layout;
    f->resample_sample_rate    = avctx->sample_rate;

    f->audio_st = ic->streams[f->audio_stream_index];
    f->audio_st->discard = AVDISCARD_DEFAULT;

    memset(&f->audio_pkt, 0, sizeof(f->audio_pkt));
    packet_queue_init(&p->audioq, &p->flush_pkt);
    SDL_PauseAudio(0);

    return 0;
}

static int init_decode(GroovePlayer *player, GrooveFile *file) {
    GrooveFilePrivate *f = file->internals;
    AVFormatContext *ic = avformat_alloc_context();
    if (!ic) {
        av_log(NULL, AV_LOG_ERROR, "error creating format context: out of memory\n");
        return -1;
    }
    ic->interrupt_callback.callback = decode_interrupt_cb;
    ic->interrupt_callback.opaque = file;
    int err = avformat_open_input(&ic, f->filename, NULL, NULL);
    if (err < 0) {
        avformat_free_context(ic);
        av_log(NULL, AV_LOG_ERROR, "error opening %s\n", f->filename);
        return -1;
    }
    f->ic = ic;

    err = avformat_find_stream_info(ic, NULL);
    if (err < 0) {
        f->abort_request = 0;
        avformat_close_input(&f->ic);
        avformat_free_context(ic);
        av_log(NULL, AV_LOG_ERROR, "%s: could not find codec parameters\n", f->filename);
        return -1;
    }

    f->seek_by_bytes = !!(ic->iformat->flags & AVFMT_TS_DISCONT);

    for (int i = 0; i < ic->nb_streams; i++)
        ic->streams[i]->discard = AVDISCARD_ALL;

    f->audio_stream_index = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, -1, &f->decoder, 0);

    if (f->audio_stream_index < 0) {
        f->abort_request = 0;
        avformat_close_input(&f->ic);
        avformat_free_context(ic);
        av_log(NULL, AV_LOG_ERROR, "%s: no audio stream found\n", f->filename);
        return -1;
    }

    if (!f->decoder) {
        f->abort_request = 0;
        avformat_close_input(&f->ic);
        avformat_free_context(ic);
        av_log(NULL, AV_LOG_ERROR, "%s: no decoder found\n", f->filename);
        return -1;
    }

    if (stream_component_open(player, file) < 0) {
        f->abort_request = 0;
        avformat_close_input(&f->ic);
        avformat_free_context(ic);
        return -1;
    }
    return 0;
}

static GrooveFile * remove_queue_item(GroovePlayer *player, GrooveQueueItem *item) {
    GrooveFile * file = item->file;

    if (item->prev) {
        item->prev->next = item->next;
    } else {
        player->queue_head = item->next;
    }
    if (item->next) {
        item->next->prev = item->prev;
    } else {
        player->queue_tail = item->prev;
    }
    av_free(item);

    return file;
}

static void packet_queue_flush(PacketQueue *q) {
    AVPacketList *pkt, *pkt1;

    SDL_LockMutex(q->mutex);
    for (pkt = q->first_pkt; pkt != NULL; pkt = pkt1) {
        pkt1 = pkt->next;
        av_free_packet(&pkt->pkt);
        av_freep(&pkt);
    }
    q->last_pkt = NULL;
    q->first_pkt = NULL;
    q->nb_packets = 0;
    q->size = 0;
    SDL_UnlockMutex(q->mutex);
}

// this thread is responsible for maintaining the audio queue
static int decode_thread(void *arg) {
    GroovePlayer *player = arg;
    GroovePlayerPrivate *p = player->internals;

    AVPacket pkt1;
    AVPacket *pkt = &pkt1;

    while (!p->abort_request) {
        // if playback is stopped or paused no need to do anything.
        if (player->state != GROOVE_STATE_PLAYING) {
            SDL_Delay(1);
            continue;
        }
        // if there is no song in the playlist, we are done playing
        if (!player->queue_head) {
            player->state = GROOVE_STATE_STOPPED;
            continue;
        }
        // if the queue is full, no need to read more
        if (p->audioq.size > MAX_QUEUE_SIZE || p->audioq.size > MIN_AUDIOQ_SIZE) {
            SDL_Delay(10);
            continue;
        }
        GrooveFile * file = player->queue_head->file;
        GrooveFilePrivate * f = file->internals;

        // if the file has not been initialized for decoding
        if (f->audio_stream_index < 0) {
            int err = init_decode(player, file);
            if (err < 0) {
                // we cannot play this song. skip it
                groove_player_next(player);
                continue;
            }
        }
        if (f->abort_request)
            break;
        if (f->paused != f->last_paused) {
            f->last_paused = f->paused;
            if (f->paused)
                av_read_pause(f->ic);
            else
                av_read_play(f->ic);
        }
        if (f->seek_req) {
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
                packet_queue_flush(&p->audioq);
                packet_queue_put(&p->audioq, &p->flush_pkt, &p->flush_pkt);
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
                packet_queue_put(&p->audioq, pkt, &p->flush_pkt);
            }
            // TODO: this will need to be changed to get gapless playback
            SDL_Delay(10);
            if (p->audioq.size == 0) {
                groove_player_next(player);
            }
            continue;
        }
        int err = av_read_frame(f->ic, pkt);
        if (err < 0) {
            // treat all errors as EOF, but log non-EOF errors.
            if (err != AVERROR_EOF) {
                av_log(NULL, AV_LOG_ERROR, "error reading frames\n");
            }
            f->eof = 1;
            continue;
        }
        // check if packet is in play range specified by user, then queue, otherwise discard
        if (pkt->stream_index == f->audio_stream_index) {
            packet_queue_put(&p->audioq, pkt, &p->flush_pkt);
        } else {
            av_free_packet(pkt);
        }
    }

    return 0;
}

static void deinit_network() {
    avformat_network_deinit();
}

static int maybe_init() {
    if (initialized)
        return 0;
    initialized = 1;

    /* register all codecs, demux and protocols */
    avcodec_register_all();
    av_register_all();
    avformat_network_init();

    atexit(deinit_network);

    av_log_set_level(AV_LOG_QUIET);
    return 0;
}

static int maybe_init_sdl() {
    if (initialized_sdl)
        return 0;
    initialized_sdl = 1;

    int flags = SDL_INIT_AUDIO | SDL_INIT_TIMER;
    if (SDL_Init(flags)) {
        av_log(NULL, AV_LOG_ERROR, "Could not initialize SDL - %s\n", SDL_GetError());
        return -1;
    }
    atexit(SDL_Quit);
    return 0;
}

GroovePlayer * groove_create_player() {
    if (maybe_init() < 0)
        return NULL;

    if (maybe_init_sdl() < 0)
        return NULL;

    GroovePlayer * player = av_mallocz(sizeof(GroovePlayer));
    GroovePlayerPrivate * p = av_mallocz(sizeof(GroovePlayerPrivate));
    if (!player || !p) {
        av_free(player);
        av_free(p);
        av_log(NULL, AV_LOG_ERROR, "Could not create player - out of memory\n");
        return NULL;
    }

    player->internals = p;

    SDL_AudioSpec wanted_spec;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.freq = 44100;
    wanted_spec.channels = 2;
    wanted_spec.silence = 0;
    wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = player;
    if (SDL_OpenAudio(&wanted_spec, &p->spec) < 0) {
        av_free(player);
        fprintf(stderr, "unable to open audio device: %s\n", SDL_GetError());
        return NULL;
    }

    player->volume = 1.0;

    av_init_packet(&p->flush_pkt);

    p->audio_buf_size  = 0;
    p->audio_buf_index = 0;

    p->thread_id = SDL_CreateThread(decode_thread, player);

    if (!p->thread_id) {
        av_free(player);
        av_log(NULL, AV_LOG_ERROR, "Error creating player thread: Out of memory\n");
        return NULL;
    }

    return player;
}

static void packet_queue_abort(PacketQueue *q) {
    SDL_LockMutex(q->mutex);

    q->abort_request = 1;

    SDL_CondSignal(q->cond);
    SDL_UnlockMutex(q->mutex);
}

static void packet_queue_end(PacketQueue *q)
{
    packet_queue_flush(q);
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->cond);
}

void groove_destroy_player(GroovePlayer *player) {
    groove_player_clear(player);

    GroovePlayerPrivate * p = player->internals;

    packet_queue_abort(&p->audioq);
    SDL_CloseAudio();
    packet_queue_end(&p->audioq);
    av_free_packet(&p->flush_pkt);

    p->abort_request = 1;
    SDL_WaitThread(p->thread_id, NULL);

    av_freep(&p->audio_buf1);
    p->audio_buf = NULL;

    av_free(p);
    av_free(player);
}

GrooveFile * groove_open(char* filename) {
    if (maybe_init() < 0)
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
    av_strlcpy(f->filename, filename, sizeof(f->filename));

    return file;
}

static void stream_component_close(GrooveFile *file) {
    GrooveFilePrivate * f = file->internals;

    if (f->audio_stream_index < 0)
        return;

    AVFormatContext *ic = f->ic;
    AVCodecContext *avctx = ic->streams[f->audio_stream_index]->codec;

    av_free_packet(&f->audio_pkt);
    if (f->avr)
        avresample_free(&f->avr);
    avcodec_free_frame(&f->frame);

    ic->streams[f->audio_stream_index]->discard = AVDISCARD_ALL;
    avcodec_close(avctx);
    f->audio_st = NULL;
    f->audio_stream_index = -1;
}

void groove_close(GrooveFile * file) {
    GrooveFilePrivate * f = file->internals;

    stream_component_close(file);

    // disable interrupting
    f->abort_request = 0;

    if (f->ic) {
        avformat_close_input(&f->ic);
    }

    av_free(f);
    av_free(file);
}

void groove_player_play(GroovePlayer *player) {
    GrooveQueueItem * item = player->queue_head;
    if (!item) {
        // cannot play. empty queue
        return;
    }
    GrooveFilePrivate * f = item->file->internals;
    if (player->state == GROOVE_STATE_STOPPED || player->state == GROOVE_STATE_PAUSED) {
        f->paused = 0;
        player->state = GROOVE_STATE_PLAYING;
    } else if (player->state == GROOVE_STATE_PLAYING) {
        // nothing to do
    } else {
        fprintf(stderr, "groove_player_play: player has invalid state: %i\n", player->state);
        exit(1);
    }
}

static void stream_seek(GrooveFile *file, int64_t pos, int64_t rel, int seek_by_bytes) {
    GrooveFilePrivate *f = file->internals;

    if (!f->seek_req) {
        f->seek_pos = pos;
        f->seek_rel = rel;
        f->seek_flags &= ~AVSEEK_FLAG_BYTE;
        if (seek_by_bytes)
            f->seek_flags |= AVSEEK_FLAG_BYTE;
        f->seek_req = 1;
    }
}

static double get_audio_clock(GroovePlayer *player, GrooveFile *file) {
    GroovePlayerPrivate * p = player->internals;
    GrooveFilePrivate * f = file->internals;

    double pts = f->audio_clock;
    int hw_buf_size = p->audio_buf_size - p->audio_buf_index;
    int bytes_per_sec = 0;
    if (f->audio_st) {
        bytes_per_sec = f->sdl_sample_rate * f->sdl_channels *
                        av_get_bytes_per_sample(f->sdl_sample_fmt);
    }
    if (bytes_per_sec)
        pts -= (double)hw_buf_size / bytes_per_sec;
    return pts;
}

void groove_player_seek_rel(GroovePlayer *player, double seconds) {
    GrooveQueueItem * item = player->queue_head;
    if (!item) {
        // can't seek, not playing
        return;
    }
    GrooveFile * file = item->file;
    GrooveFilePrivate * f = file->internals;

    double pos;
    double incr = seconds;
    if (f->seek_by_bytes) {
        if (f->audio_pkt.pos >= 0) {
            pos = f->audio_pkt.pos;
        } else {
            pos = avio_tell(f->ic->pb);
        }
        if (f->ic->bit_rate)
            incr *= f->ic->bit_rate / 8.0;
        else
            incr *= 180000.0;
        pos += incr;
        stream_seek(file, pos, incr, 1);
    } else {
        pos = get_audio_clock(player, file);
        pos += incr;
        stream_seek(file, (int64_t)(pos * AV_TIME_BASE), (int64_t)(incr * AV_TIME_BASE), 0);
    }
}

void groove_player_seek_abs(GroovePlayer *player, double fraction) {
    GrooveQueueItem * item = player->queue_head;
    if (!item) {
        // can't seek, not playing
        return;
    }
    GrooveFile * file = item->file;
    GrooveFilePrivate * f = file->internals;

    if (f->seek_by_bytes || f->ic->duration <= 0) {
        uint64_t size = avio_size(f->ic->pb);
        stream_seek(file, size*fraction, 0, 1);
    } else {
        int64_t ts = fraction * f->ic->duration;
        if (f->ic->start_time != AV_NOPTS_VALUE)
            ts += f->ic->start_time;
        stream_seek(file, ts, 0, 0);
    }
}

void groove_player_stop(GroovePlayer *player) {
    GrooveQueueItem * item = player->queue_head;
    if (!item) {
        // cannot stop. empty queue
        return;
    }
    GrooveFilePrivate * f = item->file->internals;
    if (player->state == GROOVE_STATE_PAUSED || player->state == GROOVE_STATE_PLAYING) {
        groove_player_seek_abs(player, 0);
        f->paused = 1;
        player->state = GROOVE_STATE_STOPPED;
    } else if (player->state == GROOVE_STATE_STOPPED) {
        // nothing to do
    } else {
        fprintf(stderr, "groove_player_stop: player has invalid state: %i\n", player->state);
        exit(1);
    }
}

void groove_player_pause(GroovePlayer *player) {
    GrooveQueueItem * item = player->queue_head;
    if (!item) {
        // cannot pause. empty queue
        return;
    }
    GrooveFilePrivate * f = item->file->internals;
    if (player->state == GROOVE_STATE_STOPPED || player->state == GROOVE_STATE_PAUSED) {
        // nothing to do
    } else if (player->state == GROOVE_STATE_PLAYING) {
        f->paused = 1;
        player->state = GROOVE_STATE_PAUSED;
    } else {
        fprintf(stderr, "groove_player_stop: player has invalid state: %i\n", player->state);
        exit(1);
    }
}

void groove_player_next(GroovePlayer *player) {
    groove_close(remove_queue_item(player, player->queue_head));
}


GrooveQueueItem * groove_player_queue(GroovePlayer *player, GrooveFile *file) {
    GrooveQueueItem * item = av_mallocz(sizeof(GrooveQueueItem));
    item->file = file;
    if (!player->queue_head) {
        player->queue_head = item;
        player->queue_tail = item;
    } else {
        player->queue_tail->next = item;
        player->queue_tail = item;
    }
    return item;
}

GrooveFile * groove_player_remove(GroovePlayer *player, GrooveQueueItem *item) {
    // if it's currently being played, we stop playback and start playback on
    // the next item
    int resume_playback = 0;
    if (item == player->queue_head && player->state != GROOVE_STATE_STOPPED) {
        groove_player_stop(player);
        resume_playback = player->state == GROOVE_STATE_PLAYING;
    }

    GrooveFile * file = remove_queue_item(player, item);

    if (resume_playback)
        groove_player_play(player);

    return file;
}

void groove_player_clear(GroovePlayer *player) {
    groove_player_stop(player);
    GrooveQueueItem * node = player->queue_head;
    while (node) {
        GrooveFile * file = groove_player_remove(player, node);
        groove_close(file);
        node = node->next;
    }
}

int groove_player_count(GroovePlayer *player) {
    GrooveQueueItem * node = player->queue_head;
    int count = 0;
    while (node) {
        count += 1;
        node = node->next;
    }
    return count;
}

void groove_set_logging(int enabled) {
    maybe_init();
    if (enabled) {
        av_log_set_level(AV_LOG_WARNING);
    } else {
        av_log_set_level(AV_LOG_QUIET);
    }
}

void groove_player_set_replaygain_mode(GroovePlayer *player, enum GrooveReplayGainMode mode) {
    player->replaygain_mode = mode;

    // TODO
}
