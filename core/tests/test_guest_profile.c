/*
 * S5LBox — guest profile histogram and dyld shared cache symbolization.
 *
 * The cache here is synthetic but laid out the way update_dyld_shared_cache
 * writes an iPhone OS 3 armv6 cache: header, mapping table, image table and
 * path strings at the front of the first (text) mapping, each image's
 * mach_header at the start of its text, and every image's LC_SYMTAB pointing
 * into a shared LINKEDIT region by cache-file offset.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "guest_profile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void put32(uint8_t *b, size_t off, uint32_t v) {
    b[off] = (uint8_t)v; b[off + 1] = (uint8_t)(v >> 8);
    b[off + 2] = (uint8_t)(v >> 16); b[off + 3] = (uint8_t)(v >> 24);
}
static void put64(uint8_t *b, size_t off, uint64_t v) {
    put32(b, off, (uint32_t)v); put32(b, off + 4, (uint32_t)(v >> 32));
}

#define TEXT_VA   0x30000000u
#define TEXT_SIZE 0x10000u
#define LE_VA     0x30010000u
#define LE_OFF    0x10000u
#define LE_SIZE   0x4000u
#define FILE_SIZE (LE_OFF + LE_SIZE)

/* A mach_header with __TEXT (vmaddr, vmsize) and, if nsyms, an LC_SYMTAB. */
static void put_macho(uint8_t *b, size_t off, uint32_t vmaddr, uint32_t vmsize,
                      uint32_t symoff, uint32_t nsyms, uint32_t stroff,
                      uint32_t strsize) {
    put32(b, off, 0xfeedfaceu);
    put32(b, off + 4, 12u);            /* ARM */
    put32(b, off + 8, 6u);             /* v6 */
    put32(b, off + 12, 6u);            /* MH_DYLIB */
    put32(b, off + 16, nsyms ? 2u : 1u);
    put32(b, off + 20, nsyms ? 56u + 24u : 56u);
    size_t lc = off + 28u;
    put32(b, lc, 1u);                  /* LC_SEGMENT */
    put32(b, lc + 4, 56u);
    memcpy(b + lc + 8, "__TEXT", 7);
    put32(b, lc + 24, vmaddr);
    put32(b, lc + 28, vmsize);
    if (nsyms) {
        lc += 56u;
        put32(b, lc, 2u);              /* LC_SYMTAB */
        put32(b, lc + 4, 24u);
        put32(b, lc + 8, symoff);
        put32(b, lc + 12, nsyms);
        put32(b, lc + 16, stroff);
        put32(b, lc + 20, strsize);
    }
}

static void put_nlist(uint8_t *b, size_t off, uint32_t strx, uint8_t type,
                      uint16_t desc, uint32_t value) {
    put32(b, off, strx);
    b[off + 4] = type;
    b[off + 5] = 1;
    b[off + 6] = (uint8_t)desc;
    b[off + 7] = (uint8_t)(desc >> 8);
    put32(b, off + 8, value);
}

static uint8_t *make_cache(void) {
    uint8_t *b = calloc(1, FILE_SIZE);
    if (!b) return NULL;
    memcpy(b, "dyld_v1   armv6", 16);
    put32(b, 16, 0x40u); put32(b, 20, 2u);          /* mappings */
    put32(b, 24, 0x80u); put32(b, 28, 3u);          /* images */
    /* mapping 0: text, file 0; mapping 1: linkedit */
    put64(b, 0x40, TEXT_VA); put64(b, 0x48, TEXT_SIZE); put64(b, 0x50, 0);
    put64(b, 0x60, LE_VA); put64(b, 0x68, LE_SIZE); put64(b, 0x70, LE_OFF);
    /* images, deliberately out of address order */
    const struct { uint32_t va; uint32_t path; } img[3] = {
        { 0x30003000u, 0x200u }, { 0x30001000u, 0x240u }, { 0x30008000u, 0x280u },
    };
    for (unsigned i = 0; i < 3; i++) {
        put64(b, 0x80 + i * 32u, img[i].va);
        put32(b, 0x80 + i * 32u + 24u, img[i].path);
    }
    strcpy((char *)b + 0x200, "/usr/lib/libB.dylib");
    strcpy((char *)b + 0x240, "/System/Library/Frameworks/A.framework/A");
    strcpy((char *)b + 0x280, "/usr/lib/libC.dylib");
    /* A: text 0x30001000..0x30003000 with five symbol-table entries, of which
     * two are functions; B: text 0x30003000..0x30004000, no symtab; C: no
     * header at all (bounded by the end of the text mapping). */
    const uint32_t symoff = LE_OFF, stroff = LE_OFF + 0x100u;
    put_macho(b, 0x1000, 0x30001000u, 0x2000u, symoff, 5u, stroff, 0x60u);
    put_macho(b, 0x3000, 0x30003000u, 0x1000u, 0, 0, 0, 0);
    const char *names = "\0_funcA\0_funcB\0_stab\0_undef\0_data";
    memcpy(b + stroff, names, 34);
    put_nlist(b, symoff + 0,  1, 0x0f, 0, 0x30001000u);          /* _funcA, N_SECT|N_EXT */
    put_nlist(b, symoff + 12, 8, 0x0e, 0x0008, 0x30001200u);     /* _funcB, thumb def */
    put_nlist(b, symoff + 24, 15, 0x24, 0, 0x30001100u);         /* a stab: skipped */
    put_nlist(b, symoff + 36, 21, 0x01, 0, 0x30001180u);         /* N_UNDF|N_EXT: skipped */
    put_nlist(b, symoff + 48, 28, 0x0e, 0, 0x3000f000u);         /* outside A's text */
    return b;
}

