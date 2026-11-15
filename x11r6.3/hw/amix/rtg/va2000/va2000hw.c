/*
 * va2000hw.c -- the MNT VA2000 card for the X11R6.3 RTG layer: the card
 * record, the modes, the device, the two mappings and the mode registers.
 *
 * Copyright (c) 2025-2026 Antti Sokero
 * Copyright (c) 2026 Jussi Alanärä
 * SPDX-License-Identifier: MIT
 * See LICENSE in the repository root for the licence text.
 *
 * Derived from the X11R5 server's usr/x11r5/server/ddx/amix/rtg/va2000/
 * va2000hw.c: the mode table and its timings, the board probe through
 * SVGAIOCGetFBufSize and VA2IOC_GETFW, the bus and aperture detection, and
 * the register sequence that brings the mode up from user space.  Changed
 * for the R6.3 layer: the registers and the framebuffer are two mappings,
 * each checked against the mode, instead of one region with slack; closing
 * does not write the pass-through sequence (the monitor goes back to the
 * Amiga video through the monitor switch when the server ends, and through
 * the driver's own last close); the hand-built screen of the R5 driver is
 * the layer's cfb16 screen.
 */

#include <sys/types.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include "X.h"
#include "misc.h"
#include "os.h"
#include "../rtg.h"
#include "va2000.h"

/* ======================================================================= */
/* Modes: the R5 driver's table (timings from va2000_restest.c)            */
/* ======================================================================= */

static VA2000ModeRec va2000Modes[] = {
    /* name         w     h   hss   hse  hmax  vss  vse  vmax  clk */
    { "640x480",   640,  480,  656,  752,  800, 490, 492,  525,  1 },
    { "800x600",   800,  600,  840,  968, 1056, 601, 605,  628,  1 },
    { "1024x768", 1024,  768, 1048, 1184, 1328, 771, 777,  806,  0 },
    { "1280x720", 1280,  720, 1390, 1430, 1650, 725, 730,  750,  0 },
    { "1280x1024",1280, 1024, 1328, 1440, 1600,1025,1028, 1066,  3 },
    { "1920x1080",1920, 1080, 1992, 2000, 2287,1083,1088, 1109,  0 },
    { (char *) 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }
};

static VA2000ModePtr va2000Mode = &va2000Modes[1];     /* 800x600 */

VA2000ModePtr
va2000ModeByName(char *name)
{
    VA2000ModePtr m;

    for (m = va2000Modes; m->name; m++)
        if (strcmp(m->name, name) == 0)
            return m;
    return (VA2000ModePtr) 0;
}

static Bool
va2000SetMode(char *name)
{
    VA2000ModePtr m = va2000ModeByName(name);

    if (!m)
        return FALSE;
    va2000Mode = m;
    return TRUE;
}

static void
va2000ModeSize(int *w, int *h)
{
    *w = va2000Mode->w;
    *h = va2000Mode->h;
}

static char *
va2000BusName(int bus)
{
    if (bus == VA2000_BUS_Z2)
        return "Zorro II";
    if (bus == VA2000_BUS_Z3)
        return "Zorro III";
    return "bus unknown";
}

