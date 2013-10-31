/* play several files in a row and then exit */

#include "groove.h"
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

    if (!playlist) {
        fprintf(stderr, "Error creating playlist.\n");
        return 1;
    }

    struct GroovePlayer *player = groove_player_create();
    groove_player_attach(player, playlist);

    for (int i = 1; i < argc; i += 1) {
        char * filename = argv[i];
        struct GrooveFile * file = groove_file_open(filename);
        if (!file) {
            fprintf(stderr, "Not queuing %s\n", filename);
            continue;
        }
        groove_playlist_insert(playlist, file, 1.0, NULL);
    }
    groove_playlist_play(playlist);

    union GroovePlayerEvent event;
    struct GroovePlaylistItem *item;
    while (groove_player_event_get(player, &event, 1) >= 0) {
        switch (event.type) {
        case GROOVE_EVENT_BUFFERUNDERRUN:
            fprintf(stderr, "buffer underrun\n");
            break;
        case GROOVE_EVENT_NOWPLAYING:
            groove_player_position(player, &item, NULL);
            if (!item) {
                printf("done\n");
                item = playlist->head;
                while (item) {
                    struct GrooveFile *file = item->file;
                    struct GroovePlaylistItem *next = item->next;
                    groove_playlist_remove(playlist, item);
                    groove_file_close(file);
                    item = next;
                }
                groove_player_detach(player);
                groove_player_destroy(player);
                groove_playlist_destroy(playlist);
                return 0;
            }
            struct GrooveTag *artist_tag = groove_file_metadata_get(item->file, "artist", NULL, 0);
            struct GrooveTag *title_tag = groove_file_metadata_get(item->file, "title", NULL, 0);
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
