/*
 * NEON — the S5L8920's CDMA engine and its AES contexts (see cdma.h).
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "cdma.h"
#include "aes.h"

#include <stdlib.h>
#include <string.h>

/* 32 octets of ASCII: a key no phone has, so nothing made here unwraps on
 * one and nothing from one unwraps here. */
const uint8_t CDMA_STANDIN_KEY[32] = {
    'N','E','O','N',' ','e','m','u','l','a','t','e','d',' ','i','P',
    'h','o','n','e',' ','3','G','S',' ','U','I','D',' ','k','e','y',
};

bool cdma_init(cdma_t *d, cdma_mem_fn mem, void *mem_ctx) {
    if (!d || !mem) return false;
    memset(d, 0, sizeof *d);
    d->buf = malloc(CDMA_MAX_TRANSFER);
    if (!d->buf) return false;
    d->mem = mem;
    d->mem_ctx = mem_ctx;
    return true;
}

void cdma_free(cdma_t *d) {
    if (!d) return;
    free(d->buf);
    d->buf = NULL;
}

void cdma_set_peripheral(cdma_t *d, cdma_periph_fn fn, void *ctx) {
    d->periph = fn;
    d->periph_ctx = ctx;
}

void cdma_set_paced(cdma_t *d, cdma_paced_fn fn, void *ctx) {
    d->paced = fn;
    d->paced_ctx = ctx;
}

void cdma_reset(cdma_t *d) {
    memset(d->ch, 0, sizeof d->ch);
    memset(d->enabled, 0, sizeof d->enabled);
    memset(d->aes, 0, sizeof d->aes);
}

static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Walks channel n's chain. With `out` NULL, gathers its segments into
 * d->buf and returns the total; otherwise scatters `len` octets of `out`
 * over them. -1 if the chain is malformed or names memory that is not.
 * *end is where the engine stops: the terminating descriptor (a zero
 * command), or the next one after a descriptor that ends the request. */
static int64_t walk_chain(cdma_t *d, unsigned n, const uint8_t *out, uint32_t len,
                          uint32_t *end) {
    uint32_t desc = d->ch[n][CDMA_CAR / 4u];
    uint32_t done = 0;
    for (unsigned i = 0; i < CDMA_MAX_DESCRIPTORS; i++) {
        uint8_t raw[16];
        if (!d->mem(d->mem_ctx, desc, raw, sizeof raw, false)) return -1;
        const uint32_t next = rd32le(raw), cmd = rd32le(raw + 4);
        const uint32_t addr = rd32le(raw + 8), seg = rd32le(raw + 12);
        if (cmd == 0u) { *end = desc; return done; }
        if ((cmd & 3u) == CDMA_CMD_DATA && seg) {
            if (out) {
                const uint32_t n_out = seg < len - done ? seg : len - done;
                if (n_out && !d->mem(d->mem_ctx, addr, (uint8_t *)out + done, n_out, true))
                    return -1;
                done += n_out;
            } else {
                if (seg > CDMA_MAX_TRANSFER - done) return -1;
                if (!d->mem(d->mem_ctx, addr, d->buf + done, seg, false)) return -1;
                done += seg;
            }
        }
        if ((cmd & CDMA_CMD_LAST) || (out && done == len)) { *end = next; return done; }
        desc = next;
    }
    return -1;
}

/* AES over the whole blocks of buf[0..len) in place, as context k says.
 * An unaligned tail passes through. */
