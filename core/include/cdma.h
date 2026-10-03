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
 *   address no device claims, or whose device is not yet asking for data,
 *   stays running, counted in periph_unclaimed, until cdma_retry(). spi1's
 *   driver starts channel 18 before it sets the port's DMA bit (SETUP 0x4018,
 *   then 0x4058), which on hardware is the request line the channel waits on.
 *
 * PACED CHANNELS: AUDIO. iPhone OS 3.1.3 plays sound through channel 21
 * (the audio complex's `dma-channels`, 0x15..0x18): CSR 0x18 then 0x19,
 * +0x4 = 0xA6, +0x8 = the I2S port (0x84500000 for i2s0, the codec's), and
 * a chain built by AppleCDMA's queue routine (0xc05252a8): each queued
 * command is one or more data descriptors in the ring, the last of them
 * marked 0x300 (a one-descriptor command is 0x303, the first of a longer one
 * 0x003), with a zero-command terminator after the newest. Its interrupt
 * handler (0xc0525450) acknowledges CSR bits 19 and 20 together and
 * completes every command before CAR's index, and it never restarts the
 * channel; so the engine must carry on past a marked descriptor on its own,
 * raising bit 20 there, and stop only at the terminator (bit 19, CAR left
 * on it, where the next queued command is linked before CSR = 0x19 starts
 * it again). Two more controls, from the same kext:
 *   - position (0xc05254c0): CSR |= 0x20 (bit 5, pause), wait while the
 *     state reads 1, then read +0x10 -- the address of the next octet in the
 *     descriptor at CAR -- to split that descriptor, and restart with bit 5
 *     clear and bit 0 set; the engine reloads from CAR.
 *   - stop (0xc05255d0): CSR |= 4 (bit 2), wait for bit 21, then CSR = 2
 *     (abort) and CSR |= 0x18.
 * A device that drains its FIFO at its own pace (cdma_set_paced) claims
 * such a channel: starting it delivers nothing, and the device takes the
 * data as it plays with cdma_pull(); cdma_bytes_to_event() says how much it
 * may take before the channel next raises a line. The same CSR semantics
 * (bits 2, 5, 20, 21) apply to every channel; only a paced channel can be
 * caught running when the driver asks.
 *
 * Not modelled: time for anything but a paced channel (a request completes
 * when it can run: a memory-to-memory pair when its second channel starts,
 * a peripheral one at once), peripheral-to-memory transfers, and the IV the
 * hardware leaves behind (the driver writes it for every request).
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
#define CDMA_CUR             0x10u      /* the next octet, in the desc at CAR */
#define CDMA_CAR             0x14u
#define CDMA_ERR             0x18u

#define CDMA_CSR_GO          (1u << 0)
#define CDMA_CSR_ABORT       (1u << 1)
#define CDMA_CSR_STOP        (1u << 2)  /* acknowledged in CDMA_CSR_STOPPED  */
#define CDMA_CSR_IRQ_ENABLE  (1u << 3)
#define CDMA_CSR_PAUSE       (1u << 5)
#define CDMA_CSR_M2M         (1u << 7)
#define CDMA_CSR_CTX_SHIFT   8u
#define CDMA_CSR_RUNNING     (1u << 16)
#define CDMA_CSR_PAUSED      (2u << 16)
#define CDMA_CSR_STATE       (3u << 16)
#define CDMA_CSR_ERROR       (1u << 18)
#define CDMA_CSR_DONE        (1u << 19)
#define CDMA_CSR_SEGMENT     (1u << 20) /* a marked descriptor finished      */
#define CDMA_CSR_STOPPED     (1u << 21)
#define CDMA_CSR_STATUS      (CDMA_CSR_ERROR | CDMA_CSR_DONE | CDMA_CSR_SEGMENT)

#define CDMA_CMD_DATA        3u
#define CDMA_CMD_LAST        (1u << 8)  /* ends a request; a paced channel's
                                         * interrupt point (see above)      */

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

/* True when the device whose FIFO is at `fifo` drains it at its own pace and
 * pulls its requests with cdma_pull(). */
typedef bool (*cdma_paced_fn)(void *ctx, uint32_t fifo);

typedef struct {
    uint32_t    ch[CDMA_CHANNELS][8];   /* each channel's registers +0..+0x1c */
    uint32_t    enabled[2];             /* the global page's enable bits      */
    uint32_t    aes[CDMA_AES_SIZE / 4u];
    cdma_mem_fn mem;
    void       *mem_ctx;
    cdma_periph_fn periph;
    void          *periph_ctx;
    cdma_paced_fn  paced;
    void          *paced_ctx;
    uint8_t    *buf;                    /* one transfer, CDMA_MAX_TRANSFER    */

    uint64_t    transfers, octets, aes_ops, hardware_key_ops, errors;
    uint64_t    periph_transfers, periph_octets, periph_unclaimed;
    uint64_t    paced_octets;
} cdma_t;

/* false if the transfer buffer cannot be allocated. */
bool cdma_init(cdma_t *d, cdma_mem_fn mem, void *mem_ctx);
void cdma_free(cdma_t *d);
/* Every register to 0; memory callbacks and counters kept. */
void cdma_reset(cdma_t *d);
/* Where peripheral requests deliver (NULL: nowhere; they stay running). */
void cdma_set_peripheral(cdma_t *d, cdma_periph_fn fn, void *ctx);
/* Offer every running peripheral request to its device again: for a device
 * that was not ready to take it when the channel started (a FIFO whose DMA
 * request is not yet enabled), call when that changes. */
void cdma_retry(cdma_t *d);
/* Which FIFOs are paced (NULL: none). */
void cdma_set_paced(cdma_t *d, cdma_paced_fn fn, void *ctx);

/* True while channel n is a running paced request. */
bool cdma_paced_running(const cdma_t *d, unsigned n);
/* Up to `len` octets of paced channel n's data into `out`, as its device
 * plays them: the channel moves through its chain, raising CDMA_CSR_SEGMENT
 * past each marked descriptor and stopping, done, at the terminator (or in
 * error at a descriptor that is not memory). Returns how many it delivered,
 * fewer than `len` once it stops. */
uint32_t cdma_pull(cdma_t *d, unsigned n, uint8_t *out, uint32_t len);
/* How many octets paced channel n delivers before it next raises a line (a
 * marked descriptor finishing, or the last one before the terminator), at
 * most `cap`; 0 when it is not running. */
uint32_t cdma_bytes_to_event(const cdma_t *d, unsigned n, uint32_t cap);

uint32_t cdma_read(cdma_t *d, uint32_t off);
void     cdma_write(cdma_t *d, uint32_t off, uint32_t v);
uint32_t cdma_aes_read(const cdma_t *d, uint32_t off);
void     cdma_aes_write(cdma_t *d, uint32_t off, uint32_t v);

/* Channel n's interrupt level: done, error or a marked descriptor finished,
 * with its interrupt enabled. */
bool cdma_irq(const cdma_t *d, unsigned n);

#endif /* NEON_CDMA_H */
