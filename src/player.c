#include "groove.h"
#include "queue.h"

#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>

#include <SDL2/SDL_thread.h>

// If there is at least this many milliseconds of buffered audio in the queue,
// the decode thread will sleep for QUEUE_FULL_DELAY rather than decoding more.
#define AUDIOQ_BUF_SIZE 200
#define QUEUE_FULL_DELAY 10
// How many ms to wait to check whether anything is added to the playlist yet.
#define EMPTY_PLAYLIST_DELAY 1

typedef struct GrooveSinkPrivate {
    GrooveQueue *audioq;
    int audioq_buf_count;
    int audioq_size;
    int min_audioq_size;
    GroovePlaylistItem *purge_item; // set temporarily
} GrooveSinkPrivate;

typedef struct SinkStack {
    GrooveSink *sink;
    struct SinkStack *next;
} SinkStack;

typedef struct SinkMap {
    SinkStack *stack_head;
    AVFilterContext *aformat_ctx;
    AVFilterContext *abuffersink_ctx;
    struct SinkMap *next;
} SinkMap;

typedef struct GroovePlayerPrivate {
    SDL_Thread *thread_id;
    int abort_request;

    AVPacket audio_pkt_temp;
    AVFrame *frame;
    int paused;
    int last_paused;

    int in_sample_rate;
    uint64_t in_channel_layout;
    enum AVSampleFormat in_sample_fmt;
    AVRational in_time_base;

    // map audio format to list of sinks
    // for each map entry, use the first sink in the stack as the example
    // of the audio format in that stack
    SinkMap *sink_map;
    int sink_map_count;

    char strbuf[512];
    AVFilterGraph *filter_graph;
    AVFilterContext *abuffer_ctx;
    AVFilterContext *volume_ctx;
    AVFilterContext *asplit_ctx;

    // this mutex applies to the variables in this block
    SDL_mutex *decode_head_mutex;
    // pointer to current playlist item being decoded
    GroovePlaylistItem *decode_head;
    // desired volume for the volume filter
    double volume;
    // set to 1 to trigger a rebuild
    int rebuild_filter_graph_flag;

    // the value for volume that was used to construct the filter graph
    double filter_volume;

    // only touched by decode_thread, tells whether we have sent the end_of_q_sentinel
    int sent_end_of_q;
} GroovePlayerPrivate;

// this is used to tell the difference between a buffer underrun
// and the end of the playlist.
// TODO see if we can use NULL for the end_of_q_sentinel
static GrooveBuffer end_of_q_sentinel;

static int frame_size(const AVFrame *frame) {
    return av_get_channel_layout_nb_channels(frame->channel_layout) *
        av_get_bytes_per_sample(frame->format) *
        frame->nb_samples;
}

