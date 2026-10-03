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

static void test_processes(void) {
    gprof_t p, q;
    CHECK(gprof_init(&p, 6u) && p.nproc == 1u, "proc[0] is reserved at init");
    uint16_t got = 99u;
    CHECK(!gprof_proc_cached(&p, 0x100000u, 8u, &got) && got == 99u, "nothing cached at first");
    const uint16_t a = gprof_proc_intern(&p, 0x100000u, "/usr/libexec/lockdownd");
    const uint16_t b = gprof_proc_intern(&p, 0x104000u, "/System/Library/CoreServices/SpringBoard.app/SpringBoard");
    CHECK(a == 1u && b == 2u && p.nproc == 3u, "two processes: %u %u n %u", a, b, p.nproc);
    CHECK(gprof_proc_intern(&p, 0x100000u, "/usr/libexec/lockdownd") == a, "same pair, same index");
    /* The same TTBR0 reused by a new task is a new process. */
    const uint16_t c = gprof_proc_intern(&p, 0x100000u, "/usr/sbin/mediaserverd");
    CHECK(c == 3u, "a reused TTBR0 with another name is new (%u)", c);
    CHECK(gprof_proc_intern(&p, 0x108000u, "") == 4u, "an unnamed address space counts apart");
    /* The cache answers until `reread` samples, then asks for a fresh read. */
    CHECK(gprof_proc_cached(&p, 0x108000u, 2u, &got) && got == 4u, "cached once");
    CHECK(gprof_proc_cached(&p, 0x108000u, 2u, &got) && got == 4u, "cached twice");
    CHECK(!gprof_proc_cached(&p, 0x108000u, 2u, &got), "then re-read");
    CHECK(!gprof_proc_cached(&p, 0x100000u, 2u, &got), "another TTBR0 is not cached");
    /* Attribution: the same pc in two processes is two slots. */
    gprof_note_in(&p, 0x3145ad59u, true, a);
    gprof_note_in(&p, 0x3145ad58u, true, a);
    gprof_note_in(&p, 0x3145ad58u, false, b);
    gprof_note_in(&p, 0x3145ad58u, true, 60u);     /* out of range: row 0 */
    uint32_t ca = 0, cb = 0, c0 = 0;
    for (uint32_t i = 0; i < p.cap; i++) {
        if (!p.slot[i].count || p.slot[i].pc != 0x3145ad58u) continue;
        if (p.slot[i].proc == a) ca = p.slot[i].count;
        if (p.slot[i].proc == b) cb = p.slot[i].count;
        if (p.slot[i].proc == 0) c0 = p.slot[i].count;
    }
    CHECK(ca == 2u && cb == 1u && c0 == 1u, "per-process slots %u %u %u", ca, cb, c0);
    CHECK(p.proc[a].samples == 2u && p.proc[a].user == 2u && p.proc[b].samples == 1u &&
          p.proc[b].user == 0u && p.proc[0].samples == 1u && p.samples == 4u,
          "per-process totals");
    /* Where in the window each ran: samples 1-2 were a, 3 was b, 4 row 0;
     * c was never sampled. */
    CHECK(p.proc[a].first == 1u && p.proc[a].last == 2u && p.proc[b].first == 3u &&
          p.proc[b].last == 3u && p.proc[0].first == 4u && p.proc[c].first == 0u &&
          p.proc[c].last == 0u, "first/last a %llu-%llu b %llu-%llu",
          (unsigned long long)p.proc[a].first, (unsigned long long)p.proc[a].last,
          (unsigned long long)p.proc[b].first, (unsigned long long)p.proc[b].last);
    CHECK(gprof_init(&q, 6u) && gprof_copy(&q, &p) && q.nproc == p.nproc &&
          !strcmp(q.proc[b].name, p.proc[b].name) && q.proc[a].samples == 2u,
          "copy carries the process table");
    /* Full table: the extra process reads back as 0 and is counted. */
    char name[32];
    for (uint32_t i = 0; p.nproc < GPROF_MAX_PROCS; i++) {
        snprintf(name, sizeof name, "/bin/p%u", i);
        CHECK(gprof_proc_intern(&p, 0x200000u + i * 0x4000u, name) != 0, "fills");
    }
    CHECK(gprof_proc_intern(&p, 0x900000u, "/bin/late") == 0 && p.proc_full == 1u,
          "a full table answers 0");
    got = 99u;
    CHECK(gprof_proc_cached(&p, 0x900000u, 8u, &got) && got == 0u,
          "and that answer is cached, not re-read every sample");
    gprof_reset(&p);
    CHECK(p.nproc == 1u && p.proc[1].samples == 0 && p.last_proc == 0 &&
          !gprof_proc_cached(&p, 0x900000u, 8u, &got), "reset empties the process table");
    gprof_free(&p);
    gprof_free(&q);
}

