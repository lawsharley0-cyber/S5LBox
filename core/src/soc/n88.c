/*
 * NEON — the iPhone 3GS (N88AP, S5L8920) machine. See n88.h.
 *
 * Every register modelled here is one the iOS 6.1.6 (10B500) kernel was seen
 * to use under tools/boot3gs, and the comments cite the kernel code that
 * fixed its behaviour. Addresses are the 10B500 kernelcache's.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "n88.h"

#include "devicetree.h"
#include "macho.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(N88_CPU_HZ % N88_TB_HZ == 0, "the timebase must divide the CPU clock");

static void st32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t ld32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static inline bool in_ram(uint32_t a, unsigned n) {
    return a >= N88_DRAM_BASE &&
           (uint64_t)a + n <= (uint64_t)N88_DRAM_BASE + N88_DRAM_SIZE;
}

/* ------------------------------------------------------------ console */

static void console_put(n88_t *m, char ch) {
    m->console_total++;
    if (m->console_len == N88_CONSOLE_CAPACITY) {     /* drop the oldest */
        m->console_head = (m->console_head + 1u) % N88_CONSOLE_CAPACITY;
        m->console_len--;
        m->console_dropped++;
    }
    m->console[(m->console_head + m->console_len) % N88_CONSOLE_CAPACITY] = ch;
    m->console_len++;
}

size_t n88_console_take(n88_t *m, char *out, size_t capacity) {
    if (!m || !out) return 0;
    size_t n = 0;
    while (n < capacity && m->console_len) {
        out[n++] = m->console[m->console_head];
        m->console_head = (m->console_head + 1u) % N88_CONSOLE_CAPACITY;
        m->console_len--;
    }
    return n;
}

/* -------------------------------------------------------------- timer */
/*
 * The PMGR timer (/arm-io/pmgr is device_type "timer"; reg 0x3f100000 ->
 * physical 0xbf100000), as the kernel's own code uses it:
 *   +0x200/+0x204  a 64-bit count, read high, low, high again until the two
 *                  highs agree (0x800895a4);
 *   +0x208         a decrementer: written with an interval in ticks
 *                  (0x800895dc; the first is 0x3a975 = 240,000 = 10 ms at
 *                  24 MHz) and read back as what remains (0x800895d0);
 *   +0x220         control: bit 0 enables the interrupt; writing bit 1
 *                  acknowledges it. The FIQ fast path (the handler at
 *                  0x8008958c, reached through the FIQ vector's "mov pc, r9")
 *                  writes the control value and then the same with bit 1
 *                  clear, and init writes 3 then 1 the same way.
 * The interrupt is VIC0 line 6, which the kernel selects as FIQ (0x8027b4f8).
 *
 * The count is the retired-cycle count divided exactly, so it can never wrap
 * or step back: cycles * TB_HZ overflows 64 bits once idle skipping has
 * pushed the cycle count past ~7.7e11 (it did, 1,281 guest seconds into a
 * harness run).
 */

uint64_t n88_timer_count(const n88_t *m) {
    return m ? m->cpu.cycles / N88_CYCLES_PER_TICK : 0u;
}

double n88_guest_seconds(const n88_t *m) {
    return (double)n88_timer_count(m) / (double)N88_TB_HZ;
}

static void irq_update(n88_t *m) {
    bool irq = false, fiq = false;
    for (unsigned i = 0; i < N88_VIC_COUNT; i++) {
        irq |= s5l_vic_irq(&m->vic[i]);
        fiq |= s5l_vic_fiq(&m->vic[i]);
    }
    m->cpu.irq_line = irq;
    m->cpu.fiq_line = fiq;
}

/* Expire the decrementer if its interval has passed, and drive line 6. */
static void timer_update(n88_t *m) {
    if (m->timer.armed &&
        n88_timer_count(m) - m->timer.start >= m->timer.interval) {
        m->timer.armed = false;
        m->timer.pending = true;
        m->timer.fired++;
    }
    s5l_vic_set_line(&m->vic[0], N88_TIMER_LINE,
                     m->timer.pending && (m->timer.ctrl & 1u));
    irq_update(m);
}

/* The first cycle at which the decrementer expires, or UINT64_MAX. */
static uint64_t timer_due_cycles(const n88_t *m) {
    if (!m->timer.armed) return UINT64_MAX;
    return (m->timer.start + m->timer.interval) * N88_CYCLES_PER_TICK;
}

/* ---------------------------------------------------------------- bus */

static bool in_timer(uint32_t pa) {
    return pa >= N88_TIMER_PA && pa < N88_TIMER_PA + 0x40u;
}

