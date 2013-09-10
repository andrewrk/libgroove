CC = clang
LIBAVSRC = /home/andy/Downloads/libav-9.8/out


ALLAVLIBS = avformat avcodec avdevice avfilter avresample avutil swscale
CFLAGS := -I$(LIBAVSRC)/include -pedantic -Werror -Wall -ggdb -O0
STATIC_LIBS := $(ALLAVLIBS:%=$(LIBAVSRC)/lib/lib%.a)
LDFLAGS := -lm -lz -pthread -lbz2 -lSDL

.PHONY: examples clean all

all: examples groove.o

groove.o: src/groove.c
	$(CC) $(CFLAGS) -o src/groove.o -c src/groove.c

examples: example/play

example/play: example/play.o
	$(CC) -o example/play example/play.o $(STATIC_LIBS) $(LDFLAGS) 

example/play.o: example/play.c
	$(CC) $(CFLAGS) -o example/play.o -c example/play.c

clean:
	rm -f example/*.o
	rm -f example/play
