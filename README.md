# libgroove

Audio dispatching library. Generic sink-based interface. Provides decoding,
encoding, resampling, and gain adjustment. Perfect for the backend of a
music player.

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
     standard to detect loudness. The values it produces are compatible with
     [ReplayGain](http://wiki.hydrogenaudio.org/index.php?title=ReplayGain_1.0_specification).
   * ([on the roadmap](https://github.com/andrewrk/libgroove/issues/19)) accoustid fingerprint
 * Thread-safe.
 * Example programs included:
   * `playlist` - play a series of songs with gapless playback
   * `metadata` - read or update song metadata
   * `replaygain` - report the suggested replaygain for a set of files
   * `transcode` - transcode one or more files into one output file
 * Cross-platform.

## Dependencies

You will need these to compile libgroove. Some dependencies are bundled
so that you can still compile if they cannot be found on your system.

 * [cmake](http://www.cmake.org/)
 * [libav](http://libav.org). (bundled) if using bundled version, needs these:
   - [libbz2-dev](http://www.bzip.org/)
   - [yasm](http://yasm.tortall.net/)
   - [libmp3lame-dev](http://lame.sourceforge.net/)
 * [libebur128](https://github.com/jiixyj/libebur128) (bundled)
 * [libsdl2-dev](http://www.libsdl.org/) (bundled)

## Installation

### Pre-Built Packages

 * [Ubuntu PPA](https://launchpad.net/~andrewrk/+archive/libgroove)

   ```
   sudo apt-add-repository ppa:andrewrk/libgroove
   sudo apt-get update
   sudo apt-get install libgroove-dev libgrooveplayer-dev libgrooveloudness-dev
   ```
 * [Debian experimental](http://serverfault.com/questions/22414)

### From Source

 1. `mkdir build && cd build && cmake ../`
 2. Verify that the configure output is to your liking.
 3. `make`
 4. `sudo make install`

## Documentation

 * Check out the example programs in the example folder.
 * Read some header files for the relevant APIs:
   * groove/groove.h
     - global stuff
     - GrooveFile
     - GroovePlaylist
     - GrooveBuffer
     - GrooveSink
   * groove/encoder.h
     - GrooveEncoder
   * grooveplayer/player.h
     - GroovePlayer
   * grooveloudness/loudness.h
     - GrooveLoudnessDetector
 * Join #libgroove on irc.freenode.org and ask questions.

## Projects Using libgroove

Feel free to make a pull request adding yours to this list.

 * [waveform](https://github.com/andrewrk/waveform) - generate a waveform
   visualization in PNG format.
 * [TrenchBowl](https://github.com/andrewrk/TrenchBowl) - a simple Qt GUI
   on top of libgroove.
 * [node-groove](https://github.com/andrewrk/node-groove) -
   [Node.js](http://nodejs.org/) bindings to libgroove.
   - [Groove Basin](https://github.com/andrewrk/groovebasin) - lazy
     multi-core replaygain scanning, web interface inspired by Amarok 1.4,
     http streaming, upload, download, dynamic playlist mode
