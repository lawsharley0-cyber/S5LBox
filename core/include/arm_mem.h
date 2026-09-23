/*
 * S5LBox — ARM Memory Access Subsystem & Software TLB Fast Paths.
 *
 * Implements high-throughput host pointer cache lookups (dread / dwrite)
 * and software fastmem translation routines for ARM1176 CPU backends.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_ARM_MEM_H
#define S5LBOX_ARM_MEM_H

#include "arm.h"
#include <string.h>

#define ARM_DREAD_BLK_MASK 0x3ffu

/* DRAM Fastmem configuration: 128 MB DRAM window at 0x08000000 */
#define ARM_FASTMEM_RAM_BASE 0x08000000u
#define ARM_FASTMEM_RAM_SIZE (128u * 1024u * 1024u)
#define ARM_FASTMEM_PAGE_SHIFT 12u
#define ARM_FASTMEM_PAGE_COUNT (ARM_FASTMEM_RAM_SIZE >> ARM_FASTMEM_PAGE_SHIFT) /* 32768 */
#define ARM_FASTMEM_BITMAP_WORDS (ARM_FASTMEM_PAGE_COUNT / 32u)                 /* 1024 */

static inline bool arm_fastmem_is_ram_va(uint32_t va) {
    return (va >= ARM_FASTMEM_RAM_BASE && (va - ARM_FASTMEM_RAM_BASE) < ARM_FASTMEM_RAM_SIZE);
}

static inline uint32_t arm_fastmem_page_idx(uint32_t va) {
    return (va - ARM_FASTMEM_RAM_BASE) >> ARM_FASTMEM_PAGE_SHIFT;
}

static inline unsigned dread_slot(uint32_t va, bool priv) {
    return (unsigned)(((va >> 10) + (priv ? ARM_DREAD_ENTRIES / 2u : 0u))
                      & (ARM_DREAD_ENTRIES - 1u));
}

static inline uint32_t dread_tag(uint32_t va, bool priv) {
    return (va & ~ARM_DREAD_BLK_MASK) | (priv ? 1u : 0u);
}

/*
 * A host pointer for an n-byte read at va, or NULL to take the slow path.
 */
static inline const uint8_t *dread_hit(arm_cpu_t *c, uint32_t va, unsigned n,
                                       bool priv) {
#ifdef S5LBOX_NO_DREAD
    (void)va; (void)n; (void)priv;
    c->dread_misses++;
    return NULL;
#else
    if (((va & ARM_DREAD_BLK_MASK) + n) > (ARM_DREAD_BLK_MASK + 1u)) {
        c->dread_misses++;
        return NULL;
    }
    const unsigned slot = dread_slot(va, priv);
    if (!c->dread[slot].host ||
        c->dread[slot].tag != dread_tag(va, priv) ||
        c->dread[slot].gen != c->tlb_gen) {
        c->dread_misses++;
        return NULL;
    }
    c->dread_hits++;
    return c->dread[slot].host + (va & ARM_DREAD_BLK_MASK);
#endif
}

/*
 * Install the block a successful walk just resolved, when it is plain RAM.
 */
static inline void dread_fill(arm_cpu_t *c, uint32_t va, uint32_t pa,
                              bool priv) {
#ifdef S5LBOX_NO_DREAD
    (void)c; (void)va; (void)pa; (void)priv;
    return;
#else
    if (!c || !c->bus || !c->bus->host_ram) return;
    uint8_t *blk = c->bus->host_ram(c->bus->ctx, pa & ~ARM_DREAD_BLK_MASK,
                                    ARM_DREAD_BLK_MASK + 1u);
    if (!blk) return;
    const unsigned slot = dread_slot(va, priv);
    c->dread[slot].host = blk;
    c->dread[slot].tag  = dread_tag(va, priv);
    c->dread[slot].gen  = c->tlb_gen;
#endif
}

/*
 * A host pointer for an n-byte write at va, or NULL to take the slow path.
 */
static inline uint8_t *dwrite_hit(arm_cpu_t *c, uint32_t va, unsigned n,
                                  bool priv) {
    if (!c || !c->bus || !c->bus->host_ram_write) return NULL;
    if (((va & ARM_DREAD_BLK_MASK) + n) > (ARM_DREAD_BLK_MASK + 1u)) {
        c->dwrite_misses++;
        return NULL;
    }
    const unsigned slot = dread_slot(va, priv);
    if (!c->dwrite[slot].host ||
        c->dwrite[slot].tag != dread_tag(va, priv) ||
        c->dwrite[slot].gen != c->tlb_gen) {
        c->dwrite_misses++;
        return NULL;
    }
    c->dwrite_hits++;
    return c->dwrite[slot].host + (va & ARM_DREAD_BLK_MASK);
}

