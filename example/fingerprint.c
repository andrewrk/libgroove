/* compute the acoustid of a list of songs */

#include <groovefingerprinter/fingerprinter.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char * argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s file1 file2 ...\n", argv[0]);
        return 1;
    }

    groove_init();
    atexit(groove_finish);
    groove_set_logging(GROOVE_LOG_INFO);

    struct GroovePlaylist *playlist = groove_playlist_create();

    for (int i = 1; i < argc; i += 1) {
        char * filename = argv[i];
        struct GrooveFile * file = groove_file_open(filename);
        if (!file) {
            fprintf(stderr, "Unable to open %s\n", filename);
            continue;
        }
        groove_playlist_insert(playlist, file, 1.0, NULL);
    }

    struct GrooveFingerprinter *printer = groove_fingerprinter_create();
    groove_fingerprinter_attach(printer, playlist);

    struct GrooveFingerprinterInfo info;
    while (groove_fingerprinter_info_get(printer, &info, 1) == 1) {
        if (info.item) {
            fprintf(stdout, "\nduration: %f: %s\n",
                    info.duration,
                    info.item->file->filename);
            fprintf(stdout, "%s\n", info.fingerprint);
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