/*
** va2000PlanMaps -- what the mode needs from the board, before anything is
** mapped.
**
** fbSize is the driver's answer to SVGAIOCGetFBufSize: the aperture less
** the register area, 4 MB - 64 KB on a Zorro II board and 32 MB - 64 KB on a
** Zorro III one, or 0 from a driver without the ioctl -- then the R5
** driver's 2 MB is assumed.  The two mappings: the register page at offset
** 0, and the mode's framebuffer bytes at VA2000_FB_OFFSET, rounded up to a
** page and never past the aperture.  A mode bigger than the framebuffer is
** refused: 1920x1080 needs 4050 KB and a Zorro II board has 4032.
*/
int
va2000PlanMaps(VA2000ModePtr mode, long fbSize, VA2000MapPlan *plan)
{
    plan->fbSize = fbSize > 0L ? fbSize : 0L;
    if (plan->fbSize > 0L) {
        plan->aperture = plan->fbSize + VA2000_FB_OFFSET;
        plan->bus = plan->fbSize >= VA2000_Z3_MIN_SIZE ? VA2000_BUS_Z3
                                                       : VA2000_BUS_Z2;
    } else {
        plan->aperture = VA2000_FALLBACK_APERTURE;
        plan->bus = VA2000_BUS_UNKNOWN;
    }
    plan->fbAvail   = plan->aperture - VA2000_FB_OFFSET;
    plan->pitch     = (long) mode->w * 2L;
    plan->modeBytes = plan->pitch * (long) mode->h;
    plan->regMapLen = VA2000_REG_MAPLEN;
    plan->fbMapLen  = (plan->modeBytes + VA2000_MAP_ROUND - 1L) &
                      ~(VA2000_MAP_ROUND - 1L);
    if (plan->fbMapLen > plan->fbAvail)
        plan->fbMapLen = plan->fbAvail;
    if (plan->modeBytes > plan->fbAvail)
        return VA2000_PLAN_TOOBIG;
    return VA2000_PLAN_OK;
}

/* ======================================================================= */
/* The device                                                              */
/* ======================================================================= */

/*
** Is the node a VA2000?  open() alone does not say: -dev can name any node
** that opens (/dev/null, a tty, another card's driver).  Only the VA2000
** kernel driver answers VA2IOC_GETFW, ('V' << 8) | 1: every version of it
** does, from the first one (which had no SVGAIOCGetFBufSize and no
** SVGAIOCGetBoardData) on, and it is a pure read of the firmware register
** answered through the return value.  No other Amix driver uses the 'V'
** range: the ZZ9000/Z3660 driver's own calls are ('Z' << 8) | n and it
** answers EINVAL here, ttys and disks EINVAL or ENOTTY, /dev/null an error.
** The SVGAIOC calls do not identify the card, since other drivers answer
** some of them (the ZZ9000 driver the monitor switch).  The argument is 0:
** a foreign driver that took the number for a pointer could copy nothing
** in or out.  Returns the firmware number, or -1 with errno set.
*/
static int
va2000Identify(int fd)
{
    return ioctl(fd, VA2IOC_GETFW, 0);
}

/* Is the card there?  Open, ask, close; nothing is mapped or written. */
static Bool
va2000Probe(void)
{
    int  fd;
    Bool ok;

    fd = open(rtgCardDevice(&va2000CardOps), O_RDWR);
    if (fd < 0)
        return FALSE;
    ok = va2000Identify(fd) != -1;
    (void) close(fd);
    return ok;
}

/*
** The mode, from user space: the R5 driver's sequence (from va2000_test.c),
** proven on the card.  The pan pointer must be zeroed for RTG; the second
** modeline write clears display glitches (va2000_blit.c).
*/
static void
va2000WriteMode(volatile unsigned char *regs, VA2000ModePtr m)
{
    VA2000_WRITEREG(regs, VA2000_REG_HTOTAL,        m->hss);
    VA2000_WRITEREG(regs, VA2000_REG_HSYNC_START,   m->hse);
    VA2000_WRITEREG(regs, VA2000_REG_HSYNC_END,     m->hmax);
    VA2000_WRITEREG(regs, VA2000_REG_VTOTAL,        m->vss);
    VA2000_WRITEREG(regs, VA2000_REG_VSYNC_START,   m->vse);
    VA2000_WRITEREG(regs, VA2000_REG_VSYNC_END,     m->vmax);
    VA2000_WRITEREG(regs, VA2000_REG_PIX_CLK,       m->clk);
    VA2000_WRITEREG(regs, VA2000_REG_COLORMODE,     VA2000_COLORMODE_16BIT);
    VA2000_WRITEREG(regs, VA2000_REG_PITCH,         m->w);
    VA2000_WRITEREG(regs, VA2000_REG_PITCH_SHF,     9);
    VA2000_WRITEREG(regs, VA2000_REG_WIDTH,         m->w);
    VA2000_WRITEREG(regs, VA2000_REG_HEIGHT,        m->h);
    VA2000_WRITEREG(regs, VA2000_REG_SCALEMODE,     0);
    VA2000_WRITEREG(regs, VA2000_REG_SAFE_X2,       0x1e0);
    VA2000_WRITEREG(regs, VA2000_REG_RAM_FETCH,     0x17);
    VA2000_WRITEREG(regs, VA2000_REG_FETCH_PREROLL, 0x1e0);
    VA2000_WRITEREG(regs, VA2000_REG_PAN_HI,        VA2000_PAN_RTG_HI);
    VA2000_WRITEREG(regs, VA2000_REG_PAN_LO,        VA2000_PAN_RTG_LO);
    VA2000_WRITEREG(regs, VA2000_REG_CAPTURE,       VA2000_CAPTURE_RTG);
    VA2000_WRITEREG(regs, VA2000_REG_HTOTAL,        m->hss);
    VA2000_WRITEREG(regs, VA2000_REG_HSYNC_START,   m->hse);
    VA2000_WRITEREG(regs, VA2000_REG_HSYNC_END,     m->hmax);
    VA2000_WRITEREG(regs, VA2000_REG_VTOTAL,        m->vss);
    VA2000_WRITEREG(regs, VA2000_REG_VSYNC_START,   m->vse);
    VA2000_WRITEREG(regs, VA2000_REG_VSYNC_END,     m->vmax);
}

