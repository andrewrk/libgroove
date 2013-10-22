#ifndef __GROOVE_H__
#define __GROOVE_H__

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

#include <stdint.h>

/************* global *************/
// call once at the beginning of your program
int groove_init();

// enable/disable logging of errors
#define GROOVE_LOG_QUIET    -8
#define GROOVE_LOG_ERROR    16
#define GROOVE_LOG_WARNING  24
#define GROOVE_LOG_INFO     32
void groove_set_logging(int level);


// channel layouts
#define GROOVE_CH_FRONT_LEFT             0x00000001
#define GROOVE_CH_FRONT_RIGHT            0x00000002
#define GROOVE_CH_FRONT_CENTER           0x00000004

#define GROOVE_CH_LAYOUT_MONO              (GROOVE_CH_FRONT_CENTER)
#define GROOVE_CH_LAYOUT_STEREO            (GROOVE_CH_FRONT_LEFT|GROOVE_CH_FRONT_RIGHT)

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

int groove_sample_format_bytes_per_sample(enum GrooveSampleFormat format);

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

// you are always responsible for calling groove_file_close on the returned GrooveFile.
GrooveFile * groove_file_open(char* filename);
void groove_file_close(GrooveFile * file);

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

// TODO rename GroovePlayer to GroovePlaylist
/************* GroovePlayer *************/
typedef struct GroovePlaylistItem {
    // all fields are read-only. modify with methods below.
    struct GroovePlaylistItem * prev;
    GrooveFile * file;
    // a volume adjustment in float format to apply to the file when it plays.
    // This is typically used for ReplayGain.
    // To convert from dB to float, use exp(log(10) * 0.05 * dB_value)
    double gain;
    struct GroovePlaylistItem * next;
} GroovePlaylistItem;

typedef struct GroovePlayer {
    // all fields are read-only. modify using methods below.
    // doubly linked list which is the playlist
    GroovePlaylistItem * playlist_head;
    GroovePlaylistItem * playlist_tail;

    // in float format, defaults to 1.0
    double volume;

    void * internals; // don't touch this
} GroovePlayer;

// a player manages keeping an audio buffer full
// to send the buffer to your speakers, use groove_device_sink_create
GroovePlayer * groove_player_create();
// this will not call groove_file_close on any files
// it will remove all playlist items and sinks from the player
void groove_player_destroy(GroovePlayer *player);


void groove_player_play(GroovePlayer *player);
void groove_player_pause(GroovePlayer *player);

void groove_player_seek(GroovePlayer *player, GroovePlaylistItem *item, double seconds);

// once you add a file to the playlist, you must not destroy it until you first
// remove it from the playlist.
// next: the item you will insert before. if it is NULL, you will append to the playlist.
// gain: see GroovePlaylistItem structure. use 0 for no adjustment.
// returns the newly created playlist item.
GroovePlaylistItem * groove_player_insert(GroovePlayer *player, GrooveFile *file,
        double gain, GroovePlaylistItem *next);

// this will not call groove_file_close on item->file !
// item is destroyed and the address it points to is no longer valid
void groove_player_remove(GroovePlayer *player, GroovePlaylistItem *item);

// get the position of the decode head
// both the current playlist item and the position in seconds in the playlist
// item are given. item will be set to NULL if the playlist is empty
// you may pass NULL for item or seconds
// Note that typically you are more interested in the position of the play
// head, not the decode head. Example methods which return the play head are
// groove_device_sink_position and groove_encoder_position
void groove_player_position(GroovePlayer *player, GroovePlaylistItem **item,
        double *seconds);

// return 1 if the player is playing; 0 if it is not.
int groove_player_playing(GroovePlayer *player);


// remove all playlist items
void groove_player_clear(GroovePlayer *player);

// return the count of playlist items
int groove_player_count(GroovePlayer *player);

void groove_player_set_gain(GroovePlayer *player, GroovePlaylistItem *item,
        double gain);

// value is in float format. defaults to 1.0
void groove_player_set_volume(GroovePlayer *player, double volume);

/************ GrooveBuffer ****************/

#define GROOVE_BUFFER_NO  0
#define GROOVE_BUFFER_YES 1
#define GROOVE_BUFFER_END 2

typedef struct GrooveAudioFormat {
    int sample_rate;
    uint64_t channel_layout;
    enum GrooveSampleFormat sample_fmt;
} GrooveAudioFormat;

