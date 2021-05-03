CFLAGS?=-std=c99 -D_GNU_SOURCE -Wall -O2 -Wno-unused-result -Wimplicit-function-declaration
CC=gcc
LDFLAGS = -g   # debugging symbols in build
LDLIBS = -lz -lcurl -pthread # link with libz

default: all

all: paster2

paster2: paster2.c zutil.c crc.c shm_stack.c
	$(CC) $(CFLAGS) -o paster2 paster2.c zutil.c crc.c shm_stack.c $(LDLIBS) $(LDFLAGS)

clean:
	rm -rf paster2