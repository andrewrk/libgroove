/* use groove to play several files in a row and then exit */

#include "groove.h"
#include <unistd.h>

int main(int argc, char * argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s file1 file2 ...\n", argv[0]);
        return 1;
    }
    groove_set_logging(true);
    GroovePlayer *player = groove_create_player();
    for (int i = 1; i < argc; i += 1) {
        char * filename = argv[i];
        GrooveFile * file = groove_open(filename);
        if (!file) {
            fprintf(stderr, "Error opening %s\n", filename);
            exit(1);
        }
        groove_player_queue(player, file);
    }
    groove_player_play(player);
    while (groove_player_count(player) > 0) {
        usleep(50000);
    }
    printf("done\n");
    return 0;
}
