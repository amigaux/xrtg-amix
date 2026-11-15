/*
 * va2000.h -- the MNT VA2000 card driver for the X11R6.3 RTG layer (rtg.h).
 *
 * Copyright (c) 2025-2026 Antti Sokero
 * Copyright (c) 2026 Jussi Alanärä
 * SPDX-License-Identifier: MIT
 * See LICENSE in the repository root for the licence text.
 *
 * Derived from the X11R5 server's usr/x11r5/server/ddx/amix/rtg/va2000/
 * va2000.h: the device and memory map, the ioctl numbers, the mode record,
 * the register offsets and the blitter wait bounds.  The SVGAIOC numbers
 * follow svgaioctl.h by Georg Hessmann and Klaus Burckert, as in that file
 * and in the kernel driver (va2000-amix, src/va2000.h).
 */

#ifndef VA2000_H
#define VA2000_H

/*
** The card behind the layer: 16-bit TrueColor (RGB565) on a framebuffer
** mapped into the server, and a blitter whose registers are mapped into the
** server as well (RTG_CAP_USERREGS).  The kernel driver (/dev/va2000) finds
** the board, answers a few ioctls, maps pages and puts the Amiga video back
** on its last close; the mode is written here, from user space, which is how
** the card has always been brought up (the driver says RTG activation needs
** a user-space write first).
**
** Memory map, relative to the board's AutoConfig base, in both bus modes:
**   +0x000000  registers (one 4 KiB page; every register used is below 0x800)
**   +0x010000  framebuffer, to the end of the aperture
** The aperture is 4 MB in Zorro II and 32 MB in Zorro III; the driver says
** which (SVGAIOCGetFBufSize).
*/

#define VA2000_NAME         "va2000"
#define VA2000_DEV          "/dev/va2000"
#define VA2000_MODES        "640x480 800x600 1024x768 1280x720 1280x1024 1920x1080"

#define VA2000_REG_MAPLEN   0x1000L         /* the register page              */
#define VA2000_FB_OFFSET    0x10000L        /* the framebuffer's mmap offset  */
#define VA2000_MAP_ROUND    0x1000L         /* mapping lengths round to this:  */
                                            /* a page on both kernel lines     */
                                            /* (2 KiB on the 68030, 4 KiB on   */
                                            /* the 68040/68060)                */
#define VA2000_FALLBACK_APERTURE 0x200000L  /* a driver without               */
                                            /* SVGAIOCGetFBufSize: assume 2 MB */
#define VA2000_Z3_MIN_SIZE  0x800000L       /* a framebuffer this big or more: */
                                            /* the board came up in Zorro III  */

/*
** Driver ioctls.  Numbers as in va2000-amix/src/va2000.h.  GetFBufSize and
** GETFW answer through the ioctl's return value; SetMonitorSwitch reads an
** unsigned short from the address it is given.
*/
#define VA2000_SVGAIOC          0xe300
#define VA2000_VA2IOC           ('V' << 8)
#ifndef SVGAIOCGetFBufSize
#define SVGAIOCGetFBufSize      (VA2000_SVGAIOC | 0x04)
#endif
#ifndef VA2IOC_GETFW
#define VA2IOC_GETFW            (VA2000_VA2IOC | 1)
#endif

#define VA2000_BUS_UNKNOWN  0
#define VA2000_BUS_Z2       1
#define VA2000_BUS_Z3       2

/* ======================================================================= */
/* = Modes                                                                = */
/* ======================================================================= */

typedef struct _VA2000ModeRec {
    char *name;     /* "800x600" etc.                                       */
    int   w;        /* width in pixels                                      */
    int   h;        /* height in pixels                                     */
    int   hss;      /* H sync start (HTOTAL register)                       */
    int   hse;      /* H sync end   (HSYNC_START register)                  */
    int   hmax;     /* H max        (HSYNC_END register)                    */
    int   vss;      /* V sync start (VTOTAL register)                       */
    int   vse;      /* V sync end   (VSYNC_START register)                  */
    int   vmax;     /* V max        (VSYNC_END register)                    */
    int   clk;      /* pixel clock: 0=75MHz, 1=40MHz, 3=100MHz              */
} VA2000ModeRec, *VA2000ModePtr;

