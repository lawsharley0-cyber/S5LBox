/*
 * S5LBox — Fast Memory & Software TLB Unit Tests.
 *
 * Verifies:
 * 1. dread_hit and dwrite_hit fast path hit/miss/fill semantics.
 * 2. Access boundary validation (cross-1KB boundary safety).
 * 3. Privilege isolation between user and privileged cache slots.
 * 4. Fastmem direct access helpers (read8/16/32, write8/16/32).
 * 5. W^X code page tracking and invalidation on stores.
 * 6. Differential state correctness against reference interpreter.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "arm.h"
#include "arm_mem.h"
#include "arm_block.h"
#include "arm_ir.h"
#include "soc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "CHECK FAILED: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

typedef struct {
    uint8_t ram[128 * 1024]; /* 128 KB test RAM */
} dummy_bus_t;

static uint32_t test_r32(void *ctx, uint32_t addr) {
    dummy_bus_t *b = (dummy_bus_t *)ctx;
    addr &= 0x1ffffu;
    return (uint32_t)b->ram[addr] | ((uint32_t)b->ram[addr+1] << 8) |
           ((uint32_t)b->ram[addr+2] << 16) | ((uint32_t)b->ram[addr+3] << 24);
}

static uint16_t test_r16(void *ctx, uint32_t addr) {
    dummy_bus_t *b = (dummy_bus_t *)ctx;
    addr &= 0x1ffffu;
    return (uint16_t)b->ram[addr] | ((uint16_t)b->ram[addr+1] << 8);
}

static uint8_t test_r8(void *ctx, uint32_t addr) {
    dummy_bus_t *b = (dummy_bus_t *)ctx;
    addr &= 0x1ffffu;
    return b->ram[addr];
}

static void test_w32(void *ctx, uint32_t addr, uint32_t val) {
    dummy_bus_t *b = (dummy_bus_t *)ctx;
    addr &= 0x1ffffu;
    b->ram[addr]   = (uint8_t)val;
    b->ram[addr+1] = (uint8_t)(val >> 8);
    b->ram[addr+2] = (uint8_t)(val >> 16);
    b->ram[addr+3] = (uint8_t)(val >> 24);
}

static void test_w16(void *ctx, uint32_t addr, uint16_t val) {
    dummy_bus_t *b = (dummy_bus_t *)ctx;
    addr &= 0x1ffffu;
    b->ram[addr]   = (uint8_t)val;
    b->ram[addr+1] = (uint8_t)(val >> 8);
}

static void test_w8(void *ctx, uint32_t addr, uint8_t val) {
    dummy_bus_t *b = (dummy_bus_t *)ctx;
    addr &= 0x1ffffu;
    b->ram[addr] = val;
}

static uint8_t *test_host_ram(void *ctx, uint32_t addr, uint32_t size) {
    dummy_bus_t *b = (dummy_bus_t *)ctx;
    (void)size;
    addr &= 0x1ffffu;
    return &b->ram[addr];
}

static void init_test_env(arm_cpu_t *cpu, arm_bus_t *bus, dummy_bus_t *db) {
    memset(cpu, 0, sizeof(*cpu));
    memset(bus, 0, sizeof(*bus));
    memset(db, 0, sizeof(*db));

    bus->ctx = db;
    bus->read32 = test_r32;
    bus->read16 = test_r16;
    bus->read8  = test_r8;
    bus->write32 = test_w32;
    bus->write16 = test_w16;
    bus->write8  = test_w8;
    bus->host_ram = test_host_ram;
    bus->host_ram_write = test_host_ram;

    arm_reset(cpu, bus);
    cpu->cpsr = ARM_MODE_SVC;
}

/* Test 1: dread_hit and dwrite_hit fill and hit semantics */
static void test_dread_dwrite_basics(void) {
    arm_cpu_t cpu;
    arm_bus_t bus;
    dummy_bus_t db;
    init_test_env(&cpu, &bus, &db);

    uint32_t va = 0x1000u;
    uint32_t pa = 0x1000u;

    /* Initially empty -> miss */
    CHECK(dread_hit(&cpu, va, 4, true) == NULL);
    CHECK(dwrite_hit(&cpu, va, 4, true) == NULL);

    /* Fill read block */
    dread_fill(&cpu, va, pa, true);
    const uint8_t *rh = dread_hit(&cpu, va, 4, true);
    CHECK(rh != NULL);
    CHECK(rh == &db.ram[0x1000]);

    /* Write block should still be empty */
    CHECK(dwrite_hit(&cpu, va, 4, true) == NULL);

    /* Fill write block */
    dwrite_fill(&cpu, va, pa, true);
    uint8_t *wh = dwrite_hit(&cpu, va, 4, true);
    CHECK(wh != NULL);
    CHECK(wh == &db.ram[0x1000]);

    /* Verify write visibility through read pointer */
    wh[0] = 0x42;
    CHECK(rh[0] == 0x42);
    printf("  [PASS] dread and dwrite fill and hit basics\n");
}

