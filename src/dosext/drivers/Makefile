top_builddir=../../..
include $(top_builddir)/Makefile.conf

# any misc. drivers (e.g. cdrom helper)

ifdef ASPI_SUPPORT
ASPI=aspi.c
endif

CFILES=cdrom.c $(ASPI)
SFILES=
ALL=$(CFILES) $(SFILES)

OBJS=$(CFILES:.c=.o)
DEPENDS=$(CFILES:.c=.d)

include $(REALTOPDIR)/src/Makefile.common

install: all