// decode one audio packet and return its uncompressed size
static int audio_decode_frame(GrooveDecodeContext *decode_ctx, GrooveFile *file) {
    GrooveFilePrivate * f = file->internals;

    AVPacket *pkt = &f->audio_pkt;
    AVCodecContext *dec = f->audio_st->codec;

    AVPacket *pkt_temp = &decode_ctx->audio_pkt_temp;
    *pkt_temp = *pkt;

    // update the audio clock with the pts if we can
    if (pkt->pts != AV_NOPTS_VALUE)
        f->audio_clock = av_q2d(f->audio_st->time_base)*pkt->pts;

    int data_size = 0;
    int len1, got_frame;
    int new_packet = 1;
    AVFrame *frame = decode_ctx->frame;

    // NOTE: the audio packet can contain several frames
    while (pkt_temp->size > 0 || (!pkt_temp->data && new_packet)) {
        avcodec_get_frame_defaults(frame);
        new_packet = 0;

        len1 = avcodec_decode_audio4(dec, frame, &got_frame, pkt_temp);
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

        // push the audio data from decoded frame into the filtergraph
        int err = av_buffersrc_write_frame(decode_ctx->abuffer_ctx, frame);
        if (err < 0) {
            av_strerror(err, decode_ctx->strbuf, sizeof(decode_ctx->strbuf));
            av_log(NULL, AV_LOG_ERROR, "error writing frame to buffersrc: %s\n",
                    decode_ctx->strbuf);
            return -1;
        }

        // pull filtered audio from the filtergraph
        AVFrame *oframe = av_frame_alloc();
        for (;;) {
            int err = av_buffersink_get_frame(decode_ctx->abuffersink_ctx, oframe);
            if (err == AVERROR_EOF || err == AVERROR(EAGAIN))
                break;
            if (err < 0) {
                av_log(NULL, AV_LOG_ERROR, "error reading buffer from buffersink\n");
                return -1;
            }
            data_size += frame_size(oframe);
            err = decode_ctx->buffer(decode_ctx, oframe);
            if (err < 0)
                return err;
        }

        // if no pts, then estimate it
        if (pkt->pts == AV_NOPTS_VALUE)
            f->audio_clock += data_size / (double)decode_ctx->dest_bytes_per_sec;

        return data_size;
    }
    return data_size;
}

