CC = c99
LIBAV_PREFIX = /usr

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

src/groove.a: src/groove.o src/scan.o src/gain_analysis.o src/decode.o
	ar rcs src/groove.a src/groove.o src/scan.o src/gain_analysis.o src/decode.o

src/decode.o: src/decode.c
	$(CC) $(CFLAGS) -o src/decode.o -c src/decode.c

src/groove.o: src/groove.c
	$(CC) $(CFLAGS) -o src/groove.o -c src/groove.c

src/scan.o: src/scan.c
	$(CC) $(CFLAGS) -o src/scan.o -c src/scan.c

src/gain_analysis.o: src/gain_analysis.c
	$(CC) $(CFLAGS) -o src/gain_analysis.o -c src/gain_analysis.c

examples: example/playlist example/metadata example/replaygain

example/metadata: example/metadata.o src/groove.a
	$(CC) -o example/metadata example/metadata.o src/groove.a $(EX_STATIC_LIBS) $(EX_LDFLAGS)

example/metadata.o: example/metadata.c
	$(CC) $(EX_CFLAGS) -o example/metadata.o -c example/metadata.c

example/playlist: example/playlist.o src/groove.a
	$(CC) -o example/playlist example/playlist.o src/groove.a $(EX_STATIC_LIBS) $(EX_LDFLAGS)

example/playlist.o: example/playlist.c
	$(CC) $(EX_CFLAGS) -o example/playlist.o -c example/playlist.c

example/replaygain: example/replaygain.o src/groove.a
	$(CC) -o example/replaygain example/replaygain.o src/groove.a $(EX_STATIC_LIBS) $(EX_LDFLAGS)

example/replaygain.o: example/replaygain.c
	$(CC) $(EX_CFLAGS) -o example/replaygain.o -c example/replaygain.c

clean:
	rm -f src/*.o src/*.so src/*.a
	rm -f example/*.o
	rm -f example/playlist
	rm -f example/metadata
	rm -f example/replaygain
