#include "groove.h"
#include "decode.h"
#include "queue.h"

#include <libavutil/channel_layout.h>

#include <SDL/SDL.h>
#include <SDL/SDL_thread.h>

// SDL audio buffer size, in samples. Should be small because there is no way
// to clear the buffer.
#define SDL_AUDIO_BUFFER_SIZE 1024

// If there is at least this many milliseconds of buffered audio in the queue,
// the decode thread will sleep for QUEUE_FULL_DELAY rather than decoding more.
#define AUDIOQ_BUF_SIZE 200
#define QUEUE_FULL_DELAY 10
// How many ms to wait to check whether anything is added to the playlist yet.
#define EMPTY_PLAYLIST_DELAY 1

typedef struct TaggedFrame {
    AVFrame *frame;
    GroovePlaylistItem *item;
    double pos;
} TaggedFrame;

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
    int min_audioq_size;
    GroovePlaylistItem *purge_item; // set temporarily
    // this is used to tell sdl_audio_callback the difference between a buffer underrun
    // and the end of the playlist.
    TaggedFrame end_of_q_sentinel;
    // only touched by sdl_audio_callback, tells whether we have reached end
    // of audio queue naturally rather than a buffer underrun
    int end_of_q;
    // only touched by decode_thread, tells whether we have sent the end_of_q_sentinel
    int sent_end_of_q;

    GrooveQueue *eventq;

    // this mutex applies to the variables in this block
    SDL_mutex *decode_head_mutex;
    // pointer to current playlist item being decoded
    GroovePlaylistItem *decode_head;

    // this mutex applies to the variables in this block
    SDL_mutex *play_head_mutex;
    // pointer to current item where the buffered audio is reaching the device
    GroovePlaylistItem *play_head;
    // number of seconds into the play_head song where the buffered audio
    // is reaching the device
    double play_pos;
} GroovePlayerPrivate;

static void audioq_put(GrooveQueue *queue, void *obj) {
    TaggedFrame *tf = obj;
    GroovePlayer *player = queue->context;
    GroovePlayerPrivate *p = player->internals;
    if (tf == &p->end_of_q_sentinel)
        return;
    p->audioq_buf_count += 1;
    p->audioq_size += tf->frame->linesize[0];
}

static void audioq_get(GrooveQueue *queue, void *obj) {
    TaggedFrame *tf = obj;
    GroovePlayer *player = queue->context;
    GroovePlayerPrivate *p = player->internals;
    if (tf == &p->end_of_q_sentinel)
        return;
    p->audioq_buf_count -= 1;
    p->audioq_size -= tf->frame->linesize[0];
}

static void audioq_cleanup(GrooveQueue *queue, void *obj) {
    TaggedFrame *tf = obj;
    GroovePlayer *player = queue->context;
    GroovePlayerPrivate *p = player->internals;
    if (tf == &p->end_of_q_sentinel)
        return;
    av_frame_free(&tf->frame);
    av_free(tf);
}

static void audioq_flush(GrooveQueue *queue) {
    GroovePlayer *player = queue->context;
    GroovePlayerPrivate *p = player->internals;
    p->audioq_buf_count = 0;
    p->audioq_size = 0;
}

static int audioq_purge(GrooveQueue *queue, void *obj) {
    GroovePlayer *player = queue->context;
    GroovePlayerPrivate *p = player->internals;
    TaggedFrame *tf = obj;
    return tf->item == p->purge_item;
}

static void emit_event(GrooveQueue *queue, enum GroovePlayerEventType type) {
    // put an event on the queue
    GroovePlayerEvent *evt = av_malloc(sizeof(GroovePlayerEvent));
    if (!evt) {
        av_log(NULL, AV_LOG_WARNING, "unable to create event: out of memory\n");
        return;
    }
    evt->type = type;
    if (groove_queue_put(queue, evt) < 0)
        av_log(NULL, AV_LOG_WARNING, "unable to put event on queue: out of memory\n");
}