/*
** What a mode needs from a board, worked out before anything is mapped.
** va2000PlanMaps() fills it from the mode and the driver's answer to
** SVGAIOCGetFBufSize (0 = no answer).
*/
typedef struct _VA2000MapPlan {
    long fbSize;        /* the driver's framebuffer size, 0 = unknown        */
    int  bus;           /* VA2000_BUS_*                                      */
    long aperture;      /* bytes the board decodes, registers included       */
    long fbAvail;       /* framebuffer bytes: aperture - VA2000_FB_OFFSET    */
    long modeBytes;     /* w * h * 2                                         */
    long pitch;         /* bytes a row: w * 2                                */
    long regMapLen;     /* the register mapping, at offset 0                 */
    long fbMapLen;      /* the framebuffer mapping, at VA2000_FB_OFFSET      */
} VA2000MapPlan;

#define VA2000_PLAN_OK      0
#define VA2000_PLAN_TOOBIG  1   /* the mode does not fit the framebuffer     */

/* ======================================================================= */
/* = Registers (byte offsets from the register page)                     = */
/* ======================================================================= */

#define VA2000_REG_SCALEMODE     0x04   /* scale mode (0=normal)            */
#define VA2000_REG_WIDTH         0x06   /* display width in pixels          */
#define VA2000_REG_HEIGHT        0x08   /* display height in pixels         */
#define VA2000_REG_COLORMODE     0x0e   /* color mode                       */
#define VA2000_COLORMODE_16BIT   1      /* RGB565                           */
#define VA2000_REG_SAFE_X2       0x14   /* display timing (0x1e0)           */
#define VA2000_REG_RAM_FETCH     0x18   /* RAM fetch timing (0x17)          */
#define VA2000_REG_FETCH_PREROLL 0x1a   /* fetch preroll (0x1e0)            */
#define VA2000_REG_PAN_HI        0x38   /* display start address high word  */
#define VA2000_REG_PAN_LO        0x3a   /* display start address low word   */
#define VA2000_PAN_RTG_HI        0
#define VA2000_PAN_RTG_LO        0
#define VA2000_REG_CAPTURE       0x4e   /* 0=RTG active, 1=passthrough      */
#define VA2000_CAPTURE_RTG       0
#define VA2000_REG_PITCH         0x58   /* framebuffer pitch in pixels      */
#define VA2000_REG_PITCH_SHF     0x5c   /* pitch shift (9)                  */
#define VA2000_REG_HTOTAL        0x70   /* H sync start                     */
#define VA2000_REG_HSYNC_START   0x72   /* H sync end                       */
#define VA2000_REG_HSYNC_END     0x74   /* H max                            */
#define VA2000_REG_VTOTAL        0x76   /* V sync start                     */
#define VA2000_REG_VSYNC_START   0x78   /* V sync end                       */
#define VA2000_REG_VSYNC_END     0x7a   /* V max                            */
#define VA2000_REG_PIX_CLK       0x7c   /* pixel clock                      */

/*
** The blitter (offsets verified by the R5 driver against the card's FPGA
** source).  Coordinates are inclusive.  0x40/0x42 hold the SDRAM word
** address of the destination's starting row, 0x44/0x46 the source's; SDRAM
** word 0 is framebuffer pixel (0,0).  The order of the coordinates sets the
** direction: X1 <= X2 and Y1 <= Y2 run top to bottom, left to right from
** the starting rows; X1 > X2 and Y1 > Y2 run bottom to top, right to left.
** ENABLE reads non-zero while the blitter is busy.
*/
#define VA2000_BLT_ROWPITCH  0x1c   /* blitter row pitch in pixels          */
#define VA2000_BLT_COLORMODE 0x1e   /* blitter color mode: 1=16bit          */
#define VA2000_BLT_X1        0x20   /* destination x, start                 */
#define VA2000_BLT_Y1        0x22   /* destination y, start                 */
#define VA2000_BLT_X2        0x24   /* destination x, end (inclusive)       */
#define VA2000_BLT_Y2        0x26   /* destination y, end (inclusive)       */
#define VA2000_BLT_RGB16     0x28   /* fill colour (RGB565)                 */
#define VA2000_BLT_ENABLE    0x2a   /* write to start; non-zero = busy      */
#define   VA2000_BLT_FILL    0x0001
#define   VA2000_BLT_COPY    0x0002
#define VA2000_BLT_X3        0x2c   /* source x, start (copy)               */
#define VA2000_BLT_Y3        0x2e   /* source y, start (copy)               */
#define VA2000_BLT_X4        0x30   /* source x, end (copy, inclusive)      */
#define VA2000_BLT_Y4        0x32   /* source y, end (copy, inclusive)      */
#define VA2000_BLT_BASE_HI   0x40   /* destination starting row [23:16]     */
#define VA2000_BLT_BASE_LO   0x42   /* destination starting row [15:0]      */
#define VA2000_BLT_BASE2_HI  0x44   /* source starting row [23:16]          */
#define VA2000_BLT_BASE2_LO  0x46   /* source starting row [15:0]           */