/*
 * Install the write block when host_ram_write is available.
 */
static inline void dwrite_fill(arm_cpu_t *c, uint32_t va, uint32_t pa,
                               bool priv) {
    if (!c || !c->bus || !c->bus->host_ram_write) return;
    uint8_t *blk = c->bus->host_ram_write(
        c->bus->ctx, pa & ~ARM_DREAD_BLK_MASK, ARM_DREAD_BLK_MASK + 1u);
    if (!blk) return;
    const unsigned slot = dread_slot(va, priv);
    c->dwrite[slot].host = blk;
    c->dwrite[slot].tag = dread_tag(va, priv);
    c->dwrite[slot].gen = c->tlb_gen;
}

/*
 * Fastmem memory access helpers for accelerated backends.
 */
static inline void arm_mem_note_abort(arm_cpu_t *c, uint32_t fsr, uint32_t va) {
    if (!c || c->abort_pending) return;
    c->abort_pending = true;
    c->abort_fsr = fsr;
    c->abort_far = va;
}

static inline uint32_t arm_fastmem_read32(arm_cpu_t *c, uint32_t va, bool priv) {
    const uint8_t *dh = dread_hit(c, va, 4, priv);
    if (dh) {
        uint32_t v;
        memcpy(&v, dh, 4);
        return v;
    }
    uint32_t pa = 0;
    uint32_t fsr = arm_mmu_translate(c, va, ARM_ACCESS_READ, priv, &pa);
    if (fsr == 0) {
        if (c->bus && c->bus->host_ram) {
            uint8_t *blk = c->bus->host_ram(c->bus->ctx, pa & ~ARM_DREAD_BLK_MASK,
                                            ARM_DREAD_BLK_MASK + 1u);
            if (blk) {
                const unsigned slot = dread_slot(va, priv);
                c->dread[slot].host = blk;
                c->dread[slot].tag  = dread_tag(va, priv);
                c->dread[slot].gen  = c->tlb_gen;
                c->dread_hits++;
                uint32_t v;
                memcpy(&v, blk + (va & ARM_DREAD_BLK_MASK), 4);
                return v;
            }
        }
        return c->bus->read32(c->bus->ctx, pa);
    }
    arm_mem_note_abort(c, fsr, va);
    return 0;
}

static inline uint16_t arm_fastmem_read16(arm_cpu_t *c, uint32_t va, bool priv) {
    const uint8_t *dh = dread_hit(c, va, 2, priv);
    if (dh) {
        uint16_t v;
        memcpy(&v, dh, 2);
        return v;
    }
    uint32_t pa = 0;
    uint32_t fsr = arm_mmu_translate(c, va, ARM_ACCESS_READ, priv, &pa);
    if (fsr == 0) {
        if (c->bus && c->bus->host_ram) {
            uint8_t *blk = c->bus->host_ram(c->bus->ctx, pa & ~ARM_DREAD_BLK_MASK,
                                            ARM_DREAD_BLK_MASK + 1u);
            if (blk) {
                const unsigned slot = dread_slot(va, priv);
                c->dread[slot].host = blk;
                c->dread[slot].tag  = dread_tag(va, priv);
                c->dread[slot].gen  = c->tlb_gen;
                c->dread_hits++;
                uint16_t v;
                memcpy(&v, blk + (va & ARM_DREAD_BLK_MASK), 2);
                return v;
            }
        }
        return c->bus->read16(c->bus->ctx, pa);
    }
    arm_mem_note_abort(c, fsr, va);
    return 0;
}

