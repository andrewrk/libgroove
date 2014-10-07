libgroove
=========

This library provides decoding and encoding of audio on a playlist. It is
intended to be used as a backend for music player applications. That said, it is
also generic enough to be used as a backend for any streaming audio processing
utility.

Features
--------

* Uses [libav](http://www.libav.org/) for robust decoding and encoding. A list
  of supported file formats and codecs [is
  available](http://www.libav.org/general.html#Supported-File-Formats-and-Codecs).
* Add and remove entries on a playlist for gapless playback.
* Supports idempotent pause, play, and seek.
* Per-playlist-item gain adjustment so you can implement loudness compensation
  without audio glitches.
* Read and write metadata tags.
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
  * **fingerprint sink** - Uses [chromaprint](acoustid.org/chromaprint) to
    generate unique song IDs that can be used with the acoustid service.
* Thread-safe.
* Example programs included:
  * `playlist` - Play a series of songs with gapless playback.
  * `metadata` - Read or update song metadata.
  * `replaygain` - Report the suggested replaygain for a set of files.
  * `transcode` - Transcode one or more files into one output file.
  * `fingerprint` - Generate acoustid fingerprints for one or more files.

Dependencies
------------

You will need these to compile libgroove.

* [cmake](http://www.cmake.org/)
* [libav](http://libav.org)
  * suggested flags: `--enable-shared --disable-static --enable-libmp3lame --enable-libvorbis --enable-gpl`
* [libebur128](https://github.com/jiixyj/libebur128)
  * make sure it is compiled with the speex dependency so that true peak
    functions are available.
* [libsdl2-dev](http://www.libsdl.org/)
* [libchromaprint-dev](http://acoustid.org/chromaprint)

Installation
------------

Installing from a package is recommended, but instructions for installing from
source are also provided at the end of this list.

### [Ubuntu PPA](https://launchpad.net/~andrewrk/+archive/libgroove)

```sh
sudo apt-add-repository ppa:andrewrk/libgroove
sudo apt-get update
sudo apt-get install libgroove-dev libgrooveplayer-dev libgrooveloudness-dev \
    libgroovefingerprinter-dev
```

### [FreeBSD Port](http://www.freshports.org/audio/libgroove/)

```sh
pkg install audio/libgroove
```

### [Debian](http://packages.qa.debian.org/libg/libgroove.html)

[Configure your system](http://serverfault.com/questions/22414/) to be able to
install packages from testing.

```sh
sudo apt-get -t testing install libgroove-dev libgrooveplayer-dev \
    libgrooveloudness-dev libgroovefingerprinter-dev
```

### [Arch Linux](https://aur.archlinux.org/packages/libgroove/)

libgroove is available through the [AUR](https://aur.archlinux.org/).

```sh
wget https://aur.archlinux.org/packages/li/libgroove/libgroove.tar.gz
tar xzf libgroove.tar.gz
cd libgroove
makepkg
sudo pacman -U libgroove-*
```

Some notes:

* libgroove depends upon several other packages. Dependencies available through
  the official repositories can be installed with pacman (e.g. `pacman -S
  --asdeps sdl2`), and dependencies available through the AUR can be installed
  via the procedure shown above.
* An [AUR helper](https://wiki.archlinux.org/index.php/AUR_helper) can ease the
  process of installing packages from the AUR.
* The [AUR User
  Guidelines](https://wiki.archlinux.org/index.php/AUR_User_Guidelines) page on
  the Arch Wiki contains gobs of useful information. Please see that page if you
  have any further questions about using the AUR.

### [Mac OS X Homebrew](http://brew.sh/)

```sh
brew install libgroove
```

### From Source

```sh
mkdir build && cd build && cmake ../
# Verify that the configure output is to your liking.
make
sudo make install
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
* [TrenchBowl](https://github.com/andrewrk/TrenchBowl) is a simple Qt GUI
  on top of libgroove.
* [node-groove](https://github.com/andrewrk/node-groove) provides
  [Node.js](http://nodejs.org/) bindings to libgroove.
* [Groove Basin](https://github.com/andrewrk/groovebasin) is a music player with
  lazy multi-core replaygain scanning, a web interface inspired by Amarok 1.4,
  http streaming, upload, download and a dynamic playlist mode.
* [ruby-groove](https://github.com/johnmuhl/ruby-groove) provides Ruby FFI
  bindings to libgroove.
