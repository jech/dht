CFLAGS = -g -Wall -DHAS_STDINT_H
LDLIBS = -lcrypt

dht-example: dht-example.o dht.o

all: dht-example

clean:
	-rm -f dht-example dht-example.o dht-example.id dht.o *~ core
