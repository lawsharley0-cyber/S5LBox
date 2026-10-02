/*
 * NEON — the S5L8920's CDMA engine and its AES contexts: the iPhone 3GS's
 * /arm-io/cdma (reg 0x07000000 + 0x1c000 channels, 0x07800000 + 0x9000 AES,
 * so 0x87000000 and 0x87800000 through /arm-io's ranges).
 *
 * Only the memory-to-memory path is modelled: the one iOS 6's
 * IOAESAccelerator drives, through AppleCDMA (10B500), for every hardware
 * AES operation, AppleKeyStore's UID-key derivations among them. With no
 * engine behind these registers the first such request (a few seconds into
 * launchd) never completes and the system keybag can never be made.
 *
 * Read from AppleCDMA's code:
 *
 *   Channel n (1..27) at n << 12: CSR +0x0, DBR (bytes) +0xc, CAR (command
 *   chain, physical) +0x14, ERR +0x18. Starting a request (0x808a46d8,
 *   0x808a404e): if CSR bits 17:16 (state) read 0, write CSR|2 (abort) and
 *   write the value read back (clearing status); CAR = chain; CSR =
 *   0x88 | ctx << 8 (bit 7 memory-to-memory, bit 3 interrupt enable, bits
 *   15:8 the AES context filtering it); DBR = length; then CSR |= 1 (go).
 *   The interrupt (0x808a3f70) reads CSR and writes it back: bit 18 is an
 *   error, bit 19 done, both write-one-to-clear. Channel n's line is the
 *   tree's interrupts[n - 1], 0x2a + n.
 *
 *   A command chain (0x808a4620) is 32-octet descriptors {next, command,
 *   address, length, ...}: command bits 1:0 = 3 for a data segment, bit 8 on
 *   the last, a zero command ends it, and next is the following
 *   descriptor's physical address. The first segment of an AES-filtered
 *   transfer also carries bits 17:16, which this model does not need.
 *
 *   When a request ends, CAR holds where the engine stopped: past the last
 *   descriptor it consumed (the zero-command one, or the next one after a
 *   last). iPhone OS 3.1.3's AppleCDMA depends on it: its descriptors are a
 *   ring of 128 (one page), and its interrupt handler (0xc0525450)
 *   completes those from its consumer index up to (CAR - ring) / 32. A
 *   request queued while the channel is busy is linked in place of the
 *   terminator and started with CSR = 0x19 again, CAR untouched
 *   (0xc05252a8), so the engine carries on from where it stopped.
 *
 *   A memory-to-memory request is a pair: the odd channel reads (and is the
 *   one an AES context filters), the even one after it writes. Seen in a boot: channel
 *   1 with CSR 0x188 and a 0x30103 descriptor over the input, channel 2
 *   with 0x88 and a 0x103 descriptor over the output.
 *
 *   AES context k (1..8) at 0x800000 + (k << 12) (0x1000 + (k << 12) in the
 *   AES window): control +0x0, IV +0x10..+0x1c, key +0x20..+0x3c. Control
 *   (0x808a4222): bits 15:8 the channel it filters, bit 16 encrypt, bit 17
 *   CBC, bits 19:18 the key size (0 128, 1 192, 2 256), bit 20 the key in
 *   +0x20; otherwise bits 24:21 pick a hardware key (0 UID, 1 GID). The
 *   kernel stores the IV and key buffers' words as they are, so register i
 *   holds octets 4i..4i+3, little-endian. A UID request in a boot:
 *   control 0x30100 (channel 1, encrypt, CBC, 128-bit, UID), IV zero.
 *
 * THE HARDWARE KEYS. A real UID key is fused into each phone and never
 * leaves it, and nothing here knows any phone's. The emulated phone has its
 * own instead: a fixed stand-in (CDMA_STANDIN_KEY), used for UID and GID
 * alike. Everything the guest wraps with it is unwrapped only by this same
 * engine, which is all the guest needs, and it unwraps nothing made on a
 * real phone.
 *
 *   A peripheral request is one channel without bit 7. iPhone OS 3.1.3's
 *   AppleCDMA feeds the SHA-1 engine this way (channel 4, the tree's
 *   dma-channels): CSR = 0x18, +0x4 = 0xCA (transfer configuration), +0x8 =
 *   0x801000A0 (the peripheral's FIFO), the chain, then CSR |= 1; its
 *   descriptors carry command 0x303 over the memory to send.
 *
 *   Peripheral requests are served memory-to-peripheral only: the chain's
 *   octets go to the device whose FIFO is at +0x8 (cdma_set_peripheral), and
 *   the channel completes when the device takes them. A channel whose
 *   address no device claims stays running, counted in periph_unclaimed.
 *
 * Not modelled: time (a request completes when it can run: a memory-to-
 * memory pair when its second channel starts, a peripheral one at once),
 * peripheral-to-memory transfers, pausing, and the IV the hardware leaves
 * behind (the driver writes it for every request).
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef NEON_CDMA_H
#define NEON_CDMA_H

#include <stdbool.h>
#include <stdint.h>

#define CDMA_CHANNELS        28u        /* slot 0 is the global page        */
#define CDMA_SIZE            UINT32_C(0x1c000)
#define CDMA_AES_SIZE        UINT32_C(0x9000)
#define CDMA_AES_CONTEXTS    8u

