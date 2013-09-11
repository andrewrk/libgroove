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

// SDL audio buffer size, in samples. Should be small because there is no way
// to clear the buffer.
#define SDL_AUDIO_BUFFER_SIZE 1024

// How many bytes to decode before telling the decode thread to sleep rather
// than decode more. Since we usually output 16-bit 44100hz stereo to the
// audio device, 44100 * 2 * 2 would be one second.
// Note that when the queue is full, we call SDL_Delay(QUEUE_FULL_DELAY)
// so make sure that the delay finishes well before the queue runs out.
// QUEUE_FULL_DELAY is in milliseconds.
#define MIN_AUDIOQ_SIZE (44100 * 2 * 2 / 4)
#define QUEUE_FULL_DELAY 10

typedef struct BufferList {
    uint8_t * buffer;
    int size;
    struct BufferList * next;
} BufferList;

typedef struct BufferQueue {
    BufferList *first_buf;
    BufferList *last_buf;
    int nb_buffers;
    int size;
    int abort_request;
    SDL_mutex *mutex;
    SDL_cond *cond;
} BufferQueue;

typedef struct GrooveFilePrivate {
    int audio_stream_index;
    int abort_request; // true when we're closing the file
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
    int seek_req;
    int seek_flags;
    int64_t seek_pos;
    int64_t seek_rel;
    int eof;
    double audio_clock;
    AVAudioResampleContext *avr;
    AVPacket audio_pkt;
} GrooveFilePrivate;

typedef struct GroovePlayerPrivate {
    SDL_Thread *thread_id;
    int abort_request;
    BufferQueue audioq;
    SDL_AudioSpec spec;
    uint8_t *audio_buf;
    unsigned int audio_buf_size; // in bytes
    int audio_buf_index; // in bytes
    AVPacket audio_pkt_temp;
    AVFrame *frame;
    int paused;
    int last_paused;
} GroovePlayerPrivate;

static int initialized = 0;
static int initialized_sdl = 0;

static int buffer_queue_put(BufferQueue *q, BufferList *buf_list) {
    BufferList * buf1 = av_malloc(sizeof(BufferList));

    if (!buf1)
        return -1;

    *buf1 = *buf_list;
    buf1->next = NULL;

    SDL_LockMutex(q->mutex);

    if (!q->last_buf)
        q->first_buf = buf1;
    else
        q->last_buf->next = buf1;
    q->last_buf = buf1;

    q->nb_buffers += 1;
    q->size += buf1->size;

    SDL_CondSignal(q->cond);
    SDL_UnlockMutex(q->mutex);

    return 0;
}

