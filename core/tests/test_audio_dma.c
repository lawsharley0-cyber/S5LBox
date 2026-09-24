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
#include "snapshot.h"
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

    /* Host audio queue consumes samples, releasing backpressure (ready == true).
     * The I2S FIFO then takes what it can hold, and no more: its frame clock
     * has not been started, so nothing drains it. */
    sink.ready = true;
    s5l8900_tick(&m, 0);

    const unsigned held = S5L_I2S_TX_FIFO_BYTES / 4u;
    CHECK(sink.count == held, "DMA did not fill the FIFO after ready: got %u, "
          "expected %u", sink.count, held);
    CHECK((m.bus.read32(m.bus.ctx, S5L8900_DMAC0_BASE + 0x014u) & 1u) == 0u,
          "terminal count raised with half the item still in memory");
    CHECK((m.bus.read32(m.bus.ctx, ch0_base + 0x10u) & 1u) != 0u,
          "channel 0 was disabled while the FIFO was full");

    /* configure() starts the frame clock; each frame makes room for one more
     * word. 16 frames is 16 x 6e6 / 44100 = 2176.9 timebase ticks. */
    m.cpu_hz = m.tb_hz;                  /* one CPU tick per timebase tick */
    m.bus.write32(m.bus.ctx, S5L8900_I2S0_BASE, 1u);
    s5l8900_tick(&m, 2177u);

    CHECK(sink.count == N, "DMA did not resume with the clock: got %u, expected %u",
          sink.count, N);
    for (unsigned i = 0; i < N; i++) {
        CHECK(sink.words[i] == 0xa0000000u + i,
              "word %u mismatch: 0x%08x", i, sink.words[i]);
    }
    uint32_t tc_done = m.bus.read32(m.bus.ctx, S5L8900_DMAC0_BASE + 0x014u /* OFF_RAWTC */);
    CHECK((tc_done & 1u) != 0u, "terminal count not set after resume");

    s5l8900_free(&m);
}

/* ------------------------------------------------------ the frame clock --- */

#define I2S0_FIFO (S5L8900_I2S0_BASE + S5L_I2S_TX_FIFO_OFF)

/* Rising edges the clock has crossed after `t` ticks from phase 0. */
static uint64_t edges_after(uint64_t t, uint32_t tb_hz) {
    return t * S5L_I2S_FRAME_HZ / tb_hz;
}