static void test_histogram(void) {
    gprof_t p, q;
    CHECK(!gprof_init(&p, 3u), "a 3-bit table is refused");
    CHECK(gprof_init(&p, 4u), "a 16-slot table allocates");
    for (int i = 0; i < 5; i++) gprof_note(&p, 0x1001u, true);   /* thumb bit */
    gprof_note(&p, 0x1000u, false);
    gprof_note(&p, 0xc0000000u, false);
    CHECK(p.samples == 7u && p.user == 5u && p.used == 2u,
          "samples %llu user %llu used %u", (unsigned long long)p.samples,
          (unsigned long long)p.user, p.used);
    uint32_t c1000 = 0;
    for (uint32_t i = 0; i < p.cap; i++)
        if (p.slot[i].count && p.slot[i].pc == 0x1000u) c1000 = p.slot[i].count;
    CHECK(c1000 == 6u, "0x1000 counted %u times, expected 6 (thumb bit folded)", c1000);
    /* Fill: 12 distinct pcs fit in 16 slots (a quarter kept free). */
    for (uint32_t pc = 0x2000u; pc < 0x2000u + 40u * 4u; pc += 4u) gprof_note(&p, pc, true);
    CHECK(p.used == 12u && p.dropped == 30u, "full table: used %u dropped %llu",
          p.used, (unsigned long long)p.dropped);
    CHECK(gprof_init(&q, 4u) && gprof_copy(&q, &p) && q.used == p.used &&
          q.samples == p.samples && memcmp(q.slot, p.slot, 16u * sizeof *p.slot) == 0,
          "copy is exact");
    gprof_reset(&p);
    CHECK(p.used == 0 && p.samples == 0 && p.slot[0].count == 0, "reset empties it");
    gprof_t r;
    CHECK(gprof_init(&r, 5u) && !gprof_copy(&r, &q), "copy refuses a different capacity");
    gprof_free(&p); gprof_free(&q); gprof_free(&r);
}

static void test_cache(void) {
    uint8_t *b = make_cache();
    gprof_cache_t c;
    uint32_t off = 0;
    CHECK(b && gprof_cache_open(&c, b, FILE_SIZE), "the synthetic cache opens: %s",
          b ? c.detail : "alloc");
    CHECK(c.nimage == 3u && c.image[0].start == 0x30001000u &&
          c.image[1].start == 0x30003000u && c.image[2].start == 0x30008000u,
          "images sorted by address");
    CHECK(c.image[0].end == 0x30003000u && c.image[1].end == 0x30004000u &&
          c.image[2].end == TEXT_VA + TEXT_SIZE,
          "extents from headers, the last from its mapping: %08x %08x %08x",
          c.image[0].end, c.image[1].end, c.image[2].end);
    CHECK(strcmp(c.detail, "1 of 3 image headers were outside the bytes read") == 0,
          "the headerless image is reported: %s", c.detail);

    gprof_image_t *a = gprof_cache_image_at(&c, 0x30001204u);
    CHECK(a && strcmp(gprof_basename(a->path), "A") == 0, "0x30001204 is in A");
    CHECK(!gprof_cache_image_at(&c, 0x30000800u), "below the first image is nothing");
    CHECK(!gprof_cache_image_at(&c, 0x30005000u), "the gap after B's text is nothing");
    CHECK(gprof_cache_image_at(&c, 0x3000f000u) == &c.image[2], "C runs to the mapping end");

    const char *s = gprof_cache_symbolize(&c, a, 0x30001010u, 0x1000u, &off);
    CHECK(s && strcmp(s, "_funcA") == 0 && off == 0x10u, "_funcA+0x10: %s +%x",
          s ? s : "(null)", off);
    s = gprof_cache_symbolize(&c, a, 0x30001201u, 0x1000u, &off);
    CHECK(s && strcmp(s, "_funcB") == 0 && off == 0u, "a thumb pc names _funcB");
    s = gprof_cache_symbolize(&c, a, 0x30001190u, 0x1000u, &off);
    CHECK(s && strcmp(s, "_funcA") == 0 && off == 0x190u,
          "stab and undefined entries are not symbols: %s", s ? s : "(null)");
    CHECK(a->nsym == 2u, "two symbols kept, %u", a->nsym);
    s = gprof_cache_symbolize(&c, a, 0x30002f00u, 0x100u, &off);
    CHECK(!s, "past max_span is unnamed");
    CHECK(!gprof_cache_symbolize(&c, &c.image[1], 0x30003010u, 0x1000u, &off),
          "an image without a symtab is unnamed");
    gprof_cache_close(&c);

    /* A prefix: headers past it stay unknown, images are still attributed. */
    CHECK(gprof_cache_open(&c, b, 0x2000u), "a 8 KiB prefix opens");
    CHECK(c.image[0].end == 0x30003000u && c.image[1].end == 0x30008000u,
          "past the prefix an image runs to the next one: %08x", c.image[1].end);
    a = gprof_cache_image_at(&c, 0x30001010u);
    CHECK(a && !gprof_cache_symbolize(&c, a, 0x30001010u, 0x1000u, &off),
          "symbols past the prefix are not read");
    gprof_cache_close(&c);

    /* Refusals. */
    CHECK(!gprof_cache_open(&c, b, 0x20u), "a truncated header is refused");
    gprof_cache_close(&c);
    b[0] = 'X';
    CHECK(!gprof_cache_open(&c, b, FILE_SIZE) && strstr(c.detail, "magic"),
          "a wrong magic is refused: %s", c.detail);
    gprof_cache_close(&c);
    b[0] = 'd';
    put32(b, 20, 99u);
    CHECK(!gprof_cache_open(&c, b, FILE_SIZE), "too many mappings is refused");
    gprof_cache_close(&c);
    free(b);
}

int main(void) {
    test_histogram();
    test_cache();
    printf("guest profile: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
