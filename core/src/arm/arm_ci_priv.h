/*
 * S5LBox — cached interpreter internals shared by arm_ci.c (cache, run loop,
 * executor) and arm_ci_decode.c (decoder). Not installed.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_ARM_CI_PRIV_H
#define S5LBOX_ARM_CI_PRIV_H

#include "arm_ci.h"
#include <stdint.h>

/* ------------------------------------------------------------ op records ---
 *
 * One record per guest instruction (a Thumb BL pair is two), 16 bytes, stored
 * contiguously per block in a pooled arena: no allocation per instruction.
 * The guest PC of a record is implied by its index (block va + i*4 or i*2),
 * so it is not stored. `raw` keeps the instruction word for reference
 * fallback and for verify mode.
 */
typedef struct ci_op {
    uint8_t  kind;   /* CI_K_* below                                        */
    uint8_t  cond;   /* ARM condition 0..14; 14 (AL) means unconditional    */
    uint8_t  rd;
    uint8_t  rn;
    uint8_t  rm;
    uint8_t  rs;
    uint8_t  sh;     /* shift type CI_SH_* | CI_SH_SUB for memory offsets;
                        for DP immediates: the constant carry-out            */
    uint8_t  sa;     /* shift amount 1..32; DP immediates: 1 = carry defined */
    uint32_t imm;    /* immediate / signed offset / target / constant        */
    uint32_t raw;    /* the instruction as fetched (Thumb: low 16 bits)      */
} ci_op_t;

#define CI_COND_AL 14u

/* Immediate-shift types, normalised by the decoder: LSL 1..31, LSR 1..32,
 * ASR 1..32, ROR 1..31, RRX. "LSL #0" is the plain-register form. For the
 * register-shift form only 0..3 occur. */
enum { CI_SH_LSL = 0, CI_SH_LSR = 1, CI_SH_ASR = 2, CI_SH_ROR = 3, CI_SH_RRX = 4 };
#define CI_SH_SUB 0x80u  /* memory register offset is subtracted (U == 0) */

/* ------------------------------------------------------------- op kinds ---
 *
 * Data processing: CI_K_DP + ((opcode * 4 + form) * 2 + S). Opcode is the
 * ARM field (AND=0 ... MVN=15). TST/TEQ/CMP/CMN exist only with S.
 * Memory: CI_K_MEM + ((access * 2 + source) * 3 + mode).
 */
enum { CI_F_IMM = 0, CI_F_REG = 1, CI_F_SHI = 2, CI_F_SHR = 3 };
enum { CI_M_OFF = 0, CI_M_PRE = 1, CI_M_POST = 2 };
enum { CI_S_IMM = 0, CI_S_REG = 1 };
enum { CI_A_LDR = 0, CI_A_LDRB, CI_A_STR, CI_A_STRB,
       CI_A_LDRH, CI_A_LDRSB, CI_A_LDRSH, CI_A_STRH, CI_A_COUNT };

enum {
    CI_K_REF = 0,      /* execute through the reference semantics            */
    CI_K_NOP,          /* PLD and other architectural no-ops                 */
    CI_K_CLREX,
    CI_K_MRS_CPSR,
    CI_K_MUL, CI_K_MULS, CI_K_MLA, CI_K_MLAS,
    CI_K_UMULL, CI_K_UMLAL, CI_K_SMULL, CI_K_SMLAL,
    CI_K_CLZ,
    CI_K_SXTB, CI_K_SXTH, CI_K_UXTB, CI_K_UXTH,
    CI_K_REV, CI_K_REV16, CI_K_REVSH,
    CI_K_LDR_LIT,      /* LDR rd, [pc, #imm]: imm is the absolute address    */
    /* Block transfers (LDM/STM, Thumb PUSH/POP/LDMIA/STMIA) with S == 0:
     * imm = register list, rn = base, rm = number of registers,
     * sa = (int8) offset of the lowest address from the base,
     * rs = (int8) writeback delta, 0 for none. */
    CI_K_LDM,
    CI_K_LDM_PC,       /* list includes r15: interworking load, ends block   */
    CI_K_STM,          /* never r15 in the list                              */
    /* CP15 c13 thread-ID registers: rd = core register, sa = opc2 (2..4),
     * rm = CRm. User-mode access rules are checked at run time. */
    CI_K_MRC_TID,
    CI_K_MCR_TID,
    /* VFP (cp10/cp11, ARM state): the reference's VFP unit called directly,
     * without the decode tree. Runs through the reference tail in the
     * executor; sa carries ARM_CI_REF_VFP for the statistics. */
    CI_K_VFP,
    /* Control flow. All end the block. */
    /* LDR pc (word, not LDRT): interworking load. rn = base, rs = mode
     * (CI_M_*) | 4 for a register offset (rm, sh, sa as CI_S_REG) | 8 when
     * the base is the PC (address of the instruction + 8, no writeback);
     * imm = signed immediate offset. */
    CI_K_LDR_PC,
    CI_K_JMP,          /* target = rm + imm, bit 0 (Thumb) or bits 1:0 clear */
    CI_K_B,            /* imm = target                                       */
    CI_K_BL,           /* ARM: LR = pc + 4                                   */
    CI_K_TBL2,         /* Thumb BL suffix: target = LR + imm, LR = pc+2 | 1  */
    CI_K_BX,           /* target = rm; bit 0 selects Thumb                   */
    CI_K_BLX_R,        /* as BX, LR = imm (precomputed return address)       */
    CI_K_BLX_I,        /* ARM BLX imm: LR = pc + 4, enter Thumb at imm       */
    CI_K_DP,
    CI_K_DP_END = CI_K_DP + 16 * 4 * 2,
    CI_K_MEM = CI_K_DP_END,
    CI_K_MEM_END = CI_K_MEM + CI_A_COUNT * 2 * 3,
    CI_K_COUNT = CI_K_MEM_END
};

