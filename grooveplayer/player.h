/*
 * Copyright (c) 2013 Andrew Kelley
 *
 * This file is part of libgroove, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#ifndef GROOVE_PLAYER_H
#define GROOVE_PLAYER_H

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

    /* when the audio device is re-opened due to audio format changing */
    GROOVE_EVENT_DEVICEREOPENED,

    /* when the audio device gets an error re-opening */
    GROOVE_EVENT_DEVICE_REOPEN_ERROR,

    /* user requested wakeup */
    GROOVE_EVENT_WAKEUP
};

union GroovePlayerEvent {
    enum GroovePlayerEventType type;
};

struct GrooveDevice;

struct GroovePlayer {
    /* Set this to the device you want to open. You may use NULL for default.
     * If you set `use_dummy_device` to 1, this field is ignored.
     */
    struct GrooveDevice *device;

    /* The desired audio format settings with which to open the device.
     * groove_player_create defaults these to 48000 Hz,
     * signed 32-bit native endian integer, stereo.
     * These are preferences; if a setting cannot be used, a substitute will be
     * used instead. actual_audio_format is set to the actual values.
     * If you set `use_exact_audio_format` to 1, this field is ignored.
     */
    struct GrooveAudioFormat target_audio_format;

    /* how big the sink buffer should be, in sample frames.
     * groove_player_create defaults this to 8192
     */
    int sink_buffer_size;

    /* Volume adjustment to make to this player.
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

    /* If you set this to 1, `device` is ignored and a dummy device is opened
     * instead.
     */
    int use_dummy_device;
};

struct GroovePlayerContext {
    // Defaults to NULL. Put whatever you want here.
    void *userdata;
    // Optional callback. Called when the list of devices change. Only called
    // during a call to groove_player_context_flush_events or
    // groove_player_context_flush_events
    void (*on_devices_change)(struct GroovePlayerContext *);
    // Optional callback. Called from an unknown thread that you should not use
    // to call any groove functions. You may use this to signal a condition
    // variable to wake up. Called when groove_wait_events would be woken up.
    void (*on_events_signal)(struct GroovePlayerContext *);
};

struct GroovePlayerContext *groove_player_context_create(void);
void groove_player_context_destroy(struct GroovePlayerContext *player_context);

int groove_player_context_connect(struct GroovePlayerContext *player_context);
void groove_player_context_disconnect(struct GroovePlayerContext *player_context);

/* when you call this, the on_devices_change and on_events_signal callbacks
 * might be called. This is the only time those callbacks will be called.
 * When devices are updated on the system, you won't see the updates until you
 * call this function.
 */
void groove_player_context_flush_events(struct GroovePlayerContext *player_context);

/* Flushes events as they occur, blocks until you call groove_player_context_wakeup.
 * Be ready for spurious wakeups.
 */
void groove_player_context_wait(struct GroovePlayerContext *player_context);

/* wake up groove_player_context_wait */
void groove_player_context_wakeup(struct GroovePlayerContext *player_context);

/* Returns the number of available devices. */
int groove_player_context_device_count(struct GroovePlayerContext *player_context);

/* Returns the index of the default device */
int groove_player_context_device_default(struct GroovePlayerContext *player_context);

/* Call groove_device_unref when done */
struct GrooveDevice *groove_player_context_get_device(
        struct GroovePlayerContext *player_context, int index);



const char *groove_device_id(struct GrooveDevice *device);
const char *groove_device_name(struct GrooveDevice *device);
int groove_device_is_raw(struct GrooveDevice *device);
void groove_device_ref(struct GrooveDevice *device);
void groove_device_unref(struct GrooveDevice *device);


struct GroovePlayer *groove_player_create(struct GroovePlayerContext *);
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

/* wakes up a blocking call to groove_player_event_get or
 * groove_player_event_peek with a GROOVE_EVENT_WAKEUP.
 */
void groove_player_event_wakeup(struct GroovePlayer *player);

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

#endif
