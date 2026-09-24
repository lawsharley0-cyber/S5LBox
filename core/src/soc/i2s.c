/*
 * S5LBox — the two S5L8900 I2S controller windows.
 *
 * Honest storage for the seven offsets AppleS5L8900XI2SController writes, and
 * bounded visibility for anything else. It stores rather than interprets, with
 * one exception: +0x00 bit 0, which configure() always sets, starts the frame
 * clock. The driver never reads a register back and nothing in the
 * kernelcache documents them.
 *
 * The frame clock and the transmit FIFO it drains are described in the I2S
 * block of soc.h, with the startTransfer() disassembly that needs them.
 *
 * See the I2S block in soc.h for the enumeration of the seven write sites and
 * for the four checks that establish `readRegister` is dead code.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "soc.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/*
 * The offsets, in this model's storage order. This is the whole register map:
 * every writeRegister dispatch in the class resolves to one of these seven, so
 * a driver that touched an eighth would be doing something no shipped code path
 * does — which is exactly what the unknown-offset log exists to make visible.
 */
static const uint32_t I2S_OFFSETS[S5L_I2S_REGS] = {
    0x00u, 0x04u, 0x08u, 0x30u, 0x34u, 0x3cu, 0x40u
};

uint32_t s5l_i2s_offset(unsigned index) {
    if (index >= S5L_I2S_REGS) return UINT32_MAX;
    return I2S_OFFSETS[index];
}

/*
 * The FIFO offsets, named. See the `dma-channels` decode in soc.h for where
 * both of them and the direction inference come from.
 *
 * This is deliberately a pure classifier rather than a branch inside
 * s5l_i2s_write(). Recognising an offset must not start excusing it: a CPU
 * store to a FIFO whose physical address the tree hands to the PL080 is the
 * driver doing programmed I/O where DMA was intended, and that stays counted
 * as an unknown-offset access below, where a census will see it. Naming it and
 * forgiving it are different changes and only the first one is made here.
 */
s5l_i2s_fifo_t s5l_i2s_fifo_role(uint32_t off) {
    if (off == S5L_I2S_TX_FIFO_OFF) return S5L_I2S_FIFO_TX;
    if (off == S5L_I2S_RX_FIFO_OFF) return S5L_I2S_FIFO_RX;
    return S5L_I2S_FIFO_NONE;
}

uint32_t s5l_i2s_fifo_pa(unsigned index, s5l_i2s_fifo_t which) {
    uint32_t base;
    if (index == 0u)      base = S5L8900_I2S0_BASE;
    else if (index == 1u) base = S5L8900_I2S1_BASE;
    else                  return UINT32_MAX;
    if (which == S5L_I2S_FIFO_TX) return base + S5L_I2S_TX_FIFO_OFF;
    if (which == S5L_I2S_FIFO_RX) return base + S5L_I2S_RX_FIFO_OFF;
    return UINT32_MAX;
}

static int slot_for(uint32_t off) {
    for (unsigned i = 0; i < S5L_I2S_REGS; i++)
        if (I2S_OFFSETS[i] == off) return (int)i;
    return -1;
}

void s5l_i2s_reset(s5l_i2s_t *i2s) {
    if (!i2s) return;
    memset(i2s, 0, sizeof *i2s);
}

static void note_unknown(s5l_i2s_t *i2s, uint32_t off) {
    for (unsigned i = 0; i < i2s->unknown_off_count; i++)
        if (i2s->unknown_off[i] == off) return;
    if (i2s->unknown_off_count < S5L_I2S_UNKNOWN_OFF)
        i2s->unknown_off[i2s->unknown_off_count++] = off;
}

uint32_t s5l_i2s_read(s5l_i2s_t *i2s, uint32_t off) {
    if (!i2s) return 0u;
    i2s->reads++;
    int slot = slot_for(off);
    if (slot >= 0) return i2s->regs[slot];
    /*
     * A read is already off the established path — the stock driver issues
     * none at all — so every one of them is worth recording, including reads of
     * the FIFO offsets 0x10 and 0x38. Those two are the PL080's, delivered to
     * it as physical addresses out of the device tree, and a CPU read of them
     * would mean something is doing programmed I/O where DMA was intended.
     */
    i2s->unknown_reads++;
    note_unknown(i2s, off);
    return 0u;
}

