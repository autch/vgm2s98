# gcc / host build.  For a PC-98 DOS binary, omit optional bulk:
#   make ZLIB=0 GD3=0
# ZLIB=0  no .vgz (no zlib); decompress on the host first
# GD3=0   no GD3 -> [S98] tag (conversion cost not worth it on DOS)
CC = gcc
CFLAGS = -std=c89 -pedantic -Wall -Wextra -O2
ZLIB ?= 1
GD3  ?= 1
ifeq ($(ZLIB),1)
CFLAGS += -DHAVE_ZLIB
LDLIBS = -lz
endif
ifeq ($(GD3),1)
CFLAGS += -DHAVE_GD3
endif

OBJS = main.o errmsg.o vgm.o s98.o convert.o
ifeq ($(GD3),1)
OBJS += gd3.o
endif

vgm2s98: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(OBJS): vgm2s98.h
main.o: vgm.h s98.h convert.h
ifeq ($(GD3),1)
main.o: gd3.h
endif
convert.o: vgm.h s98.h convert.h
gd3.o: vgm.h gd3.h
vgm.o: vgm.h
s98.o: s98.h

clean:
	rm -f vgm2s98 $(OBJS)

.PHONY: clean
