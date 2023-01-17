/*
 * Copyright (c) 2013 Andrew Kelley
 *
 * This file is part of libgroove, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#ifndef GROOVE_LOUDNESS_H_INCLUDED
#define GROOVE_LOUDNESS_H_INCLUDED

#include <groove/groove.h>

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

struct GrooveLoudnessDetectorInfo {
    /* loudness is in LUFS. 1 LUFS == 1 dB
     * EBU R128 specifies that playback should target -23 LUFS. replaygain on
     * the other hand is a suggestion of how many dB to adjust the gain so
     * that it equals -18 dB.
     * so, for playback you might adjust the gain so that it is equal to -18 dB
     * (this would be the replaygain standard) or so that it is equal to -23 dB
     * (this would be the EBU R128 standard).
     */
    double loudness;
    /* peak amplitude in float format */
    double peak;
    /* how many seconds long this song is */
    double duration;

    /* if item is NULL, this info applies to all songs analyzed until
     * this point. otherwise it is the playlist item that this info
     * applies to.
     * when disable_album is set, this sentinel is still sent, but loudness
     * will be set to 0
     */
    struct GroovePlaylistItem *item;
};

struct GrooveLoudnessDetector {
    /* maximum number of GrooveLoudnessDetectorInfo items to store in this
     * loudness detector's queue. this defaults to MAX_INT, meaning that
     * the loudness detector will cause the decoder to decode the entire
     * playlist. if you want to instead, for example, obtain loudness info
     * at the same time as playback, you might set this value to 1.
     */
    int info_queue_size;

    /* how big the sink buffer should be, in sample frames.
     * groove_loudness_detector_create defaults this to 8192
     */
    int sink_buffer_size;

    /* set to 1 to only compute track loudness. This is faster and requires
     * less memory than computing both.
     */
    int disable_album;

    /* read-only. set when attached and cleared when detached */
    struct GroovePlaylist *playlist;
};

struct GrooveLoudnessDetector *groove_loudness_detector_create(void);
void groove_loudness_detector_destroy(struct GrooveLoudnessDetector *detector);

/* once you attach, you must detach before destroying the playlist */
int groove_loudness_detector_attach(struct GrooveLoudnessDetector *detector,
        struct GroovePlaylist *playlist);
int groove_loudness_detector_detach(struct GrooveLoudnessDetector *detector);

/* returns < 0 on error, 0 on aborted (block=1) or no info ready (block=0),
 * 1 on info returned
 */
int groove_loudness_detector_info_get(struct GrooveLoudnessDetector *detector,
        struct GrooveLoudnessDetectorInfo *info, int block);

/* returns < 0 on error, 0 on no info ready, 1 on info ready
 * if block is 1, block until info is ready
 */
int groove_loudness_detector_info_peek(struct GrooveLoudnessDetector *detector,
        int block);

/* get the position of the detect head
 * both the current playlist item and the position in seconds in the playlist
 * item are given. item will be set to NULL if the playlist is empty
 * you may pass NULL for item or seconds
 */
void groove_loudness_detector_position(struct GrooveLoudnessDetector *detector,
        struct GroovePlaylistItem **item, double *seconds);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* GROOVE_LOUDNESS_H_INCLUDED */