static bool run_aes(cdma_t *d, unsigned k, uint32_t len) {
    const uint32_t *r = &d->aes[(0x1000u * k) / 4u];
    const uint32_t control = r[0];
    static const unsigned bits[4] = { 128u, 192u, 256u, 256u };
    const unsigned key_bits = bits[(control >> CDMA_AES_SIZE_SHIFT) & 3u];
    const unsigned keysel = (control >> CDMA_AES_KEYSEL_SHIFT) & 0xfu;
    uint8_t key[32], iv[16];
    if ((control & CDMA_AES_CUSTOM_KEY) || keysel == 0xfu) {
        for (unsigned i = 0; i < 8u; i++)
            for (unsigned b = 0; b < 4u; b++) key[4u * i + b] = (uint8_t)(r[8u + i] >> (8u * b));
    } else {
        memcpy(key, CDMA_STANDIN_KEY, sizeof key);
        d->hardware_key_ops++;
    }
    for (unsigned i = 0; i < 4u; i++)
        for (unsigned b = 0; b < 4u; b++) iv[4u * i + b] = (uint8_t)(r[4u + i] >> (8u * b));

    aes_ctx_t ctx;
    if (!aes_init(&ctx, key, key_bits)) return false;
    const bool enc = (control & CDMA_AES_ENCRYPT) != 0u, cbc = (control & CDMA_AES_CBC) != 0u;
    uint8_t chain[16], blk[16];
    memcpy(chain, iv, sizeof chain);
    for (uint32_t o = 0; o + 16u <= len; o += 16u) {
        uint8_t *p = d->buf + o;
        if (enc) {
            if (cbc) for (unsigned i = 0; i < 16u; i++) p[i] ^= chain[i];
            aes_encrypt_block(&ctx, p, blk);
            memcpy(p, blk, 16);
            memcpy(chain, blk, 16);
        } else {
            memcpy(blk, p, 16);
            aes_decrypt_block(&ctx, blk, p);
            if (cbc) for (unsigned i = 0; i < 16u; i++) p[i] ^= chain[i];
            memcpy(chain, blk, 16);
        }
    }
    d->aes_ops++;
    return true;
}

static void finish(cdma_t *d, unsigned n, bool error) {
    uint32_t *c = d->ch[n];
    c[CDMA_CSR / 4u] = (c[CDMA_CSR / 4u] & ~CDMA_CSR_STATE) |
                       (error ? CDMA_CSR_ERROR : CDMA_CSR_DONE);
    c[CDMA_DBR / 4u] = 0u;
    if (error) c[CDMA_ERR / 4u] = 1u;
}

/* A memory-to-memory request runs once both channels of its pair (odd
 * source, the even one after it the sink) are going. */
static void try_transfer(cdma_t *d, unsigned n) {
    const unsigned src = (n & 1u) ? n : n - 1u, dst = src + 1u;
    if (src == 0u || dst >= CDMA_CHANNELS) return;
    const uint32_t s = d->ch[src][0], t = d->ch[dst][0];
    if (!(s & CDMA_CSR_RUNNING) || !(t & CDMA_CSR_RUNNING) ||
        !(s & CDMA_CSR_M2M) || !(t & CDMA_CSR_M2M))
        return;
    bool ok = true;
    uint32_t src_end = 0, dst_end = 0;
    const int64_t got = walk_chain(d, src, NULL, 0, &src_end);
    if (got < 0) ok = false;
    uint32_t len = got < 0 ? 0u : (uint32_t)got;
    unsigned k = (s >> CDMA_CSR_CTX_SHIFT) & 0xffu;
    if (!k) k = (t >> CDMA_CSR_CTX_SHIFT) & 0xffu;
    if (ok && k) ok = k <= CDMA_AES_CONTEXTS && run_aes(d, k, len);
    if (ok) ok = walk_chain(d, dst, d->buf, len, &dst_end) >= 0;
    d->transfers++;
    if (ok) {
        d->octets += len;
        d->ch[src][CDMA_CAR / 4u] = src_end;
        d->ch[dst][CDMA_CAR / 4u] = dst_end;
    } else {
        d->errors++;
    }
    finish(d, src, !ok);
    finish(d, dst, !ok);
}

/* ------------------------------------------------------ paced channels */

static bool is_paced(const cdma_t *d, unsigned n) {
    return d->paced && !(d->ch[n][0] & CDMA_CSR_M2M) &&
           d->paced(d->paced_ctx, d->ch[n][CDMA_DAR / 4u]);
}

typedef struct { uint32_t next, cmd, addr, len; } cdma_desc_t;

static bool read_desc(const cdma_t *d, uint32_t at, cdma_desc_t *out) {
    uint8_t raw[16];
    if (!d->mem(d->mem_ctx, at, raw, sizeof raw, false)) return false;
    out->next = rd32le(raw);
    out->cmd = rd32le(raw + 4);
    out->addr = rd32le(raw + 8);
    out->len = (out->cmd & 3u) == CDMA_CMD_DATA ? rd32le(raw + 12) : 0u;
    return true;
}

