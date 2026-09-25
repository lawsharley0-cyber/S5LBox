/*
 * boot3gs — start the iPhone 3GS (N88AP, S5L8920) iOS 6 kernel on a bare
 * machine and record what it touches.
 *
 * A research harness, not the product and not a machine model. It exists to
 * answer one question empirically: which devices does the iOS 6 kernel need,
 * in what order, before it prints its first console line? The iPhone OS 3
 * machine was brought up the same way (docs/BOOTLOG.md): run the real kernel,
 * see what it asks for, model that, repeat.
 *
 * What it does, as iBoot would:
 *   - maps the kernelcache's segments at physical = vmaddr - 0x80000000 +
 *     0x40000000 (DRAM is 256 MB at 0x40000000; the kernel is linked at
 *     0x80000000; /arm-io maps devices at physical 0x80000000 and up);
 *   - copies the device tree after the kernel and fills in what iBoot fills
 *     in: /memory, the clock frequencies, and /chosen/memory-map entries for
 *     the device tree and boot_args;
 *   - builds boot_args (the layout core/src/boot/bringup.c documents) after
 *     the tree, and topOfKernelData after that, 16 KiB aligned because the
 *     kernel builds its first-level translation table there;
 *   - starts a Cortex-A8 core (ARM_ARCH_V7_A8) in SVC mode, MMU off, at the
 *     entry point's physical address, with r0 = boot_args.
 *
 * Then it single-steps the reference interpreter and reports:
 *   - every device-register access (address, size, value, pc, symbol), the
 *     first 400 in order and then a count per register;
 *   - every exception taken (vector, faulting pc, DFSR/DFAR or IFSR/IFAR);
 *   - bytes written to UART0's transmit register (Samsung UART, UTXH at
 *     0x20), as text;
 *   - why it stopped: an instruction the core refused, the instruction
 *     budget, or a tight loop.
 * Devices answer zero, except UART0's status register, which reports an
 * empty transmitter so a polled console write can finish.
 *
 * Usage:
 *   boot3gs <kernelcache.macho> <devicetree.bin> [-n instructions]
 *           [-c "boot-args"] [-v] [-m dram.bin] [-u node/path]...
 * -v logs every device access instead of the first 400; -m saves all of DRAM
 * at the end (the kernel's message buffer is in there); -u un-matches a
 * device-tree node (e.g. "arm-io/iop") so no driver claims it.
 * The kernelcache must be decrypted and decompressed (a plain Mach-O), and
 * the device tree decrypted (the flat tree inside the IMG3). Both come from
 * the user's own IPSW; nothing Apple-owned is in this repository.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "arm.h"
#include "ksyms.h"
#include "macho.h"
#include "soc.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DRAM_BASE   UINT32_C(0x40000000)
#define DRAM_SIZE   UINT32_C(0x10000000)    /* 256 MB */
#define VIRT_BASE   UINT32_C(0x80000000)
#define IO_BASE     UINT32_C(0x80000000)    /* /arm-io ranges: child 0 */
#define UART0_PA    UINT32_C(0x82500000)    /* /arm-io/uart0 reg 0x02500000 */
/*
 * The top of DRAM is boot-owned, as iBoot leaves it: boot_args.memSize stops
 * below it, so the kernel never allocates it. It holds /pram, the persistent
 * buffer the kernel keeps its panic log in: the platform expert maps
 * /pram:reg and refuses anything under 16 KB (0x8027baac), and with the
 * template's {0, 0} it maps nothing and faults on the first read. The size is
 * PROVISIONAL (the 3GS's is not known here); 64 KB clears the kernel's floor.
 */
#define TOP_RESERVE UINT32_C(0x00100000)
#define PRAM_PA     (DRAM_BASE + DRAM_SIZE - TOP_RESERVE)
#define PRAM_SIZE   UINT32_C(0x00010000)

/*
 * Clock frequencies the tree carries as zero and iBoot fills in. CPU is the
 * iPhone 3GS's documented 600 MHz. The rest are PROVISIONAL: the timebase is
 * whatever the timer model will count at, so it only has to be consistent
 * with that model, and the bus/memory/peripheral values only have to be
 * non-zero and in sane ratios (see bringup.c's note on the same values for
 * the S5L8900). Replace them if an iPhone2,1 IORegistry dump turns up.
 */
#define CPU_HZ   600000000u
#define BUS_HZ   100000000u
#define MEM_HZ   200000000u
#define PRF_HZ    50000000u
#define FIX_HZ    24000000u
#define TB_HZ     24000000u

