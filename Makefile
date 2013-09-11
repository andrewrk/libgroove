CC = c99
LIBAV_PREFIX = /usr
TMP = tmpbuild

# for compiling groove
ALLAVLIBS = avfilter avformat avcodec avresample avutil
CFLAGS := -I$(LIBAV_PREFIX)/include -pedantic -Werror -Wall -g -O0 -fPIC 
STATIC_LIBS := $(ALLAVLIBS:%=$(LIBAV_PREFIX)/lib/lib%.a)
LDFLAGS := -lSDL -lbz2 -lz -lm -pthread

# for compiling examples
EX_CFLAGS := -Isrc -D_POSIX_C_SOURCE=200809L -pedantic -Werror -Wall -g -O0
EX_STATIC_LIBS := src/groove.a $(STATIC_LIBS)
EX_LDFLAGS := $(LDFLAGS)

.PHONY: examples clean all

all: examples

src/groove.a: src/groove.o
	ar rcs src/groove.a src/groove.o

src/groove.o: src/groove.c
	$(CC) $(CFLAGS) -o src/groove.o -c src/groove.c

examples: example/playlist example/identify

example/identify: example/identify.o src/groove.a
	$(CC) -o example/identify example/identify.o src/groove.a $(EX_STATIC_LIBS) $(EX_LDFLAGS)

example/identify.o: example/identify.c
	$(CC) $(EX_CFLAGS) -o example/identify.o -c example/identify.c

example/playlist: example/playlist.o src/groove.a
	$(CC) -o example/playlist example/playlist.o src/groove.a $(EX_STATIC_LIBS) $(EX_LDFLAGS)

example/playlist.o: example/playlist.c
	$(CC) $(EX_CFLAGS) -o example/playlist.o -c example/playlist.c

clean:
	rm -f src/*.o src/*.so src/*.a
	rm -f example/*.o
	rm -f example/playlist
	rm -f example/identify
