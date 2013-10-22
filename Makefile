CC = c99
PREFIX = /usr/local
VERSION_MAJOR = 1
VERSION_MINOR = 0
VERSION_PATCH = 0

LIBAV_SRC = $(abspath deps/libav)
# do NOT set this to /usr - it will be rm -rf'd before building!
LIBAV_PREFIX = $(LIBAV_SRC)/out
LIBAV_DEP = $(LIBAV_PREFIX)/lib/libavformat.a

EBUR128_SRC = $(abspath deps/ebur128)
# do NOT set this to /usr - it will be rm -rf'd before building!
EBUR128_PREFIX = $(EBUR128_SRC)/build
EBUR128_DEP = $(EBUR128_PREFIX)/libebur128.a

GROOVE_SO_NAME = libgroove.so.$(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_PATCH)
GROOVE_SO_SRC = src/groove.so
GROOVE_SO_DEST = $(PREFIX)/lib/$(GROOVE_SO_NAME)
GROOVE_A_SRC = src/libgroove.a
GROOVE_A_DEST = $(PREFIX)/lib/libgroove.a

# for compiling groove
ALLAVLIBS = avfilter avformat avcodec avresample swscale avutil
CFLAGS := -I$(LIBAV_PREFIX)/include -I$(EBUR128_SRC) -I$(abspath include) -pedantic -Werror -Wall -g -O0 -fPIC -D_REENTRANT
STATIC_LIBS := $(ALLAVLIBS:%=$(LIBAV_PREFIX)/lib/lib%.a) $(EBUR128_DEP)
LDLIBS = -lbz2 -lz -lm -lpthread -lSDL2
LDFLAGS = -fPIC -shared -Wl,-soname,libgroove.so.$(VERSION_MAJOR) -Wl,-Bsymbolic

O_FILES = src/scan.o src/player.o src/queue.o src/device_sink.o src/encoder.o src/file.o src/global.o

# for compiling examples
EX_CFLAGS = -D_POSIX_C_SOURCE=200809L -pedantic -Werror -Wall -g -O0
EX_LDLIBS = -lgroove
EX_LDFLAGS =

.PHONY: examples libs clean distclean install uninstall install-examples uninstall-examples

libs: $(GROOVE_SO_SRC) $(GROOVE_A_SRC)


$(GROOVE_A_SRC): $(O_FILES)
	ar rcs $(GROOVE_A_SRC) $(O_FILES)

$(GROOVE_SO_SRC): $(O_FILES) $(EBUR128_DEP)
	$(CC) $(LDFLAGS) -o $(GROOVE_SO_SRC) $(O_FILES) $(STATIC_LIBS) $(LDLIBS)

src/device_sink.o: src/device_sink.c $(LIBAV_DEP)
	$(CC) $(CFLAGS) -o src/device_sink.o -c src/device_sink.c

src/encoder.o: src/encoder.c $(LIBAV_DEP)
	$(CC) $(CFLAGS) -o src/encoder.o -c src/encoder.c

src/file.o: src/file.c $(LIBAV_DEP)
	$(CC) $(CFLAGS) -o src/file.o -c src/file.c

src/global.o: src/global.c $(LIBAV_DEP)
	$(CC) $(CFLAGS) -o src/global.o -c src/global.c

src/queue.o: src/queue.c $(LIBAV_DEP)
	$(CC) $(CFLAGS) -o src/queue.o -c src/queue.c

src/scan.o: src/scan.c $(LIBAV_DEP)
	$(CC) $(CFLAGS) -o src/scan.o -c src/scan.c

src/player.o: src/player.c $(LIBAV_DEP)
	$(CC) $(CFLAGS) -o src/player.o -c src/player.c

examples: example/playlist example/metadata example/replaygain

example/metadata: example/metadata.o
	$(CC) $(EX_LDFLAGS) -o example/metadata example/metadata.o $(EX_LDLIBS)

example/metadata.o: example/metadata.c
	$(CC) $(EX_CFLAGS) -o example/metadata.o -c example/metadata.c

example/playlist: example/playlist.o
	$(CC) $(EX_LDFLAGS) -o example/playlist example/playlist.o $(EX_LDLIBS)

example/playlist.o: example/playlist.c
	$(CC) $(EX_CFLAGS) -o example/playlist.o -c example/playlist.c

example/replaygain: example/replaygain.o
	$(CC) $(EX_CLDFLAGS) -o example/replaygain example/replaygain.o $(EX_LDLIBS)

example/replaygain.o: example/replaygain.c
	$(CC) $(EX_CFLAGS) -o example/replaygain.o -c example/replaygain.c

$(LIBAV_DEP): $(LIBAV_SRC)/configure
	rm -rf $(LIBAV_PREFIX) && cd $(LIBAV_SRC) && ./configure --prefix=$(LIBAV_PREFIX) --enable-pic && $(MAKE) && $(MAKE) install

$(EBUR128_DEP): $(EBUR128_SRC)/CMakeLists.txt
	rm -rf $(EBUR128_PREFIX) && mkdir -p $(EBUR128_PREFIX) && cd $(EBUR128_PREFIX) && cmake ../ && $(MAKE)

clean:
	rm -f src/*.o src/*.so src/*.a
	rm -f example/*.o
	rm -f example/playlist
	rm -f example/metadata
	rm -f example/replaygain

distclean: clean
	rm -rf $(LIBAV_PREFIX) $(EBUR128_PREFIX)
	cd $(LIBAV_SRC) && $(MAKE) distclean

install: $(GROOVE_SO_SRC) $(GROOVE_A_SRC) include/groove.h
	install -m 0755 $(GROOVE_SO_SRC) $(GROOVE_SO_DEST)
	install -m 0755 $(GROOVE_A_SRC) $(GROOVE_A_DEST)
	install -m 0644 include/groove.h $(PREFIX)/include
	rm -f $(PREFIX)/lib/libgroove.so
	ln -s $(GROOVE_SO_DEST) $(PREFIX)/lib/libgroove.so
	ldconfig -n $(PREFIX)/lib

uninstall:
	rm -f $(PREFIX)/lib/libgroove.so*
	rm -f $(PREFIX)/lib/libgroove.a*
	rm -f $(PREFIX)/include/groove.h

install-examples: examples
	install -m 0755 example/replaygain $(PREFIX)/bin
	install -m 0755 example/playlist $(PREFIX)/bin
	install -m 0755 example/metadata $(PREFIX)/bin

uninstall-examples:
	rm -f $(PREFIX)/bin/replaygain
	rm -f $(PREFIX)/bin/metadata
	rm -f $(PREFIX)/bin/playlist
