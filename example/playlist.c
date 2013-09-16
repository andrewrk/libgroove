/* use groove to play several files in a row and then exit */

#include "groove.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static void print_now_playing(GroovePlayer *player) {
    GrooveTag *artist_tag = groove_file_metadata_get(player->queue_head->file,
            "artist", NULL, 0);
    GrooveTag *title_tag = groove_file_metadata_get(player->queue_head->file,
            "title", NULL, 0);
    if (artist_tag && title_tag) {
        printf("Now playing: %s - %s\n", groove_tag_value(artist_tag),
                groove_tag_value(title_tag));
    } else {
        printf("Now playing: %s\n", groove_file_filename(player->queue_head->file));
    }
}

int main(int argc, char * argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s file1 file2 ...\n", argv[0]);
        return 1;
    }
    if (groove_init() < 0) {
        fprintf(stderr, "error initializing libgroove\n");
        return 1;
    }
    groove_set_logging(GROOVE_LOG_INFO);
    GroovePlayer *player = groove_create_player();
    for (int i = 1; i < argc; i += 1) {
        char * filename = argv[i];
        GrooveFile * file = groove_open(filename);
        if (!file) {
            fprintf(stderr, "Not queuing %s\n", filename);
            continue;
        }
        groove_player_queue(player, file);
    }
    groove_player_play(player);

    GroovePlayerEvent event;
    while (groove_player_event_wait(player, &event) >= 0) {
        switch (event.type) {
        case GROOVE_PLAYER_EVENT_NOWPLAYING:
            if (groove_player_count(player) == 0) {
                printf("done\n");
                groove_destroy_player(player);
                return 0;
            }
            print_now_playing(player);
            break;
        }
    }
    return 1;
}
