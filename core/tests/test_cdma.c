/*
 * NEON — the S5L8920 CDMA engine and AES contexts: copies, the hardware-key
 * and register-key AES paths against the software AES, the interrupt and
 * status protocol, and the request iOS 6 made in a boot, register by
 * register.
 */
#include "cdma.h"
#include "aes.h"

#include <stdio.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(cond, ...) do { \
    if (cond) g_pass++; \
    else { g_fail++; printf("  FAIL %s:%d: ", __func__, __LINE__); \
           printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define RAM_PA   UINT32_C(0x40000000)
#define RAM_SIZE (1u << 20)
static uint8_t g_ram[RAM_SIZE];

static bool mem(void *ctx, uint32_t pa, uint8_t *buf, uint32_t len, bool write) {
    (void)ctx;
    if (pa < RAM_PA || (uint64_t)pa + len > (uint64_t)RAM_PA + RAM_SIZE) return false;
    if (write) memcpy(g_ram + (pa - RAM_PA), buf, len);
    else memcpy(buf, g_ram + (pa - RAM_PA), len);
    return true;
}

static void put32(uint32_t pa, uint32_t v) {
    uint8_t *p = g_ram + (pa - RAM_PA);
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void desc(uint32_t at, uint32_t next, uint32_t cmd, uint32_t addr, uint32_t len) {
    put32(at, next); put32(at + 4, cmd); put32(at + 8, addr); put32(at + 12, len);
}
static uint32_t ch(unsigned n, uint32_t reg) { return (n << 12) + reg; }

/* AppleCDMA's start of one channel (0x808a46d8 and 0x808a404e). */
static void start(cdma_t *d, unsigned n, uint32_t chain, unsigned ctx, uint32_t len) {
    const uint32_t base = 0x88u | (ctx << 8);
    if (!(cdma_read(d, ch(n, CDMA_CSR)) & CDMA_CSR_STATE)) {
        cdma_write(d, ch(n, CDMA_CSR), base | CDMA_CSR_ABORT);
        cdma_write(d, ch(n, CDMA_CSR), cdma_read(d, ch(n, CDMA_CSR)));
    }
    cdma_write(d, ch(n, CDMA_CAR), chain);
    cdma_write(d, ch(n, CDMA_CSR), base);
    cdma_write(d, ch(n, CDMA_DBR), len);
    cdma_write(d, ch(n, CDMA_CSR), cdma_read(d, ch(n, CDMA_CSR)) | CDMA_CSR_GO);
}
/* The interrupt: read CSR, write it back. */
static uint32_t ack(cdma_t *d, unsigned n) {
    const uint32_t s = cdma_read(d, ch(n, CDMA_CSR));
    cdma_write(d, ch(n, CDMA_CSR), s);
    return s;
}

static void aes_context(cdma_t *d, unsigned k, uint32_t control, const uint8_t iv[16],
                        const uint8_t *key, unsigned key_len) {
    const uint32_t b = 0x1000u * k;
    cdma_aes_write(d, b, control);
    for (unsigned i = 0; i < 4u; i++)
        cdma_aes_write(d, b + 0x10u + 4u * i, iv ? (uint32_t)iv[4*i] | (uint32_t)iv[4*i+1] << 8 |
                                                   (uint32_t)iv[4*i+2] << 16 | (uint32_t)iv[4*i+3] << 24 : 0u);
    for (unsigned i = 0; key && i < key_len / 4u; i++)
        cdma_aes_write(d, b + 0x20u + 4u * i, (uint32_t)key[4*i] | (uint32_t)key[4*i+1] << 8 |
                                              (uint32_t)key[4*i+2] << 16 | (uint32_t)key[4*i+3] << 24);
}

static void test_copy_across_segments(void) {
    cdma_t d;
    CHECK(cdma_init(&d, mem, NULL), "init");
    memset(g_ram, 0, sizeof g_ram);
    for (unsigned i = 0; i < 100u; i++) g_ram[0x1000 + i] = (uint8_t)(i * 5 + 1);
    /* Source: 60 + 40 octets; sink: 30 + 70, linked out of order. */
    desc(RAM_PA + 0x100, RAM_PA + 0x180, CDMA_CMD_DATA, RAM_PA + 0x1000, 60);
    desc(RAM_PA + 0x180, 0, CDMA_CMD_DATA | CDMA_CMD_LAST, RAM_PA + 0x103c, 40);
    desc(RAM_PA + 0x200, RAM_PA + 0x240, CDMA_CMD_DATA, RAM_PA + 0x2000, 30);
    desc(RAM_PA + 0x240, 0, CDMA_CMD_DATA | CDMA_CMD_LAST, RAM_PA + 0x3000, 70);
    start(&d, 3, RAM_PA + 0x100, 0, 100);
    CHECK(d.transfers == 0u && (cdma_read(&d, ch(3, 0)) & CDMA_CSR_RUNNING) && !cdma_irq(&d, 3),
          "one channel of the pair ran alone");
    start(&d, 4, RAM_PA + 0x200, 0, 100);
    CHECK(memcmp(g_ram + 0x2000, g_ram + 0x1000, 30) == 0 &&
          memcmp(g_ram + 0x3000, g_ram + 0x1000 + 30, 70) == 0 && d.octets == 100u,
          "the copy did not follow both chains");
    CHECK(cdma_irq(&d, 3) && cdma_irq(&d, 4) && cdma_read(&d, ch(4, CDMA_DBR)) == 0u &&
          !(cdma_read(&d, ch(3, 0)) & CDMA_CSR_STATE), "completion");
    const uint32_t s = ack(&d, 3);
    CHECK((s & CDMA_CSR_DONE) && !(s & CDMA_CSR_ERROR) && !cdma_irq(&d, 3) && cdma_irq(&d, 4),
          "writing CSR back clears done on that channel only");
    ack(&d, 4);
    CHECK(!cdma_irq(&d, 4) && (cdma_read(&d, ch(4, 0)) & 0xffffu) == 0x88u,
          "the driver's control bits survive the acknowledge");
    cdma_free(&d);
}

static void test_uid_request_as_ios6_made_it(void) {
    cdma_t d;
    CHECK(cdma_init(&d, mem, NULL), "init");
    memset(g_ram, 0, sizeof g_ram);
    uint8_t plain[16];
    for (unsigned i = 0; i < 16u; i++) plain[i] = (uint8_t)(0xa0 + i);
    memcpy(g_ram + 0x5280, plain, 16);
    /* The boot's: context 1 = 0x30100 with a zero IV; channel 1 0x30103
     * over the input, channel 2 0x103 over the output; 16 octets. */
    aes_context(&d, 1, 0x30100u, NULL, NULL, 0);
    desc(RAM_PA + 0xe000, RAM_PA + 0xe020, 0x30103u, RAM_PA + 0x5280, 16);
    desc(RAM_PA + 0xf000, RAM_PA + 0xf020, 0x00103u, RAM_PA + 0x6d00, 16);
    start(&d, 1, RAM_PA + 0xe000, 1, 16);
    start(&d, 2, RAM_PA + 0xf000, 0, 16);
    uint8_t want[16];
    aes_ctx_t a;
    aes_init(&a, CDMA_STANDIN_KEY, 128);
    static const uint8_t zero_iv[16];
    aes_cbc_encrypt(&a, zero_iv, plain, want, 16);
    CHECK(memcmp(g_ram + 0x6d00, want, 16) == 0 && d.aes_ops == 1u && d.hardware_key_ops == 1u,
          "a UID encrypt is AES-128-CBC under the stand-in key");
    CHECK(memcmp(g_ram + 0x5280, plain, 16) == 0, "the source changed");
    CHECK(cdma_irq(&d, 1) && cdma_irq(&d, 2), "both lines up");

    /* And back: a UID decrypt of that output gives the input again. */
    ack(&d, 1); ack(&d, 2);
    aes_context(&d, 1, 0x20100u, NULL, NULL, 0);
    desc(RAM_PA + 0xe000, 0, 0x30103u, RAM_PA + 0x6d00, 16);
    desc(RAM_PA + 0xf000, 0, 0x00103u, RAM_PA + 0x7000, 16);
    start(&d, 1, RAM_PA + 0xe000, 1, 16);
    start(&d, 2, RAM_PA + 0xf000, 0, 16);
    CHECK(memcmp(g_ram + 0x7000, plain, 16) == 0, "a UID decrypt did not round-trip");
    cdma_free(&d);
}

static void test_register_key_and_ecb(void) {
    cdma_t d;
    CHECK(cdma_init(&d, mem, NULL), "init");
    memset(g_ram, 0, sizeof g_ram);
    uint8_t key[32], iv[16], plain[48], want[48];
    for (unsigned i = 0; i < 32u; i++) key[i] = (uint8_t)(i * 7 + 3);
    for (unsigned i = 0; i < 16u; i++) iv[i] = (uint8_t)(0x55 ^ i);
    for (unsigned i = 0; i < 48u; i++) plain[i] = (uint8_t)(i * 11);
    aes_ctx_t a;
    aes_init(&a, key, 256);
    aes_cbc_encrypt(&a, iv, plain, want, 48);
    memcpy(g_ram + 0x1000, want, 48);
    /* A 256-bit register key, CBC decrypt, on context 3 for channels 5/6. */
    aes_context(&d, 3, CDMA_AES_CUSTOM_KEY | 2u << CDMA_AES_SIZE_SHIFT | CDMA_AES_CBC | 5u << 8,
                iv, key, 32);
    desc(RAM_PA + 0x100, 0, CDMA_CMD_DATA | CDMA_CMD_LAST, RAM_PA + 0x1000, 48);
    desc(RAM_PA + 0x200, 0, CDMA_CMD_DATA | CDMA_CMD_LAST, RAM_PA + 0x2000, 48);
    start(&d, 5, RAM_PA + 0x100, 3, 48);
    start(&d, 6, RAM_PA + 0x200, 0, 48);
    CHECK(memcmp(g_ram + 0x2000, plain, 48) == 0 && d.hardware_key_ops == 0u,
          "a register-key CBC decrypt");

    /* ECB: each block on its own, so two equal blocks stay equal. */
    ack(&d, 5); ack(&d, 6);
    memset(g_ram + 0x1000, 0x3c, 32);
    aes_context(&d, 3, CDMA_AES_CUSTOM_KEY | CDMA_AES_ENCRYPT | 5u << 8, NULL, key, 16);
    desc(RAM_PA + 0x100, 0, CDMA_CMD_DATA | CDMA_CMD_LAST, RAM_PA + 0x1000, 32);
    desc(RAM_PA + 0x200, 0, CDMA_CMD_DATA | CDMA_CMD_LAST, RAM_PA + 0x2000, 32);
    start(&d, 5, RAM_PA + 0x100, 3, 32);
    start(&d, 6, RAM_PA + 0x200, 0, 32);
    uint8_t blk[16];
    aes_init(&a, key, 128);
    aes_encrypt_block(&a, g_ram + 0x1000, blk);
    CHECK(memcmp(g_ram + 0x2000, blk, 16) == 0 && memcmp(g_ram + 0x2010, blk, 16) == 0,
          "128-bit ECB encrypt");
    cdma_free(&d);
}

static void test_errors_and_abort(void) {
    cdma_t d;
    CHECK(cdma_init(&d, mem, NULL), "init");
    memset(g_ram, 0, sizeof g_ram);
    desc(RAM_PA + 0x100, 0, CDMA_CMD_DATA | CDMA_CMD_LAST, 0x10000000u, 16);   /* not memory */
    desc(RAM_PA + 0x200, 0, CDMA_CMD_DATA | CDMA_CMD_LAST, RAM_PA + 0x2000, 16);
    start(&d, 1, RAM_PA + 0x100, 0, 16);
    start(&d, 2, RAM_PA + 0x200, 0, 16);
    CHECK((cdma_read(&d, ch(1, 0)) & CDMA_CSR_ERROR) && (cdma_read(&d, ch(2, 0)) & CDMA_CSR_ERROR) &&
          cdma_read(&d, ch(1, CDMA_ERR)) && d.errors == 1u && cdma_irq(&d, 1),
          "a chain over non-memory ends both channels in error");

    /* A chain that loops on itself is cut off, not walked forever. */
    ack(&d, 1); ack(&d, 2);
    desc(RAM_PA + 0x100, RAM_PA + 0x100, CDMA_CMD_DATA, RAM_PA + 0x1000, 16);
    start(&d, 1, RAM_PA + 0x100, 0, 16);
    start(&d, 2, RAM_PA + 0x200, 0, 16);
    CHECK(d.errors == 2u, "a looping chain");

    /* Abort stops a channel waiting for its partner. */
    ack(&d, 1); ack(&d, 2);
    start(&d, 7, RAM_PA + 0x100, 0, 16);
    cdma_write(&d, ch(7, 0), 0x88u | CDMA_CSR_ABORT);
    CHECK(!(cdma_read(&d, ch(7, 0)) & CDMA_CSR_STATE), "abort");
    start(&d, 8, RAM_PA + 0x200, 0, 16);
    CHECK(d.transfers == 2u, "an aborted partner still ran");
    /* An AES context past the eighth is an error, not a read past the window. */
    desc(RAM_PA + 0x100, 0, CDMA_CMD_DATA | CDMA_CMD_LAST, RAM_PA + 0x1000, 16);
    start(&d, 9, RAM_PA + 0x100, 9, 16);
    start(&d, 10, RAM_PA + 0x200, 0, 16);
    CHECK(d.errors == 3u, "context 9");

    cdma_write(&d, 0x0, 0x6u);
    cdma_write(&d, 0x8, 0x2u);
    CHECK(cdma_read(&d, 0x10) == 0x4u, "the global enable bits");
    cdma_reset(&d);
    CHECK(cdma_read(&d, ch(1, 0)) == 0u && !cdma_irq(&d, 1) && cdma_aes_read(&d, 0x1000) == 0u,
          "reset");
    CHECK(!cdma_irq(&d, 0) && !cdma_irq(&d, CDMA_CHANNELS) && cdma_read(&d, CDMA_SIZE) == 0u,
          "out of range");
    cdma_free(&d);
}

/* Where CAR is left: past what the engine consumed. iPhone OS 3.1.3's
 * AppleCDMA keeps a ring of 128 descriptors and finds its progress there;
 * a request queued while the channel is busy replaces the terminator and
 * starts the channel again without touching CAR. */
static uint32_t g_taken[4], g_takes;
static bool sink(void *ctx, uint32_t fifo, const uint8_t *data, uint32_t len) {
    (void)ctx; (void)data;
    if (fifo != 0x801000a0u) return false;
    if (g_takes < 4u) g_taken[g_takes] = len;
    g_takes++;
    return true;
}

static void test_car_after_requests(void) {
    cdma_t d;
    CHECK(cdma_init(&d, mem, NULL), "init");
    memset(g_ram, 0, sizeof g_ram);
    /* Memory to memory: CAR ends past each channel's last descriptor. */
    desc(RAM_PA + 0x100, RAM_PA + 0x120, CDMA_CMD_DATA | CDMA_CMD_LAST, RAM_PA + 0x1000, 32);
    desc(RAM_PA + 0x200, RAM_PA + 0x220, CDMA_CMD_DATA, RAM_PA + 0x2000, 32);
    start(&d, 1, RAM_PA + 0x100, 0, 32);
    start(&d, 2, RAM_PA + 0x200, 0, 32);
    CHECK(cdma_read(&d, ch(1, CDMA_CAR)) == RAM_PA + 0x120 &&
          cdma_read(&d, ch(2, CDMA_CAR)) == RAM_PA + 0x220, "CAR past the copy's descriptors");

    /* The ring: descriptor i links to i + 1, the last back to the first. */
    const uint32_t ring = RAM_PA + 0x8000;
    for (unsigned i = 0; i < 128u; i++)
        desc(ring + 32u * i, ring + 32u * ((i + 1u) & 127u), 0, 0, 0);
    cdma_set_peripheral(&d, sink, NULL);
    cdma_write(&d, ch(4, CDMA_CSR), 0x18u);
    cdma_write(&d, ch(4, 0x4), 0xcau);
    cdma_write(&d, ch(4, CDMA_DAR), 0x801000a0u);
    cdma_write(&d, ch(4, CDMA_CAR), ring);
    /* Request one: two descriptors, 0..1, the last with 0x300. */
    put32(ring + 0x24, 0x303u); put32(ring + 0x28, RAM_PA + 0x3000); put32(ring + 0x2c, 64u);
    put32(ring + 0x04, 0x3u);   put32(ring + 0x08, RAM_PA + 0x4000); put32(ring + 0x0c, 128u);
    cdma_write(&d, ch(4, CDMA_CSR), 0x19u);
    CHECK(g_takes == 1u && g_taken[0] == 192u && cdma_read(&d, ch(4, CDMA_CAR)) == ring + 0x40u &&
          cdma_irq(&d, 4), "the first request, CAR at its terminator");
    /* Request two, queued in place of the terminator: descriptor 2. */
    put32(ring + 0x48, RAM_PA + 0x5000); put32(ring + 0x4c, 64u);
    put32(ring + 0x44, 0x303u);
    cdma_write(&d, ch(4, CDMA_CSR), 0x19u);
    CHECK(g_takes == 2u && g_taken[1] == 64u && cdma_read(&d, ch(4, CDMA_CAR)) == ring + 0x60u,
          "the second carries on from CAR");
    /* Descriptor 127 wraps: CAR comes back to the ring's start. */
    cdma_write(&d, ch(4, CDMA_CAR), ring + 127u * 32u);
    put32(ring + 127u * 32u + 8u, RAM_PA + 0x6000); put32(ring + 127u * 32u + 12u, 64u);
    put32(ring + 127u * 32u + 4u, 0x303u);
    cdma_write(&d, ch(4, CDMA_CSR), 0x19u);
    CHECK(g_takes == 3u && cdma_read(&d, ch(4, CDMA_CAR)) == ring, "wraps to the ring's start");
    /* Unclaimed: CAR stays where it was. */
    cdma_write(&d, ch(4, CDMA_DAR), 0x82000010u);
    cdma_write(&d, ch(4, CDMA_CSR), 0x19u);
    CHECK(g_takes == 3u && cdma_read(&d, ch(4, CDMA_CAR)) == ring, "unclaimed leaves CAR");
    cdma_free(&d);
}

/* A peripheral that is not asking for data yet: the request waits, running,
 * and goes through on cdma_retry() once the device is ready (spi1's driver
 * starts channel 18 before it sets the port's DMA bit). */
static bool g_ready;
static uint32_t g_got;
static bool late_sink(void *ctx, uint32_t fifo, const uint8_t *data, uint32_t len) {
    (void)ctx; (void)data;
    if (fifo != 0x82100010u || !g_ready) return false;
    g_got += len;
    return true;
}

static void test_retry_when_ready(void) {
    cdma_t d;
    CHECK(cdma_init(&d, mem, NULL), "init");
    memset(g_ram, 0, sizeof g_ram);
    desc(RAM_PA + 0x100, RAM_PA + 0x120, 0x303u, RAM_PA + 0x1000, 96);
    cdma_set_peripheral(&d, late_sink, NULL);
    g_ready = false;
    g_got = 0;
    cdma_write(&d, ch(18, CDMA_CSR), 0x18u);
    cdma_write(&d, ch(18, CDMA_DAR), 0x82100010u);
    cdma_write(&d, ch(18, CDMA_CAR), RAM_PA + 0x100);
    cdma_write(&d, ch(18, CDMA_CSR), 0x19u);
    CHECK((cdma_read(&d, ch(18, CDMA_CSR)) & CDMA_CSR_RUNNING) && !cdma_irq(&d, 18) && g_got == 0u,
          "not ready: still running");
    cdma_retry(&d);
    CHECK(g_got == 0u, "a retry before the device is ready changes nothing");
    g_ready = true;
    cdma_retry(&d);
    CHECK(g_got == 96u && cdma_irq(&d, 18) && cdma_read(&d, ch(18, CDMA_CAR)) == RAM_PA + 0x120u,
          "ready: delivered, done, CAR past the descriptor");
    cdma_retry(&d);
    CHECK(g_got == 96u, "a finished request is not sent twice");
    cdma_free(&d);
}

/* A paced channel, as iPhone OS 3.1.3 plays sound through channel 21 into
 * i2s0 (cdma.h, "paced channels"): the device pulls the data as it plays,
 * the channel runs past marked descriptors raising bit 20, stops at the
 * terminator, and answers the driver's pause and stop. */
static bool i2s_fifo(void *ctx, uint32_t fifo) {
    (void)ctx;
    return fifo == 0x84500000u;
}

static void test_paced_channel(void) {
    cdma_t d;
    CHECK(cdma_init(&d, mem, NULL), "init");
    memset(g_ram, 0, sizeof g_ram);
    for (unsigned i = 0; i < 0x5000u; i++) g_ram[0x3000 + i] = (uint8_t)(i * 7 + 3);
    const uint32_t ring = RAM_PA + 0x8000;
    for (unsigned i = 0; i < 128u; i++)
        desc(ring + 32u * i, ring + 32u * ((i + 1u) & 127u), 0, 0, 0);
    cdma_set_peripheral(&d, sink, NULL);
    cdma_set_paced(&d, i2s_fifo, NULL);
    g_takes = 0;
    const unsigned n = 21;
    cdma_write(&d, ch(n, CDMA_CSR), 0x18u);
    cdma_write(&d, ch(n, 0x4), 0xa6u);
    cdma_write(&d, ch(n, CDMA_DAR), 0x84500000u);
    cdma_write(&d, ch(n, CDMA_CAR), ring);
    /* Command A: descriptors 0 and 1, the last marked; 2 terminates. */
    put32(ring + 0x24, 0x303u); put32(ring + 0x28, RAM_PA + 0x4000); put32(ring + 0x2c, 32u);
    put32(ring + 0x04, 0x3u);   put32(ring + 0x08, RAM_PA + 0x3000); put32(ring + 0x0c, 64u);
    cdma_write(&d, ch(n, CDMA_CSR), 0x19u);
    CHECK(cdma_paced_running(&d, n) && g_takes == 0u && !cdma_irq(&d, n) &&
          cdma_read(&d, ch(n, CDMA_CUR)) == RAM_PA + 0x3000u,
          "started: nothing delivered, CUR at the first octet");
    CHECK(cdma_bytes_to_event(&d, n, 1u << 16) == 96u, "the event is the marked descriptor's end");

    uint8_t out[256];
    CHECK(cdma_pull(&d, n, out, 40) == 40u && !memcmp(out, g_ram + 0x3000, 40) &&
          cdma_read(&d, ch(n, CDMA_CUR)) == RAM_PA + 0x3028u && !cdma_irq(&d, n),
          "40 octets from the first descriptor");
    CHECK(cdma_pull(&d, n, out, 40) == 40u && !memcmp(out, g_ram + 0x3028, 24) &&
          !memcmp(out + 24, g_ram + 0x4000, 16) && cdma_read(&d, ch(n, CDMA_CAR)) == ring + 0x20u &&
          cdma_read(&d, ch(n, CDMA_CUR)) == RAM_PA + 0x4010u && !cdma_irq(&d, n),
          "across into the second, CAR and CUR on it");
    CHECK(cdma_bytes_to_event(&d, n, 1u << 16) == 16u, "16 left to the event");

    /* Command B linked in place of the terminator while A plays; the kick
     * changes nothing about where A is. */
    put32(ring + 0x48, RAM_PA + 0x5000); put32(ring + 0x4c, 48u);
    put32(ring + 0x44, 0x303u);
    cdma_write(&d, ch(n, CDMA_CSR), 0x19u);
    CHECK(cdma_read(&d, ch(n, CDMA_CUR)) == RAM_PA + 0x4010u, "a kick while running keeps CUR");
    CHECK(cdma_pull(&d, n, out, 100) == 64u && !memcmp(out + 16, g_ram + 0x5000, 48),
          "the rest of A, then B, then the terminator");
    const uint32_t csr = cdma_read(&d, ch(n, CDMA_CSR));
    CHECK(!(csr & CDMA_CSR_STATE) && (csr & CDMA_CSR_DONE) && (csr & CDMA_CSR_SEGMENT) &&
          cdma_irq(&d, n) && cdma_read(&d, ch(n, CDMA_CAR)) == ring + 0x60u &&
          g_takes == 0u, "done and marked, CAR on the terminator, nothing sent to a sink");
    ack(&d, n);
    CHECK(!cdma_irq(&d, n) && cdma_pull(&d, n, out, 4) == 0u, "acknowledged; idle");

    /* Command C, then the driver's position read: pause, read CUR, split the
     * descriptor, restart. */
    put32(ring + 0x68, RAM_PA + 0x6000); put32(ring + 0x6c, 64u);
    put32(ring + 0x64, 0x303u);
    cdma_write(&d, ch(n, CDMA_CSR), 0x19u);
    CHECK(cdma_pull(&d, n, out, 20) == 20u, "C plays");
    cdma_write(&d, ch(n, CDMA_CSR), cdma_read(&d, ch(n, CDMA_CSR)) | CDMA_CSR_PAUSE);
    CHECK((cdma_read(&d, ch(n, CDMA_CSR)) & CDMA_CSR_STATE) == CDMA_CSR_PAUSED &&
          cdma_pull(&d, n, out, 4) == 0u && cdma_read(&d, ch(n, CDMA_CUR)) == RAM_PA + 0x6014u,
          "paused: nothing plays, CUR is the position");
    put32(ring + 0x68, RAM_PA + 0x6014); put32(ring + 0x6c, 44u);
    cdma_write(&d, ch(n, CDMA_CSR),
               (cdma_read(&d, ch(n, CDMA_CSR)) & ~UINT32_C(0x21)) | CDMA_CSR_GO);
    CHECK(cdma_paced_running(&d, n) && cdma_pull(&d, n, out, 64) == 44u &&
          !memcmp(out, g_ram + 0x6014, 44), "restarted from the split descriptor");
    ack(&d, n);

    /* Command D, stopped: bit 2 halts it and bit 21 says so; abort clears. */
    put32(ring + 0x88, RAM_PA + 0x7000); put32(ring + 0x8c, 64u);
    put32(ring + 0x84, 0x303u);
    cdma_write(&d, ch(n, CDMA_CSR), 0x19u);
    cdma_write(&d, ch(n, CDMA_CSR), cdma_read(&d, ch(n, CDMA_CSR)) | CDMA_CSR_STOP);
    CHECK((cdma_read(&d, ch(n, CDMA_CSR)) & CDMA_CSR_STOPPED) && !cdma_paced_running(&d, n) &&
          cdma_pull(&d, n, out, 4) == 0u, "stopped, and says so");
    cdma_write(&d, ch(n, CDMA_CSR), CDMA_CSR_ABORT);
    cdma_write(&d, ch(n, CDMA_CSR), cdma_read(&d, ch(n, CDMA_CSR)) | 0x18u);
    CHECK(!(cdma_read(&d, ch(n, CDMA_CSR)) & (CDMA_CSR_STOPPED | CDMA_CSR_STATE)),
          "abort clears the acknowledge");

    /* A descriptor that is not memory ends the request in error. */
    cdma_write(&d, ch(n, CDMA_CAR), ring + 0xa0u);
    put32(ring + 0xa8, 0x10000000u); put32(ring + 0xac, 64u);
    put32(ring + 0xa4, 0x303u);
    cdma_write(&d, ch(n, CDMA_CSR), 0x19u);
    CHECK(cdma_pull(&d, n, out, 8) == 0u &&
          (cdma_read(&d, ch(n, CDMA_CSR)) & CDMA_CSR_ERROR) && cdma_irq(&d, n),
          "an unmapped segment is an error");
    CHECK(d.paced_octets == 40u + 40u + 64u + 20u + 44u, "every octet counted once");
    cdma_free(&d);
}

int main(void) {
    printf("NEON CDMA + AES tests\n");
    test_copy_across_segments();
    test_uid_request_as_ios6_made_it();
    test_register_key_and_ecb();
    test_errors_and_abort();
    test_car_after_requests();
    test_retry_when_ready();
    test_paced_channel();
    printf("  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