static void test_frame_clock_counts_exact_frames_in_any_step(void) {
    const uint32_t tb_hz = S5L8900_TB_HZ;
    s5l_i2s_t i2s;
    s5l_i2s_reset(&i2s);

    /* Stopped until configure() sets +0x00 bit 0. */
    CHECK(!s5l_i2s_clocking(&i2s), "a reset controller is clocking");
    CHECK(s5l_i2s_advance(&i2s, 1000000u, tb_hz) == 0u && i2s.frames == 0u,
          "a stopped clock counted frames");
    CHECK(s5l_i2s_ticks_to_toggle(&i2s, tb_hz) == 0u &&
          s5l_i2s_ticks_to_frame(&i2s, 1u, tb_hz) == 0u,
          "a stopped clock named an edge");
    s5l_i2s_write(&i2s, 0x00u, 0x10u);
    CHECK(!s5l_i2s_clocking(&i2s), "a value without bit 0 started the clock");
    s5l_i2s_write(&i2s, 0x00u, 0x11u);
    CHECK(s5l_i2s_clocking(&i2s), "configure()'s bit 0 did not start the clock");

    /* One second of timebase is exactly S5L_I2S_FRAME_HZ frames, whatever
     * the step: the phase carries the remainder. */
    static const uint32_t steps[] = { 1u, 7u, 136u, 137u, 1000u, 6000000u };
    for (unsigned s = 0; s < sizeof steps / sizeof steps[0]; s++) {
        s5l_i2s_reset(&i2s);
        s5l_i2s_write(&i2s, 0x00u, 1u);
        uint64_t sum = 0;
        for (uint32_t t = 0; t < tb_hz; t += steps[s])
            sum += s5l_i2s_advance(&i2s, tb_hz - t < steps[s] ? tb_hz - t : steps[s],
                                   tb_hz);
        CHECK(sum == S5L_I2S_FRAME_HZ && i2s.frames == S5L_I2S_FRAME_HZ &&
              i2s.fclk_phase == 0u,
              "step %u: %llu frames in one second, phase %llu", steps[s],
              (unsigned long long)sum, (unsigned long long)i2s.fclk_phase);
    }

    /* The answers the wake table uses: one tick short does not get there,
     * the named tick does. */
    s5l_i2s_reset(&i2s);
    s5l_i2s_write(&i2s, 0x00u, 1u);
    CHECK(s5l_i2s_frame_level(&i2s, tb_hz), "a frame does not start high");
    unsigned toggles = 0, frames_ok = 0;
    for (unsigned n = 0; n < 2000u; n++) {
        const bool level = s5l_i2s_frame_level(&i2s, tb_hz);
        const uint32_t t = s5l_i2s_ticks_to_toggle(&i2s, tb_hz);
        s5l_i2s_t probe = i2s;
        if (t > 1u) (void)s5l_i2s_advance(&probe, t - 1u, tb_hz);
        const bool early = s5l_i2s_frame_level(&probe, tb_hz) == level;
        (void)s5l_i2s_advance(&probe, 1u, tb_hz);
        if (early && s5l_i2s_frame_level(&probe, tb_hz) != level) toggles++;

        const uint64_t k = 1u + n % 5u;
        const uint32_t f = s5l_i2s_ticks_to_frame(&i2s, k, tb_hz);
        s5l_i2s_t p2 = i2s;
        const uint32_t short_by_one = f > 1u ? s5l_i2s_advance(&p2, f - 1u, tb_hz) : 0u;
        const uint32_t reached = short_by_one + s5l_i2s_advance(&p2, 1u, tb_hz);
        if (short_by_one < k && reached == k) frames_ok++;

        (void)s5l_i2s_advance(&i2s, 1u + (n * 37u) % 101u, tb_hz);
    }
    CHECK(toggles == 2000u, "ticks_to_toggle was exact %u of 2000 times", toggles);
    CHECK(frames_ok == 2000u, "ticks_to_frame was exact %u of 2000 times", frames_ok);
    CHECK(edges_after(137u, tb_hz) == 1u && edges_after(136u, tb_hz) == 0u,
          "a frame is not 136.05 ticks of a 6 MHz timebase");
}

typedef struct { uint32_t words[64]; unsigned count; } frame_log_t;
static void log_frame(void *ctx, uint32_t w) {
    frame_log_t *l = ctx;
    if (l->count < 64u) l->words[l->count] = w;
    l->count++;
}

