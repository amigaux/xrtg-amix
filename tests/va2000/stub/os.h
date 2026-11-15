/* os.h -- host test stub: the X server's allocator and ErrorF.
 * Copyright (c) 2026 Jussi Alanärä
 * SPDX-License-Identifier: MIT */
#ifndef VA2000_TEST_OS_H
#define VA2000_TEST_OS_H
#include <stdlib.h>
#define xalloc(n)  malloc(n)
#define xfree(p)   free(p)
extern void ErrorF(const char *fmt, ...);
#endif
