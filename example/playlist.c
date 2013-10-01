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
    GroovePlayer *player = groove_create_player();
    for (int i = 1; i < argc; i += 1) {
        char * filename = argv[i];
        GrooveFile * file = groove_open(filename);
        if (!file) {
            fprintf(stderr, "Not queuing %s\n", filename);
            continue;
        }
        groove_player_insert(player, file, 1.0, NULL);
    }
    groove_player_play(player);

    GroovePlayerEvent event;
    GroovePlaylistItem *item;
    while (groove_player_event_wait(player, &event) >= 0) {
        switch (event.type) {
        case GROOVE_PLAYER_EVENT_BUFFERUNDERRUN:
            printf("buffer underrun\n");
            break;
        case GROOVE_PLAYER_EVENT_NOWPLAYING:
            groove_player_position(player, &item, NULL);
            if (!item) {
                printf("done\n");
                item = player->playlist_head;
                while (item) {
                    GrooveFile *file = item->file;
                    GroovePlaylistItem *next = item->next;
                    groove_player_remove(player, item);
                    groove_close(file);
                    item = next;
                }
                groove_destroy_player(player);
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