// abuffer -> volume -> asplit for each audio format
//                     -> aformat -> abuffersink
static int init_filter_graph(GroovePlayer *player, GrooveFile *file) {
    GroovePlayerPrivate *p = player->internals;
    GrooveFilePrivate *f = file->internals;

    // destruct old graph
    avfilter_graph_free(&p->filter_graph);

    // create new graph
    p->filter_graph = avfilter_graph_alloc();
    if (!p->filter_graph) {
        av_log(NULL, AV_LOG_ERROR, "unable to create filter graph: out of memory\n");
        return -1;
    }

    AVFilter *abuffer = avfilter_get_by_name("abuffer");
    AVFilter *volume = avfilter_get_by_name("volume");
    AVFilter *asplit = avfilter_get_by_name("asplit");
    AVFilter *aformat = avfilter_get_by_name("aformat");
    AVFilter *abuffersink = avfilter_get_by_name("abuffersink");

    int err;
    // create abuffer filter
    AVCodecContext *avctx = f->audio_st->codec;
    AVRational time_base = f->audio_st->time_base;
    snprintf(p->strbuf, sizeof(p->strbuf),
            "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%"PRIx64, 
            time_base.num, time_base.den, avctx->sample_rate,
            av_get_sample_fmt_name(avctx->sample_fmt),
            avctx->channel_layout);
    av_log(NULL, AV_LOG_INFO, "abuffer: %s\n", p->strbuf);
    // save these values so we can compare later and check
    // whether we have to reconstruct the graph
    p->in_sample_rate = avctx->sample_rate;
    p->in_channel_layout = avctx->channel_layout;
    p->in_sample_fmt = avctx->sample_fmt;
    p->in_time_base = time_base;
    err = avfilter_graph_create_filter(&p->abuffer_ctx, abuffer,
            NULL, p->strbuf, NULL, p->filter_graph);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "error initializing abuffer filter\n");
        return err;
    }
    // as we create filters, this points the next source to link to
    AVFilterContext *audio_src_ctx = p->abuffer_ctx;

    // save the volume value so we can compare later and check
    // whether we have to reconstruct the graph
    p->filter_volume = p->volume;
    // if volume is not equal to 1.0, create volume filter
    double vol = p->volume;
    if (vol > 1.0) vol = 1.0;
    if (vol < 0.0) vol = 0.0;
    if (vol == 1.0) {
        p->volume_ctx = NULL;
    } else {
        snprintf(p->strbuf, sizeof(p->strbuf), "volume=%f", vol);
        av_log(NULL, AV_LOG_INFO, "volume: %s\n", p->strbuf);
        err = avfilter_graph_create_filter(&p->volume_ctx, volume, NULL,
                p->strbuf, NULL, p->filter_graph);
        if (err < 0) {
            av_log(NULL, AV_LOG_ERROR, "error initializing volume filter\n");
            return err;
        }
        err = avfilter_link(audio_src_ctx, 0, p->volume_ctx, 0);
        if (err < 0) {
            av_log(NULL, AV_LOG_ERROR, "unable to link filters\n");
            return err;
        }
        audio_src_ctx = p->volume_ctx;
    }

    // if only one sink, no need for asplit
    if (p->sink_map_count < 2) {
        p->asplit_ctx = NULL;
    } else {
        snprintf(p->strbuf, sizeof(p->strbuf), "%d", p->sink_map_count);
        av_log(NULL, AV_LOG_INFO, "asplit: %s\n", p->strbuf);
        err = avfilter_graph_create_filter(&p->asplit_ctx, asplit,
                NULL, p->strbuf, NULL, p->filter_graph);
        if (err < 0) {
            av_log(NULL, AV_LOG_ERROR, "unable to create asplit filter\n");
            return err;
        }
        err = avfilter_link(audio_src_ctx, 0, p->asplit_ctx, 0);
        if (err < 0) {
            av_log(NULL, AV_LOG_ERROR, "unable to link to asplit\n");
            return err;
        }
        audio_src_ctx = p->asplit_ctx;
    }

    // for each audio format, create aformat and abuffersink filters
    SinkMap *map_item = p->sink_map;
    int pad_index = 0;
    while (map_item) {
        GrooveAudioFormat *audio_format = &map_item->stack_head->sink.audio_format;

        // create aformat filter
        snprintf(p->strbuf, sizeof(p->strbuf),
                "sample_fmts=%s:sample_rates=%d:channel_layouts=0x%"PRIx64,
                av_get_sample_fmt_name(audio_format->sample_fmt),
                audio_format->sample_rate, audio_format->channel_layout);
        av_log(NULL, AV_LOG_INFO, "aformat: %s\n", p->strbuf);
        err = avfilter_graph_create_filter(&map_item->aformat_ctx, aformat,
                NULL, p->strbuf, NULL, p->filter_graph);
        if (err < 0) {
            av_log(NULL, AV_LOG_ERROR, "unable to create aformat filter\n");
            return err;
        }
        err = avfilter_link(audio_src_ctx, pad_index, map_item->aformat_ctx, 0);
        if (err < 0) {
            av_log(NULL, AV_LOG_ERROR, "unable to link filters\n");
            return err;
        }

        // create abuffersink filter
        err = avfilter_graph_create_filter(&map_item->abuffersink_ctx, abuffersink,
                NULL, NULL, NULL, p->filter_graph);
        if (err < 0) {
            av_log(NULL, AV_LOG_ERROR, "unable to create abuffersink filter\n");
            return err;
        }
        err = avfilter_link(map_item->aformat_ctx, 0, map_item->abuffersink_ctx, 0);
        if (err < 0) {
            av_log(NULL, AV_LOG_ERROR, "unable to link filters\n");
            return err;
        }

        pad_index += 1;
        map_item = map_item->next;
    }

    err = avfilter_graph_config(p->filter_graph, NULL);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "error configuring the filter graph\n");
        return err;
    }

    p->rebuild_filter_graph_flag = 0;

    return 0;
}

static int maybe_init_filter_graph(GroovePlayer *player, GrooveFile *file) {
    GroovePlayerPrivate *p = player->internals;
    GrooveFilePrivate *f = file->internals;
    AVCodecContext *avctx = f->audio_st->codec;
    AVRational time_base = f->audio_st->time_base;

    // if the input format stuff has changed, then we need to re-build the graph
    if (!p->filter_graph || p->rebuild_filter_graph_flag ||
        p->in_sample_rate != avctx->sample_rate ||
        p->in_channel_layout != avctx->channel_layout ||
        p->in_sample_fmt != avctx->sample_fmt ||
        p->in_time_base.num != time_base.num ||
        p->in_time_base.den != time_base.den ||
        p->volume != p->filter_volume)
    {
        return init_filter_graph(player, file);
    }

    return 0;
}

