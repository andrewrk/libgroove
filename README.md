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

 * libsdl1.2-dev - http://www.libsdl.org/
 * libav - http://www.libav.org/
   - groove depends on libav version 9.9. As this version is not in Ubuntu or
     Debian's package manager, groove is compiled against libav statically.
     See instructions below.

### Building Against libav Statically

 1. Download [libav 9.9](https://www.libav.org/download.html#release_9)
 2. In the source tree of libav you just downloaded,
    `./configure --prefix=$(pwd)/out && make && make install`
 3. To build groove, `make LIBAV_PREFIX=/home/you/Downloads/libav9.9/out`
    (make sure that path is correct)
