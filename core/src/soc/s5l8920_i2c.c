/*
 * NEON — the S5L8920's I2C controllers and the register-file slave (see
 * s5l8920_i2c.h).
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "s5l8920_i2c.h"

#include <string.h>

void s5l8920_i2c_reset(s5l8920_i2c_t *c) {
    memset(c->reg, 0, sizeof c->reg);
    c->status = 0;
    c->fifo_len = c->fifo_pos = 0;
    c->rx = false;
}

bool s5l8920_i2c_attach(s5l8920_i2c_t *c, const s5l_i2c_slave_t *slave) {
    if (!slave || !slave->start || slave->addr > 0x7fu || c->slave_count >= S5L8920_I2C_SLAVES)
        return false;
    for (unsigned i = 0; i < c->slave_count; i++)
        if (c->slaves[i].addr == slave->addr) return false;
    c->slaves[c->slave_count++] = *slave;
    return true;
}

static s5l_i2c_slave_t *find(s5l8920_i2c_t *c, uint32_t addr) {
    for (unsigned i = 0; i < c->slave_count; i++)
        if (c->slaves[i].addr == (addr & 0x7fu)) return &c->slaves[i];
    return NULL;
}

/* One whole transfer, as +0x24 starts it. */
static void run(s5l8920_i2c_t *c, bool write) {
    const uint32_t count = c->reg[S5L8920_I2C_COUNT / 4u];
    s5l_i2c_slave_t *s = find(c, c->reg[S5L8920_I2C_ADDR / 4u]);
    bool ok = s && count <= S5L8920_I2C_FIFO_LEN && s->start(s->ctx, false);
    c->transfers++;
    if (ok) ok = !s->write || s->write(s->ctx, (uint8_t)c->reg[S5L8920_I2C_FIRST / 4u]);
    if (ok) c->bytes_tx++;
    if (ok && write) {
        for (unsigned i = 0; ok && i < count && i < c->fifo_len; i++) {
            ok = !s->write || s->write(s->ctx, c->fifo[i]);
            if (ok) c->bytes_tx++;
        }
    } else if (ok) {
        if (s->stop) s->stop(s->ctx);              /* the repeated start */
        ok = s->start(s->ctx, true);
        for (unsigned i = 0; ok && i < count; i++) {
            c->fifo[i] = s->read ? s->read(s->ctx) : 0xffu;
            c->bytes_rx++;
        }
    }
    if (s && s->stop) s->stop(s->ctx);
    c->fifo_pos = 0;
    c->rx = !write && ok;
    c->fifo_len = c->rx ? count : 0u;
    if (!ok) c->naks++;
    c->status |= ok ? S5L8920_I2C_ST_DONE : S5L8920_I2C_ST_ERROR;
}

uint32_t s5l8920_i2c_read(s5l8920_i2c_t *c, uint32_t off) {
    if (off >= S5L8920_I2C_SIZE || (off & 3u)) return 0u;
    switch (off) {
    case S5L8920_I2C_STATUS: return c->status;
    case S5L8920_I2C_FIFO:
        return c->fifo_pos < c->fifo_len ? c->fifo[c->fifo_pos++] : 0u;
    default: return c->reg[off >> 2];
    }
}

void s5l8920_i2c_write(s5l8920_i2c_t *c, uint32_t off, uint32_t v) {
    if (off >= S5L8920_I2C_SIZE || (off & 3u)) return;
    switch (off) {
    case S5L8920_I2C_STATUS:
        c->status &= ~v;
        return;
    case S5L8920_I2C_FIFO:
        if (c->rx) { c->rx = false; c->fifo_len = c->fifo_pos = 0; }
        if (c->fifo_len < S5L8920_I2C_FIFO_LEN) c->fifo[c->fifo_len++] = (uint8_t)v;
        return;
    case S5L8920_I2C_CONTROL:
        c->reg[off >> 2] = v;
        if (v & S5L8920_I2C_CTRL_START) run(c, (v & S5L8920_I2C_CTRL_WRITE) != 0u);
        return;
    default:
        c->reg[off >> 2] = v;
        return;
    }
}

bool s5l8920_i2c_irq(const s5l8920_i2c_t *c) {
    return (c->status & c->reg[S5L8920_I2C_IEN / 4u]) != 0u;
}

/* ------------------------------------------------------ register file */

void i2c_regfile_init(i2c_regfile_t *r, uint8_t addr) {
    memset(r, 0, sizeof *r);
    r->addr = addr;
}

static bool rf_start(void *ctx, bool read) {
    i2c_regfile_t *r = ctx;
    if (!read) r->have_ptr = false;
    return true;
}

static bool rf_write(void *ctx, uint8_t byte) {
    i2c_regfile_t *r = ctx;
    if (!r->have_ptr) {
        r->ptr = (uint8_t)(byte & ~r->autoinc_bit);
        r->have_ptr = true;
        return true;
    }
    const uint8_t at = r->ptr++;
    r->write_map[at >> 3] |= (uint8_t)(1u << (at & 7u));
    r->writes++;
    r->reg[at] = byte;
    if (r->on_write) r->on_write(r, at, byte);
    return true;
}

static uint8_t rf_read(void *ctx) {
    i2c_regfile_t *r = ctx;
    const uint8_t at = r->ptr++;
    r->read_map[at >> 3] |= (uint8_t)(1u << (at & 7u));
    r->reads++;
    return r->on_read ? r->on_read(r, at) : r->reg[at];
}

s5l_i2c_slave_t i2c_regfile_slave(i2c_regfile_t *r) {
    return (s5l_i2c_slave_t){
        .addr = r->addr, .ctx = r,
        .start = rf_start, .write = rf_write, .read = rf_read, .stop = NULL,
    };
}