static bool in_vic(uint32_t pa) {
    return pa >= N88_VIC_PA && pa < N88_VIC_PA + N88_VIC_COUNT * 0x10000u;
}

static bool in_gpio(uint32_t pa) {
    return pa >= N88_GPIO_PA && pa < N88_GPIO_PA + N88_GPIO_SIZE;
}

static uint32_t mmio_read(n88_t *m, uint32_t pa, unsigned size) {
    if (in_timer(pa)) {
        /* Reading the timer changes no level, so it does not stop the cached
         * interpreter: the kernel reads the count on every
         * mach_absolute_time(). */
        m->mmio++;
        const uint64_t now = n88_timer_count(m);
        switch (pa - N88_TIMER_PA) {
        case 0x00: return (uint32_t)now;
        case 0x04: return (uint32_t)(now >> 32);
        case 0x08: {
            const uint64_t gone = now - m->timer.start;
            return m->timer.armed && gone < m->timer.interval
                 ? m->timer.interval - (uint32_t)gone : 0u;
        }
        case 0x20: return m->timer.ctrl;
        default:   break;
        }
    }
    if (in_gpio(pa)) {
        /* A pure register-file read: the value the CPU last wrote to this
         * pin's config, reset 0. It drives no interrupt line here, so, like
         * the timer count, it does not stop the cached interpreter. */
        m->mmio++;
        return m->gpio[(pa - N88_GPIO_PA) >> 2];
    }
    m->level_dirty = true;
    if (pa == N88_UART0_PA + 0x10u) {           /* UTRSTAT: transmitter empty */
        m->mmio++;
        return 0x6u;
    }
    if (in_vic(pa)) {
        m->mmio++;
        const unsigned n = (pa - N88_VIC_PA) >> 16;
        const uint32_t off = pa & 0xffffu;
        return off == VIC_VECTADDR ? s5l_vic_vectaddr(&m->vic[n], 32u * n)
                                   : s5l_vic_read(&m->vic[n], off);
    }
    m->unmodelled++;
    if (m->trace) m->trace(m->trace_ctx, pa, size, false, 0u, m->cpu.r[15]);
    return 0u;
}

static void mmio_write(n88_t *m, uint32_t pa, unsigned size, uint32_t v) {
    m->level_dirty = true;
    bool modelled = true;
    if (pa == N88_UART0_PA + 0x20u) {           /* UTXH */
        console_put(m, (char)(v & 0xffu));
    } else if (pa == N88_UART0_PA || pa == N88_UART0_PA + 0x04u ||
               pa == N88_UART0_PA + 0x08u || pa == N88_UART0_PA + 0x0cu ||
               pa == N88_UART0_PA + 0x28u || pa == N88_UART0_PA + 0x2cu) {
        /* ULCON, UCON, UFCON, UMCON, UBRDIV, UDIVSLOT: line setup a polled
         * console does not need, accepted and ignored. */
    } else if (pa == N88_TIMER_PA + 0x08u) {
        m->timer.start = n88_timer_count(m);
        m->timer.interval = v;
        m->timer.armed = true;
    } else if (pa == N88_TIMER_PA + 0x20u) {
        if (v & 2u) m->timer.pending = false;     /* acknowledge */
        m->timer.ctrl = v & 1u;
    } else if (in_vic(pa)) {
        s5l_vic_write(&m->vic[(pa - N88_VIC_PA) >> 16], pa & 0xffffu, v);
    } else if (in_gpio(pa)) {
        m->gpio[(pa - N88_GPIO_PA) >> 2] = v;   /* stored, read back verbatim */
    } else {
        modelled = false;
    }
    if (modelled) m->mmio++;
    else {
        m->unmodelled++;
        if (m->trace) m->trace(m->trace_ctx, pa, size, true, v, m->cpu.r[15]);
    }
    timer_update(m);
}