/* A 1 MiB guest RAM at 0x08000000 with ARMv6 tables in it. */
#define RAM_PA   0x08000000u
#define RAM_SIZE 0x100000u
#define L1_PA    (RAM_PA + 0x0000u)      /* 16 KiB, TTBR0 with N = 0        */
#define L1H_PA   (RAM_PA + 0x4000u)      /* TTBR1's table                   */
#define L2_PA    (RAM_PA + 0x8000u)      /* one coarse table                */

static void test_walk(void) {
    uint8_t *ram = calloc(1, RAM_SIZE);
    gprof_ram_t m = { ram, RAM_PA, RAM_SIZE };
    uint32_t pa = 0;
    /* 0x2ff00000: coarse table; page 0x2ffff -> 0x08050000, page 0x2fffe
     * -> 0x08040000; a 64 KiB large page for 0x2ff10000..; the rest fault. */
    put32(ram, (L1_PA - RAM_PA) + (0x2ffu << 2), L2_PA | 1u);
    put32(ram, (L2_PA - RAM_PA) + (0xffu << 2), 0x08050000u | 0x2u);
    put32(ram, (L2_PA - RAM_PA) + (0xfeu << 2), 0x08040000u | 0x3u);   /* XN small */
    for (unsigned i = 0x10; i < 0x20; i++)
        put32(ram, (L2_PA - RAM_PA) + (i << 2), 0x08060000u | 0x1u);
    CHECK(gprof_va_to_pa(&m, L1_PA, 0, 0, 0x2ffff123u, &pa) && pa == 0x08050123u,
          "small page: 0x%08x", pa);
    CHECK(gprof_va_to_pa(&m, L1_PA, 0, 0, 0x2fffe004u, &pa) && pa == 0x08040004u,
          "extended small page: 0x%08x", pa);
    CHECK(gprof_va_to_pa(&m, L1_PA, 0, 0, 0x2ff1abcdu, &pa) && pa == 0x0806abcdu,
          "large page: 0x%08x", pa);
    CHECK(!gprof_va_to_pa(&m, L1_PA, 0, 0, 0x2fffd000u, &pa), "a fault entry faults");
    CHECK(!gprof_va_to_pa(&m, L1_PA, 0, 0, 0x10000000u, &pa), "an empty L1 entry faults");
    /* Sections and supersections. */
    put32(ram, (L1_PA - RAM_PA) + (0x001u << 2), 0x08000000u | 0x2u);
    for (unsigned i = 0x010; i < 0x020; i++)      /* a supersection spans 16 */
        put32(ram, (L1_PA - RAM_PA) + (i << 2), 0x09000000u | (1u << 18) | 0x2u);
    CHECK(gprof_va_to_pa(&m, L1_PA, 0, 0, 0x00112345u, &pa) && pa == 0x08012345u,
          "section: 0x%08x", pa);
    CHECK(gprof_va_to_pa(&m, L1_PA, 0, 0, 0x01abcdefu, &pa) && pa == 0x09abcdefu,
          "supersection: 0x%08x", pa);
    /* TTBCR.N = 1: 0x80000000 and up walk TTBR1's table. */
    put32(ram, (L1H_PA - RAM_PA) + (0xc00u << 2), 0x08000000u | 0x2u);
    CHECK(gprof_va_to_pa(&m, L1_PA, L1H_PA, 1u, 0xc0000040u, &pa) && pa == 0x08000040u,
          "TTBR1 for the top half: 0x%08x", pa);
    CHECK(!gprof_va_to_pa(&m, L1_PA, L1H_PA, 0u, 0xc0000040u, &pa),
          "N = 0 walks TTBR0 for every address");
    /* A table outside RAM is a miss, not a read out of bounds. */
    CHECK(!gprof_va_to_pa(&m, 0x40000000u, 0, 0, 0x2ffff000u, &pa), "table outside RAM");
    put32(ram, (L1_PA - RAM_PA) + (0x2feu << 2), 0x7ffffc00u | 1u);
    CHECK(!gprof_va_to_pa(&m, L1_PA, 0, 0, 0x2fe00000u, &pa), "L2 outside RAM");
    free(ram);
}