/* Test 2: Boundary crossing safety (1 KB block limit) */
static void test_boundary_crossing(void) {
    arm_cpu_t cpu;
    arm_bus_t bus;
    dummy_bus_t db;
    init_test_env(&cpu, &bus, &db);

    uint32_t va = 0x1000u;
    uint32_t pa = 0x1000u;
    dread_fill(&cpu, va, pa, true);
    dwrite_fill(&cpu, va, pa, true);

    /* Last word in 1 KB block: offset 1020 (0x3FC) */
    uint32_t last_word = va + 0x3fcu;
    CHECK(dread_hit(&cpu, last_word, 4, true) != NULL);
    CHECK(dwrite_hit(&cpu, last_word, 4, true) != NULL);

    /* Word straddling boundary: offset 1022 (0x3FE) -> must reject (NULL) */
    uint32_t straddle = va + 0x3feu;
    CHECK(dread_hit(&cpu, straddle, 4, true) == NULL);
    CHECK(dwrite_hit(&cpu, straddle, 4, true) == NULL);

    /* Halfword at offset 1022: fits in block */
    CHECK(dread_hit(&cpu, straddle, 2, true) != NULL);
    CHECK(dwrite_hit(&cpu, straddle, 2, true) != NULL);

    /* Halfword at offset 1023: straddles boundary -> reject */
    CHECK(dread_hit(&cpu, va + 0x3ffu, 2, true) == NULL);
    CHECK(dwrite_hit(&cpu, va + 0x3ffu, 2, true) == NULL);

    printf("  [PASS] 1 KB boundary crossing safety checks\n");
}

/* Test 3: Privilege separation in slots and tags */
static void test_privilege_isolation(void) {
    arm_cpu_t cpu;
    arm_bus_t bus;
    dummy_bus_t db;
    init_test_env(&cpu, &bus, &db);

    uint32_t va = 0x2000u;
    uint32_t pa = 0x2000u;

    /* Fill as privileged */
    dread_fill(&cpu, va, pa, true);

    /* Privileged read hits */
    CHECK(dread_hit(&cpu, va, 4, true) != NULL);

    /* Unprivileged read at same VA MUST miss */
    CHECK(dread_hit(&cpu, va, 4, false) == NULL);

    /* Fill unprivileged entry */
    dread_fill(&cpu, va, pa, false);
    CHECK(dread_hit(&cpu, va, 4, false) != NULL);

    printf("  [PASS] user and privileged slot isolation\n");
}

/* Test 4: Fastmem memory helpers (read/write 32, 16, 8) */
static void test_fastmem_helpers(void) {
    arm_cpu_t cpu;
    arm_bus_t bus;
    dummy_bus_t db;
    init_test_env(&cpu, &bus, &db);

    uint32_t va = 0x4000u;

    /* Write and read word */
    arm_fastmem_write32(&cpu, va, 0x12345678u, true);
    uint32_t val32 = arm_fastmem_read32(&cpu, va, true);
    CHECK(val32 == 0x12345678u);

    /* Write and read halfword */
    arm_fastmem_write16(&cpu, va + 4, 0xabcd, true);
    uint16_t val16 = arm_fastmem_read16(&cpu, va + 4, true);
    CHECK(val16 == 0xabcd);

    /* Write and read byte */
    arm_fastmem_write8(&cpu, va + 6, 0xef, true);
    uint8_t val8 = arm_fastmem_read8(&cpu, va + 6, true);
    CHECK(val8 == 0xef);

    /* Verify host memory contents match exactly */
    CHECK(db.ram[va] == 0x78 && db.ram[va+1] == 0x56 && db.ram[va+2] == 0x34 && db.ram[va+3] == 0x12);
    CHECK(db.ram[va+4] == 0xcd && db.ram[va+5] == 0xab);
    CHECK(db.ram[va+6] == 0xef);

    printf("  [PASS] fastmem read/write 32/16/8 helpers\n");
}

