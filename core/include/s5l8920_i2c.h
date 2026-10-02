/*
 * NEON — the S5L8920's I2C controllers ("i2c,s5l8920x": /arm-io/i2c0 at
 * 0x83200000, line 0x13; /arm-io/i2c2 at 0x83400000, line 0x11), and a
 * register-file slave the devices on them are built from.
 *
 * Not the S5L8900's Samsung-style controller (soc.h's s5l_i2c_t): this one
 * runs a whole transfer from a few registers. Read from iPhone OS 3.1.3's
 * AppleS5L8920XI2CController (7E18, the class at 0xc0678dc0..0xc0679144):
 *
 *   enable (0xc0678dc0): +0xC = 0x37 (clear the status), +0x8 = 0x30
 *   (interrupt enable: done and error); disabling writes +0x8 = 0.
 *
 *   transfer (0xc0678f38): +0x0 = the slave's 7-bit address, +0x10 = the
 *   first byte after the address (a register number, or a write's first
 *   data byte), +0x18 = the count of further bytes, a write's bytes into the
 *   FIFO at +0x20, then +0x24 = 4 | write. A read sends the +0x10 byte, then
 *   a repeated start, and reads `count` bytes.
 *
 *   interrupt (0xc0678e94): reads +0xC: bit 5 an error (no acknowledge),
 *   bit 4 done, after which a read pops its bytes from +0x20; then writes
 *   the value back, clearing it. Polled when interrupts are off.
 *
 * A transfer completes as soon as it starts. The line is (status & enable).
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef NEON_S5L8920_I2C_H
#define NEON_S5L8920_I2C_H

#include <stdbool.h>
#include <stdint.h>

#include "soc.h"                 /* s5l_i2c_slave_t */

#define S5L8920_I2C_SIZE      UINT32_C(0x1000)

#define S5L8920_I2C_ADDR      0x00u
#define S5L8920_I2C_IEN       0x08u
#define S5L8920_I2C_STATUS    0x0cu     /* write one to clear             */
#define S5L8920_I2C_FIRST     0x10u
#define S5L8920_I2C_COUNT     0x18u
#define S5L8920_I2C_FIFO      0x20u
#define S5L8920_I2C_CONTROL   0x24u

#define S5L8920_I2C_CTRL_WRITE  (1u << 0)
#define S5L8920_I2C_CTRL_START  (1u << 2)
#define S5L8920_I2C_ST_DONE     (1u << 4)
#define S5L8920_I2C_ST_ERROR    (1u << 5)

#define S5L8920_I2C_SLAVES    8u
#define S5L8920_I2C_FIFO_LEN  256u

typedef struct {
    uint32_t reg[S5L8920_I2C_SIZE / 4u];   /* everything not below          */
    uint32_t status;
    uint8_t  fifo[S5L8920_I2C_FIFO_LEN];
    unsigned fifo_len, fifo_pos;           /* a write's bytes, or a read's  */
    bool     rx;                           /* the FIFO holds a read's bytes  */

    s5l_i2c_slave_t slaves[S5L8920_I2C_SLAVES];
    unsigned        slave_count;

    uint64_t transfers, naks, bytes_tx, bytes_rx;
} s5l8920_i2c_t;

/* Registers to 0, slaves kept. */
void     s5l8920_i2c_reset(s5l8920_i2c_t *c);
bool     s5l8920_i2c_attach(s5l8920_i2c_t *c, const s5l_i2c_slave_t *slave);
uint32_t s5l8920_i2c_read(s5l8920_i2c_t *c, uint32_t off);
void     s5l8920_i2c_write(s5l8920_i2c_t *c, uint32_t off, uint32_t v);
bool     s5l8920_i2c_irq(const s5l8920_i2c_t *c);

/*
 * A slave that is a file of 256 byte registers, the usual shape: after a
 * start for writing, the first byte sets the register pointer and later ones
 * store and advance it; reads return the register at the pointer and
 * advance it. `autoinc_bit`, when set, is masked out of the register byte
 * (the LIS3xx accelerometers' auto-increment flag). Hooks may compute a
 * read or see a write; every register touched is recorded.
 */
typedef struct i2c_regfile {
    uint8_t  addr;
    uint8_t  autoinc_bit;
    uint8_t  reg[256];
    uint8_t  ptr;
    bool     have_ptr;
    uint8_t (*on_read)(struct i2c_regfile *r, uint8_t reg);   /* NULL: stored */
    void    (*on_write)(struct i2c_regfile *r, uint8_t reg, uint8_t v);
    void    *ctx;
    uint8_t  read_map[32], write_map[32];
    uint64_t reads, writes;
} i2c_regfile_t;

void i2c_regfile_init(i2c_regfile_t *r, uint8_t addr);
/* The slave as a controller sees it; `r` must outlive the controller. */
s5l_i2c_slave_t i2c_regfile_slave(i2c_regfile_t *r);

#endif /* NEON_S5L8920_I2C_H */
