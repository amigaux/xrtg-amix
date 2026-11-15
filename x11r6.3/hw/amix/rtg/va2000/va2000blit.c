/*
 * va2000blit.c -- the MNT VA2000 card for the X11R6.3 RTG layer: the
 * blitter, its start-up self-test, and the card's fillBox, copyBox and flush.
 *
 * Copyright (c) 2025-2026 Antti Sokero
 * Copyright (c) 2026 Jussi Alanärä
 * SPDX-License-Identifier: MIT
 * See LICENSE in the repository root for the licence text.
 *
 * Derived from the X11R5 server's usr/x11r5/server/ddx/amix/rtg/va2000/
 * va2000draw.c: the bounded blitter wait (va2000BlitWait), the fill and copy
 * register sequences (blitFill, blitCopy) and the copy direction logic
 * (copyVRAMBox).  Changed for the R6.3 layer: an operation is started and
 * left running (the layer's flush waits for it), so the CPU is not held
 * waiting after every blit; a wait that reaches its bound gives the blitter
 * up for the rest of the server's life and draws what it held on the CPU;
 * the blitter is tested once at start-up; the counters and the slow-wait log
 * are built only with VA2000_STATS.
 */

#include <sys/types.h>
#include "X.h"
#include "misc.h"
#include "os.h"
#include "../rtg.h"
#include "va2000.h"

/*
** The blitter was lost in this process (a timeout, or a failed self-test).
** It stays lost across server resets: the card does not try it again.
*/
static int  va2000BlitDead = 0;
static long va2000BlitTimeouts = 0;

#ifdef VA2000_STATS
static long va2000StatBlits = 0;        /* operations started              */
static long va2000StatPolls = 0;        /* status reads spent waiting      */
static long va2000StatSlow = 0;         /* waits of VA2000_BLIT_SLOW polls */
static long va2000StatWorst = 0;        /* the longest wait, in polls      */
static long va2000StatFillPx = 0;       /* pixels filled by the blitter    */
static long va2000StatCopyPx = 0;       /* pixels copied by the blitter    */
#endif

/*
** Wait until the blitter is idle, at most VA2000_BLIT_TIMEOUT status reads.
** TRUE = idle.  One read when it already is.
*/
static Bool
va2000BlitIdle(va2000ScreenPtr pVA)
{
    long n = 0;

    while (VA2000_READREG(pVA->regBase, VA2000_BLT_ENABLE) != 0) {
        if (++n >= VA2000_BLIT_TIMEOUT) {
            va2000BlitTimeouts++;
            return FALSE;
        }
    }
#ifdef VA2000_STATS
    va2000StatPolls += n;
    if (n > va2000StatWorst)
        va2000StatWorst = n;
    if (n >= VA2000_BLIT_SLOW && ++va2000StatSlow <= VA2000_BLIT_LOG)
        ErrorF("va2000: slow blit, %ld polls\n", n);
#endif
    return TRUE;
}

/*
** The blitter is gone: no more work for it, in this server's life.  What it
** held is drawn on the CPU (exact for a fill and for a copy whose boxes do
** not overlap; an overlapping copy the blitter half did can leave damage
** that the next expose repairs), and the layer is told (rtgBlitMin = 0).
*/
static void
va2000BlitLost(rtgScreenPtr pRTG, char *why)
{
    va2000ScreenPtr pVA = GetVA2000Screen(pRTG);
    int             held = pVA->held;

    pVA->blitOn = FALSE;
    pVA->held = VA2000_HELD_NONE;
    if (!va2000BlitDead)
        ErrorF("va2000: %s; running without the blitter\n", why);
    va2000BlitDead = 1;
    va2000CardOps.caps.flags &= ~RTG_CAP_BLITTER;
    va2000CardOps.caps.blitMin = 0;
    rtgBlitMin = 0;
    if (held == VA2000_HELD_FILL)
        rtgCpuFill(pRTG, pVA->hx, pVA->hy, pVA->hx + pVA->hw,
                   pVA->hy + pVA->hh, pVA->hpixel);
    else if (held == VA2000_HELD_COPY)
        rtgCpuCopy(pRTG, pVA->hx, pVA->hy, pVA->hw, pVA->hh,
                   pVA->hdx, pVA->hdy);
}

