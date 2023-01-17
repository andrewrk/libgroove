/*
 * Copyright (c) 2013 Andrew Kelley
 *
 * This file is part of libgroove, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#ifndef GROOVE_FINGERPRINTER_H_INCLUDED
#define GROOVE_FINGERPRINTER_H_INCLUDED

#include <groove/groove.h>

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

/* use this to find out the unique id of an audio track */

struct GrooveFingerprinterInfo {
    /* raw fingerprint. A fingerprint is an array of signed 32-bit integers. */
    int32_t *fingerprint;
    /* the number of 32-bit integers in the fingerprint array */
    int fingerprint_size;

    /* how many seconds long this song is */
    double duration;

    /* the playlist item that this info applies to.
     * When this is NULL this is the end-of-playlist sentinel and
     * other properties are undefined.
     */
    struct GroovePlaylistItem *item;
};

struct GrooveFingerprinter {
    /* maximum number of GrooveFingerprinterInfo items to store in this
     * fingerprinter's queue. this defaults to MAX_INT, meaning that
     * the fingerprinter will cause the decoder to decode the entire
     * playlist. if you want to instead, for example, obtain fingerprints
     * at the same time as playback, you might set this value to 1.
     */
    int info_queue_size;

    /* how big the sink buffer should be, in sample frames.
     * groove_fingerprinter_create defaults this to 8192
     */
    int sink_buffer_size;

    /* read-only. set when attached and cleared when detached */
    struct GroovePlaylist *playlist;
};

struct GrooveFingerprinter *groove_fingerprinter_create(void);
void groove_fingerprinter_destroy(struct GrooveFingerprinter *printer);

/* once you attach, you must detach before destroying the playlist */
int groove_fingerprinter_attach(struct GrooveFingerprinter *printer,
        struct GroovePlaylist *playlist);
int groove_fingerprinter_detach(struct GrooveFingerprinter *printer);

/* returns < 0 on error, 0 on aborted (block=1) or no info ready (block=0),
 * 1 on info returned.
 * When you get info you must free it with groove_fingerprinter_free_info.
 */
int groove_fingerprinter_info_get(struct GrooveFingerprinter *printer,
        struct GrooveFingerprinterInfo *info, int block);

void groove_fingerprinter_free_info(struct GrooveFingerprinterInfo *info);

/* returns < 0 on error, 0 on no info ready, 1 on info ready
 * if block is 1, block until info is ready
 */
int groove_fingerprinter_info_peek(struct GrooveFingerprinter *printer,
        int block);

/* get the position of the printer head
 * both the current playlist item and the position in seconds in the playlist
 * item are given. item will be set to NULL if the playlist is empty
 * you may pass NULL for item or seconds
 */
void groove_fingerprinter_position(struct GrooveFingerprinter *printer,
        struct GroovePlaylistItem **item, double *seconds);

/**
 * Compress and base64-encode a raw fingerprint
 *
 * The caller is responsible for freeing the returned pointer using
 * groove_fingerprinter_dealloc().
 *
 * Parameters:
 *  - fp: pointer to an array of signed 32-bit integers representing the raw
 *        fingerprint to be encoded
 *  - size: number of items in the raw fingerprint
 *  - encoded_fp: pointer to a pointer, where the encoded fingerprint will be
 *                stored
 *
 * Returns:
 *  - 0 on success, < 0 on error
 */
int groove_fingerprinter_encode(int32_t *fp, int size, char **encoded_fp);

/**
 * Uncompress and base64-decode an encoded fingerprint
 *
 * The caller is responsible for freeing the returned pointer using
 * groove_fingerprinter_dealloc().
 *
 * Parameters:
 *  - encoded_fp: Pointer to an encoded fingerprint
 *  - encoded_size: Size of the encoded fingerprint in bytes
 *  - fp: Pointer to a pointer, where the decoded raw fingerprint (array
 *        of signed 32-bit integers) will be stored
 *  - size: Number of items in the returned raw fingerprint
 *
 * Returns:
 *  - 0 on success, < 0 on error
 */
int groove_fingerprinter_decode(char *encoded_fp, int32_t **fp, int *size);

void groove_fingerprinter_dealloc(void *ptr);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* GROOVE_FINGERPRINTER_H_INCLUDED */
