#include "groove.h"
#include "decode.h"
#include "queue.h"

#include <libavutil/mem.h>
#include <libavutil/log.h>
#include <libavutil/channel_layout.h>

#include <SDL/SDL.h>
#include <SDL/SDL_thread.h>

#include <ebur128.h>

typedef struct FileListItem {
    char *filename;
    double peak_amplitude; // scale of 0 to 1
    double loudness; // in LUFS
    struct FileListItem *next;
} FileListItem;

typedef struct AlbumListItem {
    char *album_name;
    FileListItem *first_file; // first pointer to list of tracks in this album
    size_t track_count;
    double peak_amplitude; // scale of 0 to 1
    double loudness; // in dB
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
    GrooveQueue *eventq;
    SDL_Thread *thread_id;
    int abort_request;
    GrooveDecodeContext decode_ctx;

    ebur128_state **ebur_states;
    size_t next_ebur_state_index;
    size_t ebur_state_count;
    char strbuf[100];
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
    album_item->track_count += 1;
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
            node->track_count += 1;
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

    GrooveFilePrivate *f = file->internals;
    f->seek_pos = 0;
    f->seek_flush = 1;
    GrooveDecodeContext *decode_ctx = &s->decode_ctx;
    while (groove_decode(decode_ctx, file) >= 0) {}
    groove_close(file);

    return 0;
}

static double clamp_rg(double x) {
    if (x < -51.0) return -51.0;
    else if (x > 51.0) return 51.0;
    else return x;
}

static double loudness_to_replaygain(double loudness) {
    // loudness is in LUFS. The goal is for the loudness to be -23 LUFS.
    // however replaygain is a suggestion of how many dB to adjust the gain
    // so that it equals -18 dB.
    // 1 LUFS = 1 dB

    return clamp_rg(-18.0 - loudness);
}

static int update_with_rg_info(GrooveReplayGainScan *scan, AlbumListItem *album_item,
        FileListItem *file_item)
{
    GrooveFile *file = groove_open(file_item->filename);
    if (!file) {
        av_log(NULL, AV_LOG_WARNING, "error opening %s\n", file_item->filename);
        return -1;
    }
    GrooveReplayGainScanPrivate *s = scan->internals;

    snprintf(s->strbuf, sizeof(s->strbuf), "%.2f dB",
            loudness_to_replaygain(file_item->loudness));
    groove_file_metadata_set(file, "REPLAYGAIN_TRACK_GAIN", s->strbuf, 0);

    snprintf(s->strbuf, sizeof(s->strbuf), "%.2f dB",
            loudness_to_replaygain(album_item->loudness));
    groove_file_metadata_set(file, "REPLAYGAIN_ALBUM_GAIN", s->strbuf, 0);

    snprintf(s->strbuf, sizeof(s->strbuf), "%f", file_item->peak_amplitude);
    groove_file_metadata_set(file, "REPLAYGAIN_TRACK_PEAK", s->strbuf, 0);

    snprintf(s->strbuf, sizeof(s->strbuf), "%f", album_item->peak_amplitude);
    groove_file_metadata_set(file, "REPLAYGAIN_ALBUM_PEAK", s->strbuf, 0);

    int err = groove_file_save(file);
    groove_close(file);
    return err;
}

static void cleanup_ebur(GrooveReplayGainScan *scan) {
    GrooveReplayGainScanPrivate *s = scan->internals;
    for (int i = 0; i < s->ebur_state_count; i += 1) {
        if (s->ebur_states[i])
            ebur128_destroy(&s->ebur_states[i]);
    }
    av_freep(&s->ebur_states);
    s->ebur_state_count = 0;
    s->next_ebur_state_index = 0;
}

