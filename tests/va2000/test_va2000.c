/*
 * test_va2000.c -- host test harness for the X11R6.3 VA2000 card driver
 * (x11r6.3/hw/amix/rtg/va2000).  Builds and runs on Linux; the card cannot
 * be emulated, so this checks the driver against a software model of the
 * card's registers, blitter and kernel driver, not against the card.
 *
 * The model: /dev/va2000's open, close, ioctl (GETFW, GetFBufSize,
 * SetMonitorSwitch) and mmap over a Zorro II (4 MB), Zorro III (32 MB) or
 * unknown aperture; the register page; a blitter that takes inclusive
 * coordinates and starting-row base addresses, runs forward or reverse by
 * the order of its coordinates, moves one pixel at a time in that order (so
 * a wrong direction on an overlapping copy shows), stays busy for a set
 * number of status reads before its result lands, and can be made to stick,
 * to do nothing, or to read back the firmware byte for every register.
 * The reference: a plain CPU fill and copy (as if the source were read
 * whole first) on a shadow of the screen.
 *
 * Copyright (c) 2026 Jussi Alanärä
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "misc.h"
#include "os.h"
#include "rtg.h"
#include "va2000.h"
#include "sim.h"

/* ===================================================================== */
/* Checks                                                                 */
/* ===================================================================== */

static long nChecks, nFails;
static int  verbose;

