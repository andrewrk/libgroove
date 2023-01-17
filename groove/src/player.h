/*
 * Copyright (c) 2013 Andrew Kelley
 *
 * This file is part of libgroove, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#ifndef GROOVE_PLAYER_H_INCLUDED
#define GROOVE_PLAYER_H_INCLUDED

#include <groove/groove.h>

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

/* use this to make a playlist utilize your speakers */

enum GroovePlayerEventType {
    /* when the currently playing track changes. */
    GROOVE_EVENT_NOWPLAYING,

    /* when something tries to read from an empty buffer */
    GROOVE_EVENT_BUFFERUNDERRUN,

    /* when the audio device is re-opened due to audio format changing*/
    GROOVE_EVENT_DEVICEREOPENED
};

union GroovePlayerEvent {
    enum GroovePlayerEventType type;
};

#define GROOVE_PLAYER_DEFAULT_DEVICE (-1)
#define GROOVE_PLAYER_DUMMY_DEVICE   (-2)

struct GroovePlayer {
    /* set this to the device you want to open
     * could also be GROOVE_PLAYER_DEFAULT_DEVICE or GROOVE_PLAYER_DUMMY_DEVICE
     */
    int device_index;

    /* The desired audio format settings with which to open the device.
     * groove_player_create defaults these to 44100 Hz,
     * signed 16-bit int, stereo.
     * These are preferences; if a setting cannot be used, a substitute will be
     * used instead. actual_audio_format is set to the actual values.
     */
    struct GrooveAudioFormat target_audio_format;

    /* how big the device buffer should be, in sample frames.
     * must be a power of 2.
     * groove_player_create defaults this to 1024
     */
    int device_buffer_size;

    /* how big the sink buffer should be, in sample frames.
     * groove_player_create defaults this to 8192
     */
    int sink_buffer_size;

    /* This volume adjustment to make to this player.
     * It is recommended that you leave this at 1.0 and instead adjust the
     * gain of the underlying playlist.
     * If you want to change this value after you have already attached the
     * sink to the playlist, you must use groove_player_set_gain.
     * float format. Defaults to 1.0
     */
    double gain;

    /* read-only. set when you call groove_player_attach and cleared when
     * you call groove_player_detach
     */
    struct GroovePlaylist *playlist;

    /* read-only. set to the actual format you get when you open the device.
     * ideally will be the same as target_audio_format but might not be.
     */
    struct GrooveAudioFormat actual_audio_format;

    /* If you set this to 1, target_audio_format and actual_audio_format are
     * ignored and no resampling, channel layout remapping, or sample format
     * conversion will occur. The audio device will be reopened with exact
     * parameters whenever necessary.
     */
    int use_exact_audio_format;
};

/* Returns the number of available devices exposed by the current driver or -1
 * if an explicit list of devices can't be determined. A return value of -1
 * does not necessarily mean an error condition.
 * In many common cases, when this function returns a value <= 0, it can still
 * successfully open the default device (NULL for the device name)
 * This function may trigger a complete redetect of available hardware. It
 * should not be called for each iteration of a loop, but rather once at the
 * start of a loop.
 */
int groove_device_count(void);

/* Returns the name of the audio device at the requested index, or NULL on error
 * The string returned by this function is UTF-8 encoded, read-only, and
 * managed internally. You are not to free it. If you need to keep the string
 * for any length of time, you should make your own copy of it.
 */
const char *groove_device_name(int index);

struct GroovePlayer *groove_player_create(void);
void groove_player_destroy(struct GroovePlayer *player);

/* Attaches the player to the playlist instance and opens the device to
 * begin playback.
 * Internally this creates a GrooveSink and sends the samples to the device.
 * you must detach a player before destroying it or the playlist it is
 * attached to
 * returns 0 on success, < 0 on error
 */
int groove_player_attach(struct GroovePlayer *player,
        struct GroovePlaylist *playlist);
/* returns 0 on success, < 0 on error */
int groove_player_detach(struct GroovePlayer *player);

/* get the position of the play head
 * both the current playlist item and the position in seconds in the playlist
 * item are given. item will be set to NULL if the playlist is empty
 * you may pass NULL for item or seconds
 */
void groove_player_position(struct GroovePlayer *player,
        struct GroovePlaylistItem **item, double *seconds);

/* returns < 0 on error, 0 on no event ready, 1 on got event */
int groove_player_event_get(struct GroovePlayer *player,
        union GroovePlayerEvent *event, int block);
/* returns < 0 on error, 0 on no event ready, 1 on event ready
 * if block is 1, block until event is ready
 */
int groove_player_event_peek(struct GroovePlayer *player, int block);

/* See the gain property of GrooveSink. It is recommended that you leave this
 * at 1.0 and instead adjust the gain of the playlist.
 * returns 0 on success, < 0 on error
 */
int groove_player_set_gain(struct GroovePlayer *player, double gain);

/* When you set the use_exact_audio_format field to 1, the audio device is
 * closed and re-opened as necessary. When this happens, a
 * GROOVE_EVENT_DEVICEREOPENED event is emitted, and you can use this function
 * to discover the audio format of the device.
 */
struct GrooveAudioFormat groove_player_get_device_audio_format(struct GroovePlayer *player);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* GROOVE_PLAYER_H_INCLUDED */