/*
** The bounded blitter wait (the R5 driver's numbers).  A poll is a Zorro
** register read, on the order of 200 ns; the longest real blit, a full
** 1920x1080 fill at about 44 Mpixel/s, is under 50 ms; 500000 polls is
** about 100 ms.  A wait that reaches the bound means the blitter is lost:
** the card draws what it held on the CPU and runs without the blitter.
*/
#define VA2000_BLIT_TIMEOUT      500000L
#define VA2000_BLIT_SLOW         50000L     /* VA2000_STATS: a slow wait     */
#define VA2000_BLIT_LOG          8          /* VA2000_STATS: log this many   */

#define VA2000_TEST_PIXEL        0xf81f     /* the self-test's colour        */

/*
** Register access.  The register page is mapped noncacheable and serialised
** by the kernel, so a volatile 16-bit access is one bus cycle, in order, and
** it waits for the CPU's earlier framebuffer writes.  The host test harness
** (tests/va2000) builds these files with VA2000_HOSTSIM and a software model
** of the card behind the two calls.
*/
#ifdef VA2000_HOSTSIM
extern void           va2000SimWriteReg(volatile unsigned char *base, int reg,
                                        unsigned int val);
extern unsigned short va2000SimReadReg(volatile unsigned char *base, int reg);
#define VA2000_WRITEREG(base, reg, val) \
    va2000SimWriteReg((base), (reg), (unsigned int)(val))
#define VA2000_READREG(base, reg) \
    va2000SimReadReg((base), (reg))
#else
#define VA2000_WRITEREG(base, reg, val) \
    (*((volatile unsigned short *)((base) + (reg))) = (unsigned short)(val))
#define VA2000_READREG(base, reg) \
    (*((volatile unsigned short *)((base) + (reg))))
#endif

/* ======================================================================= */
/* = The card's private record                                           = */
/* ======================================================================= */

#define VA2000_HELD_NONE    0
#define VA2000_HELD_FILL    1
#define VA2000_HELD_COPY    2

typedef struct _va2000ScreenRec {
    int                     fd;         /* VA2000_DEV                        */
    caddr_t                 regMap;     /* the register mapping, as mapped   */
    long                    regLen;
    volatile unsigned char *regBase;    /* regMap, for register access       */
    unsigned char          *fbMap;      /* the framebuffer mapping: (0,0)    */
    long                    fbLen;
    int                     width;      /* the mode's size, the blitter's    */
    int                     height;     /* row pitch in pixels is width      */
    int                     blitOn;     /* the blitter takes work            */
    int                     held;       /* VA2000_HELD_*: started, not seen  */
                                        /* finished                          */
    int                     hx, hy, hw, hh;     /* the held operation's      */
    int                     hdx, hdy;           /* box (fill: hx,hy,hw,hh)   */
    unsigned long           hpixel;
} va2000ScreenRec, *va2000ScreenPtr;

#define GetVA2000Screen(pRTG) ((va2000ScreenPtr)(pRTG)->cardPrivate)

/* ======================================================================= */
/* = Functions                                                           = */
/* ======================================================================= */

/* va2000hw.c: the record, the modes, the device and the mappings */
extern rtgCardOps     va2000CardOps;
extern VA2000ModePtr  va2000ModeByName(char *name);
extern int            va2000PlanMaps(VA2000ModePtr mode, long fbSize,
                                     VA2000MapPlan *plan);

/* va2000blit.c: the blitter, its self-test, and the card's fillBox,
 * copyBox and flush */
extern void va2000BlitStart(rtgScreenPtr pRTG);
extern void va2000BlitStop(rtgScreenPtr pRTG);
extern Bool va2000FillBox(rtgScreenPtr pRTG, int x1, int y1, int x2, int y2,
                          unsigned long pixel);
extern Bool va2000CopyBox(rtgScreenPtr pRTG, int sx, int sy, int w, int h,
                          int dx, int dy);
extern void va2000Flush(rtgScreenPtr pRTG);

#endif /* VA2000_H */
