/*
 * NEON — the S5L8920's SHA-1 engine, the iPhone 3GS's /arm-io/sha1 (reg
 * 0x00100000 + 0x100000, so 0x80100000; "sha1,s5l8920x", sha1-version 1,
 * "slave-dma-only": data reaches it only through CDMA channel 4, which writes
 * into the FIFO at +0xA0, the tree's dma-channels entry).
 *
 * What the driver does, read from iPhone OS 3.1.3's AppleS5L8920XSHA1
 * (7E18, 0xc0470000), the hasher behind the kernel's SHA1Init/SHA1Update
 * when IOCryptoAcceleratorFamily is present -- code-signing page validation
 * (cs_validate_page) among its users:
 *
 *   start (0xc04715a8): +0x4 = 1, +0x10 = 0. For a request that continues a
 *   hash it loads the running state into +0x20..+0x30, each word byte-
 *   reversed (0xc04711ec: rev, then the store), and starts with +0x0 = 0xA;
 *   a request that begins a hash writes no state and starts with +0x0 = 0x2.
 *   So bit 1 starts and bit 3 says "continue from the loaded state" (the
 *   engine otherwise begins from the standard initial value). Padding is
 *   the driver's own (0xc0471134): the engine only ever sees whole 64-octet
 *   blocks, delivered by DMA.
 *
 *   completion (0xc0471588, the DMA completion callback): reads +0x20..
 *   +0x30, byte-reversing each word, as the digest -- no status register is
 *   polled, so the blocks must be absorbed by the time the DMA completes.
 *
 * The model: H0..H4 (read and written byte-reversed at +0x20..+0x30),
 * a 64-octet block buffer fed through s5l_sha1_feed() (the DMA path) or
 * CPU stores to the FIFO, and the rest as a register file. Each full block
 * is compressed at once. The engine's interrupt (the tree's 0x21) is not
 * raised: the driver does not wait for it.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef NEON_S5L_SHA1_H
#define NEON_S5L_SHA1_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define S5L_SHA1_SIZE        UINT32_C(0x1000)

#define S5L_SHA1_CONTROL     0x00u
#define S5L_SHA1_RESET       0x04u
#define S5L_SHA1_STATE       0x20u      /* H0..H4, five words, byte-reversed */
#define S5L_SHA1_FIFO        0xa0u

#define S5L_SHA1_START       (1u << 1)
#define S5L_SHA1_CONTINUE    (1u << 3)

typedef struct {
    uint32_t reg[S5L_SHA1_SIZE / 4u];
    uint32_t h[5];
    uint8_t  block[64];
    unsigned fill;               /* octets in block[]                  */
    uint64_t blocks;             /* blocks compressed                  */
} s5l_sha1_t;

void     s5l_sha1_reset(s5l_sha1_t *s);
uint32_t s5l_sha1_read(const s5l_sha1_t *s, uint32_t off);
void     s5l_sha1_write(s5l_sha1_t *s, uint32_t off, uint32_t v);
/* Octets for the FIFO, in memory order (what CDMA delivers). */
void     s5l_sha1_feed(s5l_sha1_t *s, const uint8_t *data, size_t len);

#endif /* NEON_S5L_SHA1_H */