/*
** initHW: open the device, check that it is a VA2000, ask what board it
** is, map the register page and the mode's framebuffer, write the mode,
** clear the screen, test the blitter.  FALSE = no screen, everything undone.
*/
static Bool
va2000InitHW(rtgScreenPtr pRTG)
{
    va2000ScreenPtr pVA;
    VA2000MapPlan   plan;
    caddr_t         regs, fb;
    int             fd, fw, v;

    pVA = (va2000ScreenPtr) xalloc(sizeof(va2000ScreenRec));
    if (!pVA) {
        ErrorF("va2000: out of memory\n");
        return FALSE;
    }
    memset((char *) pVA, 0, sizeof(*pVA));

    fd = open(rtgCardDevice(&va2000CardOps), O_RDWR);
    if (fd < 0) {
        ErrorF("va2000: cannot open %s (%s)\n", rtgCardDevice(&va2000CardOps),
               strerror(errno));
        xfree(pVA);
        return FALSE;
    }

    /* a VA2000, before anything is mapped or written */
    fw = va2000Identify(fd);
    if (fw == -1) {
        ErrorF("va2000: %s is not a VA2000 (%s)\n",
               rtgCardDevice(&va2000CardOps), strerror(errno));
        (void) close(fd);
        xfree(pVA);
        return FALSE;
    }

    /* the board, before anything is touched: the line to ask a tester for */
    v = ioctl(fd, SVGAIOCGetFBufSize, 0);
    (void) va2000PlanMaps(va2000Mode, v > 0 ? (long) v : 0L, &plan);
    ErrorF("va2000: firmware %d, framebuffer %ld KB, %s; mode %s\n",
           fw > 0 ? fw : 0, plan.fbAvail >> 10, va2000BusName(plan.bus),
           va2000Mode->name);
    if (plan.modeBytes > plan.fbAvail) {
        ErrorF("va2000: mode %s needs %ld KB, the board offers %ld KB%s\n",
               va2000Mode->name, plan.modeBytes >> 10, plan.fbAvail >> 10,
               plan.bus == VA2000_BUS_Z2 ? " (Zorro II)" : "");
        (void) close(fd);
        xfree(pVA);
        return FALSE;
    }

    regs = (caddr_t) mmap((caddr_t) 0, (size_t) plan.regMapLen,
                          PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t) 0);
    if (regs == (caddr_t) -1) {
        ErrorF("va2000: cannot map the registers (%s)\n", strerror(errno));
        (void) close(fd);
        xfree(pVA);
        return FALSE;
    }
    fb = (caddr_t) mmap((caddr_t) 0, (size_t) plan.fbMapLen,
                        PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                        (off_t) VA2000_FB_OFFSET);
    if (fb == (caddr_t) -1) {
        ErrorF("va2000: cannot map %ld KB of framebuffer (%s)\n",
               plan.fbMapLen >> 10, strerror(errno));
        (void) munmap(regs, (size_t) plan.regMapLen);
        (void) close(fd);
        xfree(pVA);
        return FALSE;
    }

    pVA->fd      = fd;
    pVA->regMap  = regs;
    pVA->regLen  = plan.regMapLen;
    pVA->regBase = (volatile unsigned char *) regs;
    pVA->fbMap   = (unsigned char *) fb;
    pVA->fbLen   = plan.fbMapLen;
    pVA->width   = va2000Mode->w;
    pVA->height  = va2000Mode->h;
    pVA->held    = VA2000_HELD_NONE;

    va2000WriteMode(pVA->regBase, va2000Mode);
    memset((char *) pVA->fbMap, 0, (size_t) plan.modeBytes);

    pRTG->frameBase    = pVA->fbMap;
    pRTG->fbOffset     = 0;
    pRTG->fbBase       = (unsigned short *) pVA->fbMap;
    pRTG->width        = va2000Mode->w;
    pRTG->height       = va2000Mode->h;
    pRTG->pitch        = (int) plan.pitch;
    pRTG->bitsPerPixel = 16;
    pRTG->depth        = 16;
    pRTG->cardPrivate  = (unsigned char *) pVA;
    pRTG->devFd        = fd;

    va2000BlitStart(pRTG);
    return TRUE;
}

