/*
 * boot3gs — start the iPhone 3GS (N88AP, S5L8920) iOS 6 kernel on the core's
 * 3GS machine (core/include/n88.h) and record what it asks for.
 *
 * A research harness, not the product. It exists to answer one question
 * empirically: which devices does the iOS 6 kernel need, in what order? The
 * iPhone OS 3 machine was brought up the same way (docs/BOOTLOG.md): run the
 * real kernel, see what it asks for, model that, repeat. The machine itself
 * (DRAM, UART0, the PMGR timer, the VICs, bring-up as iBoot does it) lives
 * in core/src/soc/n88.c, so the app runs exactly what this harness runs.
 *
 * On top of the machine it reports:
 *   - every access to a device register nothing models (address, size,
 *     value, pc, symbol), the first 400 in order and then a count per
 *     register -- the kernel's next request shows up there;
 *   - every exception taken other than interrupts and the lazy-VFP undefined
 *     traps (vector, faulting pc, DFSR/DFAR or IFSR/IFAR);
 *   - panic() calls, with the format string, when the kernel has symbols;
 *   - an r7 backtrace every 250M instructions;
 *   - the UART0 console text;
 *   - why it stopped: an instruction the core refused, the budget, more than
 *     20 exceptions, or a branch to itself.
 *
 * Usage:
 *   boot3gs <kernelcache.macho> <devicetree.bin> [-n instructions]
 *           [-c "boot-args"] [-v] [-m dram.bin] [-u node/path]... [-e]
 * -v logs every unmodelled access instead of the first 400; -m saves all of
 * DRAM at the end (the kernel's message buffer is in there); -u un-matches a
 * device-tree node (replacing the default, "arm-io/iop"); -e runs on the
 * cached interpreter in large slices, the way the app does, reporting only
 * the backtraces, the console and the end state -- the speed of that mode is
 * the app's.
 * The kernelcache must be decrypted and decompressed (a plain Mach-O), and
 * the device tree decrypted (the flat tree inside the IMG3). Both come from
 * the user's own IPSW; nothing Apple-owned is in this repository.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "arm.h"
#include "ksyms.h"
#include "n88.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static n88_t   g_m;
static ksyms_t g_syms;
static bool    g_have_syms;
static bool    g_verbose;

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

static bool in_ram(uint32_t a, unsigned n) {
    return a >= N88_DRAM_BASE && (uint64_t)a + n <= (uint64_t)N88_DRAM_BASE + N88_DRAM_SIZE;
}

/* ------------------------------------------------- unmodelled accesses */

typedef struct { uint32_t pa; unsigned size; bool write; uint32_t count;
                 uint32_t first_value, first_pc; } mmio_stat_t;
#define MMIO_STATS 4096
static mmio_stat_t g_stats[MMIO_STATS];
static unsigned g_nstats, g_logged;

