/* play several files in a row and then exit */

#include <grooveplayer/player.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(const char *exe) {
    fprintf(stderr, "Usage: %s [--volume 1.0] file1 file2 ...\n", exe);
    return 1;
}

int main(int argc, char * argv[]) {
    // parse arguments
    const char *exe = argv[0];
    if (argc < 2) return usage(exe);

    groove_init();
    atexit(groove_finish);
    groove_set_logging(GROOVE_LOG_INFO);
    struct GroovePlaylist *playlist = groove_playlist_create();

    if (!playlist) {
        fprintf(stderr, "Error creating playlist.\n");
        return 1;
    }

    struct GroovePlayer *player = groove_player_create();

    for (int i = 1; i < argc; i += 1) {
        char *arg = argv[i];
        if (arg[0] == '-' && arg[1] == '-') {
            arg += 2;
            if (strcmp(arg, "dummy") == 0) {
                player->device_index = GROOVE_PLAYER_DUMMY_DEVICE;
            } else if (i + 1 >= argc) {
                return usage(exe);
            } else if (strcmp(arg, "volume") == 0) {
                double volume = atof(argv[++i]);
                groove_playlist_set_gain(playlist, volume);
            } else {
                return usage(exe);
            }
        } else {
            struct GrooveFile * file = groove_file_open(arg);
            if (!file) {
                fprintf(stderr, "Not queuing %s\n", arg);
                continue;
            }
            groove_playlist_insert(playlist, file, 1.0, 1.0, NULL);
        }
    }
    groove_player_attach(player, playlist);

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
