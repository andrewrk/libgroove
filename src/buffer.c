/*
 * Copyright (c) 2013 Andrew Kelley
 *
 * This file is part of libgroove, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "buffer.h"

#include <libavutil/mem.h>

void groove_buffer_ref(struct GrooveBuffer *buffer) {
    struct GrooveBufferPrivate *b = (struct GrooveBufferPrivate *) buffer;

    SDL_LockMutex(b->mutex);
    b->ref_count += 1;
    SDL_UnlockMutex(b->mutex);
}

void groove_buffer_unref(struct GrooveBuffer *buffer) {
    if (!buffer)
        return;

    struct GrooveBufferPrivate *b = (struct GrooveBufferPrivate *) buffer;

    SDL_LockMutex(b->mutex);
    b->ref_count -= 1;
    int free = b->ref_count == 0;
    SDL_UnlockMutex(b->mutex);

    if (free) {
        SDL_DestroyMutex(b->mutex);
        if (b->is_packet && b->data) {
            av_free(b->data);
        } else if (b->frame) {
            av_frame_free(&b->frame);
        }
        av_free(b);
    }
}
