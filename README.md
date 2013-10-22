# libgroove

Music player backend library.

For documentation, see include/groove.h and the examples. Join us in #libgroove on Freenode.

## Features

 * Uses [libav](http://www.libav.org/) for robust decoding and encoding.
   - [list of supported formats](http://www.libav.org/general.html#Supported-File-Formats-and-Codecs)
 * Add and remove entries on a playlist for gapless playback.
 * Supports idempotent pause, play, and seek.
 * Read and write metadata tags.
 * Audio scanning to find out extra metadata.
   - [EBU R 128](http://tech.ebu.ch/loudness) loudness scanning which outputs
     [ReplayGain](http://wiki.hydrogenaudio.org/index.php?title=ReplayGain_1.0_specification)-compatible values.
   - progress callbacks at a configurable interval
   - (on the roadmap) perfectly accurate duration
   - (on the roadmap) accoustid fingerprint
 * Per-playlist item gain adjustment so you can implement
   loudness compensation without audio glitches.
 * Generic audio routing.
   - You can create multiple player instances.
   - You can attach multiple raw audio sinks to a player instance.
     This could be used for live audio visualization, for example.
   - You can attach multiple playback devices to a player instance.
   - You can attach multiple realtime encoders to a player instance.
     This could be used to create an http audio stream, for example.
 * Thread-safe.

## Dependencies

You will need these to compile libgroove. These are most likely in your
distribution's package manager.

 * [libbz2](http://www.bzip.org/)
 * [yasm](http://yasm.tortall.net/)
 * [cmake](http://www.cmake.org/)
 * [libsdl2-dev](http://www.libsdl.org/)

### Bundled Dependencies

These are bundled with libgroove. You don't need to do anything except
appreciate them.

Once libav makes a debian upstream release, we will no longer bundle these
dependencies and instead make our own debian upstream release.

 * [libav](http://libav.org)
 * [libebur128](https://github.com/jiixyj/libebur128)

## Installation

 1. Once you have the dependencies installed, you can use `make` to build
    libgroove.so. Using the `-jx` option where x is how many cores you have
    is recommended.
 2. Next install libgroove to your system with `make install`. You will need
    root privileges if you leave the `PREFIX` variable to its default, which
    is `/usr/local`.
 3. With libgroove installed in your system, you can compile the examples with
    `make examples`.
 4. Optionally you can install the examples to your system with
   `make install-examples`. These examples are:
    * `playlist` - play a series of songs with gapless playback
    * `metadata` - read or update song metadata
    * `replaygain` - report the suggested replaygain for a set of files

## Projects Using libgroove

Feel free to make a pull request adding yours to this list.

 * [TrenchBowl](https://github.com/superjoe30/TrenchBowl) - a simple Qt GUI
   on top of libgroove.
 * [node-groove](https://github.com/superjoe30/node-groove) -
   [Node.js](http://nodejs.org/) bindings to libgroove.
   - [Groove Basin](https://github.com/superjoe30/groovebasin) - lazy
     multi-core replaygain scanning, web interface inspired by Amarok 1.4,
     http streaming, upload, download, dynamic playlist mode
