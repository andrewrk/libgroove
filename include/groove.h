/*
 * Copyright (c) 2013 Andrew Kelley
 *
 * This file is part of libgroove, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#ifndef GROOVE_H_INCLUDED
#define GROOVE_H_INCLUDED

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

#include <stdint.h>

/************* global *************/
// call once at the beginning of your program from the main thread
int groove_init(void);
// call at the end of your program to clean up. after calling this
// you may no longer use this API.
void groove_finish(void);

// enable/disable logging of errors
#define GROOVE_LOG_QUIET    -8
#define GROOVE_LOG_ERROR    16
#define GROOVE_LOG_WARNING  24
#define GROOVE_LOG_INFO     32
void groove_set_logging(int level);


// channel layouts
#define GROOVE_CH_FRONT_LEFT       0x00000001
#define GROOVE_CH_FRONT_RIGHT      0x00000002
#define GROOVE_CH_FRONT_CENTER     0x00000004

#define GROOVE_CH_LAYOUT_MONO      (GROOVE_CH_FRONT_CENTER)
#define GROOVE_CH_LAYOUT_STEREO    (GROOVE_CH_FRONT_LEFT|GROOVE_CH_FRONT_RIGHT)

// get the channel count for the channel layout
int groove_channel_layout_count(uint64_t channel_layout);

// get the default channel layout based on the channel count
uint64_t groove_channel_layout_default(int count);

enum GrooveSampleFormat {
    GROOVE_SAMPLE_FMT_NONE = -1,
    GROOVE_SAMPLE_FMT_U8,          ///< unsigned 8 bits
    GROOVE_SAMPLE_FMT_S16,         ///< signed 16 bits
    GROOVE_SAMPLE_FMT_S32,         ///< signed 32 bits
    GROOVE_SAMPLE_FMT_FLT,         ///< float (32 bits)
    GROOVE_SAMPLE_FMT_DBL,         ///< double (64 bits)

    GROOVE_SAMPLE_FMT_U8P,         ///< unsigned 8 bits, planar
    GROOVE_SAMPLE_FMT_S16P,        ///< signed 16 bits, planar
    GROOVE_SAMPLE_FMT_S32P,        ///< signed 32 bits, planar
    GROOVE_SAMPLE_FMT_FLTP,        ///< float (32 bits), planar
    GROOVE_SAMPLE_FMT_DBLP,        ///< double (64 bits), planar
};

struct GrooveAudioFormat {
    int sample_rate;
    uint64_t channel_layout;
    enum GrooveSampleFormat sample_fmt;
};

int groove_sample_format_bytes_per_sample(enum GrooveSampleFormat format);


// loudness is in LUFS. EBU R128 specifies that playback should target
// -23 LUFS. replaygain on the other hand is a suggestion of how many dB to
// adjust the gain so that it equals -18 dB.
// 1 LUFS = 1 dB
double groove_loudness_to_replaygain(double loudness);


/************* GrooveFile *************/
struct GrooveFile {
    int dirty; // read-only
    char *filename; // read-only
};

// flags to groove_file_metadata_*
#define GROOVE_TAG_MATCH_CASE      1
#define GROOVE_TAG_DONT_OVERWRITE 16

// If the entry already exists, append to it.  Note that no
// delimiter is added, the strings are simply concatenated.
#define GROOVE_TAG_APPEND         32

struct GrooveTag;

const char *groove_tag_key(struct GrooveTag *tag);
const char *groove_tag_value(struct GrooveTag *tag);

// you are always responsible for calling groove_file_close on the
// returned GrooveFile.
struct GrooveFile *groove_file_open(char *filename);
void groove_file_close(struct GrooveFile *file);

struct GrooveTag *groove_file_metadata_get(struct GrooveFile *file,
        const char *key, const struct GrooveTag *prev, int flags);
// key entry to add to metadata. will be strdup'd
// value entry to add to metadata. will be strdup'd
//    passing NULL causes existing entry to be deleted.
// return >= 0 on success otherwise an error code < 0
// note that this will not save the file; you must call groove_file_save
// to do that.
int groove_file_metadata_set(struct GrooveFile *file, const char *key,
        const char *value, int flags);

