/*
 * S5LBox — Micro-Op Intermediate Representation (IR) Lifter.
 *
 * Converts pre-decoded ARM and Thumb instructions into clean, atomic micro-operations.
 * Decomposes compound operations (e.g. shifted-register ALU operands and complex
 * address calculations) into simple two-operand micro-ops.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "arm_ir.h"
#include <string.h>

static void emit_insn(arm_ir_insn_t *buf, unsigned *idx, unsigned max_out,
                      arm_ir_op_t op, uint8_t cond, bool cond_always,
                      arm_ir_vreg_t dst, arm_ir_vreg_t src1, arm_ir_vreg_t src2,
                      uint32_t imm, uint32_t guest_pc) {
    if (*idx >= max_out) return;
    arm_ir_insn_t *i = &buf[(*idx)++];
    i->op = op;
    i->cond = cond;
    i->cond_always = cond_always;
    i->dst = dst;
    i->src1 = src1;
    i->src2 = src2;
    i->imm = imm;
    i->guest_pc = guest_pc;
    i->dead = false;
}

bool arm_ir_lift_instruction(const arm_decoded_insn_t *di,
                             arm_ir_insn_t *out,
                             unsigned max_out,
                             unsigned *written_out) {
    if (!di || !out || max_out == 0) return false;
    unsigned idx = 0;

    /* Safe fallback for unhandled or complex coprocessor forms */
    if (di->op == ARM_OP_FALLBACK || di->op == ARM_OP_CPS || di->op == ARM_OP_SVC) {
        emit_insn(out, &idx, max_out, IR_FALLBACK, di->cond, di->cond_always,
                  VREG_NONE, VREG_NONE, VREG_NONE, 0, di->pc);
        if (written_out) *written_out = idx;
        return true;
    }

    if (di->op == ARM_OP_NOP) {
        emit_insn(out, &idx, max_out, IR_NOP, di->cond, di->cond_always,
                  VREG_NONE, VREG_NONE, VREG_NONE, 0, di->pc);
        if (written_out) *written_out = idx;
        return true;
    }

    /* Branches */
    if (di->op == ARM_OP_B) {
        if (di->cond_always) {
            emit_insn(out, &idx, max_out, IR_BRANCH, di->cond, true,
                      VREG_R15, VREG_CONST, VREG_NONE, di->imm, di->pc);
        } else {
            emit_insn(out, &idx, max_out, IR_BRANCH_COND, di->cond, false,
                      VREG_R15, VREG_CONST, VREG_CONST, di->imm, di->pc);
        }
        if (written_out) *written_out = idx;
        return true;
    }

    if (di->op == ARM_OP_BL) {
        /* Set LR = next_pc, then branch */
        emit_insn(out, &idx, max_out, IR_LI, di->cond, di->cond_always,
                  VREG_R14, VREG_CONST, VREG_NONE, di->next_pc, di->pc);
        emit_insn(out, &idx, max_out, IR_BRANCH, di->cond, di->cond_always,
                  VREG_R15, VREG_CONST, VREG_NONE, di->imm, di->pc);
        if (written_out) *written_out = idx;
        return true;
    }

    if (di->op == ARM_OP_BX || di->op == ARM_OP_BLX_REG) {
        if (di->op == ARM_OP_BLX_REG) {
            emit_insn(out, &idx, max_out, IR_LI, di->cond, di->cond_always,
                      VREG_R14, VREG_CONST, VREG_NONE, di->next_pc, di->pc);
        }
        emit_insn(out, &idx, max_out, IR_BRANCH, di->cond, di->cond_always,
                  VREG_R15, (arm_ir_vreg_t)di->rm, VREG_NONE, 0, di->pc);
        if (written_out) *written_out = idx;
        return true;
    }

    /* Load / Store Immediate */
    if (di->op == ARM_OP_LDR_IMM) {
        emit_insn(out, &idx, max_out, IR_LOAD32, di->cond, di->cond_always,
                  (arm_ir_vreg_t)di->rd, (arm_ir_vreg_t)di->rn, VREG_CONST,
                  di->imm, di->pc);
        if (written_out) *written_out = idx;
        return true;
    }
    if (di->op == ARM_OP_LDRB_IMM) {
        emit_insn(out, &idx, max_out, IR_LOAD8, di->cond, di->cond_always,
                  (arm_ir_vreg_t)di->rd, (arm_ir_vreg_t)di->rn, VREG_CONST,
                  di->imm, di->pc);
        if (written_out) *written_out = idx;
        return true;
    }
    if (di->op == ARM_OP_STR_IMM) {
        emit_insn(out, &idx, max_out, IR_STORE32, di->cond, di->cond_always,
                  VREG_NONE, (arm_ir_vreg_t)di->rn, (arm_ir_vreg_t)di->rd,
                  di->imm, di->pc);
        if (written_out) *written_out = idx;
        return true;
    }
    if (di->op == ARM_OP_STRB_IMM) {
        emit_insn(out, &idx, max_out, IR_STORE8, di->cond, di->cond_always,
                  VREG_NONE, (arm_ir_vreg_t)di->rn, (arm_ir_vreg_t)di->rd,
                  di->imm, di->pc);
        if (written_out) *written_out = idx;
        return true;
    }

    /* ALU Operations */
    if (di->op >= ARM_OP_AND && di->op <= ARM_OP_MVN) {
        arm_ir_vreg_t op2_vreg = VREG_NONE;
        uint32_t imm_val = di->imm;

        if (di->is_imm) {
            op2_vreg = VREG_CONST;
        } else if (di->shift_imm == 0 && !di->shift_by_reg) {
            op2_vreg = (arm_ir_vreg_t)di->rm;
        } else {
            /* Decompose shifted register operand: lift into TMP0 */
            arm_ir_op_t shift_op = IR_LSL;
            switch (di->shift_type) {
                case ARM_SHIFT_LSL: shift_op = IR_LSL; break;
                case ARM_SHIFT_LSR: shift_op = IR_LSR; break;
                case ARM_SHIFT_ASR: shift_op = IR_ASR; break;
                case ARM_SHIFT_ROR: shift_op = IR_ROR; break;
                default: shift_op = IR_LSL; break;
            }
            arm_ir_vreg_t shift_amt_reg = di->shift_by_reg ? (arm_ir_vreg_t)di->rs : VREG_CONST;
            emit_insn(out, &idx, max_out, shift_op, di->cond, di->cond_always,
                      VREG_TMP0, (arm_ir_vreg_t)di->rm, shift_amt_reg,
                      di->shift_imm, di->pc);
            op2_vreg = VREG_TMP0;
        }

        /* Map ALU opcode */
        arm_ir_op_t ir_op = IR_ADD;
        switch (di->op) {
            case ARM_OP_AND: ir_op = IR_AND; break;
            case ARM_OP_EOR: ir_op = IR_EOR; break;
            case ARM_OP_SUB: ir_op = IR_SUB; break;
            case ARM_OP_RSB: ir_op = IR_RSB; break;
            case ARM_OP_ADD: ir_op = IR_ADD; break;
            case ARM_OP_ADC: ir_op = IR_ADC; break;
            case ARM_OP_SBC: ir_op = IR_SBC; break;
            case ARM_OP_RSC: ir_op = IR_RSC; break;
            case ARM_OP_TST: ir_op = IR_AND; break;
            case ARM_OP_TEQ: ir_op = IR_EOR; break;
            case ARM_OP_CMP: ir_op = IR_SUB; break;
            case ARM_OP_CMN: ir_op = IR_ADD; break;
            case ARM_OP_ORR: ir_op = IR_ORR; break;
            case ARM_OP_MOV: ir_op = (op2_vreg == VREG_CONST) ? IR_LI : IR_MOV; break;
            case ARM_OP_BIC: ir_op = IR_BIC; break;
            case ARM_OP_MVN: ir_op = IR_MVN; break;
            default: break;
        }

        arm_ir_vreg_t dst_vreg = (arm_ir_vreg_t)di->rd;
        if (di->op == ARM_OP_CMP || di->op == ARM_OP_CMN ||
            di->op == ARM_OP_TST || di->op == ARM_OP_TEQ) {
            dst_vreg = VREG_TMP1; /* Comparisons write to temporary */
        }

        arm_ir_vreg_t src1_vreg = (di->op == ARM_OP_MOV || di->op == ARM_OP_MVN)
                                ? ((op2_vreg == VREG_CONST) ? VREG_NONE : op2_vreg)
                                : (arm_ir_vreg_t)di->rn;
        arm_ir_vreg_t src2_vreg = (di->op == ARM_OP_MOV || di->op == ARM_OP_MVN)
                                ? VREG_NONE : op2_vreg;

        emit_insn(out, &idx, max_out, ir_op, di->cond, di->cond_always,
                  dst_vreg, src1_vreg, src2_vreg, imm_val, di->pc);

        /* Flags update micro-op */
        if (di->sets_flags) {
            if (di->op == ARM_OP_ADD || di->op == ARM_OP_CMN) {
                emit_insn(out, &idx, max_out, IR_UPDATE_NZCV_ADD, di->cond, di->cond_always,
                          dst_vreg, src1_vreg, src2_vreg, imm_val, di->pc);
            } else if (di->op == ARM_OP_SUB || di->op == ARM_OP_CMP) {
                emit_insn(out, &idx, max_out, IR_UPDATE_NZCV_SUB, di->cond, di->cond_always,
                          dst_vreg, src1_vreg, src2_vreg, imm_val, di->pc);
            } else {
                emit_insn(out, &idx, max_out, IR_UPDATE_NZ, di->cond, di->cond_always,
                          dst_vreg, VREG_NONE, VREG_NONE, 0, di->pc);
            }
        }

        if (written_out) *written_out = idx;
        return true;
    }

    /* Fallback for any unhandled forms */
    emit_insn(out, &idx, max_out, IR_FALLBACK, di->cond, di->cond_always,
              VREG_NONE, VREG_NONE, VREG_NONE, 0, di->pc);
    if (written_out) *written_out = idx;
    return true;
}
