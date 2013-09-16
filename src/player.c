#include "groove.h"
#include "decode.h"
#include "queue.h"

#include <libavutil/channel_layout.h>

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

typedef struct GroovePlayerPrivate {
    SDL_Thread *thread_id;
    int abort_request;

    GrooveDecodeContext decode_ctx;

    AVFrame *audio_buf;
    size_t audio_buf_size; // in bytes
    size_t audio_buf_index; // in bytes

    GrooveQueue *audioq;
    int audioq_buf_count;
    int audioq_size;

    GrooveQueue *eventq;
} GroovePlayerPrivate;

static void audioq_put(GrooveQueue *queue, void *obj) {
    AVFrame *frame = obj;
    GroovePlayer *player = queue->context;
    GroovePlayerPrivate *p = player->internals;
    p->audioq_buf_count += 1;
    p->audioq_size += frame->linesize[0];
}

static void audioq_get(GrooveQueue *queue, void *obj) {
    AVFrame *frame = obj;
    GroovePlayer *player = queue->context;
    GroovePlayerPrivate *p = player->internals;
    p->audioq_buf_count -= 1;
    p->audioq_size -= frame->linesize[0];
}

static void audioq_cleanup(void *obj) {
    AVFrame *frame = obj;
    av_frame_free(&frame);
}

static void audioq_flush(GrooveQueue *queue) {
    GroovePlayer *player = queue->context;
    GroovePlayerPrivate *p = player->internals;
    p->audioq_buf_count = 0;
    p->audioq_size = 0;
}

