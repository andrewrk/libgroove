#ifndef __BUFFER_H__
#define __BUFFER_H__

#include "groove.h"

#include <libavutil/frame.h>
#include <libavcodec/avcodec.h>
#include <SDL2/SDL_thread.h>

typedef struct GrooveBufferPrivate {
    AVFrame *frame;
    int is_packet;
    int ref_count;
    SDL_mutex *mutex;
    // used for when is_packet is true
    // GrooveBuffer::data[0] will point to this
    uint8_t *data;
} GrooveBufferPrivate;

#endif /* __BUFFER_H__ */
