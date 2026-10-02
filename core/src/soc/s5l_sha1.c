/*
 * NEON — the S5L8920's SHA-1 engine (see s5l_sha1.h). The compression
 * function is FIPS 180-4's.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "s5l_sha1.h"

#include <string.h>

static const uint32_t k_iv[5] = {
    0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u
};

static uint32_t rol(uint32_t x, unsigned n) { return (x << n) | (x >> (32u - n)); }
static uint32_t bswap(uint32_t x) {
    return (x >> 24) | ((x >> 8) & 0xff00u) | ((x << 8) & 0xff0000u) | (x << 24);
}

static void compress(uint32_t h[5], const uint8_t b[64]) {
    uint32_t w[80];
    for (unsigned i = 0; i < 16u; i++)
        w[i] = (uint32_t)b[4*i] << 24 | (uint32_t)b[4*i+1] << 16 |
               (uint32_t)b[4*i+2] << 8 | b[4*i+3];
    for (unsigned i = 16; i < 80u; i++) w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4];
    for (unsigned i = 0; i < 80u; i++) {
        uint32_t f, k;
        if (i < 20u)      { f = (bb & c) | (~bb & d);          k = 0x5a827999u; }
        else if (i < 40u) { f = bb ^ c ^ d;                    k = 0x6ed9eba1u; }
        else if (i < 60u) { f = (bb & c) | (bb & d) | (c & d); k = 0x8f1bbcdcu; }
        else              { f = bb ^ c ^ d;                    k = 0xca62c1d6u; }
        const uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(bb, 30); bb = a; a = t;
    }
    h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e;
}

void s5l_sha1_reset(s5l_sha1_t *s) {
    memset(s, 0, sizeof *s);
    memcpy(s->h, k_iv, sizeof s->h);
}

uint32_t s5l_sha1_read(const s5l_sha1_t *s, uint32_t off) {
    if (off >= S5L_SHA1_SIZE || (off & 3u)) return 0u;
    if (off >= S5L_SHA1_STATE && off < S5L_SHA1_STATE + 20u)
        return bswap(s->h[(off - S5L_SHA1_STATE) >> 2]);
    return s->reg[off >> 2];
}

void s5l_sha1_feed(s5l_sha1_t *s, const uint8_t *data, size_t len) {
    while (len) {
        size_t n = 64u - s->fill;
        if (n > len) n = len;
        memcpy(s->block + s->fill, data, n);
        s->fill += (unsigned)n;
        data += n;
        len -= n;
        if (s->fill == 64u) {
            compress(s->h, s->block);
            s->fill = 0;
            s->blocks++;
        }
    }
}

void s5l_sha1_write(s5l_sha1_t *s, uint32_t off, uint32_t v) {
    if (off >= S5L_SHA1_SIZE || (off & 3u)) return;
    if (off >= S5L_SHA1_STATE && off < S5L_SHA1_STATE + 20u) {
        s->h[(off - S5L_SHA1_STATE) >> 2] = bswap(v);
        return;
    }
    if (off == S5L_SHA1_FIFO) {
        const uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
        s5l_sha1_feed(s, b, 4);
        return;
    }
    s->reg[off >> 2] = v;
    if (off == S5L_SHA1_RESET && (v & 1u)) s->fill = 0;
    if (off == S5L_SHA1_CONTROL && (v & S5L_SHA1_START)) {
        s->fill = 0;
        if (!(v & S5L_SHA1_CONTINUE)) memcpy(s->h, k_iv, sizeof s->h);
    }
}
