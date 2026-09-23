/*
 * S5LBox — Micro-Op Intermediate Representation (IR) Unit Tests.
 *
 * Verifies:
 * 1. IR Lifting: Decomposing compound ARM/Thumb instructions into atomic micro-ops.
 * 2. IR Optimization Passes: Constant folding, dead temporary elimination, flag pruning.
 * 3. Execution Fidelity: Differential testing between arm_ir_exec and reference arm_step.
 * 4. Cache & Invalidation: Compilation, cache hits/misses, and page invalidation.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "arm.h"
#include "soc.h"
#include "arm_block.h"
#include "arm_ir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "CHECK FAILED: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

typedef struct {
    uint8_t ram[65536];
} dummy_bus_t;

static uint32_t bus_r32(void *ctx, uint32_t addr) {
    dummy_bus_t *b = (dummy_bus_t *)ctx;
    addr &= 0xffffu;
    return (uint32_t)b->ram[addr] | ((uint32_t)b->ram[addr+1] << 8) |
           ((uint32_t)b->ram[addr+2] << 16) | ((uint32_t)b->ram[addr+3] << 24);
}

static uint16_t bus_r16(void *ctx, uint32_t addr) {
    dummy_bus_t *b = (dummy_bus_t *)ctx;
    addr &= 0xffffu;
    return (uint16_t)b->ram[addr] | ((uint16_t)b->ram[addr+1] << 8);
}

static uint8_t bus_r8(void *ctx, uint32_t addr) {
    dummy_bus_t *b = (dummy_bus_t *)ctx;
    addr &= 0xffffu;
    return b->ram[addr];
}

static void bus_w32(void *ctx, uint32_t addr, uint32_t val) {
    dummy_bus_t *b = (dummy_bus_t *)ctx;
    addr &= 0xffffu;
    b->ram[addr]   = (uint8_t)val;
    b->ram[addr+1] = (uint8_t)(val >> 8);
    b->ram[addr+2] = (uint8_t)(val >> 16);
    b->ram[addr+3] = (uint8_t)(val >> 24);
}

static void bus_w16(void *ctx, uint32_t addr, uint16_t val) {
    dummy_bus_t *b = (dummy_bus_t *)ctx;
    addr &= 0xffffu;
    b->ram[addr]   = (uint8_t)val;
    b->ram[addr+1] = (uint8_t)(val >> 8);
}

static void bus_w8(void *ctx, uint32_t addr, uint8_t val) {
    dummy_bus_t *b = (dummy_bus_t *)ctx;
    addr &= 0xffffu;
    b->ram[addr] = val;
}

static uint8_t *bus_host_ram(void *ctx, uint32_t addr, uint32_t size) {
    dummy_bus_t *b = (dummy_bus_t *)ctx;
    (void)size;
    addr &= 0xffffu;
    return &b->ram[addr];
}

static void init_test_cpu(arm_cpu_t *cpu, arm_bus_t *bus, dummy_bus_t *db) {
    memset(cpu, 0, sizeof(*cpu));
    memset(bus, 0, sizeof(*bus));
    memset(db, 0, sizeof(*db));

    bus->ctx = db;
    bus->read32 = bus_r32;
    bus->read16 = bus_r16;
    bus->read8  = bus_r8;
    bus->write32 = bus_w32;
    bus->write16 = bus_w16;
    bus->write8  = bus_w8;
    bus->host_ram = bus_host_ram;

    arm_reset(cpu, bus);
    cpu->cpsr = ARM_MODE_USR; /* User mode */
}

