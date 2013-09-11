#ifndef __GROOVE_H__
#define __GROOVE_H__

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

#include <stdint.h>

typedef struct GrooveFile {
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


/* misc methods */

// enable/disable logging of errors
void groove_set_logging(int enabled);



/* GrooveFile methods */

GrooveFile * groove_open(char* filename);
void groove_close(GrooveFile * file);

char * groove_file_filename(GrooveFile *file);

// a comma separated list of short names for the format
const char * groove_file_short_names(GrooveFile *file);


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