/* Paced channel n arrives at the descriptor at CAR, from the start of it
 * when `reload` (a start, or a restart after a pause) and otherwise where
 * CUR says. A terminator stops it, done; empty descriptors are passed over.
 * False when it is no longer running. */
static bool paced_settle(cdma_t *d, unsigned n, bool reload) {
    uint32_t *c = d->ch[n];
    for (unsigned i = 0; i < CDMA_MAX_DESCRIPTORS; i++) {
        cdma_desc_t ds;
        if (!read_desc(d, c[CDMA_CAR / 4u], &ds)) {
            d->errors++;
            finish(d, n, true);
            return false;
        }
        if (ds.cmd == 0u) {
            finish(d, n, false);
            return false;
        }
        const uint32_t cur = c[CDMA_CUR / 4u];
        if (reload || cur < ds.addr || cur - ds.addr > ds.len) c[CDMA_CUR / 4u] = ds.addr;
        if (c[CDMA_CUR / 4u] - ds.addr < ds.len) return true;
        /* Nothing left in this one: on to the next. */
        if (ds.cmd & CDMA_CMD_LAST) c[0] |= CDMA_CSR_SEGMENT;
        c[CDMA_CAR / 4u] = ds.next;
        reload = true;
    }
    d->errors++;
    finish(d, n, true);
    return false;
}

bool cdma_paced_running(const cdma_t *d, unsigned n) {
    return n > 0u && n < CDMA_CHANNELS &&
           (d->ch[n][0] & CDMA_CSR_STATE) == CDMA_CSR_RUNNING && is_paced(d, n);
}

uint32_t cdma_pull(cdma_t *d, unsigned n, uint8_t *out, uint32_t len) {
    uint32_t got = 0;
    if (!cdma_paced_running(d, n)) return 0u;
    uint32_t *c = d->ch[n];
    while (got < len) {
        if (!paced_settle(d, n, false)) break;
        cdma_desc_t ds;
        if (!read_desc(d, c[CDMA_CAR / 4u], &ds)) break;   /* settle read it */
        const uint32_t cur = c[CDMA_CUR / 4u];
        uint32_t take = ds.len - (cur - ds.addr);
        if (take > len - got) take = len - got;
        if (!d->mem(d->mem_ctx, cur, out + got, take, false)) {
            d->errors++;
            finish(d, n, true);
            break;
        }
        got += take;
        c[CDMA_CUR / 4u] = cur + take;
        /* Finishing a descriptor moves on at once, so a request that has
         * played its last octet is done the moment it has. */
        if (cur + take - ds.addr == ds.len) (void)paced_settle(d, n, false);
    }
    d->paced_octets += got;
    return got;
}

uint32_t cdma_bytes_to_event(const cdma_t *d, unsigned n, uint32_t cap) {
    if (!cdma_paced_running(d, n)) return 0u;
    const uint32_t *c = d->ch[n];
    uint32_t at = c[CDMA_CAR / 4u], cur = c[CDMA_CUR / 4u], total = 0;
    for (unsigned i = 0; i < CDMA_MAX_DESCRIPTORS && total < cap; i++) {
        cdma_desc_t ds, after;
        if (!read_desc(d, at, &ds) || ds.cmd == 0u) break;
        const uint32_t done = i == 0u && cur >= ds.addr && cur - ds.addr <= ds.len
                            ? cur - ds.addr : 0u;
        total += ds.len - done;
        if (ds.cmd & CDMA_CMD_LAST) break;
        if (!read_desc(d, ds.next, &after) || after.cmd == 0u) break;
        at = ds.next;
    }
    return total < cap ? total : cap;
}

/* A peripheral request on channel n: its chain's octets to the device at
 * its FIFO address. Unclaimed, it stays running. */
static void try_peripheral(cdma_t *d, unsigned n) {
    if (!d->periph) { d->periph_unclaimed++; return; }
    uint32_t end = 0;
    const int64_t got = walk_chain(d, n, NULL, 0, &end);
    if (got < 0) {
        d->transfers++;
        d->errors++;
        finish(d, n, true);
        return;
    }
    if (!d->periph(d->periph_ctx, d->ch[n][CDMA_DAR / 4u], d->buf, (uint32_t)got)) {
        d->periph_unclaimed++;
        return;
    }
    d->transfers++;
    d->periph_transfers++;
    d->periph_octets += (uint64_t)got;
    d->ch[n][CDMA_CAR / 4u] = end;
    finish(d, n, false);
}