static int every_sink(GroovePlayer *player, int (*func)(GrooveSink *), int default_value) {
    GroovePlayerPrivate *p = player->internals;
    SinkMap *map_item = p->sink_map;
    while (map_item) {
        SinkStack *stack_item = map_item->stack_head;
        while (stack_item) {
            GrooveSink *sink = stack_item->sink;
            int value = func(sink);
            if (value != default_value)
                return value;
            stack_item = stack_item->next;
        }
        map_item = map_item->next;
    }
    return default_value;
}

static int sink_is_full(GrooveSink *sink) {
    GrooveSinkPrivate *s = sink->internals;
    return s->audioq_size >= s->min_audioq_size;
}

static int every_sink_full(GroovePlayer *player) {
    return every_sink(player, sink_is_full, 1);
}

static int sink_signal_end(GrooveSink *sink) {
    GrooveSinkPrivate *s = sink->internals;
    groove_queue_put(s->audioq, &end_of_q_sentinel);
    return 0;
}

static void every_sink_signal_end(GroovePlayer *player) {
    every_sink(player, sink_signal_end, 0);
}

static int sink_flush(GrooveSink *sink) {
    GrooveSinkPrivate *s = sink->internals;

    groove_queue_flush(s->audioq);
    if (sink->flush)
        sink->flush(sink);

    return 0;
}

static void every_sink_flush(GroovePlayer *player) {
    every_sink(player, sink_flush, 0);


    // TODO flush each sink
    GroovePlayerPrivate *p = player->internals;
    groove_queue_flush(p->audioq);
}

static int decode_one_frame(GroovePlayer *player, GrooveFile *file) {
    GroovePlayerPrivate *p = player->internals;
    GrooveFilePrivate * f = file->internals;
    AVPacket *pkt = &f->audio_pkt;

    // might need to rebuild the filter graph if certain things changed
    if (maybe_init_filter_graph(player, file) < 0)
        return -1;

    // abort_request is set if we are destroying the file
    if (f->abort_request)
        return -1;

    // handle pause requests
    // only read p->paused once so that we don't need a mutex
    int paused = p->paused;
    if (paused != p->last_paused) {
        p->last_paused = paused;
        if (paused) {
            av_read_pause(f->ic);
        } else {
            av_read_play(f->ic);
        }
    }

    // handle seek requests
    SDL_LockMutex(f->seek_mutex);
    if (f->seek_pos >= 0) {
        if (av_seek_frame(f->ic, f->audio_stream_index, f->seek_pos, 0) < 0) {
            av_log(NULL, AV_LOG_ERROR, "%s: error while seeking\n", f->ic->filename);
        } else if (f->seek_flush) {
            every_sink_flush(player);
        }
        avcodec_flush_buffers(f->audio_st->codec);
        f->seek_pos = -1;
        f->eof = 0;
    }
    SDL_UnlockMutex(f->seek_mutex);

    if (f->eof) {
        if (f->audio_st->codec->codec->capabilities & CODEC_CAP_DELAY) {
            av_init_packet(pkt);
            pkt->data = NULL;
            pkt->size = 0;
            pkt->stream_index = f->audio_stream_index;
            if (audio_decode_frame(decode_ctx, file) > 0) {
                // keep flushing
                return 0;
            }
        }
        // this file is complete. move on
        return -1;
    }
    int err = av_read_frame(f->ic, pkt);
    if (err < 0) {
        // treat all errors as EOF, but log non-EOF errors.
        if (err != AVERROR_EOF) {
            av_log(NULL, AV_LOG_WARNING, "error reading frames\n");
        }
        f->eof = 1;
        return 0;
    }
    if (pkt->stream_index != f->audio_stream_index) {
        // we're only interested in the One True Audio Stream
        av_free_packet(pkt);
        return 0;
    }
    audio_decode_frame(decode_ctx, file);
    av_free_packet(pkt);
    return 0;
}