static uint8_t *g_ram;
static ksyms_t  g_syms;
static bool     g_have_syms;
static arm_cpu_t g_cpu;
static bool     g_verbose;

/* ------------------------------------------------------------------ utils */

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "boot3gs: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot open %s", path);
    if (fseek(f, 0, SEEK_END) != 0) die("cannot seek %s", path);
    long n = ftell(f);
    if (n <= 0) die("%s is empty", path);
    rewind(f);
    uint8_t *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) die("cannot read %s", path);
    fclose(f);
    *len = (size_t)n;
    return b;
}

static const char *sym(uint32_t addr) {
    static char buf[4][160];
    static unsigned k;
    char *b = buf[k++ & 3u];
    if (!g_have_syms) { snprintf(b, 160, "?"); return b; }
    return ksyms_resolve(&g_syms, addr, b, 160);
}

static uint32_t ld32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}
static void st32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* -------------------------------------------------------- the flat tree */
/* Apple's flattened device tree: node = u32 nprops, u32 nchildren, props,
 * children; prop = char name[32], u32 length (bit 31 is a flag), value
 * padded to 4. */

static size_t dt_skip_node(const uint8_t *b, size_t len, size_t off);

static size_t dt_props_end(const uint8_t *b, size_t len, size_t off,
                           uint32_t *nchildren) {
    if (off + 8u > len) return 0;
    uint32_t np = ld32(b + off), nc = ld32(b + off + 4u);
    off += 8u;
    for (uint32_t i = 0; i < np; i++) {
        if (off + 36u > len) return 0;
        uint32_t l = ld32(b + off + 32u) & 0x7fffffffu;
        off += 36u + ((l + 3u) & ~3u);
        if (off > len) return 0;
    }
    if (nchildren) *nchildren = nc;
    return off;
}

static size_t dt_skip_node(const uint8_t *b, size_t len, size_t off) {
    uint32_t nc;
    off = dt_props_end(b, len, off, &nc);
    for (uint32_t i = 0; off && i < nc; i++) off = dt_skip_node(b, len, off);
    return off;
}

/* The value of property `name` of the node at `off`, or NULL. */
static uint8_t *dt_prop(uint8_t *b, size_t len, size_t off, const char *name,
                        uint32_t *plen) {
    if (off + 8u > len) return NULL;
    uint32_t np = ld32(b + off);
    off += 8u;
    for (uint32_t i = 0; i < np; i++) {
        if (off + 36u > len) return NULL;
        uint32_t l = ld32(b + off + 32u) & 0x7fffffffu;
        if (strncmp((const char *)b + off, name, 32) == 0) {
            if (plen) *plen = l;
            return b + off + 36u;
        }
        off += 36u + ((l + 3u) & ~3u);
    }
    return NULL;
}

/* A node by path below the root, "" being the root, "a/b" by names. */
static size_t dt_find(uint8_t *b, size_t len, const char *path) {
    size_t off = 0;
    while (*path) {
        const char *slash = strchr(path, '/');
        size_t n = slash ? (size_t)(slash - path) : strlen(path);
        uint32_t nc;
        size_t child = dt_props_end(b, len, off, &nc);
        size_t found = 0;
        for (uint32_t i = 0; child && i < nc; i++) {
            uint32_t l;
            const uint8_t *nm = dt_prop(b, len, child, "name", &l);
            if (nm && l > n && memcmp(nm, path, n) == 0 && nm[n] == 0) {
                found = child;
                break;
            }
            child = dt_skip_node(b, len, child);
        }
        if (!found) return (size_t)-1;
        off = found;
        path = slash ? slash + 1 : path + n;
    }
    return off;
}

static void dt_set(uint8_t *b, size_t len, const char *path, const char *prop,
                   const uint32_t *words, uint32_t count) {
    size_t node = dt_find(b, len, path);
    uint32_t l = 0;
    uint8_t *p = node == (size_t)-1 ? NULL : dt_prop(b, len, node, prop, &l);
    if (!p || l != 4u * count) die("device tree: /%s:%s is missing or not %u bytes",
                                   path, prop, 4u * count);
    for (uint32_t i = 0; i < count; i++) st32(p + 4u * i, words[i]);
}

/* Claim a MemoryMapReserved-* placeholder in /chosen/memory-map for `key`,
 * as iBoot does (bringup.c's dt_memmap_add explains the mechanism). */
