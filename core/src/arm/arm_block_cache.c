/*
 * S5LBox — Basic Block Cache & Direct Linking.
 *
 * Implements:
 * - Basic block discovery and compilation
 * - Fast pointer hash-table lookup
 * - Page invalidation and generation management
 * - Direct block linking (chaining)
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "arm_block.h"
#include <stdlib.h>
#include <string.h>

static inline uint32_t block_hash(uint32_t va, bool thumb, bool priv) {
    uint32_t h = (va >> (thumb ? 1 : 2));
    if (thumb) h ^= 0x55555555u;
    if (priv)  h ^= 0xaaaaaaaa;
    h ^= (h >> 16);
    h ^= (h >> 8);
    return h & ARM_BLOCK_CACHE_MASK;
}

arm_block_cache_t *arm_block_cache_create(uint32_t pool_capacity) {
    if (pool_capacity == 0) pool_capacity = ARM_BLOCK_DEFAULT_POOL_SIZE;

    arm_block_cache_t *cache = (arm_block_cache_t *)malloc(sizeof(arm_block_cache_t));
    if (!cache) return NULL;
    memset(cache, 0, sizeof(*cache));

    cache->pool = (arm_basic_block_t *)malloc(sizeof(arm_basic_block_t) * pool_capacity);
    if (!cache->pool) {
        free(cache);
        return NULL;
    }
    memset(cache->pool, 0, sizeof(arm_basic_block_t) * pool_capacity);

    cache->pool_capacity = pool_capacity;
    cache->pool_used = 0;
    cache->generation = 1u;
#if defined(S5LBOX_JIT)
    if (jit_host_can_execute()) {
        cache->jit_arena_valid = jit_buf_alloc(&cache->jit_arena, 4 * 1024 * 1024);
    }
#endif
    return cache;
}

void arm_block_cache_destroy(arm_block_cache_t *cache) {
    if (!cache) return;
#if defined(S5LBOX_JIT)
    if (cache->jit_arena_valid) {
        jit_buf_free(&cache->jit_arena);
        cache->jit_arena_valid = false;
    }
#endif
    if (cache->pool) free(cache->pool);
    free(cache);
}

void arm_block_cache_reset(arm_block_cache_t *cache) {
    if (!cache) return;
    memset(cache->table, 0, sizeof(cache->table));
    if (cache->pool) {
        memset(cache->pool, 0, sizeof(arm_basic_block_t) * cache->pool_capacity);
    }
    memset(cache->code_pages, 0, sizeof(cache->code_pages));
    cache->pool_used = 0;
    cache->generation = 1u;
    cache->lookups = 0;
    cache->hits = 0;
    cache->misses = 0;
    cache->invalidations = 0;
    cache->links_taken = 0;
}

void arm_block_cache_invalidate(arm_block_cache_t *cache) {
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

void arm_block_cache_mark_page_code(arm_block_cache_t *cache, uint32_t page_va) {
    if (!cache) return;
    if (arm_fastmem_is_ram_va(page_va)) {
        uint32_t idx = arm_fastmem_page_idx(page_va);
        cache->code_pages[idx / 32u] |= (1u << (idx % 32u));
    }
}

bool arm_block_cache_is_page_code(const arm_block_cache_t *cache, uint32_t page_va) {
    if (!cache) return false;
    if (arm_fastmem_is_ram_va(page_va)) {
        uint32_t idx = arm_fastmem_page_idx(page_va);
        return (cache->code_pages[idx / 32u] & (1u << (idx % 32u))) != 0;
    }
    return false;
}

void arm_block_cache_clear_page_code(arm_block_cache_t *cache, uint32_t page_va) {
    if (!cache) return;
    if (arm_fastmem_is_ram_va(page_va)) {
        uint32_t idx = arm_fastmem_page_idx(page_va);
        cache->code_pages[idx / 32u] &= ~(1u << (idx % 32u));
    }
}

void arm_block_cache_invalidate_page(arm_block_cache_t *cache, uint32_t page_va) {
    if (!cache) return;
    arm_block_cache_clear_page_code(cache, page_va);
    uint32_t page_base = page_va & ~0xfffu; /* 4 KB page */
    for (size_t i = 0; i < ARM_BLOCK_CACHE_SIZE; i++) {
        arm_basic_block_t *b = cache->table[i];
        if (b && b->valid && (b->start_va & ~0xfffu) == page_base) {
            b->valid = false;
            b->link_target = NULL;
            b->link_fallthrough = NULL;
            cache->table[i] = NULL;
        }
    }
}

