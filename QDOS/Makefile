# ---------------------------------------------------------------
#| qxltool.c : Access QXL.WIN files from other operating systems |
#|                                                               |
#| (c) Jonathan Hudson, April 1998                               |
#| No Warranty                                                   |
# ---------------------------------------------------------------
VPATH = ../
srcdir = ..

DEFS = -DHAVE_CONFIG_H

QCC = qdos-gcc
# zlib is needed for the unzip command
LIBS = -lz -lm
QFLAGS = -O2 -I. -I$(VPATH) $(DEFS)

qxltool: qxltool.o
	$(QCC) $(QFLAGS) -o qxltool qxltool.o $(LIBS)
	ls -l qxltool
	qcp qxltool /ql/exe/qxltool

qxltool.o : ../qxltool.c ../qxltool.h $(CURDIR)/version.h
	$(QCC) $(QFLAGS) -c -o $@ $<

include $(srcdir)/version.mk

clean:
	rm -f *.o qxltool version.h

