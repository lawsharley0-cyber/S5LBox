/*
 * S5LBox — Single-Pass ARM & Thumb Instruction Decoder for Cached Interpreter.
 *
 * Translates raw 32-bit ARM and 16-bit Thumb instruction words into
 * pre-decoded arm_decoded_insn_t records. Pre-extracts registers, condition
 * codes, shift modes, and immediate values so execution does not repeat
 * bitfield parsing.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "arm_block.h"
#include <string.h>

static bool decode_arm(uint32_t pc, uint32_t insn, arm_decoded_insn_t *out) {
    out->pc = pc;
    out->next_pc = pc + 4;
    out->raw_insn = insn;
    out->is_thumb = false;
    out->cond = (uint8_t)(insn >> 28);
    out->cond_always = (out->cond == 0xEu || out->cond == 0xFu);
    out->writes_pc = false;
    out->sets_flags = false;
    out->shift_by_reg = false;
    out->is_imm = false;

    /* Unconditional space (cond == 0xF) */
    if (out->cond == 0xFu) {
        if (insn == 0xf57ff01fu) {
            out->op = ARM_OP_CLREX;
            return true;
        }
        if ((insn & 0x0c00f000u) == 0x0400f000u) {
            /* PLD hint -> NOP */
            out->op = ARM_OP_NOP;
            return true;
        }
        if ((insn & 0xfe000000u) == 0xfa000000u) {
            /* BLX <imm> */
            int32_t off = (int32_t)(insn << 8) >> 6;
            uint32_t h = (insn >> 24) & 1u;
            out->op = ARM_OP_BLX_IMM;
            out->imm = (pc + 8 + (uint32_t)off + (h << 1)) & ~1u;
            out->writes_pc = true;
            return true;
        }
        if ((insn & 0xfff1fe20u) == 0xf1000000u) {
            out->op = ARM_OP_CPS;
            out->imm = insn;
            return true;
        }
        out->op = ARM_OP_FALLBACK;
        return true;
    }

    /* Branch / Branch with Link (B, BL) */
    if ((insn & 0x0e000000u) == 0x0a000000u) {
        bool is_bl = (insn >> 24) & 1u;
        int32_t imm24 = (int32_t)(insn << 8) >> 6; /* Sign-extend, << 2 */
        out->op = is_bl ? ARM_OP_BL : ARM_OP_B;
        out->imm = (uint32_t)(pc + 8 + imm24);
        out->writes_pc = true;
        return true;
    }

    /* Branch Exchange (BX, BLX reg) */
    if ((insn & 0x0ffffff0u) == 0x012fff10u) {
        out->op = ARM_OP_BX;
        out->rm = (uint8_t)(insn & 0xfu);
        out->writes_pc = true;
        return true;
    }
    if ((insn & 0x0ffffff0u) == 0x012fff30u) {
        out->op = ARM_OP_BLX_REG;
        out->rm = (uint8_t)(insn & 0xfu);
        out->writes_pc = true;
        return true;
    }

    /* Software Interrupt (SVC / SWI) */
    if ((insn & 0x0f000000u) == 0x0f000000u) {
        out->op = ARM_OP_SVC;
        out->imm = insn & 0x00ffffffu;
        out->writes_pc = true;
        return true;
    }

    /* Data Processing (ALU) & Multiplies */
    if ((insn & 0x0c000000u) == 0x00000000u) {
        /* Multiplies check */
        if ((insn & 0x0fc000f0u) == 0x00000090u) {
            bool a = (insn >> 21) & 1u;
            out->op = a ? ARM_OP_MLA : ARM_OP_MUL;
            out->sets_flags = (insn >> 20) & 1u;
            out->rd = (uint8_t)((insn >> 16) & 0xfu);
            out->rn = (uint8_t)((insn >> 12) & 0xfu);
            out->rs = (uint8_t)((insn >> 8) & 0xfu);
            out->rm = (uint8_t)(insn & 0xfu);
            out->writes_pc = (out->rd == 15);
            return true;
        }
        if ((insn & 0x0f8000f0u) == 0x00800090u) {
            bool is_signed = (insn >> 22) & 1u;
            bool is_accum  = (insn >> 21) & 1u;
            out->op = is_signed ? (is_accum ? ARM_OP_SMLAL : ARM_OP_SMULL)
                                : (is_accum ? ARM_OP_UMLAL : ARM_OP_UMULL);
            out->sets_flags = (insn >> 20) & 1u;
            out->rd = (uint8_t)((insn >> 16) & 0xfu); /* RdHi */
            out->rn = (uint8_t)((insn >> 12) & 0xfu); /* RdLo */
            out->rs = (uint8_t)((insn >> 8) & 0xfu);
            out->rm = (uint8_t)(insn & 0xfu);
            out->writes_pc = (out->rd == 15 || out->rn == 15);
            return true;
        }

        /* Halfword / byte sign extend & multiply / misc v6 */
        if ((insn & 0x0e000090u) == 0x00000090u) {
            /* Fallback to reference interpreter for miscellaneous extensions */
            out->op = ARM_OP_FALLBACK;
            return true;
        }

        /* Standard ALU */
        unsigned opcode = (insn >> 21) & 0xfu;
        out->op = (arm_decoded_op_t)(ARM_OP_AND + opcode);
        out->sets_flags = (insn >> 20) & 1u;
        out->rn = (uint8_t)((insn >> 16) & 0xfu);
        out->rd = (uint8_t)((insn >> 12) & 0xfu);
        out->writes_pc = (out->rd == 15);
        out->is_imm = (insn >> 25) & 1u;

        if (out->is_imm) {
            unsigned rot = ((insn >> 8) & 0xfu) * 2u;
            uint32_t val = insn & 0xffu;
            out->imm = rot ? ((val >> rot) | (val << (32u - rot))) : val;
        } else {
            out->rm = (uint8_t)(insn & 0xfu);
            out->shift_by_reg = (insn >> 4) & 1u;
            out->shift_type = (arm_shift_type_t)((insn >> 5) & 3u);
            if (out->shift_by_reg) {
                out->rs = (uint8_t)((insn >> 8) & 0xfu);
            } else {
                out->shift_imm = (uint8_t)((insn >> 7) & 0x1fu);
            }
        }
        return true;
    }

    /* Single Data Transfer (LDR / STR) */
    if ((insn & 0x0c000000u) == 0x04000000u) {
        bool is_load = (insn >> 20) & 1u;
        bool is_byte = (insn >> 22) & 1u;
        bool is_imm  = !((insn >> 25) & 1u); /* Bit 25 == 0 is immediate in ARM! */
        bool p_bit   = (insn >> 24) & 1u;
        bool u_bit   = (insn >> 23) & 1u;
        bool w_bit   = (insn >> 21) & 1u;

        out->rn = (uint8_t)((insn >> 16) & 0xfu);
        out->rd = (uint8_t)((insn >> 12) & 0xfu);
        out->writes_pc = (is_load && out->rd == 15);

        /* Fast path: standard pre-indexed, no-writeback, positive/negative immediate */
        if (is_imm && p_bit && !w_bit) {
            uint32_t offset = insn & 0xfffu;
            out->op = is_load ? (is_byte ? ARM_OP_LDRB_IMM : ARM_OP_LDR_IMM)
                              : (is_byte ? ARM_OP_STRB_IMM : ARM_OP_STR_IMM);
            out->imm = u_bit ? offset : (uint32_t)(-(int32_t)offset);
            out->is_imm = true;
            return true;
        }

        /* Complex addressing forms fall back to reference interpreter */
        out->op = ARM_OP_FALLBACK;
        return true;
    }

    /* Block Data Transfer (LDM / STM) */
    if ((insn & 0x0e000000u) == 0x08000000u) {
        bool is_load = (insn >> 20) & 1u;
        out->op = is_load ? ARM_OP_LDM : ARM_OP_STM;
        out->rn = (uint8_t)((insn >> 16) & 0xfu);
        out->imm = insn & 0xffffu; /* Register list */
        if (is_load && (out->imm & (1u << 15))) {
            out->writes_pc = true;
        }
        /* Fall back to reference interpreter for complex mode-switching LDM */
        if ((insn & (1u << 22)) != 0) {
            out->op = ARM_OP_FALLBACK;
        }
        return true;
    }

    /* Coprocessor / VFP / Undefined -> Safe fallback */
    out->op = ARM_OP_FALLBACK;
    return true;
}

