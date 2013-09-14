#ifndef __GROOVE_H__
#define __GROOVE_H__

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

#include <stdint.h>

typedef struct GrooveFile {
    int dirty; // read-only
    void * internals;
} GrooveFile;

typedef struct GrooveQueueItem {
    struct GrooveQueueItem * prev;
    struct GrooveFile * file;
    struct GrooveQueueItem * next;
} GrooveQueueItem;

enum GrooveReplayGainMode {
    GROOVE_REPLAYGAINMODE_OFF,
    GROOVE_REPLAYGAINMODE_TRACK,
    GROOVE_REPLAYGAINMODE_ALBUM,
};

enum GrooveState {
    GROOVE_STATE_STOPPED,
    GROOVE_STATE_PLAYING,
    GROOVE_STATE_PAUSED,
};

typedef struct GroovePlayer {
    enum GrooveState state; // read-only
    GrooveQueueItem * queue_head;
    GrooveQueueItem * queue_tail;
    enum GrooveReplayGainMode replaygain_mode;

    void * internals;
} GroovePlayer;

// flags to groove_file_metadata_*
#define GROOVE_TAG_MATCH_CASE      1
#define GROOVE_TAG_DONT_OVERWRITE 16

// If the entry already exists, append to it.  Note that no
// delimiter is added, the strings are simply concatenated.
#define GROOVE_TAG_APPEND         32

typedef void GrooveTag;

typedef struct GrooveReplayGainScan {
    void * internals;
} GrooveReplayGainScan;

enum GrooveRgEventType {
    GROOVE_RG_EVENT_PROGRESS,
};

typedef struct GrooveRgEventProgress {
    enum GrooveRgEventType type;
    int metadata_current;
    int metadata_total;
    int scanning_current;
    int scanning_total;
    int update_current;
    int update_total;
} GrooveRgEventProgress;

typedef union GrooveRgEvent {
    enum GrooveRgEventType type;
    GrooveRgEventProgress rg_progress;
} GrooveRgEvent;

/* GrooveReplayGainScan methods */
GrooveReplayGainScan * groove_create_replaygainscan();
// filename is strdup'd. you may not call add after you call exec
int groove_replaygainscan_add(GrooveReplayGainScan *scan, char *filename);
int groove_replaygainscan_exec(GrooveReplayGainScan *scan);
// call this to abort a scan or if you never call exec
void groove_replaygainscan_destroy(GrooveReplayGainScan *scan);
// returns < 0 on error
int groove_replaygainscan_event_poll(GrooveReplayGainScan *scan, GrooveRgEvent *event);
// returns < 0 on error
int groove_replaygainscan_event_wait(GrooveReplayGainScan *scan, GrooveRgEvent *event);


/* GrooveTag methods */
const char * groove_tag_key(GrooveTag *tag);
const char * groove_tag_value(GrooveTag *tag);


/* misc methods */
// enable/disable logging of errors
void groove_set_logging(int enabled);



/* GrooveFile methods */

GrooveFile * groove_open(char* filename);
void groove_close(GrooveFile * file);

char * groove_file_filename(GrooveFile *file);

GrooveTag *groove_file_metadata_get(GrooveFile *file, const char *key,
        const GrooveTag *prev, int flags);
// key entry to add to metadata. will be strdup'd
// value entry to add to metadata. will be strdup'd
//    passing NULL causes existing entry to be deleted.
// return >= 0 on success otherwise an error code < 0
// note that this will not save the file; you must call groove_file_save to do that.
int groove_file_metadata_set(GrooveFile *file, const char *key, const char *value, int flags);

// a comma separated list of short names for the format
const char * groove_file_short_names(GrooveFile *file);

// write changes made to metadata to disk.
// return < 0 on error
int groove_file_save(GrooveFile *file);


/* GroovePlayer methods */

// you may not create two simultaneous players on the same device
GroovePlayer * groove_create_player();
void groove_destroy_player(GroovePlayer *player);

void groove_player_play(GroovePlayer *player);
void groove_player_stop(GroovePlayer *player);
void groove_player_pause(GroovePlayer *player);
void groove_player_next(GroovePlayer *player);

void groove_player_seek_rel(GroovePlayer *player, double seconds);
void groove_player_seek_abs(GroovePlayer *player, double fraction);

// ownership of file switches from user to groove
// groove owns the returned queue item
GrooveQueueItem * groove_player_queue(GroovePlayer *player, GrooveFile *file);

// ownership of the returned file switches from groove to user
// item is destroyed
GrooveFile * groove_player_remove(GroovePlayer *player, GrooveQueueItem *item);

// stop playback then remove and destroy all queue items
void groove_player_clear(GroovePlayer *player);

// return the count of queue items
int groove_player_count(GroovePlayer *player);

void groove_player_set_replaygain_mode(GroovePlayer *player, enum GrooveReplayGainMode mode);



#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __GROOVE_H__ */
