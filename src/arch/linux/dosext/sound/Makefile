
top_builddir=../../../../..
include $(top_builddir)/Makefile.conf

CFILES = linux_sound.c
HFILES = linux_sound.h $(REALTOPDIR)/include/sound.h
DEPENDS=$(CFILES:.c=.d)

ALL = $(CFILES) $(HFILES) README.sound TODO

OBJS = linux_sound.o

include $(REALTOPDIR)/src/Makefile.common

all: lib

install: all
