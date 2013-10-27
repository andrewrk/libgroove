#include "buffer.h"

#include <libavutil/mem.h>

void groove_buffer_ref(GrooveBuffer *buffer) {
    GrooveBufferPrivate *b = buffer->internals;

    SDL_LockMutex(b->mutex);
    b->ref_count += 1;
    SDL_UnlockMutex(b->mutex);
}

void groove_buffer_unref(GrooveBuffer *buffer) {
    if (!buffer)
        return;

    GrooveBufferPrivate *b = buffer->internals;

    SDL_LockMutex(b->mutex);
    b->ref_count -= 1;
    int free = b->ref_count == 0;
    SDL_UnlockMutex(b->mutex);

    if (free) {
        SDL_DestroyMutex(b->mutex);
        if (b->is_packet && b->packet) {
            av_free_packet(b->packet);
            av_free(b->packet);
        } else if (b->frame) {
            av_frame_free(&b->frame);
        }
        av_free(b);
        av_free(buffer);
    }

}
