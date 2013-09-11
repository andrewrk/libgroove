/* read or update metadata in a media file */

#include "groove.h"
#include <stdio.h>

int main(int argc, char * argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <file>\n", argv[0]);
        return 1;
    }
    char * filename = argv[1];
    GrooveFile * file = groove_open(filename);
    GrooveTag *tag;
    while ((tag = groove_file_metadata_get(file, "", tag, GROOVE_TAG_IGNORE_SUFFIX)))
        printf("%s=%s\n", groove_tag_key(tag), groove_tag_value(tag));
    groove_close(file);
    return 0;
}
