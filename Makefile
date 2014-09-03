CFLAGS = -g -Wall

ifneq ($(shell uname -s),Darwin)
  LDLIBS = -lcrypt
endif

dht-example: dht-example.o dht.o

all: dht-example

clean:
	-rm -f dht-example dht-example.o dht-example.id dht.o *~ core
