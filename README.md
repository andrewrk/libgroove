# libgroove

Generic music player backend library.

For documentation, see include/groove.h and the examples. Live chat is
available in the #libgroove channel of irc.freenode.org.

## Features

 * Uses [libav](http://www.libav.org/) for robust decoding and encoding.
   - [list of supported formats](http://www.libav.org/general.html#Supported-File-Formats-and-Codecs)
 * Add and remove entries on a playlist for gapless playback.
 * Supports idempotent pause, play, and seek.
 * Per-playlist-item gain adjustment so you can implement loudness compensation
   without audio glitches.
 * Read and write metadata tags.
 * Extensible sink-based interface. A sink provides resampling
   and keeps its buffer full. Types of sinks:
   * **raw sink** - provides reference-counted raw audio buffers you can do
     whatever you like with. For example a real-time audio visualization.
     All other sink types are built on top of this one.
   * **player sink** - sends frames to a sound device.
   * **encoder sink** - provides encoded audio buffers. For example you could
     use this to create an HTTP audio stream.
   * **loudness scanner sink** - uses the [EBU R 128](http://tech.ebu.ch/loudness)
     standard to provide
     [ReplayGain](http://wiki.hydrogenaudio.org/index.php?title=ReplayGain_1.0_specification)-compatible values.
   * ([on the roadmap](https://github.com/superjoe30/libgroove/issues/23)) accoustid fingerprint
   * ([on the roadmap](https://github.com/superjoe30/libgroove/issues/19)) perfect duration analyzer
 * Thread-safe.

## Dependencies

You will need these to compile libgroove. These are most likely in your
distribution's package manager.

 * [libbz2-dev](http://www.bzip.org/)
 * [yasm](http://yasm.tortall.net/)
 * [cmake](http://www.cmake.org/)
 * [libsdl2-dev](http://www.libsdl.org/)
 * [libmp3lame-dev](http://lame.sourceforge.net/)

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
    * `transcode` - transcode one or more files into one output file

## Projects Using libgroove

Feel free to make a pull request adding yours to this list.

 * [TrenchBowl](https://github.com/superjoe30/TrenchBowl) - a simple Qt GUI
   on top of libgroove.
 * [node-groove](https://github.com/superjoe30/node-groove) -
   [Node.js](http://nodejs.org/) bindings to libgroove.
   - [Groove Basin](https://github.com/superjoe30/groovebasin) - lazy
     multi-core replaygain scanning, web interface inspired by Amarok 1.4,
     http streaming, upload, download, dynamic playlist mode
