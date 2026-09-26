/*
 * NEON — the SPI NOR flash: each command AppleARMSPIFlashController sends,
 * the framing rules that decide whether it takes effect, and the driver's own
 * ID read carried through a version 1 SPI controller.
 */
#include "spi_nor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(cond, ...) do { \
    if (cond) g_pass++; \
    else { g_fail++; printf("  FAIL %s:%d: ", __func__, __LINE__); \
           printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define SIZE (1u << 20)
static uint8_t g_mem[SIZE];

static void fresh(spi_nor_t *nor) {
    memset(g_mem, 0xff, sizeof g_mem);
    CHECK(spi_nor_init(nor, g_mem, SIZE, SPI_NOR_M25PE80_ID), "init");
}

/* One selection: send `n` octets, keep the answers in `in` (may be NULL). */
static void xfer(spi_nor_t *nor, const uint8_t *out, size_t n, uint8_t *in) {
    spi_nor_select(nor, true);
    for (size_t i = 0; i < n; i++) {
        const uint8_t v = spi_nor_transfer(nor, out[i]);
        if (in) in[i] = v;
    }
    spi_nor_select(nor, false);
}
static uint8_t rdsr(spi_nor_t *nor) {
    const uint8_t o[2] = { 0x05, 0xff };
    uint8_t i[2];
    xfer(nor, o, 2, i);
    return i[1];
}
static void wren(spi_nor_t *nor) { const uint8_t o = 0x06; xfer(nor, &o, 1, NULL); }

static void test_init_and_idle(void) {
    spi_nor_t nor;
    CHECK(!spi_nor_init(NULL, g_mem, SIZE, 1u) && !spi_nor_init(&nor, NULL, SIZE, 1u) &&
          !spi_nor_init(&nor, g_mem, 3u * 4096u, 1u) &&
          !spi_nor_init(&nor, g_mem, 2048u, 1u) &&
          !spi_nor_init(&nor, g_mem, SIZE, 0x1000000u), "unusable arguments accepted");
    fresh(&nor);
    CHECK(spi_nor_transfer(&nor, 0x9f) == 0xffu && nor.commands == 0u,
          "a deselected part answered or took a command");
    spi_nor_select(&nor, false);
    CHECK(nor.selections == 0u, "a release with no select counted");
}

static void test_read_id_and_status(void) {
    spi_nor_t nor;
    fresh(&nor);
    const uint8_t id[4] = { 0x9f, 0xff, 0xff, 0xff };
    uint8_t in[4];
    xfer(&nor, id, 4, in);
    CHECK(in[1] == 0x20 && in[2] == 0x80 && in[3] == 0x14,
          "RDID %02x %02x %02x is not the M25PE80", in[1], in[2], in[3]);
    /* How the driver forms it (0x804b8102): (b1 << 16) | (b2 << 8) | b3. */
    CHECK(((uint32_t)in[1] << 16 | (uint32_t)in[2] << 8 | in[3]) == SPI_NOR_M25PE80_ID,
          "the driver's ID");
    CHECK(rdsr(&nor) == 0u, "status not clear at power-on");
    wren(&nor);
    CHECK(rdsr(&nor) == SPI_NOR_SR_WEL, "write enable did not latch");
    const uint8_t wrdi = 0x04;
    xfer(&nor, &wrdi, 1, NULL);
    CHECK(rdsr(&nor) == 0u, "write disable did not clear the latch");
    const uint8_t wren_long[2] = { 0x06, 0x00 };
    xfer(&nor, wren_long, 2, NULL);
    CHECK(rdsr(&nor) == 0u, "a write enable released off its octet boundary latched");
}

static void test_read(void) {
    spi_nor_t nor;
    fresh(&nor);
    for (unsigned i = 0; i < 8u; i++) g_mem[0xfa000 + i] = (uint8_t)(0x10 + i);
    g_mem[0] = 0x5a;
    uint8_t out[4 + 8], in[4 + 8];
    memset(out, 0xff, sizeof out);
    out[0] = 0x03; out[1] = 0x0f; out[2] = 0xa0; out[3] = 0x00;
    xfer(&nor, out, sizeof out, in);
    bool ok = true;
    for (unsigned i = 0; i < 8u; i++) ok &= in[4 + i] == (uint8_t)(0x10 + i);
    CHECK(ok && nor.reads == 1u && nor.read_octets == 8u, "READ at 0xfa000");
    /* The address wraps at the end of the array. */
    out[1] = 0x0f; out[2] = 0xff; out[3] = 0xff;
    xfer(&nor, out, 6, in);
    CHECK(in[4] == 0xff && in[5] == 0x5a, "READ did not wrap to 0");
}

static void test_program_and_erase(void) {
    spi_nor_t nor;
    fresh(&nor);
    uint8_t pp[4 + 3] = { 0x02, 0x00, 0x10, 0xfe, 0x0f, 0xf0, 0x3c };
    xfer(&nor, pp, sizeof pp, NULL);
    CHECK(g_mem[0x10fe] == 0xff && nor.refused == 1u && nor.programs == 0u,
          "a page program without write enable changed the array");
    wren(&nor);
    xfer(&nor, pp, sizeof pp, NULL);
    CHECK(g_mem[0x10fe] == 0x0f && g_mem[0x10ff] == 0xf0 && g_mem[0x1000] == 0x3c &&
          g_mem[0x1100] == 0xff && nor.programs == 1u && rdsr(&nor) == 0u,
          "page program: the third octet must wrap to the page's start, and the "
          "latch must clear");
    wren(&nor);
    uint8_t again[5] = { 0x02, 0x00, 0x10, 0xfe, 0xf0 };
    xfer(&nor, again, sizeof again, NULL);
    CHECK(g_mem[0x10fe] == 0x00, "programming set a bit it may only clear");

    /* More than a page: the last 256 octets are the ones programmed. */
    static uint8_t big[4 + 300];
    big[0] = 0x02; big[1] = 0x02; big[2] = 0x00; big[3] = 0x00;
    for (unsigned i = 0; i < 300u; i++) big[4 + i] = (uint8_t)i;
    wren(&nor);
    xfer(&nor, big, sizeof big, NULL);
    CHECK(g_mem[0x20000 + 44] == 44u && g_mem[0x20000 + 43] == (uint8_t)(256 + 43) &&
          g_mem[0x20000 + 255] == 255u && nor.programmed_octets == 3u + 1u + 256u,
          "an overlong program kept the wrong 256 octets");

    /* A 4 KiB erase clears its block and only its block. */
    const uint8_t se[4] = { 0x20, 0x00, 0x10, 0x80 };
    xfer(&nor, se, sizeof se, NULL);
    CHECK(g_mem[0x1000] == 0x3c && nor.refused == 2u, "an erase without write enable ran");
    wren(&nor);
    const uint8_t se_short[3] = { 0x20, 0x00, 0x10 };
    xfer(&nor, se_short, sizeof se_short, NULL);
    CHECK(g_mem[0x1000] == 0x3c && nor.erases == 0u,
          "an erase with a two-octet address ran");
    xfer(&nor, se, sizeof se, NULL);
    bool erased = true;
    for (unsigned i = 0x1000; i < 0x2000u; i++) erased &= g_mem[i] == 0xff;
    CHECK(erased && g_mem[0x20000] == 0u && nor.erases == 1u && rdsr(&nor) == 0u,
          "4 KiB erase");
}

static void test_status_write_and_unknown(void) {
    spi_nor_t nor;
    fresh(&nor);
    const uint8_t wrsr[2] = { 0x01, 0xff };
    xfer(&nor, wrsr, 2, NULL);
    CHECK(rdsr(&nor) == 0u && nor.refused == 1u, "a status write without write enable");
    wren(&nor);
    xfer(&nor, wrsr, 2, NULL);
    CHECK(rdsr(&nor) == SPI_NOR_SR_WRITABLE && nor.status_writes == 1u,
          "status write: only SRWD and BP2..BP0 are writable, and the latch clears");
    const uint8_t clear[2] = { 0x01, 0x00 };
    wren(&nor);
    xfer(&nor, clear, 2, NULL);
    CHECK(rdsr(&nor) == 0u, "the driver's unprotect (01 00) did not clear them");

    const uint8_t fast[6] = { 0x0b, 0x00, 0x00, 0x00, 0x00, 0x00 };
    uint8_t in[6];
    xfer(&nor, fast, 6, in);
    CHECK(nor.unknown == 1u && nor.last_unknown == 0x0b && in[5] == 0xff,
          "an unimplemented command was not refused and counted");
    const uint8_t deep[1] = { 0xb9 };
    xfer(&nor, deep, 1, NULL);
    CHECK(nor.unknown == 2u && rdsr(&nor) == 0u, "unknown commands changed state");
    spi_nor_select(&nor, true);
    spi_nor_reset(&nor);
    CHECK(!nor.selected && nor.status == 0u, "reset");
}

/*
 * The driver's ID read (0x804b80d0) as AppleSamsungSPI carries it on a
 * version 1 controller: select through the GPIO pin, CNT = 4, prefill, go,
 * then the filter drains the four answers.
 */
static void test_driver_read_id_through_the_controller(void) {
    spi_nor_t nor;
    fresh(&nor);
    s5l_spi_t bus;
    s5l_spi_slave_t slave;
    s5l_spi_reset(&bus);
    CHECK(s5l_spi_set_version(&bus, 1u), "version 1");
    spi_nor_bind(&nor, &slave);
    CHECK(s5l_spi_attach(&bus, 0u, &slave), "attach");

    s5l_spi_write(&bus, SPI_STATUS, SPI_STATUS_EVENTS_V1);
    s5l_spi_write(&bus, SPI_SETUP, 0x4018u);
    s5l_spi_write(&bus, SPI_CONTROL, SPI_CONTROL_START);
    s5l_spi_write(&bus, SPI_CNT, 4u);
    s5l_spi_write(&bus, SPI_CNT_V1, 4u);
    spi_nor_select(&nor, true);                      /* the GPIO pin, low  */
    s5l_spi_write(&bus, SPI_SETUP, 0x4038u);
    const uint8_t tx[4] = { 0x9f, 0xff, 0xff, 0xff };
    for (unsigned i = 0; i < 4u; i++) s5l_spi_write(&bus, SPI_TXDATA, tx[i]);
    s5l_spi_write(&bus, SPI_SETUP, 0x2041b8u);
    CHECK(s5l_spi_irq(&bus), "no completion interrupt");
    const uint32_t s = s5l_spi_read(&bus, SPI_STATUS);
    const unsigned have = (s >> SPI_STATUS_RX_SHIFT_V1) & SPI_STATUS_LEVEL_V1;
    uint8_t rx[4] = {0};
    for (unsigned i = 0; i < have && i < 4u; i++) rx[i] = (uint8_t)s5l_spi_read(&bus, SPI_RXDATA);
    s5l_spi_write(&bus, SPI_STATUS, s);
    s5l_spi_write(&bus, SPI_SETUP, 0x4018u | 0x200000u);
    spi_nor_select(&nor, false);                     /* the GPIO pin, high */
    CHECK(have == 4u && rx[1] == 0x20 && rx[2] == 0x80 && rx[3] == 0x14,
          "the driver read %u octets, ID %02x %02x %02x", have, rx[1], rx[2], rx[3]);
    CHECK(!s5l_spi_irq(&bus), "the line stayed up after the filter");
}

int main(void) {
    printf("NEON SPI NOR flash tests\n");
    test_init_and_idle();
    test_read_id_and_status();
    test_read();
    test_program_and_erase();
    test_status_write_and_unknown();
    test_driver_read_id_through_the_controller();
    printf("  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
