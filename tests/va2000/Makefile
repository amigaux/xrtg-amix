# Host test harness for the X11R6.3 VA2000 card driver.
#
#   make -C tests/va2000 check            the default build and the
#                                         VA2000_STATS build, both run
#   make -C tests/va2000 check RTG_H=...  against another rtg.h (e.g. the
#                                         patched X11R6.3 tree's)
#
# Copyright (c) 2026 Jussi Alanärä
# SPDX-License-Identifier: MIT

CC      ?= cc
CARD    := ../../x11r6.3/hw/amix/rtg/va2000
RTG_H   ?= rtg.h
B       := build
WARN    := -Wall -Wextra -Wmissing-prototypes -Wpointer-arith -Wshadow -Werror
CFLAGS  := -std=gnu11 -O2 -g -U_FORTIFY_SOURCE $(WARN)
# the card files: the stubs (their ../rtg.h is $(B)/rtg.h), the model's
# syscalls and register access
CARDFLAGS := -I$(B)/stub -I$(CARD) -DVA2000_HOSTSIM -DVA2000_SIM_RENAME -include sim.h -I.

SRCS := $(CARD)/va2000hw.c $(CARD)/va2000blit.c

all: $(B)/test_va2000 $(B)/test_va2000_stats

$(B)/stub/misc.h: stub/X.h stub/misc.h stub/os.h $(RTG_H)
	mkdir -p $(B)/stub
	cp stub/X.h stub/misc.h stub/os.h $(B)/stub/
	cp $(RTG_H) $(B)/rtg.h

$(B)/test_va2000: test_va2000.c sim.h $(SRCS) $(CARD)/va2000.h $(B)/stub/misc.h
	$(CC) $(CFLAGS) $(CARDFLAGS) -c $(CARD)/va2000hw.c -o $(B)/hw.o
	$(CC) $(CFLAGS) $(CARDFLAGS) -c $(CARD)/va2000blit.c -o $(B)/blit.o
	$(CC) $(CFLAGS) -I$(B)/stub -I$(B) -I$(CARD) -DVA2000_HOSTSIM -c test_va2000.c -o $(B)/test.o
	$(CC) -o $@ $(B)/test.o $(B)/hw.o $(B)/blit.o

$(B)/test_va2000_stats: test_va2000.c sim.h $(SRCS) $(CARD)/va2000.h $(B)/stub/misc.h
	$(CC) $(CFLAGS) $(CARDFLAGS) -DVA2000_STATS -c $(CARD)/va2000hw.c -o $(B)/hw_s.o
	$(CC) $(CFLAGS) $(CARDFLAGS) -DVA2000_STATS -c $(CARD)/va2000blit.c -o $(B)/blit_s.o
	$(CC) $(CFLAGS) -I$(B)/stub -I$(B) -I$(CARD) -DVA2000_HOSTSIM -DVA2000_STATS -c test_va2000.c -o $(B)/test_s.o
	$(CC) -o $@ $(B)/test_s.o $(B)/hw_s.o $(B)/blit_s.o

check: all
	$(B)/test_va2000
	$(B)/test_va2000_stats

clean:
	rm -rf $(B)

.PHONY: all check clean
