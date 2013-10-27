#ifndef __BUFFER_H__
#define __BUFFER_H__

#include "groove.h"

#include <libavutil/frame.h>
#include <libavcodec/avcodec.h>
#include <SDL2/SDL_thread.h>

typedef struct GrooveBufferPrivate {
    AVFrame *frame;
    AVPacket *packet;
    int is_packet;
    int ref_count;
    SDL_mutex *mutex;
} GrooveBufferPrivate;

#endif /* __BUFFER_H__ */
