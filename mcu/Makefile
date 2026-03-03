# SPDX-License-Identifier: GPL-2.0-or-later
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
LDFLAGS ?=

all: pcat2-mcu.o

pcat2-mcu.o: pcat2-mcu.c pcat2-mcu.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f *.o

.PHONY: all clean
