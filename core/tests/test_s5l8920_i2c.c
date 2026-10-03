/*
 * NEON — the S5L8920 I2C controller and the register-file slave, driven the
 * way iPhone OS 3.1.3's AppleS5L8920XI2CController drives them, and the
 * S5L8920 DART's slots and translation, and the DSIM's HS-clock handshake.
 */
#include "s5l8920_i2c.h"
#include "s5l8920_dart.h"
#include "s5l8920_dsim.h"

#include <stdio.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(cond, ...) do { \
    if (cond) g_pass++; \
    else { g_fail++; printf("  FAIL %s:%d: ", __func__, __LINE__); \
           printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* The driver's enable (0xc0678dc0) and transfer (0xc0678f38). */
static void enable(s5l8920_i2c_t *c) {
    s5l8920_i2c_write(c, 0xc, 0x37u);
    s5l8920_i2c_write(c, 0x8, 0x30u);
}
static void start(s5l8920_i2c_t *c, uint8_t addr, uint8_t first, const uint8_t *data,
                  unsigned count, bool write) {
    s5l8920_i2c_write(c, 0x0, addr);
    s5l8920_i2c_write(c, 0x10, first);
    s5l8920_i2c_write(c, 0x18, count);
    for (unsigned i = 0; write && i < count; i++) s5l8920_i2c_write(c, 0x20, data[i]);
    s5l8920_i2c_write(c, 0x24, 4u | (write ? 1u : 0u));
}
/* The interrupt (0xc0678e94): status, a read's bytes, the write-back. */
static uint32_t finish(s5l8920_i2c_t *c, uint8_t *out, unsigned count) {
    const uint32_t st = s5l8920_i2c_read(c, 0xc);
    if ((st & 0x10u) && out)
        for (unsigned i = 0; i < count; i++) out[i] = (uint8_t)s5l8920_i2c_read(c, 0x20);
    s5l8920_i2c_write(c, 0xc, st);
    return st;
}

static void test_transfers(void) {
    static s5l8920_i2c_t c;
    static i2c_regfile_t accel, pmu;
    memset(&c, 0, sizeof c);
    s5l8920_i2c_reset(&c);
    i2c_regfile_init(&accel, 0x1d);
    accel.autoinc_bit = 0x80u;
    accel.reg[0x0f] = 0x3bu;
    accel.reg[0x29] = 0x01u; accel.reg[0x2a] = 0x02u; accel.reg[0x2b] = 0x03u;
    i2c_regfile_init(&pmu, 0x74);
    const s5l_i2c_slave_t a = i2c_regfile_slave(&accel), p = i2c_regfile_slave(&pmu);
    CHECK(s5l8920_i2c_attach(&c, &a) && s5l8920_i2c_attach(&c, &p), "attach");
    CHECK(!s5l8920_i2c_attach(&c, &a), "a second slave at one address is refused");
    enable(&c);

    /* The first transfer of a 3.1.3 boot: the accelerometer's WHO_AM_I. */
    start(&c, 0x1d, 0x0f, NULL, 1, false);
    CHECK(s5l8920_i2c_irq(&c), "done raises the line");
    uint8_t b[4] = {0};
    CHECK(finish(&c, b, 1) == 0x10u && b[0] == 0x3bu && !s5l8920_i2c_irq(&c),
          "WHO_AM_I reads 0x3b and the write-back clears it");

    /* A multi-byte read with the auto-increment bit. */
    start(&c, 0x1d, 0x80u | 0x29u, NULL, 3, false);
    CHECK(finish(&c, b, 3) == 0x10u && b[0] == 1u && b[1] == 2u && b[2] == 3u, "three bytes");

    /* A write: register byte at +0x10, data through the FIFO, then read back. */
    const uint8_t w[2] = { 0x87u, 0xdfu };
    start(&c, 0x74, 0x09, w, 2, true);
    CHECK(finish(&c, NULL, 0) == 0x10u && pmu.reg[0x09] == 0x87u && pmu.reg[0x0a] == 0xdfu,
          "a two-byte write lands at 0x09, 0x0a");
    start(&c, 0x74, 0x09, NULL, 2, false);
    CHECK(finish(&c, b, 2) == 0x10u && b[0] == 0x87u && b[1] == 0xdfu, "and reads back");
    CHECK(((pmu.write_map[1] >> 1) & 1u) && ((pmu.read_map[1] >> 1) & 1u), "touched registers recorded");

    /* No device at the address: the error bit, no data. */
    start(&c, 0x4a, 0x01, NULL, 1, false);
    CHECK(finish(&c, b, 1) == 0x20u && c.naks == 1u, "no acknowledge sets bit 5");

    /* Interrupts off: the status is still there to poll, the line is not. */
    s5l8920_i2c_write(&c, 0x8, 0u);
    start(&c, 0x1d, 0x0f, NULL, 1, false);
    CHECK(!s5l8920_i2c_irq(&c) && s5l8920_i2c_read(&c, 0xc) == 0x10u, "polled completion");
}

static void test_dart(void) {
    static uint8_t ram[0x40000];
    memset(ram, 0, sizeof ram);
    const s5l8920_dart_ram_t dram = { ram, 0x40000000u, sizeof ram };
    s5l8920_dart_t d;
    s5l8920_dart_reset(&d);
    /* AppleH2PDART's setup (0xc0545124): off, config, 16 slots, on. Slot n's
     * table at DRAM + 0x10000 + n pages. */
    s5l8920_dart_write(&d, 0xc, 0u);
    s5l8920_dart_write(&d, 0x0, 0x702u);
    for (uint32_t n = 0; n < 16u; n++)
        s5l8920_dart_write(&d, 0x8, (0x10000u + (n << 12)) | (n << 8) | 1u);
    uint32_t pa = 0;
    CHECK(!s5l8920_dart_translate(&d, &dram, 0x3c0d8000u, &pa), "off: nothing translates");
    s5l8920_dart_write(&d, 0xc, 0x80000070u);
    /* Map I/O page 0x3c0d8000 (slot 0, entry 0xd8) to DRAM + 0x2000, and
     * 0x3c4d9000 (slot 1, entry 0xd9) to DRAM + 0x3000. */
    const uint32_t e0 = 0x10000u + 0xd8u * 4u, e1 = 0x11000u + 0xd9u * 4u;
    ram[e0] = 0x01; ram[e0 + 1] = 0x20;
    ram[e1] = 0x01; ram[e1 + 1] = 0x30;
    CHECK(s5l8920_dart_translate(&d, &dram, 0x3c0d8123u, &pa) && pa == 0x40002123u, "slot 0");
    CHECK(s5l8920_dart_translate(&d, &dram, 0x3c4d9010u, &pa) && pa == 0x40003010u, "slot 1");
    CHECK(!s5l8920_dart_translate(&d, &dram, 0x3c0d9000u, &pa), "an unmapped page");
    s5l8920_dart_write(&d, 0x8, (0x12000u) | (2u << 8));
    CHECK(!s5l8920_dart_translate(&d, &dram, 0x3c800000u, &pa), "an invalid slot");
    s5l8920_dart_write(&d, 0x8, 0x0fff0000u | (3u << 8) | 1u);
    CHECK(!s5l8920_dart_translate(&d, &dram, 0x3cc00000u, &pa), "a table outside DRAM");
    CHECK(s5l8920_dart_read(&d, 0x8) == (0x0fff0000u | (3u << 8) | 1u), "+0x8 reads back");
}

/* AppleS5L8900XMIPIDSIController's HS clock (0xc069a638): set CLKCTRL bit
 * 31, wait for STATUS bit 10; clear it, wait for the bit to clear. */
static void test_dsim(void) {
    s5l8920_dsim_t d;
    s5l8920_dsim_reset(&d);
    uint32_t st = s5l8920_dsim_read(&d, 0x0);
    CHECK((st & 0x80000000u) && (st & 0xfu) == 0xfu && (st & 0x100u) && !(st & 0x400u),
          "reset: PLL stable, every lane stopped (%08x)", st);
    s5l8920_dsim_write(&d, 0x8, s5l8920_dsim_read(&d, 0x8) | 0x80000000u);
    st = s5l8920_dsim_read(&d, 0x0);
    CHECK((st & 0x400u) && !(st & 0x100u), "HS requested: ready for HS (%08x)", st);
    s5l8920_dsim_write(&d, 0x8, s5l8920_dsim_read(&d, 0x8) & ~0x80000000u);
    CHECK(!(s5l8920_dsim_read(&d, 0x0) & 0x400u), "released");
    s5l8920_dsim_write(&d, 0x0, 0u);
    CHECK(s5l8920_dsim_read(&d, 0x0) & 0x80000000u, "STATUS is read-only");
    /* The software reset (0xc069aa3a) and ULPS in and out (0xc069aace). */
    s5l8920_dsim_write(&d, 0x4, 1u);
    CHECK(s5l8920_dsim_read(&d, 0x0) & 0x100000u, "reset released");
    s5l8920_dsim_write(&d, 0x14, 0x8au);
    st = s5l8920_dsim_read(&d, 0x0);
    CHECK((st & 0x2f0u) == 0x2f0u && !(st & 0x10fu), "ULPS: every lane in it (%08x)", st);
    s5l8920_dsim_write(&d, 0x14, 0x8fu);
    st = s5l8920_dsim_read(&d, 0x0);
    CHECK(!(st & 0x2f0u) && (st & 0x10fu) == 0x10fu, "ULPS exit: stopped again (%08x)", st);
    s5l8920_dsim_write(&d, 0x38, 0x11223344u);
    s5l8920_dsim_write(&d, 0x34, 0x00002905u);                 /* DCS display on */
    CHECK(d.packets == 1u && d.payload_words == 1u && d.last_header == 0x2905u &&
          s5l8920_dsim_read(&d, 0x34) == 0x2905u, "a packet counted");
}

int main(void) {
    printf("NEON S5L8920 I2C, DART and DSIM tests\n");
    test_transfers();
    test_dart();
    test_dsim();
    printf("  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
