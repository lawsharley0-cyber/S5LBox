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

static void gpio_lines(n88_t *m);

/* A pad's level as the pin drives it: bits 1:0 = 1x drive bit 0 out (spi0's
 * select is 0x12 asserted, 0x13 released); 0x is an input, which the board
 * pulls high -- the touch reset is released by 0x12 -> 0x10. */
static bool pad_level(uint32_t v) {
    return (v & 2u) ? (v & 1u) != 0u : true;
}

/* Everything time moves: expire the decrementer if its interval has passed
 * (line 6), start display frames that are due (line 0x25). */
static void timer_update(n88_t *m) {
    const uint64_t now = n88_timer_count(m);
    if (m->timer.armed && now - m->timer.start >= m->timer.interval) {
        m->timer.armed = false;
        m->timer.pending = true;
        m->timer.fired++;
    }
    s5l_vic_set_line(&m->vic[0], N88_TIMER_LINE,
                     m->timer.pending && (m->timer.ctrl & 1u));
    m2clcd_advance(&m->clcd, now);
    s5l_vic_set_line(&m->vic[N88_CLCD_LINE / 32u], N88_CLCD_LINE % 32u, m2clcd_irq(&m->clcd));
    irq_update(m);
    gpio_lines(m);
}

/* The first cycle at which time raises a line -- the decrementer expiring
 * or an enabled display frame starting -- or UINT64_MAX. */