/* Test 1: Decomposing shifted-register operands in lifter */
static void test_ir_lifting(void) {
    arm_decoded_insn_t di;
    memset(&di, 0, sizeof(di));

    /* Case A: Simple immediate MOV R0, #42 */
    di.op = ARM_OP_MOV;
    di.cond = 0xe;
    di.cond_always = true;
    di.is_imm = true;
    di.rd = 0;
    di.imm = 42;
    di.pc = 0x1000;
    di.next_pc = 0x1004;

    arm_ir_insn_t ir_buf[8];
    unsigned written = 0;
    CHECK(arm_ir_lift_instruction(&di, ir_buf, 8, &written));
    CHECK(written == 1);
    CHECK(ir_buf[0].op == IR_LI);
    CHECK(ir_buf[0].dst == VREG_R0);
    CHECK(ir_buf[0].imm == 42);

    /* Case B: Compound shifted operand ADD R1, R2, R3, LSL #2 */
    memset(&di, 0, sizeof(di));
    di.op = ARM_OP_ADD;
    di.cond = 0xe;
    di.cond_always = true;
    di.is_imm = false;
    di.rd = 1;
    di.rn = 2;
    di.rm = 3;
    di.shift_type = ARM_SHIFT_LSL;
    di.shift_imm = 2;
    di.shift_by_reg = false;
    di.pc = 0x1004;
    di.next_pc = 0x1008;

    CHECK(arm_ir_lift_instruction(&di, ir_buf, 8, &written));
    CHECK(written == 2);
    /* First micro-op: TMP0 = R3 << 2 */
    CHECK(ir_buf[0].op == IR_LSL);
    CHECK(ir_buf[0].dst == VREG_TMP0);
    CHECK(ir_buf[0].src1 == VREG_R3);
    CHECK(ir_buf[0].imm == 2);
    /* Second micro-op: R1 = R2 + TMP0 */
    CHECK(ir_buf[1].op == IR_ADD);
    CHECK(ir_buf[1].dst == VREG_R1);
    CHECK(ir_buf[1].src1 == VREG_R2);
    CHECK(ir_buf[1].src2 == VREG_TMP0);

    /* Case C: ADDS R1, R2, #10 (ALU with flag updates) */
    memset(&di, 0, sizeof(di));
    di.op = ARM_OP_ADD;
    di.cond = 0xe;
    di.cond_always = true;
    di.sets_flags = true;
    di.is_imm = true;
    di.rd = 1;
    di.rn = 2;
    di.imm = 10;
    di.pc = 0x1008;
    di.next_pc = 0x100c;

    CHECK(arm_ir_lift_instruction(&di, ir_buf, 8, &written));
    CHECK(written == 2);
    CHECK(ir_buf[0].op == IR_ADD);
    CHECK(ir_buf[1].op == IR_UPDATE_NZCV_ADD);
}

/* Test 2: Optimization Passes */
static void test_ir_optimization(void) {
    arm_ir_block_t block;
    memset(&block, 0, sizeof(block));

    /* Instruction 0: LI TMP0, #5 */
    block.insns[0].op = IR_LI;
    block.insns[0].cond_always = true;
    block.insns[0].dst = VREG_TMP0;
    block.insns[0].imm = 5;
    block.insns[0].guest_pc = 0x1000;

    /* Instruction 1: ADD R0, R1, TMP0 (Candidate for constant propagation) */
    block.insns[1].op = IR_ADD;
    block.insns[1].cond_always = true;
    block.insns[1].dst = VREG_R0;
    block.insns[1].src1 = VREG_R1;
    block.insns[1].src2 = VREG_TMP0;
    block.insns[1].guest_pc = 0x1004;

    /* Instruction 2: UPDATE_NZ R0 (Candidate for pruning if followed by another update) */
    block.insns[2].op = IR_UPDATE_NZ;
    block.insns[2].cond_always = true;
    block.insns[2].src1 = VREG_R0;
    block.insns[2].guest_pc = 0x1004;

    /* Instruction 3: LI TMP1, #100 (Dead temporary: never read) */
    block.insns[3].op = IR_LI;
    block.insns[3].cond_always = true;
    block.insns[3].dst = VREG_TMP1;
    block.insns[3].imm = 100;
    block.insns[3].guest_pc = 0x1008;

    /* Instruction 4: UPDATE_NZCV_ADD R0, R1, #5 (Second flag update) */
    block.insns[4].op = IR_UPDATE_NZCV_ADD;
    block.insns[4].cond_always = true;
    block.insns[4].dst = VREG_R0;
    block.insns[4].src1 = VREG_R1;
    block.insns[4].src2 = VREG_CONST;
    block.insns[4].imm = 5;
    block.insns[4].guest_pc = 0x100c;

    block.insn_count = 5;

    /* Constant folding pass */
    unsigned folded = arm_ir_opt_constant_folding(&block);
    CHECK(folded == 1);
    CHECK(block.insns[1].src2 == VREG_CONST);
    CHECK(block.insns[1].imm == 5);

    /* Redundant flags pruning pass */
    unsigned pruned = arm_ir_opt_redundant_flags(&block);
    CHECK(pruned == 1);
    CHECK(block.insns[2].dead == true);

    /* Dead temporary elimination pass */
    unsigned dce = arm_ir_opt_dead_code(&block);
    CHECK(dce >= 1); /* TMP1 (and TMP0 if no other reads) eliminated */

    /* Run full optimizer pipeline */
    arm_ir_optimize_block(&block);
    /* Block should now be compacted */
    CHECK(block.insn_count < 5);
    for (unsigned i = 0; i < block.insn_count; i++) {
        CHECK(!block.insns[i].dead);
    }
}