#define CHECK(cond, ...) \
    do { nChecks++; if (!(cond)) { nFails++; \
        fprintf(stderr, "  FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* ===================================================================== */
/* The model                                                              */
/* ===================================================================== */

#define AP_Z2       0x400000L
#define AP_Z3       0x2000000L
#define SIM_DEVFD0  100

enum { BLIT_OK, BLIT_STUCK, BLIT_NOWRITE, BLIT_READSFW, BLIT_STUCKAFTER };

static struct {
    int  present;       /* the device opens                              */
    long aperture;      /* bytes the board decodes                       */
    int  fbsizeIoctl;   /* the driver answers SVGAIOCGetFBufSize         */
    int  fw;            /* VA2IOC_GETFW's answer                         */
    int  blit;          /* BLIT_*                                        */
    int  latency;       /* status reads a blit stays busy                */
    long stuckAfter;    /* BLIT_STUCKAFTER: operations that still work   */
    int  mmapFailAt;    /* fail the n-th mmap (1-based), 0 = never       */
} cfg;

static unsigned char *simMem;           /* the aperture                  */
static unsigned short simReg[0x800];    /* the register page, by word    */
static unsigned short simSnap[0x800];   /* the registers at the start    */
static int  simBusy, simBusyLeft, simOp;
static long simOps, simEnableReads, simRegWrites, simViol, simOpenFds;
static long simOpens, simCloses, simMmaps;
static int  simSwitchVal, simSwitchSize, simSwitchFd, simSwitchCalls;
static struct { void *p; size_t len; long off; int live; } simMaps[8];
static int  simNMaps;
static char errLog[8192];
static int  errCount;

#define FBW ((unsigned short *) (simMem + VA2000_FB_OFFSET))
#define REG(r) simReg[(r) >> 1]
#define SNAP(r) simSnap[(r) >> 1]

static void
viol(const char *what)
{
    simViol++;
    if (simViol <= 5)
        fprintf(stderr, "  model: %s\n", what);
}

static void
simReset(long aperture)
{
    if (!simMem) {
        simMem = malloc(AP_Z3);
        if (!simMem) { perror("malloc"); exit(2); }
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.present = 1;
    cfg.aperture = aperture;
    cfg.fbsizeIoctl = 1;
    cfg.fw = 98;
    cfg.blit = BLIT_OK;
    cfg.latency = 3;
    memset(simMem, 0xa5, AP_Z3);        /* junk: the clear must happen */
    memset(simReg, 0, sizeof(simReg));
    simBusy = simBusyLeft = simOp = 0;
    simOps = simEnableReads = simRegWrites = simViol = simOpenFds = 0;
    simOpens = simCloses = simMmaps = 0;
    simSwitchVal = simSwitchSize = simSwitchCalls = 0;
    simSwitchFd = -1;
    simNMaps = 0;
    errLog[0] = 0;
    errCount = 0;
}

static int
simLiveMaps(void)
{
    int i, n = 0;

    for (i = 0; i < simNMaps; i++)
        n += simMaps[i].live;
    return n;
}

static void
simApply(void)
{
    long pitch = SNAP(VA2000_BLT_ROWPITCH);
    long nwords = (cfg.aperture - VA2000_FB_OFFSET) / 2;
    long base = ((long) SNAP(VA2000_BLT_BASE_HI) << 16) | SNAP(VA2000_BLT_BASE_LO);
    long base2 = ((long) SNAP(VA2000_BLT_BASE2_HI) << 16) | SNAP(VA2000_BLT_BASE2_LO);
    int  x1 = SNAP(VA2000_BLT_X1), y1 = SNAP(VA2000_BLT_Y1);
    int  x2 = SNAP(VA2000_BLT_X2), y2 = SNAP(VA2000_BLT_Y2);
    int  x3 = SNAP(VA2000_BLT_X3), y3 = SNAP(VA2000_BLT_Y3);
    int  x4 = SNAP(VA2000_BLT_X4), y4 = SNAP(VA2000_BLT_Y4);
    long k, j, rows, cols, d, s;
    int  dir;

    if (pitch != REG(VA2000_REG_PITCH))
        viol("blitter row pitch is not the display pitch");
    if (SNAP(VA2000_BLT_COLORMODE) != 1)
        viol("blitter colour mode is not 16-bit");
    if (base != (long) y1 * pitch)
        viol("destination base is not the starting row's address");
    if (simOp == VA2000_BLT_FILL) {
        if (x1 > x2 || y1 > y2) { viol("reverse fill"); return; }
        for (k = 0; k <= y2 - y1; k++)
            for (j = x1; j <= x2; j++) {
                d = base + k * pitch + j;
                if (d < 0 || d >= nwords) { viol("fill outside the framebuffer"); return; }
                FBW[d] = SNAP(VA2000_BLT_RGB16);
            }
        return;
    }
    if (simOp != VA2000_BLT_COPY) { viol("unknown blitter operation"); return; }
    if (base2 != (long) y3 * pitch)
        viol("source base is not the starting row's address");
    dir = (y1 > y2 || (y1 == y2 && x1 > x2)) ? -1 : 1;
    if ((x2 - x1) * dir < 0 || (y2 - y1) * dir < 0 ||
        (x4 - x3) * dir < 0 || (y4 - y3) * dir < 0) {
        viol("copy coordinates in mixed directions");
        return;
    }
    cols = (x2 - x1) * dir + 1;
    rows = (y2 - y1) * dir + 1;
    if ((x4 - x3) * dir + 1 != cols || (y4 - y3) * dir + 1 != rows) {
        viol("copy source and destination differ in size");
        return;
    }
    for (k = 0; k < rows; k++)
        for (j = 0; j < cols; j++) {
            d = base + dir * k * pitch + x1 + dir * j;
            s = base2 + dir * k * pitch + x3 + dir * j;
            if (d < 0 || d >= nwords || s < 0 || s >= nwords) {
                viol("copy outside the framebuffer");
                return;
            }
            FBW[d] = FBW[s];
        }
}

void
va2000SimWriteReg(volatile unsigned char *base, int reg, unsigned int val)
{
    simRegWrites++;
    if (base != simMem) { viol("register write outside the register mapping"); return; }
    if (reg < 0 || reg >= (int) VA2000_REG_MAPLEN || (reg & 1)) { viol("bad register offset"); return; }
    if (val > 0xffff)
        viol("register value wider than 16 bits");
    if (simBusy && ((reg >= VA2000_BLT_ROWPITCH && reg <= VA2000_BLT_Y4) ||
                    (reg >= VA2000_BLT_BASE_HI && reg <= VA2000_BLT_BASE2_LO)))
        viol("blitter register written while the blitter is busy");
    REG(reg) = (unsigned short) val;
    if (reg != VA2000_BLT_ENABLE || !val)
        return;
    simOps++;
    if (cfg.blit == BLIT_NOWRITE)
        return;
    memcpy(simSnap, simReg, sizeof(simReg));
    simOp = (int) val;
    simBusy = 1;
    if (cfg.blit == BLIT_STUCK ||
        (cfg.blit == BLIT_STUCKAFTER && simOps > cfg.stuckAfter)) {
        simBusyLeft = -1;
        return;
    }
    simBusyLeft = cfg.latency;
    if (simBusyLeft <= 0) {
        simApply();
        simBusy = 0;
    }
}

unsigned short
va2000SimReadReg(volatile unsigned char *base, int reg)
{
    if (base != simMem) { viol("register read outside the register mapping"); return 0; }
    if (cfg.blit == BLIT_READSFW)
        return 0x5a;
    if (reg != VA2000_BLT_ENABLE)
        return REG(reg);
    simEnableReads++;
    if (!simBusy)
        return 0;
    if (simBusyLeft < 0)
        return (unsigned short) simOp;
    if (--simBusyLeft <= 0) {
        simApply();
        simBusy = 0;
        return 0;
    }
    return (unsigned short) simOp;
}

static int
simFdOk(int fd)
{
    return fd >= SIM_DEVFD0 && fd < SIM_DEVFD0 + 64;
}

int
simOpen(const char *path, int flags, ...)
{
    (void) flags;
    if (strcmp(path, VA2000_DEV) != 0 || !cfg.present) {
        errno = ENXIO;
        return -1;
    }
    simOpens++;
    simOpenFds++;
    return SIM_DEVFD0 + (int) simOpens;
}

int
simClose(int fd)
{
    if (!simFdOk(fd)) { viol("close of a descriptor the model did not hand out"); return -1; }
    simCloses++;
    simOpenFds--;
    return 0;
}

int
simIoctl(int fd, int req, long arg, int argsize)
{
    if (!simFdOk(fd)) { errno = EBADF; return -1; }
    if (req == VA2IOC_GETFW)
        return cfg.fw;
    if (req == SVGAIOCGetFBufSize) {
        if (!cfg.fbsizeIoctl) { errno = EINVAL; return -1; }
        return (int) (cfg.aperture - VA2000_FB_OFFSET);
    }
    if (req == SVGAIOCSetMonitorSwitch) {
        simSwitchCalls++;
        simSwitchFd = fd;
        simSwitchSize = argsize;
        simSwitchVal = *(unsigned short *) arg;     /* what the driver reads */
        return 0;
    }
    errno = EINVAL;
    return -1;
}

void *
simMmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    (void) addr; (void) prot; (void) flags;
    simMmaps++;
    if (!simFdOk(fd)) { errno = EBADF; return MAP_FAILED; }
    if (cfg.mmapFailAt && simMmaps == cfg.mmapFailAt) { errno = ENOMEM; return MAP_FAILED; }
    if (off < 0 || (off & 0x7ff) || len == 0 || off + (long) len > cfg.aperture) {
        errno = ENXIO;
        return MAP_FAILED;
    }
    if (simNMaps >= 8) { errno = ENOMEM; return MAP_FAILED; }
    simMaps[simNMaps].p = simMem + off;
    simMaps[simNMaps].len = len;
    simMaps[simNMaps].off = off;
    simMaps[simNMaps].live = 1;
    simNMaps++;
    return simMem + off;
}

int
simMunmap(void *addr, size_t len)
{
    int i;

    for (i = 0; i < simNMaps; i++)
        if (simMaps[i].live && simMaps[i].p == addr && simMaps[i].len == len) {
            simMaps[i].live = 0;
            return 0;
        }
    viol("munmap of a mapping the model did not make (or another length)");
    return -1;
}

/* ===================================================================== */
/* The layer's side (rtgdraw.c, rtgInit.c), as far as the card meets it   */
/* ===================================================================== */

int         rtgBlitMin = 1024;
int         rtgMove16 = 0;
rtgCardOps *rtgCard = &va2000CardOps;
static long cpuFills, cpuCopies;

void
ErrorF(const char *fmt, ...)
{
    va_list ap;
    size_t  n = strlen(errLog);

    va_start(ap, fmt);
    if (n < sizeof(errLog) - 1)
        vsnprintf(errLog + n, sizeof(errLog) - n, fmt, ap);
    va_end(ap);
    errCount++;
    if (verbose) {
        va_start(ap, fmt);
        fputs("  ErrorF: ", stderr);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
    }
}

static int
countIn(const char *hay, const char *needle)
{
    int n = 0;
    const char *p = hay;

    while ((p = strstr(p, needle)) != NULL) { n++; p++; }
    return n;
}

void
rtgCpuFill(rtgScreenPtr pRTG, int x1, int y1, int x2, int y2,
           unsigned long pixel)
{
    int x, y, stride = pRTG->pitch / 2;

    cpuFills++;
    for (y = y1; y < y2; y++)
        for (x = x1; x < x2; x++)
            pRTG->fbBase[(long) y * stride + x] = (unsigned short) pixel;
}

void
rtgCpuCopy(rtgScreenPtr pRTG, int sx, int sy, int w, int h, int dx, int dy)
{
    int stride = pRTG->pitch / 2, y;
    unsigned short *tmp = malloc((size_t) w * h * 2);

    cpuCopies++;
    for (y = 0; y < h; y++)
        memcpy(tmp + (long) y * w, pRTG->fbBase + (long) (sy + y) * stride + sx,
               (size_t) w * 2);
    for (y = 0; y < h; y++)
        memcpy(pRTG->fbBase + (long) (dy + y) * stride + dx, tmp + (long) y * w,
               (size_t) w * 2);
    free(tmp);
}

void
rtgMonitorSwitchInt(int fd, int which)
{
    (void) fd; (void) which;
}

/* ===================================================================== */
/* The reference and the layer's calling pattern                          */
/* ===================================================================== */

static unsigned short *ref;
static int refW, refH;

static void
refInit(rtgScreenPtr pRTG, unsigned seed)
{
    long i, n = (long) pRTG->width * pRTG->height;

    free(ref);
    refW = pRTG->width;
    refH = pRTG->height;
    ref = malloc((size_t) n * 2);
    srand(seed);
    for (i = 0; i < n; i++) {
        ref[i] = (unsigned short) (rand() & 0xffff);
        pRTG->fbBase[i] = ref[i];
    }
}

static void
refFill(int x1, int y1, int x2, int y2, unsigned long p)
{
    int x, y;

    for (y = y1; y < y2; y++)
        for (x = x1; x < x2; x++)
            ref[(long) y * refW + x] = (unsigned short) p;
}

static void
refCopy(int sx, int sy, int w, int h, int dx, int dy)
{
    unsigned short *tmp = malloc((size_t) w * h * 2);
    int y;

    for (y = 0; y < h; y++)
        memcpy(tmp + (long) y * w, ref + (long) (sy + y) * refW + sx, (size_t) w * 2);
    for (y = 0; y < h; y++)
        memcpy(ref + (long) (dy + y) * refW + dx, tmp + (long) y * w, (size_t) w * 2);
    free(tmp);
}

/* rtgFillBox's pattern: the blitter if it qualifies and takes it, else
 * flush and the CPU */
static void
layerFill(rtgScreenPtr pRTG, int x1, int y1, int x2, int y2, unsigned long p)
{
    refFill(x1, y1, x2, y2, p);
    if (RTG_BLIT_WANT(x2 - x1, y2 - y1) &&
        (*rtgCard->fillBox)(pRTG, x1, y1, x2, y2, p)) {
        pRTG->queued = 1;
        return;
    }
    RTG_FLUSH(pRTG);
    rtgCpuFill(pRTG, x1, y1, x2, y2, p);
}

static void
layerCopy(rtgScreenPtr pRTG, int sx, int sy, int w, int h, int dx, int dy)
{
    refCopy(sx, sy, w, h, dx, dy);
    if (RTG_BLIT_WANT(w, h) &&
        (*rtgCard->copyBox)(pRTG, sx, sy, w, h, dx, dy)) {
        pRTG->queued = 1;
        return;
    }
    RTG_FLUSH(pRTG);
    rtgCpuCopy(pRTG, sx, sy, w, h, dx, dy);
}

/* After a flush the framebuffer is the reference; returns mismatches. */
static long
refDiff(rtgScreenPtr pRTG)
{
    long i, n = (long) refW * refH, bad = 0;

    RTG_FLUSH(pRTG);
    for (i = 0; i < n; i++)
        if (pRTG->fbBase[i] != ref[i])
            bad++;
    return bad;
}

/* ===================================================================== */
/* Set-up                                                                 */
/* ===================================================================== */

static rtgScreenRec scr;

static void
cardUp(long aperture, char *mode)
{
    (void) mode;
    simReset(aperture);
}

static Bool
initScreen(char *mode)
{
    memset(&scr, 0, sizeof(scr));
    scr.devFd = -1;
    if (mode && !(*va2000CardOps.setMode)(mode))
        return FALSE;
    return (*va2000CardOps.initHW)(&scr);
}

static long
apertureOf(int bus)
{
    return bus == VA2000_BUS_Z3 ? AP_Z3 : AP_Z2;
}

static const struct { char *name; int w, h; } modes[] = {
    { "640x480", 640, 480 }, { "800x600", 800, 600 },
    { "1024x768", 1024, 768 }, { "1280x720", 1280, 720 },
    { "1280x1024", 1280, 1024 }, { "1920x1080", 1920, 1080 },
};
#define NMODES ((int) (sizeof(modes) / sizeof(modes[0])))

/* ===================================================================== */
/* The tests, one group per function, each in its own process            */
/* ===================================================================== */

static void
testRecord(void)
{
    rtgCardOps *c = &va2000CardOps;
    char buf[256], *tok;
    int  i, w, h, n = 0;

    CHECK(c->version == RTG_CARD_VERSION && RTG_CARD_VERSION == 1, "version %d", c->version);
    CHECK(strcmp(c->name, "va2000") == 0, "name %s", c->name);
    CHECK(strcmp(c->device, "/dev/va2000") == 0, "device %s", c->device);
    CHECK(c->caps.depth == 16 && c->caps.bitsPerPixel == 16, "depth/bpp");
    CHECK(c->caps.redMask == 0xf800 && c->caps.greenMask == 0x07e0 &&
          c->caps.blueMask == 0x001f, "RGB565 masks");
    CHECK(c->caps.bitsPerRGB == 6, "bitsPerRGB");
    CHECK(c->caps.blitMin > 0, "blitMin default");
    CHECK((c->caps.flags & (RTG_CAP_BLITTER | RTG_CAP_USERREGS)) ==
          (RTG_CAP_BLITTER | RTG_CAP_USERREGS), "flags 0x%x", c->caps.flags);
    CHECK(c->probe && c->setMode && c->modeSize && c->initHW && c->closeHW &&
          c->fillBox && c->copyBox && c->flush && c->monitorSwitch, "every hook set");
    (*c->modeSize)(&w, &h);
    CHECK(w == 800 && h == 600, "default mode %dx%d", w, h);
    snprintf(buf, sizeof(buf), "%s", c->modes);
    for (tok = strtok(buf, " "); tok; tok = strtok(NULL, " "), n++) {
        CHECK(n < NMODES && strcmp(tok, modes[n].name) == 0, "modes list entry %s", tok);
        CHECK((*c->setMode)(tok), "setMode %s", tok);
        (*c->modeSize)(&w, &h);
        CHECK(n < NMODES && w == modes[n].w && h == modes[n].h, "modeSize %s", tok);
    }
    CHECK(n == NMODES, "modes list has %d modes", n);
    for (i = 0; i < 4; i++) {
        static char *bad[] = { "800x601", "foo", "", "1920x1200" };
        CHECK(!(*c->setMode)(bad[i]), "setMode refuses '%s'", bad[i]);
    }
    (*c->modeSize)(&w, &h);
    CHECK(w == 1920 && h == 1080, "a refused setMode keeps the mode");
}

static void
testProbe(void)
{
    cardUp(AP_Z2, NULL);
    CHECK((*va2000CardOps.probe)(), "probe finds the card");
    CHECK(simOpens == 1 && simCloses == 1, "probe opens and closes once");
    CHECK(simNMaps == 0 && simRegWrites == 0, "probe maps and writes nothing");
    cardUp(AP_Z2, NULL);
    cfg.present = 0;
    CHECK(!(*va2000CardOps.probe)(), "probe without the card");
    CHECK(simOpenFds == 0, "no descriptor left");
}

static void
testPlan(void)
{
    static const long fbSizes[3] = { AP_Z2 - VA2000_FB_OFFSET, AP_Z3 - VA2000_FB_OFFSET, 0 };
    static const int buses[3] = { VA2000_BUS_Z2, VA2000_BUS_Z3, VA2000_BUS_UNKNOWN };
    static const long aps[3] = { AP_Z2, AP_Z3, 0x200000L };
    VA2000MapPlan p;
    int b, m, r;

    for (b = 0; b < 3; b++)
        for (m = 0; m < NMODES; m++) {
            long bytes = (long) modes[m].w * modes[m].h * 2;
            long avail = aps[b] - 0x10000L;
            long len = (bytes + 0xfff) & ~0xfffL;

            r = va2000PlanMaps(va2000ModeByName(modes[m].name), fbSizes[b], &p);
            CHECK(p.bus == buses[b] && p.aperture == aps[b] && p.fbAvail == avail,
                  "%s bus %d: bus/aperture", modes[m].name, b);
            CHECK(p.modeBytes == bytes && p.pitch == modes[m].w * 2,
                  "%s bus %d: %ld bytes", modes[m].name, b, p.modeBytes);
            CHECK(p.regMapLen == 0x1000, "register mapping one page");
            CHECK((r == VA2000_PLAN_OK) == (bytes <= avail),
                  "%s bus %d: verdict %d", modes[m].name, b, r);
            if (r == VA2000_PLAN_OK) {
                CHECK(p.fbMapLen == len && p.fbMapLen >= bytes &&
                      p.fbMapLen % 0x800 == 0 &&
                      VA2000_FB_OFFSET + p.fbMapLen <= aps[b],
                      "%s bus %d: fb mapping %ld", modes[m].name, b, p.fbMapLen);
                CHECK(p.fbMapLen - bytes < 0x1000, "no slack beyond the page");
            }
            if (verbose)
                fprintf(stderr, "  plan %-9s %-9s %s fb %5ld KB of %5ld KB, map %5ld KB\n",
                        modes[m].name, b == 0 ? "Zorro II" : b == 1 ? "Zorro III" : "unknown",
                        r == VA2000_PLAN_OK ? "ok     " : "REFUSED",
                        bytes >> 10, avail >> 10, p.fbMapLen >> 10);
        }
    r = va2000PlanMaps(va2000ModeByName("1920x1080"), fbSizes[0], &p);
    CHECK(r == VA2000_PLAN_TOOBIG, "1920x1080 refused on Zorro II");
    r = va2000PlanMaps(va2000ModeByName("1920x1080"), fbSizes[1], &p);
    CHECK(r == VA2000_PLAN_OK, "1920x1080 taken on Zorro III");
    r = va2000PlanMaps(va2000ModeByName("1280x1024"), fbSizes[0], &p);
    CHECK(r == VA2000_PLAN_OK, "1280x1024 taken on Zorro II");
    r = va2000PlanMaps(va2000ModeByName("1280x1024"), 0, &p);
    CHECK(r == VA2000_PLAN_TOOBIG, "1280x1024 refused with no GetFBufSize");
}

static void
testInit(void)
{
    int b, m, i;

    for (b = VA2000_BUS_Z2; b <= VA2000_BUS_Z3; b++)
        for (m = 0; m < NMODES; m++) {
            long ap = apertureOf(b);
            long bytes = (long) modes[m].w * modes[m].h * 2;
            Bool fits = bytes <= ap - VA2000_FB_OFFSET;
            long w, closeWrites, zero = 1, n;
            Bool ok;

            cardUp(ap, NULL);
            ok = initScreen(modes[m].name);
            CHECK(ok == fits, "%s on bus %d: initHW %d", modes[m].name, b, ok);
            if (!ok) {
                CHECK(simLiveMaps() == 0 && simOpenFds == 0 && simRegWrites == 0,
                      "%s refused: nothing mapped, open or written", modes[m].name);
                CHECK(strstr(errLog, "needs") != NULL, "refusal says why");
                continue;
            }
            CHECK(simLiveMaps() == 2 && simOpenFds == 1, "two mappings, one fd");
            CHECK(simMaps[0].off == 0 && simMaps[0].len == 0x1000, "register page mapping");
            CHECK(simMaps[1].off == VA2000_FB_OFFSET &&
                  (long) simMaps[1].len == ((bytes + 0xfff) & ~0xfffL),
                  "framebuffer mapping %zu", simMaps[1].len);
            CHECK(scr.width == modes[m].w && scr.height == modes[m].h &&
                  scr.pitch == modes[m].w * 2 && scr.bitsPerPixel == 16 &&
                  scr.depth == 16, "screen geometry");
            CHECK(scr.fbBase == (unsigned short *) (simMem + VA2000_FB_OFFSET) &&
                  scr.frameBase == (unsigned char *) scr.fbBase && scr.fbOffset == 0,
                  "fbBase is the framebuffer mapping");
            CHECK(simFdOk(scr.devFd), "devFd is the device");
            CHECK(REG(VA2000_REG_PITCH) == modes[m].w && REG(VA2000_REG_WIDTH) == modes[m].w &&
                  REG(VA2000_REG_HEIGHT) == modes[m].h && REG(VA2000_REG_CAPTURE) == 0 &&
                  REG(VA2000_REG_PAN_HI) == 0 && REG(VA2000_REG_PAN_LO) == 0 &&
                  REG(VA2000_REG_COLORMODE) == 1 && REG(VA2000_REG_PITCH_SHF) == 9,
                  "mode registers");
            n = (long) modes[m].w * modes[m].h;
            for (i = 0; i < n; i++)
                if (FBW[i]) { zero = 0; break; }
            CHECK(zero, "%s: screen cleared, self-test pixel put back", modes[m].name);
            CHECK(simViol == 0, "no model violations");
            w = simRegWrites;
            (*va2000CardOps.closeHW)(&scr);
            closeWrites = simRegWrites - w;
            CHECK(closeWrites == 0, "closeHW writes no register (%ld)", closeWrites);
            CHECK(simLiveMaps() == 0 && simOpenFds == 0 && scr.cardPrivate == NULL &&
                  scr.devFd == -1, "closeHW unmaps both and closes");
            CHECK(simViol == 0, "no model violations at close");
        }

    /* no GetFBufSize: the R5 driver's 2 MB */
    cardUp(AP_Z2, NULL);
    cfg.fbsizeIoctl = 0;
    CHECK(initScreen("1280x720"), "1280x720 with an old driver");
    (*va2000CardOps.closeHW)(&scr);
    cardUp(AP_Z2, NULL);
    cfg.fbsizeIoctl = 0;
    CHECK(!initScreen("1280x1024") && simOpenFds == 0, "1280x1024 refused with an old driver");

    /* failures undo everything */
    cardUp(AP_Z3, NULL);
    cfg.present = 0;
    CHECK(!initScreen("800x600") && simOpenFds == 0, "no device: no screen");
    cardUp(AP_Z3, NULL);
    cfg.mmapFailAt = 1;
    CHECK(!initScreen("800x600") && simOpenFds == 0 && simLiveMaps() == 0 &&
          simRegWrites == 0, "register mapping refused: undone");
    cardUp(AP_Z3, NULL);
    cfg.mmapFailAt = 2;
    CHECK(!initScreen("800x600") && simOpenFds == 0 && simLiveMaps() == 0 &&
          simRegWrites == 0 && simViol == 0, "framebuffer mapping refused: undone");

    /* a server reset: close and init again, the picture is not touched in between */
    cardUp(AP_Z3, NULL);
    CHECK(initScreen("1024x768"), "generation 1");
    (*va2000CardOps.closeHW)(&scr);
    CHECK(REG(VA2000_REG_CAPTURE) == 0, "reset: still RTG after closeHW");
    CHECK(initScreen(NULL), "generation 2");
    CHECK(simLiveMaps() == 2 && simOpenFds == 1 && simViol == 0, "generation 2 mapped");
    (*va2000CardOps.closeHW)(&scr);
}

/* fills: edges, corners, strips, random; every one through the blitter */
static void
fillRun(long aperture, char *mode, int nrandom)
{
    int W, H, i, k;
    long bad;

    cardUp(aperture, mode);
    CHECK(initScreen(mode), "%s init", mode);
    W = scr.width; H = scr.height;
    refInit(&scr, 1);
    rtgBlitMin = 1;
    cpuFills = 0;
    {
        int e[][4] = {
            { 0, 0, W, H }, { 0, 0, 1, 1 }, { W - 1, H - 1, W, H }, { W - 1, 0, W, 1 },
            { 0, H - 1, 1, H }, { 0, 0, W, 1 }, { 0, H - 1, W, H }, { 0, 0, 1, H },
            { W - 1, 0, W, H }, { 1, 1, W - 1, H - 1 }, { W / 2, H / 2, W / 2 + 1, H / 2 + 1 },
            { 0, 0, 2, 2 }, { W - 2, H - 2, W, H }, { 3, 5, 4, H - 5 }, { 3, 5, W - 3, 6 },
        };
        for (i = 0; i < (int) (sizeof(e) / sizeof(e[0])); i++) {
            layerFill(&scr, e[i][0], e[i][1], e[i][2], e[i][3], 0x1000 + i * 77);
            bad = refDiff(&scr);
            CHECK(bad == 0, "%s edge fill %d: %ld pixels differ", mode, i, bad);
        }
    }
    srand(7);
    for (k = 0; k < nrandom; k++) {
        int x1 = rand() % W, y1 = rand() % H;
        int x2 = x1 + 1 + rand() % (W - x1), y2 = y1 + 1 + rand() % (H - y1);

        layerFill(&scr, x1, y1, x2, y2, (unsigned long) (rand() & 0xffff));
        if (k % 10 == 9) {
            bad = refDiff(&scr);
            CHECK(bad == 0, "%s random fills to %d: %ld pixels differ", mode, k, bad);
        }
    }
    bad = refDiff(&scr);
    CHECK(bad == 0, "%s fills: %ld pixels differ", mode, bad);
    CHECK(cpuFills == 0, "%s: every fill taken by the blitter (%ld on the CPU)", mode, cpuFills);
    CHECK(simViol == 0, "%s fills: no model violations", mode);
    (*va2000CardOps.closeHW)(&scr);
}

static void
testFill(void)
{
    fillRun(AP_Z2, "640x480", 200);
    fillRun(AP_Z2, "1280x1024", 60);
    fillRun(AP_Z3, "1920x1080", 60);
}

/* copies: every direction, overlapping and not, at the edges */
static void
copyRun(long aperture, char *mode, int flushEach)
{
    int W, H, ddx, ddy, i, n = 0;
    long bad, total = 0;

    cardUp(aperture, mode);
    CHECK(initScreen(mode), "%s init", mode);
    W = scr.width; H = scr.height;
    refInit(&scr, 2);
    rtgBlitMin = 1;
    cpuCopies = 0;

    /* overlapping, all eight directions and the same rows/columns */
    for (ddy = -7; ddy <= 7; ddy++)
        for (ddx = -7; ddx <= 7; ddx++) {
            int bw = 40, bh = 30, sx = 100, sy = 100;

            if (!ddx && !ddy)
                continue;
            layerCopy(&scr, sx, sy, bw, bh, sx + ddx, sy + ddy);
            n++;
            if (flushEach) {
                bad = refDiff(&scr);
                CHECK(bad == 0, "%s overlap (%d,%d): %ld differ", mode, ddx, ddy, bad);
            }
        }
    /* the four axis moves by one, wide and narrow */
    {
        int c[][6] = {
            { 0, 1, W, H - 1, 0, 0 },           /* scroll up a row, whole screen    */
            { 0, 0, W, H - 1, 0, 1 },           /* scroll down a row                */
            { 1, 0, W - 1, H, 0, 0 },           /* left a column                    */
            { 0, 0, W - 1, H, 1, 0 },           /* right a column                   */
            { 0, 16, W, H - 16, 0, 0 },         /* a text line up                   */
            { 0, 0, W, H - 16, 0, 16 },         /* a text line down                 */
            { 0, 0, 1, 1, W - 1, H - 1 },       /* corner to corner                 */
            { W - 1, H - 1, 1, 1, 0, 0 },
            { 0, 0, 64, 48, W - 64, H - 48 },   /* box to the far edges             */
            { W - 64, H - 48, 64, 48, 0, 0 },
            { 0, 0, W / 2, H, W / 2, 0 },       /* halves, touching                 */
            { W / 2, 0, W / 2, H, 0, 0 },
            { 0, 0, W, H / 2, 0, H / 2 },
            { 0, H / 2, W, H / 2, 0, 0 },
            { 5, 5, 1, H - 10, 6, 5 },          /* one column right, overlap         */
            { 6, 5, 1, H - 10, 5, 5 },
            { 5, 5, W - 10, 1, 5, 6 },          /* one row down                     */
            { 5, 6, W - 10, 1, 5, 5 },
            { 10, 10, 100, 100, 10, 10 },       /* onto itself                      */
            { 0, 0, W - 1, H - 1, 1, 1 },       /* diagonal, whole screen            */
            { 1, 1, W - 1, H - 1, 0, 0 },
            { 1, 0, W - 1, H - 1, 0, 1 },
            { 0, 1, W - 1, H - 1, 1, 0 },
        };
        for (i = 0; i < (int) (sizeof(c) / sizeof(c[0])); i++) {
            layerCopy(&scr, c[i][0], c[i][1], c[i][2], c[i][3], c[i][4], c[i][5]);
            n++;
            if (flushEach) {
                bad = refDiff(&scr);
                CHECK(bad == 0, "%s edge copy %d: %ld differ", mode, i, bad);
            }
        }
    }
    /* random, with fills between */
    srand(11);
    for (i = 0; i < 100; i++) {
        int w = 1 + rand() % (W / 2), h = 1 + rand() % (H / 2);
        int sx = rand() % (W - w + 1), sy = rand() % (H - h + 1);
        int dx = rand() % (W - w + 1), dy = rand() % (H - h + 1);

        if (i % 3 == 0) {
            dx = sx + (rand() % 9) - 4; dy = sy + (rand() % 9) - 4;
            if (dx < 0) dx = 0;
            if (dy < 0) dy = 0;
            if (dx + w > W) dx = W - w;
            if (dy + h > H) dy = H - h;
        }
        layerCopy(&scr, sx, sy, w, h, dx, dy);
        if (i % 5 == 0)
            layerFill(&scr, dx, dy, dx + 1 + w / 2, dy + 1 + h / 2, (unsigned long) i * 513);
        n++;
        if (flushEach) {
            bad = refDiff(&scr);
            CHECK(bad == 0, "%s random copy %d: %ld differ", mode, i, bad);
        }
    }
    bad = refDiff(&scr);
    total += bad;
    CHECK(bad == 0, "%s copies (%d, %s): %ld differ", mode, n,
          flushEach ? "flushed each" : "one flush", bad);
    CHECK(cpuCopies == 0, "%s: every copy taken by the blitter (%ld on the CPU)", mode, cpuCopies);
    CHECK(simViol == 0, "%s copies: no model violations", mode);
    (*va2000CardOps.closeHW)(&scr);
}

static void
testCopy(void)
{
    copyRun(AP_Z2, "640x480", 1);
    copyRun(AP_Z2, "640x480", 0);
    copyRun(AP_Z2, "1280x1024", 0);
    copyRun(AP_Z3, "1920x1080", 1);
}

/* fillBox and copyBox start the blit and return; flush waits */
static void
testAsync(void)
{
    long reads;

    cardUp(AP_Z3, NULL);
    cfg.latency = 50;
    CHECK(initScreen("1024x768"), "init");
    refInit(&scr, 3);
    rtgBlitMin = 1;
    reads = simEnableReads;
    layerFill(&scr, 0, 0, 100, 100, 0x1234);
    CHECK(simBusy, "fillBox returned with the blit still running");
    CHECK(simEnableReads - reads == 1, "fillBox read the status once (%ld)", simEnableReads - reads);
    layerCopy(&scr, 0, 0, 100, 100, 50, 50);
    CHECK(simBusy, "copyBox returned with the blit still running");
    CHECK(simViol == 0, "copyBox waited for the fill before its registers");
    layerFill(&scr, 10, 10, 20, 20, 0x4321);
    CHECK(refDiff(&scr) == 0, "after the flush everything has landed");
    CHECK(!simBusy, "flush left the blitter idle");
    RTG_FLUSH(&scr);
    CHECK(simViol == 0, "no model violations");
    (*va2000CardOps.closeHW)(&scr);

    cardUp(AP_Z3, NULL);
    cfg.latency = 0;                    /* finishes before the first read */
    CHECK(initScreen("800x600"), "init, latency 0");
    refInit(&scr, 4);
    layerFill(&scr, 0, 0, 800, 600, 0x7777);
    layerCopy(&scr, 0, 0, 400, 300, 1, 1);
    CHECK(refDiff(&scr) == 0 && simViol == 0, "latency 0");
    (*va2000CardOps.closeHW)(&scr);
}

static void
testSelfTestOk(void)
{
    cardUp(AP_Z2, NULL);
    rtgBlitMin = 1024;
    CHECK(initScreen("800x600"), "init");
    CHECK(rtgBlitMin == 1024, "the self-test keeps -blitmin");
    CHECK(va2000CardOps.caps.flags & RTG_CAP_BLITTER, "the blitter stays");
    CHECK(simOps == 1, "one self-test blit (%ld)", simOps);
    CHECK(FBW[0] == 0, "the self-test pixel is put back");
    CHECK(strstr(errLog, "without the blitter") == NULL, "nothing said");
    CHECK(simViol == 0, "no model violations");
    (*va2000CardOps.closeHW)(&scr);
}

/* the self-test fails: no blitter, said once, still correct on the CPU */
static void
selfTestFails(int how, char *expect)
{
    long reads;

    cardUp(AP_Z3, NULL);
    cfg.blit = how;
    rtgBlitMin = 1024;
    reads = simEnableReads;
    CHECK(initScreen("800x600"), "init with a broken blitter still gives a screen");
    CHECK(rtgBlitMin == 0, "rtgBlitMin 0 (%d)", rtgBlitMin);
    CHECK(!(va2000CardOps.caps.flags & RTG_CAP_BLITTER) && va2000CardOps.caps.blitMin == 0,
          "caps: no blitter");
    CHECK(strstr(errLog, expect) != NULL, "says: %s", expect);
    CHECK(countIn(errLog, "running without the blitter") == 1, "said once");
    if (how != BLIT_NOWRITE)
        CHECK(simEnableReads - reads <= VA2000_BLIT_TIMEOUT + 2,
              "the wait is bounded (%ld reads)", simEnableReads - reads);
    CHECK(FBW[0] == 0, "the test pixel is clear");
    CHECK(!(*va2000CardOps.fillBox)(&scr, 0, 0, 10, 10, 1), "fillBox refuses");
    CHECK(!(*va2000CardOps.copyBox)(&scr, 0, 0, 10, 10, 5, 5), "copyBox refuses");
    refInit(&scr, 5);
    layerFill(&scr, 0, 0, 200, 200, 0xabcd);
    layerCopy(&scr, 0, 0, 200, 200, 10, 10);
    CHECK(refDiff(&scr) == 0, "drawing on the CPU is right");
    (*va2000CardOps.closeHW)(&scr);
    /* a server reset: the blitter is not tried again, nothing said again */
    rtgBlitMin = 1024;
    reads = simOps;
    CHECK(initScreen(NULL), "generation 2");
    CHECK(rtgBlitMin == 0 && simOps == reads, "generation 2: no blitter, no new test");
    CHECK(countIn(errLog, "running without the blitter") == 1, "still said once");
    (*va2000CardOps.closeHW)(&scr);
    CHECK(simLiveMaps() == 0 && simOpenFds == 0, "closed");
}

static void testSelfTestStuck(void)  { selfTestFails(BLIT_STUCK, "did not finish"); }
static void testSelfTestNoWrite(void){ selfTestFails(BLIT_NOWRITE, "did not reach the framebuffer"); }
static void testSelfTestReadsFw(void){ selfTestFails(BLIT_READSFW, "stays busy at start-up"); }

/* the blitter is lost later: what it held is drawn on the CPU */
static void
lostLater(long after, int copyLast)
{
    cardUp(AP_Z2, NULL);
    cfg.blit = BLIT_STUCKAFTER;
    cfg.stuckAfter = after;             /* counts the self-test's blit */
    rtgBlitMin = 1;
    CHECK(initScreen("640x480"), "init");
    CHECK(rtgBlitMin == 1, "the self-test passed");
    refInit(&scr, 6);
    cpuFills = cpuCopies = 0;
    layerFill(&scr, 0, 0, 300, 200, 0x1111);
    layerCopy(&scr, 0, 0, 300, 200, 20, 30);
    if (copyLast)
        layerCopy(&scr, 50, 50, 100, 100, 200, 200);
    else
        layerFill(&scr, 400, 300, 640, 480, 0x2222);
    layerFill(&scr, 600, 0, 640, 40, 0x3333);
    layerCopy(&scr, 0, 0, 640, 479, 0, 1);
    CHECK(refDiff(&scr) == 0, "after the loss the screen is right");
    CHECK(rtgBlitMin == 0, "rtgBlitMin 0");
    CHECK(countIn(errLog, "running without the blitter") == 1, "said once");
    CHECK(strstr(errLog, "blitter timeout") != NULL, "says timeout");
    CHECK(cpuFills + cpuCopies >= 1, "the held operation was drawn on the CPU");
    CHECK(simViol == 0, "no model violations");
    (*va2000CardOps.closeHW)(&scr);
    CHECK(strstr(errLog, "blitter timeouts") != NULL, "closeHW reports the timeouts");
}

static void testLostFill(void) { lostLater(3, 0); }
static void testLostCopy(void) { lostLater(3, 1); }
static void testLostFlush(void) { lostLater(5, 0); }

static void
testMonitorSwitch(void)
{
    cardUp(AP_Z2, NULL);
    (*va2000CardOps.monitorSwitch)(SIM_DEVFD0 + 9, SVGAMONITORSWITCH_SVGA);
    CHECK(simSwitchCalls == 1 && simSwitchFd == SIM_DEVFD0 + 9, "sent on the layer's fd");
    CHECK(simSwitchSize == 2, "an unsigned short (%d bytes)", simSwitchSize);
    CHECK(simSwitchVal == SVGAMONITORSWITCH_SVGA, "SVGA");
    (*va2000CardOps.monitorSwitch)(SIM_DEVFD0 + 9, SVGAMONITORSWITCH_Amiga);
    CHECK(simSwitchSize == 2 && simSwitchVal == SVGAMONITORSWITCH_Amiga, "Amiga");
    CHECK(simRegWrites == 0, "no register written for the switch");
}

static void
testMove16AndBounds(void)
{
    cardUp(AP_Z3, NULL);
    rtgMove16 = 1;
    CHECK(initScreen("1280x720"), "init");
    CHECK(rtgMove16 == 0, "-move16 off on this card");
    CHECK(strstr(errLog, "move16") != NULL, "and said");
    rtgBlitMin = 1;
    refInit(&scr, 8);
    simOps = 0;
    CHECK(!(*va2000CardOps.fillBox)(&scr, 0, 0, 1281, 10, 1), "fill past the right edge refused");
    CHECK(!(*va2000CardOps.fillBox)(&scr, 0, 715, 10, 721, 1), "fill past the bottom refused");
    CHECK(!(*va2000CardOps.fillBox)(&scr, -1, 0, 10, 10, 1), "fill left of 0 refused");
    CHECK(!(*va2000CardOps.fillBox)(&scr, 5, 5, 5, 10, 1), "empty fill refused");
    CHECK(!(*va2000CardOps.copyBox)(&scr, 1200, 0, 100, 10, 0, 0), "copy source past the edge refused");
    CHECK(!(*va2000CardOps.copyBox)(&scr, 0, 0, 100, 10, 0, 711), "copy destination past the edge refused");
    CHECK(!(*va2000CardOps.copyBox)(&scr, 0, 0, 0, 10, 5, 5), "empty copy refused");
    CHECK(simOps == 0 && refDiff(&scr) == 0, "nothing started, nothing drawn");
    CHECK((*va2000CardOps.fillBox)(&scr, 1279, 719, 1280, 720, 9), "the last pixel taken");
    scr.queued = 1;
    refFill(1279, 719, 1280, 720, 9);
    CHECK(refDiff(&scr) == 0 && simViol == 0, "the last pixel right");
    (*va2000CardOps.closeHW)(&scr);
}

/* ===================================================================== */

static const struct { char *name; void (*fn)(void); } groups[] = {
    { "record", testRecord },
    { "probe", testProbe },
    { "plan (modes x Zorro II/III/unknown)", testPlan },
    { "initHW/closeHW (modes x Zorro II/III, failures, reset)", testInit },
    { "fills", testFill },
    { "copies", testCopy },
    { "asynchronous blits and flush", testAsync },
    { "self-test passes", testSelfTestOk },
    { "self-test: blitter stuck busy", testSelfTestStuck },
    { "self-test: blitter writes nothing", testSelfTestNoWrite },
    { "self-test: registers read the firmware byte", testSelfTestReadsFw },
    { "blitter lost later, on a fill", testLostFill },
    { "blitter lost later, on a copy", testLostCopy },
    { "blitter lost later, in flush", testLostFlush },
    { "monitor switch", testMonitorSwitch },
    { "move16 and box bounds", testMove16AndBounds },
};

int
main(int argc, char **argv)
{
    int  g, ng = (int) (sizeof(groups) / sizeof(groups[0])), groupFails = 0;
    long checks = 0, fails = 0;

    verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    printf("va2000 host harness (%s)\n",
#ifdef VA2000_STATS
           "VA2000_STATS"
#else
           "default build"
#endif
           );
    for (g = 0; g < ng; g++) {
        int   fd[2], status;
        long  res[2] = { 0, 1 };
        pid_t pid;

        fflush(stdout);
        if (pipe(fd) != 0) { perror("pipe"); return 2; }
        pid = fork();
        if (pid == 0) {
            close(fd[0]);
            (*groups[g].fn)();
            res[0] = nChecks; res[1] = nFails;
            if (write(fd[1], res, sizeof(res)) != (ssize_t) sizeof(res))
                _exit(3);
            _exit(0);
        }
        close(fd[1]);
        if (read(fd[0], res, sizeof(res)) != (ssize_t) sizeof(res))
            res[0] = 0, res[1] = 1;
        close(fd[0]);
        waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            res[1] = res[1] ? res[1] : 1;
        printf("%-4s %-58s %5ld checks, %ld failed\n", res[1] ? "FAIL" : "ok",
               groups[g].name, res[0], res[1]);
        checks += res[0];
        fails += res[1];
        groupFails += res[1] != 0;
    }
    printf("%d groups, %ld checks, %ld failed\n", ng, checks, fails);
    return fails ? 1 : 0;
}