static uint32_t b_r32(void *c, uint32_t a) {
    n88_t *m = c;
    if (in_ram(a, 4)) { uint32_t v; memcpy(&v, m->ram + (a - N88_DRAM_BASE), 4); return v; }
    return mmio_read(m, a, 4);
}
static uint16_t b_r16(void *c, uint32_t a) {
    n88_t *m = c;
    if (in_ram(a, 2)) { uint16_t v; memcpy(&v, m->ram + (a - N88_DRAM_BASE), 2); return v; }
    return (uint16_t)mmio_read(m, a, 2);
}
static uint8_t b_r8(void *c, uint32_t a) {
    n88_t *m = c;
    if (in_ram(a, 1)) return m->ram[a - N88_DRAM_BASE];
    return (uint8_t)mmio_read(m, a, 1);
}
static void b_w32(void *c, uint32_t a, uint32_t v) {
    n88_t *m = c;
    if (in_ram(a, 4)) {
        memcpy(m->ram + (a - N88_DRAM_BASE), &v, 4);
        if (m->ci) arm_ci_note_ram_write(m->ci, a, 4);
        return;
    }
    mmio_write(m, a, 4, v);
}
static void b_w16(void *c, uint32_t a, uint16_t v) {
    n88_t *m = c;
    if (in_ram(a, 2)) {
        memcpy(m->ram + (a - N88_DRAM_BASE), &v, 2);
        if (m->ci) arm_ci_note_ram_write(m->ci, a, 2);
        return;
    }
    mmio_write(m, a, 2, v);
}
static void b_w8(void *c, uint32_t a, uint8_t v) {
    n88_t *m = c;
    if (in_ram(a, 1)) {
        m->ram[a - N88_DRAM_BASE] = v;
        if (m->ci) arm_ci_note_ram_write(m->ci, a, 1);
        return;
    }
    mmio_write(m, a, 1, v);
}
static uint8_t *b_host(void *c, uint32_t a, uint32_t n) {
    n88_t *m = c;
    return in_ram(a, n) ? m->ram + (a - N88_DRAM_BASE) : NULL;
}
/* Direct stores are fine, except into a range holding cached-interpreter
 * code: every store there must pass b_w*, which is where the cache learns of
 * it (the same refusal as the iPhone OS 3 machine's). */
static uint8_t *b_host_write(void *c, uint32_t a, uint32_t n) {
    n88_t *m = c;
    if (m->ci && arm_ci_range_has_code(m->ci, a, n)) return NULL;
    return b_host(c, a, n);
}
/* WFI: nothing else runs, so time jumps to the decrementer's expiry instead
 * of the host spinning through an idle guest. */
static bool b_wfi(void *c) {
    n88_t *m = c;
    m->wfi++;
    if (m->timer.armed && (m->timer.ctrl & 1u)) {
        const uint64_t due = timer_due_cycles(m);
        if (due > m->cpu.cycles) m->cpu.cycles = due;
    }
    timer_update(m);
    return m->cpu.irq_line || m->cpu.fiq_line;
}

uint32_t n88_read32(n88_t *m, uint32_t pa) { return b_r32(m, pa); }
void n88_write32(n88_t *m, uint32_t pa, uint32_t v) { b_w32(m, pa, v); }

/* ---------------------------------------------------------- lifecycle */

bool n88_init(n88_t *m, bool cached_engine) {
    if (!m) return false;
    memset(m, 0, sizeof *m);
    m->ram = calloc(1, N88_DRAM_SIZE);
    if (!m->ram) return false;
    m->bus = (arm_bus_t){
        .ctx = m,
        .read32 = b_r32, .read16 = b_r16, .read8 = b_r8,
        .write32 = b_w32, .write16 = b_w16, .write8 = b_w8,
        .host_ram = b_host, .host_ram_write = b_host_write,
        .wait_for_interrupt = b_wfi,
    };
    for (unsigned i = 0; i < N88_VIC_COUNT; i++) s5l_vic_reset(&m->vic[i]);
    m->cpu.arch = ARM_ARCH_V7_A8;
    arm_reset(&m->cpu, &m->bus);
    if (cached_engine) {
        const arm_ci_config_t cfg = {
            .ram = m->ram, .ram_base = N88_DRAM_BASE, .ram_size = N88_DRAM_SIZE,
            .level_dirty = &m->level_dirty,
        };
        m->ci = arm_ci_create(&cfg);
        if (!m->ci) { n88_free(m); return false; }
    }
    return true;
}

void n88_free(n88_t *m) {
    if (!m) return;
    arm_ci_destroy(m->ci);
    m->ci = NULL;
    free(m->ram);
    m->ram = NULL;
}

/* ---------------------------------------------------------------- run */

