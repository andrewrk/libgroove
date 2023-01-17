libgroove
=========

This library provides decoding and encoding of audio on a playlist. It is
intended to be used as a backend for music player applications. That said, it is
also generic enough to be used as a backend for any streaming audio processing
utility.

Features
--------

* Uses [ffmpeg](https://ffmpeg.org/) for robust decoding and encoding. A list
  of supported file formats and codecs [is
  available](https://ffmpeg.org/ffmpeg-formats.html).
* Add and remove entries on a playlist for gapless playback.
* Supports idempotent pause, play, and seek.
* Per-playlist-item gain adjustment so you can implement loudness compensation
  without audio glitches.
* Read and write metadata tags.
* Choose between smooth mode and exact mode during playback.
  * **smooth mode** - open the audio device once and resample everything to
    fit that sample rate and format.
  * **exact mode** - open and close the audio device as necessary in effort
    to open the audio device with parameters matching the incoming audio data.
* Extensible sink-based interface. A sink provides resampling and keeps its
  buffer full. Types of sinks:
  * **raw sink** - Provides reference-counted raw audio buffers you can do
    whatever you like with. For example a real-time audio visualization. All
    other sink types are built on top of this one.
  * **player sink** - Sends frames to a sound device.
  * **encoder sink** - Provides encoded audio buffers. For example, you could
    use this to create an HTTP audio stream.
  * **loudness scanner sink** - Uses the [EBU R
    128](http://tech.ebu.ch/loudness) standard to detect loudness. The values it
    produces are compatible with the
    [ReplayGain](http://wiki.hydrogenaudio.org/index.php?title=ReplayGain_1.0_specification)
    specification.
  * **fingerprint sink** - Uses [chromaprint](http://acoustid.org/chromaprint)
    to generate unique song IDs that can be used with the acoustid service.
* Thread-safe.
* Example programs included:
  * `playlist` - Play a series of songs with gapless playback.
  * `metadata` - Read or update song metadata.
  * `replaygain` - Report the suggested replaygain for a set of files.
  * `transcode` - Transcode one or more files into one output file.
  * `fingerprint` - Generate acoustid fingerprints for one or more files.

### From Source

#### Normal build

```sh
mkdir build && cd build && cmake ../
# Verify that the configure output is to your liking.
make
sudo make install
```

#### Building with a locally built libav

```sh
mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=$HOME/local -DCMAKE_PREFIX_PATH=$HOME/local -DCMAKE_BUILD_TYPE=Release
# Verify that the configure output is to your liking.
make
make install
```

Documentation
-------------

Check out the example programs in the example folder.

Read header files for the relevant APIs:

* groove/groove.h
  * globals
  * GrooveFile
  * GroovePlaylist
  * GrooveBuffer
  * GrooveSink
* groove/encoder.h
  * GrooveEncoder
* grooveplayer/player.h
  * GroovePlayer
* grooveloudness/loudness.h
  * GrooveLoudnessDetector
* groovefingerprinter/fingerprinter.h
  * GrooveFingerprinter

Join #libgroove on irc.freenode.org and ask questions.

Projects Using libgroove
------------------------

Feel free to make a pull request adding yours to this list.

* [waveform](https://github.com/andrewrk/waveform) generates PNG waveform
  visualizations.
* [node-groove](https://github.com/andrewrk/node-groove) provides
  [Node.js](http://nodejs.org/) bindings to libgroove.
* [Groove Basin](https://github.com/andrewrk/groovebasin) is a music player with
  lazy multi-core replaygain scanning, a web interface inspired by Amarok 1.4,
  http streaming, upload, download and a dynamic playlist mode.
* [groove-rs](https://github.com/andrewrk/groove-rs) provides
  [rust](http://rust-lang.org) bindings to libgroove.
* [ruby-groove](https://github.com/johnmuhl/ruby-groove) provides Ruby FFI
  bindings to libgroove.
* [TrenchBowl](https://github.com/andrewrk/TrenchBowl) is a simple Qt GUI
  on top of libgroove.
