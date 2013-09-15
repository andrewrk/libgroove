# libgroove

Music player backend library.

## Features

 * Robust. Decodes everything.
 * Add and remove entries on a playlist for gapless playback.
 * Supports pause, play, stop, and seek during playback.
 * Read and write metadata tags for every supported format.
 * Support [EBU R 128](http://tech.ebu.ch/loudness) loudness scanning for every format.
   - add files to a batch job and monitor progress

## Roadmap

 * Loudness compensation using ReplayGain tags during playback.
 * Poll or wait for player events.
 * Ability to keep a buffer filled with encoded audio.
   - This could be used for example for an HTTP streaming server.

## Dependencies

You will need these to compile libgrove. These are most likely in your
distribution's package manager.

 * [libsdl1.2-dev](http://www.libsdl.org/)
 * [libbz2-dev](http://www.bzip.org/)
 * [yasm](http://yasm.tortall.net/)
 * [cmake](http://www.cmake.org/)

### Bundled Dependencies

These are bundled with libgroove. You don't need to do anything except
appreciate them.

 * [libav](http://libav.org)
 * [libebur128](https://github.com/jiixyj/libebur128)