unsigned n88_run(n88_t *m, unsigned max_steps, arm_status_t *status) {
    arm_status_t st = ARM_OK;
    unsigned n = 0;
    if (!m || !m->ram) {
        if (status) *status = ARM_HALT;
        return 0;
    }
    /* Every level change a device access makes is applied by mmio_write()
     * as it happens; the only one time makes is the decrementer expiring. */
    timer_update(m);
    while (n < max_steps) {
        if (m->timer.armed) timer_update(m);
        if (m->ci) {
            /* Never past the decrementer's expiry, so the timer line moves
             * at exactly the instruction it would under arm_step. */
            unsigned budget = max_steps - n;
            const uint64_t due = timer_due_cycles(m);
            if (due != UINT64_MAX) {
                const uint64_t left = due > m->cpu.cycles ? due - m->cpu.cycles : 0u;
                if (left < budget) budget = (unsigned)left;
            }
            if (budget) {
                arm_ci_stop_t why = ARM_CI_STOP_BUDGET;
                arm_status_t est = ARM_OK;
                m->level_dirty = false;
                n += arm_ci_run(m->ci, &m->cpu, budget, &est, &why);
                if (est != ARM_OK) { st = est; break; }
                if (why != ARM_CI_STOP_STEP) continue;
            }
        }
        st = arm_step(&m->cpu);
        if (st != ARM_OK) break;
        n++;
    }
    if (status) *status = st;
    return n;
}

/* ---------------------------------------------------------- bring-up */

const char *n88_strerror(n88_status_t st) {
    switch (st) {
    case N88_OK:             return "ok";
    case N88_ERR_ARGUMENT:   return "invalid argument";
    case N88_ERR_MEMORY:     return "out of memory";
    case N88_ERR_KERNEL:     return "the kernelcache is not an ARM Mach-O this machine can map";
    case N88_ERR_DEVICETREE: return "the device tree is not one bring-up can complete";
    case N88_ERR_LAYOUT:     return "the kernel, device tree and boot_args do not fit in DRAM";
    case N88_ERR_ROOT:       return "the root filesystem cannot be attached";
    }
    return "unknown error";
}

static n88_status_t fail(n88_status_t st, char *detail, size_t cap, const char *fmt, ...) {
    if (detail && cap) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(detail, cap, fmt, ap);
        va_end(ap);
    }
    return st;
}

/* A writable pointer to property `prop` of node `path` in the RAM copy of the
 * tree, and its length, or NULL. */
static uint8_t *dt_prop_rw(uint8_t *tree, const dt_t *dt, const dt_node_t *root,
                           const char *path, const char *prop, uint32_t *len) {
    dt_node_t node;
    const uint8_t *v = NULL;
    if (*path) {
        if (dt_path(dt, root, path, &node) != DT_OK) return NULL;
    } else {
        node = *root;
    }
    if (dt_property(dt, &node, prop, &v, len) != DT_OK || !v) return NULL;
    return tree + (size_t)(v - dt->blob);
}

static bool dt_set_words(uint8_t *tree, const dt_t *dt, const dt_node_t *root,
                         const char *path, const char *prop,
                         const uint32_t *words, uint32_t count) {
    uint32_t l = 0;
    uint8_t *p = dt_prop_rw(tree, dt, root, path, prop, &l);
    if (!p || l != 4u * count) return false;
    for (uint32_t i = 0; i < count; i++) st32(p + 4u * i, words[i]);
    return true;
}

/* Claim a MemoryMapReserved-* placeholder in /chosen/memory-map for `key`, as
 * iBoot does (core/src/boot/bringup.c's dt_memmap_add explains the
 * mechanism): rename it and give it {pa, size}. */
static bool dt_memmap(uint8_t *tree, const dt_t *dt, const dt_node_t *root,
                      const char *key, uint32_t pa, uint32_t size) {
    dt_node_t mm;
    if (dt_path(dt, root, "chosen/memory-map", &mm) != DT_OK) return false;
    size_t off = mm.props_off;          /* dt_parse validated every extent */
    for (uint32_t i = 0; i < mm.n_props; i++) {
        const uint32_t l = ld32(tree + off + 32u) & 0x7fffffffu;
        if (l == 8u && strncmp((const char *)tree + off, "MemoryMapReserved-", 18) == 0 &&
            ld32(tree + off + 36u) == 0u && ld32(tree + off + 40u) == 0u) {
            memset(tree + off, 0, DT_PROP_NAME_LEN);
            memcpy(tree + off, key, strlen(key));
            st32(tree + off + 36u, pa);
            st32(tree + off + 40u, size);
            return true;
        }
        off += 36u + ((l + 3u) & ~3u);
    }
    return false;
}

/*
 * /chosen/nvram-proxy-data: the NVRAM image iBoot hands the kernel. The
 * template's is 8 KB of zeros, and IODTNVRAM's partition walk (0x80267298)
 * adds each header's length (a u16 at +2, in 16-byte blocks, read
 * little-endian with ldrh) to its offset, so a zero header never advances and
 * the walk never ends. This writes the smallest image that parses: a "common"
 * partition (signature 0x70) holding no variables, and the rest as the
 * free-space partition (0x7f, "wwwwwwwwwwww"), each header with the CHRP
 * checksum over its signature, length and name.
 */
