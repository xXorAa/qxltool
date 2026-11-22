# ---------------------------------------------------------------
#| qxltool.c : Access QXL.WIN files from other operating systems |
#|                                                               |
#| (c) Jonathan Hudson, April 1998                               |
#| No Warranty                                                   |
# ---------------------------------------------------------------
VPATH = ../
srcdir = ..

DCC = gcc-go32

# These options are mainly for Unix 
DEFS = -DHAVE_CONFIG_H

# zlib is needed for the unzip command
LIBS = -lz

CFLAGS = -O3 -Wall -s -I. -I$(VPATH)  

qxltool: qxltool.o
	$(DCC) -s -o qxltool qxltool.o $(LIBS)
	rm qxltool

qxltool.o : qxltool.c qxltool.h $(CURDIR)/version.h
	$(DCC) $(CFLAGS) $(DEFS) -c -o $@ $<

qxltool.exe : qxltool

include $(srcdir)/version.mk

clean:
	rm -f *.o *.exe *~ version.h


