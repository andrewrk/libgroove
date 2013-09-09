/* use groove to identify the type of file */

#include "groove.h"

int main(int argc, char * argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <file>\n", argv[0]);
        return 1;
    }
    char * filename = argv[1];
    GrooveFile * g = groove_open(filename);
    printf("%s\n", g->recommended_extension->data);
    groove_close(filename);
}