static void nvram_partition(uint8_t *h, uint8_t sig, uint16_t blocks, const char *name) {
    memset(h, 0, 16);
    h[0] = sig;
    h[2] = (uint8_t)blocks;
    h[3] = (uint8_t)(blocks >> 8);
    const size_t n = strlen(name);
    memcpy(h + 4, name, n < 12u ? n : 12u);
    unsigned sum = h[0];
    for (int i = 2; i < 16; i++) {
        sum += h[i];
        if (sum > 0xffu) sum = (sum & 0xffu) + 1u;
    }
    h[1] = (uint8_t)sum;
}

void n88_nvram_image(uint8_t *img, uint32_t len) {
    const uint32_t common = 0x800u;
    if (!img) return;
    memset(img, 0, len);
    if (len < common + 16u) return;
    nvram_partition(img, 0x70, (uint16_t)(common / 16u), "common");
    nvram_partition(img + common, 0x7f, (uint16_t)((len - common) / 16u), "wwwwwwwwwwww");
}

bool n88_devicetree_is_3gs(const uint8_t *blob, size_t len) {
    dt_t dt;
    dt_node_t root;
    const uint8_t *v = NULL;
    uint32_t l = 0;
    if (!blob || dt_parse(blob, len, &dt, &root) != DT_OK) return false;
    if (dt_property(&dt, &root, "compatible", &v, &l) != DT_OK || !v) return false;
    /* A list of NUL-terminated strings; any one of them. */
    for (uint32_t i = 0; i < l;) {
        const char *s = (const char *)v + i;
        const size_t n = strnlen(s, l - i);
        if (n == 5u && memcmp(s, "N88AP", 5) == 0) return true;
        i += (uint32_t)n + 1u;
    }
    return false;
}

/* The bridge writes guest RAM directly; code the cached interpreter holds
 * there must be re-read. */
static void n88_ram_written(void *ctx, uint64_t pa, uint64_t len) {
    n88_t *m = ctx;
    if (m && m->ci && pa <= UINT32_MAX)
        arm_ci_note_ram_write(m->ci, (uint32_t)pa,
                              len > UINT32_MAX ? UINT32_MAX : (uint32_t)len);
}

/*
 * /arm-io's clock-frequencies: one frequency per PMGR clock, which the
 * kernel's AppleS5L8920XIO reads (it refuses a property shorter than these 28
 * words, 0x80787ab8 in 10B500) and hands to every driver that asks for a
 * clock by name: a device's clock-ids entry 0x100 + n selects entry n, and
 * the entry answers the names listed for it (spi0-2 and uart0-4 take "pclk"
 * from entry 4). A driver that gets 0 back gives up; the SPI controller did.
 *
 * Entries 0-24 are LLB's clock registers as iBoot recomputes them (see
 * n88.h): each divides one source -- PLL1, PLL2, the 24 MHz reference, or
 * source 0, which is PLL0 for clock 0 and clock 0's output for the others --
 * and a clock whose divider LLB leaves disabled reads 0 (11, 13, 21), as it
 * does on the phone.
 * Entry 10 divides the reference by a product of two fields. Entry 15 is the
 * CPU (PLL0 undivided), 25 the memory clock (the CPU / 3), 26 the timebase
 * and 27 the USB PHY. Entry 22 is the display's pixel clock at LLB's
 * default; iBoot's display driver retunes it for the panel (0x4ff06760),
 * which runs only with a display, not modelled here.
 */
const uint32_t n88_clock_frequencies[N88_CLOCK_COUNT] = {
    150000000u, 100000000u, 100000000u,  81000000u,     /*  0 -  3 */
    100000000u, 100000000u,  54000000u, 200000000u,     /*  4 -  7 */
    150000000u, 100000000u,     50000u,         0u,     /*  8 - 11 */
    150000000u,         0u,  40500000u, 600000000u,     /* 12 - 15 */
      1000000u,   1000000u,   1000000u,  24000000u,     /* 16 - 19 */
     40500000u,         0u,  10800000u,  54000000u,     /* 20 - 23 */
    162000000u, 200000000u,  24000000u,  24000000u,     /* 24 - 27 */
};