static void on_unmodelled(void *ctx, uint32_t pa, unsigned size, bool write,
                          uint32_t v, uint32_t pc) {
    (void)ctx;
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

/* ------------------------------------------------------ guest reading */

/* A guest C string at a kernel virtual address, through the guest's own
 * translation. */
static const char *guest_str(uint32_t va) {
    static char buf[512];
    size_t i = 0;
    for (; i + 1 < sizeof buf; i++) {
        uint32_t pa;
        if (arm_mmu_translate(&g_m.cpu, va + (uint32_t)i, ARM_ACCESS_READ, true, &pa) ||
            !in_ram(pa, 1)) break;
        char ch = (char)g_m.ram[pa - N88_DRAM_BASE];
        if (!ch) break;
        buf[i] = (ch == '\n') ? '|' : ch;
    }
    buf[i] = 0;
    return buf;
}

static bool guest_word(uint32_t va, uint32_t *out) {
    uint32_t pa;
    if (arm_mmu_translate(&g_m.cpu, va, ARM_ACCESS_READ, true, &pa) || !in_ram(pa, 4))
        return false;
    memcpy(out, g_m.ram + (pa - N88_DRAM_BASE), 4);
    return true;
}

/* The r7 frame chain (iOS keeps one: [r7] is the caller's r7, [r7+4] the
 * return address), innermost first. */
static void backtrace(const char *indent) {
    printf("%s%08x %s\n", indent, g_m.cpu.r[15], sym(g_m.cpu.r[15]));
    uint32_t fp = g_m.cpu.r[7];
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
    const arm_cpu_t *c = &g_m.cpu;
    printf("  panic(\"%s\"", guest_str(c->r[0]));
    for (int i = 1; i < 4; i++) {
        printf(", %08x", c->r[i]);
        if (c->r[i] >= N88_VIRT_BASE) {
            const char *str = guest_str(c->r[i]);
            if (strlen(str) > 2) printf(" \"%.60s\"", str);
        }
    }
    printf(")  from %s\n", sym(c->r[14]));
}

static void drain_console(void) {
    char buf[4096];
    size_t n;
    while ((n = n88_console_take(&g_m, buf, sizeof buf)) > 0)
        fwrite(buf, 1, n, stderr);
}

static void dump_state(void) {
    const arm_cpu_t *c = &g_m.cpu;
    printf("\nstate: pc %08x %s  cpsr %08x\n", c->r[15], sym(c->r[15]), c->cpsr);
    for (int i = 0; i < 16; i += 4)
        printf("  r%-2d %08x  r%-2d %08x  r%-2d %08x  r%-2d %08x\n", i, c->r[i],
               i + 1, c->r[i + 1], i + 2, c->r[i + 2], i + 3, c->r[i + 3]);
    printf("  lr %s\n", sym(c->r[14]));
    printf("  backtrace:\n");
    backtrace("    ");
    printf("  sctlr %08x ttbr0 %08x ttbr1 %08x ttbcr %08x dacr %08x\n",
           c->cp15.sctlr, c->cp15.ttbr0, c->cp15.ttbr1, c->cp15.ttbcr, c->cp15.dacr);
    printf("  dfsr %08x dfar %08x ifsr %08x ifar %08x\n",
           c->cp15.dfsr, c->cp15.dfar, c->cp15.ifsr, c->cp15.ifar);
}

/* --------------------------------------------------------------- main */

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: boot3gs kernelcache.macho devicetree.bin [-n insns] "
                        "[-c boot-args] [-v] [-m dram.bin] [-u node]... [-e]\n");
        return 2;
    }
    uint64_t budget = 200000000u;
    const char *cmdline = NULL;
    const char *ram_out = NULL;
    const char *unmatch[16];
    unsigned nunmatch = 0;
    bool engine = false;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) budget = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) cmdline = argv[++i];
        else if (!strcmp(argv[i], "-v")) g_verbose = true;
        else if (!strcmp(argv[i], "-e")) engine = true;
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
    if (!n88_devicetree_is_3gs(tree, dlen))
        printf("warning: the device tree's root is not compatible \"N88AP\"\n");

    if (!n88_init(&g_m, engine)) die("cannot allocate the machine");
    g_m.trace = on_unmodelled;
    char detail[256];
    const n88_boot_t req = {
        .kernel = kernel, .kernel_size = klen, .devicetree = tree, .devicetree_size = dlen,
        .cmdline = cmdline,
        .unmatch = nunmatch ? unmatch : NULL, .unmatch_count = nunmatch,
    };
    const n88_status_t bs = n88_boot(&g_m, &req, detail, sizeof detail);
    if (bs != N88_OK) die("%s: %s", n88_strerror(bs), detail);
    printf("device tree pa %08x (%u bytes), boot_args pa %08x, topOfKernelData pa %08x\n",
           g_m.devicetree_pa, g_m.devicetree_size, g_m.boot_args_pa, g_m.tokd_pa);
    printf("boot-args \"%s\"\n", (const char *)g_m.ram + (g_m.boot_args_pa - N88_DRAM_BASE) + 0x38);
    printf("un-matched: ");
    for (unsigned i = 0; i < (nunmatch ? nunmatch : 1u); i++)
        printf("%s/%s", i ? ", " : "", nunmatch ? unmatch[i] : "arm-io/iop");
    printf("\nentry pa %08x (%s)%s\n\n", g_m.entry_pa,
           sym(g_m.entry_pa - N88_DRAM_BASE + N88_VIRT_BASE),
           engine ? ", cached interpreter" : "");

    uint32_t ring[64] = {0};
    unsigned ring_i = 0, same = 0;
    uint64_t n = 0, exceptions = 0, interrupts = 0, undefs = 0;
    arm_status_t st = ARM_OK;
    const char *why = "instruction budget";
    const uint32_t panic_va = g_have_syms ? ksyms_value(&g_syms, "_panic") & ~1u : 0u;
    const uint64_t every = 250000000u;
    const clock_t t0 = clock();
    if (engine) {
        /* The app's way: large slices, nothing per instruction. */
        while (n < budget) {
            const uint64_t to_mark = every - n % every;
            uint64_t slice = budget - n < to_mark ? budget - n : to_mark;
            if (slice > 10000000u) slice = 10000000u;
            n += n88_run(&g_m, (unsigned)slice, &st);
            drain_console();
            if (st != ARM_OK) { why = "the core refused an instruction"; break; }
            if (n % every == 0u) {
                printf("  at %" PRIu64 "M instructions, guest time %.2f s:\n",
                       n / 1000000u, n88_guest_seconds(&g_m));
                backtrace("    ");
            }
        }
    } else {
        for (; n < budget; n++) {
            const uint32_t pc = g_m.cpu.r[15], mode = g_m.cpu.cpsr & 0x1fu;
            if (panic_va && pc == panic_va) report_panic();
            ring[ring_i++ & 63u] = pc;
            if (n88_run(&g_m, 1, &st) != 1u) { why = "the core refused an instruction"; break; }
            drain_console();
            const uint32_t npc = g_m.cpu.r[15], nmode = g_m.cpu.cpsr & 0x1fu;
            const uint32_t vbase = (g_m.cpu.cp15.sctlr & ARM_SCTLR_V) ? 0xffff0000u : 0u;
            if (nmode != mode && npc >= vbase && npc < vbase + 0x20u) {
                /* Interrupts are the machine working, not news; count them.
                 * So are undefined-instruction traps: the kernel enables VFP
                 * per thread lazily, so each thread's first VFP or NEON
                 * instruction traps once and is re-run. */
                if (npc - vbase == 0x18u || npc - vbase == 0x1cu) { interrupts++; continue; }
                if (npc - vbase == 0x04u) { undefs++; continue; }
                exceptions++;
                printf("  exception vector %02x from pc %08x %s  dfsr %08x dfar %08x "
                       "ifsr %08x ifar %08x\n", npc - vbase, pc, sym(pc), g_m.cpu.cp15.dfsr,
                       g_m.cpu.cp15.dfar, g_m.cpu.cp15.ifsr, g_m.cpu.cp15.ifar);
                if (exceptions > 20u) { why = "more than 20 exceptions"; n++; break; }
            }
            if ((n + 1u) % every == 0u) {
                printf("  at %" PRIu64 "M instructions, guest time %.2f s:\n",
                       (n + 1u) / 1000000u, n88_guest_seconds(&g_m));
                backtrace("    ");
            }
            same = (npc == pc) ? same + 1u : 0u;
            if (same > 100000u) { why = "a branch to itself"; n++; break; }
        }
    }
    const double host_s = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("\nstopped after %" PRIu64 " instructions: %s", n, why);
    if (st != ARM_OK) {
        uint32_t pa = 0, insn = 0;
        if (arm_mmu_translate(&g_m.cpu, g_m.cpu.r[15], ARM_ACCESS_FETCH, true, &pa) == 0 &&
            in_ram(pa, 4))
            memcpy(&insn, g_m.ram + (pa - N88_DRAM_BASE), 4);
        printf(" (status %d, insn %08x)", (int)st, insn);
    }
    printf("\nhost CPU time %.2f s (%.1f M instructions/s)\n", host_s,
           host_s > 0 ? (double)n / host_s / 1e6 : 0.0);
    printf("%" PRIu64 " modelled device accesses, %" PRIu64 " unmodelled, %" PRIu64 " WFI, ",
           g_m.mmio, g_m.unmodelled, g_m.wfi);
    if (!engine)
        printf("%" PRIu64 " interrupts taken, %" PRIu64 " undefined-instruction traps, ",
               interrupts, undefs);
    printf("%" PRIu64 " timer expiries, guest time %.3f s\n", g_m.timer.fired,
           n88_guest_seconds(&g_m));
    printf("console: %" PRIu64 " bytes (%" PRIu64 " dropped from the ring)\n",
           g_m.console_total, g_m.console_dropped);
    dump_state();
    if (!engine) {
        printf("\nlast pcs:\n");
        for (unsigned i = 0; i < 64u; i++) {
            uint32_t p = ring[(ring_i + i) & 63u];
            if (p) printf("  %08x %s\n", p, sym(p));
        }
    }
    printf("\nunmodelled device registers touched (%u):\n", g_nstats);
    for (unsigned i = 0; i < g_nstats; i++)
        printf("  %s%-2u %08x x%-7u first %08x at %08x %s\n", g_stats[i].write ? "W" : "R",
               g_stats[i].size * 8u, g_stats[i].pa, g_stats[i].count, g_stats[i].first_value,
               g_stats[i].first_pc, sym(g_stats[i].first_pc));
    if (ram_out) {                      /* -m: all of DRAM, for offline reading */
        FILE *f = fopen(ram_out, "wb");
        if (!f || fwrite(g_m.ram, 1, N88_DRAM_SIZE, f) != N88_DRAM_SIZE)
            die("cannot write %s", ram_out);
        fclose(f);
        printf("DRAM written to %s\n", ram_out);
    }
    n88_free(&g_m);
    return 0;
}
