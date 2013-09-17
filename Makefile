CC = c99
LIBAV_SRC = $(abspath deps/libav)
LIBAV_PREFIX = $(LIBAV_SRC)/out
LIBAV_DEP = $(LIBAV_PREFIX)/lib/libavformat.a
EBUR128_SRC = $(abspath deps/ebur128)
EBUR128_PREFIX = $(abspath deps/ebur128)/build
EBUR128_DEP = $(abspath deps/ebur128)/build/libebur128.a
PREFIX = /usr/local
VERSION_MAJOR = 1
VERSION_MINOR = 0
VERSION_PATCH = 0
GROOVE_SO_NAME = libgroove.so.$(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_PATCH)
GROOVE_SO_SRC = src/groove.so
GROOVE_SO_DEST = $(PREFIX)/lib/$(GROOVE_SO_NAME)

# for compiling groove
ALLAVLIBS = avfilter avformat avcodec avresample swscale avutil
CFLAGS := -I$(LIBAV_PREFIX)/include -I$(EBUR128_SRC) -pedantic -Werror -Wall -g -O0 -fPIC 
STATIC_LIBS := $(ALLAVLIBS:%=$(LIBAV_PREFIX)/lib/lib%.a) $(EBUR128_DEP)
LDFLAGS := -lSDL -lbz2 -lz -lm -pthread

# for compiling examples
EX_CFLAGS := -D_POSIX_C_SOURCE=200809L -pedantic -Werror -Wall -g -O0
EX_LDFLAGS := -lgroove

.PHONY: examples clean distclean install uninstall

$(GROOVE_SO_SRC): src/scan.o src/decode.o src/player.o src/queue.o $(EBUR128_DEP)
	$(CC) -fPIC -shared -o $(GROOVE_SO_SRC) -Wl,-soname,libgroove.so.$(VERSION_MAJOR) -Wl,-Bsymbolic src/scan.o src/decode.o src/player.o src/queue.o $(STATIC_LIBS) $(LDFLAGS)

src/decode.o: src/decode.c $(LIBAV_DEP)
	$(CC) $(CFLAGS) -o src/decode.o -c src/decode.c

src/queue.o: src/queue.c $(LIBAV_DEP)
	$(CC) $(CFLAGS) -o src/queue.o -c src/queue.c

src/scan.o: src/scan.c $(LIBAV_DEP)
	$(CC) $(CFLAGS) -o src/scan.o -c src/scan.c

src/player.o: src/player.c $(LIBAV_DEP)
	$(CC) $(CFLAGS) -o src/player.o -c src/player.c

examples: example/playlist example/metadata example/replaygain

example/metadata: example/metadata.o
	$(CC) -o example/metadata example/metadata.o $(EX_LDFLAGS)

example/metadata.o: example/metadata.c
	$(CC) $(EX_CFLAGS) -o example/metadata.o -c example/metadata.c

example/playlist: example/playlist.o
	$(CC) -o example/playlist example/playlist.o $(EX_LDFLAGS)

example/playlist.o: example/playlist.c
	$(CC) $(EX_CFLAGS) -o example/playlist.o -c example/playlist.c

example/replaygain: example/replaygain.o
	$(CC) -o example/replaygain example/replaygain.o $(EX_LDFLAGS)

example/replaygain.o: example/replaygain.c
	$(CC) $(EX_CFLAGS) -o example/replaygain.o -c example/replaygain.c

$(LIBAV_DEP): $(LIBAV_SRC)/configure
	cd $(LIBAV_SRC) && ./configure --prefix=$(LIBAV_PREFIX) --enable-pic && $(MAKE) && $(MAKE) install

$(EBUR128_DEP): $(EBUR128_SRC)/CMakeLists.txt
	mkdir -p $(EBUR128_PREFIX) && cd $(EBUR128_PREFIX) && cmake ../ && $(MAKE)

clean:
	rm -f src/*.o src/*.so src/*.a
	rm -f example/*.o
	rm -f example/playlist
	rm -f example/metadata
	rm -f example/replaygain

distclean: clean
	rm -rf $(LIBAV_PREFIX) $(EBUR128_PREFIX)
	cd $(LIBAV_SRC) && $(MAKE) distclean

install: $(GROOVE_SO_SRC) src/groove.h
	install -m 0755 $(GROOVE_SO_SRC) $(GROOVE_SO_DEST)
	install -m 0644 src/groove.h $(PREFIX)/include
	rm -f $(PREFIX)/lib/libgroove.so
	ln -s $(GROOVE_SO_DEST) $(PREFIX)/lib/libgroove.so
	ldconfig -n $(PREFIX)/lib

uninstall:
	rm -f $(PREFIX)/lib/libgroove.so*
	rm -f $(PREFIX)/include/groove.h
