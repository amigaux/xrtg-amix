/*
 * sim.h -- the host test harness's model of the MNT VA2000 and its kernel
 * driver, as the card files see it.  Force-included (-include sim.h) into
 * the card files when they are built for the host: open, close, ioctl, mmap
 * and munmap are renamed onto the model, and the registers go through
 * VA2000_HOSTSIM's two calls (va2000.h).  The renames apply only with
 * VA2000_SIM_RENAME; the harness itself includes this file for the
 * prototypes.
 *
 * Copyright (c) 2026 Jussi Alanärä
 * SPDX-License-Identifier: MIT
 */

#ifndef VA2000_SIM_H
#define VA2000_SIM_H

#include <stddef.h>
#include <sys/types.h>

extern int   simOpen(const char *path, int flags, ...);
extern int   simClose(int fd);
extern void *simMmap(void *addr, size_t len, int prot, int flags, int fd,
                     off_t off);
extern int   simMunmap(void *addr, size_t len);

/* ioctl: the model also learns the size of what a pointer argument points
 * to, so the monitor switch's unsigned short can be told from an int */
extern int   simIoctl(int fd, int req, long arg, int argsize);

#ifdef VA2000_SIM_RENAME
#define open    simOpen
#define close   simClose
#define mmap    simMmap
#define munmap  simMunmap
#define ioctl(fd, req, arg) \
    simIoctl((fd), (req), (long) (arg), \
             _Generic((arg), unsigned short *: 2, short *: 2, \
                             int *: 4, unsigned int *: 4, long *: 8, \
                             default: 0))
#endif

#endif