typedef struct GrooveBuffer {
    // all fields read-only
    // for interleaved audio, data[0] is the only thing you need.
    // for planar audio, each channel has a separate data pointer.
    uint8_t **data;

    GrooveAudioFormat format;
    int sample_count;
    GroovePlaylistItem *item;
    double pos;

    // for convenience the total number of bytes contained in this buffer
    // are provided.
    int size;

    void *internals;
} GrooveBuffer;

void groove_buffer_ref(GrooveBuffer *buffer);
void groove_buffer_unref(GrooveBuffer *buffer);

/************** GrooveSink ****************/

// use this to get access to a realtime raw audio buffer
// for example you could use it to draw a waveform or other visualization
// GrooveDeviceSink uses this internally to get the audio buffer for playback

enum GrooveEventType {
    // when the currently playing track changes.
    GROOVE_EVENT_NOWPLAYING,

    // when something tries to read from an empty buffer
    GROOVE_EVENT_BUFFERUNDERRUN,
};

typedef union GrooveEvent {
    enum GrooveEventType type;
} GrooveEvent;

typedef struct GrooveSink {
    // set this to the audio format you want the sink to output
    GrooveAudioFormat audio_format;

    // how big the buffer should be, in sample frames.
    // groove_sink_create defaults this to 8192
    int buffer_size;

    // set to whatever you want
    void *userdata;
    // called when the audio queue is flushed. For example, if you seek to a
    // different location in the song.
    void (*flush)(struct GrooveSink *);
    // called when a playlist item is deleted. Take this opportunity to remove
    // all your references to the GroovePlaylistItem.
    void (*purge)(struct GrooveSink *, GroovePlaylistItem *);

    // read-only. set when you call groove_sink_attach. cleared when you call
    // groove_sink_detach
    GroovePlayer *player;

    // read-only. automatically computed from audio_format when you call
    // groove_sink_attach
    int bytes_per_sec;

    void *internals; // private
} GrooveSink;

GrooveSink * groove_sink_create();
void groove_sink_destroy(GrooveSink *sink);

// before calling this, set audio_format
// returns 0 on success, < 0 on error
int groove_sink_attach(GrooveSink *sink, GroovePlayer *player);
// returns 0 on success, < 0 on error
int groove_sink_detach(GrooveSink *sink);

// returns < 0 on error, GROOVE_BUFFER_NO on aborted (block=1) or no buffer ready (block=0),
// GROOVE_BUFFER_YES on buffer returned, and GROOVE_BUFFER_END on end of playlist
// buffer is always set to either a valid GrooveBuffer or NULL 
int groove_sink_get_buffer(GrooveSink *sink, GrooveBuffer **buffer, int block);


// TODO rename GrooveDeviceSink to GroovePlayer
/************* GrooveDeviceSink ****************/

// use this to make a player utilize your speakers

typedef struct GrooveDeviceSink {
    // set this to the device you want to open
    // NULL means default device
    char *device_name;

    // The desired audio format settings with which to open the device.
    // groove_device_sink_create defaults these to 44100 Hz,
    // signed 16-bit int, stereo.
    // These are preferences; if a setting cannot be used, a substitute will
    // be used instead. In this case, audio_format will be updated to reflect
    // the substituted values.
    GrooveAudioFormat target_audio_format;

    // how big the device buffer should be, in sample frames.
    // must be a power of 2.
    // groove_device_sink_create defaults this to 1024
    int device_buffer_size;

    // how big the memory buffer should be, in sample frames.
    // groove_device_sink_create defaults this to 8192
    int memory_buffer_size;

    // read-only. set when you call groove_device_sink_attach and cleared when
    // you call groove_device_sink_detach
    GroovePlayer *player;

    // read-only. set to the actual format you get when you open the device.
    // ideally will be the same as target_audio_format but might not be.
    GrooveAudioFormat actual_audio_format;

    void *internals;
} GrooveDeviceSink;

// Returns the number of available devices exposed by the current driver or -1
// if an explicit list of devices can't be determined. A return value of -1
// does not necessarily mean an error condition.
// In many common cases, when this function returns a value <= 0, it can still
// successfully open the default device (NULL for the name argument of
// groove_player_attach_device).
// This function may trigger a complete redetect of available hardware. It
// should not be called for each iteration of a loop, but rather once at the
// start of a loop.
int groove_device_count();