/* Test 3: Differential Execution Testing vs Reference Interpreter */
static void test_ir_differential_exec(void) {
    arm_cpu_t cpu_interp, cpu_ir;
    arm_bus_t bus_interp, bus_ir;
    dummy_bus_t db_interp, db_ir;

    init_test_cpu(&cpu_interp, &bus_interp, &db_interp);
    init_test_cpu(&cpu_ir, &bus_ir, &db_ir);

    /* ARM instructions:
     * 0x1000: MOV R0, #15        (0xe3a0000f)
     * 0x1004: MOV R1, #30        (0xe3a0101e)
     * 0x1008: ADD R2, R0, R1     (0xe0802001)
     * 0x100c: SUBS R3, R2, #5    (0xe2523005)
     * 0x1010: B   0x1020         (0xea000002)
     */
    static const uint32_t insns[] = {
        0xe3a0000fu, /* mov r0, #15 */
        0xe3a0101eu, /* mov r1, #30 */
        0xe0802001u, /* add r2, r0, r1 */
        0xe2523005u, /* subs r3, r2, #5 */
        0xea000002u  /* b 0x1020 */
    };

    for (unsigned i = 0; i < 5; i++) {
        bus_w32(&db_interp, 0x1000 + i * 4, insns[i]);
        bus_w32(&db_ir,     0x1000 + i * 4, insns[i]);
    }

    cpu_interp.r[15] = 0x1000;
    cpu_ir.r[15]     = 0x1000;

    /* Step 5 instructions on reference interpreter */
    for (int i = 0; i < 5; i++) {
        CHECK(arm_step(&cpu_interp) == ARM_OK);
    }

    /* Compile and execute IR block */
    arm_ir_cache_t *cache = arm_ir_cache_create(64);
    CHECK(cache != NULL);

    arm_ir_block_t *block = arm_ir_compile_block(cache, &cpu_ir, 0x1000, false, false);
    CHECK(block != NULL);
    CHECK(block->valid);
    CHECK(block->insn_count > 0);

    unsigned retired = 0;
    arm_status_t st = arm_ir_exec(&cpu_ir, block, &retired);
    CHECK(st == ARM_OK);
    CHECK(retired == 5);

    /* Compare architectural registers */
    for (int r = 0; r < 16; r++) {
        CHECK(cpu_interp.r[r] == cpu_ir.r[r]);
    }
    CHECK(cpu_interp.cpsr == cpu_ir.cpsr);

    arm_ir_cache_destroy(cache);
}

