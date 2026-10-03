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
 *           [-r root.img [-P pristine.img [-a]]] [-w] [-F screen.ppm] [-B epoch]
 *           [-D x0,y0,x1,y1,t]... [-T x,y,t]... [-K name,t[,s]]... [-R s] [-S t]...
 *           [-A out.wav] [-C lo,hi]... [-X addr]...
 * -v logs every unmodelled access instead of the first 400; -m saves all of
 * DRAM at the end (the kernel's message buffer is in there); -u un-matches a
 * device-tree node (replacing the default: "arm-io/iop", or for the 7E18
 * kernel the list at ios3_unmatch); -e runs on the
 * cached interpreter in large slices, the way the app does, reporting only
 * the backtraces, the console and the end state -- the speed of that mode is
 * the app's. -r serves a root filesystem image as /dev/md0 (the kernel must
 * be 10B500 or the 3GS 7E18, whose gates are in tools/); the image is written
 * to, so pass a working copy, never the only one; with -P, a missing -r
 * image is first made from the pristine one (see make_work_image), for the
 * 7E18 kernel with its activation record unless -a. -w (single-step mode)
 * records, for every kernel thread, the call chain of the last time it
 * blocked (entry to thread_block / thread_block_parameter), and prints each
 * thread's last wait at the end: where a stalled boot is waiting. -F writes
 * the framebuffer Boot_Video describes (n88_framebuffer) as a PPM image at
 * the end, reading each 32-bit pixel's low three octets as blue, green, red.
 * -D x0,y0,x1,y1,t drags one finger across the touchscreen from guest time t,
 * and -T x,y,t taps it at (x,y) (see gesture_tick; up to eight gestures, in
 * the order given). -K name,t[,s] presses the button "hold", "menu", "volup"
 * or "voldown" at guest time t for s seconds (0.15 if not given), or moves
 * the ringer switch to "silent" or "ring" (see press_tick; up to eight). -R s
 * sets the RTC to s seconds since the Unix epoch (default: the host's
 * clock). -S t (up to eight) also writes the screen at guest time t, to the
 * -F path with ".<t>s.ppm" appended (the engine mode only).
 * -A writes what i2s0 plays (n88_set_audio_sink) to a WAV file, 16-bit
 * stereo at the rate it plays at; with or without it, the guest time each
 * sound starts is reported, and the frames played at the end. -C lo,hi (up
 * to four; single-step mode) prints each block in [lo,hi) the first time a
 * branch enters it, and -X addr (up to four; single-step mode) the
 * registers and the r7 backtrace each time a branch reaches addr, the first
 * eight times: what a driver ran, and who called it.
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
#include "audio_capture.h"
#include "file_block.h"
#include "ios3_n88_kernel_patch.h"
#include "ios6_kernel_patch.h"
#include "ksyms.h"
#include "n88.h"
#include "n88_ios3.h"
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