/*
** Start a fill of the w x h box at (x,y) and leave it running: the R5
** driver's blitFill sequence.  The base is the SDRAM word address of the
** box's first row; the coordinates are inclusive.
*/
static void
va2000StartFill(va2000ScreenPtr pVA, int x, int y, int w, int h,
                unsigned long pixel)
{
    volatile unsigned char *regs = pVA->regBase;
    unsigned long base = (unsigned long) y * (unsigned long) pVA->width;

    VA2000_WRITEREG(regs, VA2000_BLT_BASE_HI,   base >> 16);
    VA2000_WRITEREG(regs, VA2000_BLT_BASE_LO,   base & 0xffff);
    VA2000_WRITEREG(regs, VA2000_BLT_ROWPITCH,  pVA->width);
    VA2000_WRITEREG(regs, VA2000_BLT_COLORMODE, 1);
    VA2000_WRITEREG(regs, VA2000_BLT_RGB16,     pixel & 0xffff);
    VA2000_WRITEREG(regs, VA2000_BLT_X1,        x);
    VA2000_WRITEREG(regs, VA2000_BLT_Y1,        y);
    VA2000_WRITEREG(regs, VA2000_BLT_X2,        x + w - 1);
    VA2000_WRITEREG(regs, VA2000_BLT_Y2,        y + h - 1);
    VA2000_WRITEREG(regs, VA2000_BLT_ENABLE,    VA2000_BLT_FILL);
    pVA->held   = VA2000_HELD_FILL;
    pVA->hx     = x;
    pVA->hy     = y;
    pVA->hw     = w;
    pVA->hh     = h;
    pVA->hpixel = pixel;
#ifdef VA2000_STATS
    va2000StatBlits++;
    va2000StatFillPx += (long) w * h;
#endif
}

/* The box is inside the mode and not empty. */
#define VA2000_INSIDE(pVA, x, y, w, h) \
    ((x) >= 0 && (y) >= 0 && (w) > 0 && (h) > 0 && \
     (x) + (w) <= (pVA)->width && (y) + (h) <= (pVA)->height)

/*
** fillBox: fill [x1,x2) x [y1,y2) with pixel.  The previous operation is
** waited for, this one started and left running.  FALSE = not taken.
*/
Bool
va2000FillBox(rtgScreenPtr pRTG, int x1, int y1, int x2, int y2,
              unsigned long pixel)
{
    va2000ScreenPtr pVA = GetVA2000Screen(pRTG);

    if (!pVA || !pVA->blitOn || !VA2000_INSIDE(pVA, x1, y1, x2 - x1, y2 - y1))
        return FALSE;
    if (!va2000BlitIdle(pVA)) {
        va2000BlitLost(pRTG, "blitter timeout");
        return FALSE;
    }
    va2000StartFill(pVA, x1, y1, x2 - x1, y2 - y1, pixel);
    return TRUE;
}

/*
** copyBox: copy the w x h box at (sx,sy) to (dx,dy); the boxes may overlap.
**
** The R5 driver's copyVRAMBox: forward (top row first, left column first)
** when the destination is above the source, or on the same rows and not to
** its right; otherwise reverse (bottom row first, right column first), with
** the starting rows' addresses in the base registers and the coordinates
** given end to start.  With different rows every source row is read before
** it is overwritten whichever way the columns run; on the same rows the
** column order decides, and the two forms cover both.
*/
Bool
va2000CopyBox(rtgScreenPtr pRTG, int sx, int sy, int w, int h, int dx, int dy)
{
    va2000ScreenPtr         pVA = GetVA2000Screen(pRTG);
    volatile unsigned char *regs;
    unsigned long           dbase, sbase, pitch;
    int                     x1, y1, x2, y2, x3, y3, x4, y4;

    if (!pVA || !pVA->blitOn || !VA2000_INSIDE(pVA, sx, sy, w, h) ||
        !VA2000_INSIDE(pVA, dx, dy, w, h))
        return FALSE;
    if (!va2000BlitIdle(pVA)) {
        va2000BlitLost(pRTG, "blitter timeout");
        return FALSE;
    }

    pitch = (unsigned long) pVA->width;
    if (dy < sy || (dy == sy && dx <= sx)) {
        dbase = (unsigned long) dy * pitch;
        sbase = (unsigned long) sy * pitch;
        x1 = dx;          y1 = dy;          x2 = dx + w - 1;  y2 = dy + h - 1;
        x3 = sx;          y3 = sy;          x4 = sx + w - 1;  y4 = sy + h - 1;
    } else {
        dbase = (unsigned long) (dy + h - 1) * pitch;
        sbase = (unsigned long) (sy + h - 1) * pitch;
        x1 = dx + w - 1;  y1 = dy + h - 1;  x2 = dx;          y2 = dy;
        x3 = sx + w - 1;  y3 = sy + h - 1;  x4 = sx;          y4 = sy;
    }

    regs = pVA->regBase;
    VA2000_WRITEREG(regs, VA2000_BLT_BASE_HI,   dbase >> 16);
    VA2000_WRITEREG(regs, VA2000_BLT_BASE_LO,   dbase & 0xffff);
    VA2000_WRITEREG(regs, VA2000_BLT_BASE2_HI,  sbase >> 16);
    VA2000_WRITEREG(regs, VA2000_BLT_BASE2_LO,  sbase & 0xffff);
    VA2000_WRITEREG(regs, VA2000_BLT_ROWPITCH,  pVA->width);
    VA2000_WRITEREG(regs, VA2000_BLT_COLORMODE, 1);
    VA2000_WRITEREG(regs, VA2000_BLT_X1,        x1);
    VA2000_WRITEREG(regs, VA2000_BLT_Y1,        y1);
    VA2000_WRITEREG(regs, VA2000_BLT_X2,        x2);
    VA2000_WRITEREG(regs, VA2000_BLT_Y2,        y2);
    VA2000_WRITEREG(regs, VA2000_BLT_X3,        x3);
    VA2000_WRITEREG(regs, VA2000_BLT_Y3,        y3);
    VA2000_WRITEREG(regs, VA2000_BLT_X4,        x4);
    VA2000_WRITEREG(regs, VA2000_BLT_Y4,        y4);
    VA2000_WRITEREG(regs, VA2000_BLT_ENABLE,    VA2000_BLT_COPY);
    pVA->held = VA2000_HELD_COPY;
    pVA->hx   = sx;
    pVA->hy   = sy;
    pVA->hw   = w;
    pVA->hh   = h;
    pVA->hdx  = dx;
    pVA->hdy  = dy;
#ifdef VA2000_STATS
    va2000StatBlits++;
    va2000StatCopyPx += (long) w * h;
#endif
    return TRUE;
}

