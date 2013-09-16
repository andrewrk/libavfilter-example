CC=c99
LIBAV_PREFIX=/usr

ALLAVLIBS = avfilter avformat avcodec avresample swscale avutil
CFLAGS := -I$(LIBAV_PREFIX)/include -pedantic -Werror -Wall -g -O0 -fPIC 
STATIC_LIBS := $(ALLAVLIBS:%=$(LIBAV_PREFIX)/lib/lib%.a)
LDFLAGS := -lao -lbz2 -lz -lm -pthread

# for compiling examples
EX_STATIC_LIBS := src/groove.a $(STATIC_LIBS)
EX_LDFLAGS := $(LDFLAGS)

.PHONY: clean

main: main.o
	$(CC) -o main main.o $(STATIC_LIBS) $(LDFLAGS)

main.o: main.c
	$(CC) $(CFLAGS) -o main.o -c main.c 

clean:
	rm -f main.o main