static int init_ebur(GrooveReplayGainScan *scan, size_t count) {
    GrooveReplayGainScanPrivate *s = scan->internals;
    s->next_ebur_state_index = 0;
    s->ebur_state_count = count;
    s->ebur_states = av_malloc(count * sizeof(ebur128_state*));
    if (!s->ebur_states) {
        av_log(NULL, AV_LOG_ERROR, "error initializing loudness scanner: out of memory\n");
        return -1;
    }
    int destroy = 0;
    for (int i = 0; i < count; i += 1) {
        s->ebur_states[i] = ebur128_init(2, 44100, EBUR128_MODE_SAMPLE_PEAK|EBUR128_MODE_I);
        destroy = destroy || !s->ebur_states[i];
    }
    if (destroy) {
        cleanup_ebur(scan);
        return -1;
    }
    return 0;
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
                if (!s->ebur_states && init_ebur(scan, s->album_item->track_count) < 0)
                    break;
                // replaygain scan a track from an album
                // item ends up going into scanned_file_item
                FileListItem *item = filelist_pop(&s->album_item->first_file);
                if (replaygain_scan(scan, item) >= 0)
                    rg_prog->update_total += 1;
                rg_prog->scanning_current += 1;

                // grab the loudness value and the peak value
                ebur128_state *st = s->ebur_states[s->next_ebur_state_index];
                s->next_ebur_state_index += 1;
                ebur128_loudness_global(st, &item->loudness);
                double out;
                ebur128_sample_peak(st, 0, &out);
                s->album_item->peak_amplitude = s->album_item->peak_amplitude > out ?
                    s->album_item->peak_amplitude : out;
                item->peak_amplitude = item->peak_amplitude > out ?
                    item->peak_amplitude : out;
                ebur128_sample_peak(st, 1, &out);
                s->album_item->peak_amplitude = s->album_item->peak_amplitude > out ?
                    s->album_item->peak_amplitude : out;
                item->peak_amplitude = item->peak_amplitude > out ?
                    item->peak_amplitude : out;

                filelist_push(&s->scanned_file_item, item);
                // if we're done scanning the entire album, get the loudness
                if (!s->album_item->first_file) {
                    ebur128_loudness_global_multiple(s->ebur_states, s->ebur_state_count,
                            &s->album_item->loudness);
                    cleanup_ebur(scan);
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
            break;
        }
        // put progress event on the queue
        GrooveRgEvent *evt = av_malloc(sizeof(GrooveRgEvent));
        if (!evt) {
            av_log(NULL, AV_LOG_WARNING, "unable to create progress event: out of memory\n");
            break;
        }
        *evt = s->progress_event;
        if (groove_queue_put(s->eventq, evt) < 0)
            break;
    }

    GrooveRgEvent *done_evt = av_malloc(sizeof(GrooveRgEvent));
    if (!done_evt) {
        av_log(NULL, AV_LOG_ERROR, "unable to create event: out of memory\n");
        return 0;
    }
    done_evt->type = GROOVE_RG_EVENT_COMPLETE;
    if (groove_queue_put(s->eventq, done_evt) < 0)
        av_log(NULL, AV_LOG_ERROR, "unable to put event on queue: out of memory\n");
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

static int scan_buffer(GrooveDecodeContext *decode_ctx, AVFrame *frame) {
    GrooveReplayGainScan *scan = decode_ctx->callback_context;
    GrooveReplayGainScanPrivate *s = scan->internals;

    ebur128_state *st = s->ebur_states[s->next_ebur_state_index];
    size_t frame_count = frame->linesize[0] / sizeof(double) / 2;
    ebur128_add_frames_double(st, (double*)frame->data[0], frame_count);

    av_frame_free(&frame);
    return 0;
}

int groove_replaygainscan_exec(GrooveReplayGainScan *scan) {
    GrooveReplayGainScanPrivate *s = scan->internals;
    if (s->executing) {
        av_log(NULL, AV_LOG_WARNING, "exec called more than once\n");
        return -1;
    }
    s->decode_ctx.callback_context = scan;
    s->decode_ctx.buffer = scan_buffer;
    s->decode_ctx.dest_sample_rate = 44100;
    s->decode_ctx.dest_channel_layout = AV_CH_LAYOUT_STEREO;
    s->decode_ctx.dest_sample_fmt = AV_SAMPLE_FMT_DBL;

    if (groove_init_decode_ctx(&s->decode_ctx) < 0)
        return -1;
    s->decode_ctx.replaygain_mode = GROOVE_REPLAYGAINMODE_OFF;

    s->progress_event.type = GROOVE_RG_EVENT_PROGRESS;
    s->eventq = groove_queue_create();
    if (!s->eventq) {
        av_log(NULL, AV_LOG_WARNING, "unable to create event queue: out of memory\n");
        return -1;
    }
    s->executing = 1;

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

    if (s->eventq)
        groove_queue_abort(s->eventq);

    if (s->thread_id) {
        s->abort_request = 1;
        SDL_WaitThread(s->thread_id, NULL);
    }
    cleanup_ebur(scan);
    groove_cleanup_decode_ctx(&s->decode_ctx);
    if (s->eventq)
        groove_queue_destroy(s->eventq);
    albumlist_cleanup(&s->album_item);
    filelist_cleanup(&s->file_item);
    filelist_cleanup(&s->scanned_file_item);
    av_free(s);
    av_free(scan);
}

static int get_event(GrooveReplayGainScan *scan, GrooveRgEvent *event, int block) {
    GrooveReplayGainScanPrivate *s = scan->internals;
    GrooveRgEvent *tmp;
    int err = groove_queue_get(s->eventq, (void **)&tmp, block);
    if (err < 0)
        return err;
    *event = *tmp;
    av_free(tmp);
    return 0;
}

int groove_replaygainscan_event_poll(GrooveReplayGainScan *scan, GrooveRgEvent *event) {
    return get_event(scan, event, 0);
}

int groove_replaygainscan_event_wait(GrooveReplayGainScan *scan, GrooveRgEvent *event) {
    return get_event(scan, event, 1);
}
