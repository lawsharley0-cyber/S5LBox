/*
 * NEON — the S5L8920's DARTs (see s5l8920_dart.h).
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "s5l8920_dart.h"

#include <string.h>

#define TABLE_MASK UINT32_C(0x0ffff000)

void s5l8920_dart_reset(s5l8920_dart_t *d) {
    memset(d, 0, sizeof *d);
}

uint32_t s5l8920_dart_read(const s5l8920_dart_t *d, uint32_t off) {
    return off < S5L8920_DART_SIZE && !(off & 3u) ? d->reg[off >> 2] : 0u;
}

void s5l8920_dart_write(s5l8920_dart_t *d, uint32_t off, uint32_t v) {
    if (off >= S5L8920_DART_SIZE || (off & 3u)) return;
    d->reg[off >> 2] = v;
    if (off == S5L8920_DART_SLOT) d->slot[(v >> 8) & 0xfu] = v & (TABLE_MASK | 1u);
}

bool s5l8920_dart_translate(const s5l8920_dart_t *d, const s5l8920_dart_ram_t *ram,
                            uint32_t iova, uint32_t *pa) {
    if (!(d->reg[S5L8920_DART_CONTROL / 4u] & S5L8920_DART_ENABLE)) return false;
    const uint32_t slot = d->slot[(iova >> 22) & 0xfu];
    if (!(slot & 1u)) return false;
    const uint32_t at = (slot & TABLE_MASK) + ((iova >> 12) & 0x3ffu) * 4u;
    if ((uint64_t)at + 4u > ram->size) return false;
    const uint8_t *p = ram->ram + at;
    const uint32_t pte = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
                         (uint32_t)p[3] << 24;
    if (!(pte & 1u) || (pte & TABLE_MASK) >= ram->size) return false;
    *pa = ram->base + (pte & TABLE_MASK) + (iova & 0xfffu);
    return true;
}