static void test_fifo_packs_frames_drops_overruns_and_pays_credit(void) {
    const uint32_t tb_hz = S5L8900_TB_HZ;
    s5l_i2s_t i2s;
    frame_log_t log;
    memset(&log, 0, sizeof log);
    s5l_i2s_reset(&i2s);
    i2s.tx_fn = log_frame;
    i2s.tx_ctx = &log;

    /* Two 16-bit stores are one frame, the first in the low half. That is
     * how the PL080 feeds it: the `dma-channels` template is 16-bit. */
    s5l_i2s_store(&i2s, S5L_I2S_TX_FIFO_OFF, 0x1111u, 2u);
    CHECK(log.count == 0u && i2s.tx_pack_len == 2u, "half a frame was delivered");
    s5l_i2s_store(&i2s, S5L_I2S_TX_FIFO_OFF, 0xffff2222u, 2u);
    CHECK(log.count == 1u && log.words[0] == 0x22221111u,
          "two halfwords packed to 0x%08x", log.words[0]);
    CHECK(i2s.tx_fill == 4u && i2s.tx_words == 2u && i2s.tx_frames == 1u,
          "fill %u, stores %llu, frames %llu", i2s.tx_fill,
          (unsigned long long)i2s.tx_words, (unsigned long long)i2s.tx_frames);

    /* Fill it; with no clock nothing drains, and the next store is lost. */
    for (unsigned i = 1; i < S5L_I2S_TX_FIFO_BYTES / 4u; i++)
        s5l_i2s_write(&i2s, S5L_I2S_TX_FIFO_OFF, 0x100u + i);
    CHECK(i2s.tx_fill == S5L_I2S_TX_FIFO_BYTES && !s5l_i2s_tx_room(&i2s, 1u),
          "a full FIFO asks for more: fill %u", i2s.tx_fill);
    const unsigned before = log.count;
    s5l_i2s_write(&i2s, S5L_I2S_TX_FIFO_OFF, 0xdeadbeefu);
    CHECK(log.count == before && i2s.tx_overrun == 4u,
          "an overrun reached the host (%u) or was not counted (%llu)",
          log.count - before, (unsigned long long)i2s.tx_overrun);

    /* Ten frames drain the FIFO's 16 by ten; a refresh that crosses 20 more
     * than it holds owes 20 x 4 bytes, and a push in the same refresh pays
     * that before it fills anything. */
    s5l_i2s_write(&i2s, 0x00u, 1u);
    CHECK(s5l_i2s_advance(&i2s, 1361u, tb_hz) == 10u, "1361 ticks is not 10 frames");
    CHECK(i2s.tx_fill == 24u && i2s.tx_credit == 0u, "fill %u credit %llu",
          i2s.tx_fill, (unsigned long long)i2s.tx_credit);
    s5l_i2s_settle(&i2s);
    CHECK(s5l_i2s_advance(&i2s, 3538u, tb_hz) == 26u, "3538 ticks is not 26 frames");
    CHECK(i2s.tx_fill == 0u && i2s.tx_credit == 80u, "fill %u credit %llu",
          i2s.tx_fill, (unsigned long long)i2s.tx_credit);
    for (unsigned i = 0; i < 22u; i++)
        s5l_i2s_write(&i2s, S5L_I2S_TX_FIFO_OFF, 0x200u + i);
    CHECK(i2s.tx_credit == 0u && i2s.tx_fill == 8u,
          "20 stores did not pay the credit before filling: credit %llu fill %u",
          (unsigned long long)i2s.tx_credit, i2s.tx_fill);
    s5l_i2s_settle(&i2s);
    CHECK(i2s.tx_underrun == 0u, "a paid credit was counted as an underrun");

    /* Credit nobody pays is an underrun, and it does not carry over. */
    (void)s5l_i2s_advance(&i2s, 1361u, tb_hz);
    s5l_i2s_settle(&i2s);
    CHECK(i2s.tx_underrun == 32u && i2s.tx_credit == 0u && i2s.tx_fill == 0u,
          "underrun %llu credit %llu fill %u", (unsigned long long)i2s.tx_underrun,
          (unsigned long long)i2s.tx_credit, i2s.tx_fill);
}

/* 64 stereo frames as 128 16-bit samples, left then right, at `ram`. */
static void load_frames(s5l8900_t *m, uint32_t ram) {
    for (unsigned i = 0; i < 64u; i++) {
        m->bus.write16(m->bus.ctx, ram + 4u * i, (uint16_t)(0x1000u + i));
        m->bus.write16(m->bus.ctx, ram + 4u * i + 2u, (uint16_t)(0x2000u + i));
    }
}

/* Channel 0 of dmac0: 128 halfwords from `ram` into the I2S0 FIFO, raising
 * terminal count at the end. The widths are the tree's 0x00249000 template. */
static void start_channel(s5l8900_t *m, uint32_t ram) {
    void *c = m->bus.ctx;
    const uint32_t ch = S5L8900_DMAC0_BASE + 0x100u;
    m->bus.write32(c, S5L8900_DMAC0_BASE + 0x030u, 1u);
    m->bus.write32(c, ch + 0x00u, ram);
    m->bus.write32(c, ch + 0x04u, I2S0_FIFO);
    m->bus.write32(c, ch + 0x08u, 0u);
    m->bus.write32(c, ch + 0x0cu,
                   128u | (1u << 18) | (1u << 21) | (1u << 26) | (1u << 31));
    m->bus.write32(c, ch + 0x10u, (1u << 15) | 1u);
}