// a comma separated list of short names for the format
const char *groove_file_short_names(struct GrooveFile *file);

// write changes made to metadata to disk.
// return < 0 on error
int groove_file_save(struct GrooveFile *file);

// main audio stream duration in seconds. note that this relies on a
// combination of format headers and heuristics. It can be inaccurate.
// The most accurate way to learn the duration of a file is to use
// GrooveDurationAnalyzer
double groove_file_duration(struct GrooveFile *file);

// get the audio format of the main audio stream of a file
void groove_file_audio_format(struct GrooveFile *file,
        struct GrooveAudioFormat *audio_format);

/************* GroovePlaylist *************/
struct GroovePlaylistItem {
    // all fields are read-only. modify with methods below.
    struct GroovePlaylistItem *prev;
    struct GrooveFile *file;
    // a volume adjustment in float format to apply to the file when it plays.
    // This is typically used for ReplayGain.
    // To convert from dB to float, use exp(log(10) * 0.05 * dB_value)
    double gain;
    struct GroovePlaylistItem *next;
};

struct GroovePlaylist {
    // all fields are read-only. modify using methods below.
    // doubly linked list which is the playlist
    struct GroovePlaylistItem *head;
    struct GroovePlaylistItem *tail;

    // in float format, defaults to 1.0
    double volume;
};

// a playlist manages keeping an audio buffer full
// to send the buffer to your speakers, use groove_player_create
struct GroovePlaylist *groove_playlist_create(void);
// this will not call groove_file_close on any files
// it will remove all playlist items and sinks from the playlist
void groove_playlist_destroy(struct GroovePlaylist *playlist);


void groove_playlist_play(struct GroovePlaylist *playlist);
void groove_playlist_pause(struct GroovePlaylist *playlist);

void groove_playlist_seek(struct GroovePlaylist *playlist,
        struct GroovePlaylistItem *item, double seconds);

// once you add a file to the playlist, you must not destroy it until you first
// remove it from the playlist.
// next: the item to insert before. if NULL, you will append to the playlist.
// gain: see GroovePlaylistItem structure. use 0 for no adjustment.
// returns the newly created playlist item.
struct GroovePlaylistItem *groove_playlist_insert(
        struct GroovePlaylist *playlist, struct GrooveFile *file, double gain,
        struct GroovePlaylistItem *next);

// this will not call groove_file_close on item->file !
// item is destroyed and the address it points to is no longer valid
void groove_playlist_remove(struct GroovePlaylist *playlist,
        struct GroovePlaylistItem *item);

// get the position of the decode head
// both the current playlist item and the position in seconds in the playlist
// item are given. item will be set to NULL if the playlist is empty
// you may pass NULL for item or seconds
// Note that typically you are more interested in the position of the play
// head, not the decode head. For example, if you have a GroovePlayer attached,
// groove_player_position will give you the position of the play head.
void groove_playlist_position(struct GroovePlaylist *playlist,
        struct GroovePlaylistItem **item, double *seconds);

// return 1 if the playlist is playing; 0 if it is not.
int groove_playlist_playing(struct GroovePlaylist *playlist);


// remove all playlist items
void groove_playlist_clear(struct GroovePlaylist *playlist);

// return the count of playlist items
int groove_playlist_count(struct GroovePlaylist *playlist);

void groove_playlist_set_gain(struct GroovePlaylist *playlist,
        struct GroovePlaylistItem *item, double gain);

// value is in float format. defaults to 1.0
void groove_playlist_set_volume(struct GroovePlaylist *playlist, double volume);

/************ GrooveBuffer ****************/

#define GROOVE_BUFFER_NO  0
#define GROOVE_BUFFER_YES 1
#define GROOVE_BUFFER_END 2

struct GrooveBuffer {
    // all fields read-only
    // for interleaved audio, data[0] is the buffer.
    // for planar audio, each channel has a separate data pointer.
    // for encoded audio, data[0] is the encoded buffer.
    uint8_t **data;

