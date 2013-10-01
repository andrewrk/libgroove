#include "groove.h"
#include "decode.h"

#include <ebur128.h>

#include <libavutil/channel_layout.h>

typedef struct FileStackItem {
    void *userdata;
    GrooveFile *file;
    struct FileStackItem *next;
} FileStackItem;

typedef struct GrooveReplayGainScanPrivate {
    // the stack of files to be processed
    FileStackItem *file_item;
    int file_count;
    int current_index;
    GrooveDecodeContext decode_ctx;
    ebur128_state **ebur_states;
} GrooveReplayGainScanPrivate;

static void filestack_push(FileStackItem **stack, FileStackItem *item) {
    item->next = *stack;
    *stack = item;
}

static FileStackItem * filestack_pop(FileStackItem **stack) {
    FileStackItem *popped = *stack;
    *stack = (*stack)->next;
    return popped;
}

static double clamp_rg(double x) {
    if (x < -51.0) return -51.0;
    else if (x > 51.0) return 51.0;
    else return x;
}

static double loudness_to_replaygain(double loudness) {
    // loudness is in LUFS. The goal is for the loudness to be -23 LUFS.
    // however replaygain is a suggestion of how many dB to adjust the gain
    // so that it equals -18 dB.
    // 1 LUFS = 1 dB

    return clamp_rg(-18.0 - loudness);
}

static void cleanup_ebur(GrooveReplayGainScan *scan) {
    GrooveReplayGainScanPrivate *s = scan->internals;
    if (!s->ebur_states) return;
    for (int i = 0; i < s->file_count; i += 1) {
        if (s->ebur_states[i])
            ebur128_destroy(&s->ebur_states[i]);
    }
    av_freep(&s->ebur_states);
}

GrooveReplayGainScan * groove_create_replaygainscan() {
    GrooveReplayGainScan * scan = av_mallocz(sizeof(GrooveReplayGainScan));
    GrooveReplayGainScanPrivate * s = av_mallocz(sizeof(GrooveReplayGainScanPrivate));
    if (!scan || !s) {
        av_free(scan);
        av_free(s);
        av_log(NULL, AV_LOG_ERROR, "error creating replaygain scan: out of memory\n");
        return NULL;
    }
    scan->internals = s;
    return scan;
}

int groove_replaygainscan_add(GrooveReplayGainScan *scan, GrooveFile *file, void *userdata) {
    GrooveReplayGainScanPrivate *s = scan->internals;

    FileStackItem *item = av_mallocz(sizeof(FileStackItem));
    if (!item) {
        av_log(NULL, AV_LOG_ERROR, "error adding file to replaygain scan: out of memory\n");
        return -1;
    }
    item->file = file;
    item->userdata = userdata;
    filestack_push(&s->file_item, item);
    s->file_count += 1;
    return 0;
}

static int scan_buffer(GrooveDecodeContext *decode_ctx, AVFrame *frame) {
    GrooveReplayGainScan *scan = decode_ctx->callback_context;
    GrooveReplayGainScanPrivate *s = scan->internals;

    ebur128_state *st = s->ebur_states[s->current_index];
    ebur128_add_frames_double(st, (double*)frame->data[0], frame->nb_samples);

    av_frame_free(&frame);
    return 0;
}

int groove_replaygainscan_exec(GrooveReplayGainScan *scan, double *scan_gain, double *scan_peak) {
    GrooveReplayGainScanPrivate *s = scan->internals;
    s->decode_ctx.callback_context = scan;
    s->decode_ctx.buffer = scan_buffer;
    s->decode_ctx.dest_sample_rate = 44100;
    s->decode_ctx.dest_channel_layout = AV_CH_LAYOUT_STEREO;
    s->decode_ctx.dest_sample_fmt = AV_SAMPLE_FMT_DBL;

    if (groove_init_decode_ctx(&s->decode_ctx) < 0)
        return -1;
    s->decode_ctx.replaygain_mode = GROOVE_REPLAYGAINMODE_OFF;

    // init libebur128
    s->ebur_states = av_malloc(s->file_count * sizeof(ebur128_state*));
    if (!s->ebur_states) {
        av_log(NULL, AV_LOG_ERROR, "error initializing loudness scanner: out of memory\n");
        return -1;
    }
    int destroy = 0;
    for (int i = 0; i < s->file_count; i += 1) {
        s->ebur_states[i] = ebur128_init(2, 44100, EBUR128_MODE_SAMPLE_PEAK|EBUR128_MODE_I);
        destroy = destroy || !s->ebur_states[i];
    }
    if (destroy) {
        cleanup_ebur(scan);
        return -1;
    }

    FileStackItem *node = NULL;
    double album_peak = 0;
    double progress_interval = scan->progress_interval;
    while (s->file_item) {
        node = filestack_pop(&s->file_item);
        GrooveFile *file = node->file;
        void *userdata = node->userdata;
        av_freep(&node);

        // flush buffers and seek to 0
        GrooveFilePrivate *f = file->internals;
        f->seek_pos = 0;
        f->seek_flush = 1;
        double seconds_passed = 0;
        double prev_clock = 0;
        double duration = av_q2d(f->audio_st->time_base) * f->audio_st->duration;
        while (groove_decode(&s->decode_ctx, file) >= 0) {
            if (scan->file_progress) {
                seconds_passed += f->audio_clock - prev_clock;
                prev_clock = f->audio_clock;
                if (seconds_passed > progress_interval) {
                    double percent = f->audio_clock / duration;
                    seconds_passed -= progress_interval;
                    scan->file_progress(userdata, percent);
                    if (scan->abort_request) {
                        return 0;
                    }
                }
            }
        }

        // grab the loudness value and the peak value
        ebur128_state *st = s->ebur_states[s->current_index];
        double track_loudness;
        ebur128_loudness_global(st, &track_loudness);
        double track_peak = 0;
        ebur128_sample_peak(st, 0, &track_peak);
        double out;
        ebur128_sample_peak(st, 1, &out);
        track_peak = track_peak > out ? track_peak : out;
        album_peak = album_peak > track_peak ? album_peak : track_peak;

        scan->file_complete(userdata, loudness_to_replaygain(track_loudness), track_peak);
        if (scan->abort_request) {
            return 0;
        }
        s->current_index += 1;
    }

    double album_loudness;
    ebur128_loudness_global_multiple(s->ebur_states, s->file_count, &album_loudness);

    *scan_gain = loudness_to_replaygain(album_loudness);
    *scan_peak = album_peak;

    return 0;
}

static void filestack_cleanup(FileStackItem **stack) {
    while (*stack) {
        FileStackItem *node = filestack_pop(stack);
        av_free(node);
    }
    *stack = NULL;
}

void groove_replaygainscan_destroy(GrooveReplayGainScan *scan) {
    if (!scan)
        return;
    GrooveReplayGainScanPrivate *s = scan->internals;

    cleanup_ebur(scan);
    groove_cleanup_decode_ctx(&s->decode_ctx);
    filestack_cleanup(&s->file_item);
    av_free(s);
    av_free(scan);
}