static uint32_t ld32le(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
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

/* ------------------------------------------------------------ screens */
/* The framebuffer (n88_framebuffer) as a PPM image, each 32-bit pixel's low
 * three octets read as blue, green, red. */
static void write_screen(const char *path) {
    const uint8_t *fb = n88_framebuffer(&g_m);
    FILE *f = fb ? fopen(path, "wb") : NULL;
    if (!f) die("cannot write %s", path);
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
    printf("framebuffer written to %s (%" PRIu64 " of %u pixels not black, guest time %.2f s)\n",
           path, lit, N88_FB_WIDTH * N88_FB_HEIGHT, n88_guest_seconds(&g_m));
}

/* -------------------------------------------------------------- touch */
/*
 * -D x0,y0,x1,y1,t: one finger dragged in a straight line, as the touch
 * controller reports it: down at (x0,y0) once t seconds of guest time have
 * passed, DRAG_STEPS - 2 moves, and a lift at (x1,y1), one report per
 * MTZ2_FRAME_PERIOD_MS of guest time. -T x,y,t: a tap, the same with
 * TAP_STEPS reports at one point. Gestures run one after another, in the
 * order given. A report the device cannot take yet (one still unread) is
 * offered again on the next slice. Panel pixels.
 */
#define DRAG_STEPS 24u
#define TAP_STEPS   4u
#define MAX_GESTURES 8u
typedef struct {
    double   x0, y0, x1, y1, t, next;
    unsigned step, steps;
    uint64_t refused;
} gesture_t;
static gesture_t g_gesture[MAX_GESTURES];
static unsigned  g_ngestures;

/* The gesture under way or next, or NULL when all are done. */
static gesture_t *gesture_current(void) {
    for (unsigned i = 0; i < g_ngestures; i++)
        if (g_gesture[i].step < g_gesture[i].steps) return &g_gesture[i];
    return NULL;
}

static void gesture_tick(void) {
    gesture_t *g = gesture_current();
    if (!g) return;
    const double now = n88_guest_seconds(&g_m);
    if (now < g->t || now < g->next) return;
    const unsigned last = g->steps - 1u;
    const double f = (double)g->step / (double)last;
    s5l_mt_contact_t c;
    memset(&c, 0, sizeof c);
    c.id = 1;
    c.x = (uint16_t)(g->x0 + (g->x1 - g->x0) * f + 0.5);
    c.y = (uint16_t)(g->y0 + (g->y1 - g->y0) * f + 0.5);
    c.phase = g->step == 0u ? MTZ2_PHASE_MAKE_TOUCH
            : g->step == last ? MTZ2_PHASE_BREAK_TOUCH : MTZ2_PHASE_TOUCHING;
    c.pressure = g->step == last ? 0u : 160u;
    c.major = c.minor = 10u;
    if (!s5l_mtz2_set_contacts(&g_m.touch, &c, 1)) { g->refused++; return; }
    const char *what = g->steps == TAP_STEPS ? "tap" : "drag";
    if (g->step == 0u) printf("  %s: finger down at %u,%u, guest time %.3f s\n", what, c.x, c.y, now);
    if (g->step == last) printf("  %s: lifted at %u,%u, guest time %.3f s\n", what, c.x, c.y, now);
    g->step++;
    g->next = now + MTZ2_FRAME_PERIOD_MS / 1000.0;
}

/* ----------------------------------------------------------- coverage */
/*
 * -C lo,hi (single-step mode; up to four): print each block in [lo,hi) the
 * first time a branch enters it, with where it came from and r0-r3. What a
 * driver's start ran, and where it stopped, without a breakpoint per guess.
 */
#define MAX_COVER 4u
static struct {
    uint32_t lo, hi;
    uint8_t *seen;              /* one bit per halfword                     */
} g_cover[MAX_COVER];
static unsigned g_ncover;

/* -X addr (single-step mode; up to four): the registers and the r7 backtrace
 * each time a branch reaches addr, the first eight times. Who called it. */
#define MAX_XTRACE 4u
static uint32_t g_xtrace[MAX_XTRACE];
static unsigned g_nxtrace, g_xtrace_hits[MAX_XTRACE];
static void backtrace(const char *indent);

static void cover_note(uint32_t from, uint32_t to) {
    if (to == from + 2u || to == from + 4u) return;
    for (unsigned i = 0; i < g_ncover; i++) {
        if (to < g_cover[i].lo || to >= g_cover[i].hi) continue;
        const uint32_t bit = (to - g_cover[i].lo) >> 1;
        if (g_cover[i].seen[bit >> 3] & (1u << (bit & 7u))) return;
        g_cover[i].seen[bit >> 3] |= (uint8_t)(1u << (bit & 7u));
        printf("  cov %08x %-44s from %08x  r0 %08x r1 %08x r2 %08x r3 %08x  t %.4f\n", to,
               sym(to), from, g_m.cpu.r[0], g_m.cpu.r[1], g_m.cpu.r[2], g_m.cpu.r[3],
               n88_guest_seconds(&g_m));
        return;
    }
}

/* -------------------------------------------------------------- sound */
/*
 * -A out.wav: i2s0's frames as the machine plays them (n88_set_audio_sink),
 * 16-bit stereo at the rate of the first frames. The header is written
 * last, with the length.
 */
static FILE    *g_wav;
static uint32_t g_wav_rate;
static uint64_t g_wav_frames, g_wav_nonzero;
static double   g_wav_heard = -1.0;     /* guest time of the last sound      */

static void wav_header(FILE *f, uint32_t rate, uint64_t frames) {
    const audio_format_t fmt = { rate, 16u, 2u };
    const uint64_t bytes = frames * N88_AUDIO_FRAME_BYTES;
    uint8_t h[WAV_HEADER_BYTES];
    if (!wav_header_build(h, &fmt, bytes > UINT32_MAX - 36u ? UINT32_MAX - 36u : (uint32_t)bytes))
        return;
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, f);
    fseek(f, 0, SEEK_END);
}

