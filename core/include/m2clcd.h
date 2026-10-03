/*
 * NEON — the S5L8920's display controller, the iPhone 3GS's /arm-io/clcd
 * (reg 0x05400000 + 0x300000, so 0x85400000; interrupt 0x25; compatible
 * "clcd,s5l8920x"). iOS 6's driver is AppleM2CLCD in
 * com.apple.driver.AppleM2DisplayDrivers (10B500, 0x80993000).
 *
 * What the driver does with it, read from its code (addresses in 10B500):
 *
 *   start_hardware (0x80995652): +0x0 |= 0x100 (soft reset), wait, clear it;
 *   reprogram; write 0xF to +0x1B2C; +0x0 |= 1 (enable), then poll +0x0
 *   until bit 0 reads 1 (six tries, 2 ms apart); 0xF to +0x1B2C again; the
 *   interrupt enable mask to +0x8.
 *
 *   The interrupt (0x809947f4): status = +0xC & the enable mask it keeps
 *   for +0x8. Bit 0 is the frame interrupt: it writes 0xF to +0x1B2C and 1
 *   to +0xC (write one to clear), then completes the pending swap. Bits
 *   0x1700 are underruns, cleared the same way. With no swap pending it
 *   clears bit 0 of +0x8 (frame interrupts off until the next swap).
 *
 *   create_default_fb_surface (0x809961f8) adopts iBoot's framebuffer from
 *   the registers: if +0x4 bit 4, window A (+0x20 control, +0x24 address,
 *   +0x28 stride in pixels, +0x30 size); else if bit 5, window B (+0x40..
 *   +0x50); else there is none. Size: width in bits 24:16, height in bits
 *   8:0. Control bits 11:8 are the format: 7 is 32-bit ARGB (stride * 4
 *   bytes), anything else RGB565 (stride * 2).
 *
 *   It also programs layer blending and a YCbCr->RGB matrix (+0x3C..+0x13C,
 *   +0x2C0..+0x304), an indexed table (+0x408 select, +0x40C data, +0x410
 *   read) and display timing (+0x1B10..+0x1B2C), which openiBoot's iPhone3GS
 *   platform code (independent, GPLv3, read for facts) also writes.
 *
 * The model: a register file that reads back what was written, with these
 * behaviours on top:
 *
 *   - At reset the controller is as iBoot leaves it: running, window A on
 *     and describing the boot framebuffer (m2clcd_reset's arguments).
 *   - +0x0: bit 0 enable reads back as written; bit 1 reads 1 while the
 *     controller is stopped (openiBoot waits for it after clearing bit 0).
 *   - +0xC: status, write one to clear; bit 0 latches at every frame start
 *     while the controller is enabled.
 *   - The interrupt line is (status & +0x8) != 0.
 *   - Frames start every CLCD_FRAME_TICKS of the caller's timebase.
 *
 * Not modelled: underruns (never raised), the soft reset's effect on other
 * registers (none: the driver reprograms what it needs), video layers and
 * colour conversion (stored only), and anything about the panel, which sits
 * behind the MIPI-DSI link.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef NEON_M2CLCD_H
#define NEON_M2CLCD_H

#include <stdbool.h>
#include <stdint.h>

#define M2CLCD_SIZE          UINT32_C(0x2000)   /* registers the driver uses */

#define M2CLCD_CTRL          0x000u
#define M2CLCD_LAYERS        0x004u
#define M2CLCD_INT_ENABLE    0x008u
#define M2CLCD_INT_STATUS    0x00cu
#define M2CLCD_WIN_A         0x020u
#define M2CLCD_WIN_B         0x040u
#define M2CLCD_WIN_CTRL      0x00u   /* within a window */
#define M2CLCD_WIN_ADDR      0x04u
#define M2CLCD_WIN_STRIDE    0x08u
#define M2CLCD_WIN_SIZE      0x10u
#define M2CLCD_UPDATE        0x1b2cu

#define M2CLCD_CTRL_ENABLE   (1u << 0)
#define M2CLCD_CTRL_IDLE     (1u << 1)
#define M2CLCD_CTRL_RESET    (1u << 8)
#define M2CLCD_LAYERS_A      (1u << 4)
#define M2CLCD_LAYERS_B      (1u << 5)
#define M2CLCD_INT_FRAME     (1u << 0)
#define M2CLCD_FORMAT_ARGB   7u      /* window control bits 11:8 */

/* What iBoot leaves the controller showing. */
typedef struct {
    uint32_t addr;          /* physical address of the framebuffer */
    uint32_t width, height; /* pixels */
    uint32_t stride;        /* pixels per row */
} m2clcd_boot_fb_t;

/* What is on screen: the first enabled window, as the driver reads it. */
typedef struct {
    uint32_t addr;
    uint32_t width, height;
    uint32_t stride_bytes;
    uint32_t bpp;           /* 32 (ARGB) or 16 (RGB565) */
} m2clcd_scanout_t;

typedef struct {
    uint32_t reg[M2CLCD_SIZE / 4u];
    uint64_t next_frame;    /* timebase tick of the next frame start */
    uint64_t frames;        /* frame starts while enabled */
    uint64_t frame_ticks;   /* period in timebase ticks */
} m2clcd_t;

/* Reset to iBoot's hand-off: enabled, window A = *fb, 32-bit ARGB, no
 * interrupts enabled. `now` is the current timebase tick; frames start
 * every `frame_ticks` from it (0: never). */
void m2clcd_reset(m2clcd_t *c, const m2clcd_boot_fb_t *fb, uint64_t now,
                  uint64_t frame_ticks);

uint32_t m2clcd_read(m2clcd_t *c, uint32_t off);
void     m2clcd_write(m2clcd_t *c, uint32_t off, uint32_t v);

/* Bring frame starts up to `now`. True if one or more happened. */
bool     m2clcd_advance(m2clcd_t *c, uint64_t now);
/* The tick of the next frame start that would raise the line, or
 * UINT64_MAX when no frame interrupt is enabled. */
uint64_t m2clcd_due(const m2clcd_t *c);
bool     m2clcd_irq(const m2clcd_t *c);

/* The scanout, or false when no window is enabled or it has no size. */
bool     m2clcd_scanout(const m2clcd_t *c, m2clcd_scanout_t *out);

#endif /* NEON_M2CLCD_H */
