#include "file.h"

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
    ebur128_state **ebur_states;
    GroovePlayer *player;
    GrooveSink *sink;
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

GrooveReplayGainScan * groove_replaygainscan_create() {
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

int groove_replaygainscan_exec(GrooveReplayGainScan *scan, double *scan_gain, double *scan_peak) {
    GrooveReplayGainScanPrivate *s = scan->internals;

    GrooveAudioFormat audio_format;
    audio_format.sample_rate = 44100;
    audio_format.channel_layout = GROOVE_CH_LAYOUT_STEREO;
    audio_format.sample_fmt = GROOVE_SAMPLE_FMT_DBL;

    s->player = groove_player_create();
    GrooveSink *sink = groove_player_attach_sink(player, &audio_format);

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
        GrooveFilePrivate *f = file->internals;
        void *userdata = node->userdata;
        av_freep(&node);

        // flush buffers and seek to 0
        f->seek_pos = 0;
        f->seek_flush = 1;

        groove_player_clear(s->player);
        groove_player_insert(s->player, file, 1.0, NULL);
        double seconds_passed = 0;
        double prev_clock = 0;
        double duration = av_q2d(f->audio_st->time_base) * f->audio_st->duration;


        GrooveBuffer *buffer;
        while (groove_player_sink_get_buffer(s->player, s->sink, &buffer, 1) == GROOVE_BUFFER_YES) {
            // process buffer
            ebur128_state *st = s->ebur_states[s->current_index];
            ebur128_add_frames_double(st, (double*)buffer->data[0], buffer->sample_count);
            groove_buffer_unref(buffer);

            // handle progress
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
    groove_player_destroy(s->player);
    filestack_cleanup(&s->file_item);
    av_free(s);
    av_free(scan);
}
