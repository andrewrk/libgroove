/*
 * Copyright (c) 2013 Andrew Kelley
 *
 * This file is part of libgroove, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "buffer.hpp"
#include "ffmpeg.hpp"
#include "util.hpp"

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
    int is_free = b->ref_count == 0;
    pthread_mutex_unlock(&b->mutex);

    if (is_free) {
        pthread_mutex_destroy(&b->mutex);
        if (b->is_packet && b->data) {
            deallocate(b->data);
        } else if (b->frame) {
            av_frame_free(&b->frame);
        }
        deallocate(b);
    }
}
