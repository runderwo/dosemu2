top_builddir=../../..
include $(top_builddir)/Makefile.conf

DOCSOURCES := $(notdir $(wildcard $(srcdir)/*.sgml))
TXT = $(DOCSOURCES:.sgml=.txt)
HTML = $(DOCSOURCES:.sgml=.html)

all:    $(TXT) $(HTML)

$(TXT): %.txt: %.sgml
	$(srcdir)/../tools/doSgmlTools.pl -t $<

$(HTML): %.html: %.sgml
	$(srcdir)/../tools/doSgmlTools.pl -h $<


clean:
	rm -f *.txt *.html *~ *.tex *.log *.aux *.toc *.dvi *.ps

install:
	mv *.txt $(REALTOPDIR)/doc