/* flush: return when the operation the blitter holds has finished. */
void
va2000Flush(rtgScreenPtr pRTG)
{
    va2000ScreenPtr pVA = GetVA2000Screen(pRTG);

    if (!pVA || pVA->held == VA2000_HELD_NONE)
        return;
    if (va2000BlitIdle(pVA))
        pVA->held = VA2000_HELD_NONE;
    else
        va2000BlitLost(pRTG, "blitter timeout");
}

/*
** The start-up self-test, at every initHW: the blitter must be idle, and a
** 1x1 fill at (0,0) must finish within the bound and reach the framebuffer
** (mapped noncacheable, so the read sees the card's memory).  Otherwise the
** server runs without the blitter, and says so once.  The screen has just
** been cleared, and the pixel is put back.
*/
void
va2000BlitStart(rtgScreenPtr pRTG)
{
    va2000ScreenPtr          pVA = GetVA2000Screen(pRTG);
    volatile unsigned short *fb = (volatile unsigned short *) pVA->fbMap;
    char                    *why = (char *) 0;

    pVA->blitOn = FALSE;
    pVA->held = VA2000_HELD_NONE;
    if (va2000BlitDead || !(va2000CardOps.caps.flags & RTG_CAP_BLITTER)) {
        rtgBlitMin = 0;
        return;
    }
    if (!va2000BlitIdle(pVA)) {
        why = "the blitter stays busy at start-up";
    } else {
        va2000StartFill(pVA, 0, 0, 1, 1, VA2000_TEST_PIXEL);
        pVA->held = VA2000_HELD_NONE;   /* nothing to redraw if it fails */
        if (!va2000BlitIdle(pVA))
            why = "the blitter's self-test fill did not finish";
        else if (fb[0] != VA2000_TEST_PIXEL)
            why = "the blitter's self-test fill did not reach the framebuffer";
        fb[0] = 0;
    }
    if (why) {
        va2000BlitLost(pRTG, why);
        return;
    }
    pVA->blitOn = TRUE;
}

/* At closeHW: nothing may be left running; the counters, if built. */
void
va2000BlitStop(rtgScreenPtr pRTG)
{
    va2000ScreenPtr pVA = GetVA2000Screen(pRTG);

    if (pVA->held != VA2000_HELD_NONE)
        va2000Flush(pRTG);
    if (va2000BlitTimeouts)
        ErrorF("va2000: %ld blitter timeouts\n", va2000BlitTimeouts);
#ifdef VA2000_STATS
    if (va2000StatBlits)
        ErrorF("va2000: %ld blits (%ld px filled, %ld px copied), "
               "%ld polls waiting, %ld slow, worst %ld polls\n",
               va2000StatBlits, va2000StatFillPx, va2000StatCopyPx,
               va2000StatPolls, va2000StatSlow, va2000StatWorst);
#endif
}
