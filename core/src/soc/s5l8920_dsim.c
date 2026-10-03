/*
 * NEON — the S5L8920's MIPI-DSI master (see s5l8920_dsim.h).
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "s5l8920_dsim.h"

#include <stdbool.h>
#include <string.h>

void s5l8920_dsim_reset(s5l8920_dsim_t *d) {
    memset(d, 0, sizeof *d);
}

uint32_t s5l8920_dsim_read(const s5l8920_dsim_t *d, uint32_t off) {
    if (off >= S5L8920_DSIM_SIZE || (off & 3u)) return 0u;
    if (off == S5L8920_DSIM_STATUS) {
        const uint32_t esc = d->reg[S5L8920_DSIM_ESCMODE / 4u];
        const bool ulps = (esc & 0xau) == 0xau && !(esc & 0x5u);
        const bool hs = (d->reg[S5L8920_DSIM_CLKCTRL / 4u] & S5L8920_DSIM_HS_REQ) != 0u;
        const uint32_t base = S5L8920_DSIM_PLL_OK | S5L8920_DSIM_SWRST_RLS;
        if (ulps) return base | S5L8920_DSIM_ULPS_DAT | S5L8920_DSIM_ULPS_CLK;
        return base | S5L8920_DSIM_DAT_STOP |
               (hs ? S5L8920_DSIM_HS_READY : S5L8920_DSIM_CLK_STOP);
    }
    return d->reg[off >> 2];
}

void s5l8920_dsim_write(s5l8920_dsim_t *d, uint32_t off, uint32_t v) {
    if (off >= S5L8920_DSIM_SIZE || (off & 3u)) return;
    if (off == S5L8920_DSIM_STATUS) return;                  /* read-only */
    d->reg[off >> 2] = v;
    if (off == S5L8920_DSIM_PKTHDR) { d->packets++; d->last_header = v; }
    if (off == S5L8920_DSIM_PAYLOAD) d->payload_words++;
}