/*
** closeHW: unmap and close (the layer has flushed).  No register is
** written: at a server reset the picture stays, and when the server ends the
** layer has already sent the monitor switch, and the driver's last close puts
** the Amiga video back in any case.
*/
static void
va2000CloseHW(rtgScreenPtr pRTG)
{
    va2000ScreenPtr pVA = GetVA2000Screen(pRTG);

    if (!pVA)
        return;
    va2000BlitStop(pRTG);
    (void) munmap((caddr_t) pVA->fbMap, (size_t) pVA->fbLen);
    (void) munmap(pVA->regMap, (size_t) pVA->regLen);
    (void) close(pVA->fd);
    xfree(pVA);
    pRTG->cardPrivate = (unsigned char *) 0;
    pRTG->frameBase   = (unsigned char *) 0;
    pRTG->fbBase      = (unsigned short *) 0;
    pRTG->devFd       = -1;
}

/*
** The monitor switch: the VA2000 driver reads an unsigned short from the
** address it is given (SVGAIOCSetMonitorSwitch); an int would read as 0 on
** this big-endian machine.  The fd is the layer's own; errors are ignored.
*/
static void
va2000MonitorSwitch(int fd, int which)
{
    unsigned short v = (unsigned short) which;

    (void) ioctl(fd, SVGAIOCSetMonitorSwitch, &v);
}

/*
** The card record (rtg.h), listed in rtgcards.c under RTG_VA2000.
** -blitmin's default is a guess until it is measured on the card: the R5
** driver blitted every window fill, but never PolyFillRect, which the R6.3
** layer also sends to fillBox.
*/
rtgCardOps va2000CardOps = {
    RTG_CARD_VERSION,
    VA2000_NAME,
    VA2000_DEV,
    VA2000_MODES,
    {
        16, 16,                 /* depth, bitsPerPixel */
        0xf800, 0x07e0, 0x001f, /* RGB565 */
        6,                      /* bitsPerRGB */
        1024,                   /* -blitmin's default, pixels */
        /* no move16 bursts: not proven on the card's Zorro III bus */
        RTG_CAP_BLITTER | RTG_CAP_USERREGS | RTG_CAP_NOMOVE16,
    },
    va2000Probe,
    va2000SetMode,
    va2000ModeSize,
    va2000InitHW,
    va2000CloseHW,
    va2000FillBox,
    va2000CopyBox,
    va2000Flush,
    va2000MonitorSwitch,
};
