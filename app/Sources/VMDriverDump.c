/*
 * S5LBox — what a guest driver calls. See VMDriverDump.h.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "VMDriverDump.h"

#include <stdlib.h>

typedef struct {
    uint32_t target;
    uint8_t  kind;
} raw_ref_t;

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }

static int raw_cmp(const void *a, const void *b) {
    const raw_ref_t *x = a, *y = b;
    return x->target < y->target ? -1 : x->target > y->target;
}

size_t vm_driver_collect_refs(const uint8_t *code, uint32_t va, uint32_t len,
                              uint32_t range_lo, uint32_t range_hi,
                              vm_driver_ref_t *out, size_t cap, size_t *total) {
    if (total) *total = 0;
    if (!code || len < 4u || range_lo >= range_hi) return 0;
    /* At most one word reference and one ARM branch per word, and one Thumb
     * pair per halfword. */
    const size_t most = (size_t)len / 2u + (size_t)len / 4u * 2u;
    raw_ref_t *raw = malloc(most * sizeof *raw);
    if (!raw) return 0;
    size_t n = 0;
#define KEEP(t, k) do { const uint32_t t_ = (t) & ~1u; \
        if (t_ >= range_lo && t_ < range_hi && (t_ < va || t_ - va >= len)) { \
            raw[n].target = t_; raw[n].kind = (k); n++; } } while (0)

    for (uint32_t off = 0; off + 4u <= len; off += 2u) {
        const uint32_t pc = va + off;
        /* Thumb BL/BLX: 11110 imm11(high) then 11111 (BL) / 11101 (BLX). */
        const uint32_t h1 = rd16(code + off), h2 = rd16(code + off + 2u);
        if ((h1 & 0xf800u) == 0xf000u &&
            ((h2 & 0xf800u) == 0xf800u || (h2 & 0xf800u) == 0xe800u)) {
            uint32_t hi = h1 & 0x7ffu;
            if (hi & 0x400u) hi |= 0xfffff800u;             /* sign-extend */
            uint32_t target = pc + 4u + (hi << 12) + ((h2 & 0x7ffu) << 1);
            if ((h2 & 0xf800u) == 0xe800u) target &= ~3u;   /* BLX to ARM */
            KEEP(target, VM_DRIVER_REF_CALL);
        }
        if (off & 3u) continue;
        const uint32_t w = rd32(code + off);
        /* ARM B/BL (cond != 1111, 101L in 27:24; a B out of the kext is a
         * tail call) and BLX imm (1111 101H). */
        if ((w & 0x0e000000u) == 0x0a000000u) {
            uint32_t imm = w & 0x00ffffffu;
            if (imm & 0x00800000u) imm |= 0xff000000u;
            uint32_t target = pc + 8u + (imm << 2);
            if ((w >> 28) == 0xfu) target += (w >> 23) & 2u;    /* H */
            KEEP(target, VM_DRIVER_REF_CALL);
        }
        KEEP(w, VM_DRIVER_REF_WORD);
    }
#undef KEEP

    qsort(raw, n, sizeof *raw, raw_cmp);
    size_t distinct = 0, stored = 0;
    for (size_t i = 0; i < n;) {
        size_t j = i;
        uint8_t kinds = 0;
        while (j < n && raw[j].target == raw[i].target) kinds |= raw[j++].kind;
        if (stored < cap && out) {
            out[stored].target = raw[i].target;
            out[stored].count = (uint32_t)(j - i);
            out[stored].kinds = kinds;
            stored++;
        }
        distinct++;
        i = j;
    }
    free(raw);
    if (total) *total = distinct;
    return stored;
}
