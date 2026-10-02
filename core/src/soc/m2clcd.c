/*
 * NEON — the S5L8920's display controller (see m2clcd.h).
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "m2clcd.h"

#include <string.h>

#define R(off) c->reg[(off) >> 2]

void m2clcd_reset(m2clcd_t *c, const m2clcd_boot_fb_t *fb, uint64_t now,
                  uint64_t frame_ticks) {
    memset(c, 0, sizeof *c);
    c->frame_ticks = frame_ticks;
    c->next_frame = frame_ticks ? now + frame_ticks : UINT64_MAX;
    R(M2CLCD_CTRL) = M2CLCD_CTRL_ENABLE;
    if (fb && fb->width && fb->height) {
        R(M2CLCD_LAYERS) = M2CLCD_LAYERS_A;
        R(M2CLCD_WIN_A + M2CLCD_WIN_CTRL) = M2CLCD_FORMAT_ARGB << 8;
        R(M2CLCD_WIN_A + M2CLCD_WIN_ADDR) = fb->addr;
        R(M2CLCD_WIN_A + M2CLCD_WIN_STRIDE) = fb->stride;
        R(M2CLCD_WIN_A + M2CLCD_WIN_SIZE) = (fb->width & 0x1ffu) << 16 | (fb->height & 0x1ffu);
    }
}

uint32_t m2clcd_read(m2clcd_t *c, uint32_t off) {
    if (off >= M2CLCD_SIZE || (off & 3u)) return 0u;
    uint32_t v = R(off);
    if (off == M2CLCD_CTRL) {
        v &= ~M2CLCD_CTRL_IDLE;
        if (!(v & M2CLCD_CTRL_ENABLE)) v |= M2CLCD_CTRL_IDLE;
    }
    return v;
}

void m2clcd_write(m2clcd_t *c, uint32_t off, uint32_t v) {
    if (off >= M2CLCD_SIZE || (off & 3u)) return;
    if (off == M2CLCD_INT_STATUS) { R(off) &= ~v; return; }    /* write one to clear */
    if (off == M2CLCD_CTRL) v &= ~M2CLCD_CTRL_IDLE;            /* read-only */
    R(off) = v;
}

bool m2clcd_advance(m2clcd_t *c, uint64_t now) {
    if (!c->frame_ticks || now < c->next_frame) return false;
    const uint64_t n = (now - c->next_frame) / c->frame_ticks + 1u;
    c->next_frame += n * c->frame_ticks;
    if (!(R(M2CLCD_CTRL) & M2CLCD_CTRL_ENABLE)) return false;
    c->frames += n;
    R(M2CLCD_INT_STATUS) |= M2CLCD_INT_FRAME;
    return true;
}

uint64_t m2clcd_due(const m2clcd_t *c) {
    if (!c->frame_ticks || !(R(M2CLCD_CTRL) & M2CLCD_CTRL_ENABLE) ||
        !(R(M2CLCD_INT_ENABLE) & M2CLCD_INT_FRAME))
        return UINT64_MAX;
    return c->next_frame;
}

bool m2clcd_irq(const m2clcd_t *c) {
    return (R(M2CLCD_INT_STATUS) & R(M2CLCD_INT_ENABLE)) != 0u;
}

bool m2clcd_scanout(const m2clcd_t *c, m2clcd_scanout_t *out) {
    const uint32_t layers = R(M2CLCD_LAYERS);
    uint32_t w;
    if (layers & M2CLCD_LAYERS_A) w = M2CLCD_WIN_A;
    else if (layers & M2CLCD_LAYERS_B) w = M2CLCD_WIN_B;
    else return false;
    const uint32_t size = R(w + M2CLCD_WIN_SIZE);
    const uint32_t width = (size >> 16) & 0x1ffu, height = size & 0x1ffu;
    if (!width || !height) return false;
    const bool argb = ((R(w + M2CLCD_WIN_CTRL) >> 8) & 0xfu) == M2CLCD_FORMAT_ARGB;
    out->addr = R(w + M2CLCD_WIN_ADDR);
    out->width = width;
    out->height = height;
    out->bpp = argb ? 32u : 16u;
    out->stride_bytes = R(w + M2CLCD_WIN_STRIDE) * (argb ? 4u : 2u);
    return true;
}