#define CDMA_CSR             0x00u
#define CDMA_DAR             0x08u      /* a peripheral request's FIFO      */
#define CDMA_DBR             0x0cu
#define CDMA_CAR             0x14u
#define CDMA_ERR             0x18u

#define CDMA_CSR_GO          (1u << 0)
#define CDMA_CSR_ABORT       (1u << 1)
#define CDMA_CSR_IRQ_ENABLE  (1u << 3)
#define CDMA_CSR_M2M         (1u << 7)
#define CDMA_CSR_CTX_SHIFT   8u
#define CDMA_CSR_RUNNING     (1u << 16)
#define CDMA_CSR_STATE       (3u << 16)
#define CDMA_CSR_ERROR       (1u << 18)
#define CDMA_CSR_DONE        (1u << 19)

#define CDMA_CMD_DATA        3u
#define CDMA_CMD_LAST        (1u << 8)

#define CDMA_AES_ENCRYPT     (1u << 16)
#define CDMA_AES_CBC         (1u << 17)
#define CDMA_AES_SIZE_SHIFT  18u
#define CDMA_AES_CUSTOM_KEY  (1u << 20)
#define CDMA_AES_KEYSEL_SHIFT 21u

/* The longest chain and transfer a request may describe; past either, the
 * request ends in error rather than walking memory without bound. */
#define CDMA_MAX_DESCRIPTORS 1024u
#define CDMA_MAX_TRANSFER    (UINT32_C(1) << 24)

/* The emulated phone's hardware key (see above). */
extern const uint8_t CDMA_STANDIN_KEY[32];

/* Physical memory as the engine reads and writes it: false when any of the
 * `len` octets at `pa` is not memory. */
typedef bool (*cdma_mem_fn)(void *ctx, uint32_t pa, uint8_t *buf, uint32_t len, bool write);

/* A peripheral request's data for the device whose FIFO is at `fifo`: true
 * when such a device took it, false when no device is there. */
typedef bool (*cdma_periph_fn)(void *ctx, uint32_t fifo, const uint8_t *data, uint32_t len);

typedef struct {
    uint32_t    ch[CDMA_CHANNELS][8];   /* each channel's registers +0..+0x1c */
    uint32_t    enabled[2];             /* the global page's enable bits      */
    uint32_t    aes[CDMA_AES_SIZE / 4u];
    cdma_mem_fn mem;
    void       *mem_ctx;
    cdma_periph_fn periph;
    void          *periph_ctx;
    uint8_t    *buf;                    /* one transfer, CDMA_MAX_TRANSFER    */

    uint64_t    transfers, octets, aes_ops, hardware_key_ops, errors;
    uint64_t    periph_transfers, periph_octets, periph_unclaimed;
} cdma_t;

/* false if the transfer buffer cannot be allocated. */
bool cdma_init(cdma_t *d, cdma_mem_fn mem, void *mem_ctx);
void cdma_free(cdma_t *d);
/* Every register to 0; memory callbacks and counters kept. */
void cdma_reset(cdma_t *d);
/* Where peripheral requests deliver (NULL: nowhere; they stay running). */
void cdma_set_peripheral(cdma_t *d, cdma_periph_fn fn, void *ctx);

uint32_t cdma_read(cdma_t *d, uint32_t off);
void     cdma_write(cdma_t *d, uint32_t off, uint32_t v);
uint32_t cdma_aes_read(const cdma_t *d, uint32_t off);
void     cdma_aes_write(cdma_t *d, uint32_t off, uint32_t v);

/* Channel n's interrupt level: done or error with its interrupt enabled. */
bool cdma_irq(const cdma_t *d, unsigned n);

#endif /* NEON_CDMA_H */