static void test_exec_path(void) {
    uint8_t *ram = calloc(1, RAM_SIZE);
    gprof_ram_t m = { ram, RAM_PA, RAM_SIZE };
    char out[GPROF_NAME_MAX];
    put32(ram, (L1_PA - RAM_PA) + (0x2ffu << 2), L2_PA | 1u);
    /* Only the top page mapped: nothing there yet. */
    put32(ram, (L2_PA - RAM_PA) + (0xffu << 2), 0x08050000u | 0x2u);
    CHECK(!gprof_exec_path(&m, L1_PA, 0, 0, 0x30000000u, out, sizeof out) && !out[0],
          "an empty stack page names nothing");
    /* exec's layout: argc, argv/envp pointers (0x2fffxxxx: bytes xx xx ff 2f),
     * then the path, argv and envp strings up to the top. */
    uint8_t *top = ram + 0x50000u;
    size_t o = 0xe00u;
    put32(top, o, 1u); o += 4;
    put32(top, o, 0x2fffff2fu); o += 4;          /* pointer ending in '/'      */
    put32(top, o, 0); o += 4;
    put32(top, o, 0x2fffff60u); o += 4;
    put32(top, o, 0); o += 4;
    static const char strings[] = "\0/usr/libexec/lockdownd\0lockdownd\0PATH=/usr/bin\0";
    memcpy(top + o, strings, sizeof strings);
    CHECK(gprof_exec_path(&m, L1_PA, 0, 0, 0x30000000u, out, sizeof out) &&
          !strcmp(out, "/usr/libexec/lockdownd"), "the exec path: '%s'", out);
    /* A path that starts in the lower page and ends in the top one. */
    memset(top, 0, 0x1000u);
    put32(ram, (L2_PA - RAM_PA) + (0xfeu << 2), 0x08040000u | 0x2u);
    uint8_t *low = ram + 0x40000u;
    static const char path[] = "/var/mobile/Applications/X/AngryBirds.app/AngryBirds";
    memcpy(low + 0x1000u - 10u, path, 10u);
    memcpy(top, path + 10, sizeof path - 10u);
    CHECK(gprof_exec_path(&m, L1_PA, 0, 0, 0x30000000u, out, sizeof out) &&
          !strcmp(out, path), "a path across the two pages: '%s'", out);
    /* "executable_path=" is dropped; a short output buffer truncates. */
    memset(low, 0, 0x1000u);
    memset(top, 0, 0x1000u);
    static const char apple[] = "\0executable_path=/sbin/launchd\0";
    memcpy(top + 0x100u, apple, sizeof apple);
    char small[6];
    CHECK(gprof_exec_path(&m, L1_PA, 0, 0, 0x30000000u, out, sizeof out) &&
          !strcmp(out, "/sbin/launchd"), "prefix dropped: '%s'", out);
    CHECK(gprof_exec_path(&m, L1_PA, 0, 0, 0x30000000u, small, sizeof small) &&
          !strcmp(small, "/sbin"), "truncated: '%s'", small);
    /* The regression the bc45a3f reports showed: launchd's address space
     * named "/var/mobile/Library/Preferences/com.apple.PortableStorage.plist",
     * "/S+K" and "/dev/md0" by turns, because those strings sat in stack
     * frames BELOW exec's strings. The run starts above the pointer array. */
    memset(low, 0, 0x1000u);
    memset(top, 0, 0x1000u);
    static const char junk[] = "\0/var/mobile/Library/Preferences/com.apple.PortableStorage.plist\0/S+K\0";
    put32(low, 0x7f8u, 0x2fffe810u);                      /* a frame: saved r7 */
    memcpy(low + 0x800u, junk, sizeof junk);
    put32(low, 0x860u, 0x3145b001u);                      /* saved lr */
    o = 0xf00u;
    put32(top, o, 1u); o += 4;                            /* argc */
    put32(top, o, 0x2fffff20u); o += 4;                   /* argv[0] */
    put32(top, o, 0); o += 4;
    put32(top, o, 0x2fffff40u); o += 4;                   /* envp[0] */
    put32(top, o, 0); o += 4;
    static const char exec_strings[] = "/sbin/launchd\0/sbin/launchd\0HOME=/var/root\0";
    memcpy(top + o, exec_strings, sizeof exec_strings);
    CHECK(gprof_exec_path(&m, L1_PA, 0, 0, 0x30000000u, out, sizeof out) &&
          !strcmp(out, "/sbin/launchd"), "a path on the stack below was taken: '%s'", out);
    /* With nothing above the pointer array, a stack path is still not taken. */
    memset(top + 0xf00u, 0, 0x100u);
    put32(top, 0xff8u, 0x2fffff20u);
    CHECK(!gprof_exec_path(&m, L1_PA, 0, 0, 0x30000000u, out, sizeof out),
          "a stack path was taken with no exec strings: '%s'", out);
    /* An unterminated run at the very top is not a path. */
    memset(top, 0, 0x1000u);
    memset(top + 0xff0u, 'a', 16u);
    top[0xfefu] = '/';
    CHECK(!gprof_exec_path(&m, L1_PA, 0, 0, 0x30000000u, out, sizeof out),
          "an unterminated string is not taken");
    CHECK(!gprof_exec_path(&m, L1_PA, 0, 0, 0x30000001u, out, sizeof out) &&
          !gprof_exec_path(NULL, L1_PA, 0, 0, 0x30000000u, out, sizeof out),
          "bad arguments");
    free(ram);
}

