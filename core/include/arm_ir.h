/*
 * S5LBox — Micro-Op Intermediate Representation (IR) Architecture.
 *
 * Implements:
 * - Phase 7: ARM-to-ARM Fast Paths & IR Definition
 * - Phase 8: Micro-Op IR (Decomposing compound ARM/Thumb instructions)
 * - Phase 9: Conservative IR Optimizations
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_ARM_IR_H
#define S5LBOX_ARM_IR_H

#include "arm.h"
#include "arm_block.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define ARM_IR_MAX_INSNS 256
#define ARM_IR_CACHE_SIZE 4096u
#define ARM_IR_CACHE_MASK (ARM_IR_CACHE_SIZE - 1u)
#define ARM_IR_DEFAULT_POOL_SIZE 4096u

/* Virtual Registers */
typedef enum {
    /* Architectural Guest GPRs (0 - 15) */
    VREG_R0 = 0,
    VREG_R1,  VREG_R2,  VREG_R3,  VREG_R4,
    VREG_R5,  VREG_R6,  VREG_R7,  VREG_R8,
    VREG_R9,  VREG_R10, VREG_R11, VREG_R12,
    VREG_R13, /* SP */
    VREG_R14, /* LR */
    VREG_R15, /* PC */

    /* Special Registers */
    VREG_CPSR,

    /* Virtual Temporaries for Compound Instruction Decomposition */
    VREG_TMP0,
    VREG_TMP1,
    VREG_TMP2,
    VREG_TMP3,
    VREG_TMP4,
    VREG_TMP5,
    VREG_TMP6,
    VREG_TMP7,

    /* Virtual Register Sentinel */
    VREG_CONST, /* Uses insn->imm instead of a register */
    VREG_NONE   /* Unused operand */
} arm_ir_vreg_t;

/* Micro-Operation Opcodes */
typedef enum {
    /* No operation */
    IR_NOP = 0,

    /* Register Data Movement */
    IR_MOV,             /* dst = src1 */
    IR_LI,              /* dst = imm */

    /* Arithmetic 32-bit */
    IR_ADD,             /* dst = src1 + src2 */
    IR_SUB,             /* dst = src1 - src2 */
    IR_RSB,             /* dst = src2 - src1 */
    IR_ADC,             /* dst = src1 + src2 + carry_in */
    IR_SBC,             /* dst = src1 - src2 - (!carry_in) */
    IR_RSC,             /* dst = src2 - src1 - (!carry_in) */
    IR_MUL,             /* dst = src1 * src2 */
    IR_MLA,             /* dst = src1 * src2 + src3 (or imm) */

    /* Bitwise Logic 32-bit */
    IR_AND,             /* dst = src1 & src2 */
    IR_ORR,             /* dst = src1 | src2 */
    IR_EOR,             /* dst = src1 ^ src2 */
    IR_BIC,             /* dst = src1 & ~src2 */
    IR_MVN,             /* dst = ~src1 */

    /* Shifts / Rotates */
    IR_LSL,             /* dst = src1 << src2 (or imm) */
    IR_LSR,             /* dst = (uint32)src1 >> src2 */
    IR_ASR,             /* dst = (int32)src1 >> src2 */
    IR_ROR,             /* dst = ror(src1, src2) */

    /* Condition Flags Management */
    IR_UPDATE_NZ,       /* Update CPSR N, Z from src1 */
    IR_UPDATE_NZCV_ADD, /* Update CPSR N, Z, C, V from addition (src1, src2, dst) */
    IR_UPDATE_NZCV_SUB, /* Update CPSR N, Z, C, V from subtraction (src1, src2, dst) */

    /* Memory Access */
    IR_LOAD32,          /* dst = read32(src1 + imm) */
    IR_LOAD16,          /* dst = read16(src1 + imm) */
    IR_LOAD8,           /* dst = read8(src1 + imm) */
    IR_STORE32,         /* write32(src1 + imm, src2) */
    IR_STORE16,         /* write16(src1 + imm, src2) */
    IR_STORE8,          /* write8(src1 + imm, src2) */

    /* Control Flow */
    IR_BRANCH,          /* PC = src1 or imm */
    IR_BRANCH_COND,     /* if (cond) PC = target else PC = fallthrough */

    /* State Sync & Fallback */
    IR_SYNC_REG,        /* Commit temporary to canonical guest register */
    IR_FALLBACK         /* Bail out to reference arm_step() */
} arm_ir_op_t;

