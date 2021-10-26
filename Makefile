PROGS	= mqttinputevent
PROGS	+= mqtt1wtemp
PROGS	+= mqttapa102led
PROGS	+= mqttbridge
PROGS	+= mqttiio
PROGS	+= mqttimport
PROGS	+= mqttled
PROGS	+= mqttlogic
PROGS	+= mqttmaclight
PROGS	+= mqttmotor
PROGS	+= mqttnow
PROGS	+= mqttpoort
PROGS	+= mqttsysfsrd
PROGS	+= mqttteleruptor
PROGS	+= rpntest
PROGS	+= testteleruptor
PROGS	+= testpoort
default	: $(PROGS)

PREFIX	= /usr/local

CC	= gcc
CFLAGS	= -Wall
CPPFLAGS= -D_GNU_SOURCE
LDLIBS	= -lm -lmosquitto
INSTOPTS= -s

VERSION := $(shell git describe --tags --always)

-include config.mk

CPPFLAGS += -DVERSION=\"$(VERSION)\"

mqtt1wtemp: common.o lib/libt.o

mqttapa102led: common.o lib/libt.o

mqttbridge: common.o lib/libt.o \
	lib/libe.o \
	lib/liburi.o

mqttiio: LDLIBS+= -lm
mqttiio: common.o lib/libt.o lib/libe.o

mqttimport: common.o
mqttinputevent: common.o

mqttled: common.o lib/libt.o

mqttlogic: LDLIBS+=-lm
mqttlogic: common.o lib/libt.o lib/libe.o \
	rpnlogic.o astronomics.o

mqttmaclight: common.o lib/libt.o

mqttmotor: common.o lib/libt.o

mqttnow: common.o lib/libt.o

mqttpoort: LDLIBS+=-lm
mqttpoort: common.o lib/libt.o

mqttsysfsrd: common.o lib/libt.o

mqttteleruptor: common.o lib/libt.o

rpntest: LDLIBS+=-lm
rpntest: common.o lib/libt.o rpnlogic.o astronomics.o

testpoort: common.o lib/libt.o
testteleruptor: common.o lib/libt.o

install: $(PROGS)
	$(foreach PROG, $(PROGS), install -vp -m 0777 $(INSTOPTS) $(PROG) $(DESTDIR)$(PREFIX)/bin/$(PROG);)

clean:
	rm -rf $(wildcard *.o lib/*.o) $(PROGS)
