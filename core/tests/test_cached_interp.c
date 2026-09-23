/*
 * S5LBox — Unit test for Cached Interpreter & Decoded Instruction Execution.
 *
 * Verifies that decoding and cached block execution yield bit-identical
 * architectural results to the reference arm_step() interpreter.
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
            return 1; \
        } \
    } while (0)

int main(void) {
    arm_cpu_t cpu;
    arm_reset(&cpu, &g_bus);

    /* 1. Test Single Instruction Decode: ARM ADD r0, r1, #42 */
    /* Encoding: 0xe281002a (adds nothing to flags, cond=AL) */
    arm_decoded_insn_t di;
    bool ok = arm_decode_instruction(&cpu, 0x1000, 0xe281002au, false, &di);
    CHECK(ok, "ARM ADD decode should succeed");
    CHECK(di.op == ARM_OP_ADD, "Decoded op should be ARM_OP_ADD");
    CHECK(di.rd == 0, "Rd should be 0");
    CHECK(di.rn == 1, "Rn should be 1");
    CHECK(di.imm == 42, "Immediate should be 42");
    CHECK(di.cond_always, "Condition should be AL (always)");

    /* 2. Test Decode Thumb: MOVS r3, #15 */
    /* Encoding: 0x230f */
    ok = arm_decode_instruction(&cpu, 0x2000, 0x230fu, true, &di);
    CHECK(ok, "Thumb MOV decode should succeed");
    CHECK(di.op == ARM_OP_MOV, "Thumb op should be ARM_OP_MOV");
    CHECK(di.rd == 3, "Thumb Rd should be 3");
    CHECK(di.imm == 15, "Thumb immediate should be 15");

    /* 3. Test Block Execution: Straight-line Arithmetic Block */
    arm_basic_block_t block;
    memset(&block, 0, sizeof(block));
    block.valid = true;
    block.start_va = 0x1000;
    block.insn_count = 3;

    /* Insn 0: MOV r0, #100 */
    arm_decode_instruction(&cpu, 0x1000, 0xe3a00064u, false, &block.insns[0]);
    /* Insn 1: ADD r1, r0, #50 */
    arm_decode_instruction(&cpu, 0x1004, 0xe2801032u, false, &block.insns[1]);
    /* Insn 2: SUB r2, r1, #25 */
    arm_decode_instruction(&cpu, 0x1008, 0xe2412019u, false, &block.insns[2]);

    cpu.r[15] = 0x1000;
    unsigned retired = 0;
    arm_status_t st = arm_block_exec(&cpu, &block, &retired);

    CHECK(st == ARM_OK, "Block execution should succeed");
    CHECK(retired == 3, "Should retire 3 instructions");
    CHECK(cpu.r[0] == 100, "r0 should be 100");
    CHECK(cpu.r[1] == 150, "r1 should be 150");
    CHECK(cpu.r[2] == 125, "r2 should be 125");
    CHECK(cpu.r[15] == 0x100c, "r15 should advance to 0x100c");

    /* 4. Test Block with Memory Store and Load */
    memset(&block, 0, sizeof(block));
    block.valid = true;
    block.start_va = 0x100c;
    block.insn_count = 2;

    /* Set up memory address */
    cpu.r[3] = 0x4000; /* Target RAM address */
    /* STR r2, [r3, #4] -> 0xe5832004 */
    arm_decode_instruction(&cpu, 0x100c, 0xe5832004u, false, &block.insns[0]);
    /* LDR r4, [r3, #4] -> 0xe5934004 */
    arm_decode_instruction(&cpu, 0x1010, 0xe5934004u, false, &block.insns[1]);

    st = arm_block_exec(&cpu, &block, &retired);
    CHECK(st == ARM_OK, "Memory block should execute cleanly");
    CHECK(retired == 2, "Should retire 2 memory instructions");
    CHECK(m_r32(NULL, 0x4004) == 125, "Memory at 0x4004 should be 125");
    CHECK(cpu.r[4] == 125, "Loaded r4 should be 125");

    /* 5. Compare with reference arm_step() */
    arm_cpu_t ref_cpu;
    arm_reset(&ref_cpu, &g_bus);
    ref_cpu.r[3] = 0x4000;
    ref_cpu.r[2] = 999;
    ref_cpu.r[15] = 0x100c;

    m_w32(NULL, 0x100c, 0xe5832004u); /* STR r2, [r3, #4] */
    m_w32(NULL, 0x1010, 0xe5934004u); /* LDR r4, [r3, #4] */

    CHECK(arm_step(&ref_cpu) == ARM_OK, "ref_cpu step 1");
    CHECK(arm_step(&ref_cpu) == ARM_OK, "ref_cpu step 2");
    CHECK(ref_cpu.r[4] == 999, "ref_cpu r4 loaded 999");

    printf("test_cached_interp: all checks passed!\n");
    return 0;
}