static bool channel_done(s5l8900_t *m) {
    return (m->bus.read32(m->bus.ctx, S5L8900_DMAC0_BASE + 0x014u) & 1u) != 0u;
}

static void test_the_frame_clock_paces_dma_one_frame_per_edge(void) {
    s5l8900_t m;
    CHECK(s5l8900_init(&m, 0u, 1u << 20), "machine init failed");
    m.cpu_hz = m.tb_hz;
    frame_log_t log;
    memset(&log, 0, sizeof log);
    s5l8900_set_audio_sink(&m, log_frame, NULL, &log);
    load_frames(&m, 0x3000u);
    m.bus.write32(m.bus.ctx, S5L8900_I2S0_BASE, 1u);      /* configure() */
    s5l8900_tick(&m, 0u);
    start_channel(&m, 0x3000u);
    /* The CPU is somewhere in the kernel while DMA runs. None of the DMA's
     * stores may be credited to that pc. */
    m.cpu.r[15] = 0xc0123456u;
    const uint64_t kernel_before = m.pcm_accesses;
    s5l8900_tick(&m, 0u);

    /* The FIFO's worth goes at once; then one frame per edge, never more. */
    CHECK(log.count == 16u, "the first refresh delivered %u frames", log.count);
    uint64_t t = 0;
    unsigned wrong = 0;
    while (!channel_done(&m) && t < 20000u) {
        s5l8900_tick(&m, 1u);
        t++;
        const uint64_t want = 16u + edges_after(t, m.tb_hz);
        if (log.count != (want < 64u ? want : 64u)) wrong++;
    }
    CHECK(wrong == 0u, "%u ticks delivered a count other than one per edge", wrong);
    CHECK(log.count == 64u && edges_after(t, m.tb_hz) == 48u &&
          edges_after(t - 1u, m.tb_hz) == 47u,
          "terminal count at tick %llu, frame %llu, with %u frames delivered",
          (unsigned long long)t, (unsigned long long)edges_after(t, m.tb_hz),
          log.count);
    unsigned bad = 0;
    for (unsigned i = 0; i < 64u; i++)
        if (log.words[i] != (((0x2000u + i) << 16) | (0x1000u + i))) bad++;
    CHECK(bad == 0u, "%u frames were not left | right << 16", bad);
    CHECK(m.i2s[0].tx_underrun == 0u, "the clock underran a FIFO DMA was feeding");

    CHECK(m.pcm_accesses == kernel_before,
          "%llu DMA stores were counted as kernel code touching I2S",
          (unsigned long long)(m.pcm_accesses - kernel_before));
    char text[2048];
    (void)s5l_access_log_describe(m.pcm_recent, S5L_ACCESS_LOG, S5L_ACCESS_LOG,
                                  text, sizeof text);
    CHECK(strstr(text, "dma W 3ca00010 <-") != NULL &&
          strstr(text, "x128 h (dma to device i2s0)") != NULL &&
          strstr(text, "c0123456") == NULL,
          "the PCM log does not show the DMA as DMA:\n%s", text);
    s5l8900_free(&m);
}

/*
 * A core in WFI skips to the next edge any enabled source names, in ONE
 * refresh. The channel's own wake source must name the edge that ends its
 * item, and the long refresh must deliver what one refresh per tick did.
 */
