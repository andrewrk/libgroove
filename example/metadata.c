/* read or update metadata in a media file */

#include "groove.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int usage(char *exe) {
    fprintf(stderr, "Usage: %s <file> [--update key value] [--delete key]\n"
            "Repeat --update and --delete as many times as you need to.\n", exe);
    return 1;
}

int main(int argc, char * argv[]) {
    /* parse arguments */
    char *exe = argv[0];
    char *filename;
    struct GrooveFile *file;
    int i;
    char *arg;
    char *key;
    char *value;
    struct GrooveTag *tag;

    if (argc < 2)
        return usage(exe);

    printf("Using libgroove v%s\n", groove_version());

    filename = argv[1];
    groove_init();
    atexit(groove_finish);
    groove_set_logging(GROOVE_LOG_INFO);
    file = groove_file_open(filename);
    if (!file) {
        fprintf(stderr, "error opening file\n");
        return 1;
    }

    for (i = 2; i < argc; i += 1) {
        arg = argv[i];
        if (strcmp("--update", arg) == 0) {
            if (i + 2 >= argc) {
                groove_file_close(file);
                fprintf(stderr, "--update requires 2 arguments");
                return usage(exe);
            }
            key = argv[++i];
            value = argv[++i];
            groove_file_metadata_set(file, key, value, 0);
        } else if (strcmp("--delete", arg) == 0) {
            if (i + 1 >= argc) {
                groove_file_close(file);
                fprintf(stderr, "--delete requires 1 argument");
                return usage(exe);
            }
            key = argv[++i];
            groove_file_metadata_set(file, key, NULL, 0);
        } else {
            groove_file_close(file);
            return usage(exe);
        }
    }
    tag = NULL;
    printf("duration=%f\n", groove_file_duration(file));
    while ((tag = groove_file_metadata_get(file, "", tag, 0)))
        printf("%s=%s\n", groove_tag_key(tag), groove_tag_value(tag));
    if (file->dirty && groove_file_save(file) < 0)
        fprintf(stderr, "error saving file\n");
    groove_file_close(file);
    return 0;
}
