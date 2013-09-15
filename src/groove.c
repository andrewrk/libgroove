#include "groove.h"
#include "decode.h"

#include <stdio.h>
#include <inttypes.h>

#include <libavutil/avstring.h>
#include <libavutil/dict.h>
#include <libavutil/samplefmt.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavformat/avformat.h>

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

typedef struct BufferQueue {
    BufferList *first_buf;
    BufferList *last_buf;
    int nb_buffers;
    int size;
    int abort_request;
    SDL_mutex *mutex;
    SDL_cond *cond;
} BufferQueue;

typedef struct GroovePlayerPrivate {
    SDL_Thread *thread_id;
    int abort_request;
    BufferQueue audioq;
    SDL_AudioSpec spec;
    AVFrame *audio_buf;
    size_t audio_buf_size; // in bytes
    size_t audio_buf_index; // in bytes
    DecodeContext decode_ctx;
} GroovePlayerPrivate;

static int buffer_queue_put(BufferQueue *q, AVFrame *frame) {
    BufferList * buf1 = av_malloc(sizeof(BufferList));

    if (!buf1)
        return -1;

    buf1->frame = frame;
    buf1->next = NULL;

    SDL_LockMutex(q->mutex);

    if (!q->last_buf)
        q->first_buf = buf1;
    else
        q->last_buf->next = buf1;
    q->last_buf = buf1;

    q->nb_buffers += 1;
    q->size += buf1->frame->linesize[0];

    SDL_CondSignal(q->cond);
    SDL_UnlockMutex(q->mutex);

    return 0;
}

// return < 0 if aborted, 0 if no buffer and > 0 if buffer.
static int buffer_queue_get(BufferQueue *q, AVFrame **frame, int block) {
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
            q->size -= buf1->frame->linesize[0];
            *frame = buf1->frame;
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
    DecodeContext *decode_ctx = &p->decode_ctx;

    while (len > 0) {
        if (!decode_ctx->paused && p->audio_buf_index >= p->audio_buf_size) {
            av_frame_free(&p->audio_buf);
            p->audio_buf_index = 0;
            p->audio_buf_size = 0;
            if (buffer_queue_get(&p->audioq, &p->audio_buf, 1) > 0)
                p->audio_buf_size = p->audio_buf->linesize[0];
        }
        if (decode_ctx->paused || !p->audio_buf) {
            // fill with silence
            memset(stream, 0, len);
            break;
        }
        size_t len1 = p->audio_buf_size - p->audio_buf_index;
        if (len1 > len)
            len1 = len;
        memcpy(stream, p->audio_buf->data[0] + p->audio_buf_index, len1);
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
        av_frame_free(&buf->frame);
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

static void player_flush(DecodeContext *decode_ctx) {
    GroovePlayer *player = decode_ctx->callback_context;
    GroovePlayerPrivate *p = player->internals;
    buffer_queue_flush(&p->audioq);
}

static int player_buffer(DecodeContext *decode_ctx, AVFrame *frame) {
    GroovePlayer *player = decode_ctx->callback_context;
    GroovePlayerPrivate *p = player->internals;
    if (buffer_queue_put(&p->audioq, frame) < 0) {
        av_log(NULL, AV_LOG_ERROR, "error allocating buffer queue item: out of memory\n");
        return -1;
    }
    return 0;
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
        if (decode(&p->decode_ctx, file) < 0)
            next_without_flush(player);
    }

    return 0;
}

static enum AVSampleFormat sdl_format_to_av_format(Uint16 sdl_format) {
    switch (sdl_format) {
        case AUDIO_U8:
            return AV_SAMPLE_FMT_U8;
        case AUDIO_S16SYS:
            return AV_SAMPLE_FMT_S16;
        default:
            return AV_SAMPLE_FMT_NONE;
    }
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

    p->decode_ctx.callback_context = player;
    p->decode_ctx.flush = player_flush;
    p->decode_ctx.buffer = player_buffer;

    p->decode_ctx.frame = avcodec_alloc_frame();
    if (!p->decode_ctx.frame) {
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
        groove_destroy_player(player);
        av_log(NULL, AV_LOG_ERROR, "unable to open audio device: %s\n", SDL_GetError());
        return NULL;
    }
    p->decode_ctx.dest_sample_fmt = sdl_format_to_av_format(p->spec.format);
    if (p->decode_ctx.dest_sample_fmt == AV_SAMPLE_FMT_NONE) {
        groove_destroy_player(player);
        av_log(NULL, AV_LOG_ERROR, "unsupported audio device format\n");
        return NULL;
    }

    p->decode_ctx.dest_sample_rate = p->spec.freq;
    p->decode_ctx.dest_channel_layout = p->spec.channels == 2 ?
        AV_CH_LAYOUT_STEREO : AV_CH_LAYOUT_MONO;
    p->decode_ctx.dest_channel_count = p->spec.channels;

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

    av_frame_free(&p->audio_buf);

    cleanup_decode_ctx(&p->decode_ctx);
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

    // prepare audio output
    if (!avctx->channel_layout)
        avctx->channel_layout = av_get_default_channel_layout(avctx->channels);
    if (!avctx->channel_layout) {
        groove_close(file);
        av_log(NULL, AV_LOG_ERROR, "unable to guess channel layout\n");
        return NULL;
    }

    memset(&f->audio_pkt, 0, sizeof(f->audio_pkt));
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

void groove_player_play(GroovePlayer *player) {
    GrooveQueueItem * item = player->queue_head;
    if (!item) {
        // cannot play. empty queue
        return;
    }
    GroovePlayerPrivate * p = player->internals;
    if (player->state == GROOVE_STATE_STOPPED || player->state == GROOVE_STATE_PAUSED) {
        p->decode_ctx.paused = 0;
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
    DecodeContext *decode_ctx = &p->decode_ctx;

    double pts = f->audio_clock;
    int hw_buf_size = p->audio_buf_size - p->audio_buf_index;
    int bytes_per_sec = 0;
    if (f->audio_st) {
        bytes_per_sec = decode_ctx->dest_sample_rate * decode_ctx->dest_channel_count *
                        av_get_bytes_per_sample(decode_ctx->dest_sample_fmt);
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
        p->decode_ctx.paused = 1;
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
        p->decode_ctx.paused = 1;
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

    // TODO adjust the volume property of the filter
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
    return av_dict_get(f->ic->metadata, key, e, flags&AV_DICT_IGNORE_SUFFIX);
}

int groove_file_metadata_set(GrooveFile *file, const char *key,
        const char *value, int flags)
{
    file->dirty = 1;
    GrooveFilePrivate *f = file->internals;
    return av_dict_set(&f->ic->metadata, key, value, flags&AV_DICT_IGNORE_SUFFIX);
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
