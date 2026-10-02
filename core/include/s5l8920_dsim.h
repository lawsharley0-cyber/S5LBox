/*
 * NEON — the S5L8920's MIPI-DSI master (/arm-io/mipi-dsim at 0x89000000,
 * "mipi-dsim,s5l8920x"; a Samsung DSIM), as far as iPhone OS 3.1.3's
 * AppleS5L8900XMIPIDSIController needs it to talk to a panel that is not
 * there.
 *
 * Read from the driver (7E18, kext AppleS5L8720X at 0xc0697000; register
 * read and write are its vtable slots 0x368 and 0x36c):
 *
 *   HS clock (0xc069a638): CLKCTRL (+0x8) bit 31 requests the high-speed
 *   clock (TxRequestHsClk), then the driver spins, with no timeout, until
 *   STATUS (+0x0) bit 10 (TxReadyHsClk) follows it; clearing the request
 *   spins until the bit clears. With no model it spun 730 million times.
 *
 *   Software reset (0xc069aa3a): SWRST (+0x4) = 1, then a spin until STATUS
 *   bit 20 (SwRstRls) is set.
 *
 *   Ultra-low-power state (0xc069a9ce, 0xc069aace): ESCMODE (+0x14) = 0x8A
 *   asks for ULPS on the clock (bit 1) and data lanes (bit 3), and the
 *   driver spins until STATUS bits 7:4 (data lanes in ULPS) and bit 9 (clock
 *   in ULPS) are all set; 0x8F (the exit bits 0 and 2 too) and 0x80 leave
 *   it, and it spins until those bits clear and the stop states (bits 3:0,
 *   bit 8) are back.
 *
 *   Packets (0xc069a698): a long packet's payload words go to +0x38, then
 *   the header to +0x34; short packets (the panel driver's DCS 0x11/0x10
 *   sleep out/in, 0x29/0x28 display on/off) are a header alone.
 *
 * The model: a register file whose STATUS reads PLL stable (bit 31), the
 * software reset released (bit 20), and, as ESCMODE and CLKCTRL ask, either
 * every lane in ULPS or the data lanes stopped (bits 3:0) with the clock
 * lane stopped (bit 8) or ready for HS (bit 10). Packets are accepted and
 * counted; nothing answers a read request (none is made).
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef NEON_S5L8920_DSIM_H
#define NEON_S5L8920_DSIM_H

#include <stdint.h>

#define S5L8920_DSIM_SIZE      UINT32_C(0x1000)
#define S5L8920_DSIM_STATUS    0x00u
#define S5L8920_DSIM_CLKCTRL   0x08u
#define S5L8920_DSIM_SWRST     0x04u
#define S5L8920_DSIM_ESCMODE   0x14u
#define S5L8920_DSIM_PKTHDR    0x34u
#define S5L8920_DSIM_PAYLOAD   0x38u

#define S5L8920_DSIM_HS_REQ    (UINT32_C(1) << 31)   /* CLKCTRL             */
#define S5L8920_DSIM_PLL_OK    (UINT32_C(1) << 31)   /* STATUS              */
#define S5L8920_DSIM_SWRST_RLS (UINT32_C(1) << 20)
#define S5L8920_DSIM_ULPS_CLK  (UINT32_C(1) << 9)
#define S5L8920_DSIM_ULPS_DAT  UINT32_C(0xf0)
#define S5L8920_DSIM_HS_READY  (UINT32_C(1) << 10)
#define S5L8920_DSIM_CLK_STOP  (UINT32_C(1) << 8)
#define S5L8920_DSIM_DAT_STOP  UINT32_C(0xf)

typedef struct {
    uint32_t reg[S5L8920_DSIM_SIZE / 4u];
    uint64_t packets, payload_words;
    uint32_t last_header;
} s5l8920_dsim_t;

void     s5l8920_dsim_reset(s5l8920_dsim_t *d);
uint32_t s5l8920_dsim_read(const s5l8920_dsim_t *d, uint32_t off);
void     s5l8920_dsim_write(s5l8920_dsim_t *d, uint32_t off, uint32_t v);

#endif /* NEON_S5L8920_DSIM_H */