static void test_a_long_refresh_moves_what_short_ones_do(void) {
    s5l8900_t m;
    CHECK(s5l8900_init(&m, 0u, 1u << 20), "machine init failed");
    m.cpu_hz = m.tb_hz;
    frame_log_t log;
    memset(&log, 0, sizeof log);
    s5l8900_set_audio_sink(&m, log_frame, NULL, &log);
    load_frames(&m, 0x3000u);
    m.bus.write32(m.bus.ctx, S5L8900_I2S0_BASE, 1u);
    s5l8900_tick(&m, 0u);
    start_channel(&m, 0x3000u);
    s5l8900_tick(&m, 0u);

    const s5l_wake_source_t *src = NULL;
    const unsigned n = s5l8900_wake_sources(&src);
    uint32_t at = 0;
    CHECK(s5l8900_next_wake(&m, src, n, &at) == S5L_WAKE_NEVER,
          "a masked DMA line named an edge");
    m.bus.write32(m.bus.ctx, S5L8900_VIC0_BASE + VIC_INTENABLE,
                  1u << S5L8900_IRQ_DMAC0);
    CHECK(s5l8900_next_wake(&m, src, n, &at) == S5L_WAKE_AT &&
          at == s5l_i2s_ticks_to_frame(&m.i2s[0], 48u, m.tb_hz),
          "the channel named tick %u, not the 48th frame's %u", at,
          s5l_i2s_ticks_to_frame(&m.i2s[0], 48u, m.tb_hz));

    s5l8900_tick(&m, at - 1u);
    CHECK(!channel_done(&m) && log.count == 63u,
          "one tick early: done %d, %u frames", channel_done(&m), log.count);
    s5l8900_tick(&m, 1u);
    CHECK(channel_done(&m) && log.count == 64u && m.cpu.irq_line,
          "at the named tick: done %d, %u frames, irq %d", channel_done(&m),
          log.count, m.cpu.irq_line);
    unsigned bad = 0;
    for (unsigned i = 0; i < 64u; i++)
        if (log.words[i] != (((0x2000u + i) << 16) | (0x1000u + i))) bad++;
    CHECK(bad == 0u, "%u frames differ from the per-tick run", bad);

    /* With nothing in flight the controller names nothing again. */
    m.bus.write32(m.bus.ctx, S5L8900_VIC0_BASE + VIC_INTENCLEAR, ~0u);
    m.bus.write32(m.bus.ctx, S5L8900_VIC0_BASE + VIC_INTENABLE,
                  1u << S5L8900_IRQ_DMAC0);
    CHECK(s5l8900_next_wake(&m, src, n, &at) == S5L_WAKE_NEVER,
          "a finished channel still named an edge");
    s5l8900_free(&m);
}

/*
 * startTransfer() at 0xc05a3928 waits for two interrupts on GPIO-IC line
 * 0x86 before it starts DMA. This does what it does: unmask the line the way
 * the GPIO driver's enableVector does (clear the stale bit, then enable),
 * wait for edges, acknowledge each, and mask it again after the second.
 */