static void dt_memmap(uint8_t *b, size_t len, const char *key, uint32_t pa,
                      uint32_t size) {
    size_t node = dt_find(b, len, "chosen/memory-map");
    if (node == (size_t)-1) die("device tree: no /chosen/memory-map");
    uint32_t np = ld32(b + node);
    size_t off = node + 8u;
    for (uint32_t i = 0; i < np; i++) {
        uint32_t l = ld32(b + off + 32u) & 0x7fffffffu;
        if (l == 8u && strncmp((char *)b + off, "MemoryMapReserved-", 18) == 0 &&
            ld32(b + off + 36u) == 0u && ld32(b + off + 40u) == 0u) {
            memset(b + off, 0, 32);
            memcpy(b + off, key, strlen(key));
            st32(b + off + 36u, pa);
            st32(b + off + 40u, size);
            return;
        }
        off += 36u + ((l + 3u) & ~3u);
    }
    die("device tree: no free memory-map placeholder for %s", key);
}

/*
 * /chosen/nvram-proxy-data: the NVRAM image iBoot hands the kernel. The
 * template's is 8 KB of zeros, and IODTNVRAM's partition walk (0x80267298)
 * adds each header's length (a u16 at +2, in 16-byte blocks, read
 * little-endian with ldrh) to its offset, so a zero header never advances
 * and the walk never ends. This writes the smallest image that parses: a
 * "common" partition (signature 0x70) holding no variables, and the rest of
 * the 8 KB as the free-space partition (0x7f, "wwwwwwwwwwww"), each header
 * with the CHRP checksum over its signature, length and name.
 */
static void nvram_partition(uint8_t *h, uint8_t sig, uint16_t blocks,
                            const char *name) {
    memset(h, 0, 16);
    h[0] = sig;
    h[2] = (uint8_t)blocks;
    h[3] = (uint8_t)(blocks >> 8);
    memcpy(h + 4, name, strlen(name) < 12 ? strlen(name) : 12);
    unsigned sum = h[0];
    for (int i = 2; i < 16; i++) {
        sum += h[i];
        if (sum > 0xffu) sum = (sum & 0xffu) + 1u;
    }
    h[1] = (uint8_t)sum;
}

static void nvram_image(uint8_t *img, uint32_t len) {
    const uint32_t common = 0x800u;
    memset(img, 0, len);
    nvram_partition(img, 0x70, (uint16_t)(common / 16u), "common");
    nvram_partition(img + common, 0x7f, (uint16_t)((len - common) / 16u),
                    "wwwwwwwwwwww");
}

/* ------------------------------------------------------------- the bus */

typedef struct { uint32_t pa; unsigned size; bool write; uint32_t count;
                 uint32_t first_value, first_pc; } mmio_stat_t;
#define MMIO_STATS 4096
static mmio_stat_t g_stats[MMIO_STATS];
static unsigned g_nstats, g_logged;
static uint64_t g_mmio_total;
static char g_uart[8192];
static size_t g_uart_len;

static void mmio_note(uint32_t pa, unsigned size, bool write, uint32_t v) {
    g_mmio_total++;
    const uint32_t pc = g_cpu.r[15];
    for (unsigned i = 0; i < g_nstats; i++)
        if (g_stats[i].pa == pa && g_stats[i].write == write &&
            g_stats[i].size == size) { g_stats[i].count++; goto log; }
    if (g_nstats < MMIO_STATS)
        g_stats[g_nstats++] = (mmio_stat_t){ pa, size, write, 1u, v, pc };
log:
    if (g_logged < 400u || g_verbose) {
        g_logged++;
        printf("  mmio %s%u %08x %s %08x  pc %08x %s\n", write ? "W" : "R", size * 8u,
               pa, write ? "<-" : "->", v, pc, sym(pc));
    }
}

/*
 * The PMGR timer (/arm-io/pmgr is device_type "timer"; reg 0x3f100000 ->
 * physical 0xbf100000), as the kernel's own code uses it:
 *   +0x200/+0x204  a 64-bit count, read high, low, high again until the two
 *                  highs agree (0x800895a4);
 *   +0x208         a decrementer: written with an interval in ticks
 *                  (0x800895dc; 0x3a975 = 240,000 = 10 ms at 24 MHz is the
 *                  first one) and read back as what remains (0x800895d0);
 *   +0x220         control: bit 0 enables the interrupt; writing bit 1
 *                  acknowledges it. The FIQ fast path (reached through the
 *                  FIQ vector's "mov pc, r9", the handler at 0x8008958c)
 *                  writes the control value and then the same with bit 1
 *                  clear, and init writes 3 then 1 the same way.
 * The interrupt is VIC0 line 6, which the kernel selects as FIQ
 * (0x8027b4f8). The count advances at the advertised timebase against an
 * assumed one instruction per cycle at the advertised CPU clock, so
 * TB_HZ / CPU_HZ = 1/25 of the retired-instruction count.
 */
