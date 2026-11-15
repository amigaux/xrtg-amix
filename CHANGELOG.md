# Changelog

Changes in the amigaux fork, newest first. The upstream history is in git.

## 2026-11-15 — the VA2000 driver follows the shared layer's final card interface

On a failed start-up self-test the driver clears its blitter capability and leaves the blit threshold to the shared layer, which reads the capabilities again after `initHW`. The capabilities carry `RTG_CAP_NOMOVE16`; the driver no longer writes the layer's move16 switch. The vendored `rtg.h` in `tests/va2000` is the shared layer's, and the harness checks the new rules.

## 2026-11-15 — the VA2000 card driver for the X11R6.3 RTG server

The MNT VA2000 card driver for the X11R6.3 RTG server `Xrtg`, in `x11r6.3/hw/amix/rtg/va2000`, beside the X11R5 tree, which is unchanged. It provides `va2000CardOps` for `rtg.h` version 1 and is ported from the R5 `va2000hw.c` and `va2000draw.c`. Differences from R5: the registers and the framebuffer are two mappings; closing writes no pass-through registers; the blitter is self-tested at start-up and given up cleanly on a timeout. A host test harness with a software model of the card is in `tests/va2000`. Not yet run on a real card.