// decode one audio packet and return its uncompressed size
static int audio_decode_frame(GroovePlayer *player, GrooveFile *file) {
    GroovePlayerPrivate * p = player->internals;
    GrooveFilePrivate * f = file->internals;

    AVPacket *pkt = &f->audio_pkt;
    AVCodecContext *dec = f->audio_st->codec;

    AVPacket *pkt_temp = &p->audio_pkt_temp;
    *pkt_temp = *pkt;

    // update the audio clock with the pts if we can
    if (pkt->pts != AV_NOPTS_VALUE) {
        f->audio_clock = av_q2d(f->audio_st->time_base)*pkt->pts;
    }

    int data_size = 0;
    int n, len1, got_frame;
    int new_packet = 1;

    // NOTE: the audio packet can contain several frames
    while (pkt_temp->size > 0 || (!pkt_temp->data && new_packet)) {
        avcodec_get_frame_defaults(p->frame);
        new_packet = 0;

        len1 = avcodec_decode_audio4(dec, p->frame, &got_frame, pkt_temp);
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
        data_size = av_samples_get_buffer_size(NULL, dec->channels,
                       p->frame->nb_samples, p->frame->format, 1);

        int audio_resample = p->frame->format     != f->sdl_sample_fmt     ||
                         p->frame->channel_layout != f->sdl_channel_layout ||
                         p->frame->sample_rate    != f->sdl_sample_rate;

        int resample_changed = p->frame->format     != f->resample_sample_fmt     ||
                           p->frame->channel_layout != f->resample_channel_layout ||
                           p->frame->sample_rate    != f->resample_sample_rate;

        if ((!f->avr && audio_resample) || resample_changed) {
            int ret;
            if (f->avr) {
                avresample_close(f->avr);
            } else if (audio_resample) {
                f->avr = avresample_alloc_context();
                if (!f->avr) {
                    av_log(NULL, AV_LOG_ERROR, "error allocating AVAudioResampleContext\n");
                    return -1;
                }
            }
            if (audio_resample) {
                av_opt_set_int(f->avr, "in_channel_layout",  p->frame->channel_layout, 0);
                av_opt_set_int(f->avr, "in_sample_fmt",      p->frame->format,         0);
                av_opt_set_int(f->avr, "in_sample_rate",     p->frame->sample_rate,    0);
                av_opt_set_int(f->avr, "out_channel_layout", f->sdl_channel_layout,    0);
                av_opt_set_int(f->avr, "out_sample_fmt",     f->sdl_sample_fmt,        0);
                av_opt_set_int(f->avr, "out_sample_rate",    f->sdl_sample_rate,       0);

                if ((ret = avresample_open(f->avr)) < 0) {
                    av_log(NULL, AV_LOG_ERROR, "error initializing libavresample\n");
                    return -1;
                }
            }
            f->resample_sample_fmt     = p->frame->format;
            f->resample_channel_layout = p->frame->channel_layout;
            f->resample_sample_rate    = p->frame->sample_rate;
        }

        BufferList buf_list;
        if (audio_resample) {
            int osize      = av_get_bytes_per_sample(f->sdl_sample_fmt);
            int nb_samples = p->frame->nb_samples;

            int out_linesize;
            buf_list.size = av_samples_get_buffer_size(&out_linesize,
                                  f->sdl_channels, nb_samples, f->sdl_sample_fmt, 0);
            buf_list.buffer = av_malloc(buf_list.size);
            if (!buf_list.buffer) {
                av_log(NULL, AV_LOG_ERROR, "error allocating buffer: out of memory\n");
                return -1;
            }

            int out_samples = avresample_convert(f->avr, &buf_list.buffer,
                    out_linesize, nb_samples, p->frame->data,
                    p->frame->linesize[0], p->frame->nb_samples);
            if (out_samples < 0) {
                av_log(NULL, AV_LOG_ERROR, "avresample_convert() failed\n");
                break;
            }
            data_size = out_samples * osize * f->sdl_channels;
            buf_list.size = data_size;
        } else {
            buf_list.size = data_size;
            buf_list.buffer = av_malloc(buf_list.size);
            if (!buf_list.buffer) {
                av_log(NULL, AV_LOG_ERROR, "error allocating buffer: out of memory\n");
                return -1;
            }
            memcpy(buf_list.buffer, p->frame->data[0], buf_list.size);
        }
        if (buffer_queue_put(&p->audioq, &buf_list) < 0) {
            av_log(NULL, AV_LOG_ERROR, "error allocating buffer queue item: out of memory\n");
            return -1;
        }
        // if no pts, then compute it
        if (pkt->pts == AV_NOPTS_VALUE) {
            n = f->sdl_channels * av_get_bytes_per_sample(f->sdl_sample_fmt);
            f->audio_clock += (double)data_size / (double)(n * f->sdl_sample_rate);
        }
        return data_size;
    }
    return data_size;
}

// return < 0 if aborted, 0 if no buffer and > 0 if buffer.
static int buffer_queue_get(BufferQueue *q, BufferList *buf_list, int block) {
    BufferList *buf1;
    int ret;

    SDL_LockMutex(q->mutex);

    for (;;) {
        if (q->abort_request) {
            ret = -1;
            break;
        }

        buf1 = q->first_buf;
        if (buf1) {
            q->first_buf = buf1->next;
            if (!q->first_buf)
                q->last_buf = NULL;
            q->nb_buffers -= 1;
            q->size -= buf1->size;
            *buf_list = *buf1;
            av_free(buf1);
            ret = 1;
            break;
        } else if(!block) {
            ret = 0;
            break;
        } else {
            SDL_CondWait(q->cond, q->mutex);
        }
    }

    SDL_UnlockMutex(q->mutex);
    return ret;
}

