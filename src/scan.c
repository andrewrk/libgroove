#include "groove.h"
#include "gain_analysis.h"

#include <libavutil/mem.h>
#include <libavutil/log.h>

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
    double peak_amplitude; // in dB
    double replay_gain; // in dB
    struct FileListItem *next;
} FileListItem;

typedef struct AlbumListItem {
    char *album_name;
    FileListItem *first_file; // first pointer to list of tracks in this album
    double peak_amplitude; // in dB
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
    int executing;
    GrooveRgEvent progress_event;
    EventQueue eventq;
    SDL_Thread *thread_id;
    int abort_request;
    GainAnalysis *anal;
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

static int albumlist_push(AlbumListItem **list, FileListItem *item, const char *album_name) {
    AlbumListItem *album_item = av_mallocz(sizeof(AlbumListItem));
    if (!album_item)
        return -1;
    album_item->next = *list;
    album_item->album_name = av_strdup(album_name);
    *list = album_item;
    FileListItem *file_list = (*list)->first_file;
    filelist_push(&file_list, item);
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
            FileListItem *file_list = node->first_file;
            filelist_push(&file_list, item);
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
    if (s) {}
    // TODO

    groove_close(file);
    return 0;
}

static int update_with_rg_info(GrooveReplayGainScan *scan, AlbumListItem *album_item,
        FileListItem *file_item)
{
    // TODO
    return -1;
}

static int scan_thread(void *arg) {
    GrooveReplayGainScan *scan = arg;
    GrooveReplayGainScanPrivate *s = scan->internals;
    GrooveRgEventProgress * rg_prog = &s->progress_event.rg_progress;
    while (!s->abort_request) {
        if (s->file_item) {
            // scan for metadata and group by album
            FileListItem *item = filelist_pop(&s->file_item);
            if (metadata_scan(scan, item) >= 0)
                rg_prog->scanning_total += 1;
            rg_prog->metadata_current += 1;
        } else if (s->album_item && s->album_item->first_file) {
            // replaygain scan a track from an album
            FileListItem *item = filelist_pop(&s->album_item->first_file);
            if (replaygain_scan(scan, item) >= 0)
                rg_prog->update_total += 1;
            rg_prog->scanning_current += 1;
        } else if (s->scanned_file_item) {
            // update tracks from an album which has completed
            FileListItem *item = filelist_pop(&s->scanned_file_item);
            update_with_rg_info(scan, s->album_item, item);
            rg_prog->update_current += 1;
        } else {
            // we must be done.
            break;
        }
        // put progress event on the queue
        if (event_queue_put(&s->eventq, &s->progress_event) < 0)
            av_log(NULL, AV_LOG_WARNING, "unable to create progress event: out of memory\n");
    }
    return 0;
}

GrooveReplayGainScan * groove_create_replaygainscan() {
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

void groove_replaygainscan_exec(GrooveReplayGainScan *scan) {
    GrooveReplayGainScanPrivate *s = scan->internals;
    if (s->executing) {
        av_log(NULL, AV_LOG_WARNING, "exec called more than once\n");
        return;
    }
    s->executing = 1;
    s->progress_event.type = GROOVE_RG_EVENT_PROGRESS;
    event_queue_init(&s->eventq);
    s->anal = gain_create_analysis(44100);
    s->thread_id = SDL_CreateThread(scan_thread, scan);
}

void groove_replaygainscan_destroy(GrooveReplayGainScan *scan) {
    if (!scan)
        return;
    GrooveReplayGainScanPrivate *s = scan->internals;
    if (s->executing) {
        event_queue_abort(&s->eventq);
        event_queue_end(&s->eventq);
        s->abort_request = 1;
        SDL_WaitThread(s->thread_id, NULL);
        gain_destroy_analysis(s->anal);
    }
    // TODO free all the things

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
