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
 *           [-r root.img [-P pristine.img]] [-w] [-F screen.ppm] [-B epoch]
 * -v logs every unmodelled access instead of the first 400; -m saves all of
 * DRAM at the end (the kernel's message buffer is in there); -u un-matches a
 * device-tree node (replacing the default, "arm-io/iop"); -e runs on the
 * cached interpreter in large slices, the way the app does, reporting only
 * the backtraces, the console and the end state -- the speed of that mode is
 * the app's. -r serves a root filesystem image as /dev/md0 (the kernel must
 * be 10B500 or the 3GS 7E18, whose gates are in tools/); the image is written
 * to, so pass a working copy, never the only one; with -P, a missing -r
 * image is first made from the pristine one (see make_work_image). -w (single-step mode)
 * records, for every kernel thread, the call chain of the last time it
 * blocked (entry to thread_block / thread_block_parameter), and prints each
 * thread's last wait at the end: where a stalled boot is waiting. -F writes
 * the framebuffer Boot_Video describes (n88_framebuffer) as a PPM image at
 * the end, reading each 32-bit pixel's low three octets as blue, green, red.
 * -B sets the boot_args version the kernel checks (default 5, iOS 6's; 4
 * when the kernel is the 3GS's iPhone OS 3.1.3 7E18 one, which wants it).
 * -r works with either known kernel, each with its own memory-disk gate.
 * The kernelcache must be decrypted and decompressed (a plain Mach-O), and
 * the device tree decrypted (the flat tree inside the IMG3). Both come from
 * the user's own IPSW; nothing Apple-owned is in this repository.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "arm.h"
#include "file_block.h"
#include "ios3_n88_kernel_patch.h"
#include "ios6_kernel_patch.h"
#include "ksyms.h"
#include "n88.h"
#include "rootfs_work.h"

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

/* --------------------------------------------------- the working image */
/*
 * -P: the working image, made from the pristine one by rootfs_work_create,
 * the provisioner iPhone OS 3 machines use. Its default rewrites /etc/fstab
 * to mount / from /dev/md0 (the stock one names the NAND's disk0s1 and
 * disk0s2, which do not exist here). It also grows the volume: Apple ships
 * the root filesystem with no free blocks, because on the phone /private/var
 * is a separate partition, and without room the first file the system
 * creates fails (on 10B500, corecrypto's FIPS self-test control file, and
 * launchd reboots).
 */
#define WORK_GROWTH_BYTES (UINT64_C(256) << 20)

static void make_work_image(const char *pristine, const char *work) {
    rootfs_work_options_t ro;
    rootfs_work_result_t rr;
    memset(&ro, 0, sizeof ro);
    memset(&rr, 0, sizeof rr);
    ro.growth_bytes = WORK_GROWTH_BYTES;
    printf("making %s from %s\n", work, pristine);
    const rootfs_work_status_t rs = rootfs_work_create(pristine, work, &ro, &rr);
    if (rs != ROOTFS_WORK_OK)
        die("rootfs_work: %s at %s (%s)", rootfs_work_status_name(rs),
            rootfs_work_stage_name(rr.stage), rr.detail);
    printf("  fstab rewritten at offset %llu; volume grown to %llu bytes\n",
           (unsigned long long)rr.fstab_offset, (unsigned long long)rr.final_size);
}

/* ------------------------------------------------------- root disk log */
/* The root disk as the kernel uses it: every request the bridge makes,
 * forwarded to the file and logged (the first 200 in order, with the guest
 * pc and instruction count, then only counted). */
static const vm_block_t *g_root_inner;
static unsigned g_root_logged;

static vm_block_io_status_t logged_read(void *ctx, uint64_t off, void *dst,
                                        size_t n, size_t *actual) {
    (void)ctx;
    if (g_root_logged++ < 200u)
        printf("  disk R %10llx +%-6zu  pc %08x  thread %08x  guest %.3f s\n",
               (unsigned long long)off, n, g_m.cpu.r[15], g_m.cpu.cp15.tpidrprw,
               n88_guest_seconds(&g_m));
    return g_root_inner->read_at(g_root_inner->context, off, dst, n, actual);
}
static vm_block_io_status_t logged_write(void *ctx, uint64_t off, const void *src,
                                         size_t n, size_t *actual) {
    (void)ctx;
    if (g_root_logged++ < 200u)
        printf("  disk W %10llx +%-6zu  pc %08x  thread %08x  guest %.3f s\n",
               (unsigned long long)off, n, g_m.cpu.r[15], g_m.cpu.cp15.tpidrprw,
               n88_guest_seconds(&g_m));
    return g_root_inner->write_at(g_root_inner->context, off, src, n, actual);
}
static vm_block_io_status_t logged_flush(void *ctx) {
    (void)ctx;
    return g_root_inner->flush ? g_root_inner->flush(g_root_inner->context) : VM_BLOCK_IO_OK;
}

/* ------------------------------------------------------ thread waits */
/* -w: each kernel thread's most recent block, as a call chain. */
#define WAIT_FRAMES 12u
#define WAIT_THREADS 1024u
typedef struct {
    uint32_t thread;
    uint32_t frames[WAIT_FRAMES];
    unsigned nframes;
    uint64_t blocks;
    double   at;
} waitrec_t;
static waitrec_t g_waits[WAIT_THREADS];
static unsigned g_nwaits;

static bool guest_word(uint32_t va, uint32_t *out);

/* At the entry of a blocking function: lr is the return into the caller,
 * and the caller's r7 frame chain continues from there. */
static void note_wait(void) {
    const uint32_t th = g_m.cpu.cp15.tpidrprw;
    waitrec_t *w = NULL;
    for (unsigned i = 0; i < g_nwaits; i++)
        if (g_waits[i].thread == th) { w = &g_waits[i]; break; }
    if (!w) {
        if (g_nwaits == WAIT_THREADS) return;
        w = &g_waits[g_nwaits++];
        memset(w, 0, sizeof *w);
        w->thread = th;
    }
    w->blocks++;
    w->at = n88_guest_seconds(&g_m);
    w->nframes = 0;
    w->frames[w->nframes++] = g_m.cpu.r[14];
    uint32_t fp = g_m.cpu.r[7];
    while (w->nframes < WAIT_FRAMES && fp) {
        uint32_t next, lr;
        if (!guest_word(fp, &next) || !guest_word(fp + 4u, &lr) || !lr) break;
        w->frames[w->nframes++] = lr;
        if (next <= fp) break;
        fp = next;
    }
}

static int wait_by_time(const void *a, const void *b) {
    const double x = ((const waitrec_t *)a)->at, y = ((const waitrec_t *)b)->at;
    return x < y ? 1 : x > y ? -1 : 0;
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

/* The console, each line stamped with the guest time it was drained at. */
static void drain_console(void) {
    static bool at_line_start = true;
    char buf[4096];
    size_t n;
    while ((n = n88_console_take(&g_m, buf, sizeof buf)) > 0) {
        for (size_t i = 0; i < n; i++) {
            if (at_line_start) fprintf(stderr, "[%9.3f] ", n88_guest_seconds(&g_m));
            fputc(buf[i], stderr);
            at_line_start = buf[i] == '\n';
        }
    }
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
                        "[-c boot-args] [-v] [-m dram.bin] [-u node]... [-e] "
                        "[-r root.img] [-F screen.ppm] [-B epoch]\n");
        return 2;
    }
    uint64_t budget = 200000000u;
    const char *cmdline = NULL;
    const char *ram_out = NULL, *screen_out = NULL;
    const char *root_path = NULL, *pristine_path = NULL;
    const char *unmatch[16];
    unsigned nunmatch = 0;
    bool engine = false, waits = false;
    unsigned epoch = 0;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) budget = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) cmdline = argv[++i];
        else if (!strcmp(argv[i], "-v")) g_verbose = true;
        else if (!strcmp(argv[i], "-e")) engine = true;
        else if (!strcmp(argv[i], "-w")) waits = true;
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) ram_out = argv[++i];
        else if (!strcmp(argv[i], "-F") && i + 1 < argc) screen_out = argv[++i];
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) root_path = argv[++i];
        else if (!strcmp(argv[i], "-P") && i + 1 < argc) pristine_path = argv[++i];
        else if (!strcmp(argv[i], "-B") && i + 1 < argc) {
            epoch = (unsigned)strtoul(argv[++i], NULL, 0);
            if (!epoch || epoch > 255u) die("-B wants an epoch from 1 to 255");
        }
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

    /* The two kernels this machine knows, by LC_UUID: each has its own
     * memory-disk gate, and 3.1.3 its own boot epoch. */
    const bool is_ios6 = ios6_kernel_patch_identify(kernel, klen);
    const bool is_ios3 = !is_ios6 && ios3_n88_kernel_patch_identify(kernel, klen);
    if (is_ios3 && !epoch) epoch = IOS3_N88_KERNEL_BOOT_ARGS_VERSION;
    printf("kernel: %s\n", is_ios6 ? "iOS 6.1.6 10B500" : is_ios3 ? "iPhone OS 3.1.3 7E18" : "unrecognised");

    file_block_t *root_file = NULL;
    const vm_block_t *root = NULL;
    if (root_path) {
        if (!is_ios6 && !is_ios3)
            die("-r needs the iPhone2,1 10B500 or 7E18 kernel (its UUID is neither)");
        FILE *existing = fopen(root_path, "rb");
        if (existing) fclose(existing);
        else if (pristine_path) make_work_image(pristine_path, root_path);
        root_file = file_block_create();
        FILE *f = fopen(root_path, "rb");
        if (!f || fseek(f, 0, SEEK_END) != 0) die("cannot open %s", root_path);
        const long size = ftell(f);
        fclose(f);
        file_block_status_t fs;
        if (!root_file || size <= 0 ||
            (fs = file_block_open(root_file, root_path, (uint64_t)size)) != FILE_BLOCK_STATUS_OK)
            die("cannot open %s as a block device", root_path);
        g_root_inner = file_block_get(root_file);
        static vm_block_t logged;
        logged = *g_root_inner;
        logged.context = NULL;
        logged.read_at = logged_read;
        logged.write_at = logged_write;
        logged.flush = logged_flush;
        root = &logged;
        printf("root filesystem: %s, %ld bytes, as /dev/md0\n", root_path, size);
    }

    if (!n88_init(&g_m, engine)) die("cannot allocate the machine");
    g_m.trace = on_unmodelled;
    char detail[256];
    const n88_boot_t req = {
        .kernel = kernel, .kernel_size = klen, .devicetree = tree, .devicetree_size = dlen,
        .cmdline = cmdline,
        .unmatch = nunmatch ? unmatch : NULL, .unmatch_count = nunmatch,
        .root = root,
        .md_read_site_pc = !root ? 0u : is_ios6 ? IOS6_KERNEL_PATCH_MD_READ_VA
                                                : IOS3_N88_KERNEL_PATCH_MD_READ_VA,
        .md_write_site_pc = !root ? 0u : is_ios6 ? IOS6_KERNEL_PATCH_MD_WRITE_VA
                                                 : IOS3_N88_KERNEL_PATCH_MD_WRITE_VA,
        .boot_args_version = (uint8_t)epoch,
    };
    const n88_status_t bs = n88_boot(&g_m, &req, detail, sizeof detail);
    if (bs != N88_OK) die("%s: %s", n88_strerror(bs), detail);
    if (root) {
        guest_patch_report_t pr;
        const guest_patch_status_t ps = is_ios6
            ? ios6_kernel_patch_apply(g_m.ram, N88_DRAM_SIZE, &pr)
            : ios3_n88_kernel_patch_apply(g_m.ram, N88_DRAM_SIZE, &pr);
        if (ps != GUEST_PATCH_STATUS_OK)
            die("the kernel patch was refused: %s (entry %u, va %08llx)",
                guest_patch_status_string(ps), pr.entry_index,
                (unsigned long long)pr.virtual_address);
        printf("kernel patched for the memory-disk bridge (%u sites)\n", is_ios6 ? 4u : 3u);
    }
    printf("device tree pa %08x (%u bytes), boot_args pa %08x, topOfKernelData pa %08x\n",
           g_m.devicetree_pa, g_m.devicetree_size, g_m.boot_args_pa, g_m.tokd_pa);
    printf("boot-args \"%s\"\n", (const char *)g_m.ram + (g_m.boot_args_pa - N88_DRAM_BASE) + 0x38);
    printf("un-matched: ");
    for (unsigned i = 0; i < (nunmatch ? nunmatch : 1u); i++)
        printf("%s/%s", i ? ", " : "", nunmatch ? unmatch[i] : "arm-io/iop");
    printf("\nentry pa %08x (%s)%s\n\n", g_m.entry_pa,
           sym(g_m.entry_pa - N88_DRAM_BASE +
               (is_ios3 ? IOS3_N88_KERNEL_PATCH_VIRT_BASE : N88_VIRT_BASE)),
           engine ? ", cached interpreter" : "");

    uint32_t ring[64] = {0};
    unsigned ring_i = 0, same = 0;
    uint64_t n = 0, exceptions = 0, interrupts = 0, undefs = 0;
    uint32_t last_fault_pc = 0, last_fault_addr = 0, repeats = 0;
    arm_status_t st = ARM_OK;
    const char *why = "instruction budget";
    const uint32_t panic_va = g_have_syms ? ksyms_value(&g_syms, "_panic") & ~1u : 0u;
    const uint32_t block_va = waits && g_have_syms
                            ? ksyms_value(&g_syms, "_thread_block") & ~1u : 0u;
    const uint32_t blockp_va = waits && g_have_syms
                             ? ksyms_value(&g_syms, "_thread_block_parameter") & ~1u : 0u;
    if (waits && (engine || !block_va || !blockp_va))
        die("-w needs single-step mode and a kernel with thread_block symbols");
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
            if (waits && (pc == block_va || pc == blockp_va)) note_wait();
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
                /* Aborts can be the machine working too: iPhone OS 3's kernel
                 * memory is pageable, so it touches a new buffer page by page
                 * and faults each one in. What is news is the same
                 * instruction faulting on the same address over and over. */
                exceptions++;
                const uint32_t fault_at = npc - vbase == 0x0cu ? g_m.cpu.cp15.ifar
                                                               : g_m.cpu.cp15.dfar;
                repeats = (pc == last_fault_pc && fault_at == last_fault_addr) ? repeats + 1u : 0u;
                last_fault_pc = pc;
                last_fault_addr = fault_at;
                if (exceptions <= 20u)
                    printf("  exception vector %02x from pc %08x %s  dfsr %08x dfar %08x "
                           "ifsr %08x ifar %08x\n", npc - vbase, pc, sym(pc), g_m.cpu.cp15.dfsr,
                           g_m.cpu.cp15.dfar, g_m.cpu.cp15.ifsr, g_m.cpu.cp15.ifar);
                if (exceptions <= 5u) {
                    /* r7 is not banked, so the chain is still the faulting
                     * code's; start it from the faulting pc. */
                    const uint32_t vpc = g_m.cpu.r[15];
                    g_m.cpu.r[15] = pc;
                    backtrace("      ");
                    g_m.cpu.r[15] = vpc;
                }
                if (repeats >= 20u) {
                    why = "the same fault 20 times in a row"; n++; break;
                }
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
    if (engine && g_m.ci) {
        /* The cached interpreter's own accounting: how much of the boot the
         * engine retired, and which instruction classes drop to the slower
         * reference path -- the levers for real-device speed (#37). */
        arm_ci_stats_t cs;
        arm_ci_get_stats(g_m.ci, &cs);
        char desc[1024];
        arm_ci_describe_stats(&cs, n, desc, sizeof desc);
        printf("engine:\n%s", desc);
        if (desc[0] && desc[strlen(desc) - 1] != '\n') printf("\n");
    }
    printf("console: %" PRIu64 " bytes (%" PRIu64 " dropped from the ring)\n",
           g_m.console_total, g_m.console_dropped);
    if (g_m.has_root) {
        const md_bridge_stats_t *ms = &g_m.md.stats;
        printf("root disk: %" PRIu64 " reads (%" PRIu64 " bytes), %" PRIu64 " writes (%"
               PRIu64 " bytes), %" PRIu64 " failures", ms->successful_reads, ms->bytes_read,
               ms->successful_writes, ms->bytes_written, ms->failures);
        if (ms->failures)
            printf("; last: %s at pc %08x", md_bridge_error_string(g_m.md.last_error.code),
                   g_m.md.last_error.pc);
        printf("\n");
    }
    {
        const s5l_spi_t *sp = &g_m.spi0;
        const spi_nor_t *nr = &g_m.nor;
        printf("spi0: %" PRIu64 " words, %" PRIu64 " receive reads, %" PRIu64
               " interrupt rises, %" PRIu64 " transmit drops\n",
               sp->words, sp->rx_reads, sp->irq_rises, sp->tx_drops);
        printf("nor: %" PRIu64 " selections, %" PRIu64 " commands; %" PRIu64 " reads (%"
               PRIu64 " octets), %" PRIu64 " programs (%" PRIu64 " octets), %" PRIu64
               " erases, %" PRIu64 " status writes, %" PRIu64 " refused, %" PRIu64
               " unknown", nr->selections, nr->commands, nr->reads, nr->read_octets,
               nr->programs, nr->programmed_octets, nr->erases, nr->status_writes,
               nr->refused, nr->unknown);
        if (nr->unknown) printf(" (last %02x)", nr->last_unknown);
        printf("\n");
        const cdma_t *dm = &g_m.cdma;
        printf("cdma: %" PRIu64 " transfers (%" PRIu64 " octets), %" PRIu64 " AES (%" PRIu64
               " with the stand-in hardware key), %" PRIu64 " errors\n", dm->transfers,
               dm->octets, dm->aes_ops, dm->hardware_key_ops, dm->errors);
        if (dm->periph_transfers || dm->periph_unclaimed)
            printf("cdma peripheral: %" PRIu64 " transfers (%" PRIu64 " octets), %" PRIu64
                   " unclaimed; sha1: %" PRIu64 " blocks\n", dm->periph_transfers,
                   dm->periph_octets, dm->periph_unclaimed, g_m.sha1.blocks);
    }
    dump_state();
    if (!engine) {
        printf("\nlast pcs:\n");
        for (unsigned i = 0; i < 64u; i++) {
            uint32_t p = ring[(ring_i + i) & 63u];
            if (p) printf("  %08x %s\n", p, sym(p));
        }
    }
    if (waits) {
        qsort(g_waits, g_nwaits, sizeof g_waits[0], wait_by_time);
        printf("\nthreads by last block, newest first (%u):\n", g_nwaits);
        for (unsigned i = 0; i < g_nwaits; i++) {
            const waitrec_t *w = &g_waits[i];
            printf("  thread %08x  %" PRIu64 " blocks, last at %.3f s\n", w->thread,
                   w->blocks, w->at);
            for (unsigned f = 0; f < w->nframes; f++)
                printf("      %08x %s\n", w->frames[f], sym(w->frames[f]));
        }
    }
    printf("\nunmodelled device registers touched (%u):\n", g_nstats);
    for (unsigned i = 0; i < g_nstats; i++)
        printf("  %s%-2u %08x x%-7u first %08x at %08x %s\n", g_stats[i].write ? "W" : "R",
               g_stats[i].size * 8u, g_stats[i].pa, g_stats[i].count, g_stats[i].first_value,
               g_stats[i].first_pc, sym(g_stats[i].first_pc));
    if (screen_out) {                   /* -F: the framebuffer as a PPM image */
        const uint8_t *fb = n88_framebuffer(&g_m);
        FILE *f = fb ? fopen(screen_out, "wb") : NULL;
        if (!f) die("cannot write %s", screen_out);
        fprintf(f, "P6\n%u %u\n255\n", N88_FB_WIDTH, N88_FB_HEIGHT);
        uint64_t lit = 0;
        for (unsigned y = 0; y < N88_FB_HEIGHT; y++)
            for (unsigned x = 0; x < N88_FB_WIDTH; x++) {
                const uint8_t *px = fb + y * N88_FB_STRIDE + x * 4u;
                const uint8_t rgb[3] = { px[2], px[1], px[0] };
                lit += (px[0] | px[1] | px[2]) != 0;
                fwrite(rgb, 1, 3, f);
            }
        fclose(f);
        printf("framebuffer written to %s (%" PRIu64 " of %u pixels not black)\n",
               screen_out, lit, N88_FB_WIDTH * N88_FB_HEIGHT);
    }
    if (ram_out) {                      /* -m: all of DRAM, for offline reading */
        FILE *f = fopen(ram_out, "wb");
        if (!f || fwrite(g_m.ram, 1, N88_DRAM_SIZE, f) != N88_DRAM_SIZE)
            die("cannot write %s", ram_out);
        fclose(f);
        printf("DRAM written to %s\n", ram_out);
    }
    n88_free(&g_m);
    if (root_file) {
        file_block_close(root_file);
        file_block_destroy(&root_file);
    }
    return 0;
}
