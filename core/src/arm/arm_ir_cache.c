/*
 * S5LBox — Micro-Op Intermediate Representation (IR) Cache & Compilation.
 *
 * Implements:
 * - Dynamic pool allocation of IR blocks
 * - Hash-table lookup by VA, Thumb mode, and privilege state
 * - Block compilation: Instruction decoding, micro-op lifting, and optimization
 * - Generation tracking and page invalidation
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "arm_ir.h"
#include <stdlib.h>
#include <string.h>

static inline uint32_t ir_block_hash(uint32_t va, bool thumb, bool priv) {
    uint32_t h = (va >> (thumb ? 1 : 2));
    if (thumb) h ^= 0x55555555u;
    if (priv)  h ^= 0xaaaaaaaa;
    h ^= (h >> 16);
    h ^= (h >> 8);
    return h & ARM_IR_CACHE_MASK;
}

arm_ir_cache_t *arm_ir_cache_create(uint32_t pool_capacity) {
    if (pool_capacity == 0) pool_capacity = ARM_IR_DEFAULT_POOL_SIZE;

    arm_ir_cache_t *cache = (arm_ir_cache_t *)malloc(sizeof(arm_ir_cache_t));
    if (!cache) return NULL;
    memset(cache, 0, sizeof(*cache));

    cache->pool = (arm_ir_block_t *)malloc(sizeof(arm_ir_block_t) * pool_capacity);
    if (!cache->pool) {
        free(cache);
        return NULL;
    }
    memset(cache->pool, 0, sizeof(arm_ir_block_t) * pool_capacity);

    cache->pool_capacity = pool_capacity;
    cache->pool_used = 0;
    cache->generation = 1u;
    return cache;
}

void arm_ir_cache_destroy(arm_ir_cache_t *cache) {
    if (!cache) return;
    if (cache->pool) free(cache->pool);
    free(cache);
}

void arm_ir_cache_reset(arm_ir_cache_t *cache) {
    if (!cache) return;
    memset(cache->table, 0, sizeof(cache->table));
    if (cache->pool) {
        memset(cache->pool, 0, sizeof(arm_ir_block_t) * cache->pool_capacity);
    }
    memset(cache->code_pages, 0, sizeof(cache->code_pages));
    cache->pool_used = 0;
    cache->generation = 1u;
    cache->lookups = 0;
    cache->hits = 0;
    cache->misses = 0;
    cache->invalidations = 0;
}

void arm_ir_cache_invalidate(arm_ir_cache_t *cache) {
    if (!cache) return;
    cache->invalidations++;
    cache->generation++;
    memset(cache->code_pages, 0, sizeof(cache->code_pages));
    if (cache->generation == 0u) {
        memset(cache->table, 0, sizeof(cache->table));
        cache->pool_used = 0;
        cache->generation = 1u;
    }
}

void arm_ir_cache_mark_page_code(arm_ir_cache_t *cache, uint32_t page_va) {
    if (!cache) return;
    if (arm_fastmem_is_ram_va(page_va)) {
        uint32_t idx = arm_fastmem_page_idx(page_va);
        cache->code_pages[idx / 32u] |= (1u << (idx % 32u));
    }
}

bool arm_ir_cache_is_page_code(const arm_ir_cache_t *cache, uint32_t page_va) {
    if (!cache) return false;
    if (arm_fastmem_is_ram_va(page_va)) {
        uint32_t idx = arm_fastmem_page_idx(page_va);
        return (cache->code_pages[idx / 32u] & (1u << (idx % 32u))) != 0;
    }
    return false;
}

void arm_ir_cache_clear_page_code(arm_ir_cache_t *cache, uint32_t page_va) {
    if (!cache) return;
    if (arm_fastmem_is_ram_va(page_va)) {
        uint32_t idx = arm_fastmem_page_idx(page_va);
        cache->code_pages[idx / 32u] &= ~(1u << (idx % 32u));
    }
}

void arm_ir_cache_invalidate_page(arm_ir_cache_t *cache, uint32_t page_va) {
    if (!cache) return;
    arm_ir_cache_clear_page_code(cache, page_va);
    uint32_t page_base = page_va & ~0xfffu;
    for (size_t i = 0; i < ARM_IR_CACHE_SIZE; i++) {
        arm_ir_block_t *b = cache->table[i];
        if (b && b->valid && (b->start_va & ~0xfffu) == page_base) {
            b->valid = false;
            cache->table[i] = NULL;
        }
    }
}

arm_ir_block_t *arm_ir_cache_lookup(arm_ir_cache_t *cache, uint32_t va, bool thumb, bool priv) {
    if (!cache) return NULL;
    cache->lookups++;
    uint32_t idx = ir_block_hash(va, thumb, priv);
    arm_ir_block_t *entry = cache->table[idx];

    if (entry &&
        entry->valid &&
        entry->generation == cache->generation &&
        entry->start_va == va &&
        entry->thumb == thumb &&
        entry->priv == priv) {
        cache->hits++;
        return entry;
    }

    cache->misses++;
    return NULL;
}

arm_ir_block_t *arm_ir_compile_block(arm_ir_cache_t *cache, arm_cpu_t *cpu,
                                     uint32_t start_va, bool thumb, bool priv) {
    if (!cache || !cpu || !cpu->bus) return NULL;

    uint32_t start_pa = 0u;
    if (arm_mmu_translate(cpu, start_va, ARM_ACCESS_FETCH, priv, &start_pa) != 0u) {
        return NULL;
    }

    if (cache->pool_used >= cache->pool_capacity) {
        memset(cache->table, 0, sizeof(cache->table));
        cache->pool_used = 0;
        cache->generation++;
    }

    uint32_t idx = ir_block_hash(start_va, thumb, priv);
    arm_ir_block_t *block = &cache->pool[cache->pool_used++];
    memset(block, 0, sizeof(*block));

    block->start_va = start_va;
    block->start_pa = start_pa;
    block->thumb = thumb;
    block->priv = priv;
    block->generation = cache->generation;
    block->exit_type = ARM_EXIT_SEQUENTIAL;
    block->cache = cache;
    arm_ir_cache_mark_page_code(cache, start_va);

    uint32_t cur_va = start_va;
    const uint32_t block_limit = start_va & ~0x3ffu; /* Bound inside current 1 KB fetch page */
    unsigned ir_insn_idx = 0;
    unsigned decoded_insn_count = 0;

    while (decoded_insn_count < ARM_BLOCK_MAX_INSNS && ir_insn_idx + 8 < ARM_IR_MAX_INSNS) {
        if ((cur_va & ~0x3ffu) != block_limit) {
            block->exit_type = ARM_EXIT_PAGE_BOUNDARY;
            block->fallthrough_target = cur_va;
            break;
        }

        uint32_t raw = 0u;
        uint32_t cur_pa = 0u;
        if (arm_mmu_translate(cpu, cur_va, ARM_ACCESS_FETCH, priv, &cur_pa) != 0u) {
            break;
        }

        if (thumb) {
            if (cpu->fetch_host && (cur_va & ~0x3ffu) == cpu->fetch_blk &&
                cpu->fetch_gen == cpu->tlb_gen && cpu->fetch_priv == priv) {
                const uint8_t *h = cpu->fetch_host + (cur_va - cpu->fetch_blk);
                raw = (uint32_t)h[0] | ((uint32_t)h[1] << 8);
            } else {
                raw = cpu->bus->read16(cpu->bus->ctx, cur_pa);
            }
        } else {
            if (cpu->fetch_host && (cur_va & ~0x3ffu) == cpu->fetch_blk &&
                cpu->fetch_gen == cpu->tlb_gen && cpu->fetch_priv == priv) {
                const uint8_t *h = cpu->fetch_host + (cur_va - cpu->fetch_blk);
                raw = (uint32_t)h[0] | ((uint32_t)h[1] << 8) |
                      ((uint32_t)h[2] << 16) | ((uint32_t)h[3] << 24);
            } else {
                raw = cpu->bus->read32(cpu->bus->ctx, cur_pa);
            }
        }

        arm_decoded_insn_t di;
        if (!arm_decode_instruction(cpu, cur_va, raw, thumb, &di)) {
            break;
        }

        if (di.op == ARM_OP_FALLBACK) {
            if (decoded_insn_count == 0) {
                return NULL;
            }
            block->exit_type = ARM_EXIT_FALLBACK;
            block->fallthrough_target = cur_va;
            break;
        }

        unsigned written = 0;
        if (!arm_ir_lift_instruction(&di, &block->insns[ir_insn_idx],
                                     ARM_IR_MAX_INSNS - ir_insn_idx, &written)) {
            break;
        }

        ir_insn_idx += written;
        decoded_insn_count++;

        if (di.op == ARM_OP_SVC) {
            block->exit_type = ARM_EXIT_SVC;
            block->fallthrough_target = di.next_pc;
            break;
        }

        if (di.writes_pc) {
            if (di.op == ARM_OP_B || di.op == ARM_OP_BL) {
                if (di.cond_always) {
                    block->exit_type = ARM_EXIT_BRANCH_DIRECT;
                    block->branch_target = di.imm;
                } else {
                    block->exit_type = ARM_EXIT_BRANCH_COND;
                    block->branch_target = di.imm;
                    block->fallthrough_target = di.next_pc;
                }
            } else if (di.op == ARM_OP_BLX_IMM) {
                block->exit_type = ARM_EXIT_BRANCH_DIRECT;
                block->branch_target = di.imm;
            } else {
                block->exit_type = ARM_EXIT_INDIRECT;
            }
            break;
        }

        cur_va = di.next_pc;
    }

    if (ir_insn_idx == 0) {
        return NULL;
    }

    block->insn_count = ir_insn_idx;

    /* Phase 9: Run optimization passes (constant folding, dead code elimination, redundant flags pruning) */
    arm_ir_optimize_block(block);

    block->valid = true;
    cache->table[idx] = block;
    return block;
}
