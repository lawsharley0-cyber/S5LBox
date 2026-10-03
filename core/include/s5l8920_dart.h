/*
 * NEON — the S5L8920's DARTs, the I/O address translators between bus
 * masters and DRAM ("dart,s5l8920x": /arm-io/dart0 at 0xBFE00000, the
 * display controller's iommu-parent; /arm-io/dart1 at 0xBFF00000).
 *
 * Read from iPhone OS 3.1.3's AppleH2PDART (7E18, setup at 0xc0545124) and
 * the tables it builds:
 *
 *   +0xC bit 31 is translation on: cleared before the tables are loaded,
 *   set (with bits 6:4) after. +0x0 is configuration (0x702).
 *
 *   +0x8 loads one of 16 first-level slots per write: bits 11:8 the slot,
 *   bits 27:12 where its second-level table is (an offset into DRAM, so
 *   0x00F7D001 is the table at DRAM + 0xF7D000), bit 0 valid. Bits 31:28
 *   are kept from a read of the register.
 *
 *   A second-level table is one page of 1024 words: (DRAM offset & 0x0FFFF000)
 *   | 1 maps a page. So an I/O address is slot (bits 25:22), entry (bits
 *   21:12) and offset; 3.1.3 maps from 0x3C000000, where the slots' 64 MiB
 *   begin.
 *
 * The translation reads the tables when it is asked (there is no cached
 * entry to invalidate); the model needs only the guest's DRAM to do it.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef NEON_S5L8920_DART_H
#define NEON_S5L8920_DART_H

#include <stdbool.h>
#include <stdint.h>

#define S5L8920_DART_SIZE      UINT32_C(0x1000)
#define S5L8920_DART_CONFIG    0x00u
#define S5L8920_DART_SLOT      0x08u
#define S5L8920_DART_CONTROL   0x0cu
#define S5L8920_DART_ENABLE    (UINT32_C(1) << 31)
#define S5L8920_DART_SLOTS     16u

typedef struct {
    uint32_t reg[S5L8920_DART_SIZE / 4u];
    uint32_t slot[S5L8920_DART_SLOTS];   /* as written: table offset | valid */
} s5l8920_dart_t;

void     s5l8920_dart_reset(s5l8920_dart_t *d);
uint32_t s5l8920_dart_read(const s5l8920_dart_t *d, uint32_t off);
void     s5l8920_dart_write(s5l8920_dart_t *d, uint32_t off, uint32_t v);

/* The guest's DRAM as the translation reads it: `ram` holds `size` bytes
 * from physical `base`. */
typedef struct {
    const uint8_t *ram;
    uint32_t       base, size;
} s5l8920_dart_ram_t;

/* The physical address of I/O address `iova`; false when translation is
 * off, the slot or page is not mapped, or a table lies outside DRAM. */
bool s5l8920_dart_translate(const s5l8920_dart_t *d, const s5l8920_dart_ram_t *ram,
                            uint32_t iova, uint32_t *pa);

#endif /* NEON_S5L8920_DART_H */
