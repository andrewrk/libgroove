#include <stdio.h>
#include <inttypes.h>

#include "libavutil/avstring.h"
#include "libavutil/dict.h"
#include "libavutil/samplefmt.h"
#include "libavformat/avformat.h"
#include "libavresample/avresample.h"
#include "libavutil/opt.h"

#include <SDL/SDL.h>
#include <SDL/SDL_thread.h>

/* SDL audio buffer size, in samples. Should be small to have precise
   A/V sync as SDL does not have hardware buffer fullness info. */
#define SDL_AUDIO_BUFFER_SIZE 1024

typedef struct PacketQueue {
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    int abort_request;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;

typedef struct GrooveFilePrivate {
    int audio_stream_index;
    int abort_request; // true when we're closing the file
    char filename[1024];
    AVFormatContext *ic;
    int seek_by_bytes;
    AVCodec *decoder;
    int sdl_sample_rate;
    uint64_t sdl_channel_layout;
    int sdl_channels;
    enum AVSampleFormat sdl_sample_fmt;
    enum AVSampleFormat resample_sample_fmt;
    uint64_t resample_channel_layout;
    int resample_sample_rate;
    AVStream *audio_st;
    AVPacket audio_pkt;
    AVPacket audio_pkt_temp;
    int paused;
    int last_paused;
    int seek_req;
    int seek_flags;
    int64_t seek_pos;
    int64_t seek_rel;
    int eof;
    double audio_clock;
    AVAudioResampleContext *avr;
    AVFrame *frame;
} GrooveFilePrivate;

typedef struct GroovePlayerPrivate {
    AVPacket flush_pkt;
    SDL_Thread *thread_id;
    int abort_request;
    PacketQueue audioq;
    SDL_AudioSpec spec;
    uint8_t silence_buf[SDL_AUDIO_BUFFER_SIZE];
    uint8_t *audio_buf;
    uint8_t *audio_buf1;
    unsigned int audio_buf_size; /* in bytes */
    int audio_buf_index; /* in bytes */
} GroovePlayerPrivate;