static void audioq_put(GrooveQueue *queue, void *obj) {
    GrooveBuffer *buffer = obj;
    GrooveSink *sink = queue->context;
    GrooveSinkPrivate *s = sink->internals;
    if (buffer == &end_of_q_sentinel)
        return;
    s->audioq_buf_count += 1;
    s->audioq_size += tb->buffer->size;
}

static void audioq_get(GrooveQueue *queue, void *obj) {
    TaggedBuffer *tb = obj;
    GrooveSink *sink = queue->context;
    GrooveSinkPrivate *s = sink->internals;
    if (tb == &end_of_q_sentinel)
        return;
    s->audioq_buf_count -= 1;
    s->audioq_size -= tb->buffer->size;
}

static void audioq_cleanup(GrooveQueue *queue, void *obj) {
    TaggedBuffer *tb = obj;
    GrooveSink *sink = queue->context;
    GrooveSinkPrivate *s = sink->internals;
    if (tb == &end_of_q_sentinel)
        return;
    s->audioq_buf_count -= 1;
    s->audioq_size -= tb->buffer->size;
    groove_buffer_unref(tb->buffer);
    av_free(tb);
}

static int audioq_purge(GrooveQueue *queue, void *obj) {
    GrooveSink *sink = queue->context;
    GrooveSinkPrivate *s = sink->internals;
    TaggedBuffer *tb = obj;
    return tb->item == s->purge_item;
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


// this thread is responsible for decoding and inserting buffers of decoded
// audio into each sink
static int decode_thread(void *arg) {
    GroovePlayer *player = arg;
    GroovePlayerPrivate *p = player->internals;

    while (!p->abort_request) {
        SDL_LockMutex(p->decode_head_mutex);

        // if we don't have anything to decode, wait until we do
        if (!p->decode_head) {
            if (!p->sent_end_of_q) {
                every_sink_signal_end(player);
                p->sent_end_of_q = 1;
            }
            SDL_UnlockMutex(p->decode_head_mutex);
            SDL_Delay(EMPTY_PLAYLIST_DELAY);
            continue;
        }
        p->sent_end_of_q = 0;

        // if all sinks are filled up, no need to read more
        if (every_sink_full(player)) {
            SDL_UnlockMutex(p->decode_head_mutex);
            SDL_Delay(QUEUE_FULL_DELAY);
            continue;
        }

        GrooveFile *file = p->decode_head->file;
        GrooveFilePrivate *f = file->internals;

        p->volume = p->decode_head->gain * player->volume;

        if (decode_one_frame(player, file) < 0) {
            p->decode_head = p->decode_head->next;
            // seek to beginning of next song
            if (p->decode_head) {
                GrooveFile *next_file = p->decode_head->file;
                GrooveFilePrivate *next_f = next_file->internals;
                SDL_LockMutex(next_f->seek_mutex);
                next_f->seek_pos = 0;
                next_f->seek_flush = 0;
                SDL_UnlockMutex(next_f->seek_mutex);
            }
        }

        SDL_UnlockMutex(p->decode_head_mutex);
    }

    return 0;
}

static void destroy_sink(GrooveSink *sink) {
    GrooveSinkPrivate *s = sink->internals;

    if (s->audioq)
        groove_queue_destroy(s->audioq);

    av_free(s);
    av_free(sink);
}

static int audio_formats_equal(const GrooveAudioFormat *a, const GrooveAudioFormat *b) {
    return a->sample_rate == b->sample_rate &&
        a->channel_layout == b->channel_layout &&
        a->sample_fmt == b->sample_fmt;
}

static int remove_sink_from_map(GrooveSink *sink) {
    GroovePlayer *player = sink->player;
    GroovePlayerPrivate *p = player->internals;

    SinkMap *map_item = p->sink_map;
    SinkMap *prev_map_item = NULL;
    while (map_item) {
        SinkMap *next_map_item = map_item->next;
        SinkStack *stack_item = map_item->stack_head;
        SinkStack *prev_stack_item = NULL;
        while (stack_item) {
            SinkStack *next_stack_item = stack_item->next;
            GrooveSink *item_sink = stack_item->sink;
            if (item_sink == sink) {
                av_free(stack_item);
                if (prev_stack_item) {
                    prev_stack_item->next = next_stack_item;
                } else if (next_stack_item) {
                    map_item->stack_head = next_stack_item;
                } else {
                    // the stack is empty; delete the map item
                    av_free(map_item);
                    p->sink_map_count -= 1;
                    if (prev_map_item) {
                        prev_map_item->next = next_map_item;
                    } else {
                        p->sink_map = next_map_item;
                    }
                }
                return 0;
            }

            prev_stack_item = stack_item;
            stack_item = next_stack_item;
        }
        prev_map_item = map_item;
        map_item = next_map_item;
    }

    return -1;
}

static int add_sink_to_map(GroovePlayer *player, GrooveSink *sink) {
    GroovePlayerPrivate *p = player->internals;

    SinkStack *stack_entry = av_mallocz(sizeof(SinkStack));
    stack_entry->sink = sink;

    if (!stack_entry)
        return -1;

    SinkMap *map_item = p->sink_map;
    while (map_item) {
        // if our sink matches the example sink from this map entry,
        // push our sink onto the stack and we're done
        GrooveSink *example_sink = map_item->stack_head->sink;
        if (audio_formats_equal(&example_sink->audio_format, &sink->audio_format)) {
            stack_entry->next = map_item->stack_head;
            map_item->stack_head = stack_entry;
            return 0;
        }
        map_item = map_item->next;
    }
    // we did not find somewhere to put it, so push it onto the stack.
    SinkMap *map_entry = av_mallocz(sizeof(SinkMap));
    map_entry->stack_head = stack_entry;
    if (!map_entry) {
        av_free(stack_entry);
        return -1;
    }
    if (p->sink_map) {
        map_entry->next = p->sink_map;
        p->sink_map = map_entry;
    } else {
        p->sink_map = map_entry;
    }
    p->sink_map_count += 1;
    return 0;
}

void groove_sink_detach(GrooveSink *sink) {
    GrooveSinkPrivate *s = device_sink->internals;
    GrooveSink *s = sink->internals;

    // flush audio queue
    if (s->audioq) {
        groove_queue_abort(s->audioq);
        groove_queue_flush(s->audioq);
    }
    // remove from the map
    remove_sink_from_map(sink);
}

int groove_sink_attach(GrooveSink *sink, GroovePlayer *player) {
    GroovePlayerPrivate *p = player->internals;
    GrooveSinkPrivate *s = sink->internals;

    // cache computed audio format stuff
    int channel_count = av_get_channel_layout_nb_channels(sink->audio_format.channel_layout);
    sink->bytes_per_sec = channel_count * sink->audio_format.sample_rate *
        av_get_bytes_per_sample(sink->audio_format.sample_fmt);
    s->min_audioq_size = AUDIOQ_BUF_SIZE * sink->bytes_per_sec / 1000;
    av_log(NULL, AV_LOG_INFO, "audio queue size: %d\n", s->min_audioq_size);

    // in case we've called abort on the queue, reset
    groove_queue_reset(s->audioq);

    // add the sink to the entry that matches its audio format
    int err = add_sink_to_map(player, sink);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "unable to attach device: out of memory\n");
        return err;
    }

    return 0;
}