// prepare a new audio buffer
static void sdl_audio_callback(void *opaque, Uint8 *stream, int len) {
    GroovePlayer *player = opaque;
    GroovePlayerPrivate *p = player->internals;
    double bytes_per_sec = p->decode_ctx.dest_bytes_per_sec;

    TaggedFrame *tf;

    SDL_LockMutex(p->play_head_mutex);

    while (len > 0) {
        int paused = p->decode_ctx.paused;
        if (!paused && p->audio_buf_index >= p->audio_buf_size) {
            av_frame_free(&p->audio_buf);
            p->audio_buf_index = 0;
            p->audio_buf_size = 0;

            if (groove_queue_get(p->audioq, (void**)&tf, 0) > 0) {
                if (tf->item != p->play_head)
                    emit_event(p->eventq, GROOVE_PLAYER_EVENT_NOWPLAYING);
                if (tf == &p->end_of_q_sentinel) {
                    p->end_of_q = 1;
                    p->play_head = NULL;
                    p->play_pos = -1.0;
                } else {
                    p->end_of_q = 0;
                    p->play_head = tf->item;
                    p->audio_buf = tf->frame;
                    p->play_pos = tf->pos;
                    p->audio_buf_size = p->audio_buf->linesize[0];
                }
            } else if (!p->end_of_q) {
                emit_event(p->eventq, GROOVE_PLAYER_EVENT_BUFFERUNDERRUN);
            }
        }
        if (paused || !p->audio_buf) {
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
        p->play_pos += len1 / bytes_per_sec;
    }
    SDL_UnlockMutex(p->play_head_mutex);
}

static void player_flush(GroovePlayer *player) {
    GroovePlayerPrivate *p = player->internals;
    groove_queue_flush(p->audioq);

    // also flush p->audio_buf
    SDL_LockMutex(p->play_head_mutex);
    av_frame_free(&p->audio_buf);
    p->audio_buf_index = 0;
    p->audio_buf_size = 0;
    SDL_UnlockMutex(p->play_head_mutex);
}

static void player_flush_cb(GrooveDecodeContext *decode_ctx) {
    GroovePlayer *player = decode_ctx->callback_context;
    player_flush(player);
}

static int player_buffer(GrooveDecodeContext *decode_ctx, AVFrame *frame) {
    GroovePlayer *player = decode_ctx->callback_context;
    GroovePlayerPrivate *p = player->internals;
    TaggedFrame *tf = av_malloc(sizeof(TaggedFrame));
    if (!tf) {
        av_log(NULL, AV_LOG_ERROR, "error allocating tagged frame: out of memory\n");
        return -1;
    }
    tf->frame = frame;
    tf->item = p->decode_head;
    GrooveFile *file = p->decode_head->file;
    GrooveFilePrivate *f = file->internals;
    tf->pos = f->audio_clock;
    if (groove_queue_put(p->audioq, tf) < 0) {
        av_free(tf);
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
        SDL_LockMutex(p->decode_head_mutex);

        // if we don't have anything to decode, wait until we do
        if (!p->decode_head) {
            if (!p->sent_end_of_q) {
                groove_queue_put(p->audioq, &p->end_of_q_sentinel);
                p->sent_end_of_q = 1;
            }
            SDL_UnlockMutex(p->decode_head_mutex);
            SDL_Delay(EMPTY_PLAYLIST_DELAY);
            continue;
        }
        p->sent_end_of_q = 0;

        // if the queue is full, no need to read more
        if (p->audioq_size > p->min_audioq_size) {
            SDL_UnlockMutex(p->decode_head_mutex);
            SDL_Delay(QUEUE_FULL_DELAY);
            continue;
        }

        GrooveFile *file = p->decode_head->file;
        GrooveFilePrivate *f = file->internals;

        p->decode_ctx.replaygain_mode = p->decode_head->replaygain_mode;

        SDL_LockMutex(f->seek_mutex);
        if (groove_decode(&p->decode_ctx, file) < 0)
            p->decode_head = p->decode_head->next;
        SDL_UnlockMutex(f->seek_mutex);

        SDL_UnlockMutex(p->decode_head_mutex);
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
        av_free(p);
        av_free(player);
        av_log(NULL, AV_LOG_ERROR, "Could not create player - out of memory\n");
        return NULL;
    }

    player->internals = p;

    p->eventq = groove_queue_create();
    p->audioq = groove_queue_create();
    p->play_head_mutex = SDL_CreateMutex();
    p->decode_head_mutex = SDL_CreateMutex();
    if (!p->audioq || !p->eventq || !p->decode_head_mutex || !p->play_head_mutex) {
        groove_destroy_player(player);
        av_log(NULL, AV_LOG_WARNING, "unable to create player: out of memory\n");
        return NULL;
    }
    p->audioq->context = player;
    p->audioq->cleanup = audioq_cleanup;
    p->audioq->put = audioq_put;
    p->audioq->get = audioq_get;
    p->audioq->flush = audioq_flush;
    p->audioq->purge = audioq_purge;

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
    p->decode_ctx.flush = player_flush_cb;
    p->decode_ctx.buffer = player_buffer;

    p->decode_ctx.dest_sample_rate = spec.freq;
    p->decode_ctx.dest_channel_layout = spec.channels == 2 ?
        AV_CH_LAYOUT_STEREO : AV_CH_LAYOUT_MONO;

    if (groove_init_decode_ctx(&p->decode_ctx) < 0) {
        groove_destroy_player(player);
        return NULL;
    };
    p->min_audioq_size = AUDIOQ_BUF_SIZE * p->decode_ctx.dest_bytes_per_sec / 1000;
    av_log(NULL, AV_LOG_INFO, "audio queue size: %d\n", p->min_audioq_size);


    p->audio_buf_size  = 0;
    p->audio_buf_index = 0;
    p->play_pos = -1.0;

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

    if (p->play_head_mutex)
        SDL_DestroyMutex(p->play_head_mutex);
    if (p->decode_head_mutex)
        SDL_DestroyMutex(p->decode_head_mutex);

    av_free(p);
    av_free(player);
}

void groove_player_play(GroovePlayer *player) {
    GroovePlayerPrivate * p = player->internals;
    // no mutex needed for this boolean flag
    p->decode_ctx.paused = 0;
}

void groove_player_pause(GroovePlayer *player) {
    GroovePlayerPrivate * p = player->internals;
    // no mutex needed for this boolean flag
    p->decode_ctx.paused = 1;
}

void groove_player_seek(GroovePlayer *player, GroovePlaylistItem *item, double seconds) {
    GrooveFile * file = item->file;
    GrooveFilePrivate * f = file->internals;

    int64_t ts = seconds * f->audio_st->time_base.den / f->audio_st->time_base.num;
    if (f->ic->start_time != AV_NOPTS_VALUE)
        ts += f->ic->start_time;

    GroovePlayerPrivate *p = player->internals;

    SDL_LockMutex(p->decode_head_mutex);
    SDL_LockMutex(f->seek_mutex);

    f->seek_pos = ts;
    f->seek_flush = 1;
    p->decode_head = item;

    SDL_UnlockMutex(f->seek_mutex);
    SDL_UnlockMutex(p->decode_head_mutex);
}

GroovePlaylistItem * groove_player_insert(GroovePlayer *player, GrooveFile *file,
        GroovePlaylistItem *next)
{
    GroovePlaylistItem * item = av_mallocz(sizeof(GroovePlaylistItem));
    if (!item)
        return NULL;

    item->file = file;
    item->next = next;
    item->replaygain_mode = GROOVE_REPLAYGAINMODE_ALBUM;

    GroovePlayerPrivate *p = player->internals;

    // lock decode_head_mutex so that decode_head cannot point to a new item
    // while we're screwing around with the queue
    SDL_LockMutex(p->decode_head_mutex);

    if (next) {
        if (next->prev) {
            item->prev = next->prev;
            item->prev->next = item;
            next->prev = item;
        } else {
            player->playlist_head = item;
        }
    } else if (!player->playlist_head) {
        player->playlist_head = item;
        player->playlist_tail = item;

        p->decode_head = player->playlist_head;
    } else {
        item->prev = player->playlist_tail;
        player->playlist_tail->next = item;
        player->playlist_tail = item;
    }

    SDL_UnlockMutex(p->decode_head_mutex);
    return item;
}

void groove_player_remove(GroovePlayer *player, GroovePlaylistItem *item) {
    GroovePlayerPrivate *p = player->internals;
    GrooveFile *file = item->file;
    GrooveFilePrivate *f = file->internals;

    SDL_LockMutex(p->decode_head_mutex);
    SDL_LockMutex(p->play_head_mutex);

    // if it's currently being played, seek to the next item
    if (item == p->decode_head) {
        f->seek_pos = 0;
        f->seek_flush = 0;
        p->decode_head = item->next ? item->next : item->prev;
    }

    if (item->prev) {
        item->prev->next = item->next;
    } else {
        player->playlist_head = item->next;
    }
    if (item->next) {
        item->next->prev = item->prev;
    } else {
        player->playlist_tail = item->prev;
    }

    // we must be absolutely sure to purge the audio buffer queue
    // of references to item before freeing it at the bottom of this method
    p->purge_item = item;
    groove_queue_purge(p->audioq);
    p->purge_item = NULL;

    if (p->play_head == item) {
        p->play_head = NULL;
        p->play_pos = -1.0;
        av_frame_free(&p->audio_buf);
        p->audio_buf_index = 0;
        p->audio_buf_size = 0;
    }

    SDL_UnlockMutex(p->play_head_mutex);
    SDL_UnlockMutex(p->decode_head_mutex);

    av_free(item);
}

void groove_player_clear(GroovePlayer *player) {
    GroovePlaylistItem * node = player->playlist_head;
    if (!node) return;
    while (node) {
        groove_player_remove(player, node);
        node = node->next;
    }
}

int groove_player_count(GroovePlayer *player) {
    GroovePlaylistItem * node = player->playlist_head;
    int count = 0;
    while (node) {
        count += 1;
        node = node->next;
    }
    return count;
}

void groove_player_set_replaygain_mode(GroovePlayer *player, GroovePlaylistItem *item,
        enum GrooveReplayGainMode mode)
{
    GroovePlayerPrivate *p = player->internals;

    SDL_LockMutex(p->decode_head_mutex);
    item->replaygain_mode = mode;
    SDL_UnlockMutex(p->decode_head_mutex);
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

void groove_player_position(GroovePlayer *player, GroovePlaylistItem **item, double *seconds) {
    GroovePlayerPrivate *p = player->internals;

    SDL_LockMutex(p->play_head_mutex);
    if (item)
        *item = p->play_head;
    if (seconds)
        *seconds = p->play_pos;
    SDL_UnlockMutex(p->play_head_mutex);
}

void groove_player_set_replaygain_preamp(GroovePlayer *player, double preamp) {
    GroovePlayerPrivate *p = player->internals;

    SDL_LockMutex(p->decode_head_mutex);
    p->decode_ctx.replaygain_preamp = preamp;
    SDL_UnlockMutex(p->decode_head_mutex);
}

double groove_player_get_replaygain_preamp(GroovePlayer *player) {
    GroovePlayerPrivate *p = player->internals;
    return p->decode_ctx.replaygain_preamp;
}

void groove_player_set_replaygain_default(GroovePlayer *player, double value) {
    GroovePlayerPrivate *p = player->internals;

    SDL_LockMutex(p->decode_head_mutex);
    p->decode_ctx.replaygain_default = value;
    SDL_UnlockMutex(p->decode_head_mutex);
}
double groove_player_get_replaygain_default(GroovePlayer *player) {
    GroovePlayerPrivate *p = player->internals;
    return p->decode_ctx.replaygain_default;
}

double groove_player_get_volume(GroovePlayer *player) {
    GroovePlayerPrivate *p = player->internals;
    return p->decode_ctx.volume;
}

void groove_player_set_volume(GroovePlayer *player, double volume) {
    GroovePlayerPrivate *p = player->internals;

    SDL_LockMutex(p->decode_head_mutex);
    p->decode_ctx.volume = volume;
    SDL_UnlockMutex(p->decode_head_mutex);
}

int groove_player_playing(GroovePlayer *player) {
    GroovePlayerPrivate *p = player->internals;
    return !p->decode_ctx.paused;
}