/* Every frame i2s0 plays; the guest time a sound starts after a quarter of
 * a second of silence is reported (with or without -A). */
static void wav_sink(void *ctx, const uint32_t *frames, size_t count, uint32_t rate) {
    (void)ctx;
    if (!g_wav_rate) g_wav_rate = rate;
    bool heard = false;
    for (size_t i = 0; i < count; i++) {
        const uint8_t b[4] = { (uint8_t)frames[i], (uint8_t)(frames[i] >> 8),
                               (uint8_t)(frames[i] >> 16), (uint8_t)(frames[i] >> 24) };
        if (g_wav) fwrite(b, 1, 4, g_wav);
        if (frames[i]) { g_wav_nonzero++; heard = true; }
    }
    g_wav_frames += count;
    if (heard) {
        const double now = n88_guest_seconds(&g_m);
        if (g_wav_heard < 0.0 || now - g_wav_heard > 0.25)
            printf("  sound: playing at guest time %.3f s (%u Hz)\n", now, rate);
        g_wav_heard = now;
    }
}

/* ------------------------------------------------------------ buttons */
/*
 * -K name,t[,s]: a button pressed at guest time t and released s seconds
 * later, or the ringer switch moved at t (n88_set_input).
 */
#define MAX_PRESSES 8u
typedef struct {
    n88_input_t in;
    bool        on;             /* the switch: where it goes                */
    bool        is_switch;
    double      t, hold;
    unsigned    state;          /* 0 waiting, 1 pressed, 2 done             */
} press_t;
static press_t  g_press[MAX_PRESSES];
static unsigned g_npresses;

static const char *const input_name[N88_INPUT_COUNT] = {
    "hold", "menu", "volup", "voldown", "silent",
};

static void press_tick(void) {
    const double now = n88_guest_seconds(&g_m);
    for (unsigned i = 0; i < g_npresses; i++) {
        press_t *p = &g_press[i];
        if (p->state == 0u && now >= p->t) {
            n88_set_input(&g_m, p->in, p->on);
            printf("  %s %s, guest time %.3f s\n", p->is_switch ? "ringer switch to" : "pressed",
                   p->is_switch ? (p->on ? "silent" : "ring") : input_name[p->in], now);
            p->state = p->is_switch ? 2u : 1u;
        } else if (p->state == 1u && now >= p->t + p->hold) {
            n88_set_input(&g_m, p->in, false);
            printf("  released %s, guest time %.3f s\n", input_name[p->in], now);
            p->state = 2u;
        }
    }
}

static bool press_held(void) {
    for (unsigned i = 0; i < g_npresses; i++)
        if (g_press[i].state == 1u) return true;
    return false;
}

/* --------------------------------------------------- the working image */
/*
 * -P: the working image, made from the pristine one by n88_ios3_make_work_image
 * (tools/n88_ios3.h, the recipe the app uses too): /etc/fstab mounts / from
 * /dev/md0 (the stock one names the NAND's disk0s1 and disk0s2, which do not
 * exist here), and the volume grows, because Apple ships the root filesystem
 * with no free blocks (on the phone /private/var is a separate partition), and
 * without room the first file the system creates fails (on 10B500,
 * corecrypto's FIPS self-test control file, and launchd reboots).
 *
 * `activate`: also create the lockdown activation record iPhone OS 3's
 * lockdownd reads, so SpringBoard goes past its iTunes activation screen. In
 * the work copy only; for the 7E18 kernel.
 */