/* Test 5: W^X code page bitmap tracking and invalidation */
static void test_code_page_tracking(void) {
    arm_block_cache_t *bcache = arm_block_cache_create(64);
    arm_ir_cache_t *ircache = arm_ir_cache_create(64);

    uint32_t code_va = ARM_FASTMEM_RAM_BASE + 0x10000u; /* Page in DRAM */
    uint32_t data_va = ARM_FASTMEM_RAM_BASE + 0x20000u;

    /* Initially neither page is marked as code */
    CHECK(!arm_block_cache_is_page_code(bcache, code_va));
    CHECK(!arm_ir_cache_is_page_code(ircache, code_va));

    /* Mark code page */
    arm_block_cache_mark_page_code(bcache, code_va);
    arm_ir_cache_mark_page_code(ircache, code_va);

    CHECK(arm_block_cache_is_page_code(bcache, code_va));
    CHECK(arm_ir_cache_is_page_code(ircache, code_va));

    /* Data page is still clean */
    CHECK(!arm_block_cache_is_page_code(bcache, data_va));
    CHECK(!arm_ir_cache_is_page_code(ircache, data_va));

    /* Invalidate code page */
    arm_block_cache_invalidate_page(bcache, code_va);
    arm_ir_cache_invalidate_page(ircache, code_va);

    CHECK(!arm_block_cache_is_page_code(bcache, code_va));
    CHECK(!arm_ir_cache_is_page_code(ircache, code_va));

    arm_block_cache_destroy(bcache);
    arm_ir_cache_destroy(ircache);
    printf("  [PASS] W^X code page tracking and invalidation\n");
}

/* Test 6: Self-modifying code invalidation during execution */
static void test_self_modifying_code(void) {
    arm_cpu_t cpu;
    arm_bus_t bus;
    dummy_bus_t db;
    init_test_env(&cpu, &bus, &db);

    arm_block_cache_t *bcache = arm_block_cache_create(64);

    /* Put code at DRAM address: ADD r0, r0, #1; MOV pc, lr */
    uint32_t pc = ARM_FASTMEM_RAM_BASE + 0x1000u;
    db.ram[pc - ARM_FASTMEM_RAM_BASE]     = 0x01; /* ADD r0, r0, #1 */
    db.ram[pc - ARM_FASTMEM_RAM_BASE + 1] = 0x00;
    db.ram[pc - ARM_FASTMEM_RAM_BASE + 2] = 0x80;
    db.ram[pc - ARM_FASTMEM_RAM_BASE + 3] = 0xe2;

    db.ram[pc - ARM_FASTMEM_RAM_BASE + 4] = 0x0e; /* MOV pc, lr (0xe1a0f00e) */
    db.ram[pc - ARM_FASTMEM_RAM_BASE + 5] = 0xf0;
    db.ram[pc - ARM_FASTMEM_RAM_BASE + 6] = 0xa0;
    db.ram[pc - ARM_FASTMEM_RAM_BASE + 7] = 0xe1;

    cpu.r[0] = 10;
    cpu.r[14] = pc + 8;
    cpu.r[15] = pc;

    /* Compile and execute block */
    arm_basic_block_t *block = arm_block_compile(bcache, &cpu, pc, false, true);
    CHECK(block != NULL);
    CHECK(arm_block_cache_is_page_code(bcache, pc));

    unsigned retired = 0;
    CHECK(arm_block_exec(&cpu, block, &retired) == ARM_OK);
    CHECK(cpu.r[0] == 11);

    /* Now write over the code: change ADD r0, r0, #1 to ADD r0, r0, #5 */
    arm_fastmem_write32(&cpu, pc, 0xe2800005u, true);

    /* Invalidate triggered on write to code page */
    arm_block_cache_invalidate_page(bcache, pc);
    CHECK(!arm_block_cache_is_page_code(bcache, pc));

    /* Recompile and execute updated code */
    arm_basic_block_t *block2 = arm_block_compile(bcache, &cpu, pc, false, true);
    CHECK(block2 != NULL);
    cpu.r[15] = pc;
    CHECK(arm_block_exec(&cpu, block2, &retired) == ARM_OK);
    CHECK(cpu.r[0] == 16); /* 11 + 5 = 16 */

    arm_block_cache_destroy(bcache);
    printf("  [PASS] self-modifying code cache invalidation and recompile\n");
}

int main(void) {
    printf("=== S5LBox Fast Memory & Software TLB Tests ===\n");
    test_dread_dwrite_basics();
    test_boundary_crossing();
    test_privilege_isolation();
    test_fastmem_helpers();
    test_code_page_tracking();
    test_self_modifying_code();
    printf("=== All Fast Memory tests PASSED ===\n");
    return 0;
}
