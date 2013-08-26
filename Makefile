
CFLAGS=-Wall `pkg-config fuse --cflags --libs` -O2
LDLIBS=`pkg-config fuse --libs`

all: testfuse

testfuse: testfuse.o

clean:
	rm -f testfuse testfuse.o

