### Version 3.1.1 (2014-04-21)
 * fingerprinter example: add --raw option
 * fingerprinter sink: add error checking to chromaprint function calls
 * fingerprinter sink: fix documentation. raw fingerprints are signed 32-bit
   integers, not unsigned 32-bit integers.
 * fingerprinter sink: change `void *` to `int32_t *` for encode/decode
   functions

### Version 3.1.0 (2014-04-19)

 * add acoustid fingerprinter sink. Closes #19
 * build: revert GNUInstallDirs
 * update to libav 10.1

### Version 3.0.8 (2014-04-01)

 * loudness scanning: fix memory corruption when scanning large album
 * update to libav10
 * playlist: fix segfault on out of memory
 * playlist: fix segfault race condition when adding a sink

### Version 3.0.7 (2014-03-16)

 * build: fix cmake warnings
 * use ebur128 true peak instead of sample peak
 * fix bug where accessing "album" metadata would instead
   return the "album_artist"
 * update to libav 10 beta2
 * use the compand filter to allow setting the gain to > 1.0. Closes #45
 * log error when cannot set up filter graph
 * loudness scanning: fix crash for songs with 0 frames. Closes #48
 * playlist example: fix race condition

### Version 3.0.6 (2014-02-20)

 * build: avoid useless dependencies

### Version 3.0.5 (2014-02-20)

 * update to libav dff1c19140e70. Closes #16 ASF seeking bug
 * build: use GNUInstallDirs

### Version 3.0.4 (2014-02-09)
 
 * delete SDL2 config-generated files from repo
   (they were causing an issue with debian packaging)

### Version 3.0.3 (2014-02-09)

 * update libav to 246d3bf0ec

### Version 3.0.2 (2013-11-25)

 * build: add static targets to all libraries

### Version 3.0.1 (2013-11-25)

 * build: depend on system libav if possible

### Version 3.0.0 (2013-11-24)

 * queue: depend on pthread instead of SDL2
 * file: depend on pthreads instead of SDL2
 * isolate SDL dependency to player
 * encoder: depend on pthread instead of SDL2
 * player: use pthread for mutexes instead of SDL2
 * loudness detector: depend on pthread instead of SDL2
 * playlist: use pthread for mutexes instead of SDL2
 * separate independent components into their own librares. closes #39
 * build: use the same version info for all libs

### Version 2.0.4 (2013-11-23)

 * update libav to d4df02131b5522
 * playlist: set sent_end_of_q at playlist create
 * better timestamp setting

### Version 2.0.3 (2013-11-22)

 * fix build when libspeexdsp is installed
 * cmake: support 2.8.7
 * groove.h conforms to C90
 * buffer implemented with pthreads instead of SDL
 * playlist: fix GrooveBuffer ref/unref race condition. closes #28

### Version 2.0.2 (2013-11-19)

 * out-of-tree build for bundled libav
 * update libav to 16e7b189c54

### Version 2.0.1 (2013-11-18)

 * compile with -O3
 * update libav to 1c01b0253eb
 * build system: bundle SDL2 but try to use system SDL2 if possible
 * when doing bundled SDL2, use the cmake build
 * enable SDL_File because osx needs it
 * try to build against system libebur128 and fall back on bundled. closes #38

### Version 2.0.0 (2013-11-16)

 * decode: remove last_decoded_file
 * SDL2 bundled dependency cleanup
 * Makefile: libgroove.so links against -ldl
 * fix 100% CPU by not disabling sdl timer. closes #20
 * playlist example: check for failure to create player
 * decode thread: end of playlist sentinel and full checking for every sink
 * update init_filter_graph to route based on sinks
 * sink: separate create from attach / destroy from detach
 * depend on system SDL2
 * expose GrooveSink buffer_size to public API
 * rename GroovePlayer to GroovePlaylist
 * rename player.c to playlist.c
 * rename GrooveDeviceSink to GroovePlayer
 * rename device_sink.c to player.c
 * rename sample_count to frame_count
 * print error string when cannot create aformat filter
 * playlist: protect sink_map with a mutex
 * player: flush queue on detach; reset queue on attach
 * rename LICENSE to COPYING as per ubuntu suggestion
 * encoder sink implementation
 * transcoding example
 * workaround for av_guess_codec bug
 * fix some memory leaks found with valgrind
 * fix segfault when logging is on for file open errors
 * fix segfault when playlist item deleted
 * logging: no audio stream found is INFO not ERROR
 * encoder: stop when encoding buffer full. closes #32
 * file: loggeng unrecognized format is INFO not ERROR
 * encoder: default bit_rate to 256kbps
 * support setting buffer_frame_count. closes #33
 * transcode example: ability to join multiple tracks into one
 * encoder: add flushes to correctly obtain format header
 * encoder: set pts and dts to stream->nb_samples. closes #34
 * playlist: decode_head waits on a mutex condition instead of sleeping
 * playlist: when sinks are full wait on mutex condition
 * encoder: draining uses a mutex condition instead of delay. closes #24
 * transcode example: copy metadata to new file. closes #31
 * encoder: fix deadlock race condition
 * add missing condition signals
 * consistent pointer conventions in groove.h
 * avoid prefixing header defines with underscores
 * better public/private pattern for GrooveFile. see #37
 * queue: better public/private pattern and no typedefs
 * encoder: better public/private pattern
 * buffer: better public/private pattern and get rid of typedef
 * player: better public/private pattern and remove typedefs
 * playlist: better public/private pattern and remove typedefs
 * remove typedef on GrooveTag. see #36
 * scan: better public/private pattern and remove typedefs
 * add license information to each file. closes #29
 * use proper prototypes when no arguments
 * do not use atexit; provide groove_finish
 * encoder: fix cleanup deadlock race condition
 * playlist: fix small memory leak on cleanup
 * player: delete unused field
 * loudness detector rewrite. closes #27 and #25
 * LoudnessDetector also reports duration. closes #23
 * loudness detector: more error checking
 * readme update
 * loudness detector: ability to get position (progress)
 * add _peek() methods to all sinks
 * remove groove_loudness_to_replaygain from API
 * loudness detector: faster and more accurate album info
 * add groove_encoder_position API
 * update libav to 72ca830f511fcdc
 * build with cmake
 * fix build errors with clang
 * cmake: only do -Wl,-Bsymbolic on unix environments
 * fix build on OSX

### Version 1.0.0 (2013-08-08)

  * initial public release
