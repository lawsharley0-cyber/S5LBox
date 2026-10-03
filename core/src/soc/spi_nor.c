/*
 * NEON — a serial NOR flash on an SPI bus. See core/include/spi_nor.h for
 * what drives it, which part it presents and what it does not model.
 *
 * The rules below are the ones every SPI NOR datasheet states and the M25P
 * family's in particular: a command is the first octet after select asserts;
 * write enable, write disable, a status write, a page program and an erase
 * take effect only when select releases, and only when it releases on the
 * octet boundary the command defines; program, erase and status write need
 * the write enable latch and clear it; programming can only clear bits; a
 * page program wraps within its page and keeps the last 256 octets sent.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "spi_nor.h"

#include <string.h>

bool spi_nor_init(spi_nor_t *nor, uint8_t *mem, uint32_t size, uint32_t jedec_id) {
    if (!nor || !mem || size < SPI_NOR_ERASE_4K || (size & (size - 1u)) != 0u ||
        jedec_id > 0xffffffu)
        return false;
    memset(nor, 0, sizeof *nor);
    nor->mem = mem;
    nor->size = size;
    nor->jedec_id = jedec_id;
    return true;
}

void spi_nor_reset(spi_nor_t *nor) {
    if (!nor) return;
    nor->selected = false;
    nor->status = 0u;
    nor->cmd = 0u;
    nor->pos = 0u;
    nor->addr = 0u;
    nor->ignoring = false;
    nor->page_fill = 0u;
}

/* The commands this model implements; see spi_nor.h for why these. */
static bool known(uint8_t cmd) {
    switch (cmd) {
    case SPI_NOR_CMD_WRSR: case SPI_NOR_CMD_PP:   case SPI_NOR_CMD_READ:
    case SPI_NOR_CMD_WRDI: case SPI_NOR_CMD_RDSR: case SPI_NOR_CMD_WREN:
    case SPI_NOR_CMD_SSE:  case SPI_NOR_CMD_RDID:
        return true;
    default:
        return false;
    }
}

static void commit(spi_nor_t *nor) {
    const bool wel = (nor->status & SPI_NOR_SR_WEL) != 0u;
    switch (nor->cmd) {
    case SPI_NOR_CMD_WREN:
        if (nor->pos == 1u) nor->status |= SPI_NOR_SR_WEL;
        break;
    case SPI_NOR_CMD_WRDI:
        if (nor->pos == 1u) nor->status &= (uint8_t)~SPI_NOR_SR_WEL;
        break;
    case SPI_NOR_CMD_WRSR:
        if (nor->pos != 2u) break;
        if (!wel) { nor->refused++; break; }
        nor->status = (uint8_t)((nor->status & ~SPI_NOR_SR_WRITABLE) |
                                (nor->wrsr_value & SPI_NOR_SR_WRITABLE));
        nor->status &= (uint8_t)~SPI_NOR_SR_WEL;
        nor->status_writes++;
        break;
    case SPI_NOR_CMD_PP: {
        if (nor->pos < 5u) break;           /* no data octet: nothing to do */
        if (!wel) { nor->refused++; break; }
        const uint32_t base = nor->addr & (nor->size - 1u) & ~(SPI_NOR_PAGE - 1u);
        const uint32_t col = nor->addr & (SPI_NOR_PAGE - 1u);
        const uint32_t n = nor->page_fill < SPI_NOR_PAGE ? nor->page_fill : SPI_NOR_PAGE;
        for (uint32_t k = nor->page_fill - n; k < nor->page_fill; k++)
            nor->mem[base + ((col + k) & (SPI_NOR_PAGE - 1u))] &=
                nor->page[k & (SPI_NOR_PAGE - 1u)];
        nor->programs++;
        nor->programmed_octets += n;
        nor->status &= (uint8_t)~SPI_NOR_SR_WEL;
        break;
    }
    case SPI_NOR_CMD_SSE:
        if (nor->pos != 4u) break;
        if (!wel) { nor->refused++; break; }
        memset(nor->mem + (nor->addr & (nor->size - 1u) & ~(SPI_NOR_ERASE_4K - 1u)),
               0xff, SPI_NOR_ERASE_4K);
        nor->erases++;
        nor->status &= (uint8_t)~SPI_NOR_SR_WEL;
        break;
    default:
        break;
    }
}

void spi_nor_select(spi_nor_t *nor, bool active) {
    if (!nor || active == nor->selected) return;
    if (active) {
        nor->selected = true;
        nor->selections++;
        nor->cmd = 0u;
        nor->pos = 0u;
        nor->addr = 0u;
        nor->ignoring = false;
        nor->page_fill = 0u;
        return;
    }
    if (nor->pos && !nor->ignoring) commit(nor);
    nor->selected = false;
}

uint8_t spi_nor_transfer(void *ctx, uint8_t out) {
    spi_nor_t *nor = ctx;
    if (!nor || !nor->selected) return 0xffu;       /* nobody drives the line */
    const uint32_t pos = nor->pos;
    if (nor->pos != UINT32_MAX) nor->pos++;
    if (pos == 0u) {
        nor->cmd = out;
        nor->commands++;
        if (!known(out)) {
            nor->ignoring = true;
            nor->unknown++;
            nor->last_unknown = out;
        } else if (out == SPI_NOR_CMD_READ) {
            nor->reads++;
        }
        return 0xffu;
    }
    if (nor->ignoring) return 0xffu;
    switch (nor->cmd) {
    case SPI_NOR_CMD_RDID:
        /* Three ID octets; the driver reads no more, and past them this
         * answers 0. */
        return pos <= 3u ? (uint8_t)(nor->jedec_id >> (8u * (3u - pos))) : 0x00u;
    case SPI_NOR_CMD_RDSR:
        return nor->status;
    case SPI_NOR_CMD_WRSR:
        if (pos == 1u) nor->wrsr_value = out;
        return 0xffu;
    case SPI_NOR_CMD_READ:
    case SPI_NOR_CMD_PP:
    case SPI_NOR_CMD_SSE:
        if (pos <= 3u) {
            nor->addr = (nor->addr << 8) | out;
            return 0xffu;
        }
        if (nor->cmd == SPI_NOR_CMD_READ) {
            const uint8_t v = nor->mem[nor->addr & (nor->size - 1u)];
            nor->addr++;
            nor->read_octets++;
            return v;
        }
        if (nor->cmd == SPI_NOR_CMD_PP) {
            nor->page[nor->page_fill & (SPI_NOR_PAGE - 1u)] = out;
            if (nor->page_fill != UINT32_MAX) nor->page_fill++;
        }
        return 0xffu;
    default:
        return 0xffu;
    }
}

void spi_nor_bind(spi_nor_t *nor, s5l_spi_slave_t *slave) {
    if (!slave) return;
    memset(slave, 0, sizeof *slave);
    slave->ctx = nor;
    slave->transfer = spi_nor_transfer;
}
