CC=gcc
CFLAGS=-I/usr/local/milk/include/ImageStreamIO
LDFLAGS=-L/usr/local/milk/lib
LIBS=-lImageStreamIO -lasdk -lpthread -lrt -lcfitsio

all: runALPAO.c resetALPAO.c releaseALPAO.c
	$(CC) -o runALPAO runALPAO.c $(CFLAGS) $(LIBS) $(LDFLAGS)

clean:
	rm runALPAO
