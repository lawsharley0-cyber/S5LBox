/*
 * S5LBox — Unit test for Basic Block Cache & Direct Linking.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "arm.h"
#include "arm_block.h"
#include <stdio.h>
#include <string.h>

#define RAM_SIZE (1024 * 1024)
static uint8_t g_ram[RAM_SIZE];

static uint32_t m_r32(void *ctx, uint32_t a){ (void)ctx; uint32_t v; memcpy(&v, &g_ram[a & (RAM_SIZE-1)], 4); return v; }
static uint16_t m_r16(void *ctx, uint32_t a){ (void)ctx; uint16_t v; memcpy(&v, &g_ram[a & (RAM_SIZE-1)], 2); return v; }
static uint8_t  m_r8 (void *ctx, uint32_t a){ (void)ctx; return g_ram[a & (RAM_SIZE-1)]; }
static void m_w32(void *ctx, uint32_t a, uint32_t v){ (void)ctx; memcpy(&g_ram[a & (RAM_SIZE-1)], &v, 4); }
static void m_w16(void *ctx, uint32_t a, uint16_t v){ (void)ctx; memcpy(&g_ram[a & (RAM_SIZE-1)], &v, 2); }
static void m_w8 (void *ctx, uint32_t a, uint8_t  v){ (void)ctx; g_ram[a & (RAM_SIZE-1)] = v; }
static uint8_t *m_host_ram(void *ctx, uint32_t a, uint32_t len){ (void)ctx; if ((uint64_t)a + len > RAM_SIZE) return NULL; return &g_ram[a]; }

static const arm_bus_t g_bus = {
    .ctx = NULL,
    .read32 = m_r32, .read16 = m_r16, .read8 = m_r8,
    .write32 = m_w32, .write16 = m_w16, .write8 = m_w8,
    .host_ram = m_host_ram,
    .host_ram_write = m_host_ram
};

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL: %s at %s:%d\n", msg, __FILE__, __LINE__); \
            fflush(stderr); \
            printf("FAIL: %s at %s:%d\n", msg, __FILE__, __LINE__); \
            fflush(stdout); \
            return 1; \
        } \
    } while (0)

int main(void) {
    arm_cpu_t cpu;
    arm_reset(&cpu, &g_bus);

    arm_block_cache_t *cache = arm_block_cache_create(256);
    CHECK(cache != NULL, "arm_block_cache_create should succeed");

    /* Write code for Block 1 (0x1000):
     * 0x1000: MOV r0, #1
     * 0x1004: ADD r1, r0, #2
     * 0x1008: B 0x1020 (target block 2)
     */
    m_w32(NULL, 0x1000, 0xe3a00001u); /* MOV r0, #1 */
    m_w32(NULL, 0x1004, 0xe2801002u); /* ADD r1, r0, #2 */
    m_w32(NULL, 0x1008, 0xea000004u); /* B 0x1020 (offset = +4 words from pc+8) */

    /* Write code for Block 2 (0x1020):
     * 0x1020: ADD r2, r1, #3
     * 0x1024: BX lr
     */
    m_w32(NULL, 0x1020, 0xe2812003u); /* ADD r2, r1, #3 */
    m_w32(NULL, 0x1024, 0xe12fff1eu); /* BX lr (r14) */

    /* Compile Block 1 */
    arm_basic_block_t *b1 = arm_block_compile(cache, &cpu, 0x1000, false, true);
    CHECK(b1 != NULL, "Block 1 compile should succeed");
    CHECK(b1->insn_count == 3, "Block 1 should contain 3 instructions");
    CHECK(b1->exit_type == ARM_EXIT_BRANCH_DIRECT, "Block 1 exit should be direct branch");
    CHECK(b1->branch_target == 0x1020, "Block 1 target should be 0x1020");

    /* Lookup Block 1 */
    arm_basic_block_t *lookup1 = arm_block_cache_lookup(cache, 0x1000, false, true);
    CHECK(lookup1 == b1, "Lookup should return Block 1");
    CHECK(cache->hits == 1, "Cache hit count should be 1");

    /* Compile Block 2 */
    arm_basic_block_t *b2 = arm_block_compile(cache, &cpu, 0x1020, false, true);
    CHECK(b2 != NULL, "Block 2 compile should succeed");
    CHECK(b2->insn_count == 2, "Block 2 should contain 2 instructions");
    CHECK(b2->exit_type == ARM_EXIT_INDIRECT, "Block 2 exit should be indirect (BX lr)");

    /* Test Direct Block Linking (Phase 6) */
    arm_block_link(b1, b2, NULL);
    CHECK(b1->link_target == b2, "Block 1 direct link target should be Block 2");

    /* Execute Block 1 then follow direct link to Block 2 */
    cpu.r[14] = 0x2000; /* Return address for BX lr */
    cpu.r[15] = 0x1000;

    unsigned retired = 0;
    CHECK(arm_block_exec(&cpu, b1, &retired) == ARM_OK, "Exec Block 1");
    CHECK(retired == 3, "Block 1 should retire 3 instructions");
    CHECK(cpu.r[15] == 0x1020, "PC should be at Block 2 start");

    /* Follow direct link! */
    arm_basic_block_t *next_block = b1->link_target;
    CHECK(next_block == b2, "Next block should be Block 2");

    unsigned retired2 = 0;
    CHECK(arm_block_exec(&cpu, next_block, &retired2) == ARM_OK, "Exec Block 2");
    CHECK(retired2 == 2, "Block 2 should retire 2 instructions");
    CHECK(cpu.r[0] == 1, "r0 == 1");
    CHECK(cpu.r[1] == 3, "r1 == 3");
    CHECK(cpu.r[2] == 6, "r2 == 6");
    CHECK(cpu.r[15] == 0x2000, "PC should return to lr (0x2000)");

    /* Test Invalidation */
    arm_block_cache_invalidate_page(cache, 0x1000);
    arm_basic_block_t *lookup_after = arm_block_cache_lookup(cache, 0x1000, false, true);
    CHECK(lookup_after == NULL, "Block 1 should be invalid after page invalidation");

    arm_block_cache_destroy(cache);
    printf("test_block_cache: all checks passed!\n");
    return 0;
}