static void make_work_image(const char *pristine, const char *work, bool activate) {
    rootfs_work_result_t rr;
    printf("making %s from %s%s\n", work, pristine, activate ? ", activated" : "");
    const rootfs_work_status_t rs =
        n88_ios3_make_work_image(pristine, work, activate, NULL, NULL, &rr);
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
    bool activate = true;
    double shot_at[8];
    unsigned nshots = 0, shots_taken = 0;
    bool engine = false, waits = false;
    unsigned epoch = 0;
    uint64_t rtc = 0;
    bool rtc_given = false;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) budget = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) cmdline = argv[++i];
        else if (!strcmp(argv[i], "-v")) g_verbose = true;
        else if (!strcmp(argv[i], "-e")) engine = true;
        else if (!strcmp(argv[i], "-w")) waits = true;
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) ram_out = argv[++i];
        else if (!strcmp(argv[i], "-F") && i + 1 < argc) screen_out = argv[++i];
        else if (!strcmp(argv[i], "-A") && i + 1 < argc) {
            g_wav = fopen(argv[++i], "wb");
            if (!g_wav) die("-A: cannot write %s", argv[i]);
            wav_header(g_wav, N88_AUDIO_HZ, 0u);
        }
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) root_path = argv[++i];
        else if (!strcmp(argv[i], "-P") && i + 1 < argc) pristine_path = argv[++i];
        else if (!strcmp(argv[i], "-a")) activate = false;
        else if (!strcmp(argv[i], "-D") && i + 1 < argc && g_ngestures < MAX_GESTURES) {
            gesture_t *g = &g_gesture[g_ngestures++];
            if (sscanf(argv[++i], "%lf,%lf,%lf,%lf,%lf", &g->x0, &g->y0, &g->x1, &g->y1,
                       &g->t) != 5)
                die("-D wants x0,y0,x1,y1,t");
            g->steps = DRAG_STEPS;
        }
        else if (!strcmp(argv[i], "-T") && i + 1 < argc && g_ngestures < MAX_GESTURES) {
            gesture_t *g = &g_gesture[g_ngestures++];
            if (sscanf(argv[++i], "%lf,%lf,%lf", &g->x0, &g->y0, &g->t) != 3)
                die("-T wants x,y,t");
            g->x1 = g->x0;
            g->y1 = g->y0;
            g->steps = TAP_STEPS;
        }
        else if (!strcmp(argv[i], "-K") && i + 1 < argc && g_npresses < MAX_PRESSES) {
            press_t *p = &g_press[g_npresses++];
            char name[16];
            p->hold = 0.15;
            if (sscanf(argv[++i], "%15[a-z],%lf,%lf", name, &p->t, &p->hold) < 2)
                die("-K wants name,t[,seconds]");
            p->on = true;
            if (!strcmp(name, "ring")) { p->in = N88_INPUT_SILENT; p->on = false; p->is_switch = true; }
            else if (!strcmp(name, "silent")) { p->in = N88_INPUT_SILENT; p->is_switch = true; }
            else {
                unsigned k = 0;
                while (k < N88_INPUT_SILENT && strcmp(name, input_name[k])) k++;
                if (k == N88_INPUT_SILENT) die("-K: no button %s", name);
                p->in = (n88_input_t)k;
            }
        }
        else if (!strcmp(argv[i], "-C") && i + 1 < argc && g_ncover < MAX_COVER) {
            unsigned long lo = 0, hi = 0;
            if (sscanf(argv[++i], "%lx,%lx", &lo, &hi) != 2 || hi <= lo || hi - lo > 0x1000000u)
                die("-C wants lo,hi in hex, at most 16 MiB apart");
            g_cover[g_ncover].lo = (uint32_t)lo;
            g_cover[g_ncover].hi = (uint32_t)hi;
            g_cover[g_ncover].seen = calloc(1, ((hi - lo) >> 4) + 1u);
            if (!g_cover[g_ncover].seen) die("out of memory");
            g_ncover++;
        }
        else if (!strcmp(argv[i], "-X") && i + 1 < argc && g_nxtrace < MAX_XTRACE)
            g_xtrace[g_nxtrace++] = (uint32_t)strtoul(argv[++i], NULL, 16) & ~1u;
        else if (!strcmp(argv[i], "-R") && i + 1 < argc) {
            rtc = strtoull(argv[++i], NULL, 0);
            rtc_given = true;
        }
        else if (!strcmp(argv[i], "-S") && i + 1 < argc && nshots < 8u)
            shot_at[nshots++] = strtod(argv[++i], NULL);
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
    const bool is_ios3 = !is_ios6 && n88_ios3_identify(kernel, klen);
    if (is_ios3 && !epoch) epoch = IOS3_N88_KERNEL_BOOT_ARGS_VERSION;
    printf("kernel: %s\n", is_ios6 ? "iOS 6.1.6 10B500" : is_ios3 ? "iPhone OS 3.1.3 7E18" : "unrecognised");
    /* 3.1.3's devices that are worse declared and silent than absent
     * (n88_ios3_unmatch). Any -u replaces the whole list. */
    if (is_ios3 && !nunmatch)
        for (; nunmatch < N88_IOS3_UNMATCH_COUNT; nunmatch++)
            unmatch[nunmatch] = n88_ios3_unmatch[nunmatch];

    file_block_t *root_file = NULL;
    const vm_block_t *root = NULL;
    if (root_path) {
        if (!is_ios6 && !is_ios3)
            die("-r needs the iPhone2,1 10B500 or 7E18 kernel (its UUID is neither)");
        FILE *existing = fopen(root_path, "rb");
        if (existing) fclose(existing);
        else if (pristine_path) make_work_image(pristine_path, root_path, is_ios3 && activate);
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
    n88_set_rtc(&g_m, rtc_given ? (uint32_t)rtc : (uint32_t)time(NULL));
    n88_set_audio_sink(&g_m, wav_sink, NULL);
    if (root) {
        guest_patch_report_t pr;
        const guest_patch_status_t ps = is_ios6
            ? ios6_kernel_patch_apply(g_m.ram, N88_DRAM_SIZE, &pr)
            : n88_ios3_patch(&g_m, &pr);
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
            /* While a gesture is under way, slices short enough to pace its
             * reports in guest time. */
            if ((gesture_current() || press_held()) && slice > 1000000u) slice = 1000000u;
            const bool was_asleep = n88_asleep(&g_m);
            const unsigned ran = n88_run(&g_m, (unsigned)slice, &st);
            /* Asleep, a slice is time passing: count it against the budget. */
            n += was_asleep ? slice : ran;
            if (!was_asleep && n88_asleep(&g_m))
                printf("  asleep: the CPU parked for the PMU, guest time %.3f s\n",
                       n88_guest_seconds(&g_m));
            drain_console();
            gesture_tick();
            press_tick();
            if (was_asleep && !n88_asleep(&g_m))
                printf("  awake: resumed through the vector page, guest time %.3f s\n",
                       n88_guest_seconds(&g_m));
            while (shots_taken < nshots && n88_guest_seconds(&g_m) >= shot_at[shots_taken]) {
                char path[512];
                snprintf(path, sizeof path, "%s.%.1fs.ppm", screen_out ? screen_out : "screen",
                         shot_at[shots_taken]);
                write_screen(path);
                shots_taken++;
            }
            if (st != ARM_OK) { why = "the core refused an instruction"; break; }
            if (n % every == 0u) {
                printf("  at %" PRIu64 "M instructions, guest time %.2f s (ttbr0 %08x,"
                       " user sp %08x lr %08x):\n", n / 1000000u, n88_guest_seconds(&g_m),
                       g_m.cpu.cp15.ttbr0, g_m.cpu.bank_r13[ARM_BANK_USR],
                       g_m.cpu.bank_r14[ARM_BANK_USR]);
                backtrace("    ");
            }
        }
    } else {
        for (; n < budget; n++) {
            const uint32_t pc = g_m.cpu.r[15], mode = g_m.cpu.cpsr & 0x1fu;
            if (panic_va && pc == panic_va) report_panic();
            if (waits && (pc == block_va || pc == blockp_va)) note_wait();
            ring[ring_i++ & 63u] = pc;
            if (n88_run(&g_m, 1, &st) != 1u) {
                why = n88_asleep(&g_m) ? "the CPU parked with interrupts masked (asleep)"
                                       : "the core refused an instruction";
                break;
            }
            drain_console();
            const uint32_t npc = g_m.cpu.r[15], nmode = g_m.cpu.cpsr & 0x1fu;
            if (g_ncover) cover_note(pc, npc);
            for (unsigned x = 0; x < g_nxtrace; x++) {
                if (npc != g_xtrace[x] || npc == pc + 2u || npc == pc + 4u ||
                    g_xtrace_hits[x]++ >= 8u)
                    continue;
                printf("  reached %08x %s from %08x, guest time %.4f s\n", npc, sym(npc), pc,
                       n88_guest_seconds(&g_m));
                for (int r = 0; r < 16; r += 4)
                    printf("    r%-2d %08x  r%-2d %08x  r%-2d %08x  r%-2d %08x\n", r,
                           g_m.cpu.r[r], r + 1, g_m.cpu.r[r + 1], r + 2, g_m.cpu.r[r + 2],
                           r + 3, g_m.cpu.r[r + 3]);
                backtrace("    ");
            }
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
    if (g_m.sleeps)
        printf("slept %" PRIu64 " times, woke %" PRIu64 " times%s\n", g_m.sleeps, g_m.wakes,
               n88_asleep(&g_m) ? "; asleep at the end" : "");
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
        {
            const m2clcd_t *lc = &g_m.clcd;
            m2clcd_scanout_t so;
            const bool on = m2clcd_scanout(lc, &so);
            printf("clcd: %" PRIu64 " frames; control %08x, +0x4 %08x; window A", lc->frames,
                   lc->reg[0], lc->reg[1]);
            for (unsigned r = 0x20u; r < 0x40u; r += 4u) printf(" %08x", lc->reg[r / 4u]);
            if (on) printf("; scanout %ux%u, %u bytes a row, %u bpp at %08x", so.width, so.height,
                           so.stride_bytes, so.bpp, so.addr);
            printf("\n");
        }
        {
            const s5l_mtz2_t *t = &g_m.touch;
            printf("spi1: %" PRIu64 " words, %" PRIu64 " tx drops, %" PRIu64 " dma arms; gpioic"
                   " %08x %08x %08x %08x %08x %08x %08x, pad 0x2d0 %08x\n",
                   g_m.spi1.words, g_m.spi1.tx_drops, g_m.spi1.dma_arms,
                   g_m.gpioic_status[0], g_m.gpioic_status[1], g_m.gpioic_status[2],
                   g_m.gpioic_status[3], g_m.gpioic_status[4], g_m.gpioic_status[5],
                   g_m.gpioic_status[6], g_m.gpio[N88_TOUCH_ATN_IRQ]);
            printf("touch: %" PRIu64 " octets, %" PRIu64 " select edges, atn %d, in hbpp %d,"
                   " in reset %d; hbpp: %" PRIu64 " probes, %" PRIu64 " data packets (%" PRIu64
                   " octets), %" PRIu64 " reg reads, %" PRIu64 " reg writes, %" PRIu64
                   " calibs, %" PRIu64 " execs, %" PRIu64 " atn acks; %" PRIu64 " unknown"
                   " (last %02x); frames %" PRIu64 " queued %" PRIu64 " read\n",
                   t->octets, t->select_edges, (int)t->atn, (int)t->hbpp_mode,
                   (int)t->in_reset, t->hbpp_probes, t->hbpp_data_packets, t->hbpp_data_bytes,
                   t->hbpp_reg_reads, t->hbpp_reg_writes, t->hbpp_calibs, t->hbpp_execs,
                   t->hbpp_atn_acks, t->unknown_opcodes, t->last_unknown_op,
                   t->frames_queued, t->frames_read);
            printf("  register log:");
            for (unsigned i = 0; i < t->reg_log_n && i < 16u; i++)
                printf(" %c%08x=%08x", t->reg_log_write[i] ? 'W' : 'R', t->reg_log_addr[i],
                       t->reg_log_val[i]);
            printf("\n  packets:");
            for (unsigned i = 0; i < t->pkt_n && i < 128u; i++)
                printf(" %02x/%u", t->pkt_op[i], t->pkt_len[i]);
            printf("\n");
        }
        for (unsigned i = 0; i < g_ngestures; i++)
            printf("%s %u: %u of %u reports queued, %" PRIu64 " offers refused (device busy,"
                   " in reset, or not yet told it is alive)\n",
                   g_gesture[i].steps == TAP_STEPS ? "tap" : "drag", i, g_gesture[i].step,
                   g_gesture[i].steps, g_gesture[i].refused);
        printf("dsim: %" PRIu64 " packets (%" PRIu64 " payload words), last header %08x\n",
               g_m.dsim.packets, g_m.dsim.payload_words, g_m.dsim.last_header);
        for (unsigned b = 0; b < 2u; b++) {
            const s5l8920_i2c_t *ic = b ? &g_m.i2c2 : &g_m.i2c0;
            printf("i2c%u: %" PRIu64 " transfers, %" PRIu64 " not acknowledged, %" PRIu64
                   " octets out, %" PRIu64 " in\n", b ? 2u : 0u, ic->transfers, ic->naks,
                   ic->bytes_tx, ic->bytes_rx);
        }
        static const char *const rf_name[4] = { "accelerometer", "pmu", "mikey", "codec" };
        for (unsigned d = 0; d < 4u; d++) {
            const i2c_regfile_t *rf = d == 0u ? &g_m.accel : d == 1u ? &g_m.pmu
                                    : d == 2u ? &g_m.mikey : &g_m.codec;
            if (!rf->reads && !rf->writes) continue;
            printf("  %s (0x%02x): %" PRIu64 " reads, %" PRIu64 " writes; registers",
                   rf_name[d], rf->addr, rf->reads, rf->writes);
            for (unsigned r = 0; r < 256u; r++) {
                const bool rd = (rf->read_map[r >> 3] >> (r & 7u)) & 1u;
                const bool wr = (rf->write_map[r >> 3] >> (r & 7u)) & 1u;
                if (rd || wr) printf(" %02x%s%s=%02x", r, rd ? "r" : "", wr ? "w" : "", rf->reg[r]);
            }
            printf("\n");
        }
        if (dm->periph_transfers || dm->periph_unclaimed)
            printf("cdma peripheral: %" PRIu64 " transfers (%" PRIu64 " octets), %" PRIu64
                   " unclaimed; sha1: %" PRIu64 " blocks\n", dm->periph_transfers,
                   dm->periph_octets, dm->periph_unclaimed, g_m.sha1.blocks);
        printf("sound: %" PRIu64 " i2s0 frames played (%" PRIu64 " not silent), %" PRIu64
               " with nothing to play, at %u Hz; %" PRIu64 " octets through paced channels\n",
               g_m.audio_frames, g_wav_nonzero, g_m.audio_silent, n88_audio_rate(&g_m),
               dm->paced_octets);
        for (unsigned n = 1; n < CDMA_CHANNELS; n++) {
            const uint32_t *c = dm->ch[n];
            if (!c[0] && !c[CDMA_CAR / 4u]) continue;
            printf("  cdma channel %u: csr %08x +4 %08x dar %08x dbr %08x car %08x err %08x\n",
                   n, c[0], c[1], c[2], c[3], c[CDMA_CAR / 4u], c[CDMA_ERR / 4u]);
            uint32_t desc = c[CDMA_CAR / 4u];
            for (unsigned k = 0; k < 4u && in_ram(desc, 32u); k++) {
                const uint8_t *q = g_m.ram + (desc - N88_DRAM_BASE);
                printf("    desc %08x:", desc);
                for (unsigned w = 0; w < 8u; w++) printf(" %08x", ld32le(q + 4u * w));
                printf("\n");
                desc = ld32le(q);
            }
        }
    }
    if (g_wav) {
        wav_header(g_wav, g_wav_rate ? g_wav_rate : N88_AUDIO_HZ, g_wav_frames);
        fclose(g_wav);
        g_wav = NULL;
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
    if (screen_out) write_screen(screen_out);   /* -F: the framebuffer as a PPM */
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