static uint64_t timer_due_cycles(const n88_t *m) {
    uint64_t due = m->timer.armed
        ? (m->timer.start + m->timer.interval) * N88_CYCLES_PER_TICK : UINT64_MAX;
    const uint64_t frame = m2clcd_due(&m->clcd);
    if (frame != UINT64_MAX && frame * N88_CYCLES_PER_TICK < due)
        due = frame * N88_CYCLES_PER_TICK;
    return due;
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

static bool in_spi0(uint32_t pa) {
    return pa >= N88_SPI0_PA && pa < N88_SPI0_PA + N88_SPI0_SIZE;
}

static bool in_spi1(uint32_t pa) {
    return pa >= N88_SPI1_PA && pa < N88_SPI1_PA + N88_SPI0_SIZE;
}

static bool in_clcd(uint32_t pa) {
    return pa >= N88_CLCD_PA && pa < N88_CLCD_PA + M2CLCD_SIZE;
}

static bool in_cdma(uint32_t pa) {
    return pa >= N88_CDMA_PA && pa < N88_CDMA_PA + CDMA_SIZE;
}

static bool in_cdma_aes(uint32_t pa) {
    return pa >= N88_CDMA_AES_PA && pa < N88_CDMA_AES_PA + CDMA_AES_SIZE;
}

static bool in_i2c0(uint32_t pa) {
    return pa >= N88_I2C0_PA && pa < N88_I2C0_PA + S5L8920_I2C_SIZE;
}

static bool in_i2c2(uint32_t pa) {
    return pa >= N88_I2C2_PA && pa < N88_I2C2_PA + S5L8920_I2C_SIZE;
}

static bool in_dsim(uint32_t pa) {
    return pa >= N88_DSIM_PA && pa < N88_DSIM_PA + S5L8920_DSIM_SIZE;
}

static bool in_dart(uint32_t pa) {
    return (pa >= N88_DART0_PA && pa < N88_DART0_PA + S5L8920_DART_SIZE) ||
           (pa >= N88_DART1_PA && pa < N88_DART1_PA + S5L8920_DART_SIZE);
}

static s5l8920_dart_t *dart_at(n88_t *m, uint32_t pa) {
    return pa >= N88_DART1_PA ? &m->dart1 : &m->dart0;
}

static bool in_sha1(uint32_t pa) {
    return pa >= N88_SHA1_PA && pa < N88_SHA1_PA + S5L_SHA1_SIZE;
}

/* The engine's view of memory: DRAM only, and a write tells the cached
 * interpreter, as a CPU store does, in case it lands on translated code. */
static bool cdma_mem(void *ctx, uint32_t pa, uint8_t *buf, uint32_t len, bool write) {
    n88_t *m = ctx;
    if (!len) return true;
    if (pa < N88_DRAM_BASE || (uint64_t)pa + len > (uint64_t)N88_DRAM_BASE + N88_DRAM_SIZE)
        return false;
    uint8_t *p = m->ram + (pa - N88_DRAM_BASE);
    if (write) {
        memcpy(p, buf, len);
        if (m->ci) arm_ci_note_ram_write(m->ci, pa, len);
    } else {
        memcpy(buf, p, len);
    }
    return true;
}

/* Where CDMA's peripheral requests go: the SHA-1 engine's FIFO, and spi1's
 * transmit FIFO (channel 18, the touch controller's firmware). The SPI shifts
 * each octet as it lands; in DMA mode nothing waits on the receive side. */
static bool cdma_periph(void *ctx, uint32_t fifo, const uint8_t *data, uint32_t len) {
    n88_t *m = ctx;
    if (fifo == N88_SHA1_PA + S5L_SHA1_FIFO) {
        s5l_sha1_feed(&m->sha1, data, len);
        return true;
    }
    if (fifo == N88_SPI1_PA + SPI_TXDATA) {
        /* The port asks for data only in DMA mode (SETUP bit 6); until then
         * the request waits, and n88 retries it when SETUP changes. */
        if (!(m->spi1.setup & SPI_SETUP_DMA)) return false;
        for (uint32_t i = 0; i < len; i++) {
            s5l_spi_write(&m->spi1, SPI_TXDATA, data[i]);
            s5l_spi_step(&m->spi1);
        }
        return true;
    }
    return false;
}

/* Every channel's level onto its VIC line. */
static void cdma_lines(n88_t *m) {
    for (unsigned n = 1; n < CDMA_CHANNELS; n++) {
        const unsigned line = N88_CDMA_LINE0 + n;
        s5l_vic_set_line(&m->vic[line / 32u], line % 32u, cdma_irq(&m->cdma, n));
    }
}

static void i2c_lines(n88_t *m) {
    s5l_vic_set_line(&m->vic[0], N88_I2C0_LINE, s5l8920_i2c_irq(&m->i2c0));
    s5l_vic_set_line(&m->vic[0], N88_I2C2_LINE, s5l8920_i2c_irq(&m->i2c2));
}

/* The LIS331DL's CTRL_REG2 BOOT bit reloads its trim and clears itself;
 * AppleLIS302DL::enableAccelerometer panics if it reads it set 500 ms on. */
static void accel_write(i2c_regfile_t *r, uint8_t reg, uint8_t v) {
    if (reg == 0x21u) r->reg[0x21] = (uint8_t)(v & ~0x40u);
}

/* The devices on the I2C buses as they power up. */
static void i2c_devices_reset(n88_t *m) {
    i2c_regfile_init(&m->accel, N88_ACCEL_ADDR);
    m->accel.autoinc_bit = 0x80u;               /* the LIS3xx's MSB flag     */
    m->accel.reg[0x0f] = 0x3bu;                 /* WHO_AM_I: LIS331DL        */
    m->accel.on_write = accel_write;
    i2c_regfile_init(&m->pmu, N88_PMU_ADDR);
}

/* The GPIO interrupt controller's line, from the one source wired to it:
 * the touch controller's attention, on interrupt N88_TOUCH_ATN_IRQ, while
 * its pad does not mask it. A level-triggered pin's status follows the
 * line; an edge-triggered one latches on assertion until written back. */
static void gpio_lines(n88_t *m) {
    const bool atn = s5l_mtz2_irq(&m->touch);
    const uint32_t pad = m->gpio[N88_TOUCH_ATN_IRQ];
    const unsigned g = N88_TOUCH_ATN_IRQ / 32u, bit = N88_TOUCH_ATN_IRQ % 32u;
    if (!(pad & N88_GPIO_IRQ_MASKED)) {
        if ((pad & 0xcu) == 4u) {
            if (atn) m->gpioic_status[g] |= 1u << bit;
            else     m->gpioic_status[g] &= ~(1u << bit);
        } else if (atn && !m->touch_atn_last) {
            m->gpioic_status[g] |= 1u << bit;
        }
    }
    m->touch_atn_last = atn;
    bool any = false;
    for (unsigned i = 0; i < N88_GPIOIC_GROUPS; i++) any |= m->gpioic_status[i] != 0u;
    s5l_vic_set_line(&m->vic[N88_GPIOIC_LINE / 32u], N88_GPIOIC_LINE % 32u, any);
    s5l_spi_irq_note(&m->spi1);
    s5l_vic_set_line(&m->vic[N88_SPI1_LINE / 32u], N88_SPI1_LINE % 32u, m->spi1.irq_last);
}

/* spi0's interrupt is a level the controller derives from its own state;
 * sampling it here also counts its rising edges (irq_rises). */
static void spi0_line(n88_t *m) {
    s5l_spi_irq_note(&m->spi0);
    s5l_vic_set_line(&m->vic[0], N88_SPI0_LINE, m->spi0.irq_last);
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
    if (in_gpio(pa) && pa - N88_GPIO_PA >= N88_GPIOIC_STATUS &&
        pa - N88_GPIO_PA < N88_GPIOIC_STATUS + 4u * N88_GPIOIC_GROUPS) {
        m->mmio++;
        return m->gpioic_status[(pa - N88_GPIO_PA - N88_GPIOIC_STATUS) >> 2];
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
    if (in_spi1(pa)) {
        m->mmio++;
        const uint32_t v = s5l_spi_read(&m->spi1, pa - N88_SPI1_PA);
        gpio_lines(m);
        irq_update(m);
        return v;
    }
    if (in_spi0(pa)) {
        /* Not a pure read: RXDATA pops the receive FIFO, which can let the
         * shifter run and move the line. */
        m->mmio++;
        const uint32_t v = s5l_spi_read(&m->spi0, pa - N88_SPI0_PA);
        spi0_line(m);
        irq_update(m);
        return v;
    }
    if (in_cdma(pa)) {
        m->mmio++;
        return cdma_read(&m->cdma, pa - N88_CDMA_PA);
    }
    if (in_clcd(pa)) {
        /* Frame starts are brought up to date first, so a polled status
         * read sees the frame the timebase says has begun. */
        m->mmio++;
        timer_update(m);
        return m2clcd_read(&m->clcd, pa - N88_CLCD_PA);
    }
    if (in_cdma_aes(pa)) {
        m->mmio++;
        return cdma_aes_read(&m->cdma, pa - N88_CDMA_AES_PA);
    }
    if (in_sha1(pa)) {
        m->mmio++;
        return s5l_sha1_read(&m->sha1, pa - N88_SHA1_PA);
    }
    if (in_i2c0(pa)) {
        m->mmio++;
        return s5l8920_i2c_read(&m->i2c0, pa - N88_I2C0_PA);
    }
    if (in_dart(pa)) {
        m->mmio++;
        return s5l8920_dart_read(dart_at(m, pa), pa & (S5L8920_DART_SIZE - 1u));
    }
    if (in_dsim(pa)) {
        m->mmio++;
        return s5l8920_dsim_read(&m->dsim, pa - N88_DSIM_PA);
    }
    if (in_i2c2(pa)) {
        m->mmio++;
        return s5l8920_i2c_read(&m->i2c2, pa - N88_I2C2_PA);
    }
    if (in_vic(pa)) {
        m->mmio++;
        const unsigned n = (pa - N88_VIC_PA) >> 16;
        const uint32_t off = pa & 0xffffu;
        if (off != VIC_VECTADDR) return s5l_vic_read(&m->vic[n], off);
        /* The three are daisy-chained: a VIC with nothing of its own
         * pending passes the next one's vector through. AppleARMPL192VIC
         * (0x807e5c54) reads VIC0's for every interrupt and, for a source
         * on VIC1 or VIC2, acknowledges each VIC down the chain. */
        for (unsigned i = n; i < N88_VIC_COUNT; i++) {
            const uint32_t v = s5l_vic_vectaddr(&m->vic[i], 32u * i);
            if (v) return v;
        }
        return 0u;
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
    } else if (in_gpio(pa) && pa - N88_GPIO_PA >= N88_GPIOIC_STATUS &&
               pa - N88_GPIO_PA < N88_GPIOIC_STATUS + 4u * N88_GPIOIC_GROUPS) {
        m->gpioic_status[(pa - N88_GPIO_PA - N88_GPIOIC_STATUS) >> 2] &= ~v;   /* W1C */
    } else if (in_gpio(pa)) {
        m->gpio[(pa - N88_GPIO_PA) >> 2] = v;   /* stored, read back verbatim */
        if (pa - N88_GPIO_PA == N88_SPI0_CS_GPIO)
            spi_nor_select(&m->nor, (v & 1u) == 0u);    /* active low */
        if (pa - N88_GPIO_PA == N88_TOUCH_CS_PAD) s5l_mtz2_select_pin(&m->touch, pad_level(v));
        if (pa - N88_GPIO_PA == N88_TOUCH_RESET_PAD) s5l_mtz2_reset_pin(&m->touch, pad_level(v));
    } else if (in_spi1(pa)) {
        s5l_spi_write(&m->spi1, pa - N88_SPI1_PA, v);
        if (pa - N88_SPI1_PA == SPI_SETUP) {
            cdma_retry(&m->cdma);
            cdma_lines(m);
        }
    } else if (in_spi0(pa)) {
        s5l_spi_write(&m->spi0, pa - N88_SPI0_PA, v);
    } else if (in_cdma(pa)) {
        cdma_write(&m->cdma, pa - N88_CDMA_PA, v);
        cdma_lines(m);
    } else if (in_cdma_aes(pa)) {
        cdma_aes_write(&m->cdma, pa - N88_CDMA_AES_PA, v);
    } else if (in_sha1(pa)) {
        s5l_sha1_write(&m->sha1, pa - N88_SHA1_PA, v);
    } else if (in_dart(pa)) {
        s5l8920_dart_write(dart_at(m, pa), pa & (S5L8920_DART_SIZE - 1u), v);
    } else if (in_dsim(pa)) {
        s5l8920_dsim_write(&m->dsim, pa - N88_DSIM_PA, v);
    } else if (in_i2c0(pa)) {
        s5l8920_i2c_write(&m->i2c0, pa - N88_I2C0_PA, v);
        i2c_lines(m);
    } else if (in_i2c2(pa)) {
        s5l8920_i2c_write(&m->i2c2, pa - N88_I2C2_PA, v);
        i2c_lines(m);
    } else if (in_clcd(pa)) {
        m2clcd_advance(&m->clcd, n88_timer_count(m));
        m2clcd_write(&m->clcd, pa - N88_CLCD_PA, v);
    } else {
        modelled = false;
    }
    if (modelled) m->mmio++;
    else {
        m->unmodelled++;
        if (m->trace) m->trace(m->trace_ctx, pa, size, true, v, m->cpu.r[15]);
    }
    spi0_line(m);
    gpio_lines(m);
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
    uint64_t due = m->timer.armed && (m->timer.ctrl & 1u)
        ? (m->timer.start + m->timer.interval) * N88_CYCLES_PER_TICK : UINT64_MAX;
    const uint64_t frame = m2clcd_due(&m->clcd);
    if (frame != UINT64_MAX && frame * N88_CYCLES_PER_TICK < due)
        due = frame * N88_CYCLES_PER_TICK;
    if (due != UINT64_MAX && due > m->cpu.cycles) m->cpu.cycles = due;
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
    m->nor_mem = malloc(N88_NOR_SIZE);
    m->scanout = calloc(1, N88_FB_STRIDE * N88_FB_HEIGHT);
    if (!m->ram || !m->nor_mem || !m->scanout) { n88_free(m); return false; }
    memset(m->nor_mem, 0xff, N88_NOR_SIZE);     /* an erased part */
    if (!spi_nor_init(&m->nor, m->nor_mem, N88_NOR_SIZE, SPI_NOR_M25PE80_ID)) {
        n88_free(m);
        return false;
    }
    if (!cdma_init(&m->cdma, cdma_mem, m)) { n88_free(m); return false; }
    cdma_set_peripheral(&m->cdma, cdma_periph, m);
    i2c_devices_reset(m);
    {
        const s5l_i2c_slave_t accel = i2c_regfile_slave(&m->accel);
        const s5l_i2c_slave_t pmu = i2c_regfile_slave(&m->pmu);
        if (!s5l8920_i2c_attach(&m->i2c0, &accel) || !s5l8920_i2c_attach(&m->i2c0, &pmu)) {
            n88_free(m);
            return false;
        }
    }
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
    free(m->nor_mem);
    m->nor_mem = NULL;
    free(m->scanout);
    m->scanout = NULL;
    cdma_free(&m->cdma);
}

uint8_t *n88_nor(n88_t *m) {
    return m ? m->nor_mem : NULL;
}

const uint8_t *n88_framebuffer(const n88_t *m) {
    if (!m || !m->ram || !m->booted) return NULL;
    /* What the display controller scans out, while it keeps the boot
     * framebuffer's geometry (the hosts show N88_FB_WIDTH x N88_FB_HEIGHT
     * at N88_FB_STRIDE); otherwise iBoot's framebuffer. */
    m2clcd_scanout_t s;
    const uint32_t bytes = N88_FB_STRIDE * N88_FB_HEIGHT;
    if (m2clcd_scanout(&m->clcd, &s) && s.bpp == N88_FB_DEPTH &&
        s.width == N88_FB_WIDTH && s.height == N88_FB_HEIGHT &&
        s.stride_bytes == N88_FB_STRIDE) {
        if (in_ram(s.addr, bytes)) return m->ram + (s.addr - N88_DRAM_BASE);
        /* An I/O address: gathered through dart0 a page at a time, with a
         * page it does not map shown black. */
        const s5l8920_dart_ram_t dram = { m->ram, N88_DRAM_BASE, N88_DRAM_SIZE };
        uint32_t pa = 0;
        if (s5l8920_dart_translate(&m->dart0, &dram, s.addr, &pa)) {
            for (uint32_t o = 0; o < bytes;) {
                const uint32_t iova = s.addr + o;
                uint32_t n = 0x1000u - (iova & 0xfffu);
                if (n > bytes - o) n = bytes - o;
                if (s5l8920_dart_translate(&m->dart0, &dram, iova, &pa) && in_ram(pa, n))
                    memcpy(m->scanout + o, m->ram + (pa - N88_DRAM_BASE), n);
                else
                    memset(m->scanout + o, 0, n);
                o += n;
            }
            return m->scanout;
        }
    }
    return m->ram + (N88_VRAM_PA - N88_DRAM_BASE);
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
        if (m->timer.armed || m2clcd_due(&m->clcd) != UINT64_MAX) timer_update(m);
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

    /*
     * The kernel's virtual base, from its own Mach-O: the va that maps to the
     * start of DRAM. iOS 6's 3GS kernel is linked at 0x80000000, iPhone OS
     * 3.1.3's at 0xC0000000; both load at physical 0x40000000. Rounding the
     * lowest segment down to a 256 MiB boundary (the DRAM size) gives that
     * base (0x80001000 -> 0x80000000, 0xC0008000 -> 0xC0000000), so the
     * physical layout below is identical for iOS 6 and correct for 3.1.3.
     */
    const uint32_t virt_base = k.vm_low & ~(uint32_t)(N88_DRAM_SIZE - 1u);

    /* Where everything goes: the kernel at its link address, then the tree,
     * boot_args and topOfKernelData, 16 KiB aligned because the kernel builds
     * its first-level translation table there. */
    const uint64_t kernel_end = (uint64_t)k.vm_high - virt_base + N88_DRAM_BASE;
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
        if (s->vmaddr < virt_base)
            return fail(N88_ERR_KERNEL, detail, cap, "segment %s below %08x", s->name, virt_base);
        const uint32_t pa = s->vmaddr - virt_base + N88_DRAM_BASE;
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
    const uint32_t pram[2] = { N88_PRAM_PA, N88_PRAM_SIZE };
    const uint32_t vram[2] = { N88_VRAM_PA, N88_VRAM_SIZE };
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
    /* The framebuffer pool; like iBoot, pass over a tree without /vram. */
    (void)dt_set_words(tree, &dt, &root, "vram", "reg", vram, 2);
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
        /* iOS 6's iBoot hands the kernel NVRAM through this property; iPhone
         * OS 3's tree has none (its kernel reads NVRAM from the NOR), so a
         * tree without it is passed over, and only a short one refused. */
        uint32_t nl = 0;
        uint8_t *nv = dt_prop_rw(tree, &dt, &root, "chosen", "nvram-proxy-data", &nl);
        if (nv && nl < 0x1000u)
            return fail(N88_ERR_DEVICETREE, detail, cap,
                        "device tree: /chosen:nvram-proxy-data is under 4 KB");
        if (nv) n88_nvram_image(nv, nl);
    }
    {
        /* The panel's identity: iBoot reads it from the panel over MIPI-DSI
         * and writes it over the tree's placeholder 0. iPhone OS 3.1.3's
         * ApplePinotLCD refuses to start on a zero lcd-panel-id (0xc03fc2b6),
         * and with no panel driver the display driver waits for its
         * lcd_enable function forever. Nothing reads the value otherwise
         * (it is logged), so the emulated panel's is a made-up one. Passed
         * over in a tree without the property. */
        const uint32_t panel = N88_LCD_PANEL_ID;
        (void)dt_set_words(tree, &dt, &root, "arm-io/mipi-dsim/lcd", "lcd-panel-id", &panel, 1);
    }
    /* Un-match: an 'x' over the first byte of every string in the node's
     * compatible, so no driver claims it. Every string, not just the first
     * (core/src/boot/bringup.c strikes only that): S5L8920 nodes list their
     * older relatives too ("usb-otg,s5l8920x\0usb-otg,s5l8720x\0..."), and a
     * driver matching a later one would still start. */
    static const char *const default_unmatch[] = { "arm-io/iop" };
    const char *const *um = req->unmatch ? req->unmatch : default_unmatch;
    const unsigned un = req->unmatch ? req->unmatch_count : 1u;
    for (unsigned i = 0; i < un; i++) {
        uint32_t cl = 0;
        uint8_t *compat = um[i] ? dt_prop_rw(tree, &dt, &root, um[i], "compatible", &cl) : NULL;
        if (!compat || !cl)
            return fail(N88_ERR_DEVICETREE, detail, cap, "device tree: cannot un-match /%s",
                        um[i] ? um[i] : "(null)");
        for (uint32_t at = 0; at < cl; at++)
            if (at == 0 || compat[at - 1] == '\0') compat[at] = compat[at] ? 'x' : compat[at];
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
    /* The version is the kernel's epoch: pe_identify_machine loads
     * boot_args+2 and panics unless it matches. iOS 6 (0x8027ace0) wants 5,
     * the default; the 3GS's iPhone OS 3.1.3 (0xc01a292e) wants 4. */
    ba[2] = req->boot_args_version ? req->boot_args_version : 5u; ba[3] = 0;
    st32(ba + 0x04, virt_base);
    st32(ba + 0x08, N88_DRAM_BASE);
    /* The top is boot-owned. Whole MiB: iPhone OS 3.1.3's start code
     * (0xc00670c8) maps memory a MiB at a time until memSize reaches exactly
     * zero, and walks off the end of DRAM forever on any remainder. */
    st32(ba + 0x0c, (N88_DRAM_SIZE - N88_TOP_RESERVE) & ~UINT32_C(0xfffff));
    st32(ba + 0x10, (uint32_t)tokd_pa);
    /* Boot_Video: iBoot's first framebuffer (n88.h). v_display selects the
     * console mode, not whether a display exists: the iPhone OS 3 machine
     * measured that non-zero picks graphics mode, in which the kernel never
     * attaches its text console, and zero makes it paint the boot log onto
     * the framebuffer (core/src/boot/bringup.c). This machine has no boot
     * logo to show, so it asks for the log. */
    st32(ba + 0x14, N88_VRAM_PA);
    st32(ba + 0x18, 0u);
    st32(ba + 0x1c, N88_FB_STRIDE);
    st32(ba + 0x20, N88_FB_WIDTH);
    st32(ba + 0x24, N88_FB_HEIGHT);
    st32(ba + 0x28, N88_FB_DEPTH);
    st32(ba + 0x30, (uint32_t)tree_pa - N88_DRAM_BASE + virt_base);
    st32(ba + 0x34, (uint32_t)req->devicetree_size);
    memcpy(ba + 0x38, cmdline, strlen(cmdline));

    /* The core: SVC mode, interrupts masked, MMU off, r0 = boot_args. */
    for (unsigned i = 0; i < N88_VIC_COUNT; i++) s5l_vic_reset(&m->vic[i]);
    memset(&m->timer, 0, sizeof m->timer);
    memset(m->gpio, 0, sizeof m->gpio);
    /* spi0 at version 1 with the flash at its only select; the flash keeps
     * its array, as a real one does across a reboot. */
    s5l_spi_reset(&m->spi0);
    s5l_spi_set_version(&m->spi0, 1u);
    {
        s5l_spi_slave_t nor_slave;
        spi_nor_bind(&m->nor, &nor_slave);
        s5l_spi_attach(&m->spi0, 0u, &nor_slave);
    }
    spi_nor_reset(&m->nor);
    /* spi1 at version 1 with the touch controller at its only select, powered
     * (its LDO is the PMU's) and held in reset until the driver releases it. */
    s5l_spi_reset(&m->spi1);
    s5l_spi_set_version(&m->spi1, 1u);
    s5l_mtz2_reset(&m->touch);
    {
        s5l_spi_slave_t touch_slave;
        s5l_mtz2_bind(&m->touch, &touch_slave);
        s5l_spi_attach(&m->spi1, 0u, &touch_slave);
    }
    s5l_mtz2_power_pin(&m->touch, true);
    memset(m->gpioic_status, 0, sizeof m->gpioic_status);
    m->touch_atn_last = false;
    cdma_reset(&m->cdma);
    s5l_sha1_reset(&m->sha1);
    s5l8920_i2c_reset(&m->i2c0);
    s5l8920_i2c_reset(&m->i2c2);
    i2c_devices_reset(m);
    s5l8920_dart_reset(&m->dart0);
    s5l8920_dart_reset(&m->dart1);
    s5l8920_dsim_reset(&m->dsim);
    m->cpu.arch = ARM_ARCH_V7_A8;
    arm_reset(&m->cpu, &m->bus);
    {
        /* After the CPU reset, whose cycle count is the clock. The display
         * as iBoot leaves it: running, showing the first
         * boot framebuffer. */
        const m2clcd_boot_fb_t fb = { N88_VRAM_PA, N88_FB_WIDTH, N88_FB_HEIGHT, N88_FB_WIDTH };
        m2clcd_reset(&m->clcd, &fb, n88_timer_count(m), N88_TB_HZ / N88_FRAME_HZ);
    }
    if (m->ci) arm_ci_flush(m->ci);
    m->cpu.cpsr = ARM_MODE_SVC | ARM_CPSR_I | ARM_CPSR_F | ARM_CPSR_A;
    const uint32_t entry_pa = k.entry - virt_base + N88_DRAM_BASE;
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
