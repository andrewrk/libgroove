#include "groove.h"

#include <libavutil/channel_layout.h>

void groove_buffer_ref(GrooveBuffer *buffer) {

}

void groove_buffer_unref(GrooveBuffer *buffer) {
    if (!buffer)
        return;

    // TODO this should probably be unref... make sure that will wokr
    av_frame_free(&frame);
}
