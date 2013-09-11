/* display the short name of a media file */

#include "groove.h"
#include <stdio.h>

int main(int argc, char * argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <file>\n", argv[0]);
        return 1;
    }
    char * filename = argv[1];
    GrooveFile * file = groove_open(filename);
    printf("%s\n", groove_file_short_names(file));
    groove_close(file);
}
