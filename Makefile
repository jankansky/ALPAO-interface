CC=gcc
CFLAGS=-g -I/usr/local/milk/include/ImageStreamIO
LDFLAGS=-L/usr/local/milk/lib
LIBS=-lImageStreamIO -lasdk -lpthread -lrt -lcfitsio

all: runALPAO resetALPAO releaseALPAO

runALPAO: runALPAO.c
	$(CC) -o runALPAO runALPAO.c $(CFLAGS) $(LIBS) $(LDFLAGS)

resetALPAO: resetALPAO.c
	$(CC) -o resetALPAO resetALPAO.c $(CFLAGS) $(LIBS) $(LDFLAGS)

releaseALPAO: releaseALPAO.c
	$(CC) -o releaseALPAO releaseALPAO.c $(CFLAGS) $(LIBS) $(LDFLAGS)


clean:
	rm runALPAO releaseALPAO resetALPAO