// prepare a new audio buffer
static void sdl_audio_callback(void *opaque, Uint8 *stream, int len) {
    GroovePlayer *player = opaque;
    GroovePlayerPrivate *p = player->internals;

    BufferList buf_list;

    while (len > 0) {
        if (!p->paused && p->audio_buf_index >= p->audio_buf_size) {
            av_freep(&p->audio_buf);
            p->audio_buf_index = 0;
            p->audio_buf_size = 0;
            if (buffer_queue_get(&p->audioq, &buf_list, 1) > 0) {
                p->audio_buf = buf_list.buffer;
                p->audio_buf_size = buf_list.size;
            }
        }
        if (p->paused || !p->audio_buf) {
            // fill with silence
            memset(stream, 0, len);
            break;
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

static void buffer_queue_init(BufferQueue *q) {
    memset(q, 0, sizeof(BufferQueue));
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
}

static int decode_interrupt_cb(void *ctx) {
    GrooveFile *file = ctx;
    if (!file)
        return 0;
    GrooveFilePrivate *f = file->internals;
    return f->abort_request;
}

// return < 0 if error
static int init_decode(GrooveFile *file) {
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

    AVCodecContext *avctx = f->ic->streams[f->audio_stream_index]->codec;

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

    f->audio_st = f->ic->streams[f->audio_stream_index];
    f->audio_st->discard = AVDISCARD_DEFAULT;

    memset(&f->audio_pkt, 0, sizeof(f->audio_pkt));

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

static void buffer_queue_flush(BufferQueue *q) {
    SDL_LockMutex(q->mutex);

    BufferList *buf;
    BufferList *buf1;
    for (buf = q->first_buf; buf != NULL; buf = buf1) {
        buf1 = buf->next;
        av_free(buf->buffer);
        av_free(buf);
    }
    q->last_buf = NULL;
    q->first_buf = NULL;
    q->nb_buffers = 0;
    q->size = 0;
    SDL_UnlockMutex(q->mutex);
}

static void next_without_flush(GroovePlayer *player) {
    groove_close(remove_queue_item(player, player->queue_head));
}

// this thread is responsible for maintaining the audio queue
static int decode_thread(void *arg) {
    GroovePlayer *player = arg;
    GroovePlayerPrivate *p = player->internals;

    while (!p->abort_request) {
        // if playback is stopped or paused no need to do anything.
        if (player->state == GROOVE_STATE_STOPPED) {
            SDL_Delay(1);
            continue;
        }
        // if there is no song in the playlist, we are done playing
        if (!player->queue_head) {
            player->state = GROOVE_STATE_STOPPED;
            continue;
        }
        // if the queue is full, no need to read more
        if (p->audioq.size > MIN_AUDIOQ_SIZE) {
            SDL_Delay(QUEUE_FULL_DELAY);
            continue;
        }
        GrooveFile * file = player->queue_head->file;
        GrooveFilePrivate * f = file->internals;
        AVPacket *pkt = &f->audio_pkt;

        // if the file has not been initialized for decoding
        if (f->audio_stream_index < 0) {
            int err = init_decode(file);
            if (err < 0) {
                // we cannot play this song. skip it
                next_without_flush(player);
                continue;
            }
        }
        if (f->abort_request)
            break;
        if (p->paused != p->last_paused) {
            p->last_paused = p->paused;
            if (p->paused) {
                av_read_pause(f->ic);
            } else {
                av_read_play(f->ic);
            }
        }
        AVCodecContext *dec = f->audio_st->codec;
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
                buffer_queue_flush(&p->audioq);
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
                if (audio_decode_frame(player, file) > 0) {
                    // keep flushing
                    continue;
                }
            }
            next_without_flush(player);
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
        if (pkt->stream_index != f->audio_stream_index) {
            // we're only interested in the One True Audio Stream
            av_free_packet(pkt);
            continue;
        }
        audio_decode_frame(player, file);
        av_free_packet(pkt);
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

    // register all codecs, demux and protocols
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

    p->frame = avcodec_alloc_frame();
    if (!p->frame) {
        av_free(player);
        av_free(p);
        av_log(NULL, AV_LOG_ERROR, "unable to alloc frame: out of memory\n");
        return NULL;
    }

    p->audio_buf_size  = 0;
    p->audio_buf_index = 0;

    p->thread_id = SDL_CreateThread(decode_thread, player);

    if (!p->thread_id) {
        av_free(player);
        av_log(NULL, AV_LOG_ERROR, "Error creating player thread: Out of memory\n");
        return NULL;
    }

    buffer_queue_init(&p->audioq);

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

    SDL_PauseAudio(0);

    return player;
}

static void buffer_queue_abort(BufferQueue *q) {
    SDL_LockMutex(q->mutex);

    q->abort_request = 1;

    SDL_CondSignal(q->cond);
    SDL_UnlockMutex(q->mutex);
}

static void buffer_queue_end(BufferQueue *q)
{
    buffer_queue_flush(q);
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->cond);
}

void groove_destroy_player(GroovePlayer *player) {
    groove_player_clear(player);

    GroovePlayerPrivate * p = player->internals;

    buffer_queue_abort(&p->audioq);
    SDL_CloseAudio();
    buffer_queue_end(&p->audioq);

    p->abort_request = 1;
    SDL_WaitThread(p->thread_id, NULL);

    p->audio_buf = NULL;

    avcodec_free_frame(&p->frame);
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
        av_log(NULL, AV_LOG_ERROR, "error opening %s\n", f->ic->filename);
        return NULL;
    }

    err = avformat_find_stream_info(f->ic, NULL);
    if (err < 0) {
        groove_close(file);
        av_log(NULL, AV_LOG_ERROR, "%s: could not find codec parameters\n", f->ic->filename);
        return NULL;
    }

    f->seek_by_bytes = !!(f->ic->iformat->flags & AVFMT_TS_DISCONT);
    return file;
}

// should be safe to call no matter what state the file is in
void groove_close(GrooveFile * file) {
    if (!file)
        return;

    GrooveFilePrivate * f = file->internals;

    if (f) {
        if (f->audio_stream_index >= 0) {
            AVCodecContext *avctx = f->ic->streams[f->audio_stream_index]->codec;

            av_free_packet(&f->audio_pkt);
            if (f->avr)
                avresample_free(&f->avr);

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

void groove_player_play(GroovePlayer *player) {
    GrooveQueueItem * item = player->queue_head;
    if (!item) {
        // cannot play. empty queue
        return;
    }
    GroovePlayerPrivate * p = player->internals;
    if (player->state == GROOVE_STATE_STOPPED || player->state == GROOVE_STATE_PAUSED) {
        p->paused = 0;
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
    GroovePlayerPrivate * p = player->internals;
    if (player->state == GROOVE_STATE_PAUSED || player->state == GROOVE_STATE_PLAYING) {
        groove_player_seek_abs(player, 0);
        p->paused = 1;
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
    GroovePlayerPrivate * p = player->internals;
    if (player->state == GROOVE_STATE_STOPPED || player->state == GROOVE_STATE_PAUSED) {
        // nothing to do
    } else if (player->state == GROOVE_STATE_PLAYING) {
        p->paused = 1;
        player->state = GROOVE_STATE_PAUSED;
    } else {
        fprintf(stderr, "groove_player_stop: player has invalid state: %i\n", player->state);
        exit(1);
    }
}

void groove_player_next(GroovePlayer *player) {
    GroovePlayerPrivate * p = player->internals;
    buffer_queue_flush(&p->audioq);
    next_without_flush(player);
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

char * groove_file_filename(GrooveFile *file) {
    GrooveFilePrivate * f = file->internals;
    return f->ic->filename;
}

const char * groove_file_short_names(GrooveFile *file) {
    GrooveFilePrivate * f = file->internals;
    return f->ic->iformat->name;
}
