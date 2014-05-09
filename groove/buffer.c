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

    pthread_mutex_lock(&b->mutex);
    b->ref_count += 1;
    pthread_mutex_unlock(&b->mutex);
}

void groove_buffer_unref(struct GrooveBuffer *buffer) {
    if (!buffer)
        return;

    struct GrooveBufferPrivate *b = (struct GrooveBufferPrivate *) buffer;

    pthread_mutex_lock(&b->mutex);
    b->ref_count -= 1;
    int free = b->ref_count == 0;
    pthread_mutex_unlock(&b->mutex);

    if (free) {
        pthread_mutex_destroy(&b->mutex);
        if (b->is_packet && b->data) {
            av_free(b->data);
        } else if (b->frame) {
            av_frame_free(&b->frame);
        }
        av_free(b);
    }
}

uint64_t groove_buffer_pts(struct GrooveBuffer *buffer) {
    struct GrooveBufferPrivate *b = (struct GrooveBufferPrivate *) buffer;
    return b->pts;
}