// prepare a new audio buffer
static void sdl_audio_callback(void *opaque, Uint8 *stream, int len) {
    GroovePlayer *player = opaque;
    GroovePlayerPrivate *p = player->internals;
    GrooveDecodeContext *decode_ctx = &p->decode_ctx;

    while (len > 0) {
        if (!decode_ctx->paused && p->audio_buf_index >= p->audio_buf_size) {
            av_frame_free(&p->audio_buf);
            p->audio_buf_index = 0;
            p->audio_buf_size = 0;

            if (groove_queue_get(p->audioq, (void**)&p->audio_buf, 1) > 0)
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

static void emit_nowplaying(GroovePlayer *player) {
    // put an event on the queue
    GroovePlayerEvent *evt = av_malloc(sizeof(GroovePlayerEvent));
    if (!evt) {
        av_log(NULL, AV_LOG_WARNING, "unable to create event: out of memory\n");
        return;
    }
    evt->type = GROOVE_PLAYER_EVENT_NOWPLAYING;
    GroovePlayerPrivate *p = player->internals;
    if (groove_queue_put(p->eventq, evt) < 0)
        av_log(NULL, AV_LOG_WARNING, "unable to put event on queue: out of memory\n");
}

static void next_without_flush(GroovePlayer *player) {
    groove_close(remove_queue_item(player, player->queue_head));
    emit_nowplaying(player);
}

static void player_flush(GrooveDecodeContext *decode_ctx) {
    GroovePlayer *player = decode_ctx->callback_context;
    GroovePlayerPrivate *p = player->internals;
    groove_queue_flush(p->audioq);
    // TODO: also flush p->audio_buf
    // note this would require careful use of mutexes
}

static int player_buffer(GrooveDecodeContext *decode_ctx, AVFrame *frame) {
    GroovePlayer *player = decode_ctx->callback_context;
    GroovePlayerPrivate *p = player->internals;
    if (groove_queue_put(p->audioq, frame) < 0) {
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
        if (p->audioq_size > MIN_AUDIOQ_SIZE) {
            SDL_Delay(QUEUE_FULL_DELAY);
            continue;
        }
        GrooveFile * file = player->queue_head->file;
        if (groove_decode(&p->decode_ctx, file) < 0)
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
    GroovePlayer * player = av_mallocz(sizeof(GroovePlayer));
    GroovePlayerPrivate * p = av_mallocz(sizeof(GroovePlayerPrivate));
    if (!player || !p) {
        av_free(player);
        av_free(p);
        av_log(NULL, AV_LOG_ERROR, "Could not create player - out of memory\n");
        return NULL;
    }

    player->internals = p;

    p->eventq = groove_queue_create();
    if (!p->eventq) {
        av_free(player);
        av_free(p);
        av_log(NULL, AV_LOG_WARNING, "unable to create event queue: out of memory\n");
        return NULL;
    }
    p->audioq = groove_queue_create();
    if (!p->audioq) {
        groove_queue_destroy(p->eventq);
        av_free(player);
        av_free(p);
        av_log(NULL, AV_LOG_WARNING, "unable to create audio queue: out of memory\n");
        return NULL;
    }
    p->audioq->context = player;
    p->audioq->cleanup = audioq_cleanup;
    p->audioq->put = audioq_put;
    p->audioq->get = audioq_get;
    p->audioq->flush = audioq_flush;

    SDL_AudioSpec wanted_spec, spec;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.freq = 44100;
    wanted_spec.channels = 2;
    wanted_spec.silence = 0;
    wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = player;
    if (SDL_OpenAudio(&wanted_spec, &spec) < 0) {
        groove_destroy_player(player);
        av_log(NULL, AV_LOG_ERROR, "unable to open audio device: %s\n", SDL_GetError());
        return NULL;
    }
    p->decode_ctx.dest_sample_fmt = sdl_format_to_av_format(spec.format);
    if (p->decode_ctx.dest_sample_fmt == AV_SAMPLE_FMT_NONE) {
        groove_destroy_player(player);
        av_log(NULL, AV_LOG_ERROR, "unsupported audio device format\n");
        return NULL;
    }

    p->decode_ctx.callback_context = player;
    p->decode_ctx.flush = player_flush;
    p->decode_ctx.buffer = player_buffer;

    p->decode_ctx.dest_sample_rate = spec.freq;
    p->decode_ctx.dest_channel_layout = spec.channels == 2 ?
        AV_CH_LAYOUT_STEREO : AV_CH_LAYOUT_MONO;

    if (groove_init_decode_ctx(&p->decode_ctx) < 0) {
        groove_destroy_player(player);
        return NULL;
    };

    p->audio_buf_size  = 0;
    p->audio_buf_index = 0;

    p->thread_id = SDL_CreateThread(decode_thread, player);

    if (!p->thread_id) {
        groove_destroy_player(player);
        av_log(NULL, AV_LOG_ERROR, "Error creating player thread: Out of memory\n");
        return NULL;
    }



    SDL_PauseAudio(0);

    return player;
}

void groove_destroy_player(GroovePlayer *player) {
    groove_player_clear(player);

    GroovePlayerPrivate * p = player->internals;

    // wait for decode thread to finish
    p->abort_request = 1;
    if (p->eventq)
        groove_queue_abort(p->eventq);
    SDL_WaitThread(p->thread_id, NULL);
    if (p->eventq)
        groove_queue_destroy(p->eventq);

    // flush audio queue
    if (p->audioq)
        groove_queue_abort(p->audioq);
    SDL_CloseAudio();
    if (p->audioq)
        groove_queue_destroy(p->audioq);

    av_frame_free(&p->audio_buf);
    groove_cleanup_decode_ctx(&p->decode_ctx);

    av_free(p);
    av_free(player);
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
    GrooveDecodeContext *decode_ctx = &p->decode_ctx;

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
    player_flush(&p->decode_ctx);
    next_without_flush(player);
}

GrooveQueueItem * groove_player_queue(GroovePlayer *player, GrooveFile *file) {
    GrooveQueueItem * item = av_mallocz(sizeof(GrooveQueueItem));
    item->file = file;
    if (!player->queue_head) {
        player->queue_head = item;
        player->queue_tail = item;
        emit_nowplaying(player);
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
    int removing_head = item == player->queue_head;
    if (removing_head && player->state != GROOVE_STATE_STOPPED) {
        groove_player_stop(player);
        resume_playback = player->state == GROOVE_STATE_PLAYING;
    }

    GrooveFile * file = remove_queue_item(player, item);

    if (resume_playback)
        groove_player_play(player);
    if (removing_head)
        emit_nowplaying(player);

    return file;
}

void groove_player_clear(GroovePlayer *player) {
    groove_player_stop(player);
    GrooveQueueItem * node = player->queue_head;
    if (!node) return;
    while (node) {
        GrooveFile * file = groove_player_remove(player, node);
        groove_close(file);
        node = node->next;
    }
    emit_nowplaying(player);
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

void groove_player_set_replaygain_mode(GroovePlayer *player, GrooveQueueItem *item,
        enum GrooveReplayGainMode mode)
{

    // TODO adjust the volume property of the filter
}

static int get_event(GroovePlayer *player, GroovePlayerEvent *event, int block) {
    GroovePlayerPrivate *p = player->internals;
    GroovePlayerEvent *tmp;
    int err = groove_queue_get(p->eventq, (void **)&tmp, block);
    if (err < 0)
        return err;
    *event = *tmp;
    av_free(tmp);
    return 0;
}

int groove_player_event_poll(GroovePlayer *player, GroovePlayerEvent *event) {
    return get_event(player, event, 0);
}

int groove_player_event_wait(GroovePlayer *player, GroovePlayerEvent *event) {
    return get_event(player, event, 1);
}
