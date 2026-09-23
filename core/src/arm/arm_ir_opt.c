/*
 * S5LBox — Micro-Op Intermediate Representation (IR) Optimization Passes.
 *
 * Implements Phase 9:
 * 1. Constant folding & immediate propagation
 * 2. Dead temporary elimination
 * 3. Redundant condition flag update pruning
 * 4. Micro-op compaction
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "arm_ir.h"
#include <string.h>

/* Pass 1: Constant Folding */
unsigned arm_ir_opt_constant_folding(arm_ir_block_t *block) {
    if (!block || block->insn_count == 0) return 0;
    unsigned folded = 0;

    for (unsigned i = 0; i < block->insn_count; i++) {
        arm_ir_insn_t *insn = &block->insns[i];
        if (insn->dead) continue;

        /* Fold consecutive LI into subsequent immediate arithmetic */
        if (insn->op == IR_LI && insn->dst >= VREG_TMP0 && insn->dst <= VREG_TMP7) {
            arm_ir_vreg_t tmp = insn->dst;
            uint32_t c = insn->imm;

            /* Check subsequent instructions for use of tmp */
            for (unsigned j = i + 1; j < block->insn_count; j++) {
                arm_ir_insn_t *next = &block->insns[j];
                if (next->dead) continue;

                if (next->src2 == tmp && (next->op == IR_ADD || next->op == IR_SUB)) {
                    /* Propagate constant directly */
                    next->src2 = VREG_CONST;
                    next->imm = c;
                    folded++;
                    break;
                }
                if (next->dst == tmp) break; /* Overwritten */
            }
        }
    }
    return folded;
}

/* Pass 2: Redundant Flag Update Pruning */
unsigned arm_ir_opt_redundant_flags(arm_ir_block_t *block) {
    if (!block || block->insn_count == 0) return 0;
    unsigned pruned = 0;

    for (unsigned i = 0; i < block->insn_count; i++) {
        arm_ir_insn_t *insn = &block->insns[i];
        if (insn->dead) continue;

        if (insn->op == IR_UPDATE_NZ ||
            insn->op == IR_UPDATE_NZCV_ADD ||
            insn->op == IR_UPDATE_NZCV_SUB) {

            /* Look ahead: if next flag-touching instruction is another update without
             * any conditional check in between, this update is dead! */
            for (unsigned j = i + 1; j < block->insn_count; j++) {
                arm_ir_insn_t *next = &block->insns[j];
                if (next->dead) continue;

                /* If an instruction depends on conditions, flags must be preserved */
                if (!next->cond_always || next->op == IR_BRANCH_COND ||
                    next->op == IR_ADC || next->op == IR_SBC || next->op == IR_RSC) {
                    break;
                }

                if (next->op == IR_UPDATE_NZ ||
                    next->op == IR_UPDATE_NZCV_ADD ||
                    next->op == IR_UPDATE_NZCV_SUB) {
                    insn->dead = true;
                    pruned++;
                    break;
                }
            }
        }
    }
    return pruned;
}

/* Pass 3: Dead Code Elimination (Temporaries) */
unsigned arm_ir_opt_dead_code(arm_ir_block_t *block) {
    if (!block || block->insn_count == 0) return 0;
    unsigned removed = 0;

    for (unsigned i = 0; i < block->insn_count; i++) {
        arm_ir_insn_t *insn = &block->insns[i];
        if (insn->dead) continue;

        /* Only virtual temporaries with no side effects are candidates */
        if (insn->dst >= VREG_TMP0 && insn->dst <= VREG_TMP7 &&
            insn->op != IR_LOAD32 && insn->op != IR_LOAD16 && insn->op != IR_LOAD8 &&
            insn->op != IR_STORE32 && insn->op != IR_STORE16 && insn->op != IR_STORE8) {

            arm_ir_vreg_t tmp = insn->dst;
            bool read_later = false;

            for (unsigned j = i + 1; j < block->insn_count; j++) {
                arm_ir_insn_t *next = &block->insns[j];
                if (next->dead) continue;

                if (next->src1 == tmp || next->src2 == tmp) {
                    read_later = true;
                    break;
                }
                if (next->dst == tmp) {
                    break; /* Overwritten without being read */
                }
            }

            if (!read_later) {
                insn->dead = true;
                removed++;
            }
        }
    }
    return removed;
}

/* Compaction */
static void compact_block(arm_ir_block_t *block) {
    unsigned write_idx = 0;
    for (unsigned read_idx = 0; read_idx < block->insn_count; read_idx++) {
        if (!block->insns[read_idx].dead) {
            if (write_idx != read_idx) {
                block->insns[write_idx] = block->insns[read_idx];
            }
            write_idx++;
        }
    }
    block->insn_count = write_idx;
}

void arm_ir_optimize_block(arm_ir_block_t *block) {
    if (!block) return;
    arm_ir_opt_constant_folding(block);
    arm_ir_opt_redundant_flags(block);
    arm_ir_opt_dead_code(block);
    compact_block(block);
}