#define TIMER_PA  UINT32_C(0xbf100200)
#define VIC_PA    UINT32_C(0xbf200000)
#define VIC_COUNT 3u
#define TIMER_LINE 6u
static s5l_vic_t g_vic[VIC_COUNT];
static struct { uint64_t start; uint32_t interval; bool armed, pending;
                uint32_t ctrl; uint64_t fired; } g_timer;

/* Cycles per timer tick, exactly: the count must never wrap or step back,
 * and cycles * TB_HZ overflows 64 bits once idle skipping has pushed the
 * cycle count past ~7.7e11 (it did, 1,281 guest seconds into a run). */
#define CYCLES_PER_TICK (CPU_HZ / TB_HZ)
_Static_assert(CPU_HZ % TB_HZ == 0, "the timebase must divide the CPU clock");
static uint64_t timer_count(void) {
    return g_cpu.cycles / CYCLES_PER_TICK;
}

static void irq_update(void) {
    bool irq = false, fiq = false;
    for (unsigned i = 0; i < VIC_COUNT; i++) {
        irq |= s5l_vic_irq(&g_vic[i]);
        fiq |= s5l_vic_fiq(&g_vic[i]);
    }
    g_cpu.irq_line = irq;
    g_cpu.fiq_line = fiq;
}

/* Expire the decrementer if its interval has passed, and drive line 6. */
static void timer_update(void) {
    if (g_timer.armed && timer_count() - g_timer.start >= g_timer.interval) {
        g_timer.armed = false;
        g_timer.pending = true;
        g_timer.fired++;
    }
    s5l_vic_set_line(&g_vic[0], TIMER_LINE, g_timer.pending && (g_timer.ctrl & 1u));
    irq_update();
}

static uint32_t mmio_read(uint32_t pa, unsigned size) {
    uint32_t v = 0;
    if (pa == UART0_PA + 0x10u) v = 0x6u;        /* UTRSTAT: TX empty */
    else if (pa == TIMER_PA) v = (uint32_t)timer_count();
    else if (pa == TIMER_PA + 4u) v = (uint32_t)(timer_count() >> 32);
    else if (pa == TIMER_PA + 8u) {
        uint64_t gone = timer_count() - g_timer.start;
        v = g_timer.armed && gone < g_timer.interval
          ? g_timer.interval - (uint32_t)gone : 0u;
    } else if (pa == TIMER_PA + 0x20u) v = g_timer.ctrl;
    else if (pa >= VIC_PA && pa < VIC_PA + VIC_COUNT * 0x10000u) {
        const unsigned n = (pa - VIC_PA) >> 16;
        const uint32_t off = pa & 0xffffu;
        v = off == VIC_VECTADDR ? s5l_vic_vectaddr(&g_vic[n], 32u * n)
                                : s5l_vic_read(&g_vic[n], off);
    }
    mmio_note(pa, size, false, v);
    return v;
}

static void mmio_write(uint32_t pa, unsigned size, uint32_t v) {
    if (pa >= TIMER_PA && pa < TIMER_PA + 0x40u && g_logged >= 400u && !g_verbose)
        printf("  timer W%u %08x <- %08x  count %" PRIu64 "  pc %08x %s\n", size * 8u,
               pa, v, timer_count(), g_cpu.r[15], sym(g_cpu.r[15]));
    if (pa == UART0_PA + 0x20u && g_uart_len + 1u < sizeof g_uart) {
        g_uart[g_uart_len++] = (char)v;
        fputc((int)(v & 0xffu), stderr);
    }
    if (pa == TIMER_PA + 8u) {
        g_timer.start = timer_count();
        g_timer.interval = v;
        g_timer.armed = true;
    } else if (pa == TIMER_PA + 0x20u) {
        if (v & 2u) g_timer.pending = false;      /* acknowledge */
        g_timer.ctrl = v & 1u;
    } else if (pa >= VIC_PA && pa < VIC_PA + VIC_COUNT * 0x10000u) {
        s5l_vic_write(&g_vic[(pa - VIC_PA) >> 16], pa & 0xffffu, v);
    }
    timer_update();
    mmio_note(pa, size, true, v);
}