int groove_sink_get_buffer(GrooveSink *sink, GrooveBuffer **buffer, int block) {
    GrooveSinkPrivate *s = sink->internals;

    if (groove_queue_get(s->audioq, (void**)buffer, block) == 1) {
        if (buffer == &end_of_q_sentinel) {
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

GroovePlayer * groove_player_create() {
    GroovePlayer * player = av_mallocz(sizeof(GroovePlayer));
    GroovePlayerPrivate * p = av_mallocz(sizeof(GroovePlayerPrivate));
    if (!player || !p) {
        av_free(p);
        av_free(player);
        av_log(NULL, AV_LOG_ERROR, "Could not create player - out of memory\n");
        return NULL;
    }
    player->internals = p;

    // the one that the player can read
    player->volume = 1.0;
    // the other volume multiplied by the playlist item's gain
    p->volume = 1.0;

    p->eventq = groove_queue_create();
    p->decode_head_mutex = SDL_CreateMutex();
    if (!p->eventq || !p->decode_head_mutex) {
        groove_player_destroy(player);
        av_log(NULL, AV_LOG_WARNING, "unable to create player: out of memory\n");
        return NULL;
    }

    p->frame = avcodec_alloc_frame();

    if (!p->frame) {
        groove_player_destroy(player);
        av_log(NULL, AV_LOG_ERROR, "unable to alloc frame: out of memory\n");
        return -1;
    }

    p->thread_id = SDL_CreateThread(decode_thread, "decode", player);

    if (!p->thread_id) {
        groove_player_destroy(player);
        av_log(NULL, AV_LOG_ERROR, "Error creating player thread: Out of memory\n");
        return NULL;
    }

    return player;
}

void groove_player_destroy(GroovePlayer *player) {
    groove_player_clear(player);

    GroovePlayerPrivate * p = player->internals;

    // wait for decode thread to finish
    p->abort_request = 1;
    if (p->eventq)
        groove_queue_abort(p->eventq);
    SDL_WaitThread(p->thread_id, NULL);
    if (p->eventq)
        groove_queue_destroy(p->eventq);

    // remove all sinks
    GrooveSink *sink = p->sink_head;
    while (sink) {
        GrooveSink *next = sink->next;
        groove_player_remove_sink(player, sink);
        sink = next;
    }

    avfilter_graph_free(&p->filter_graph);
    avcodec_free_frame(&p->frame);

    if (p->decode_head_mutex)
        SDL_DestroyMutex(p->decode_head_mutex);

    av_free(p);
    av_free(player);
}

void groove_player_play(GroovePlayer *player) {
    GroovePlayerPrivate * p = player->internals;
    // no mutex needed for this boolean flag
    p->paused = 0;
}

void groove_player_pause(GroovePlayer *player) {
    GroovePlayerPrivate * p = player->internals;
    // no mutex needed for this boolean flag
    p->paused = 1;
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
        double gain, GroovePlaylistItem *next)
{
    GroovePlaylistItem * item = av_mallocz(sizeof(GroovePlaylistItem));
    if (!item)
        return NULL;

    item->file = file;
    item->next = next;
    item->gain = gain;

    GroovePlayerPrivate *p = player->internals;
    GrooveFilePrivate *f = file->internals;

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

        SDL_LockMutex(f->seek_mutex);
        f->seek_pos = 0;
        f->seek_flush = 0;
        SDL_UnlockMutex(f->seek_mutex);
    } else {
        item->prev = player->playlist_tail;
        player->playlist_tail->next = item;
        player->playlist_tail = item;
    }

    SDL_UnlockMutex(p->decode_head_mutex);
    return item;
}

static void purge_sink(GrooveSink *sink, GroovePlaylistItem *item) {
    GrooveSinkPrivate *s = sink->internals;

    s->purge_item = item;
    groove_queue_purge(s->audioq);
    s->purge_item = NULL;

    if (sink->purge)
        sink->purge(sink, item);
}

void groove_player_remove(GroovePlayer *player, GroovePlaylistItem *item) {
    GroovePlayerPrivate *p = player->internals;
    GrooveFile *file = item->file;
    GrooveFilePrivate *f = file->internals;

    SDL_LockMutex(p->decode_head_mutex);

    // if it's currently being played, seek to the next item
    if (item == p->decode_head) {
        p->decode_head = item->next;
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

    // in each sink,
    // we must be absolutely sure to purge the audio buffer queue
    // of references to item before freeing it at the bottom of this method
    GrooveSink *sink = p->sink_head;
    while (sink) {
        purge_sink(sink, item);
        sink = sink->next;
    }

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

void groove_player_set_gain(GroovePlayer *player, GroovePlaylistItem *item,
        double gain)
{
    GroovePlayerPrivate *p = player->internals;

    SDL_LockMutex(p->decode_head_mutex);
    item->gain = gain;
    if (item == p->decode_head) {
        p->volume = player->volume * p->decode_head->gain;
    }
    SDL_UnlockMutex(p->decode_head_mutex);
}

static int get_event(GroovePlayer *player, GroovePlayerEvent *event, int block) {
    GroovePlayerPrivate *p = player->internals;
    GroovePlayerEvent *tmp;
    int err = groove_queue_get(p->eventq, (void **)&tmp, block);
    if (err > 0) {
        *event = *tmp;
        av_free(tmp);
    }
    return err;
}

int groove_player_event_poll(GroovePlayer *player, GroovePlayerEvent *event) {
    return get_event(player, event, 0);
}

int groove_player_event_wait(GroovePlayer *player, GroovePlayerEvent *event) {
    return get_event(player, event, 1);
}

int groove_player_event_peek(GroovePlayer *player, int block) {
    GroovePlayerPrivate *p = player->internals;
    return groove_queue_peek(p->eventq, block);
}

void groove_player_position(GroovePlayer *player, GroovePlaylistItem **item,
        double *seconds)
{
    GroovePlayerPrivate *p = player->internals;

    SDL_LockMutex(p->decode_head_mutex);
    if (item)
        *item = p->decode_head;

    if (seconds && p->decode_head) {
        GrooveFile *file = p->decode_head->file;
        GrooveFilePrivate * f = file->internals;
        *seconds = f->audio_clock;
    }
    SDL_UnlockMutex(p->decode_head_mutex);
}

void groove_player_set_volume(GroovePlayer *player, double volume) {
    GroovePlayerPrivate *p = player->internals;

    SDL_LockMutex(p->decode_head_mutex);
    player->volume = volume;
    p->volume = p->decode_head ? volume * p->decode_head->gain : volume;
    SDL_UnlockMutex(p->decode_head_mutex);
}

int groove_player_playing(GroovePlayer *player) {
    GroovePlayerPrivate *p = player->internals;
    return !p->paused;
}

GrooveSink * groove_sink_create() {
    GrooveSink *sink = av_mallocz(sizeof(GrooveSink));
    GrooveSinkPrivate *s = av_mallocz(sizeof(GrooveSinkPrivate));

    if (!sink || !s) {
        av_free(sink);
        av_free(s);
        av_log(NULL, AV_LOG_ERROR, "could not create sink: out of memory\n");
        return NULL;
    }

    sink->internals = s;

    s->audioq = groove_queue_create();

    if (!s->audioq) {
        groove_sink_destroy(sink);
        av_log(NULL, AV_LOG_ERROR, "could not create audio buffer: out of memory\n");
        return NULL;
    }

    s->audioq->context = sink;
    s->audioq->cleanup = audioq_cleanup;
    s->audioq->put = audioq_put;
    s->audioq->get = audioq_get;
    s->audioq->purge = audioq_purge;

    return sink;
}

void groove_sink_destroy(GrooveSink *sink) {
    if (!sink)
        return;

    GrooveSinkPrivate *s = sink->internals;

    if (s->audioq) {

    }
}
