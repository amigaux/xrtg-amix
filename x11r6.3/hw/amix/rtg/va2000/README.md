# VA2000 card driver for the X11R6.3 RTG server

This directory is the MNT VA2000 card driver for an X11R6.3 X server on
Amiga UNIX (AMIX). It is a port of the VA2000 code in this repository's
X11R5 server (`usr/x11r5/server/ddx/amix/rtg/va2000/`). The R5 server and
its install scripts are not changed by it.

**Status: nothing in this directory has run on a real VA2000.** The card
cannot be emulated. What has been checked: it compiles for the target with
the userland cross compiler (GCC 14, `-O2 -Wall`, no warnings) against the
server headers of the patched X11R6.3 tree, and a host test harness
(`tests/va2000`) passes, running it against a software model of the card.
The first results on the card will come from testers.

## What it is

The X11R6.3 server's RTG layer (`programs/Xserver/hw/amix/rtg/`, in the
x11r6.3 package's patches) runs X's cfb16 code on a mapped framebuffer, and
reaches a card only through one record, `rtgCardOps` (`rtg.h`,
`RTG_CARD_VERSION` 1). This directory supplies that record for the VA2000:
`va2000CardOps`.

| File | Contents |
|---|---|
| `va2000.h` | memory map, ioctl numbers, mode record, registers, private record |
| `va2000hw.c` | the record; modes; probe; the two mappings; the mode registers; the monitor switch |
| `va2000blit.c` | the blitter: fill, copy, flush, the bounded wait, the start-up self-test |
| `Imakefile` | builds `libva2000.a` |

What the card driver does:

- **Modes:** 640x480, 800x600 (default), 1024x768, 1280x720, 1280x1024 and
  1920x1080, with the R5 driver's timings, written from user space in the
  R5 driver's register order. The kernel driver says RTG activation needs a
  user-space write first.
- **The board:** `SVGAIOCGetFBufSize` gives the aperture, and from that the
  bus: 4 MB in Zorro II, 32 MB in Zorro III. A mode that does not fit is
  refused: 1920x1080 needs 4050 KB, and a Zorro II board has 4032 KB. With a
  driver that does not answer, 2 MB is assumed, as the R5 driver did.
- **Two mappings:** the register page (4 KB at offset 0) and the mode's
  framebuffer (rounded up to a page, at offset 0x10000), each checked
  against the mode and the aperture. The R5 driver mapped one region with
  slack.
- **The blitter:** fills and screen-to-screen copies of at least `-blitmin`
  pixels (default 1024; a guess, still to be measured). An operation is
  started and left running, and the layer's flush waits for it. Every wait
  is bounded (500000 status reads, about 100 ms). Overlapping copies use the
  R5 driver's two directions. A wait that reaches the bound means the
  blitter is lost for the rest of the server's life. The card then draws
  what the blitter held with the CPU and sets `rtgBlitMin` to 0.
- **Self-test:** at every screen start the blitter must be idle, and a 1x1
  fill must finish within the bound and appear in the framebuffer. If not,
  the server runs without the blitter and says so once.
- **Monitor:** the switch sends `SVGAIOCSetMonitorSwitch` with an
  `unsigned short`, which is what the VA2000 kernel driver reads. Closing the
  screen writes no register. At a server reset the picture stays. When the
  server ends, the layer's monitor switch and the driver's last close bring
  the Amiga video back. The R5 driver wrote the pass-through sequence at
  every close, so the monitor flashed at each reset.
- **move16:** forced off on this card.
- **Counters:** off by default. Building with `-DVA2000_STATS` adds the
  blitter counters and a log of slow waits. There are no timing calls.

Not carried over from the R5 driver: its own screen, GC, clip, colormap,
pixmap, window, text and tile code (the layer's cfb16 screen does all of
this), and the option bits (`-vaopt`, `-compat`, `-bus`).

## How the x11r6.3 package takes it in

The x11r6.3 package build pins a commit of this repository and copies this
directory into the patched tree after its patches (it needs the card table
`rtgcards.c` of the card-neutral layer):

```
cp -R <this repo>/x11r6.3/hw/amix/rtg/va2000 xc/programs/Xserver/hw/amix/rtg/va2000
```

Then, in the tree:

- `programs/Xserver/hw/amix/rtg/Imakefile`: add `va2000` to `SUBDIRS` and
  `-DRTG_VA2000` to `RTGCARDS`. The card table lists `va2000CardOps` under
  `#ifdef RTG_VA2000`.
- `programs/Xserver/Imakefile`: add
  `hw/amix/rtg/va2000/LibraryTargetName(va2000)` to the server's libraries,
  after the `rtg` library, next to the other card libraries.

The card's own files are not patched in the package. A change to them is
a change here, followed by a new pin.

## Tests

```
make -C tests/va2000 check
```

This builds the card files for the host twice (the default build and
`VA2000_STATS`) and runs them against a model of `/dev/va2000` and of the
card. The model covers open, close, `GETFW`, `GetFBufSize`,
`SetMonitorSwitch` and mmap over a Zorro II, Zorro III or unknown aperture;
the register page; and a blitter. The blitter takes inclusive coordinates
and starting-row base addresses, runs forward or reverse by the order of its
coordinates, moves one pixel at a time, stays busy for a set number of
status reads, and can be made to stick, to do nothing, or to read the
firmware byte from every register. Results are compared with a plain CPU
fill and copy. `RTG_H=<path>` runs the tests against another `rtg.h`, for
example the patched tree's. `tests/va2000/rtg.h` is a copy of version 1.

## To measure on the card

- whether the blitter-busy bit reads back on every firmware (the self-test
  says so);
- the `-blitmin` crossover, for fills and copies, on Zorro II and III;
- solid fills from PolyFillRect through the blitter (the R5 driver never
  sent those to the blitter);
- screen switching and the monitor switch with the pass-through.

## Licence

MIT. Copyright (c) 2025-2026 Antti Sokero; Copyright (c) 2026 Jussi
Alanärä. See `LICENSE` in the repository root. The SVGAIOC numbers follow
`svgaioctl.h` by Georg Hessmann and Klaus Burckert.