n88_status_t n88_boot(n88_t *m, const n88_boot_t *req, char *detail, size_t cap) {
    if (detail && cap) detail[0] = 0;
    if (!m || !m->ram || !req || !req->kernel || !req->devicetree)
        return fail(N88_ERR_ARGUMENT, detail, cap, "missing kernel or device tree");
    const char *cmdline = req->cmdline ? req->cmdline
                        : req->root ? N88_ROOT_CMDLINE : N88_DEFAULT_CMDLINE;
    vm_block_info_t root_info = {0};
    if (req->root) {
        if (vm_block_get_info(req->root, &root_info) != VM_BLOCK_STATUS_OK ||
            root_info.size == 0 || (root_info.size & 0xfffu) != 0 ||
            root_info.size > (uint64_t)UINT32_MAX + 1u - N88_MD_TOKEN_PA)
            return fail(N88_ERR_ROOT, detail, cap,
                        "the root filesystem must be whole 4 KiB pages and at "
                        "most %llu bytes",
                        (unsigned long long)((uint64_t)UINT32_MAX + 1u - N88_MD_TOKEN_PA));
        if (!req->md_read_site_pc || !req->md_write_site_pc)
            return fail(N88_ERR_ROOT, detail, cap,
                        "a root filesystem needs the kernel's two patched copy sites");
    }
    if (strlen(cmdline) > 255u)
        return fail(N88_ERR_ARGUMENT, detail, cap, "boot-args longer than 255 bytes");

    macho_t k;
    const macho_status_t ms = macho_parse(req->kernel, req->kernel_size, &k);
    if (ms != MACHO_OK)
        return fail(N88_ERR_KERNEL, detail, cap, "kernelcache: %s", macho_strerror(ms));
    if (!k.has_entry || k.vm_low < N88_VIRT_BASE || k.vm_high <= k.vm_low)
        return fail(N88_ERR_KERNEL, detail, cap,
                    "kernelcache: no entry point, or linked below 0x80000000");

    /* Where everything goes: the kernel at its link address, then the tree,
     * boot_args and topOfKernelData, 16 KiB aligned because the kernel builds
     * its first-level translation table there. */
    const uint64_t kernel_end = (uint64_t)k.vm_high - N88_VIRT_BASE + N88_DRAM_BASE;
    const uint64_t tree_pa = (kernel_end + 0xfffu) & ~(uint64_t)0xfffu;
    const uint64_t args_pa = (tree_pa + req->devicetree_size + 0xfffu) & ~(uint64_t)0xfffu;
    const uint64_t tokd_pa = (args_pa + 0x1000u + 0x3fffu) & ~(uint64_t)0x3fffu;
    const uint64_t usable_end = (uint64_t)N88_DRAM_BASE + N88_DRAM_SIZE - N88_TOP_RESERVE;
    if (req->devicetree_size > N88_DRAM_SIZE || tokd_pa >= usable_end)
        return fail(N88_ERR_LAYOUT, detail, cap,
                    "the kernel ends at %08llx and does not leave room below %08llx",
                    (unsigned long long)kernel_end, (unsigned long long)usable_end);

    /* The segments. DRAM is zero from n88_init; only a second boot must
     * clear it (clearing it always would commit all 256 MB up front). */
    if (m->booted) memset(m->ram, 0, N88_DRAM_SIZE);
    m->booted = false;
    for (unsigned i = 0; i < k.segment_count; i++) {
        const macho_segment_t *s = &k.segments[i];
        if (!s->vmsize) continue;
        if (s->vmaddr < N88_VIRT_BASE)
            return fail(N88_ERR_KERNEL, detail, cap, "segment %s below 0x80000000", s->name);
        const uint32_t pa = s->vmaddr - N88_VIRT_BASE + N88_DRAM_BASE;
        if (!in_ram(pa, 1) || (uint64_t)pa + s->vmsize > usable_end)
            return fail(N88_ERR_LAYOUT, detail, cap, "segment %s outside DRAM", s->name);
        const uint32_t n = s->filesize < s->vmsize ? s->filesize : s->vmsize;
        if (n) {
            if ((uint64_t)s->fileoff + n > req->kernel_size)
                return fail(N88_ERR_KERNEL, detail, cap, "segment %s past the file", s->name);
            memcpy(m->ram + (pa - N88_DRAM_BASE), req->kernel + s->fileoff, n);
        }
    }

    /* The device tree, filled in as iBoot would. */
    uint8_t *tree = m->ram + ((uint32_t)tree_pa - N88_DRAM_BASE);
    memcpy(tree, req->devicetree, req->devicetree_size);
    dt_t dt;
    dt_node_t root;
    const dt_status_t ds = dt_parse(tree, req->devicetree_size, &dt, &root);
    if (ds != DT_OK)
        return fail(N88_ERR_DEVICETREE, detail, cap, "device tree: %s", dt_strerror(ds));
    const uint32_t mem[2] = { N88_DRAM_BASE, N88_DRAM_SIZE };
    const uint32_t pram[2] = { N88_DRAM_BASE + N88_DRAM_SIZE - N88_TOP_RESERVE, N88_PRAM_SIZE };
    static const struct { const char *path, *prop; uint32_t v; } clocks[] = {
        { "",          "clock-frequency",      N88_BUS_HZ },
        { "cpus/cpu0", "timebase-frequency",   N88_TB_HZ  },
        { "cpus/cpu0", "clock-frequency",      N88_CPU_HZ },
        { "cpus/cpu0", "bus-frequency",        N88_BUS_HZ },
        { "cpus/cpu0", "memory-frequency",     N88_MEM_HZ },
        { "cpus/cpu0", "peripheral-frequency", N88_PRF_HZ },
        { "cpus/cpu0", "fixed-frequency",      N88_FIX_HZ },
    };
    if (!dt_set_words(tree, &dt, &root, "memory", "reg", mem, 2))
        return fail(N88_ERR_DEVICETREE, detail, cap, "device tree: no 8-byte /memory:reg");
    if (!dt_set_words(tree, &dt, &root, "pram", "reg", pram, 2))
        return fail(N88_ERR_DEVICETREE, detail, cap, "device tree: no 8-byte /pram:reg");
    for (size_t i = 0; i < sizeof clocks / sizeof clocks[0]; i++)
        if (!dt_set_words(tree, &dt, &root, clocks[i].path, clocks[i].prop, &clocks[i].v, 1))
            return fail(N88_ERR_DEVICETREE, detail, cap, "device tree: no 4-byte /%s:%s",
                        clocks[i].path, clocks[i].prop);
    {
        /* The per-clock table and the two single clocks iBoot writes beside
         * it. Like iBoot, copy as many of the 28 words as the property holds
         * and pass over a property the tree does not have. */
        uint32_t cl = 0;
        uint8_t *cf = dt_prop_rw(tree, &dt, &root, "arm-io", "clock-frequencies", &cl);
        const uint32_t words = cf ? (cl / 4u < N88_CLOCK_COUNT ? cl / 4u : N88_CLOCK_COUNT) : 0u;
        for (uint32_t i = 0; i < words; i++) st32(cf + 4u * i, n88_clock_frequencies[i]);
        const uint32_t usbphy = N88_USBPHY_HZ, ncoref = N88_NCOREF_HZ;
        (void)dt_set_words(tree, &dt, &root, "arm-io", "usbphy-frequency", &usbphy, 1);
        (void)dt_set_words(tree, &dt, &root, "arm-io/audio-complex", "ncoref-frequency",
                           &ncoref, 1);
    }
    {
        uint32_t nl = 0;
        uint8_t *nv = dt_prop_rw(tree, &dt, &root, "chosen", "nvram-proxy-data", &nl);
        if (!nv || nl < 0x1000u)
            return fail(N88_ERR_DEVICETREE, detail, cap,
                        "device tree: no /chosen:nvram-proxy-data of at least 4 KB");
        n88_nvram_image(nv, nl);
    }
    /* Un-match: an 'x' over the first byte of the node's compatible, so no
     * driver claims it (core/src/boot/bringup.c does the same). */
    static const char *const default_unmatch[] = { "arm-io/iop" };
    const char *const *um = req->unmatch ? req->unmatch : default_unmatch;
    const unsigned un = req->unmatch ? req->unmatch_count : 1u;
    for (unsigned i = 0; i < un; i++) {
        uint32_t cl = 0;
        uint8_t *compat = um[i] ? dt_prop_rw(tree, &dt, &root, um[i], "compatible", &cl) : NULL;
        if (!compat || !cl)
            return fail(N88_ERR_DEVICETREE, detail, cap, "device tree: cannot un-match /%s",
                        um[i] ? um[i] : "(null)");
        compat[0] = 'x';
    }
    if (!dt_memmap(tree, &dt, &root, "DeviceTree", (uint32_t)tree_pa,
                   (uint32_t)req->devicetree_size) ||
        !dt_memmap(tree, &dt, &root, "BootArgs", (uint32_t)args_pa, 0x1000u))
        return fail(N88_ERR_DEVICETREE, detail, cap,
                    "device tree: no free /chosen/memory-map placeholder");

    /* The root filesystem: the RAMDisk entry IOFindBSDRoot turns into md0,
     * and the bridge that answers the copies from its token address. */
    arm_bus_set_privileged_svc_handler(&m->bus, NULL, NULL);
    m->has_root = false;
    m->root_size = 0;
    if (req->root) {
        if (!dt_memmap(tree, &dt, &root, "RAMDisk", N88_MD_TOKEN_PA,
                       (uint32_t)root_info.size))
            return fail(N88_ERR_DEVICETREE, detail, cap,
                        "device tree: no free /chosen/memory-map placeholder "
                        "for the RAMDisk entry");
        md_bridge_config_t mc;
        memset(&mc, 0, sizeof mc);
        mc.read_site.pc = req->md_read_site_pc;
        mc.read_site.encoding = N88_SVC_MD_READ;
        mc.write_site.pc = req->md_write_site_pc;
        mc.write_site.encoding = N88_SVC_MD_WRITE;
        mc.token_base = N88_MD_TOKEN_PA;
        mc.media_size = root_info.size;
        mc.ram_base = N88_DRAM_BASE;
        mc.ram_size = N88_DRAM_SIZE;
        mc.ram = m->ram;
        mc.block = req->root;
        mc.ram_written = n88_ram_written;
        mc.ram_written_context = m;
        if (!md_bridge_config_valid(&mc))
            return fail(N88_ERR_ROOT, detail, cap,
                        "the memory-disk bridge refused the geometry");
        /* The root's "secure-root-prefix" ("md" on the 3GS) makes
         * AppleARMPlatform's SecureRootName handler (IOSecureBSDRoot, once
         * the root is chosen) wait for a SecureRoot call from the storage
         * stack -- which this machine does not have, so the boot thread
         * would sleep there for ever (10B500: 0x804b0596). Without the
         * property the platform treats the root as unchecked and answers at
         * once; strike the name out with an 'x' like an un-matched
         * compatible. */
        uint32_t spl = 0;
        const uint8_t *spv = NULL;
        if (dt_property(&dt, &root, "secure-root-prefix", &spv, &spl) == DT_OK && spv)
            tree[(size_t)(spv - dt.blob) - 36u] = 'x';
        md_bridge_init(&m->md, &mc);
        arm_bus_set_privileged_svc_handler(&m->bus, md_bridge_handle_svc, &m->md);
        m->has_root = true;
        m->root_size = root_info.size;
    }

    /* boot_args (the layout core/src/boot/bringup.c documents). */
    uint8_t *ba = m->ram + ((uint32_t)args_pa - N88_DRAM_BASE);
    ba[0] = 1; ba[1] = 0;                       /* Revision */
    /* Version 5: pe_identify_machine (0x8027ace0) loads boot_args+2 and
     * panics "Epoch Mismatch" unless it is 5. (iPhone OS 3's wants 6.) */
    ba[2] = 5; ba[3] = 0;
    st32(ba + 0x04, N88_VIRT_BASE);
    st32(ba + 0x08, N88_DRAM_BASE);
    st32(ba + 0x0c, N88_DRAM_SIZE - N88_TOP_RESERVE);   /* the top is boot-owned */
    st32(ba + 0x10, (uint32_t)tokd_pa);
    /* Boot_Video at 0x14..0x2f stays zero: no framebuffer yet. */
    st32(ba + 0x30, (uint32_t)tree_pa - N88_DRAM_BASE + N88_VIRT_BASE);
    st32(ba + 0x34, (uint32_t)req->devicetree_size);
    memcpy(ba + 0x38, cmdline, strlen(cmdline));

    /* The core: SVC mode, interrupts masked, MMU off, r0 = boot_args. */
    for (unsigned i = 0; i < N88_VIC_COUNT; i++) s5l_vic_reset(&m->vic[i]);
    memset(&m->timer, 0, sizeof m->timer);
    memset(m->gpio, 0, sizeof m->gpio);
    m->cpu.arch = ARM_ARCH_V7_A8;
    arm_reset(&m->cpu, &m->bus);
    if (m->ci) arm_ci_flush(m->ci);
    m->cpu.cpsr = ARM_MODE_SVC | ARM_CPSR_I | ARM_CPSR_F | ARM_CPSR_A;
    const uint32_t entry_pa = k.entry - N88_VIRT_BASE + N88_DRAM_BASE;
    m->cpu.r[15] = entry_pa & ~1u;
    if (k.entry & 1u) m->cpu.cpsr |= ARM_CPSR_T;
    m->cpu.r[0] = (uint32_t)args_pa;

    m->booted = true;
    m->entry_pa = entry_pa;
    m->boot_args_pa = (uint32_t)args_pa;
    m->devicetree_pa = (uint32_t)tree_pa;
    m->devicetree_size = (uint32_t)req->devicetree_size;
    m->tokd_pa = (uint32_t)tokd_pa;
    return N88_OK;
}