    struct GrooveAudioFormat format;

    // number of audio frames described by this buffer
    // for encoded audio, this is unknown and set to 0.
    int frame_count;

    // when encoding, if item is NULL, this is a format header or trailer. otherwise,
    // this is encoded audio for the item specified.
    // when decoding, item is never NULL.
    struct GroovePlaylistItem *item;
    double pos;

    // total number of bytes contained in this buffer
    int size;
};

void groove_buffer_ref(struct GrooveBuffer *buffer);
void groove_buffer_unref(struct GrooveBuffer *buffer);

/************** GrooveSink ****************/

// use this to get access to a realtime raw audio buffer
// for example you could use it to draw a waveform or other visualization
// GroovePlayer uses this internally to get the audio buffer for playback

struct GrooveSink {
    // set this to the audio format you want the sink to output
    struct GrooveAudioFormat audio_format;
    // Set this flag to ignore audio_format. If you set this flag, the
    // buffers you pull from this sink could have any audio format.
    int disable_resample;
    // If you leave this to its default of 0, frames pulled from the sink
    // will have sample count determined by efficiency.
    // If you set this to a positive number, frames pulled from the sink
    // will always have this number of samples.
    int buffer_sample_count;

    // how big the buffer queue should be, in sample frames.
    // groove_sink_create defaults this to 8192
    int buffer_size;

    // set to whatever you want
    void *userdata;
    // called when the audio queue is flushed. For example, if you seek to a
    // different location in the song.
    void (*flush)(struct GrooveSink *);
    // called when a playlist item is deleted. Take this opportunity to remove
    // all your references to the GroovePlaylistItem.
    void (*purge)(struct GrooveSink *, struct GroovePlaylistItem *);

    // read-only. set when you call groove_sink_attach. cleared when you call
    // groove_sink_detach
    struct GroovePlaylist *playlist;

    // read-only. automatically computed from audio_format when you call
    // groove_sink_attach
    int bytes_per_sec;
};

struct GrooveSink *groove_sink_create(void);
void groove_sink_destroy(struct GrooveSink *sink);

// before calling this, set audio_format
// returns 0 on success, < 0 on error
int groove_sink_attach(struct GrooveSink *sink, struct GroovePlaylist *playlist);
// returns 0 on success, < 0 on error
int groove_sink_detach(struct GrooveSink *sink);

// returns < 0 on error, GROOVE_BUFFER_NO on aborted (block=1) or no buffer
// ready (block=0), GROOVE_BUFFER_YES on buffer returned, and GROOVE_BUFFER_END
// on end of playlist.
// buffer is always set to either a valid GrooveBuffer or NULL
int groove_sink_buffer_get(struct GrooveSink *sink, struct GrooveBuffer **buffer,
        int block);

// returns < 0 on error, 0 on no buffer ready, 1 on buffer ready
// if block is 1, block until buffer is ready
int groove_sink_buffer_peek(struct GrooveSink *sink, int block);

/************* GroovePlayer ****************/

// use this to make a playlist utilize your speakers

enum GroovePlayerEventType {
    // when the currently playing track changes.
    GROOVE_EVENT_NOWPLAYING,

    // when something tries to read from an empty buffer
    GROOVE_EVENT_BUFFERUNDERRUN,
};

union GroovePlayerEvent {
    enum GroovePlayerEventType type;
};

struct GroovePlayer {
    // set this to the device you want to open
    // NULL means default device
    char *device_name;

    // The desired audio format settings with which to open the device.
    // groove_player_create defaults these to 44100 Hz,
    // signed 16-bit int, stereo.
    // These are preferences; if a setting cannot be used, a substitute will be
    // used instead. actual_audio_format is set to the actual values.
    struct GrooveAudioFormat target_audio_format;

    // how big the device buffer should be, in sample frames.
    // must be a power of 2.
    // groove_player_create defaults this to 1024
    int device_buffer_size;

