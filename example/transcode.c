/* transcode one or more files into one output file */

#include "groove.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int usage(char *arg0) {
    fprintf(stderr, "Usage: %s file1 [file2 ...] --output outputfile [--bitrate 320] [--format name] [--codec name] [--mime mimetype]\n", arg0);
    return 1;
}

int main(int argc, char * argv[]) {
    // arg parsing
    int bit_rate_k = 320;
    char *format = NULL;
    char *codec = NULL;
    char *mime = NULL;

    char *output_file_name = NULL;

    groove_init();
    atexit(groove_finish);
    groove_set_logging(GROOVE_LOG_INFO);
    struct GroovePlaylist *playlist = groove_playlist_create();

    for (int i = 1; i < argc; i += 1) {
        char *arg = argv[i];
        if (arg[0] == '-' && arg[1] == '-') {
            arg += 2;
            if (i + 1 >= argc) {
                return usage(argv[0]);
            } else if (strcmp(arg, "bitrate") == 0) {
                bit_rate_k = atoi(argv[++i]);
            } else if (strcmp(arg, "format") == 0) {
                format = argv[++i];
            } else if (strcmp(arg, "codec") == 0) {
                codec = argv[++i];
            } else if (strcmp(arg, "mime") == 0) {
                mime = argv[++i];
            } else if (strcmp(arg, "output") == 0) {
                output_file_name = argv[++i];
            } else {
                return usage(argv[0]);
            }
        } else {
            struct GrooveFile * file = groove_file_open(arg);
            if (!file) {
                fprintf(stderr, "Error opening input file %s\n", arg);
                return 1;
            }
            groove_playlist_insert(playlist, file, 1.0, NULL);
        }
    }
    if (!output_file_name)
        return usage(argv[0]);

    struct GrooveEncoder *encoder = groove_encoder_create();
    encoder->bit_rate = bit_rate_k * 1000;
    encoder->format_short_name = format;
    encoder->codec_short_name = codec;
    encoder->filename = output_file_name;
    encoder->mime_type = mime;
    if (groove_playlist_count(playlist) == 1) {
        groove_file_audio_format(playlist->head->file, &encoder->target_audio_format);

        // copy metadata
        struct GrooveTag *tag = NULL;
        while((tag = groove_file_metadata_get(playlist->head->file, "", tag, 0))) {
            groove_encoder_metadata_set(encoder, groove_tag_key(tag), groove_tag_value(tag), 0);
        }
    }

    if (groove_encoder_attach(encoder, playlist) < 0) {
        fprintf(stderr, "error attaching encoder\n");
        return 1;
    }

    FILE *f = fopen(output_file_name, "wb");
    if (!f) {
        fprintf(stderr, "Error opening output file %s\n", output_file_name);
        return 1;
    }

    struct GrooveBuffer *buffer;

    while (groove_encoder_get_buffer(encoder, &buffer, 1) == GROOVE_BUFFER_YES) {
        fwrite(buffer->data[0], 1, buffer->size, f);
        groove_buffer_unref(buffer);
    }

    fclose(f);

    groove_encoder_detach(encoder);
    groove_encoder_destroy(encoder);

    struct GroovePlaylistItem *item = playlist->head;
    while (item) {
        struct GrooveFile *file = item->file;
        struct GroovePlaylistItem *next = item->next;
        groove_playlist_remove(playlist, item);
        groove_file_close(file);
        item = next;
    }
    groove_playlist_destroy(playlist);

    return 0;
}
