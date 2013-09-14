#include "groove.h"
#include "decode.h"
#include "gain_analysis.h"

#include <libavutil/mem.h>
#include <libavutil/log.h>
#include <libavutil/channel_layout.h>

#include <SDL/SDL.h>
#include <SDL/SDL_thread.h>

typedef struct EventList {
    GrooveRgEvent event;
    struct EventList *next;
} EventList;

typedef struct EventQueue {
    EventList *first;
    EventList *last;
    SDL_mutex *mutex;
    SDL_cond *cond;
    int abort_request;
} EventQueue;

typedef struct FileListItem {
    char *filename;
    double peak_amplitude; // scale of 0 to 1
    double replay_gain; // in dB
    struct FileListItem *next;
} FileListItem;

typedef struct AlbumListItem {
    char *album_name;
    FileListItem *first_file; // first pointer to list of tracks in this album
    double peak_amplitude; // scale of 0 to 1
    double replay_gain; // in dB
    struct AlbumListItem *next; // next pointer in list of albums
} AlbumListItem;

typedef struct GrooveReplayGainScanPrivate {
    // the full list of unprocessed files go into file_item
    FileListItem *file_item;
    // as metadata is read from files, they go from file_item to album_item
    AlbumListItem *album_item;
    // when scanning a file completes, it goes from album_item to scanned_file_item
    // when album_item->first_file is NULL, scanned_file_item will be a list of
    // scanned files for a complete album and should have its metadata updated
    FileListItem *scanned_file_item;
    // this pointer only exists while replaygain scanning
    FileListItem *currently_scanning;
    int executing;
    GrooveRgEvent progress_event;
    EventQueue eventq;
    SDL_Thread *thread_id;
    int abort_request;
    GainAnalysis *anal;
    DecodeContext decode_ctx;
} GrooveReplayGainScanPrivate;

static void filelist_push(FileListItem **list, FileListItem *item) {
    item->next = *list;
    *list = item;
}

static FileListItem * filelist_pop(FileListItem **list) {
    FileListItem *popped = *list;
    *list = (*list)->next;
    return popped;
}

static void event_queue_init(EventQueue *q) {
    memset(q, 0, sizeof(EventQueue));
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
}

static void event_queue_flush(EventQueue *q) {
    SDL_LockMutex(q->mutex);

    EventList *el;
    EventList *el1;
    for (el = q->first; el != NULL; el = el1) {
        el1 = el->next;
        av_free(el);
    }
    q->first = NULL;
    SDL_UnlockMutex(q->mutex);
}

static void event_queue_end(EventQueue *q) {
    event_queue_flush(q);
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->cond);
}

static void event_queue_abort(EventQueue *q) {
    SDL_LockMutex(q->mutex);

    q->abort_request = 1;

    SDL_CondSignal(q->cond);
    SDL_UnlockMutex(q->mutex);
}

static int event_queue_put(EventQueue *q, GrooveRgEvent *event) {
    EventList * el1 = av_mallocz(sizeof(EventList));

    if (!el1)
        return -1;

    el1->event = *event;

    SDL_LockMutex(q->mutex);

    if (!q->last)
        q->first = el1;
    else
        q->last->next = el1;
    q->last = el1;

    SDL_CondSignal(q->cond);
    SDL_UnlockMutex(q->mutex);

    return 0;
}

