/*
 * NEON — the S5L8920 SHA-1 engine: FIPS 180 test vectors driven the way
 * iPhone OS 3.1.3's AppleS5L8920XSHA1 drives it (its own padding, whole
 * blocks through the FIFO, the state read and written byte-reversed), a hash
 * continued across two requests, and the same through CDMA channel 4.
 */
#include "s5l_sha1.h"
#include "cdma.h"

#include <stdio.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(cond, ...) do { \
    if (cond) g_pass++; \
    else { g_fail++; printf("  FAIL %s:%d: ", __func__, __LINE__); \
           printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static uint32_t bswap(uint32_t x) {
    return (x >> 24) | ((x >> 8) & 0xff00u) | ((x << 8) & 0xff0000u) | (x << 24);
}

/* The driver's padding (0xc0471134): 0x80, zeros, the length in bits as a
 * big-endian 64-bit number, to a whole number of blocks. */
static size_t pad(const char *msg, uint8_t *out) {
    const size_t n = strlen(msg);
    memcpy(out, msg, n);
    size_t len = n;
    out[len++] = 0x80;
    while ((len % 64u) != 56u) out[len++] = 0;
    const uint64_t bits = (uint64_t)n * 8u;
    for (int i = 7; i >= 0; i--) out[len++] = (uint8_t)(bits >> (8 * i));
    return len;
}

/* The digest as the completion reads it (0xc04714f8): rev of +0x20..+0x30. */
static void digest(const s5l_sha1_t *s, uint32_t h[5]) {
    for (unsigned i = 0; i < 5u; i++) h[i] = bswap(s5l_sha1_read(s, 0x20u + 4u * i));
}

static void test_vectors(void) {
    static const struct { const char *msg; uint32_t h[5]; } v[] = {
        { "abc", { 0xa9993e36u, 0x4706816au, 0xba3e2571u, 0x7850c26cu, 0x9cd0d89du } },
        { "", { 0xda39a3eeu, 0x5e6b4b0du, 0x3255bfefu, 0x95601890u, 0xafd80709u } },
        { "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
          { 0x84983e44u, 0x1c3bd26eu, 0xbaae4aa1u, 0xf95129e5u, 0xe54670f1u } },
    };
    for (unsigned t = 0; t < sizeof v / sizeof v[0]; t++) {
        s5l_sha1_t s;
        s5l_sha1_reset(&s);
        uint8_t buf[192];
        const size_t n = pad(v[t].msg, buf);
        /* A request that begins a hash: reset, then start without state. */
        s5l_sha1_write(&s, 0x4, 1u);
        s5l_sha1_write(&s, 0x10, 0u);
        s5l_sha1_write(&s, 0x0, 0x2u);
        s5l_sha1_feed(&s, buf, n);
        uint32_t h[5];
        digest(&s, h);
        CHECK(memcmp(h, v[t].h, sizeof h) == 0, "\"%s\": %08x...", v[t].msg, h[0]);
    }
}

/* Two requests: the second continues from the state the first left, loaded
 * the way the driver loads it (0xc04711ec), with control 0xA. */
static void test_continued(void) {
    const char *msg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    uint8_t buf[128];
    const size_t n = pad(msg, buf);
    s5l_sha1_t s;
    s5l_sha1_reset(&s);
    s5l_sha1_write(&s, 0x4, 1u);
    s5l_sha1_write(&s, 0x0, 0x2u);
    s5l_sha1_feed(&s, buf, 64);
    uint32_t mid[5];
    digest(&s, mid);

    s5l_sha1_t t;
    s5l_sha1_reset(&t);
    s5l_sha1_write(&t, 0x4, 1u);
    s5l_sha1_write(&t, 0x10, 0u);
    for (unsigned i = 0; i < 5u; i++) s5l_sha1_write(&t, 0x20u + 4u * i, bswap(mid[i]));
    CHECK(s5l_sha1_read(&t, 0x20) == 0x01234567u || mid[0] != 0x67452301u,
          "state registers are byte-reversed");
    s5l_sha1_write(&t, 0x0, 0xau);
    s5l_sha1_feed(&t, buf + 64, n - 64);
    uint32_t h[5];
    digest(&t, h);
    CHECK(h[0] == 0x84983e44u && h[4] == 0xe54670f1u, "continued hash %08x", h[0]);

    /* The FIFO by CPU stores, a word at a time in memory order. */
    s5l_sha1_reset(&s);
    s5l_sha1_write(&s, 0x0, 0x2u);
    uint8_t abc[64];
    pad("abc", abc);
    for (unsigned i = 0; i < 64u; i += 4u)
        s5l_sha1_write(&s, 0xa0, (uint32_t)abc[i] | (uint32_t)abc[i+1] << 8 |
                                 (uint32_t)abc[i+2] << 16 | (uint32_t)abc[i+3] << 24);
    digest(&s, h);
    CHECK(h[0] == 0xa9993e36u && s.blocks == 1u, "FIFO stores");
}

/* CDMA channel 4 as 3.1.3's AppleCDMA programs it, into the FIFO. */
#define RAM_PA   UINT32_C(0x40000000)
static uint8_t g_ram[0x10000];
static s5l_sha1_t g_sha;
static bool mem(void *ctx, uint32_t pa, uint8_t *buf, uint32_t len, bool write) {
    (void)ctx;
    if (pa < RAM_PA || (uint64_t)pa + len > RAM_PA + sizeof g_ram) return false;
    if (write) memcpy(g_ram + (pa - RAM_PA), buf, len);
    else memcpy(buf, g_ram + (pa - RAM_PA), len);
    return true;
}
static bool periph(void *ctx, uint32_t fifo, const uint8_t *data, uint32_t len) {
    (void)ctx;
    if (fifo != 0x801000a0u) return false;
    s5l_sha1_feed(&g_sha, data, len);
    return true;
}
static void put32(uint32_t pa, uint32_t v) {
    uint8_t *p = g_ram + (pa - RAM_PA);
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void test_through_cdma(void) {
    cdma_t d;
    CHECK(cdma_init(&d, mem, NULL), "init");
    cdma_set_peripheral(&d, periph, NULL);
    s5l_sha1_reset(&g_sha);
    s5l_sha1_write(&g_sha, 0x4, 1u);
    s5l_sha1_write(&g_sha, 0x0, 0x2u);
    pad("abc", g_ram + 0x2000);
    put32(RAM_PA + 0x1000, RAM_PA + 0x1020);
    put32(RAM_PA + 0x1004, 0x303u);
    put32(RAM_PA + 0x1008, RAM_PA + 0x2000);
    put32(RAM_PA + 0x100c, 64u);
    put32(RAM_PA + 0x1024, 0u);                       /* the terminator */
    cdma_write(&d, 0x4000, 2u);
    cdma_write(&d, 0x4000, cdma_read(&d, 0x4000));
    cdma_write(&d, 0x4000, 0x18u);
    cdma_write(&d, 0x4004, 0xcau);
    cdma_write(&d, 0x4008, 0x801000a0u);
    cdma_write(&d, 0x4014, RAM_PA + 0x1000);
    cdma_write(&d, 0x4000, 0x19u);
    uint32_t h[5];
    digest(&g_sha, h);
    CHECK(h[0] == 0xa9993e36u && (cdma_read(&d, 0x4000) & CDMA_CSR_DONE) && cdma_irq(&d, 4) &&
          d.periph_transfers == 1u && d.periph_octets == 64u, "SHA-1 through channel 4");

    /* A FIFO nothing claims: the channel keeps running. */
    cdma_write(&d, 0x4000, cdma_read(&d, 0x4000));
    cdma_write(&d, 0x5008, 0x82000010u);
    cdma_write(&d, 0x5014, RAM_PA + 0x1000);
    cdma_write(&d, 0x5000, 0x19u);
    CHECK((cdma_read(&d, 0x5000) & CDMA_CSR_RUNNING) && !cdma_irq(&d, 5) &&
          d.periph_unclaimed == 1u, "unclaimed peripheral request stays running");
    cdma_set_peripheral(&d, NULL, NULL);
    cdma_write(&d, 0x6014, RAM_PA + 0x1000);
    cdma_write(&d, 0x6000, 0x19u);
    CHECK(d.periph_unclaimed == 2u, "no peripheral handler at all");
    cdma_free(&d);
}

int main(void) {
    printf("NEON SHA-1 engine tests\n");
    test_vectors();
    test_continued();
    test_through_cdma();
    printf("  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
