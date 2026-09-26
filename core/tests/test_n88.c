/*
 * NEON — the iPhone 3GS machine (n88.h), on synthetic inputs only.
 *
 * No Apple bytes: the kernel is a hand-built Mach-O holding a few ARM
 * instructions and the device tree is built here with just the nodes
 * bring-up fills in. What is checked:
 *   - the bus: DRAM, the UART's status and transmit registers, unmodelled
 *     addresses answering zero, counted and traced;
 *   - the PMGR timer: count = cycles / 25, the decrementer's read-back and
 *     expiry, the enable/acknowledge control, VIC0 line 6;
 *   - a timer FIQ taken by a real program through the vector table, both
 *     from WFI (time jumps to the expiry) and from a busy loop (the
 *     interrupt lands on the same instruction on both CPU engines);
 *   - the NVRAM image's partitions and CHRP checksums;
 *   - bring-up: layout, boot_args version 5, every device-tree property it
 *     fills in, the default and explicit un-match lists, and each refusal;
 *   - the console ring, including overflow;
 *   - telling a 3GS tree from another.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "n88.h"
#include "macho.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass, g_fail;

#define CHECK(cond, ...) do {                                               \
    if (cond) g_pass++;                                                     \
    else {                                                                  \
        g_fail++;                                                           \
        printf("  FAIL %s:%d: ", __func__, __LINE__);                       \
        printf(__VA_ARGS__);                                                \
        printf("\n");                                                       \
    }                                                                       \
} while (0)

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}
static uint8_t *ram_at(n88_t *m, uint32_t pa) { return m->ram + (pa - N88_DRAM_BASE); }

/* ------------------------------------------------------- A32 encodings */

static uint32_t movw(unsigned rd, uint32_t imm) {
    return 0xe3000000u | (imm >> 12) << 16 | rd << 12 | (imm & 0xfffu);
}
static uint32_t movt(unsigned rd, uint32_t imm) {
    return 0xe3400000u | (imm >> 12) << 16 | rd << 12 | (imm & 0xfffu);
}
static uint32_t mov_imm(unsigned rd, uint32_t imm8) { return 0xe3a00000u | rd << 12 | imm8; }
static uint32_t str_off(unsigned rt, unsigned rn, uint32_t off) {
    return 0xe5800000u | rn << 16 | rt << 12 | off;
}
static uint32_t ldr_off(unsigned rt, unsigned rn, uint32_t off) {
    return 0xe5900000u | rn << 16 | rt << 12 | off;
}
static uint32_t add_imm(unsigned rd, unsigned rn, uint32_t imm8) {
    return 0xe2800000u | rn << 16 | rd << 12 | imm8;
}
static uint32_t cmp_imm(unsigned rn, uint32_t imm8) { return 0xe3500000u | rn << 16 | imm8; }
/* B<cond> from `at` to `to`. */
static uint32_t branch(uint32_t cond, uint32_t at, uint32_t to) {
    return cond << 28 | 0x0a000000u | (((to - at - 8u) >> 2) & 0x00ffffffu);
}
#define AL 0xeu
#define EQ 0x0u
#define B_SELF      0xeafffffeu
#define SUBS_PC_LR4 0xe25ef004u
#define CPSIE_F     0xf1080040u
#define WFI         0xe320f003u

/* Emit words at a physical address. */
typedef struct { n88_t *m; uint32_t pa; } emit_t;
static void emit(emit_t *e, uint32_t insn) { put32(ram_at(e->m, e->pa), insn); e->pa += 4u; }
static void emit_const(emit_t *e, unsigned rd, uint32_t v) {
    emit(e, movw(rd, v & 0xffffu));
    emit(e, movt(rd, v >> 16));
}

/* ----------------------------------------------------------------- bus */

typedef struct { unsigned calls; uint32_t pa, value; bool write; unsigned size; } trace_log_t;
static void trace_cb(void *ctx, uint32_t pa, unsigned size, bool write, uint32_t value,
                     uint32_t pc) {
    trace_log_t *t = ctx;
    (void)pc;
    t->calls++;
    t->pa = pa; t->size = size; t->write = write; t->value = value;
}

static void test_bus_routing(void) {
    n88_t *m = malloc(sizeof *m);
    CHECK(m && n88_init(m, false), "init");
    if (!m || !m->ram) { free(m); return; }
    trace_log_t t = {0};
    m->trace = trace_cb;
    m->trace_ctx = &t;

    n88_write32(m, N88_DRAM_BASE + 0x100u, 0xdeadbeefu);
    CHECK(n88_read32(m, N88_DRAM_BASE + 0x100u) == 0xdeadbeefu, "DRAM round trip");
    n88_write32(m, N88_DRAM_BASE + N88_DRAM_SIZE - 4u, 0x12345678u);
    CHECK(get32(ram_at(m, N88_DRAM_BASE + N88_DRAM_SIZE - 4u)) == 0x12345678u,
          "the last DRAM word is DRAM");
    CHECK(m->mmio == 0 && m->unmodelled == 0 && t.calls == 0, "DRAM is not a device");

    CHECK(n88_read32(m, N88_UART0_PA + 0x10u) == 0x6u, "UTRSTAT reports an empty transmitter");
    n88_write32(m, N88_UART0_PA + 0x20u, 'A');
    n88_write32(m, N88_UART0_PA + 0x20u, 0x142u);       /* only the low byte */
    char out[8];
    CHECK(n88_console_take(m, out, sizeof out) == 2 && out[0] == 'A' && out[1] == 'B',
          "UTXH bytes reach the console");
    CHECK(t.calls == 0 && m->unmodelled == 0, "the UART is modelled");

    CHECK(n88_read32(m, 0x81234560u) == 0u, "an unmodelled register answers zero");
    CHECK(m->unmodelled == 1 && t.calls == 1 && t.pa == 0x81234560u && !t.write &&
          t.size == 4, "and is counted and traced");
    n88_write32(m, 0x3fff0000u, 7u);            /* below DRAM */
    CHECK(m->unmodelled == 2 && t.calls == 2 && t.write && t.value == 7u &&
          t.pa == 0x3fff0000u, "an unmodelled store is counted and traced");
    n88_free(m);
    free(m);
}

