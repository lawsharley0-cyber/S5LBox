/*
 * S5LBox — Cached Interpreter & Basic Block Architecture.
 *
 * Implements:
 * - Phase 4: Decoded Instruction Cache (pre-decoded operations)
 * - Phase 5: Basic Block Discovery & Block Caching
 * - Phase 6: Direct Block Linking (Chaining)
 *
 * Designed specifically to maximize throughput on modern ARM64 iOS devices
 * without requiring JIT entitlements (NO-JIT mode).
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_ARM_BLOCK_H
#define S5LBOX_ARM_BLOCK_H

#include "arm.h"
#include "arm_mem.h"
#if defined(S5LBOX_JIT)
#include "jit.h"
#endif
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define ARM_BLOCK_MAX_INSNS 64

/* Opcode classifications for the cached interpreter */
typedef enum {
    /* Data processing (ALU) */
    ARM_OP_AND = 0,
    ARM_OP_EOR,
    ARM_OP_SUB,
    ARM_OP_RSB,
    ARM_OP_ADD,
    ARM_OP_ADC,
    ARM_OP_SBC,
    ARM_OP_RSC,
    ARM_OP_TST,
    ARM_OP_TEQ,
    ARM_OP_CMP,
    ARM_OP_CMN,
    ARM_OP_ORR,
    ARM_OP_MOV,
    ARM_OP_BIC,
    ARM_OP_MVN,

    /* Multiplies */
    ARM_OP_MUL,
    ARM_OP_MLA,
    ARM_OP_UMULL,
    ARM_OP_UMLAL,
    ARM_OP_SMULL,
    ARM_OP_SMLAL,

    /* Branches */
    ARM_OP_B,
    ARM_OP_BL,
    ARM_OP_BX,
    ARM_OP_BLX_IMM,
    ARM_OP_BLX_REG,

    /* Memory: Load / Store */
    ARM_OP_LDR_IMM,
    ARM_OP_LDR_REG,
    ARM_OP_LDRB_IMM,
    ARM_OP_LDRB_REG,
    ARM_OP_LDRH_IMM,
    ARM_OP_LDRH_REG,
    ARM_OP_LDRSB_IMM,
    ARM_OP_LDRSB_REG,
    ARM_OP_LDRSH_IMM,
    ARM_OP_LDRSH_REG,
    ARM_OP_STR_IMM,
    ARM_OP_STR_REG,
    ARM_OP_STRB_IMM,
    ARM_OP_STRB_REG,
    ARM_OP_STRH_IMM,
    ARM_OP_STRH_REG,

    /* Block transfers */
    ARM_OP_LDM,
    ARM_OP_STM,
    ARM_OP_PUSH,
    ARM_OP_POP,

    /* Byte / Halfword manipulations */
    ARM_OP_SXTB,
    ARM_OP_SXTH,
    ARM_OP_UXTB,
    ARM_OP_UXTH,
    ARM_OP_UXTB16,
    ARM_OP_REV,
    ARM_OP_REV16,
    ARM_OP_REVSH,
    ARM_OP_CLZ,

    /* System & Hints */
    ARM_OP_NOP,
    ARM_OP_SVC,
    ARM_OP_CPS,
    ARM_OP_CLREX,

    /* Fallback to reference arm_step() */
    ARM_OP_FALLBACK
} arm_decoded_op_t;

/* Shift types for barrel shifter */
typedef enum {
    ARM_SHIFT_LSL = 0,
    ARM_SHIFT_LSR = 1,
    ARM_SHIFT_ASR = 2,
    ARM_SHIFT_ROR = 3,
    ARM_SHIFT_RRX = 4
} arm_shift_type_t;

/* Pre-decoded instruction representation (Phase 4) */
typedef struct arm_decoded_insn {
    arm_decoded_op_t op;
    uint8_t          cond;          /* 4-bit condition code (0x0 - 0xE) */
    bool             cond_always;   /* true if cond == 0xE (AL) or 0xF (unconditional) */
    uint8_t          rd;            /* destination register (0-15) */
    uint8_t          rn;            /* base / operand 1 register (0-15) */
    uint8_t          rm;            /* operand 2 register (0-15) */
    uint8_t          rs;            /* shift amount register (0-15) */
    arm_shift_type_t shift_type;
    uint8_t          shift_imm;     /* 0-31 */
    bool             shift_by_reg;
    bool             is_imm;        /* true if operand 2 / offset is immediate */
    bool             sets_flags;    /* S-bit in ALU instructions */
    bool             is_thumb;      /* decoded from Thumb-1 instruction */
    bool             writes_pc;     /* true if Rd == 15 or branch */
    uint32_t         imm;           /* immediate value or offset */
    uint32_t         pc;            /* guest address of this instruction */
    uint32_t         next_pc;       /* sequential next PC (pc+4 or pc+2) */
    uint32_t         raw_insn;      /* original raw 32-bit or 16-bit word */
} arm_decoded_insn_t;