static bool in_ram(uint32_t a, unsigned n) {
    return a >= DRAM_BASE && (uint64_t)a + n <= (uint64_t)DRAM_BASE + DRAM_SIZE;
}

static uint32_t b_r32(void *c, uint32_t a) { (void)c;
    if (in_ram(a, 4)) { uint32_t v; memcpy(&v, g_ram + (a - DRAM_BASE), 4); return v; }
    return mmio_read(a, 4); }
static uint16_t b_r16(void *c, uint32_t a) { (void)c;
    if (in_ram(a, 2)) { uint16_t v; memcpy(&v, g_ram + (a - DRAM_BASE), 2); return v; }
    return (uint16_t)mmio_read(a, 2); }
static uint8_t b_r8(void *c, uint32_t a) { (void)c;
    if (in_ram(a, 1)) return g_ram[a - DRAM_BASE];
    return (uint8_t)mmio_read(a, 1); }
static void b_w32(void *c, uint32_t a, uint32_t v) { (void)c;
    if (in_ram(a, 4)) { memcpy(g_ram + (a - DRAM_BASE), &v, 4); return; }
    mmio_write(a, 4, v); }
static void b_w16(void *c, uint32_t a, uint16_t v) { (void)c;
    if (in_ram(a, 2)) { memcpy(g_ram + (a - DRAM_BASE), &v, 2); return; }
    mmio_write(a, 2, v); }
static void b_w8(void *c, uint32_t a, uint8_t v) { (void)c;
    if (in_ram(a, 1)) { g_ram[a - DRAM_BASE] = v; return; }
    mmio_write(a, 1, v); }
static uint8_t *b_host(void *c, uint32_t a, uint32_t n) { (void)c;
    return in_ram(a, n) ? g_ram + (a - DRAM_BASE) : NULL; }
/* WFI: nothing else runs, so jump time to the decrementer's expiry, as the
 * iPhone OS 3 machine does, instead of spinning the host through an idle
 * guest. */
static uint64_t g_wfi;
static bool b_wfi(void *c) {
    (void)c;
    g_wfi++;
    if (g_timer.armed && (g_timer.ctrl & 1u)) {
        const uint64_t due = g_timer.start + g_timer.interval;
        const uint64_t due_cycles = due * CYCLES_PER_TICK;
        if (due_cycles > g_cpu.cycles) g_cpu.cycles = due_cycles;
    }
    timer_update();
    return g_cpu.irq_line || g_cpu.fiq_line;
}

/* A guest C string at a kernel virtual address, through the guest's own
 * translation. */
static const char *guest_str(uint32_t va) {
    static char buf[512];
    size_t i = 0;
    for (; i + 1 < sizeof buf; i++) {
        uint32_t pa;
        if (arm_mmu_translate(&g_cpu, va + (uint32_t)i, ARM_ACCESS_READ, true, &pa) ||
            !in_ram(pa, 1)) break;
        char ch = (char)g_ram[pa - DRAM_BASE];
        if (!ch) break;
        buf[i] = (ch == '\n') ? '|' : ch;
    }
    buf[i] = 0;
    return buf;
}

/* A guest word at a kernel virtual address, or false. */
static bool guest_word(uint32_t va, uint32_t *out) {
    uint32_t pa;
    if (arm_mmu_translate(&g_cpu, va, ARM_ACCESS_READ, true, &pa) || !in_ram(pa, 4))
        return false;
    memcpy(out, g_ram + (pa - DRAM_BASE), 4);
    return true;
}

/* The r7 frame chain (iOS keeps one: [r7] is the caller's r7, [r7+4] the
 * return address), innermost first. */
static void backtrace(const char *indent) {
    printf("%s%08x %s\n", indent, g_cpu.r[15], sym(g_cpu.r[15]));
    uint32_t fp = g_cpu.r[7];
    for (int depth = 0; depth < 16 && fp; depth++) {
        uint32_t next, lr;
        if (!guest_word(fp, &next) || !guest_word(fp + 4u, &lr) || !lr) break;
        printf("%s%08x %s\n", indent, lr, sym(lr));
        if (next <= fp) break;
        fp = next;
    }
}

/* panic(fmt, ...): the format string and its first three arguments, each
 * also read as a string when it points at one. */
static void report_panic(void) {
    const arm_cpu_t *c = &g_cpu;
    printf("  panic(\"%s\"", guest_str(c->r[0]));
    for (int i = 1; i < 4; i++) {
        printf(", %08x", c->r[i]);
        if (c->r[i] >= VIRT_BASE) {
            const char *str = guest_str(c->r[i]);
            if (strlen(str) > 2) printf(" \"%.60s\"", str);
        }
    }
    printf(")  from %s\n", sym(c->r[14]));
}

