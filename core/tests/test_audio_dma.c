/*
 * S5LBox — audio sink delivery and PL080 DMA hardware pacing tests.
 *
 * Tests the hardware audio streaming path from guest DRAM via ARM PL080 DMA
 * to the S5L8900 I2S0 TX FIFO, and verifies that:
 *  1. Bus stores to I2S0 TX FIFO (0x3CA00010) route to the registered audio sink.
 *  2. PL080 DMA transfers targeting I2S0 TX FIFO deliver sample words to the sink.
 *  3. When the host audio sink buffer is not ready (backpressure), DMA stalls
 *     without dropping samples or completing prematurely.
 *  4. When the sink becomes ready again, DMA resumes and completes the transfer.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "soc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(cond, ...) do { \
    if (cond) g_pass++; \
    else { \
        g_fail++; \
        printf("  FAIL %s:%d: ", __func__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

typedef struct {
    uint32_t words[512];
    unsigned count;
    bool     ready;
} test_audio_sink_t;

static void test_sink_tx(void *ctx, uint32_t word) {
    test_audio_sink_t *s = ctx;
    if (s->count < sizeof(s->words) / sizeof(s->words[0])) {
        s->words[s->count++] = word;
    }
}

static bool test_sink_ready(void *ctx) {
    test_audio_sink_t *s = ctx;
    return s->ready;
}

static void test_direct_bus_store_to_audio_sink(void) {
    s5l8900_t m;
    CHECK(s5l8900_init(&m, 0u, 1u << 20), "machine init failed");

    test_audio_sink_t sink;
    memset(&sink, 0, sizeof sink);
    sink.ready = true;

    CHECK(s5l8900_set_audio_sink(&m, test_sink_tx, test_sink_ready, &sink),
          "set_audio_sink returned false");

    const unsigned N = 16u;
    for (unsigned i = 0; i < N; i++) {
        uint32_t val = 0x10000000u | (i << 16) | i;
        m.bus.write32(m.bus.ctx, S5L8900_I2S0_BASE + S5L_I2S_TX_FIFO_OFF, val);
    }

    CHECK(sink.count == N, "audio sink received %u words, expected %u",
          sink.count, N);
    for (unsigned i = 0; i < N; i++) {
        uint32_t want = 0x10000000u | (i << 16) | i;
        CHECK(sink.words[i] == want, "word %u = 0x%08x, expected 0x%08x",
              i, sink.words[i], want);
    }
    CHECK(m.i2s[0].tx_words == N, "tx_words counter = %llu, expected %u",
          (unsigned long long)m.i2s[0].tx_words, N);

    /* Writes to I2S1 (baseband) must not hit the codec sink. */
    m.bus.write32(m.bus.ctx, S5L8900_I2S1_BASE + S5L_I2S_TX_FIFO_OFF, 0xdeadbeefu);
    CHECK(sink.count == N, "i2s1 write leaked into i2s0 sink");

    s5l8900_free(&m);
}

static void test_pl080_dma_to_audio_sink(void) {
    s5l8900_t m;
    CHECK(s5l8900_init(&m, 0u, 1u << 20), "machine init failed");

    test_audio_sink_t sink;
    memset(&sink, 0, sizeof sink);
    sink.ready = true;
    s5l8900_set_audio_sink(&m, test_sink_tx, test_sink_ready, &sink);

    /* Prepare 16 PCM stereo words in guest RAM at address 0x1000. */
    const uint32_t ram_addr = 0x1000u;
    const unsigned N = 16u;
    for (unsigned i = 0; i < N; i++) {
        uint32_t word = ((uint32_t)(i * 2u + 1u) << 16) | (uint32_t)(i * 2u);
        m.bus.write32(m.bus.ctx, ram_addr + i * 4u, word);
    }

    /* Enable DMAC0. */
    m.bus.write32(m.bus.ctx, S5L8900_DMAC0_BASE + 0x030u, 1u /* PL080_CONFIG_EN */);

    /* Program Channel 0:
     * +0x100: SrcAddr
     * +0x104: DestAddr (0x3ca00010)
     * +0x108: LLI (0)
     * +0x10c: Control: size=16, swidth=word(2), dwidth=word(2), si=1, di=0, TC_irq=1
     * +0x110: Config: enable=1
     */
    uint32_t ch0_base = S5L8900_DMAC0_BASE + 0x100u;
    m.bus.write32(m.bus.ctx, ch0_base + 0x00u, ram_addr);
    m.bus.write32(m.bus.ctx, ch0_base + 0x04u, S5L8900_I2S0_BASE + S5L_I2S_TX_FIFO_OFF);
    m.bus.write32(m.bus.ctx, ch0_base + 0x08u, 0u);
    uint32_t ctrl = (N & 0xfffu) | (2u << 18) | (2u << 21) | (1u << 26) | (1u << 31);
    m.bus.write32(m.bus.ctx, ch0_base + 0x0cu, ctrl);
    m.bus.write32(m.bus.ctx, ch0_base + 0x10u, (1u << 15) | 1u /* B_ITC | B_EN */);

    /* Tick the machine: DMAC runs during s5l8900_tick(). */
    s5l8900_tick(&m, 0);

    CHECK(sink.count == N, "DMA delivered %u words to audio sink, expected %u",
          sink.count, N);
    for (unsigned i = 0; i < N; i++) {
        uint32_t want = ((uint32_t)(i * 2u + 1u) << 16) | (uint32_t)(i * 2u);
        CHECK(sink.words[i] == want, "DMA word %u = 0x%08x, expected 0x%08x",
              i, sink.words[i], want);
    }
    CHECK(m.i2s[0].tx_words == N, "i2s0 tx_words = %llu",
          (unsigned long long)m.i2s[0].tx_words);

    /* Verify channel 0 finished and raised terminal count IRQ. */
    uint32_t raw_tc = m.bus.read32(m.bus.ctx, S5L8900_DMAC0_BASE + 0x014u /* OFF_RAWTC */);
    CHECK((raw_tc & 1u) != 0u, "RawIntTCStatus ch0 bit is not set (0x%08x)", raw_tc);
    uint32_t int_tc = m.bus.read32(m.bus.ctx, S5L8900_DMAC0_BASE + 0x004u /* OFF_INTTCSTATUS */);
    CHECK((int_tc & 1u) != 0u, "IntTCStatus ch0 bit is not set (0x%08x)", int_tc);

    s5l8900_free(&m);
}

