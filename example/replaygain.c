/* replaygain scanner */

#include "groove.h"
#include <stdio.h>

static void progress_cb(void *userdata, double amount) {
    int percent = amount * 100;
    fprintf(stderr, "\rfile progress: %d%%   ", percent);
}

static void complete_cb(void *userdata, double gain, double peak) {
    GrooveFile *file = userdata;
    fprintf(stderr, "\nfile complete: %s\n", file->filename);
    fprintf(stderr, "suggested gain: %.2f dB, sample peak: %f\n", gain, peak);
}

int main(int argc, char * argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s file1 file2 ...\n", argv[0]);
        return 1;
    }

    groove_init();
    groove_set_logging(GROOVE_LOG_INFO);

    GrooveReplayGainScan * scan = groove_replaygainscan_create();
    if (!scan) {
        fprintf(stderr, "Unable to create replaygain scan\n");
        return 1;
    }
    for (int i = 1; i < argc; i += 1) {
        char * filename = argv[i];
        // TODO: after de-special-casing replaygain scan, free these files.
        GrooveFile * file = groove_file_open(filename);
        if (!file) {
            fprintf(stderr, "Unable to open %s\n", filename);
            continue;
        }
        groove_replaygainscan_add(scan, file, file);
    }
    groove_set_logging(GROOVE_LOG_QUIET);

    scan->file_progress = progress_cb;
    scan->file_complete = complete_cb;
    scan->progress_interval = 20.0;

    double gain, peak;
    int err = groove_replaygainscan_exec(scan, &gain, &peak);
    groove_replaygainscan_destroy(scan);

    if (err < 0) {
        fprintf(stderr, "Error starting scan.\n");
        return 1;
    }

    fprintf(stderr, "\nAll files complete.\n");
    fprintf(stderr, "suggested gain: %.2f dB, sample peak: %f\n", gain, peak);

    return 0;
}