static bool decode_thumb(uint32_t pc, uint16_t tinsn, arm_decoded_insn_t *out) {
    out->pc = pc;
    out->next_pc = pc + 2;
    out->raw_insn = tinsn;
    out->is_thumb = true;
    out->cond = 0xEu; /* AL */
    out->cond_always = true;
    out->writes_pc = false;
    out->sets_flags = true; /* Most Thumb instructions update flags */
    out->shift_by_reg = false;
    out->is_imm = false;

    /* Move / compare / add / sub immediate: bits [15:13] == 001 */
    if ((tinsn & 0xe000u) == 0x2000u) {
        unsigned op = (tinsn >> 11) & 3u;
        out->rd = (uint8_t)((tinsn >> 8) & 7u);
        out->rn = out->rd;
        out->imm = tinsn & 0xffu;
        out->is_imm = true;
        switch (op) {
            case 0: out->op = ARM_OP_MOV; break;
            case 1: out->op = ARM_OP_CMP; break;
            case 2: out->op = ARM_OP_ADD; break;
            case 3: out->op = ARM_OP_SUB; break;
        }
        return true;
    }

    /* Shift by immediate: bits [15:11] in 0..2 */
    if ((tinsn & 0xe000u) == 0x0000u && (tinsn & 0x1800u) != 0x1800u) {
        unsigned op = (tinsn >> 11) & 3u;
        out->rd = (uint8_t)(tinsn & 7u);
        out->rm = (uint8_t)((tinsn >> 3) & 7u);
        out->shift_imm = (uint8_t)((tinsn >> 6) & 0x1fu);
        out->shift_type = (arm_shift_type_t)op;
        out->op = ARM_OP_MOV;
        return true;
    }

    /* Add / sub register or 3-bit imm: bits [15:11] == 00011 */
    if ((tinsn & 0xf800u) == 0x1800u) {
        bool is_sub = (tinsn >> 9) & 1u;
        bool is_imm = (tinsn >> 10) & 1u;
        out->op = is_sub ? ARM_OP_SUB : ARM_OP_ADD;
        out->rd = (uint8_t)(tinsn & 7u);
        out->rn = (uint8_t)((tinsn >> 3) & 7u);
        if (is_imm) {
            out->imm = (tinsn >> 6) & 7u;
            out->is_imm = true;
        } else {
            out->rm = (uint8_t)((tinsn >> 6) & 7u);
        }
        return true;
    }

    /* ALU operations: bits [15:10] == 010000 */
    if ((tinsn & 0xfc00u) == 0x4000u) {
        unsigned op = (tinsn >> 6) & 0xfu;
        out->rd = (uint8_t)(tinsn & 7u);
        out->rn = out->rd;
        out->rm = (uint8_t)((tinsn >> 3) & 7u);
        switch (op) {
            case 0x0: out->op = ARM_OP_AND; break;
            case 0x1: out->op = ARM_OP_EOR; break;
            case 0x5: out->op = ARM_OP_ADC; break;
            case 0x6: out->op = ARM_OP_SBC; break;
            case 0x8: out->op = ARM_OP_TST; break;
            case 0xa: out->op = ARM_OP_CMP; break;
            case 0xb: out->op = ARM_OP_CMN; break;
            case 0xc: out->op = ARM_OP_ORR; break;
            case 0xd: out->op = ARM_OP_MUL; break;
            case 0xe: out->op = ARM_OP_BIC; break;
            case 0xf: out->op = ARM_OP_MVN; break;
            default:  out->op = ARM_OP_FALLBACK; break;
        }
        return true;
    }

    /* Hi-register operations / BX / BLX: bits [15:10] == 010001 */
    if ((tinsn & 0xfc00u) == 0x4400u) {
        unsigned op = (tinsn >> 8) & 3u;
        uint8_t rd = (uint8_t)(((tinsn >> 4) & 8u) | (tinsn & 7u));
        uint8_t rm = (uint8_t)((tinsn >> 3) & 0xfu);
        if (op == 3) {
            bool is_blx = (tinsn >> 7) & 1u;
            out->op = is_blx ? ARM_OP_BLX_REG : ARM_OP_BX;
            out->rm = rm;
            out->writes_pc = true;
            return true;
        }
        if (op == 2) { /* MOV Rd, Rm */
            out->op = ARM_OP_MOV;
            out->rd = rd;
            out->rm = rm;
            out->sets_flags = false;
            out->writes_pc = (rd == 15);
            return true;
        }
        if (op == 0) { /* ADD Rd, Rm */
            out->op = ARM_OP_ADD;
            out->rd = rd;
            out->rn = rd;
            out->rm = rm;
            out->sets_flags = false;
            out->writes_pc = (rd == 15);
            return true;
        }
        if (op == 1) { /* CMP Rn, Rm */
            out->op = ARM_OP_CMP;
            out->rn = rd;
            out->rm = rm;
            out->sets_flags = true;
            return true;
        }
    }

    /* PC-relative load: bits [15:11] == 01001 */
    if ((tinsn & 0xf800u) == 0x4800u) {
        out->op = ARM_OP_LDR_IMM;
        out->rd = (uint8_t)((tinsn >> 8) & 7u);
        out->rn = 15; /* PC */
        out->imm = ((uint32_t)(tinsn & 0xffu)) << 2u;
        out->is_imm = true;
        out->sets_flags = false;
        return true;
    }

    /* Load/Store word/byte immediate offset: bits [15:12] == 0110 / 0111 */
    if ((tinsn & 0xe000u) == 0x6000u) {
        bool is_load = (tinsn >> 11) & 1u;
        bool is_byte = (tinsn >> 12) & 1u;
        out->rd = (uint8_t)(tinsn & 7u);
        out->rn = (uint8_t)((tinsn >> 3) & 7u);
        uint32_t off = (tinsn >> 6) & 0x1fu;
        out->imm = is_byte ? off : (off << 2u);
        out->is_imm = true;
        out->sets_flags = false;
        out->op = is_load ? (is_byte ? ARM_OP_LDRB_IMM : ARM_OP_LDR_IMM)
                          : (is_byte ? ARM_OP_STRB_IMM : ARM_OP_STR_IMM);
        return true;
    }

    /* SP-relative Load/Store word: bits [15:11] == 10010 / 10011 */
    if ((tinsn & 0xf000u) == 0x9000u) {
        bool is_load = (tinsn >> 11) & 1u;
        out->rd = (uint8_t)((tinsn >> 8) & 7u);
        out->rn = 13; /* SP */
        out->imm = ((uint32_t)(tinsn & 0xffu)) << 2u;
        out->is_imm = true;
        out->sets_flags = false;
        out->op = is_load ? ARM_OP_LDR_IMM : ARM_OP_STR_IMM;
        return true;
    }

    /* Conditional Branch (B<cond>) / SWI: bits [15:12] == 1101 */
    if ((tinsn & 0xf000u) == 0xd000u) {
        uint8_t cond = (uint8_t)((tinsn >> 8) & 0xfu);
        if (cond == 0xfu) {
            /* SWI */
            out->op = ARM_OP_SVC;
            out->imm = tinsn & 0xffu;
            out->writes_pc = true;
            return true;
        }
        if (cond < 0xeu) {
            int32_t imm8 = (int32_t)(int8_t)(tinsn & 0xffu);
            out->op = ARM_OP_B;
            out->cond = cond;
            out->cond_always = false;
            out->imm = (uint32_t)((int32_t)pc + 4 + (imm8 << 1));
            out->writes_pc = true;
            return true;
        }
    }

    /* Unconditional branch: bits [15:11] == 11100 */
    if ((tinsn & 0xf800u) == 0xe000u) {
        int32_t imm11 = (int32_t)(tinsn << 21) >> 20; /* Sign-extend, << 1 */
        out->op = ARM_OP_B;
        out->imm = (uint32_t)((int32_t)pc + 4 + imm11);
        out->writes_pc = true;
        return true;
    }

    /* Fallback for remaining forms */
    out->op = ARM_OP_FALLBACK;
    return true;
}

bool arm_decode_instruction(arm_cpu_t *cpu, uint32_t pc, uint32_t raw, bool thumb, arm_decoded_insn_t *out) {
    (void)cpu;
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (thumb) {
        return decode_thumb(pc, (uint16_t)raw, out);
    } else {
        return decode_arm(pc, raw, out);
    }
}