static void test_pl080_dma_backpressure_flow_control(void) {
    s5l8900_t m;
    CHECK(s5l8900_init(&m, 0u, 1u << 20), "machine init failed");

    test_audio_sink_t sink;
    memset(&sink, 0, sizeof sink);
    sink.ready = false;  /* Simulating a full audio output buffer */
    s5l8900_set_audio_sink(&m, test_sink_tx, test_sink_ready, &sink);

    const uint32_t ram_addr = 0x2000u;
    const unsigned N = 32u;
    for (unsigned i = 0; i < N; i++) {
        m.bus.write32(m.bus.ctx, ram_addr + i * 4u, 0xa0000000u + i);
    }

    m.bus.write32(m.bus.ctx, S5L8900_DMAC0_BASE + 0x030u, 1u);

    uint32_t ch0_base = S5L8900_DMAC0_BASE + 0x100u;
    m.bus.write32(m.bus.ctx, ch0_base + 0x00u, ram_addr);
    m.bus.write32(m.bus.ctx, ch0_base + 0x04u, S5L8900_I2S0_BASE + S5L_I2S_TX_FIFO_OFF);
    m.bus.write32(m.bus.ctx, ch0_base + 0x08u, 0u);
    uint32_t ctrl = (N & 0xfffu) | (2u << 18) | (2u << 21) | (1u << 26) | (1u << 31);
    m.bus.write32(m.bus.ctx, ch0_base + 0x0cu, ctrl);
    m.bus.write32(m.bus.ctx, ch0_base + 0x10u, (1u << 15) | 1u /* B_ITC | B_EN */);

    /* Tick while backpressure is asserted (ready == false). */
    s5l8900_tick(&m, 0);

    CHECK(sink.count == 0u, "DMA moved %u words despite sink not ready", sink.count);
    uint32_t cfg = m.bus.read32(m.bus.ctx, ch0_base + 0x10u);
    CHECK((cfg & 1u) != 0u, "channel 0 was disabled instead of stalled (0x%08x)", cfg);
    uint32_t tc_stalled = m.bus.read32(m.bus.ctx, S5L8900_DMAC0_BASE + 0x014u /* OFF_RAWTC */);
    CHECK((tc_stalled & 1u) == 0u, "terminal count raised prematurely while stalled");

    /* Host audio queue consumes samples, releasing backpressure (ready == true). */
    sink.ready = true;
    s5l8900_tick(&m, 0);

    CHECK(sink.count == N, "DMA did not resume after ready: got %u, expected %u",
          sink.count, N);
    for (unsigned i = 0; i < N; i++) {
        CHECK(sink.words[i] == 0xa0000000u + i,
              "word %u mismatch: 0x%08x", i, sink.words[i]);
    }
    uint32_t tc_done = m.bus.read32(m.bus.ctx, S5L8900_DMAC0_BASE + 0x014u /* OFF_RAWTC */);
    CHECK((tc_done & 1u) != 0u, "terminal count not set after resume");

    s5l8900_free(&m);
}

int main(void) {
    printf("S5LBox audio DMA and I2S sink tests\n");
    test_direct_bus_store_to_audio_sink();
    test_pl080_dma_to_audio_sink();
    test_pl080_dma_backpressure_flow_control();
    printf("  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