static inline uint8_t arm_fastmem_read8(arm_cpu_t *c, uint32_t va, bool priv) {
    const uint8_t *dh = dread_hit(c, va, 1, priv);
    if (dh) return *dh;
    uint32_t pa = 0;
    uint32_t fsr = arm_mmu_translate(c, va, ARM_ACCESS_READ, priv, &pa);
    if (fsr == 0) {
        if (c->bus && c->bus->host_ram) {
            uint8_t *blk = c->bus->host_ram(c->bus->ctx, pa & ~ARM_DREAD_BLK_MASK,
                                            ARM_DREAD_BLK_MASK + 1u);
            if (blk) {
                const unsigned slot = dread_slot(va, priv);
                c->dread[slot].host = blk;
                c->dread[slot].tag  = dread_tag(va, priv);
                c->dread[slot].gen  = c->tlb_gen;
                c->dread_hits++;
                return blk[va & ARM_DREAD_BLK_MASK];
            }
        }
        return c->bus->read8(c->bus->ctx, pa);
    }
    arm_mem_note_abort(c, fsr, va);
    return 0;
}

static inline void arm_fastmem_write32(arm_cpu_t *c, uint32_t va, uint32_t val, bool priv) {
    uint8_t *dw = dwrite_hit(c, va, 4, priv);
    if (dw) {
        memcpy(dw, &val, 4);
        return;
    }
    uint32_t pa = 0;
    uint32_t fsr = arm_mmu_translate(c, va, ARM_ACCESS_WRITE, priv, &pa);
    if (fsr == 0) {
        if (c->bus && c->bus->host_ram_write) {
            uint8_t *blk = c->bus->host_ram_write(
                c->bus->ctx, pa & ~ARM_DREAD_BLK_MASK, ARM_DREAD_BLK_MASK + 1u);
            if (blk) {
                const unsigned slot = dread_slot(va, priv);
                c->dwrite[slot].host = blk;
                c->dwrite[slot].tag  = dread_tag(va, priv);
                c->dwrite[slot].gen  = c->tlb_gen;
                c->dwrite_hits++;
                memcpy(blk + (va & ARM_DREAD_BLK_MASK), &val, 4);
                return;
            }
        }
        c->bus->write32(c->bus->ctx, pa, val);
        return;
    }
    arm_mem_note_abort(c, fsr, va);
}

static inline void arm_fastmem_write16(arm_cpu_t *c, uint32_t va, uint16_t val, bool priv) {
    uint8_t *dw = dwrite_hit(c, va, 2, priv);
    if (dw) {
        memcpy(dw, &val, 2);
        return;
    }
    uint32_t pa = 0;
    uint32_t fsr = arm_mmu_translate(c, va, ARM_ACCESS_WRITE, priv, &pa);
    if (fsr == 0) {
        if (c->bus && c->bus->host_ram_write) {
            uint8_t *blk = c->bus->host_ram_write(
                c->bus->ctx, pa & ~ARM_DREAD_BLK_MASK, ARM_DREAD_BLK_MASK + 1u);
            if (blk) {
                const unsigned slot = dread_slot(va, priv);
                c->dwrite[slot].host = blk;
                c->dwrite[slot].tag  = dread_tag(va, priv);
                c->dwrite[slot].gen  = c->tlb_gen;
                c->dwrite_hits++;
                memcpy(blk + (va & ARM_DREAD_BLK_MASK), &val, 2);
                return;
            }
        }
        c->bus->write16(c->bus->ctx, pa, val);
        return;
    }
    arm_mem_note_abort(c, fsr, va);
}

static inline void arm_fastmem_write8(arm_cpu_t *c, uint32_t va, uint8_t val, bool priv) {
    uint8_t *dw = dwrite_hit(c, va, 1, priv);
    if (dw) {
        *dw = val;
        return;
    }
    uint32_t pa = 0;
    uint32_t fsr = arm_mmu_translate(c, va, ARM_ACCESS_WRITE, priv, &pa);
    if (fsr == 0) {
        if (c->bus && c->bus->host_ram_write) {
            uint8_t *blk = c->bus->host_ram_write(
                c->bus->ctx, pa & ~ARM_DREAD_BLK_MASK, ARM_DREAD_BLK_MASK + 1u);
            if (blk) {
                const unsigned slot = dread_slot(va, priv);
                c->dwrite[slot].host = blk;
                c->dwrite[slot].tag  = dread_tag(va, priv);
                c->dwrite[slot].gen  = c->tlb_gen;
                c->dwrite_hits++;
                blk[va & ARM_DREAD_BLK_MASK] = val;
                return;
            }
        }
        c->bus->write8(c->bus->ctx, pa, val);
        return;
    }
    arm_mem_note_abort(c, fsr, va);
}

#endif /* S5LBOX_ARM_MEM_H */
