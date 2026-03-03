# SPDX-License-Identifier: GPL-2.0-or-later
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
LDFLAGS ?=

MCU_DIR = ../mcu

all: mcu-dump mcu-probe

mcu-dump: mcu-dump.c $(MCU_DIR)/pcat2-mcu.c $(MCU_DIR)/pcat2-mcu.h
	$(CC) $(CFLAGS) -I$(MCU_DIR) -o $@ mcu-dump.c $(MCU_DIR)/pcat2-mcu.c $(LDFLAGS)

mcu-probe: mcu-probe.c
	$(CC) $(CFLAGS) -o $@ mcu-probe.c $(LDFLAGS)

clean:
	rm -f mcu-dump mcu-probe

.PHONY: all clean