/* --------------------------------------------------------------- timer */

static void test_timer_registers(void) {
    n88_t *m = malloc(sizeof *m);
    CHECK(m && n88_init(m, false), "init");
    if (!m || !m->ram) { free(m); return; }

    m->cpu.cycles = 2500u;                      /* 100 ticks */
    CHECK(n88_timer_count(m) == 100u, "count = cycles / 25");
    CHECK(n88_read32(m, N88_TIMER_PA) == 100u && n88_read32(m, N88_TIMER_PA + 4u) == 0u,
          "the count's two halves");
    m->cpu.cycles = (UINT64_C(1) << 32) * N88_CYCLES_PER_TICK + 50u;
    CHECK(n88_read32(m, N88_TIMER_PA + 4u) == 1u && n88_read32(m, N88_TIMER_PA) == 2u,
          "the high half carries");
    m->cpu.cycles = 2500u;

    CHECK(n88_read32(m, N88_TIMER_PA + 8u) == 0u, "an unarmed decrementer reads zero");
    n88_write32(m, N88_TIMER_PA + 8u, 40u);
    m->cpu.cycles += 10u * N88_CYCLES_PER_TICK;
    CHECK(n88_read32(m, N88_TIMER_PA + 8u) == 30u, "the decrementer counts down");
    CHECK(!m->timer.pending, "not yet");

    /* Enable the interrupt, route it as the kernel does (FIQ), let it expire. */
    n88_write32(m, N88_TIMER_PA + 0x20u, 1u);
    n88_write32(m, N88_VIC_PA + VIC_INTSELECT, 1u << N88_TIMER_LINE);
    n88_write32(m, N88_VIC_PA + VIC_INTENABLE, 1u << N88_TIMER_LINE);
    CHECK(!m->cpu.fiq_line && !m->cpu.irq_line, "no line before the expiry");
    m->cpu.cycles += 30u * N88_CYCLES_PER_TICK;
    n88_write32(m, N88_VIC_PA + VIC_INTENABLE, 1u << N88_TIMER_LINE);  /* any store updates */
    CHECK(m->timer.pending && m->timer.fired == 1 && !m->timer.armed, "expired once");
    CHECK(m->cpu.fiq_line && !m->cpu.irq_line, "VIC0 line 6 as FIQ");
    CHECK(n88_read32(m, N88_VIC_PA + VIC_FIQSTATUS) == 1u << N88_TIMER_LINE &&
          n88_read32(m, N88_VIC_PA + VIC_IRQSTATUS) == 0u, "VIC0 reports it as a FIQ");
    CHECK(n88_read32(m, N88_TIMER_PA + 8u) == 0u, "an expired decrementer reads zero");

    /* The handler's sequence: control | 2 (acknowledge), then control. */
    n88_write32(m, N88_TIMER_PA + 0x20u, 3u);
    CHECK(!m->timer.pending && !m->cpu.fiq_line, "bit 1 acknowledges");
    n88_write32(m, N88_TIMER_PA + 0x20u, 1u);
    CHECK(n88_read32(m, N88_TIMER_PA + 0x20u) == 1u, "control reads back without bit 1");

    /* Disabled: it still expires, but raises nothing. */
    n88_write32(m, N88_TIMER_PA + 0x20u, 0u);
    n88_write32(m, N88_TIMER_PA + 8u, 5u);
    m->cpu.cycles += 5u * N88_CYCLES_PER_TICK;
    n88_write32(m, N88_VIC_PA + VIC_INTENABLE, 1u << N88_TIMER_LINE);
    CHECK(m->timer.pending && m->timer.fired == 2 && !m->cpu.fiq_line,
          "disabled: pending without a line");
    n88_free(m);
    free(m);
}

/*
 * A timer FIQ through a real vector table. The MMU maps VA 0 and VA
 * 0x40000000 to the first DRAM megabyte (sections), so the low vectors are
 * RAM. The handler acknowledges the timer, bumps a counter at 0x40002000 and
 * records r6 at 0x40002004. `wait` selects WFI (time jumps to the expiry)
 * over a counting loop in r6 (the interrupt must land on an exact
 * instruction).
 */