/* --------------------------------------------------------------- main */

static void dump_state(void) {
    const arm_cpu_t *c = &g_cpu;
    printf("\nstate: pc %08x %s  cpsr %08x\n", c->r[15], sym(c->r[15]), c->cpsr);
    for (int i = 0; i < 15; i += 4)
        printf("  r%-2d %08x  r%-2d %08x  r%-2d %08x  r%-2d %08x\n", i, c->r[i],
               i + 1, c->r[i + 1], i + 2, c->r[i + 2], i + 3, i + 3 < 16 ? c->r[i + 3] : 0);
    printf("  lr %s\n", sym(c->r[14]));
    printf("  backtrace:\n");
    backtrace("    ");
    printf("  sctlr %08x ttbr0 %08x ttbr1 %08x ttbcr %08x dacr %08x\n",
           c->cp15.sctlr, c->cp15.ttbr0, c->cp15.ttbr1, c->cp15.ttbcr, c->cp15.dacr);
    printf("  dfsr %08x dfar %08x ifsr %08x ifar %08x\n",
           c->cp15.dfsr, c->cp15.dfar, c->cp15.ifsr, c->cp15.ifar);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: boot3gs kernelcache.macho devicetree.bin "
                        "[-n insns] [-c boot-args] [-v] [-m dram.bin] [-u node]...\n");
        return 2;
    }
    uint64_t budget = 200000000u;
    const char *cmdline = "debug=0x8 serial=3 -v";
    const char *ram_out = NULL;
    const char *unmatch[16];
    unsigned nunmatch = 0;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) budget = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) cmdline = argv[++i];
        else if (!strcmp(argv[i], "-v")) g_verbose = true;
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) ram_out = argv[++i];
        else if (!strcmp(argv[i], "-u") && i + 1 < argc && nunmatch < 16u)
            unmatch[nunmatch++] = argv[++i];
        else die("unknown option %s", argv[i]);
    }
    size_t klen, dlen;
    uint8_t *kernel = read_file(argv[1], &klen);
    uint8_t *tree = read_file(argv[2], &dlen);
    /* Symbols and the prelinked-kext table are loaded separately, and either
     * may fail on its own; names are usable unless the image did not parse. */
    const ksyms_status_t kst = ksyms_load(&g_syms, kernel, klen);
    g_have_syms = kst != KSYMS_ERR_MACHO;
    printf("symbols: %s\n", ksyms_strerror(kst));

    macho_t m;
    macho_status_t ms = macho_parse(kernel, klen, &m);
    if (ms != MACHO_OK) die("kernel: %s", macho_strerror(ms));
    if (!m.has_entry || m.vm_low < VIRT_BASE) die("kernel: no entry or below 0x80000000");

    g_ram = calloc(1, DRAM_SIZE);
    if (!g_ram) die("cannot allocate 256 MB");

    /* The kernel's segments. */
    for (unsigned i = 0; i < m.segment_count; i++) {
        const macho_segment_t *s = &m.segments[i];
        if (!s->vmsize) continue;
        uint32_t pa = s->vmaddr - VIRT_BASE + DRAM_BASE;
        if (!in_ram(pa, s->vmsize)) die("segment %s outside DRAM", s->name);
        if (s->filesize)
            memcpy(g_ram + (pa - DRAM_BASE), kernel + s->fileoff,
                   s->filesize < s->vmsize ? s->filesize : s->vmsize);
        printf("segment %-16s va %08x pa %08x size %08x\n", s->name, s->vmaddr, pa,
               s->vmsize);
    }
    const uint32_t kernel_end = m.vm_high - VIRT_BASE + DRAM_BASE;
    const uint32_t tree_pa = (kernel_end + 0xfffu) & ~0xfffu;
    const uint32_t args_pa = (tree_pa + (uint32_t)dlen + 0xfffu) & ~0xfffu;
    const uint32_t tokd_pa = (args_pa + 0x1000u + 0x3fffu) & ~0x3fffu;
    printf("device tree pa %08x (%zu bytes), boot_args pa %08x, topOfKernelData pa %08x\n",
           tree_pa, dlen, args_pa, tokd_pa);

    /* The device tree, filled in as iBoot would. */
    uint8_t *dt = g_ram + (tree_pa - DRAM_BASE);
    memcpy(dt, tree, dlen);
    if (dt_skip_node(dt, dlen, 0) != dlen) die("device tree is not one complete flat tree");
    const uint32_t mem[2] = { DRAM_BASE, DRAM_SIZE };
    dt_set(dt, dlen, "memory", "reg", mem, 2);
    const uint32_t pram[2] = { PRAM_PA, PRAM_SIZE };
    dt_set(dt, dlen, "pram", "reg", pram, 2);
    static const struct { const char *path, *prop; uint32_t v; } clocks[] = {
        { "",          "clock-frequency",      BUS_HZ },
        { "cpus/cpu0", "timebase-frequency",   TB_HZ  },
        { "cpus/cpu0", "clock-frequency",      CPU_HZ },
        { "cpus/cpu0", "bus-frequency",        BUS_HZ },
        { "cpus/cpu0", "memory-frequency",     MEM_HZ },
        { "cpus/cpu0", "peripheral-frequency", PRF_HZ },
        { "cpus/cpu0", "fixed-frequency",      FIX_HZ },
    };
    for (size_t i = 0; i < sizeof clocks / sizeof clocks[0]; i++)
        dt_set(dt, dlen, clocks[i].path, clocks[i].prop, &clocks[i].v, 1);
    {
        size_t chosen = dt_find(dt, dlen, "chosen");
        uint32_t nl = 0;
        uint8_t *nv = chosen == (size_t)-1 ? NULL
                    : dt_prop(dt, dlen, chosen, "nvram-proxy-data", &nl);
        if (!nv || nl < 0x1000u) die("device tree: no /chosen:nvram-proxy-data");
        nvram_image(nv, nl);
    }
    /* -u: un-match a node, the way core/src/boot/bringup.c does: an 'x'
     * over the first byte of its compatible, so no driver claims it. */
    for (unsigned i = 0; i < nunmatch; i++) {
        size_t node = dt_find(dt, dlen, unmatch[i]);
        uint32_t cl = 0;
        uint8_t *compat = node == (size_t)-1 ? NULL
                        : dt_prop(dt, dlen, node, "compatible", &cl);
        if (!compat || !cl) die("cannot un-match /%s", unmatch[i]);
        compat[0] = 'x';
        printf("un-matched /%s\n", unmatch[i]);
    }
    dt_memmap(dt, dlen, "DeviceTree", tree_pa, (uint32_t)dlen);
    dt_memmap(dt, dlen, "BootArgs", args_pa, 0x1000u);

    /* boot_args. */
    uint8_t *ba = g_ram + (args_pa - DRAM_BASE);
    ba[0] = 1; ba[1] = 0;                   /* Revision */
    /* Version 5: pe_identify_machine (0x8027ace0) loads boot_args+2 and
     * panics "Epoch Mismatch" unless it is 5. (iPhone OS 3's wants 6.) */
    ba[2] = 5; ba[3] = 0;
    st32(ba + 0x04, VIRT_BASE);
    st32(ba + 0x08, DRAM_BASE);
    st32(ba + 0x0c, DRAM_SIZE - TOP_RESERVE);   /* the top is boot-owned */
    st32(ba + 0x10, tokd_pa);
    /* Boot_Video left zero: no framebuffer yet. */
    st32(ba + 0x30, tree_pa - DRAM_BASE + VIRT_BASE);
    st32(ba + 0x34, (uint32_t)dlen);
    if (strlen(cmdline) > 255u) die("boot-args longer than 255 bytes");
    memcpy(ba + 0x38, cmdline, strlen(cmdline));
    printf("boot-args \"%s\"\n", cmdline);

    /* The core. */
    static const arm_bus_t bus = {
        .read32 = b_r32, .read16 = b_r16, .read8 = b_r8,
        .write32 = b_w32, .write16 = b_w16, .write8 = b_w8,
        .host_ram = b_host, .wait_for_interrupt = b_wfi,
    };
    g_cpu.arch = ARM_ARCH_V7_A8;
    arm_reset(&g_cpu, &bus);
    g_cpu.cpsr = ARM_MODE_SVC | ARM_CPSR_I | ARM_CPSR_F | ARM_CPSR_A;
    const uint32_t entry_pa = m.entry - VIRT_BASE + DRAM_BASE;
    g_cpu.r[15] = entry_pa & ~1u;
    if (m.entry & 1u) g_cpu.cpsr |= ARM_CPSR_T;
    g_cpu.r[0] = args_pa;
    printf("entry va %08x pa %08x (%s)\n\n", m.entry, entry_pa, sym(m.entry));

    /* Run. */
    uint32_t ring[64] = {0};
    unsigned ring_i = 0, same = 0;
    uint64_t n = 0, exceptions = 0, interrupts = 0, undefs = 0;
    arm_status_t st = ARM_OK;
    const char *why = "instruction budget";
    const uint32_t panic_va = g_have_syms ? ksyms_value(&g_syms, "_panic") & ~1u : 0u;
    for (; n < budget; n++) {
        const uint32_t pc = g_cpu.r[15], mode = g_cpu.cpsr & 0x1fu;
        if (panic_va && pc == panic_va) report_panic();
        ring[ring_i++ & 63u] = pc;
        st = arm_step(&g_cpu);
        if (st != ARM_OK) { why = "the core refused an instruction"; break; }
        if (g_timer.armed) timer_update();
        const uint32_t npc = g_cpu.r[15], nmode = g_cpu.cpsr & 0x1fu;
        const uint32_t vbase = (g_cpu.cp15.sctlr & ARM_SCTLR_V) ? 0xffff0000u : 0u;
        if (nmode != mode && npc >= vbase && npc < vbase + 0x20u) {
            /* Interrupts are the machine working, not news; count them. So
             * are undefined-instruction traps: the kernel enables VFP per
             * thread lazily, so each thread's first VFP or NEON instruction
             * traps once and is re-run (e.g. vmov.i32 d16, #0 at 0x8024dff8). */
            if (npc - vbase == 0x18u || npc - vbase == 0x1cu) { interrupts++; continue; }
            if (npc - vbase == 0x04u) { undefs++; continue; }
            exceptions++;
            printf("  exception vector %02x from pc %08x %s  dfsr %08x dfar %08x "
                   "ifsr %08x ifar %08x\n", npc - vbase, pc, sym(pc), g_cpu.cp15.dfsr,
                   g_cpu.cp15.dfar, g_cpu.cp15.ifsr, g_cpu.cp15.ifar);
            if (exceptions > 20u) { why = "more than 20 exceptions"; n++; break; }
        }
        if ((n + 1u) % 250000000u == 0u) {
            printf("  at %" PRIu64 "M instructions, guest time %.2f s:\n", (n + 1u) / 1000000u,
                   (double)timer_count() / TB_HZ);
            backtrace("    ");
        }
        same = (npc == pc) ? same + 1u : 0u;
        if (same > 100000u) { why = "a branch to itself"; n++; break; }
    }
    printf("\nstopped after %" PRIu64 " instructions: %s", n, why);
    if (st != ARM_OK) {
        uint32_t pa = 0, insn = 0;
        if (arm_mmu_translate(&g_cpu, g_cpu.r[15], ARM_ACCESS_FETCH, true, &pa) == 0 &&
            in_ram(pa, 4))
            memcpy(&insn, g_ram + (pa - DRAM_BASE), 4);
        printf(" (status %d, insn %08x)", (int)st, insn);
    }
    printf("\n%" PRIu64 " device accesses, %" PRIu64 " WFI, %" PRIu64 " interrupts taken, "
           "%" PRIu64 " undefined-instruction traps, %" PRIu64 " timer expiries, "
           "guest time %.3f s\n", g_mmio_total, g_wfi, interrupts, undefs,
           g_timer.fired, (double)timer_count() / TB_HZ);
    dump_state();
    printf("\nlast pcs:\n");
    for (unsigned i = 0; i < 64u; i++) {
        uint32_t p = ring[(ring_i + i) & 63u];
        if (p) printf("  %08x %s\n", p, sym(p));
    }
    printf("\ndevice registers touched (%u):\n", g_nstats);
    for (unsigned i = 0; i < g_nstats; i++)
        printf("  %s%-2u %08x x%-7u first %08x at %08x %s\n", g_stats[i].write ? "W" : "R",
               g_stats[i].size * 8u, g_stats[i].pa, g_stats[i].count, g_stats[i].first_value,
               g_stats[i].first_pc, sym(g_stats[i].first_pc));
    if (g_uart_len) printf("\nUART0 output (%zu bytes):\n%.*s\n", g_uart_len,
                           (int)g_uart_len, g_uart);
    if (ram_out) {                      /* -m: all of DRAM, for offline reading */
        FILE *f = fopen(ram_out, "wb");
        if (!f || fwrite(g_ram, 1, DRAM_SIZE, f) != DRAM_SIZE) die("cannot write %s", ram_out);
        fclose(f);
        printf("DRAM written to %s\n", ram_out);
    }
    return 0;
}
