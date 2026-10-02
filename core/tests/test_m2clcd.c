/*
 * NEON — the S5L8920 display controller: what AppleM2CLCD (10B500) reads at
 * hand-off, its start_hardware and interrupt sequences, frame timing, and
 * the scanout the machine shows.
 */
#include "m2clcd.h"

#include <stdio.h>

static int g_pass, g_fail;
#define CHECK(cond, ...) do { \
    if (cond) g_pass++; \
    else { g_fail++; printf("  FAIL %s:%d: ", __func__, __LINE__); \
           printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static const m2clcd_boot_fb_t k_boot = { 0x4fe3a000u, 320u, 480u, 320u };

/* create_default_fb_surface (0x809961f8), as the driver computes it. */
static void test_handoff_as_the_driver_reads_it(void) {
    m2clcd_t c;
    m2clcd_reset(&c, &k_boot, 0u, 400000u);
    const uint32_t layers = m2clcd_read(&c, 0x4);
    CHECK(layers & 0x10u, "window A not on at hand-off: %08x", layers);
    const uint32_t ctrl = m2clcd_read(&c, 0x20), addr = m2clcd_read(&c, 0x24);
    const uint32_t stride = m2clcd_read(&c, 0x28), size = m2clcd_read(&c, 0x30);
    const uint32_t w = (size >> 16) & 0x1ffu, h = size & 0x1ffu;
    const uint32_t bytes = ((ctrl & 0xf00u) == 0x700u) ? stride << 2 : stride << 1;
    CHECK(addr == 0x4fe3a000u && w == 320u && h == 480u && bytes == 1280u,
          "adopted %08x %ux%u stride %u", addr, w, h, bytes);
    CHECK(((bytes * h + 0xfffu) & ~0xfffu) == 0x96000u, "surface bytes");
    m2clcd_scanout_t s;
    CHECK(m2clcd_scanout(&c, &s) && s.addr == 0x4fe3a000u && s.bpp == 32u &&
          s.stride_bytes == 1280u, "scanout at hand-off");
    CHECK(!m2clcd_irq(&c) && m2clcd_due(&c) == UINT64_MAX, "no interrupt enabled yet");

    m2clcd_reset(&c, NULL, 0u, 400000u);
    CHECK(!(m2clcd_read(&c, 0x4) & 0x30u) && !m2clcd_scanout(&c, &s),
          "with no boot framebuffer there is no default surface");
}

/* start_hardware (0x80995652) and the interrupt (0x809947f4). */
static void test_start_hardware_and_frames(void) {
    m2clcd_t c;
    m2clcd_reset(&c, &k_boot, 1000u, 400000u);
    uint32_t v = m2clcd_read(&c, 0x0);
    m2clcd_write(&c, 0x0, v | 0x100u);
    m2clcd_write(&c, 0x0, m2clcd_read(&c, 0x0) & ~0x100u);
    m2clcd_write(&c, 0x0, m2clcd_read(&c, 0x0) & ~1u);
    CHECK(m2clcd_read(&c, 0x0) & 2u, "stopped: the idle bit openiBoot waits for");
    m2clcd_write(&c, 0x1b2c, 0xfu);
    m2clcd_write(&c, 0x0, m2clcd_read(&c, 0x0) | 1u);
    CHECK((m2clcd_read(&c, 0x0) & 3u) == 1u, "the driver's poll for bit 0");
    m2clcd_write(&c, 0x1b2c, 0xfu);
    m2clcd_write(&c, 0x8, 0x1701u);

    CHECK(m2clcd_due(&c) == 401000u, "first frame at %llu",
          (unsigned long long)m2clcd_due(&c));
    CHECK(!m2clcd_advance(&c, 400999u) && !m2clcd_irq(&c), "a frame early");
    CHECK(m2clcd_advance(&c, 401000u) && m2clcd_irq(&c), "the frame interrupt");
    const uint32_t st = m2clcd_read(&c, 0xc) & m2clcd_read(&c, 0x8);
    CHECK(st == 1u, "status & enable = %08x", st);
    m2clcd_write(&c, 0x1b2c, 0xfu);
    m2clcd_write(&c, 0xc, 1u);
    CHECK(!m2clcd_irq(&c) && m2clcd_due(&c) == 801000u, "acknowledged; next frame");

    /* Several periods at once (an idle stretch) latch once and keep phase. */
    CHECK(m2clcd_advance(&c, 2000000u) && c.frames == 1u + 3u && m2clcd_due(&c) == 2001000u,
          "catch-up: frames %llu due %llu", (unsigned long long)c.frames,
          (unsigned long long)m2clcd_due(&c));
    m2clcd_write(&c, 0xc, 1u);

    /* No swap pending: the driver turns frame interrupts off. */
    m2clcd_write(&c, 0x8, m2clcd_read(&c, 0x8) & ~1u);
    CHECK(m2clcd_due(&c) == UINT64_MAX, "no frame interrupt wanted");
    m2clcd_advance(&c, 2500000u);
    CHECK(!m2clcd_irq(&c) && (m2clcd_read(&c, 0xc) & 1u), "latched, not raised");
    m2clcd_write(&c, 0xc, 0x1700u);
    CHECK(m2clcd_read(&c, 0xc) == 1u, "clearing underruns leaves the frame bit");

    /* A stopped controller starts no frames. */
    m2clcd_write(&c, 0xc, 1u);
    m2clcd_write(&c, 0x8, 1u);
    m2clcd_write(&c, 0x0, 0u);
    CHECK(m2clcd_due(&c) == UINT64_MAX && !m2clcd_advance(&c, 9000000u) &&
          !(m2clcd_read(&c, 0xc) & 1u), "stopped");
}

static void test_registers_and_swap(void) {
    m2clcd_t c;
    m2clcd_reset(&c, &k_boot, 0u, 0u);
    CHECK(m2clcd_due(&c) == UINT64_MAX && !m2clcd_advance(&c, ~0ull), "no frame period");
    /* Everything else reads back: the colour matrix, layers, the table. */
    m2clcd_write(&c, 0x70, 0x4a8u);
    m2clcd_write(&c, 0x408, 0x10005u);
    m2clcd_write(&c, 0x1b24, (319u << 16) | 479u);
    CHECK(m2clcd_read(&c, 0x70) == 0x4a8u && m2clcd_read(&c, 0x408) == 0x10005u &&
          m2clcd_read(&c, 0x1b24) == ((319u << 16) | 479u), "read back");
    CHECK(m2clcd_read(&c, 0x2000) == 0u && m2clcd_read(&c, 0x71) == 0u, "out of range");
    m2clcd_write(&c, 0x2000, 1u);

    /* A swap to a second buffer moves the scanout. */
    m2clcd_write(&c, 0x24, 0x4fed0000u);
    m2clcd_scanout_t s;
    CHECK(m2clcd_scanout(&c, &s) && s.addr == 0x4fed0000u, "swapped scanout");
    /* An RGB565 window on B. */
    m2clcd_write(&c, 0x4, 0x20u);
    m2clcd_write(&c, 0x40, 0x500u);
    m2clcd_write(&c, 0x44, 0x48000000u);
    m2clcd_write(&c, 0x48, 320u);
    m2clcd_write(&c, 0x50, (320u << 16) | 480u);
    CHECK(m2clcd_scanout(&c, &s) && s.addr == 0x48000000u && s.bpp == 16u &&
          s.stride_bytes == 640u, "window B, 565");
}

int main(void) {
    printf("NEON M2 CLCD tests\n");
    test_handoff_as_the_driver_reads_it();
    test_start_hardware_and_frames();
    test_registers_and_swap();
    printf("  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