/* Test 4: Cache Lookup and Invalidation */
static void test_ir_cache(void) {
    arm_cpu_t cpu;
    arm_bus_t bus;
    dummy_bus_t db;
    init_test_cpu(&cpu, &bus, &db);

    bus_w32(&db, 0x2000, 0xe3a00042u); /* mov r0, #42 */
    cpu.r[15] = 0x2000;

    arm_ir_cache_t *cache = arm_ir_cache_create(32);
    CHECK(cache != NULL);

    /* Miss on first lookup */
    arm_ir_block_t *b = arm_ir_cache_lookup(cache, 0x2000, false, false);
    CHECK(b == NULL);
    CHECK(cache->misses == 1);

    /* Compile */
    b = arm_ir_compile_block(cache, &cpu, 0x2000, false, false);
    CHECK(b != NULL);

    /* Hit on second lookup */
    arm_ir_block_t *hit = arm_ir_cache_lookup(cache, 0x2000, false, false);
    CHECK(hit == b);
    CHECK(cache->hits == 1);

    /* Invalidate page */
    arm_ir_cache_invalidate_page(cache, 0x2000);
    CHECK(arm_ir_cache_lookup(cache, 0x2000, false, false) == NULL);

    arm_ir_cache_destroy(cache);
}

/* Test 5: Machine Loop Integration (s5l8900_run with pluggable CPU backend) */
static void test_machine_backend(void) {
    s5l8900_t m;
    memset(&m, 0, sizeof(m));
    CHECK(s5l8900_init(&m, 0x08000000u, 16u * 1024u * 1024u));

    /* Load code:
     * 0x08010000: mov r0, #100
     * 0x08010004: add r1, r0, #50
     * 0x08010008: b 0x08010010
     */
    static const uint32_t prog[] = {
        0xe3a00064u, /* mov r0, #100 */
        0xe2801032u, /* add r1, r0, #50 */
        0xea000000u  /* b 0x08010010 */
    };
    for (unsigned i = 0; i < 3; i++) {
        m.bus.write32(m.bus.ctx, 0x08010000u + i * 4u, prog[i]);
    }
    m.cpu.r[15] = 0x08010000u;
    arm_set_mode(&m.cpu, ARM_MODE_USR);

    /* Test A: Run with CACHED_BLOCK backend */
    s5l8900_set_cpu_backend(&m, S5L8900_CPU_BACKEND_CACHED_BLOCK);
    CHECK(s5l8900_get_cpu_backend(&m) == S5L8900_CPU_BACKEND_CACHED_BLOCK);
    arm_status_t st = ARM_OK;
    unsigned ran = s5l8900_run(&m, 3, &st);
    CHECK(ran == 3);
    CHECK(st == ARM_OK);
    CHECK(m.cpu.r[0] == 100);
    CHECK(m.cpu.r[1] == 150);
    CHECK(m.cpu.r[15] == 0x08010010u);

    /* Test B: Run with IR_OPTIMIZED backend */
    m.cpu.r[15] = 0x08010000u;
    m.cpu.r[0] = 0;
    m.cpu.r[1] = 0;
    s5l8900_set_cpu_backend(&m, S5L8900_CPU_BACKEND_IR_OPTIMIZED);
    CHECK(s5l8900_get_cpu_backend(&m) == S5L8900_CPU_BACKEND_IR_OPTIMIZED);
    ran = s5l8900_run(&m, 3, &st);
    CHECK(ran == 3);
    CHECK(st == ARM_OK);
    CHECK(m.cpu.r[0] == 100);
    CHECK(m.cpu.r[1] == 150);
    CHECK(m.cpu.r[15] == 0x08010010u);

    s5l8900_free(&m);
}

int main(void) {
    printf("Running test_ir...\n");
    test_ir_lifting();
    test_ir_optimization();
    test_ir_differential_exec();
    test_ir_cache();
    test_machine_backend();
    printf("test_ir: all checks passed!\n");
    return 0;
}
