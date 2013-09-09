#ifndef __GROOVE_H__
#define __GROOVE_H__

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

#include <groove_private.h>
#include <stdint.h>

typedef struct {
    int length;
    uint8_t * data;
} GrooveUtf8;

typedef struct {
    // the recommended extension to use for files of this type
    GrooveUtf8 * recommended_extension;
    int channel_count;
    int sample_rate; // in hz. example: 44100
    int sample_count;

    GroovePrivate priv;
} GrooveFile;

typedef struct {
    GrooveQueue * prev;
    GrooveFile * file;
    GrooveQueue * next;
} GrooveQueue;

#define GROOVE_STATE_STOPPED 0
#define GROOVE_STATE_PLAYING 1
#define GROOVE_STATE_PAUSED 2

typedef struct {
    int state;
    double volume;
    GrooveQueue * queue;
} GroovePlayer;

GrooveFile * groove_open(char* filename);
void groove_close(GrooveFile * g);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __GROOVE_H__ */
