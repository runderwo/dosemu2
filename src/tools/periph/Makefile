
top_builddir=../../..
include $(top_builddir)/Makefile.conf


D=$(REALTOPDIR)/etc
IDEST=/var/lib

CFILES=hdinfo.c mkhdimage.c putrom.c mkfatimage.c mkfatimage16.c \
    dexeconfig.c scsicheck.c dosctrl.c vbioscheck.c
SFILES=bootsect.S
SRC=$(CFILES) $(SFILES)
OBJ1=hdinfo mkhdimage putrom dexeconfig scsicheck dosctrl vbioscheck 
OBJ=$(OBJ1) $(BINPATH)/bin/mkfatimage $(BINPATH)/bin/mkfatimage16
SCRIPT=getrom

ALL_CPPFLAGS += -I.

all: $(OBJ)

$(BINPATH)/bin/mkfatimage: mkfatimage.o bootsect.o
	$(LD) $(ALL_LDFLAGS) $^ -o $@

$(BINPATH)/bin/mkfatimage16: mkfatimage16.o bootsect.o
	$(LD) $(ALL_LDFLAGS) $^ -o $@

$(OBJ1): %: %.o
	$(LD) $(ALL_LDFLAGS) $< -o $@

../../../etc/hdimage.dist: mkfatimage
	./mkfatimage -l DOSEMU $(REALTOPDIR)/$(THISVERSION)/commands/* >$@

install:
	install -d $(IDEST)/dosemu
	install -m 0755 -s mkhdimage putrom scsicheck dosctrl vbioscheck $(IDEST)/dosemu
	install -m 0755 $(SCRIPT) $(IDEST)/dosemu

clean::
	rm -f $(OBJ) *.o mkfatimage mkfatimage16
	rm -f *.out

realclean:: clean

dist:: $(SRC) $(SCRIPT)
	install -d $(DISTPATH)/periph
	install -m 0644 Makefile keyboard.c.diff $(SRC) $(DISTPATH)/periph
	install -m 0755 $(SCRIPT) $(DISTPATH)/periph

include $(REALTOPDIR)/src/Makefile.common