static void test_backtrace(void) {
    uint8_t *ram = calloc(1, RAM_SIZE);
    gprof_ram_t m = { ram, RAM_PA, RAM_SIZE };
    uint32_t f[GPROF_STACK_MAX];
    /* Stack pages 0x2fffe000 -> 0x08040000 and 0x2ffff000 -> 0x08050000. */
    put32(ram, (L1_PA - RAM_PA) + (0x2ffu << 2), L2_PA | 1u);
    put32(ram, (L2_PA - RAM_PA) + (0xfeu << 2), 0x08040000u | 0x2u);
    put32(ram, (L2_PA - RAM_PA) + (0xffu << 2), 0x08050000u | 0x2u);
    /* Frames {saved r7, saved lr}: fe00 -> fe40 -> (page cross) 0x2ffff010
     * -> 0x2ffff100 -> 0 (the outermost). Thumb return addresses keep bit 0. */
    uint8_t *lo = ram + 0x40000u, *hi = ram + 0x50000u;
    put32(lo, 0xe00u, 0x2fffee40u); put32(lo, 0xe04u, 0x3145b001u);
    put32(lo, 0xe40u, 0x2ffff010u); put32(lo, 0xe44u, 0x3145c100u);
    put32(hi, 0x010u, 0x2ffff100u); put32(hi, 0x014u, 0x00002f1du);
    put32(hi, 0x100u, 0x00000000u); put32(hi, 0x104u, 0x00001234u);
    unsigned n = gprof_backtrace(&m, L1_PA, 0, 0, 0x3145ad59u, 0x3145a001u, 0x2fffee00u,
                                 f, GPROF_STACK_MAX);
    CHECK(n == 6u && f[0] == 0x3145ad58u && f[1] == 0x3145a000u && f[2] == 0x3145b000u &&
          f[3] == 0x3145c100u && f[4] == 0x00002f1cu && f[5] == 0x00001234u,
          "leaf lr, then the chain across pages: n %u %08x %08x %08x %08x %08x %08x",
          n, f[0], f[1], f[2], f[3], f[4], f[5]);
    /* lr equal to the first saved lr: the frame is pushed, no duplicate. */
    n = gprof_backtrace(&m, L1_PA, 0, 0, 0x3145ad58u, 0x3145b001u, 0x2fffee00u, f, GPROF_STACK_MAX);
    CHECK(n == 5u && f[1] == 0x3145b000u, "no duplicate lr: n %u f1 %08x", n, f[1]);
    /* max clamps. */
    n = gprof_backtrace(&m, L1_PA, 0, 0, 0x3145ad58u, 0x3145a001u, 0x2fffee00u, f, 3u);
    CHECK(n == 3u && f[2] == 0x3145b000u, "clamped to 3");
    /* A chain that does not climb stops after the frame it read. */
    put32(lo, 0xe40u, 0x2fffee00u);
    n = gprof_backtrace(&m, L1_PA, 0, 0, 0x3145ad58u, 0, 0x2fffee00u, f, GPROF_STACK_MAX);
    CHECK(n == 3u && f[1] == 0x3145b000u && f[2] == 0x3145c100u,
          "a loop in the chain stops: n %u", n);
    /* An unmapped or misaligned fp: pc and lr only. */
    n = gprof_backtrace(&m, L1_PA, 0, 0, 0x1000u, 0x2001u, 0x10000000u, f, GPROF_STACK_MAX);
    CHECK(n == 2u && f[0] == 0x1000u && f[1] == 0x2000u, "unmapped fp: n %u", n);
    n = gprof_backtrace(&m, L1_PA, 0, 0, 0x1000u, 0, 0x2fffee02u, f, GPROF_STACK_MAX);
    CHECK(n == 1u, "misaligned fp, no lr: n %u", n);
    CHECK(gprof_backtrace(&m, L1_PA, 0, 0, 0x1000u, 0, 0, f, 0) == 0 &&
          gprof_backtrace(NULL, 0, 0, 0, 0x1000u, 0x2000u, 0x2fffee00u, f, 4u) == 2u,
          "no room; no RAM");
    free(ram);
}