void cdma_retry(cdma_t *d) {
    for (unsigned n = 1; n < CDMA_CHANNELS; n++) {
        const uint32_t csr = d->ch[n][CDMA_CSR / 4u];
        if ((csr & CDMA_CSR_STATE) == CDMA_CSR_RUNNING && !(csr & CDMA_CSR_M2M) &&
            !is_paced(d, n))
            try_peripheral(d, n);
    }
}

uint32_t cdma_read(cdma_t *d, uint32_t off) {
    if (off >= CDMA_SIZE || (off & 3u)) return 0u;
    const unsigned n = off >> 12, r = (off & 0xfffu) >> 2;
    if (n == 0u) {
        if (off == 0x10u) return d->enabled[0];
        if (off == 0x14u) return d->enabled[1];
        return 0u;
    }
    return r < 8u ? d->ch[n][r] : 0u;
}

void cdma_write(cdma_t *d, uint32_t off, uint32_t v) {
    if (off >= CDMA_SIZE || (off & 3u)) return;
    const unsigned n = off >> 12, r = (off & 0xfffu) >> 2;
    if (n == 0u) {
        switch (off) {
        case 0x0: d->enabled[0] |= v;  break;
        case 0x4: d->enabled[1] |= v;  break;
        case 0x8: d->enabled[0] &= ~v; break;
        case 0xc: d->enabled[1] &= ~v; break;
        default: break;
        }
        return;
    }
    if (r >= 8u) return;
    if (r != 0u) { d->ch[n][r] = v; return; }
    uint32_t *csr = &d->ch[n][0];
    const uint32_t old = *csr;
    /* Status is write-one-to-clear; state and the stop acknowledge are the
     * engine's; the rest of the low half (interrupt enable, pause, memory-
     * to-memory, the AES context) is the driver's. Bits 0-2 are commands:
     * go, abort, and stop, which halts the channel and says so in bit 21. */
    uint32_t next = (old & ~CDMA_CSR_STATUS) | (old & CDMA_CSR_STATUS & ~v);
    next = (next & ~UINT32_C(0xfff8)) | (v & UINT32_C(0xfff8));
    if (v & CDMA_CSR_ABORT) next &= ~(CDMA_CSR_STATE | CDMA_CSR_STOPPED);
    if (v & CDMA_CSR_STOP) next = (next & ~CDMA_CSR_STATE) | CDMA_CSR_STOPPED;
    if (v & CDMA_CSR_GO)
        next = (next & ~(CDMA_CSR_STATE | CDMA_CSR_STOPPED)) | CDMA_CSR_RUNNING;
    if ((next & CDMA_CSR_PAUSE) && (next & CDMA_CSR_STATE) == CDMA_CSR_RUNNING)
        next = (next & ~CDMA_CSR_STATE) | CDMA_CSR_PAUSED;
    *csr = next;
    if ((v & CDMA_CSR_GO) && (next & CDMA_CSR_STATE) == CDMA_CSR_RUNNING) {
        if (next & CDMA_CSR_M2M) try_transfer(d, n);
        else if (is_paced(d, n)) {
            /* A kick while it runs (a command linked behind it) changes
             * nothing; from idle or a pause it starts again at CAR. */
            if ((old & CDMA_CSR_STATE) != CDMA_CSR_RUNNING) (void)paced_settle(d, n, true);
        } else {
            try_peripheral(d, n);
        }
    }
}

uint32_t cdma_aes_read(const cdma_t *d, uint32_t off) {
    return off < CDMA_AES_SIZE && !(off & 3u) ? d->aes[off >> 2] : 0u;
}

void cdma_aes_write(cdma_t *d, uint32_t off, uint32_t v) {
    if (off < CDMA_AES_SIZE && !(off & 3u)) d->aes[off >> 2] = v;
}

bool cdma_irq(const cdma_t *d, unsigned n) {
    if (n == 0u || n >= CDMA_CHANNELS) return false;
    const uint32_t csr = d->ch[n][0];
    return (csr & CDMA_CSR_IRQ_ENABLE) && (csr & CDMA_CSR_STATUS);
}
