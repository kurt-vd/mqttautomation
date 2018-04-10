PROGS	= mqttinputevent
PROGS	+= mqtt1wtemp
PROGS	+= mqttapa102led
PROGS	+= mqttimport
PROGS	+= mqttled
PROGS	+= mqttlogic
PROGS	+= mqttmaclight
PROGS	+= mqttnow
PROGS	+= mqttteleruptor
PROGS	+= rpntest
default	: $(PROGS)

PREFIX	= /usr/local

CC	= gcc
CFLAGS	= -Wall
CPPFLAGS= -D_GNU_SOURCE
LDLIBS	= -lmosquitto
INSTOPTS= -s

VERSION := $(shell git describe --tags --always)

-include config.mk

CPPFLAGS += -DVERSION=\"$(VERSION)\"

mqtt1wtemp: lib/libt.o

mqttapa102led: lib/libt.o

mqttled: lib/libt.o

mqttlogic: LDLIBS+=-lm
mqttlogic: lib/libt.o rpnlogic.o sunposition.o

mqttmaclight: lib/libt.o

mqttnow: lib/libt.o

mqttteleruptor: lib/libt.o

rpntest: LDLIBS+=-lm
rpntest: lib/libt.o rpnlogic.o sunposition.o

install: $(PROGS)
	$(foreach PROG, $(PROGS), install -vp -m 0777 $(INSTOPTS) $(PROG) $(DESTDIR)$(PREFIX)/bin/$(PROG);)

clean:
	rm -rf $(wildcard *.o lib/*.o) $(PROGS)