#define TICKS 1000u
static void load_fiq_program(n88_t *m, bool wait) {
    emit_t e = { m, N88_DRAM_BASE };
    for (int i = 0; i < 7; i++) emit(&e, B_SELF);           /* 0x00..0x18 */
    /* 0x1c: FIQ. r8-r12 are banked. */
    emit_const(&e, 8, N88_TIMER_PA + 0x20u);
    emit(&e, mov_imm(9, 3)); emit(&e, str_off(9, 8, 0));
    emit(&e, mov_imm(9, 1)); emit(&e, str_off(9, 8, 0));
    emit_const(&e, 10, N88_DRAM_BASE + 0x2000u);
    emit(&e, ldr_off(11, 10, 0)); emit(&e, add_imm(11, 11, 1)); emit(&e, str_off(11, 10, 0));
    emit(&e, str_off(6, 10, 4));
    emit(&e, SUBS_PC_LR4);

    e.pa = N88_DRAM_BASE + 0x1000u;
    emit_const(&e, 0, N88_VIC_PA);
    emit(&e, mov_imm(1, 1u << N88_TIMER_LINE));
    emit(&e, str_off(1, 0, VIC_INTSELECT));
    emit(&e, str_off(1, 0, VIC_INTENABLE));
    emit_const(&e, 2, N88_TIMER_PA);
    emit(&e, movw(3, TICKS)); emit(&e, str_off(3, 2, 8));
    emit(&e, mov_imm(3, 1)); emit(&e, str_off(3, 2, 0x20));
    emit(&e, mov_imm(6, 0));
    emit(&e, CPSIE_F);
    if (wait) {
        emit(&e, WFI);
    } else {
        const uint32_t loop = e.pa;
        emit_const(&e, 10, N88_DRAM_BASE + 0x2000u);
        emit(&e, ldr_off(11, 10, 0));
        emit(&e, add_imm(6, 6, 1));
        emit(&e, cmp_imm(11, 0));
        emit(&e, branch(EQ, e.pa, loop));
    }
    const uint32_t check = e.pa;
    emit_const(&e, 10, N88_DRAM_BASE + 0x2000u);
    emit(&e, ldr_off(11, 10, 0));
    emit(&e, cmp_imm(11, 0));
    emit(&e, branch(EQ, e.pa, check));
    emit_const(&e, 4, N88_UART0_PA + 0x20u);
    emit(&e, mov_imm(5, 'F')); emit(&e, str_off(5, 4, 0));
    emit(&e, B_SELF);

    /* Sections, full access: VA 0 and VA 0x40000000 -> PA 0x40000000, and
     * the three device megabytes at their own addresses. */
    const uint32_t tt = N88_DRAM_BASE + 0x4000u;
    static const uint32_t map[][2] = {
        { 0u, N88_DRAM_BASE }, { N88_DRAM_BASE, N88_DRAM_BASE },
        { N88_UART0_PA, N88_UART0_PA }, { N88_TIMER_PA, N88_TIMER_PA },
        { N88_VIC_PA, N88_VIC_PA },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        put32(ram_at(m, tt + (map[i][0] >> 20) * 4u), (map[i][1] & 0xfff00000u) | 0xc02u);
    m->cpu.cp15.ttbr0 = tt;
    m->cpu.cp15.ttbcr = 0;
    m->cpu.cp15.dacr = 1u;                      /* domain 0: client */
    m->cpu.cp15.sctlr |= ARM_SCTLR_M;
    m->cpu.cpsr = ARM_MODE_SVC | ARM_CPSR_I | ARM_CPSR_F | ARM_CPSR_A;
    m->cpu.r[15] = N88_DRAM_BASE + 0x1000u;
}

typedef struct { unsigned retired; uint64_t cycles; uint32_t count, r6_at_fiq; char out[4]; size_t outn; } fiq_result_t;

static fiq_result_t run_fiq(bool engine, bool wait) {
    fiq_result_t r = {0};
    n88_t *m = malloc(sizeof *m);
    CHECK(m && n88_init(m, engine), "init");
    if (!m || !m->ram) { free(m); return r; }
    load_fiq_program(m, wait);
    arm_status_t st = ARM_HALT;
    const unsigned budget = wait ? 5000u : 60000u;
    r.retired = n88_run(m, budget, &st);
    CHECK(st == ARM_OK && r.retired == budget, "ran the whole budget (status %d, %u)",
          (int)st, r.retired);
    r.cycles = m->cpu.cycles;
    r.count = get32(ram_at(m, N88_DRAM_BASE + 0x2000u));
    r.r6_at_fiq = get32(ram_at(m, N88_DRAM_BASE + 0x2004u));
    r.outn = n88_console_take(m, r.out, sizeof r.out);
    CHECK(m->timer.fired == 1 && !m->timer.pending, "one expiry, acknowledged");
    if (wait) CHECK(m->wfi == 1, "one WFI");
    n88_free(m);
    free(m);
    return r;
}

static void test_timer_fiq_program(void) {
    for (int engine = 0; engine < 2; engine++) {
        const fiq_result_t w = run_fiq(engine, true);
        CHECK(w.count == 1 && w.outn == 1 && w.out[0] == 'F',
              "engine %d: the FIQ ran once and the program went on (%u, %zu)",
              engine, w.count, w.outn);
        CHECK(w.cycles >= (uint64_t)TICKS * N88_CYCLES_PER_TICK,
              "engine %d: WFI moved time to the expiry (%llu cycles)", engine,
              (unsigned long long)w.cycles);
    }
    const fiq_result_t a = run_fiq(false, false), b = run_fiq(true, false);
    CHECK(a.count == 1 && b.count == 1 && a.outn == 1 && b.outn == 1,
          "busy loop: the FIQ ran once on each engine");
    CHECK(a.r6_at_fiq > 100u, "the loop ran a while before the FIQ (%u)", a.r6_at_fiq);
    CHECK(a.r6_at_fiq == b.r6_at_fiq && a.cycles == b.cycles,
          "the FIQ lands on the same instruction on both engines (r6 %u vs %u)",
          a.r6_at_fiq, b.r6_at_fiq);
}

/* ---------------------------------------------------------------- NVRAM */

static unsigned chrp_sum(const uint8_t *h) {
    unsigned s = h[0];
    for (int i = 2; i < 16; i++) s += h[i];
    while (s >> 8) s = (s & 0xffu) + (s >> 8);
    return s;
}

static void test_nvram_image(void) {
    static uint8_t img[0x2000];
    memset(img, 0xa5, sizeof img);
    n88_nvram_image(img, sizeof img);
    CHECK(img[0] == 0x70 && img[2] == 0x80 && img[3] == 0 &&
          memcmp(img + 4, "common\0\0\0\0\0\0", 12) == 0, "the common partition header");
    CHECK(img[1] == chrp_sum(img), "its checksum (%02x vs %02x)", img[1], chrp_sum(img));
    const uint8_t *f = img + 0x800;
    CHECK(f[0] == 0x7f && (f[2] | f[3] << 8) == 0x180 && memcmp(f + 4, "wwwwwwwwwwww", 12) == 0,
          "the free-space partition covers the rest");
    CHECK(f[1] == chrp_sum(f), "its checksum");
    bool zero = true;
    for (size_t i = 16; i < 0x800; i++) zero &= img[i] == 0;
    for (size_t i = 0x810; i < sizeof img; i++) zero &= img[i] == 0;
    CHECK(zero, "everything else is zero");
    /* Walking it the way IODTNVRAM does ends exactly at the end. */
    uint32_t off = 0;
    unsigned parts = 0;
    while (off < sizeof img && parts < 8) {
        const uint32_t blocks = (uint32_t)img[off + 2] | (uint32_t)img[off + 3] << 8;
        if (!blocks) break;
        off += blocks * 16u;
        parts++;
    }
    CHECK(off == sizeof img && parts == 2, "the partition walk ends at the end");
}

/* ---------------------------------------------------- synthetic inputs */

typedef struct { uint8_t b[16384]; size_t n; } buf_t;

static void b_u32(buf_t *b, uint32_t v) { put32(b->b + b->n, v); b->n += 4; }
static void b_prop(buf_t *b, const char *name, const void *val, uint32_t len) {
    memset(b->b + b->n, 0, 32);
    memcpy(b->b + b->n, name, strlen(name));
    b->n += 32;
    b_u32(b, len);
    memset(b->b + b->n, 0, (len + 3u) & ~3u);
    if (val) memcpy(b->b + b->n, val, len);
    b->n += (len + 3u) & ~3u;
}
static void b_str(buf_t *b, const char *name, const char *s) {
    b_prop(b, name, s, (uint32_t)strlen(s) + 1u);
}
static void b_node(buf_t *b, uint32_t props, uint32_t children) { b_u32(b, props); b_u32(b, children); }

typedef struct { const char *compat; uint32_t compat_len; bool with_pram; } tree_opts_t;

/* The nodes bring-up touches, with the template's zeros. */
static void build_tree(buf_t *t, tree_opts_t o) {
    static const uint8_t zero8[8];
    static const uint8_t zero4[4];
    t->n = 0;
    b_node(t, 4, o.with_pram ? 5 : 4);
    b_str(t, "name", "device-tree");
    b_str(t, "secure-root-prefix", "md");
    b_prop(t, "compatible", o.compat, o.compat_len);
    b_prop(t, "clock-frequency", zero4, 4);
      b_node(t, 2, 0); b_str(t, "name", "memory"); b_prop(t, "reg", zero8, 8);
      if (o.with_pram) { b_node(t, 2, 0); b_str(t, "name", "pram"); b_prop(t, "reg", zero8, 8); }
      b_node(t, 1, 1); b_str(t, "name", "cpus");
        b_node(t, 7, 0); b_str(t, "name", "cpu0");
        b_prop(t, "timebase-frequency", zero4, 4);
        b_prop(t, "clock-frequency", zero4, 4);
        b_prop(t, "bus-frequency", zero4, 4);
        b_prop(t, "memory-frequency", zero4, 4);
        b_prop(t, "peripheral-frequency", zero4, 4);
        b_prop(t, "fixed-frequency", zero4, 4);
      b_node(t, 2, 1); b_str(t, "name", "chosen"); b_prop(t, "nvram-proxy-data", NULL, 0x2000);
        b_node(t, 5, 0); b_str(t, "name", "memory-map");
        b_prop(t, "MemoryMapReserved-0", zero8, 8);
        b_prop(t, "InUse", "\x01\x00\x00\x40\x00\x10\x00\x00", 8);     /* taken */
        b_prop(t, "MemoryMapReserved-1", zero8, 8);
        b_prop(t, "MemoryMapReserved-2", zero8, 8);
      b_node(t, 1, 1); b_str(t, "name", "arm-io");
        b_node(t, 2, 0); b_str(t, "name", "iop"); b_str(t, "compatible", "iop-s5l8920x");
}

static const char N88_COMPAT[] = "N88AP\0iPhone2,1\0AppleARM";

#define KVA_TEXT  UINT32_C(0x80001000)
/* A kernel: one __TEXT segment at KVA_TEXT holding `code`, entry at its start. */
static size_t build_kernel(uint8_t *img, size_t cap, uint32_t vmaddr, const uint32_t *code,
                           unsigned ncode) {
    const uint32_t seg = 56u, thr = 16u + 17u * 4u, cmds = seg + thr;
    const uint32_t fileoff = 0x100u, filesize = ncode * 4u;
    if (cap < fileoff + filesize) return 0;
    memset(img, 0, cap);
    put32(img + 0, MH_MAGIC_32); put32(img + 4, MH_CPU_TYPE_ARM);
    put32(img + 8, 9); put32(img + 12, MH_EXECUTE);
    put32(img + 16, 2); put32(img + 20, cmds);
    uint8_t *c = img + 28;
    put32(c + 0, LC_SEGMENT); put32(c + 4, seg);
    memcpy(c + 8, "__TEXT", 6);
    put32(c + 24, vmaddr); put32(c + 28, 0x2000u);
    put32(c + 32, fileoff); put32(c + 36, filesize);
    c += seg;
    put32(c + 0, LC_UNIXTHREAD); put32(c + 4, thr);
    put32(c + 8, 1); put32(c + 12, 17);
    put32(c + 16 + 15 * 4, vmaddr);             /* pc */
    for (unsigned i = 0; i < ncode; i++) put32(img + fileoff + 4u * i, code[i]);
    return fileoff + filesize;
}

/* A property of the RAM copy of the tree, by walking it again. */
static const uint8_t *tree_prop(const uint8_t *tree, size_t len, const char *path,
                                const char *prop, uint32_t *plen);

static size_t skip_node(const uint8_t *b, size_t off) {
    uint32_t np = get32(b + off), nc = get32(b + off + 4);
    off += 8;
    for (uint32_t i = 0; i < np; i++) off += 36u + ((get32(b + off + 32) + 3u) & ~3u);
    for (uint32_t i = 0; i < nc; i++) off = skip_node(b, off);
    return off;
}
static const uint8_t *node_prop(const uint8_t *b, size_t off, const char *prop, uint32_t *plen) {
    uint32_t np = get32(b + off);
    off += 8;
    for (uint32_t i = 0; i < np; i++) {
        uint32_t l = get32(b + off + 32) & 0x7fffffffu;
        if (strncmp((const char *)b + off, prop, 32) == 0) { if (plen) *plen = l; return b + off + 36; }
        off += 36u + ((l + 3u) & ~3u);
    }
    return NULL;
}
static const uint8_t *tree_prop(const uint8_t *b, size_t len, const char *path,
                                const char *prop, uint32_t *plen) {
    (void)len;
    size_t off = 0;
    while (*path) {
        const char *slash = strchr(path, '/');
        size_t n = slash ? (size_t)(slash - path) : strlen(path);
        uint32_t np = get32(b + off), nc = get32(b + off + 4);
        size_t child = off + 8;
        for (uint32_t i = 0; i < np; i++) child += 36u + ((get32(b + child + 32) + 3u) & ~3u);
        size_t found = 0;
        for (uint32_t i = 0; i < nc; i++) {
            const uint8_t *nm = node_prop(b, child, "name", NULL);
            if (nm && memcmp(nm, path, n) == 0 && nm[n] == 0) { found = child; break; }
            child = skip_node(b, child);
        }
        if (!found) return NULL;
        off = found;
        path = slash ? slash + 1 : path + n;
    }
    return node_prop(b, off, prop, plen);
}

/* ---------------------------------------------------------- bring-up */

static void test_boot_layout_and_tree(void) {
    static buf_t tree;
    static uint8_t kernel[0x400];
    build_tree(&tree, (tree_opts_t){ N88_COMPAT, sizeof N88_COMPAT, true });
    /* The "kernel": print "OK\n" on UART0 with the MMU off, then stop. */
    const uint32_t code[] = {
        movw(4, (N88_UART0_PA + 0x20u) & 0xffffu), movt(4, (N88_UART0_PA + 0x20u) >> 16),
        mov_imm(5, 'O'), str_off(5, 4, 0),
        mov_imm(5, 'K'), str_off(5, 4, 0),
        mov_imm(5, '\n'), str_off(5, 4, 0),
        B_SELF,
    };
    const size_t klen = build_kernel(kernel, sizeof kernel, KVA_TEXT, code,
                                     (unsigned)(sizeof code / sizeof code[0]));

    for (int engine = 0; engine < 2; engine++) {
        n88_t *m = malloc(sizeof *m);
        CHECK(m && n88_init(m, engine), "init");
        if (!m || !m->ram) { free(m); return; }
        char detail[160];
        const n88_boot_t req = { .kernel = kernel, .kernel_size = klen,
                                 .devicetree = tree.b, .devicetree_size = tree.n };
        const n88_status_t st = n88_boot(m, &req, detail, sizeof detail);
        CHECK(st == N88_OK, "boot: %s (%s)", n88_strerror(st), detail);
        if (st != N88_OK) { n88_free(m); free(m); return; }

        /* The kernel ends at 0x80003000 -> 0x40003000; the tree follows. */
        const uint32_t tree_pa = 0x40003000u;
        const uint32_t args_pa = (tree_pa + (uint32_t)tree.n + 0xfffu) & ~0xfffu;
        const uint32_t tokd_pa = (args_pa + 0x1000u + 0x3fffu) & ~0x3fffu;
        CHECK(m->devicetree_pa == tree_pa && m->boot_args_pa == args_pa &&
              m->tokd_pa == tokd_pa && m->entry_pa == 0x40001000u,
              "layout: tree %08x args %08x tokd %08x entry %08x", m->devicetree_pa,
              m->boot_args_pa, m->tokd_pa, m->entry_pa);
        CHECK(get32(ram_at(m, 0x40001000u)) == code[0], "the segment is at its physical address");
        CHECK(m->cpu.r[15] == 0x40001000u && m->cpu.r[0] == args_pa &&
              m->cpu.cpsr == (ARM_MODE_SVC | ARM_CPSR_I | ARM_CPSR_F | ARM_CPSR_A) &&
              !(m->cpu.cp15.sctlr & ARM_SCTLR_M), "the CPU as iBoot leaves it");

        const uint8_t *ba = ram_at(m, args_pa);
        CHECK(ba[0] == 1 && ba[1] == 0 && ba[2] == 5 && ba[3] == 0, "boot_args revision 1, version 5");
        CHECK(get32(ba + 0x04) == N88_VIRT_BASE && get32(ba + 0x08) == N88_DRAM_BASE &&
              get32(ba + 0x0c) == N88_DRAM_SIZE - N88_TOP_RESERVE && get32(ba + 0x10) == tokd_pa,
              "boot_args bases, memSize below the boot-owned top, topOfKernelData");
        CHECK(get32(ba + 0x30) == tree_pa - N88_DRAM_BASE + N88_VIRT_BASE &&
              get32(ba + 0x34) == tree.n, "boot_args device tree VA and size");
        CHECK(strcmp((const char *)ba + 0x38, N88_DEFAULT_CMDLINE) == 0, "the default boot-args");
        bool video_zero = true;
        for (unsigned i = 0x14; i < 0x30; i++) video_zero &= ba[i] == 0;
        CHECK(video_zero, "no framebuffer in Boot_Video");

        const uint8_t *dt = ram_at(m, tree_pa);
        uint32_t l = 0;
        const uint8_t *p = tree_prop(dt, tree.n, "memory", "reg", &l);
        CHECK(p && get32(p) == N88_DRAM_BASE && get32(p + 4) == N88_DRAM_SIZE, "/memory:reg");
        p = tree_prop(dt, tree.n, "pram", "reg", &l);
        CHECK(p && get32(p) == N88_DRAM_BASE + N88_DRAM_SIZE - N88_TOP_RESERVE &&
              get32(p + 4) == N88_PRAM_SIZE, "/pram:reg at the top of DRAM");
        p = tree_prop(dt, tree.n, "", "clock-frequency", &l);
        CHECK(p && get32(p) == N88_BUS_HZ, "root clock-frequency");
        static const struct { const char *prop; uint32_t v; } cpu0[] = {
            { "timebase-frequency", N88_TB_HZ }, { "clock-frequency", N88_CPU_HZ },
            { "bus-frequency", N88_BUS_HZ }, { "memory-frequency", N88_MEM_HZ },
            { "peripheral-frequency", N88_PRF_HZ }, { "fixed-frequency", N88_FIX_HZ },
        };
        for (size_t i = 0; i < sizeof cpu0 / sizeof cpu0[0]; i++) {
            p = tree_prop(dt, tree.n, "cpus/cpu0", cpu0[i].prop, &l);
            CHECK(p && get32(p) == cpu0[i].v, "cpu0 %s", cpu0[i].prop);
        }
        p = tree_prop(dt, tree.n, "chosen", "nvram-proxy-data", &l);
        CHECK(p && l == 0x2000 && p[0] == 0x70 && p[0x800] == 0x7f, "the NVRAM image");
        p = tree_prop(dt, tree.n, "arm-io/iop", "compatible", &l);
        CHECK(p && memcmp(p, "xop-s5l8920x", 12) == 0, "the IOP un-matched by default");
        p = tree_prop(dt, tree.n, "chosen/memory-map", "DeviceTree", &l);
        CHECK(p && l == 8 && get32(p) == tree_pa && get32(p + 4) == tree.n,
              "memory-map DeviceTree took the first placeholder");
        p = tree_prop(dt, tree.n, "chosen/memory-map", "BootArgs", &l);
        CHECK(p && get32(p) == args_pa && get32(p + 4) == 0x1000u,
              "memory-map BootArgs took the next free one");
        p = tree_prop(dt, tree.n, "chosen/memory-map", "InUse", &l);
        CHECK(p && get32(p) == 0x40000001u, "an entry in use is left alone");
        CHECK(!tree_prop(dt, tree.n, "chosen/memory-map", "RAMDisk", &l),
              "no RAMDisk entry without a root filesystem");
        CHECK(!m->has_root && !m->bus.privileged_svc_handler, "and no bridge");
        CHECK(tree_prop(dt, tree.n, "", "secure-root-prefix", &l) != NULL,
              "secure-root-prefix is left alone without a root");

        /* And it runs. */
        arm_status_t rs = ARM_HALT;
        CHECK(n88_run(m, 100, &rs) == 100 && rs == ARM_OK, "engine %d: runs", engine);
        char out[8] = {0};
        CHECK(n88_console_take(m, out, sizeof out) == 3 && memcmp(out, "OK\n", 3) == 0,
              "engine %d: the kernel's UART output", engine);

        /* A second boot starts from clean DRAM. */
        put32(ram_at(m, 0x48000000u), 0x5a5a5a5au);
        const n88_boot_t req2 = { .kernel = kernel, .kernel_size = klen,
                                  .devicetree = tree.b, .devicetree_size = tree.n,
                                  .cmdline = "-v",
                                  .unmatch = (const char *const[]){ "" },
                                  .unmatch_count = 0 };
        CHECK(n88_boot(m, &req2, NULL, 0) == N88_OK, "second boot");
        CHECK(get32(ram_at(m, 0x48000000u)) == 0u, "DRAM cleared for a second boot");
        CHECK(strcmp((const char *)ram_at(m, m->boot_args_pa) + 0x38, "-v") == 0, "given boot-args");
        p = tree_prop(ram_at(m, m->devicetree_pa), tree.n, "arm-io/iop", "compatible", &l);
        CHECK(p && p[0] == 'i', "an empty un-match list leaves the IOP");
        n88_free(m);
        free(m);
    }
}

static void test_boot_refusals(void) {
    static buf_t tree, nopram, n88tree;
    static uint8_t kernel[0x400];
    build_tree(&tree, (tree_opts_t){ N88_COMPAT, sizeof N88_COMPAT, true });
    build_tree(&nopram, (tree_opts_t){ N88_COMPAT, sizeof N88_COMPAT, false });
    const uint32_t code[] = { B_SELF };
    const size_t klen = build_kernel(kernel, sizeof kernel, KVA_TEXT, code, 1);
    (void)n88tree;

    n88_t *m = malloc(sizeof *m);
    CHECK(m && n88_init(m, false), "init");
    if (!m || !m->ram) { free(m); return; }
    char d[160];
    n88_boot_t r = { .kernel = kernel, .kernel_size = klen,
                     .devicetree = tree.b, .devicetree_size = tree.n };

    r.kernel = NULL;
    CHECK(n88_boot(m, &r, d, sizeof d) == N88_ERR_ARGUMENT, "no kernel");
    r.kernel = kernel;
    char longline[300];
    memset(longline, 'a', sizeof longline - 1);
    longline[sizeof longline - 1] = 0;
    r.cmdline = longline;
    CHECK(n88_boot(m, &r, d, sizeof d) == N88_ERR_ARGUMENT, "boot-args over 255 bytes");
    r.cmdline = NULL;

    uint8_t junk[64] = {1, 2, 3};
    r.kernel = junk; r.kernel_size = sizeof junk;
    CHECK(n88_boot(m, &r, d, sizeof d) == N88_ERR_KERNEL && d[0], "not a Mach-O: %s", d);
    static uint8_t low[0x400];
    const size_t lowlen = build_kernel(low, sizeof low, 0x00001000u, code, 1);
    r.kernel = low; r.kernel_size = lowlen;
    CHECK(n88_boot(m, &r, d, sizeof d) == N88_ERR_KERNEL, "linked below 0x80000000");
    static uint8_t high[0x400];
    const size_t highlen = build_kernel(high, sizeof high, 0x8ff00000u, code, 1);
    r.kernel = high; r.kernel_size = highlen;
    CHECK(n88_boot(m, &r, d, sizeof d) == N88_ERR_LAYOUT, "no room above the kernel: %s", d);
    r.kernel = kernel; r.kernel_size = klen;

    r.devicetree_size = tree.n - 4;
    CHECK(n88_boot(m, &r, d, sizeof d) == N88_ERR_DEVICETREE, "a truncated tree: %s", d);
    r.devicetree = nopram.b; r.devicetree_size = nopram.n;
    CHECK(n88_boot(m, &r, d, sizeof d) == N88_ERR_DEVICETREE && strstr(d, "pram"),
          "no /pram: %s", d);
    r.devicetree = tree.b; r.devicetree_size = tree.n;
    const char *const bogus[] = { "arm-io/nothing" };
    r.unmatch = bogus; r.unmatch_count = 1;
    CHECK(n88_boot(m, &r, d, sizeof d) == N88_ERR_DEVICETREE && strstr(d, "nothing"),
          "un-matching a missing node: %s", d);
    r.unmatch = NULL; r.unmatch_count = 0;
    CHECK(!m->booted, "no failed boot reports booted");
    CHECK(n88_boot(m, &r, d, sizeof d) == N88_OK && m->booted, "and the good one still boots");
    CHECK(strcmp(n88_strerror(N88_ERR_LAYOUT), "unknown error") != 0, "strerror");
    n88_free(m);
    free(m);
}

/* -------------------------------------------------------- root disk */

typedef struct { uint8_t data[16384]; } memdisk_t;
static vm_block_io_status_t md_read(void *ctx, uint64_t off, void *dst, size_t n, size_t *got) {
    memdisk_t *d = ctx;
    memcpy(dst, d->data + off, n);
    *got = n;
    return VM_BLOCK_IO_OK;
}
static vm_block_io_status_t md_write(void *ctx, uint64_t off, const void *src, size_t n,
                                     size_t *got) {
    memdisk_t *d = ctx;
    memcpy(d->data + off, src, n);
    *got = n;
    return VM_BLOCK_IO_OK;
}

/* Thumb-2 MOVW/MOVT as two halfwords. */
static void t_mov16(uint16_t *h, unsigned *n, bool top, unsigned rd, uint32_t imm) {
    h[(*n)++] = (uint16_t)((top ? 0xf2c0u : 0xf240u) | ((imm >> 1) & 0x0400u) | (imm >> 12));
    h[(*n)++] = (uint16_t)(((imm << 4) & 0x7000u) | (rd << 8) | (imm & 0xffu));
}
static void t_const(uint16_t *h, unsigned *n, unsigned rd, uint32_t v) {
    t_mov16(h, n, false, rd, v & 0xffffu);
    t_mov16(h, n, true, rd, v >> 16);
}

/*
 * A Thumb "kernel" that does what the patched strategy routine does: the
 * length on the stack, 64-bit source and destination in r1:r0 and r3:r2,
 * then the bridge's SVC in place of bcopy_phys. It reads disk offset 0x1000
 * into RAM, then writes that RAM back to disk offset 0x2000.
 */
static void test_boot_with_root(void) {
    static buf_t tree;
    static uint8_t kernel[0x400];
    static memdisk_t disk;
    build_tree(&tree, (tree_opts_t){ N88_COMPAT, sizeof N88_COMPAT, true });
    for (size_t i = 0; i < sizeof disk.data; i++) disk.data[i] = (uint8_t)(i * 7u + 3u);
    const vm_block_t block = { &disk, sizeof disk.data, 0, 0, md_read, md_write, NULL };

    uint16_t h[64];
    unsigned n = 0, read_site, write_site;
    t_const(h, &n, 0, 0x40200000u);
    h[n++] = 0x4685;                            /* mov sp, r0 */
    t_mov16(h, &n, false, 4, 256);
    h[n++] = 0x9400;                            /* str r4, [sp] */
    t_const(h, &n, 0, N88_MD_TOKEN_PA + 0x1000u);
    h[n++] = 0x2100;                            /* movs r1, #0 */
    t_const(h, &n, 2, 0x40100000u);
    h[n++] = 0x2300;                            /* movs r3, #0 */
    read_site = n;
    h[n++] = (uint16_t)N88_SVC_MD_READ;
    h[n++] = 0xbf00;
    t_const(h, &n, 0, 0x40100000u);
    h[n++] = 0x2100;
    t_const(h, &n, 2, N88_MD_TOKEN_PA + 0x2000u);
    h[n++] = 0x2300;
    write_site = n;
    h[n++] = (uint16_t)N88_SVC_MD_WRITE;
    h[n++] = 0xbf00;
    h[n++] = 0xe7fe;                            /* b . */
    if (n & 1u) h[n++] = 0xbf00;
    uint32_t words[32];
    for (unsigned i = 0; i < n / 2u; i++)
        words[i] = (uint32_t)h[2u * i] | (uint32_t)h[2u * i + 1u] << 16;
    const size_t klen = build_kernel(kernel, sizeof kernel, KVA_TEXT, words, n / 2u);
    put32(kernel + 28 + 56 + 16 + 15 * 4, KVA_TEXT | 1u);   /* a Thumb entry */

    /* MMU off, so the sites are the physical pcs the SVCs execute at. */
    const uint32_t code_pa = KVA_TEXT - N88_VIRT_BASE + N88_DRAM_BASE;
    for (int engine = 0; engine < 2; engine++) {
        n88_t *m = malloc(sizeof *m);
        CHECK(m && n88_init(m, engine), "init");
        if (!m || !m->ram) { free(m); return; }
        char d[160];
        n88_boot_t r = { .kernel = kernel, .kernel_size = klen,
                         .devicetree = tree.b, .devicetree_size = tree.n,
                         .root = &block };
        CHECK(n88_boot(m, &r, d, sizeof d) == N88_ERR_ROOT, "a root without its sites: %s", d);
        r.md_read_site_pc = code_pa + 2u * read_site;
        r.md_write_site_pc = code_pa + 2u * write_site;
        vm_block_t odd = block;
        odd.size = 10000u;
        r.root = &odd;
        CHECK(n88_boot(m, &r, d, sizeof d) == N88_ERR_ROOT, "part of a page: %s", d);
        r.root = &block;
        const n88_status_t st = n88_boot(m, &r, d, sizeof d);
        CHECK(st == N88_OK, "boot with a root: %s (%s)", n88_strerror(st), d);
        if (st != N88_OK) { n88_free(m); free(m); return; }
        CHECK(m->has_root && m->root_size == sizeof disk.data, "attached");
        CHECK(strcmp((const char *)ram_at(m, m->boot_args_pa) + 0x38, N88_ROOT_CMDLINE) == 0,
              "rd=md0 by default");
        uint32_t l = 0;
        const uint8_t *p = tree_prop(ram_at(m, m->devicetree_pa), tree.n,
                                     "chosen/memory-map", "RAMDisk", &l);
        CHECK(p && l == 8 && get32(p) == N88_MD_TOKEN_PA && get32(p + 4) == sizeof disk.data,
              "the RAMDisk entry names the token and the size");
        CHECK(!tree_prop(ram_at(m, m->devicetree_pa), tree.n, "", "secure-root-prefix", &l) &&
              tree_prop(ram_at(m, m->devicetree_pa), tree.n, "", "xecure-root-prefix", &l),
              "secure-root-prefix is struck out with a root");

        arm_status_t rs = ARM_HALT;
        n88_run(m, 200, &rs);
        CHECK(rs == ARM_OK, "engine %d: ran (status %d, pc %08x)", engine, (int)rs, m->cpu.r[15]);
        CHECK(memcmp(ram_at(m, 0x40100000u), disk.data + 0x1000, 256) == 0,
              "engine %d: the read landed in RAM", engine);
        CHECK(memcmp(disk.data + 0x2000, disk.data + 0x1000, 256) == 0,
              "engine %d: the write reached the disk", engine);
        CHECK(m->md.stats.successful_reads == 1 && m->md.stats.successful_writes == 1 &&
              m->md.stats.failures == 0, "engine %d: one of each, no failures", engine);
        for (size_t i = 0x2000; i < 0x2100; i++) disk.data[i] = (uint8_t)(i * 7u + 3u);
        n88_free(m);
        free(m);
    }
}

/* ------------------------------------------------------------ console */

static void test_console_ring(void) {
    n88_t *m = malloc(sizeof *m);
    CHECK(m && n88_init(m, false), "init");
    if (!m || !m->ram) { free(m); return; }
    const unsigned extra = 100;
    for (unsigned i = 0; i < N88_CONSOLE_CAPACITY + extra; i++)
        n88_write32(m, N88_UART0_PA + 0x20u, 'a' + i % 26u);
    CHECK(m->console_total == N88_CONSOLE_CAPACITY + extra &&
          m->console_dropped == extra && m->console_len == N88_CONSOLE_CAPACITY,
          "overflow drops the oldest");
    char *out = malloc(N88_CONSOLE_CAPACITY);
    size_t got = n88_console_take(m, out, 10);
    CHECK(got == 10 && out[0] == (char)('a' + extra % 26u), "oldest surviving byte first");
    got = n88_console_take(m, out, N88_CONSOLE_CAPACITY);
    CHECK(got == N88_CONSOLE_CAPACITY - 10u &&
          out[got - 1] == (char)('a' + (N88_CONSOLE_CAPACITY + extra - 1u) % 26u),
          "the rest, newest last");
    CHECK(n88_console_take(m, out, 10) == 0, "taken is gone");
    free(out);
    n88_free(m);
    free(m);
}

/* -------------------------------------------------------- identity */

static void test_devicetree_identity(void) {
    static buf_t t;
    build_tree(&t, (tree_opts_t){ N88_COMPAT, sizeof N88_COMPAT, true });
    CHECK(n88_devicetree_is_3gs(t.b, t.n), "N88AP first");
    static const char later[] = "iPhone2,1\0N88AP\0AppleARM";
    build_tree(&t, (tree_opts_t){ later, sizeof later, true });
    CHECK(n88_devicetree_is_3gs(t.b, t.n), "N88AP anywhere in the list");
    static const char m68[] = "M68AP\0iPhone1,1\0AppleARM";
    build_tree(&t, (tree_opts_t){ m68, sizeof m68, true });
    CHECK(!n88_devicetree_is_3gs(t.b, t.n), "an iPhone (original) tree is not a 3GS");
    static const char prefix[] = "N88APX\0N88A";
    build_tree(&t, (tree_opts_t){ prefix, sizeof prefix - 1, true });
    CHECK(!n88_devicetree_is_3gs(t.b, t.n), "only a whole string matches");
    uint8_t junk[16] = {0xff, 0xff, 0xff, 0xff};
    CHECK(!n88_devicetree_is_3gs(junk, sizeof junk), "garbage");
    CHECK(!n88_devicetree_is_3gs(NULL, 0), "nothing");
}

int main(void) {
    test_bus_routing();
    test_timer_registers();
    test_timer_fiq_program();
    test_nvram_image();
    test_boot_layout_and_tree();
    test_boot_refusals();
    test_boot_with_root();
    test_console_ring();
    test_devicetree_identity();
    printf("n88: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
