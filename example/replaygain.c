/* replaygain scanner */

#include "groove.h"
#include <stdio.h>
#include <stdlib.h>

static GrooveReplayGainScan * scan;

static int scan_dir(char *dir) {

}

int main(int argc, char * argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s dir1 dir2 ...\n", argv[0]);
        return 1;
    }

    // 1 pass to read through all the metadata and organize files into albums


    // iterate by album and perform scanning

}