/*
 * One byte into the TX FIFO. Credit first: a byte pushed while the clock has
 * already taken bytes this refresh that the FIFO did not have is a byte that,
 * one refresh per frame, would have gone in and out between two edges.
 */
static bool tx_accept_byte(s5l_i2s_t *i2s) {
    if (i2s->tx_credit) { i2s->tx_credit--; return true; }
    if (i2s->tx_fill < S5L_I2S_TX_FIFO_BYTES) { i2s->tx_fill++; return true; }
    i2s->tx_overrun++;
    return false;
}

static void tx_push(s5l_i2s_t *i2s, uint32_t val, unsigned bytes) {
    i2s->tx_words++;
    for (unsigned b = 0; b < bytes; b++) {
        /* A full FIFO drops the byte. The DMA request stops before that
         * (s5l_i2s_tx_room), so only a CPU store can get here. */
        if (!tx_accept_byte(i2s)) continue;
        i2s->tx_pack |= ((val >> (8u * b)) & 0xffu) << (8u * i2s->tx_pack_len);
        if (++i2s->tx_pack_len < S5L_I2S_FRAME_BYTES) continue;
        if (i2s->tx_fn) i2s->tx_fn(i2s->tx_ctx, i2s->tx_pack);
        i2s->tx_frames++;
        i2s->tx_pack = 0;
        i2s->tx_pack_len = 0;
    }
}

void s5l_i2s_store(s5l_i2s_t *i2s, uint32_t off, uint32_t val,
                   unsigned bytes) {
    if (!i2s) return;
    i2s->writes++;
    int slot = slot_for(off);
    if (slot >= 0) {
        i2s->regs[slot] = val;
        return;
    }
    if (off == S5L_I2S_TX_FIFO_OFF && (bytes == 1u || bytes == 2u || bytes == 4u))
        tx_push(i2s, val, bytes);
    /* Preserve the existing access census below: format derivation uses
     * writes - unknown_writes to count configuration writes. FIFO data must
     * never be mistaken for programming the audio format. */
    i2s->unknown_writes++;
    note_unknown(i2s, off);
}

void s5l_i2s_write(s5l_i2s_t *i2s, uint32_t off, uint32_t val) {
    s5l_i2s_store(i2s, off, val, 4u);
}

/* ------------------------------------------------------- the frame clock --- */

bool s5l_i2s_clocking(const s5l_i2s_t *i2s) {
    /* Slot 0 is +0x00; configure() at 0xc05a3820 stores cfg | 1 there. */
    return i2s && (i2s->regs[0] & 1u) != 0u;
}

uint32_t s5l_i2s_advance(s5l_i2s_t *i2s, uint32_t tb, uint32_t tb_hz) {
    if (!s5l_i2s_clocking(i2s) || !tb_hz || !tb) return 0u;
    /* At most 2^32 x 44100 + tb_hz, well inside 64 bits. */
    uint64_t total = i2s->fclk_phase + (uint64_t)tb * S5L_I2S_FRAME_HZ;
    /* Most refreshes are one tick and cross no edge: skip the divides. */
    if (total < tb_hz) {
        i2s->fclk_phase = total;
        return 0u;
    }
    uint64_t edges = total / tb_hz;
    i2s->fclk_phase = total % tb_hz;
    i2s->frames += edges;

    /* One frame out of the FIFO per edge; what it does not hold is owed. */
    uint64_t want = edges * S5L_I2S_FRAME_BYTES;
    uint64_t have = want < i2s->tx_fill ? want : i2s->tx_fill;
    i2s->tx_fill -= (uint32_t)have;
    i2s->tx_credit += want - have;
    return edges > UINT32_MAX ? UINT32_MAX : (uint32_t)edges;
}

void s5l_i2s_settle(s5l_i2s_t *i2s) {
    if (!i2s) return;
    i2s->tx_underrun += i2s->tx_credit;
    i2s->tx_credit = 0;
}

bool s5l_i2s_frame_level(const s5l_i2s_t *i2s, uint32_t tb_hz) {
    if (!s5l_i2s_clocking(i2s) || !tb_hz) return false;
    return 2u * i2s->fclk_phase < tb_hz;
}

