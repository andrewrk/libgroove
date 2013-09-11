# groove

Music player backend library.

## Features

 * vaporware

## Goals

 * Robust. Decodes everything.
 * Add entries to a playlist for gapless playback.
 * Read/write tags for every supported format.
 * Replaygain volume adjustment during playback even if tags are missing.
 * Always performs replaygain scanning during playback and saves replaygain tags.
 * Seeking during playback.
 * httpd streaming server

## Dependencies

 * libsdl1.2-dev - http://www.libsdl.org/
 * libreplaygain-dev - http://svn.musepack.net/libreplaygain/
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
