/* replaygain scanner */

#include "groove.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <dirent.h>
#include <string.h>

static GrooveReplayGainScan * scan;

static int scan_dir(char *dirname) {
    DIR *dp = opendir(dirname);

    if (!dp)
        return -1;

    struct dirent *ep;
    char buf[2048] = {0};
    strcpy(buf, dirname);
    strcat(buf, "/");
    size_t base_len = strlen(buf);
    char *base_end = buf + base_len;
    while ((ep = readdir(dp))) {
        // ignore . and ..
        if (strcmp(ep->d_name, ".") == 0 || strcmp(ep->d_name, "..") == 0)
            continue;
        strcpy(base_end, ep->d_name);
        if (scan_dir(buf) < 0) {
            fprintf(stderr, "found %s\n", buf);
            groove_replaygainscan_add(scan, buf);
        }
    }
    closedir(dp);
    return 0;
}

int main(int argc, char * argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s dir1 dir2 ...\n", argv[0]);
        return 1;
    }

    groove_set_logging(1);
    scan = groove_create_replaygainscan();
    if (!scan) {
        fprintf(stderr, "Unable to create replaygain scan\n");
        return 1;
    }
    // add all files to the scan
    for (int i = 1; i < argc; i += 1) {
        char * dirname = argv[i];
        if (scan_dir(dirname) < 0)
            fprintf(stderr, "Error reading dir: %s\n", dirname);
    }
    if (groove_replaygainscan_exec(scan) < 0) {
        groove_replaygainscan_destroy(scan);
        fprintf(stderr, "Error starting scan.\n");
        return 1;
    }
    GrooveRgEvent event;
    while (groove_replaygainscan_event_wait(scan, &event) >= 0) {
        switch (event.type) {
        case GROOVE_RG_EVENT_PROGRESS:
            fprintf(stderr, "\rmetadata %d/%d scanning %d/%d update %d/%d                  ",
                    event.rg_progress.metadata_current, event.rg_progress.metadata_total,
                    event.rg_progress.scanning_current, event.rg_progress.scanning_total,
                    event.rg_progress.update_current, event.rg_progress.update_total);
            fflush(stderr);
            break;
        case GROOVE_RG_EVENT_COMPLETE:
            fprintf(stderr, "\nscan complete.\n");
            return 0;
        }
    }
}
