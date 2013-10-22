/* play several files in a row and then exit */

#include "groove.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char * argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s file1 file2 ...\n", argv[0]);
        return 1;
    }
    groove_init();
    groove_set_logging(GROOVE_LOG_INFO);
    GroovePlaylist *playlist = groove_playlist_create();

    if (!playlist) {
        fprintf(stderr, "Error creating playlist.\n");
        return 1;
    }

    GrooveDeviceSink *device_sink = groove_device_sink_create();
    groove_device_sink_attach(device_sink, playlist);

    for (int i = 1; i < argc; i += 1) {
        char * filename = argv[i];
        GrooveFile * file = groove_file_open(filename);
        if (!file) {
            fprintf(stderr, "Not queuing %s\n", filename);
            continue;
        }
        groove_playlist_insert(playlist, file, 1.0, NULL);
    }
    groove_playlist_play(playlist);

    GrooveEvent event;
    GroovePlaylistItem *item;
    while (groove_device_sink_event_get(device_sink, &event, 1) >= 0) {
        switch (event.type) {
        case GROOVE_EVENT_BUFFERUNDERRUN:
            fprintf(stderr, "buffer underrun\n");
            break;
        case GROOVE_EVENT_NOWPLAYING:
            groove_device_sink_position(device_sink, &item, NULL);
            if (!item) {
                printf("done\n");
                item = playlist->head;
                while (item) {
                    GrooveFile *file = item->file;
                    GroovePlaylistItem *next = item->next;
                    groove_playlist_remove(playlist, item);
                    groove_file_close(file);
                    item = next;
                }
                groove_device_sink_detach(device_sink);
                groove_device_sink_destroy(device_sink);
                groove_playlist_destroy(playlist);
                return 0;
            }
            GrooveTag *artist_tag = groove_file_metadata_get(item->file, "artist", NULL, 0);
            GrooveTag *title_tag = groove_file_metadata_get(item->file, "title", NULL, 0);
            if (artist_tag && title_tag) {
                printf("Now playing: %s - %s\n", groove_tag_value(artist_tag),
                        groove_tag_value(title_tag));
            } else {
                printf("Now playing: %s\n", item->file->filename);
            }
            break;
        }
    }
    return 1;
}