#define CI_DP_KIND(opc, form, s) \
    ((uint8_t)(CI_K_DP + (((unsigned)(opc) * 4u + (unsigned)(form)) * 2u) + (unsigned)(s)))
#define CI_MEM_KIND(acc, src, mode) \
    ((uint8_t)(CI_K_MEM + (((unsigned)(acc) * 2u + (unsigned)(src)) * 3u) + (unsigned)(mode)))

/* The executor's `switch` needs every kind below 256. */
typedef char ci_kinds_fit_in_a_byte[(CI_K_COUNT <= 256) ? 1 : -1];

/* --------------------------------------------------------------- blocks --- */

#define CI_BLOCK_MAX_OPS 64u

typedef struct ci_block {
    struct ci_block *next;  /* hash-bucket chain, newest first               */
    uint32_t va;       /* guest VA of the first instruction                  */
    uint32_t pa_off;   /* its offset in guest DRAM                           */
    uint32_t gen;      /* region generation of pa_off's 1 KiB when built     */
    uint16_t n;        /* ops                                                */
    uint8_t  thumb;    /* CPSR.T it was decoded for                          */
    uint8_t  stop_cause; /* arm_ci_step_cause_t of the instruction that ended
                            decoding with CI_DEC_STOP (meaningful when n == 0) */
    ci_op_t *ops;
} ci_block_t;

/* How a decode ended, for the caller that builds blocks. */
typedef enum {
    CI_DEC_OP = 0,     /* one op written; continue                           */
    CI_DEC_END,        /* op written and it ends the block                   */
    CI_DEC_STOP        /* nothing written: must run through arm_step         */
} ci_dec_t;

/* Decode one instruction at guest `pc` into *op. arch selects ARMv6/v7
 * encodings exactly as the reference does (currently only for MOVW/MOVT,
 * which stay reference-executed). */
ci_dec_t ci_decode_arm(uint32_t pc, uint32_t insn, ci_op_t *op);
ci_dec_t ci_decode_thumb(uint32_t pc, uint16_t insn, ci_op_t *op);

/* Diagnostics: why an instruction that decoded to CI_DEC_STOP must run on
 * arm_step (arm_ci_step_cause_t), and the class of one that decoded to
 * CI_K_REF (arm_ci_ref_class_t). Build-time only. */
unsigned ci_stop_cause(uint32_t insn, bool thumb);
unsigned ci_ref_class(uint32_t insn, bool thumb);

/* --------------------------------------------------------- engine state --- */

#define CI_TLB_ENTRIES 1024u

typedef struct {
    uint32_t tag;      /* (va & ~0x3ff) | priv                               */
    uint32_t gen;      /* cpu->tlb_gen when filled; 0 = empty                */
    uint8_t *host;     /* host pointer to the 1 KiB block                    */
} ci_tlb_t;

/* The VA-keyed front of the block cache (Dolphin's fast_block_map idea): a
 * hit skips the fetch translation and the physical-offset hash. An entry is
 * valid only under the translation it was filled under -- the same rule the
 * host TLBs follow: equal cpu->tlb_gen, equal privilege -- and its block is
 * still checked against its region generation on every use. */
#define CI_FAST_BITS 12u
#define CI_FAST_SIZE (1u << CI_FAST_BITS)

typedef struct {
    uint32_t    va;
    uint32_t    ctx;       /* CPSR.T | priv << 1                              */
    uint32_t    gen;       /* cpu->tlb_gen when filled; 0 = empty             */
    ci_block_t *b;
} ci_fast_t;

struct arm_ci {
    arm_ci_config_t cfg;
    uint32_t  regions;         /* ram_size / 1 KiB                            */
    uint32_t *region_gen;      /* per 1 KiB: bumped when code in it is written */
    uint32_t *code_map;        /* per 1 KiB: 1 = holds cached code            */
    bool      code_written;    /* set by note_ram_write when it invalidates   */
    bool      verify;

    ci_block_t **table;        /* direct-mapped, CI_HASH_SIZE                  */
    ci_block_t  *blocks;       /* pool                                         */
    uint32_t     nblocks;
    ci_op_t     *ops;          /* pool                                         */
    uint32_t     nops;

    ci_tlb_t rtlb[CI_TLB_ENTRIES];
    ci_tlb_t wtlb[CI_TLB_ENTRIES];
    ci_fast_t fast[CI_FAST_SIZE];

    /* The translation context the entries above were filled in. They are
     * keyed by cpu->tlb_gen, which is only comparable while it moves forward:
     * arm_reset and snapshot restore set it back to 1 (seen through
     * cpu->reset_epoch), the 2^32 wrap sets it back (seen as a decrease), and
     * a host that clears SCTLR.M directly changes translation without any
     * flush (seen through the M bit). Any of them purges every
     * translation-derived entry. */
    uint32_t seen_gen;
    uint32_t seen_epoch;
    bool     seen_mmu;

    /* Instructions retired in this arm_ci_run() before the current block,
     * and before the instruction now on the reference path
     * (arm_ci_run_position). */
    unsigned run_block_base;
    unsigned run_position;

    arm_ci_stats_t st;
};

#endif /* S5LBOX_ARM_CI_PRIV_H */
