/* transcode a file */

#include "groove.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int usage(char *arg0) {
    fprintf(stderr, "Usage: %s inputfile outputfile [--bitrate 320] [--format name] [--codec name] [--mime mimetype]\n", arg0);
    return 1;
}

int main(int argc, char * argv[]) {
    // arg parsing
    int bit_rate_k = 320;
    char *format = NULL;
    char *codec = NULL;
    char *mime = NULL;

    char *input_file_name = NULL;
    char *output_file_name = NULL;

    for (int i = 1; i < argc; i += 1) {
        char *arg = argv[i];
        if (arg[0] == '-' && arg[1] == '-') {
            arg += 2;
            if (i + 1 >= argc) {
                return usage(argv[0]);
            } else if (strcmp(arg, "bitrate") == 0) {
                bit_rate_k = atoi(argv[i++]);
            } else if (strcmp(arg, "format") == 0) {
                format = argv[i++];
            } else if (strcmp(arg, "codec") == 0) {
                codec = argv[i++];
            } else if (strcmp(arg, "mime") == 0) {
                mime = argv[i++];
            } else {
                return usage(argv[0]);
            }
        } else if (!input_file_name) {
            input_file_name = arg;
        } else if (!output_file_name) {
            output_file_name = arg;
        } else {
            return usage(argv[0]);
        }
    }
    if (!input_file_name || !output_file_name)
        return usage(argv[0]);

    // args are parsed. let's begin
    int bit_rate = bit_rate_k * 1000;

    groove_init();
    groove_set_logging(GROOVE_LOG_INFO);

    GrooveFile * file = groove_file_open(input_file_name);
    if (!file) {
        fprintf(stderr, "Error opening input file %s\n", input_file_name);
        return 1;
    }

    GroovePlaylist *playlist = groove_playlist_create();
    GrooveEncoder *encoder = groove_encoder_create();
    encoder->bit_rate = bit_rate;
    encoder->format_short_name = format;
    encoder->codec_short_name = codec;
    encoder->filename = output_file_name;
    encoder->mime_type = mime;
    groove_file_audio_format(file, &encoder->target_audio_format);
    if (groove_encoder_attach(encoder, playlist) < 0) {
        fprintf(stderr, "error attaching encoder\n");
        return 1;
    }

    groove_playlist_insert(playlist, file, 1.0, NULL);

    FILE *f = fopen(output_file_name, "wb");
    if (!f) {
        fprintf(stderr, "Error opening output file %s\n", output_file_name);
        return 1;
    }

    GrooveBuffer *buffer;

    while (groove_encoder_get_buffer(encoder, &buffer, 1) == GROOVE_BUFFER_YES) {
        fwrite(buffer->data[0], 1, buffer->size, f);
        groove_buffer_unref(buffer);
    }

    fclose(f);

    groove_encoder_detach(encoder);
    groove_encoder_destroy(encoder);
    groove_playlist_destroy(playlist);

    return 0;
}
