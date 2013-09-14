# libgroove

Music player backend library.

## Features

 * Robust. Decodes everything.
 * Add and remove entries on a playlist for gapless playback.
 * Supports pause, play, stop, and seek during playback.
 * Read and write metadata tags for every supported format.

## Roadmap

 * Support ReplayGain scanning for every format.
   - add files to a batch job and monitor progress
 * ReplayGain volume adjustment during playback.
 * Poll or wait for player events.
 * Ability to keep a buffer filled with encoded audio.
   - This could be used for example for an HTTP streaming server.

## Dependencies

These are most likely in your distribution's package manager.

 * libsdl1.2-dev - http://www.libsdl.org/
 * libbz2-dev - http://www.bzip.org/
 * yasm - http://yasm.tortall.net/