    // how big the sink buffer should be, in sample frames.
    // groove_player_create defaults this to 8192
    int sink_buffer_size;

    // read-only. set when you call groove_player_attach and cleared when
    // you call groove_player_detach
    struct GroovePlaylist *playlist;

    // read-only. set to the actual format you get when you open the device.
    // ideally will be the same as target_audio_format but might not be.
    struct GrooveAudioFormat actual_audio_format;
};

// Returns the number of available devices exposed by the current driver or -1
// if an explicit list of devices can't be determined. A return value of -1
// does not necessarily mean an error condition.
// In many common cases, when this function returns a value <= 0, it can still
// successfully open the default device (NULL for the device name)
// This function may trigger a complete redetect of available hardware. It
// should not be called for each iteration of a loop, but rather once at the
// start of a loop.
int groove_device_count(void);

// Returns the name of the audio device at the requested index, or NULL on error
// The string returned by this function is UTF-8 encoded, read-only, and
// managed internally. You are not to free it. If you need to keep the string
// for any length of time, you should make your own copy of it.
const char *groove_device_name(int index);

struct GroovePlayer *groove_player_create(void);
void groove_player_destroy(struct GroovePlayer *player);

// Attaches the player to the playlist instance and opens the device to
// begin playback.
// Internally this creates a GrooveSink and sends the samples to the device.
// you must detach a player before destroying it or the playlist it is
// attached to
// returns 0 on success, < 0 on error
int groove_player_attach(struct GroovePlayer *player,
        struct GroovePlaylist *playlist);
// returns 0 on success, < 0 on error
int groove_player_detach(struct GroovePlayer *player);

// get the position of the play head
// both the current playlist item and the position in seconds in the playlist
// item are given. item will be set to NULL if the playlist is empty
// you may pass NULL for item or seconds
void groove_player_position(struct GroovePlayer *player,
        struct GroovePlaylistItem **item, double *seconds);

// returns < 0 on error, 0 on no event ready, 1 on got event
int groove_player_event_get(struct GroovePlayer *player,
        union GroovePlayerEvent *event, int block);
// returns < 0 on error, 0 on no event ready, 1 on event ready
// if block is 1, block until event is ready
int groove_player_event_peek(struct GroovePlayer *player, int block);


/************* GrooveEncoder ************/

// attach a GrooveEncoder to a playlist to keep a buffer of encoded audio full.
// for example you could use it to implement an http audio stream

struct GrooveEncoder {
    // The desired audio format to encode.
    // groove_encoder_create defaults these to 44100 Hz,
    // signed 16-bit int, stereo.
    // These are preferences; if a setting cannot be used, a substitute will be
    // used instead. actual_audio_format is set to the actual values.
    struct GrooveAudioFormat target_audio_format;

    // Select encoding quality by choosing a target bit rate in bits per
    // second. Note that typically you see this expressed in "kbps", such
    // as 320kbps or 128kbps. Surprisingly, in this circumstance 1 kbps is
    // 1000 bps, *not* 1024 bps as you would expect.
    // groove_encoder_create defaults this to 256000
    int bit_rate;

    // optional - choose a short name for the format
    // to help libgroove guess which format to use
    // use `avconv -formats` to get a list of possibilities
    char *format_short_name;
    // optional - choose a short name for the codec
    // to help libgroove guess which codec to use
    // use `avconv -codecs` to get a list of possibilities
    char *codec_short_name;
    // optional - provide an example filename
    // to help libgroove guess which format/codec to use
    char *filename;
    // optional - provide a mime type string
    // to help libgroove guess which format/codec to use
    char *mime_type;

    // how big the sink buffer should be, in sample frames.
    // groove_encoder_create defaults this to 8192
    int sink_buffer_size;

    // how big the encoded audio buffer should be, in bytes
    // groove_encoder_create defaults this to 16384
    int encoded_buffer_size;

    // read-only. set when attached and cleared when detached
    struct GroovePlaylist *playlist;

