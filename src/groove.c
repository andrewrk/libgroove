#include "groove.h"
#include <stdio.h>

void groove_read_file(GrooveFile * gf) {
    
}

GrooveFile * groove_open(char* filename) {
    GrooveFile * gf = calloc(sizeof(GrooveFile));
    gf->priv.file = fopen(filename, "rb+");
    groove_read_file(gf);
    return gf;
}

void groove_close(GrooveFile * gf) {
    fclose(gf->priv.file);
    free(gf);
}