// Returns the name of the audio device at the requested index, or NULL on error.
// The string returned by this function is UTF-8 encoded, read-only, and
// managed internally. You are not to free it. If you need to keep the string
// for any length of time, you should make your own copy of it.
const char * groove_device_name(int index);

GrooveDeviceSink* groove_device_sink_create();
void groove_device_sink_destroy(GrooveDeviceSink *device_sink);

// Attaches the device sink to the player instance and opens the device to
// begin playback.
// Internally this creates a GrooveSink and sends the samples to the device.
// you must detach a device sink before destroying it or the player it is
// attached to
// returns 0 on success, < 0 on error
int groove_device_sink_attach(GrooveDeviceSink *device_sink, GroovePlayer *player);
// returns 0 on success, < 0 on error
int groove_device_sink_detach(GrooveDeviceSink *device_sink);

// get the position of the play head
// both the current playlist item and the position in seconds in the playlist
// item are given. item will be set to NULL if the playlist is empty
// you may pass NULL for item or seconds
void groove_device_sink_position(GrooveDeviceSink *device_sink,
        GroovePlaylistItem **item, double *seconds);

// returns < 0 on error, 0 on no event ready, 1 on got event
int groove_device_sink_event_get(GrooveDeviceSink *sink,
        GroovePlayerEvent *event, int block);
// returns < 0 on error, 0 on no event ready, 1 on event ready
// if block is 1, block until event is ready
int groove_device_sink_event_peek(GrooveDeviceSink *sink, int block);


/************* GrooveEncoder ************/

// attach a GrooveEncoder to a player to keep a buffer of encoded audio full.
// for example you could use it to implement an http audio stream

typedef void GrooveEncoder;

typedef struct GrooveEncodeFormat {
    GrooveAudioFormat audio_format;
    // select encoding quality by choosing a target bit rate
    int bit_rate;
    // optional - choose a short name for the format
    // to help libgroove guess which format to use
    // use `avconv -formats` to get a list of possibilities
    char * format_short_name;
    // optional - choose a short name for the codec
    // to help libgroove guess which codec to use
    // use `avconv -codecs` to get a list of possibilities
    char * codec_short_name;
    // optional - provide an example filename
    // to help libgroove guess which format/codec to use
    char * filename;
    // optional - provide a mime type string
    // to help libgroove guess which format/codec to use
    char * mime_type;
} GrooveEncodeFormat;

GrooveEncoder* groove_encoder_create(GroovePlayer *player,
        const GrooveEncodeFormat *format);
// you must destroy all encoders before destroying their players
void groove_encoder_destroy(GrooveEncoder *encoder);
// returns < 0 on error, GROOVE_BUFFER_NO on aborted (block=1) or no buffer ready (block=0),
// GROOVE_BUFFER_YES on buffer returned, and GROOVE_BUFFER_END on end of playlist
int groove_encoder_get_buffer(GrooveEncoder *encoder, GrooveBuffer **buffer,
        int block);

/************* GrooveReplayGainScan *************/
typedef struct GrooveReplayGainScan {
    // userdata: the same value you passed to groove_replaygainscan_add
    // amount: value between 0 and 1 representing progress
    // optional callback
    void (*file_progress)(void *userdata, double amount);
    // number of seconds to decode before progress callback is called
    double progress_interval;
    // userdata: the same value you passed to groove_replaygainscan_add
    // gain: recommended gain adjustment of this file, in float format
    // peak: peak amplitude of this file, in float format
    void (*file_complete)(void *userdata, double gain, double peak);
    // set this to 1 during a callback if you want to abort the scan
    int abort_request;
    // hands off
    void *internals;
} GrooveReplayGainScan;

// after you create a GrooveReplayGainScan you may set the callbacks and call
// groove_replaygainscan_add
GrooveReplayGainScan * groove_replaygainscan_create();

// userdata will be passed back in callbacks
int groove_replaygainscan_add(GrooveReplayGainScan *scan, GrooveFile *file, void *userdata);

// starts replaygain scanning. blocks until scanning is complete.
// gain: recommended gain adjustment of all files in scan, in float format
// peak: peak amplitude of all files in scan, in float format
int groove_replaygainscan_exec(GrooveReplayGainScan *scan, double *gain, double *peak);

// must be called to cleanup. May not be called during a callback.
void groove_replaygainscan_destroy(GrooveReplayGainScan *scan);


#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __GROOVE_H__ */