// return < 0 if aborted, 0 if no buffer and > 0 if buffer.
static int event_queue_get(EventQueue *q, GrooveRgEvent *event, int block) {
    EventList *ev1;
    int ret;

    SDL_LockMutex(q->mutex);

    for (;;) {
        if (q->abort_request) {
            ret = -1;
            break;
        }

        ev1 = q->first;
        if (ev1) {
            q->first = ev1->next;
            if (!q->first)
                q->last = NULL;
            *event = ev1->event;
            av_free(ev1);
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

static void albumlist_pop(AlbumListItem **list) {
    AlbumListItem *popped = *list;
    *list = (*list)->next;
    av_free(popped->album_name);
    av_free(popped);
}

static int albumlist_push(AlbumListItem **list, FileListItem *item, const char *album_name) {
    AlbumListItem *album_item = av_mallocz(sizeof(AlbumListItem));
    if (!album_item)
        return -1;
    album_item->next = *list;
    album_item->album_name = av_strdup(album_name);
    *list = album_item;
    filelist_push(&(*list)->first_file, item);
    return 0;
}

static int albumlist_push_match(AlbumListItem **list, FileListItem *item,
        const char *album_name)
{
    if (!album_name)
        return albumlist_push(list, item, NULL);

    // find matching album_name
    AlbumListItem *node = *list;
    while (node) {
        if (node->album_name && strcmp(node->album_name, album_name) == 0) {
            // add to this album list
            filelist_push(&node->first_file, item);
            return 0;
        }
        node = node->next;
    }
    // not found
    return albumlist_push(list, item, album_name);
}

static int metadata_scan(GrooveReplayGainScan *scan, FileListItem *item) {
    GrooveReplayGainScanPrivate *s = scan->internals;
    GrooveFile *file = groove_open(item->filename);
    if (!file) {
        av_log(NULL, AV_LOG_WARNING, "error opening %s\n", item->filename);
        return -1;
    }
    GrooveTag * tag = groove_file_metadata_get(file, "album", NULL, 0);
    const char *album_name = NULL;
    if (tag) {
        album_name = groove_tag_value(tag);
        if (strlen(album_name) == 0)
            album_name = NULL;
    }
    int ret = albumlist_push_match(&s->album_item, item, album_name);
    groove_close(file);
    return ret;
}

static int replaygain_scan(GrooveReplayGainScan *scan, FileListItem *item) {
    GrooveReplayGainScanPrivate *s = scan->internals;
    GrooveFile *file = groove_open(item->filename);
    if (!file) {
        av_log(NULL, AV_LOG_WARNING, "error opening %s\n", item->filename);
        return -1;
    }

    // save this pointer to make it easier on the buffer_planar callback
    s->currently_scanning = item;
    DecodeContext *decode_ctx = &s->decode_ctx;
    while (decode(decode_ctx, file) >= 0) {}
    groove_close(file);
    s->currently_scanning = NULL;

    item->replay_gain = gain_get_title(s->anal);

    return 0;
}

static int update_with_rg_info(GrooveReplayGainScan *scan, AlbumListItem *album_item,
        FileListItem *file_item)
{
    // TODO actually update the file with the replaygain info
    fprintf(stderr, "\nUpdate %s track replaygain %f\n"
            "album replaygain %f\ntrack peak: %f\nalbum peak: %f\n", file_item->filename,
            file_item->replay_gain, album_item->replay_gain, file_item->peak_amplitude,
            album_item->peak_amplitude);
    return -1;
}

static int scan_thread(void *arg) {
    GrooveReplayGainScan *scan = arg;
    GrooveReplayGainScanPrivate *s = scan->internals;
    GrooveRgEventProgress * rg_prog = &s->progress_event.rg_progress;
    while (!s->abort_request) {
        if (s->file_item) {
            // scan for metadata and group by album
            // item ends up going into an album_item
            FileListItem *item = filelist_pop(&s->file_item);
            if (metadata_scan(scan, item) >= 0)
                rg_prog->scanning_total += 1;
            rg_prog->metadata_current += 1;
        } else if (s->album_item) {
            if (s->album_item->first_file) {
                // replaygain scan a track from an album
                // item ends up going into scanned_file_item
                FileListItem *item = filelist_pop(&s->album_item->first_file);
                if (replaygain_scan(scan, item) >= 0)
                    rg_prog->update_total += 1;
                rg_prog->scanning_current += 1;
                filelist_push(&s->scanned_file_item, item);
                // if we're done scanning the entire album, get the replay_gain
                if (!s->album_item->first_file) {
                    s->album_item->replay_gain = gain_get_album(s->anal);
                    gain_init_analysis(s->anal, s->decode_ctx.dest_sample_rate);
                }
            } else if (s->scanned_file_item) {
                // update tracks from an album which has completed
                // we are responsible for freeing item
                FileListItem *item = filelist_pop(&s->scanned_file_item);
                update_with_rg_info(scan, s->album_item, item);
                rg_prog->update_current += 1;
                av_freep(&item->filename);
                av_freep(&item);
                // if we're done updating track metadata, move on to scanning the
                // next album
                if (!s->scanned_file_item)
                    albumlist_pop(&s->album_item);
            }
        } else {
            // we must be done.
            s->executing = 2;
            event_queue_abort(&s->eventq);
            break;
        }
        // put progress event on the queue
        if (event_queue_put(&s->eventq, &s->progress_event) < 0)
            av_log(NULL, AV_LOG_WARNING, "unable to create progress event: out of memory\n");
    }
    return 0;
}

GrooveReplayGainScan * groove_create_replaygainscan() {
    if (maybe_init() < 0)
        return NULL;

    GrooveReplayGainScan * scan = av_mallocz(sizeof(GrooveReplayGainScan));
    GrooveReplayGainScanPrivate * s = av_mallocz(sizeof(GrooveReplayGainScanPrivate));
    if (!scan || !s) {
        av_free(scan);
        av_free(s);
        av_log(NULL, AV_LOG_ERROR, "error creating replaygain scan: out of memory\n");
        return NULL;
    }
    scan->internals = s;
    return scan;
}

int groove_replaygainscan_add(GrooveReplayGainScan *scan, char *filename) {
    GrooveReplayGainScanPrivate *s = scan->internals;
    char * newfilename = av_strdup(filename);
    FileListItem *item = av_mallocz(sizeof(FileListItem));
    if (!newfilename || !item) {
        av_free(newfilename);
        av_free(item);
        av_log(NULL, AV_LOG_ERROR, "error adding file to replaygain scan: out of memory\n");
        return -1;
    }
    item->filename = newfilename;
    filelist_push(&s->file_item, item);
    s->progress_event.rg_progress.metadata_total += 1;
    return 0;
}

static int scan_buffer_planar (struct DecodeContext *decode_ctx,
        uint8_t *left_buf, uint8_t *right_buf, int data_size)
{
    GrooveReplayGainScan *scan = decode_ctx->callback_context;
    GrooveReplayGainScanPrivate *s = scan->internals;
    FileListItem *item = s->currently_scanning;

    // keep track of peak
    // assume it's a bunch of doubles
    size_t count = data_size / sizeof(double);
    double *left = (double*)left_buf;
    double *right = (double*)right_buf;
    for (int i = 0; i < count; i += 1) {
        double abs_l = abs(left[i]);
        double abs_r = abs(right[i]);
        double max = abs_l > abs_r ? abs_l : abs_r;
        if (max > item->peak_amplitude)
            item->peak_amplitude = max;
        if (max > s->album_item->peak_amplitude)
            s->album_item->peak_amplitude = max;
    }

    gain_analyze_samples(s->anal, left, right, count, 2);
    return 0;
}

int groove_replaygainscan_exec(GrooveReplayGainScan *scan) {
    if (maybe_init_sdl() < 0)
        return -1;
    GrooveReplayGainScanPrivate *s = scan->internals;
    if (s->executing) {
        av_log(NULL, AV_LOG_WARNING, "exec called more than once\n");
        return -1;
    }
    s->decode_ctx.frame = avcodec_alloc_frame();
    if (!s->decode_ctx.frame) {
        av_log(NULL, AV_LOG_ERROR, "unable to alloc frame: out of memory\n");
        return -1;
    }
    s->decode_ctx.callback_context = scan;
    s->decode_ctx.buffer_planar = scan_buffer_planar;
    s->decode_ctx.dest_sample_rate = 44100;
    s->decode_ctx.dest_channel_layout = AV_CH_LAYOUT_STEREO;
    s->decode_ctx.dest_channel_count = 2;
    s->decode_ctx.dest_sample_fmt = AV_SAMPLE_FMT_DBLP;
    s->executing = 1;
    s->progress_event.type = GROOVE_RG_EVENT_PROGRESS;
    event_queue_init(&s->eventq);
    s->anal = gain_create_analysis();
    gain_init_analysis(s->anal, s->decode_ctx.dest_sample_rate);
    s->thread_id = SDL_CreateThread(scan_thread, scan);
    return 0;
}

static void filelist_cleanup(FileListItem **list) {
    while (*list) {
        FileListItem *node = filelist_pop(list);
        av_free(node->filename);
        av_free(node);
    }
    *list = NULL;
}

static void albumlist_cleanup(AlbumListItem **list) {
    while (*list)
        albumlist_pop(list);
    *list = NULL;
}

void groove_replaygainscan_destroy(GrooveReplayGainScan *scan) {
    if (!scan)
        return;
    GrooveReplayGainScanPrivate *s = scan->internals;
    if (s->executing == 1) {
        s->executing = 2;
        event_queue_abort(&s->eventq);
        s->abort_request = 1;
        SDL_WaitThread(s->thread_id, NULL);
    }
    if (s->executing == 2) {
        s->executing = 0;
        event_queue_end(&s->eventq);
        gain_destroy_analysis(s->anal);
        cleanup_decode_ctx(&s->decode_ctx);
    }
    albumlist_cleanup(&s->album_item);
    filelist_cleanup(&s->file_item);
    filelist_cleanup(&s->scanned_file_item);
    av_free(s);
    av_free(scan);
}

int groove_replaygainscan_event_poll(GrooveReplayGainScan *scan, GrooveRgEvent *event) {
    GrooveReplayGainScanPrivate *s = scan->internals;
    return event_queue_get(&s->eventq, event, 0);
}

int groove_replaygainscan_event_wait(GrooveReplayGainScan *scan, GrooveRgEvent *event) {
    GrooveReplayGainScanPrivate *s = scan->internals;
    return event_queue_get(&s->eventq, event, 1);
}