static void test_startTransfer_gets_its_two_edges(void) {
    s5l8900_t m;
    CHECK(s5l8900_init(&m, 0u, 1u << 16), "machine init failed");
    m.cpu_hz = m.tb_hz;
    void *c = m.bus.ctx;
    const unsigned group = S5L_GPIOIC_LINE_I2S0 >> 5;
    const uint32_t bit = 1u << (S5L_GPIOIC_LINE_I2S0 & 31u);
    const uint32_t stat = S5L8900_GPIOIC_PAGE + GPIOIC_INTSTAT + 4u * group;
    const uint32_t en = S5L8900_GPIOIC_PAGE + GPIOIC_INTEN + 4u * group;
    CHECK(group == 4u && bit == 0x40u, "line 0x86 is not group 4 bit 6");
    m.bus.write32(c, S5L8900_VIC0_BASE + VIC_INTENABLE,
                  1u << s5l_gpioic_cascade(group));

    const s5l_wake_source_t *src = NULL;
    const unsigned n = s5l8900_wake_sources(&src);
    uint32_t at = 0;

    /* Before configure() nothing drives the line at all. */
    m.bus.write32(c, en, bit);
    s5l8900_tick(&m, 100000u);
    CHECK(!s5l_gpioic_pending(&m.gpioic, S5L_GPIOIC_LINE_I2S0) &&
          !(m.gpioic.driven[group] & bit), "a stopped clock drove line 0x86");
    CHECK(s5l8900_next_wake(&m, src, n, &at) == S5L_WAKE_NEVER,
          "a stopped clock named an edge");
    m.bus.write32(c, en, 0u);

    m.bus.write32(c, S5L8900_I2S0_BASE + 0x04u, 0x01100301u);
    m.bus.write32(c, S5L8900_I2S0_BASE, 1u);
    s5l8900_tick(&m, 5000u);
    CHECK(s5l8900_next_wake(&m, src, n, &at) == S5L_WAKE_NEVER,
          "a masked line named an edge");
    CHECK(!m.cpu.irq_line, "a masked frame clock reached the CPU");

    /* enableVector: clear the stale bit, then unmask. */
    m.bus.write32(c, stat, bit);
    m.bus.write32(c, en, bit);
    s5l8900_tick(&m, 0u);
    CHECK(!m.cpu.irq_line, "the stale edge survived the enable path's clear");

    unsigned interrupts = 0;
    uint64_t waited = 0;
    while (interrupts < 2u && waited < 10000u) {
        CHECK(s5l8900_next_wake(&m, src, n, &at) == S5L_WAKE_AT && at >= 1u,
              "an unmasked running clock named no edge");
        s5l8900_tick(&m, at);             /* what a WFI would do */
        waited += at;
        if (!m.cpu.irq_line) continue;
        CHECK(m.bus.read32(c, stat) & bit, "the IRQ is not line 0x86's");
        interrupts++;
        m.bus.write32(c, stat, bit);      /* handleInterrupt's acknowledge */
        if (interrupts == 2u) m.bus.write32(c, en, 0u);   /* disableInterrupt */
        s5l8900_tick(&m, 0u);
    }
    CHECK(interrupts == 2u, "startTransfer saw %u interrupts", interrupts);
    CHECK(waited <= 2u * 137u, "two edges took %llu ticks, more than two frames",
          (unsigned long long)waited);
    s5l8900_tick(&m, 10000u);
    CHECK(!m.cpu.irq_line, "the line reached the CPU after it was masked");

    /* One refresh across several frames, starting and ending just after a
     * rising edge: the wire is high at both ends, and the edges in between
     * must still latch. */
    s5l8900_tick(&m, s5l_i2s_ticks_to_frame(&m.i2s[0], 1u, m.tb_hz));
    CHECK(s5l_i2s_frame_level(&m.i2s[0], m.tb_hz), "not high after an edge");
    m.bus.write32(c, stat, bit);
    m.bus.write32(c, en, bit);
    s5l8900_tick(&m, 0u);
    CHECK(!m.cpu.irq_line, "the enable path left a stale edge");
    s5l8900_tick(&m, s5l_i2s_ticks_to_frame(&m.i2s[0], 5u, m.tb_hz));
    CHECK(s5l_i2s_frame_level(&m.i2s[0], m.tb_hz) && m.cpu.irq_line,
          "five frames in one refresh raised nothing (level %d)",
          s5l_i2s_frame_level(&m.i2s[0], m.tb_hz));
    m.bus.write32(c, en, 0u);
    m.bus.write32(c, stat, bit);

    /* i2s1 drives its own line, 0xaa, and not 0x86. */
    CHECK(!(m.gpioic.driven[5] & (1u << 10)), "i2s1 drove 0xaa unconfigured");
    m.bus.write32(c, S5L8900_I2S1_BASE, 1u);
    s5l8900_tick(&m, 1000u);
    CHECK(m.gpioic.driven[5] & (1u << 10), "i2s1's clock did not drive 0xaa");
    s5l8900_free(&m);
}