static void test_stacks(void) {
    gprof_t p, q, r;
    CHECK(gprof_init(&p, 6u) && !gprof_init_stacks(&p, 3u) && gprof_init_stacks(&p, 4u) &&
          !gprof_init_stacks(&p, 4u), "stack table sizes");
    const uint16_t a = gprof_proc_intern(&p, 0x100000u, "/usr/libexec/lockdownd");
    const uint32_t s1[3] = { 0x3145ad59u, 0x3145b001u, 0x00002f1du };
    const uint32_t s2[3] = { 0x3145ad58u, 0x3145b000u, 0x00002f1cu };   /* same, bits clear */
    const uint32_t s3[2] = { 0x3145ad58u, 0x3145b000u };
    gprof_note_stack(&p, s1, 3u, a);
    gprof_note_stack(&p, s2, 3u, a);
    gprof_note_stack(&p, s3, 2u, a);          /* a prefix is its own stack */
    gprof_note_stack(&p, s2, 3u, 0);          /* another process          */
    gprof_note_stack(&p, s2, 0u, a);          /* ignored                  */
    uint32_t c3 = 0, c2 = 0, other = 0, entries = 0;
    for (uint32_t i = 0; i < p.stack_cap; i++) {
        const gprof_stack_t *e = &p.stack[i];
        if (!e->count) continue;
        entries++;
        if (e->proc == a && e->depth == 3u && e->frame[2] == 0x2f1cu) c3 = e->count;
        if (e->proc == a && e->depth == 2u) c2 = e->count;
        if (e->proc == 0) other = e->count;
    }
    CHECK(entries == 3u && c3 == 2u && c2 == 1u && other == 1u && p.stack_samples == 4u,
          "stacks: entries %u c3 %u c2 %u other %u", entries, c3, c2, other);
    /* 12 fit in 16 (a quarter free); the rest are counted as dropped. */
    for (uint32_t i = 0; i < 20u; i++) {
        const uint32_t s[1] = { 0x5000u + i * 4u };
        gprof_note_stack(&p, s, 1u, a);
    }
    CHECK(p.stack_used == 12u && p.stack_dropped == 11u, "full: used %u dropped %llu",
          p.stack_used, (unsigned long long)p.stack_dropped);
    CHECK(gprof_init(&q, 6u) && !gprof_copy(&q, &p), "copy needs the same stack capacity");
    CHECK(gprof_init_stacks(&q, 4u) && gprof_copy(&q, &p) && q.stack_used == 12u &&
          memcmp(q.stack, p.stack, 16u * sizeof *p.stack) == 0, "copy carries stacks");
    gprof_reset(&p);
    CHECK(p.stack_used == 0 && p.stack_samples == 0 && p.stack[0].count == 0 &&
          p.stack_cap == 16u, "reset keeps the table, empties it");
    CHECK(gprof_init(&r, 6u), "no stacks");
    gprof_note_stack(&r, s1, 3u, 0);
    CHECK(r.stack_samples == 0, "a table without stacks ignores them");
    gprof_free(&p); gprof_free(&q); gprof_free(&r);
}

int main(void) {
    test_histogram();
    test_cache();
    test_processes();
    test_walk();
    test_exec_path();
    test_backtrace();
    test_stacks();
    printf("guest profile: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
