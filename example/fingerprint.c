/* compute the acoustid of a list of songs */

#include <groove/fingerprinter.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

static int usage(char *arg0) {
    fprintf(stderr, "Usage: %s [--raw] file1 file2 ...\n", arg0);
    return 1;
}

int main(int argc, char * argv[]) {
    if (argc < 2)
        return usage(argv[0]);

    groove_init();
    atexit(groove_finish);
    groove_set_logging(GROOVE_LOG_INFO);

    struct GroovePlaylist *playlist = groove_playlist_create();

    int raw = 0;

    for (int i = 1; i < argc; i += 1) {
        char *arg = argv[i];
        if (arg[0] == '-' && arg[1] == '-') {
            arg += 2;
            if (strcmp(arg, "raw") == 0) {
                raw = 1;
            } else {
                return usage(argv[0]);
            }
        } else {
            struct GrooveFile * file = groove_file_open(arg);
            if (!file) {
                fprintf(stderr, "Unable to open %s\n", arg);
                continue;
            }
            groove_playlist_insert(playlist, file, 1.0, 1.0, NULL);
        }
    }

    struct GrooveFingerprinter *printer = groove_fingerprinter_create();
    groove_fingerprinter_attach(printer, playlist);

    struct GrooveFingerprinterInfo info;
    while (groove_fingerprinter_info_get(printer, &info, 1) == 1) {
        if (info.item) {
            printf("\nduration: %f: %s\n",
                    info.duration,
                    info.item->file->filename);
            if (raw) {
                for (int i = 0; i < info.fingerprint_size; i += 1) {
                    printf("%"PRId32"\n", info.fingerprint[i]);
                }
            } else {
                char *encoded_fp;
                if (groove_fingerprinter_encode(info.fingerprint,
                            info.fingerprint_size, &encoded_fp) < 0)
                {
                    fprintf(stderr, "Unable to encode fingerprint\n");
                } else {
                    printf("%s\n", encoded_fp);
                    groove_fingerprinter_dealloc(encoded_fp);
                }
            }
            groove_fingerprinter_free_info(&info);
        } else {
            break;
        }
    }

    struct GroovePlaylistItem *item = playlist->head;
    while (item) {
        struct GrooveFile *file = item->file;
        struct GroovePlaylistItem *next = item->next;
        groove_playlist_remove(playlist, item);
        groove_file_close(file);
        item = next;
    }

    groove_fingerprinter_detach(printer);
    groove_fingerprinter_destroy(printer);
    groove_playlist_destroy(playlist);

    return 0;
}