arm_basic_block_t *arm_block_cache_lookup(arm_block_cache_t *cache,
                                          uint32_t va, bool thumb, bool priv) {
    if (!cache) return NULL;
    cache->lookups++;
    uint32_t idx = block_hash(va, thumb, priv);
    arm_basic_block_t *entry = cache->table[idx];

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

arm_basic_block_t *arm_block_compile(arm_block_cache_t *cache,
                                     arm_cpu_t *cpu,
                                     uint32_t start_va,
                                     bool thumb,
                                     bool priv) {
    if (!cache || !cpu || !cpu->bus) return NULL;

    /* Verify virtual-to-physical translation */
    uint32_t start_pa = 0u;
    uint32_t fsr = arm_mmu_translate(cpu, start_va, ARM_ACCESS_FETCH, priv, &start_pa);
    if (fsr != 0u) {
        /* Prefetch fault; leave to reference interpreter */
        return NULL;
    }

    /* Allocate block from pool */
    if (cache->pool_used >= cache->pool_capacity) {
        /* Pool exhausted: reset cache table and wrap pool */
        memset(cache->table, 0, sizeof(cache->table));
        cache->pool_used = 0;
        cache->generation++;
    }

    uint32_t idx = block_hash(start_va, thumb, priv);
    arm_basic_block_t *block = &cache->pool[cache->pool_used++];
    memset(block, 0, sizeof(*block));

    block->start_va = start_va;
    block->start_pa = start_pa;
    block->thumb = thumb;
    block->priv = priv;
    block->generation = cache->generation;
    block->exit_type = ARM_EXIT_SEQUENTIAL;
    block->cache = cache;
    arm_block_cache_mark_page_code(cache, start_va);

    uint32_t cur_va = start_va;
    const uint32_t block_limit = start_va & ~0x3ffu; /* Bound inside current 1 KB fetch page */
    unsigned insn_idx = 0;

    while (insn_idx < ARM_BLOCK_MAX_INSNS) {
        /* Stop if instruction crosses 1 KB boundary */
        if ((cur_va & ~0x3ffu) != block_limit) {
            block->exit_type = ARM_EXIT_PAGE_BOUNDARY;
            block->fallthrough_target = cur_va;
            break;
        }

        /* Fetch raw instruction word/halfword */
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

        arm_decoded_insn_t *di = &block->insns[insn_idx];
        if (!arm_decode_instruction(cpu, cur_va, raw, thumb, di)) {
            break;
        }

        insn_idx++;

        /* Handle block termination */
        if (di->op == ARM_OP_FALLBACK) {
            block->exit_type = ARM_EXIT_FALLBACK;
            block->fallthrough_target = cur_va;
            break;
        }

        if (di->op == ARM_OP_SVC) {
            block->exit_type = ARM_EXIT_SVC;
            block->fallthrough_target = di->next_pc;
            break;
        }

        if (di->writes_pc) {
            if (di->op == ARM_OP_B || di->op == ARM_OP_BL) {
                if (di->cond_always) {
                    block->exit_type = ARM_EXIT_BRANCH_DIRECT;
                    block->branch_target = di->imm;
                } else {
                    block->exit_type = ARM_EXIT_BRANCH_COND;
                    block->branch_target = di->imm;
                    block->fallthrough_target = di->next_pc;
                }
            } else if (di->op == ARM_OP_BLX_IMM) {
                block->exit_type = ARM_EXIT_BRANCH_DIRECT;
                block->branch_target = di->imm;
            } else {
                block->exit_type = ARM_EXIT_INDIRECT;
            }
            break;
        }

        cur_va = di->next_pc;
    }

    if (insn_idx == 0) {
        return NULL;
    }

    block->insn_count = insn_idx;
    block->valid = true;

#if defined(S5LBOX_JIT)
    if (cache->jit_arena_valid && insn_idx > 0) {
        uint32_t temp_code[512];
        if (jit_translate(cpu, start_va, temp_code, 512, &block->jit_blk)) {
            jit_buf_begin_write(&cache->jit_arena);
            uint32_t *dest = jit_buf_take(&cache->jit_arena, block->jit_blk.code_words);
            if (dest) {
                memcpy(dest, temp_code, block->jit_blk.code_words * sizeof(uint32_t));
                block->jit_blk.code = dest;
                jit_buf_commit(&cache->jit_arena, dest, block->jit_blk.code_words * sizeof(uint32_t));
                jit_block_commit(&cache->jit_arena, &block->jit_blk);
                block->jit_compiled = true;
            }
            jit_buf_end_write(&cache->jit_arena);
        }
    }
#endif

    cache->table[idx] = block;
    return block;
}

void arm_block_link(arm_basic_block_t *from,
                    arm_basic_block_t *to_target,
                    arm_basic_block_t *to_fallthrough) {
    if (!from) return;
    if (to_target && to_target->valid && to_target->generation == from->generation) {
        from->link_target = to_target;
    }
    if (to_fallthrough && to_fallthrough->valid && to_fallthrough->generation == from->generation) {
        from->link_fallthrough = to_fallthrough;
    }
}

void arm_block_unlink_all(arm_block_cache_t *cache) {
    if (!cache) return;
    for (size_t i = 0; i < ARM_BLOCK_CACHE_SIZE; i++) {
        if (cache->table[i]) {
            cache->table[i]->link_target = NULL;
            cache->table[i]->link_fallthrough = NULL;
        }
    }
}