static void test_snapshot_carries_the_clock_and_the_fifo(void) {
    s5l8900_t a, b;
    CHECK(s5l8900_init(&a, 0u, 1u << 16), "init a failed");
    CHECK(s5l8900_init(&b, 0u, 1u << 16), "init b failed");
    a.cpu_hz = a.tb_hz;
    b.cpu_hz = b.tb_hz;
    frame_log_t la, lb;
    memset(&la, 0, sizeof la);
    memset(&lb, 0, sizeof lb);
    s5l8900_set_audio_sink(&a, log_frame, NULL, &la);
    s5l8900_set_audio_sink(&b, log_frame, NULL, &lb);

    a.bus.write32(a.bus.ctx, S5L8900_I2S0_BASE, 1u);
    s5l8900_tick(&a, 1000u);
    for (unsigned i = 0; i < 9u; i++)          /* 4.5 frames: a half is packed */
        a.bus.write16(a.bus.ctx, I2S0_FIFO, (uint16_t)(0x100u + i));
    s5l8900_tick(&a, 50u);

    uint8_t *buf = NULL;
    size_t len = 0;
    CHECK(snapshot_save_mem(&a, &buf, &len) == SNAP_OK, "save failed");
    CHECK(snapshot_load_mem(&b, buf, len) == SNAP_OK, "load failed");
    CHECK(b.i2s[0].fclk_phase == a.i2s[0].fclk_phase &&
          b.i2s[0].frames == a.i2s[0].frames &&
          b.i2s[0].tx_fill == a.i2s[0].tx_fill &&
          b.i2s[0].tx_pack == a.i2s[0].tx_pack &&
          b.i2s[0].tx_pack_len == 2u && b.i2s[0].tx_credit == 0u,
          "restored phase %llu/%llu fill %u/%u pack %u",
          (unsigned long long)b.i2s[0].fclk_phase,
          (unsigned long long)a.i2s[0].fclk_phase, b.i2s[0].tx_fill,
          a.i2s[0].tx_fill, b.i2s[0].tx_pack_len);

    /* Both continue the same way, including the half frame. */
    const unsigned before = la.count;
    for (unsigned i = 0; i < 7u; i++) {
        a.bus.write16(a.bus.ctx, I2S0_FIFO, (uint16_t)(0x200u + i));
        b.bus.write16(b.bus.ctx, I2S0_FIFO, (uint16_t)(0x200u + i));
        s5l8900_tick(&a, 97u);
        s5l8900_tick(&b, 97u);
    }
    CHECK(la.count - before == lb.count && lb.count == 4u,
          "the restored machine delivered %u frames, the original %u",
          lb.count, la.count - before);
    CHECK(lb.words[0] == ((0x200u << 16) | 0x108u),
          "the half frame was lost across the restore: 0x%08x", lb.words[0]);
    CHECK(a.i2s[0].fclk_phase == b.i2s[0].fclk_phase &&
          a.i2s[0].tx_fill == b.i2s[0].tx_fill &&
          a.i2s[0].tx_underrun == b.i2s[0].tx_underrun,
          "the two machines diverged after the restore");

    /* A FIFO fuller than the FIFO, or a partial frame with bits past its
     * length, is refused rather than clamped. i2s_state_valid() runs on the
     * way out as well as the way in, so the save is what refuses. */
    free(buf);
    buf = NULL;
    a.i2s[0].tx_fill = S5L_I2S_TX_FIFO_BYTES + 1u;
    CHECK(snapshot_save_mem(&a, &buf, &len) == SNAP_ERR_CORRUPT,
          "an impossible FIFO fill was saved");
    free(buf);
    buf = NULL;
    a.i2s[0].tx_fill = 0u;
    a.i2s[0].tx_pack_len = 1u;
    a.i2s[0].tx_pack = 0x1234u;
    CHECK(snapshot_save_mem(&a, &buf, &len) == SNAP_ERR_CORRUPT,
          "a partial frame longer than its length was saved");
    free(buf);
    s5l8900_free(&a);
    s5l8900_free(&b);
}

int main(void) {
    printf("S5LBox audio DMA and I2S sink tests\n");
    test_direct_bus_store_to_audio_sink();
    test_pl080_dma_to_audio_sink();
    test_pl080_dma_backpressure_flow_control();
    test_frame_clock_counts_exact_frames_in_any_step();
    test_fifo_packs_frames_drops_overruns_and_pays_credit();
    test_the_frame_clock_paces_dma_one_frame_per_edge();
    test_a_long_refresh_moves_what_short_ones_do();
    test_startTransfer_gets_its_two_edges();
    test_snapshot_carries_the_clock_and_the_fifo();
    printf("  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
