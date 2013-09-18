#ifndef __GROOVE_H__
#define __GROOVE_H__

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

/************* global *************/
// call once at the beginning of your program
int groove_init();
// enable/disable logging of errors
#define GROOVE_LOG_QUIET    -8
#define GROOVE_LOG_ERROR    16
#define GROOVE_LOG_WARNING  24
#define GROOVE_LOG_INFO     32
void groove_set_logging(int level);


/************* GrooveFile *************/
typedef struct GrooveFile {
    int dirty; // read-only
    char *filename; // read-only

    void *internals; // hands off
} GrooveFile;

// flags to groove_file_metadata_*
#define GROOVE_TAG_MATCH_CASE      1
#define GROOVE_TAG_DONT_OVERWRITE 16

// If the entry already exists, append to it.  Note that no
// delimiter is added, the strings are simply concatenated.
#define GROOVE_TAG_APPEND         32

typedef void GrooveTag;

const char * groove_tag_key(GrooveTag *tag);
const char * groove_tag_value(GrooveTag *tag);

// you are always responsible for calling groove_close on the returned GrooveFile.
GrooveFile * groove_open(char* filename);
void groove_close(GrooveFile * file);

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

// song duration in seconds
double groove_file_duration(GrooveFile *file);


/************* GroovePlayer *************/
enum GrooveReplayGainMode {
    GROOVE_REPLAYGAINMODE_OFF,
    GROOVE_REPLAYGAINMODE_TRACK,
    GROOVE_REPLAYGAINMODE_ALBUM,
};

// TODO rename Queue to Playlist
typedef struct GrooveQueueItem {
    struct GrooveQueueItem * prev;
    GrooveFile * file;
    enum GrooveReplayGainMode replaygain_mode;
    struct GrooveQueueItem * next;
} GrooveQueueItem;

typedef struct GroovePlayer {
    // read-only - doubly linked list which is the queue
    GrooveQueueItem * queue_head;
    GrooveQueueItem * queue_tail;

    void * internals; // don't touch this
} GroovePlayer;

enum GroovePlayerEventType {
    // when the currently playing track changes.
    GROOVE_PLAYER_EVENT_NOWPLAYING,

    // when the audio device tries to read from an empty buffer
    GROOVE_PLAYER_EVENT_BUFFERUNDERRUN,
};

typedef union GroovePlayerEvent {
    enum GroovePlayerEventType type;
} GroovePlayerEvent;

// you may not create two simultaneous players on the same device
GroovePlayer * groove_create_player();
// this will not call groove_close on any files
void groove_destroy_player(GroovePlayer *player);

void groove_player_play(GroovePlayer *player);
void groove_player_pause(GroovePlayer *player);

void groove_player_seek(GroovePlayer *player, GrooveQueueItem *item, double seconds);

// once you add a file to the queue, you must not destroy it until you first
// remove it from the queue.
// next: the item you will insert before. if it is NULL, you will append to the queue.
// returns the newly created queue item.
GrooveQueueItem * groove_player_insert(GroovePlayer *player, GrooveFile *file,
        GrooveQueueItem *next);

// this will not call groove_close on item->file !
// item is destroyed and the address it points to is no longer valid
void groove_player_remove(GroovePlayer *player, GrooveQueueItem *item);

// get the position of the playhead
// both the current queue item and the position in seconds in the queue
// item are given. item will be set to NULL if the queue is empty
// you may pass NULL for item or seconds
void groove_player_position(GroovePlayer *player, GrooveQueueItem **item, double *seconds);

// return 1 if the player is playing; 0 if it is not.
int groove_player_playing(GroovePlayer *player);


// remove all queue items
void groove_player_clear(GroovePlayer *player);

// return the count of queue items
int groove_player_count(GroovePlayer *player);

// default value: GROOVE_REPLAYGAINMODE_ALBUM
// this method sets the replaygain mode on a queue item so that the volume is
// seamlessly updated at exactly the right time when the player progresses to
// the item on the queue.
void groove_player_set_replaygain_mode(GroovePlayer *player, GrooveQueueItem *item,
        enum GrooveReplayGainMode mode);

// value is in float format. defaults to 0.75
// this value is applied when replaygain is on to make headroom for replaygain
// adjustments.
void groove_player_set_replaygain_preamp(GroovePlayer *player, double preamp);

// value is in float format. defaults to 0.25
// this is the replaygain value that is used if tags are missing.
void groove_player_set_replaygain_default(GroovePlayer *player, double value);

// value is in float format. defaults to 1.0
void groove_player_set_volume(GroovePlayer *player, double volume);

// returns < 0 on error
int groove_player_event_poll(GroovePlayer *player, GroovePlayerEvent *event);
// returns < 0 on error
int groove_player_event_wait(GroovePlayer *player, GroovePlayerEvent *event);


/************* GrooveReplayGainScan *************/
typedef struct GrooveReplayGainScan {
    void * internals;
} GrooveReplayGainScan;

enum GrooveRgEventType {
    GROOVE_RG_EVENT_PROGRESS,
    GROOVE_RG_EVENT_COMPLETE,
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




#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __GROOVE_H__ */