/* Single Micro-Operation Instruction */
typedef struct arm_ir_insn {
    arm_ir_op_t   op;
    uint8_t       cond;          /* 4-bit condition (0x0..0xE) */
    bool          cond_always;   /* True if 0xE (AL) or 0xF (unconditional) */
    arm_ir_vreg_t dst;           /* Destination virtual register */
    arm_ir_vreg_t src1;          /* Source 1 virtual register */
    arm_ir_vreg_t src2;          /* Source 2 virtual register */
    uint32_t      imm;           /* Immediate operand / offset / target address */
    uint32_t      guest_pc;      /* Guest instruction address */
    bool          dead;          /* Marked for removal by optimizer */
} arm_ir_insn_t;

struct arm_ir_cache;

/* Optimized IR Basic Block */
typedef struct arm_ir_block {
    uint32_t start_va;           /* Guest virtual address */
    uint32_t start_pa;           /* Guest physical address */
    bool     thumb;              /* CPSR.T at entry */
    bool     priv;               /* Privileged mode at entry */
    bool     valid;              /* Valid block tag */
    uint32_t generation;         /* Invalidation generation */

    unsigned insn_count;         /* Number of micro-ops */
    arm_ir_insn_t insns[ARM_IR_MAX_INSNS];

    arm_block_exit_t exit_type;
    uint32_t         branch_target;
    uint32_t         fallthrough_target;

    /* Owning IR cache */
    struct arm_ir_cache *cache;

    uint64_t exec_count;         /* Profiling executions */
} arm_ir_block_t;

/* IR Block Cache */
typedef struct arm_ir_cache {
    arm_ir_block_t *table[ARM_IR_CACHE_SIZE];
    arm_ir_block_t *pool;
    uint32_t        pool_capacity;
    uint32_t        pool_used;
    uint32_t        generation;
    uint64_t        lookups;
    uint64_t        hits;
    uint64_t        misses;
    uint64_t        invalidations;
    uint32_t        code_pages[ARM_FASTMEM_BITMAP_WORDS];
} arm_ir_cache_t;

/* Public API */

/* Lifecycle */
arm_ir_cache_t *arm_ir_cache_create(uint32_t pool_capacity);
void            arm_ir_cache_destroy(arm_ir_cache_t *cache);
void            arm_ir_cache_reset(arm_ir_cache_t *cache);
void            arm_ir_cache_invalidate(arm_ir_cache_t *cache);
void            arm_ir_cache_invalidate_page(arm_ir_cache_t *cache, uint32_t page_va);
void            arm_ir_cache_mark_page_code(arm_ir_cache_t *cache, uint32_t page_va);
bool            arm_ir_cache_is_page_code(const arm_ir_cache_t *cache, uint32_t page_va);
void            arm_ir_cache_clear_page_code(arm_ir_cache_t *cache, uint32_t page_va);

/* Lookup and Compilation */
arm_ir_block_t *arm_ir_cache_lookup(arm_ir_cache_t *cache, uint32_t va, bool thumb, bool priv);
arm_ir_block_t *arm_ir_compile_block(arm_ir_cache_t *cache, arm_cpu_t *cpu,
                                     uint32_t start_va, bool thumb, bool priv);

/* Phase 8: IR Lifting */
bool arm_ir_lift_instruction(const arm_decoded_insn_t *di,
                             arm_ir_insn_t *out_insns,
                             unsigned max_out,
                             unsigned *written_out);

/* Phase 9: IR Optimization Passes */
unsigned arm_ir_opt_constant_folding(arm_ir_block_t *block);
unsigned arm_ir_opt_dead_code(arm_ir_block_t *block);
unsigned arm_ir_opt_redundant_flags(arm_ir_block_t *block);
void     arm_ir_optimize_block(arm_ir_block_t *block);

/* Execution */
arm_status_t arm_ir_exec(arm_cpu_t *cpu, arm_ir_block_t *block, unsigned *retired_out);

#endif /* S5LBOX_ARM_IR_H */