    // read-only. set to the actual format you get when you attach to a
    // playlist. ideally will be the same as target_audio_format but might
    // not be.
    struct GrooveAudioFormat actual_audio_format;
};

struct GrooveEncoder *groove_encoder_create(void);
// detach before destroying
void groove_encoder_destroy(struct GrooveEncoder *encoder);

// once you attach, you must detach before destroying the playlist
// at playlist begin, format headers are generated. when end of playlist is
// reached, format trailers are generated.
int groove_encoder_attach(struct GrooveEncoder *encoder,
        struct GroovePlaylist *playlist);
int groove_encoder_detach(struct GrooveEncoder *encoder);

// returns < 0 on error, GROOVE_BUFFER_NO on aborted (block=1) or no buffer
// ready (block=0), GROOVE_BUFFER_YES on buffer returned, and GROOVE_BUFFER_END
// on end of playlist.
// buffer is always set to either a valid GrooveBuffer or NULL.
int groove_encoder_buffer_get(struct GrooveEncoder *encoder,
        struct GrooveBuffer **buffer, int block);

// returns < 0 on error, 0 on no buffer ready, 1 on buffer ready
// if block is 1, block until buffer is ready
int groove_encoder_buffer_peek(struct GrooveEncoder *encoder, int block);

// see docs for groove_file_metadata_get
struct GrooveTag *groove_encoder_metadata_get(struct GrooveEncoder *encoder,
        const char *key, const struct GrooveTag *prev, int flags);
// see docs for groove_file_metadata_set
int groove_encoder_metadata_set(struct GrooveEncoder *encoder, const char *key,
        const char *value, int flags);

/************* GrooveLoudnessDetector *************/
struct GrooveLoudnessDetectorInfo {
    // loudness is in LUFS. 1 LUFS == 1 dB
    // for playback you might adjust the gain so that it is equal to -18 dB
    // (this would be the replaygain standard) or so that it is equal to -23 dB
    // (this would be the EBU R128 standard).
    double loudness;
    // peak amplitude in float format
    double peak;
    // how many seconds long this song is
    double duration;

    // if item is NULL, this info applies to all songs analyzed until
    // this point. otherwise it is the playlist item that this info
    // applies to.
    struct GroovePlaylistItem *item;
};

struct GrooveLoudnessDetector {
    // maximum number of GrooveLoudnessDetectorInfo items to store in this
    // loudness detector's queue. this defaults to MAX_INT, meaning that
    // the loudness detector will cause the decoder to decode the entire
    // playlist. if you want to instead, for example, obtain loudness info
    // at the same time as playback, you might set this value to 1.
    int info_queue_size;

    // how big the sink buffer should be, in sample frames.
    // groove_loudness_detector_create defaults this to 8192
    int sink_buffer_size;

    // read-only. set when attached and cleared when detached
    struct GroovePlaylist *playlist;
};

struct GrooveLoudnessDetector *groove_loudness_detector_create(void);
void groove_loudness_detector_destroy(struct GrooveLoudnessDetector *detector);

// once you attach, you must detach before destroying the playlist
int groove_loudness_detector_attach(struct GrooveLoudnessDetector *detector,
        struct GroovePlaylist *playlist);
int groove_loudness_detector_detach(struct GrooveLoudnessDetector *detector);

// returns < 0 on error, 0 on aborted (block=1) or no info ready (block=0),
// 1 on info returned
int groove_loudness_detector_info_get(struct GrooveLoudnessDetector *detector,
        struct GrooveLoudnessDetectorInfo *info, int block);

// returns < 0 on error, 0 on no info ready, 1 on info ready
// if block is 1, block until info is ready
int groove_loudness_detector_info_peek(struct GrooveLoudnessDetector *detector,
        int block);

// get the position of the detect head
// both the current playlist item and the position in seconds in the playlist
// item are given. item will be set to NULL if the playlist is empty
// you may pass NULL for item or seconds
void groove_loudness_detector_position(struct GrooveLoudnessDetector *detector,
        struct GroovePlaylistItem **item, double *seconds);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* GROOVE_H_INCLUDED */