/* Block exit types (Phase 5) */
typedef enum {
    ARM_EXIT_SEQUENTIAL = 0,  /* block finished, sequential fallthrough */
    ARM_EXIT_BRANCH_DIRECT,   /* unconditional direct branch */
    ARM_EXIT_BRANCH_COND,     /* conditional branch */
    ARM_EXIT_INDIRECT,        /* dynamic target (BX, BLX reg, MOV pc, POP {pc}) */
    ARM_EXIT_SVC,             /* supervisor call */
    ARM_EXIT_FALLBACK,        /* unhandled opcode -> execute via arm_step */
    ARM_EXIT_PAGE_BOUNDARY    /* reached end of 1KB fetch block */
} arm_block_exit_t;

struct arm_basic_block;
struct arm_block_cache;

/* Basic Block Representation (Phase 5 & 6) */
typedef struct arm_basic_block {
    uint32_t start_va;         /* guest virtual address */
    uint32_t start_pa;         /* guest physical address */
    bool     thumb;            /* CPSR.T at entry */
    bool     priv;             /* privileged mode at entry */
    bool     valid;            /* whether this block is valid */
    uint32_t generation;       /* TLB/cache generation tag */

    unsigned insn_count;       /* number of decoded instructions (1 to 64) */
    arm_decoded_insn_t insns[ARM_BLOCK_MAX_INSNS];

    arm_block_exit_t exit_type;
    uint32_t         branch_target;     /* direct branch target VA */
    uint32_t         fallthrough_target;/* fallthrough VA */

    /* Phase 6: Direct Block Linking pointers */
    struct arm_basic_block *link_target;
    struct arm_basic_block *link_fallthrough;

    /* Owning block cache (Phase 10: W^X invalidation) */
    struct arm_block_cache *cache;

#if defined(S5LBOX_JIT)
    jit_block_t jit_blk;
    bool        jit_compiled;
#endif

    uint64_t exec_count;       /* profiling execution count */
} arm_basic_block_t;

#define ARM_BLOCK_CACHE_SIZE 4096u /* Hash table slots */
#define ARM_BLOCK_CACHE_MASK (ARM_BLOCK_CACHE_SIZE - 1u)
#define ARM_BLOCK_DEFAULT_POOL_SIZE 4096u

/* Basic Block Cache (Hash Table) */
typedef struct arm_block_cache {
    arm_basic_block_t *table[ARM_BLOCK_CACHE_SIZE];
    arm_basic_block_t *pool;
    uint32_t           pool_capacity;
    uint32_t           pool_used;
    uint32_t           generation;
    uint64_t           lookups;
    uint64_t           hits;
    uint64_t           misses;
    uint64_t           invalidations;
    uint64_t           links_taken;
    uint32_t           code_pages[ARM_FASTMEM_BITMAP_WORDS];
#if defined(S5LBOX_JIT)
    jit_buf_t          jit_arena;
    bool               jit_arena_valid;
#endif
} arm_block_cache_t;

/* Public API */
arm_block_cache_t *arm_block_cache_create(uint32_t pool_capacity);
void               arm_block_cache_destroy(arm_block_cache_t *cache);
void               arm_block_cache_reset(arm_block_cache_t *cache);
void               arm_block_cache_invalidate(arm_block_cache_t *cache);
void               arm_block_cache_invalidate_page(arm_block_cache_t *cache, uint32_t page_va);
void               arm_block_cache_mark_page_code(arm_block_cache_t *cache, uint32_t page_va);
bool               arm_block_cache_is_page_code(const arm_block_cache_t *cache, uint32_t page_va);
void               arm_block_cache_clear_page_code(arm_block_cache_t *cache, uint32_t page_va);

/* Lookup or build a basic block */
arm_basic_block_t *arm_block_cache_lookup(arm_block_cache_t *cache,
                                          uint32_t va, bool thumb, bool priv);

arm_basic_block_t *arm_block_compile(arm_block_cache_t *cache,
                                     arm_cpu_t *cpu,
                                     uint32_t start_va,
                                     bool thumb,
                                     bool priv);

/* Phase 6: Direct Block Linking */
void arm_block_link(arm_basic_block_t *from, arm_basic_block_t *to_target, arm_basic_block_t *to_fallthrough);
void arm_block_unlink_all(arm_block_cache_t *cache);

/* Phase 4 & 5: Single Instruction Decoder & Block Execution */
bool arm_decode_instruction(arm_cpu_t *cpu, uint32_t pc, uint32_t raw, bool thumb, arm_decoded_insn_t *out);

/* Execute a compiled basic block */
arm_status_t arm_block_exec(arm_cpu_t *cpu, arm_basic_block_t *block, unsigned *retired_out);

#endif /* S5LBOX_ARM_BLOCK_H */
