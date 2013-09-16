/* read or update metadata in a media file */

#include "groove.h"
#include <stdio.h>
#include <string.h>

static int usage(char *exe) {
    fprintf(stderr, "Usage: %s <file> [--update key value] [--delete key]\n"
            "Repeat --update and --delete as many times as you need to.\n", exe);
    return 1;
}

int main(int argc, char * argv[]) {
    // parse arguments
    char *exe = argv[0];

    if (argc < 2)
        return usage(exe);

    char * filename = argv[1];
    groove_set_logging(GROOVE_LOG_INFO);
    GrooveFile * file = groove_open(filename);
    if (!file) {
        fprintf(stderr, "error opening file\n");
        return 1;
    }

    for (int i = 2; i < argc; i += 1) {
        char * arg = argv[i];
        if (strcmp("--update", arg) == 0) {
            if (i + 2 >= argc) {
                groove_close(file);
                fprintf(stderr, "--update requires 2 arguments");
                return usage(exe);
            }
            char *key = argv[++i];
            char *value = argv[++i];
            groove_file_metadata_set(file, key, value, 0);
        } else if (strcmp("--delete", arg) == 0) {
            if (i + 1 >= argc) {
                groove_close(file);
                fprintf(stderr, "--delete requires 1 argument");
                return usage(exe);
            }
            char *key = argv[++i];
            groove_file_metadata_set(file, key, NULL, 0);
        } else {
            groove_close(file);
            return usage(exe);
        }
    }
    GrooveTag *tag = NULL;
    while ((tag = groove_file_metadata_get(file, "", tag, 0)))
        printf("%s=%s\n", groove_tag_key(tag), groove_tag_value(tag));
    if (file->dirty && groove_file_save(file) < 0)
        fprintf(stderr, "error saving file\n");
    groove_close(file);
    return 0;
}