static uint32_t clamp_ticks(uint64_t t) {
    return t > UINT32_MAX ? UINT32_MAX : (uint32_t)t;
}

uint32_t s5l_i2s_ticks_to_toggle(const s5l_i2s_t *i2s, uint32_t tb_hz) {
    if (!s5l_i2s_clocking(i2s) || !tb_hz) return 0u;
    /* A phase past the frame (only a malformed caller can make one) is
     * normalised by the next advance, which is one tick away. */
    if (i2s->fclk_phase >= tb_hz) return 1u;
    const uint64_t hz = S5L_I2S_FRAME_HZ;
    if (2u * i2s->fclk_phase < tb_hz)     /* high: the fall at half a frame */
        return clamp_ticks((tb_hz - 2u * i2s->fclk_phase + 2u * hz - 1u) /
                           (2u * hz));
    return clamp_ticks((tb_hz - i2s->fclk_phase + hz - 1u) / hz);
}

uint32_t s5l_i2s_ticks_to_frame(const s5l_i2s_t *i2s, uint64_t k,
                                uint32_t tb_hz) {
    if (!s5l_i2s_clocking(i2s) || !tb_hz || !k) return 0u;
    if (i2s->fclk_phase >= tb_hz) return 1u;
    /* The first t with phase + t*hz >= k*tb_hz. */
    if (k > UINT64_MAX / tb_hz) return UINT32_MAX;
    const uint64_t hz = S5L_I2S_FRAME_HZ;
    return clamp_ticks((k * tb_hz - i2s->fclk_phase + hz - 1u) / hz);
}

bool s5l_i2s_tx_room(const s5l_i2s_t *i2s, unsigned bytes) {
    if (!i2s) return false;
    return i2s->tx_credit + (S5L_I2S_TX_FIFO_BYTES - i2s->tx_fill) >= bytes;
}

/* snprintf onto the end of out[0..cap), keeping it terminated; returns the new
 * length, which stays below cap. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 4, 5)))
#endif
static size_t append(char *out, size_t cap, size_t len, const char *fmt, ...) {
    if (len + 1u >= cap) return len;
    va_list ap;
    va_start(ap, fmt);
    const int w = vsnprintf(out + len, cap - len, fmt, ap);
    va_end(ap);
    if (w < 0) return len;
    return (size_t)w >= cap - len ? cap - 1u : len + (size_t)w;
}

size_t s5l_i2s_describe(const s5l_i2s_t *i2s, const char *name,
                        char *out, size_t cap) {
    if (!out || !cap) return 0;
    out[0] = '\0';
    if (!i2s) return 0;
    if (!name) name = "i2s";
    size_t len = append(out, cap, 0, "%s:", name);
    for (unsigned i = 0; i < S5L_I2S_REGS; i++)
        len = append(out, cap, len, " +0x%02x=0x%08x", I2S_OFFSETS[i], i2s->regs[i]);
    len = append(out, cap, len, "\n  %llu reads, %llu writes, %llu/%llu to other offsets",
        (unsigned long long)i2s->reads, (unsigned long long)i2s->writes,
        (unsigned long long)i2s->unknown_reads, (unsigned long long)i2s->unknown_writes);
    for (unsigned i = 0; i < i2s->unknown_off_count && i < S5L_I2S_UNKNOWN_OFF; i++)
        len = append(out, cap, len, "%s+0x%02x", i ? " " : " at ", i2s->unknown_off[i]);
    len = append(out, cap, len, "; TX FIFO stores %llu, frames to host %llu\n",
        (unsigned long long)i2s->tx_words, (unsigned long long)i2s->tx_frames);
    len = append(out, cap, len,
        "  frame clock %s, %llu frames; TX FIFO %u/%u bytes, underrun %llu bytes, overrun %llu bytes\n",
        s5l_i2s_clocking(i2s) ? "running" : "stopped", (unsigned long long)i2s->frames,
        i2s->tx_fill, S5L_I2S_TX_FIFO_BYTES,
        (unsigned long long)i2s->tx_underrun, (unsigned long long)i2s->tx_overrun);
    return len;
}
