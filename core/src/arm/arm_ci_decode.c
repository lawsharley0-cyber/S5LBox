/*
 * S5LBox — cached interpreter decoder: one guest instruction -> one ci_op_t.
 *
 * The decode order is arm_step()'s order, and every classification below is
 * conservative: a specialised record is emitted only for an operand
 * combination whose reference semantics are exactly what the handler in
 * arm_ci.c computes. Anything unusual -- a PC operand the reference treats
 * specially, an UNPREDICTABLE/UNDEFINED combination, a translation-mode load,
 * an unaligned-sensitive corner -- becomes CI_K_REF and runs the reference
 * code for that one instruction. Instructions that must not execute inside a
 * block at all return CI_DEC_STOP.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "arm_ci_priv.h"
#include "vfp.h"
#include <string.h>

static uint32_t ror32(uint32_t v, unsigned n) {
    n &= 31u;
    return n ? (v >> n) | (v << (32u - n)) : v;
}

/* A reference record. `end` when the instruction always leaves the
 * sequential path (so decoding further would only read data). */
static ci_dec_t ref(ci_op_t *op, bool end) {
    op->kind = CI_K_REF;
    return end ? CI_DEC_END : CI_DEC_OP;
}

/* Normalise an immediate shift as the barrel shifter interprets it. Returns
 * false for "LSL #0" (the plain register form). */
static bool norm_imm_shift(unsigned type, unsigned amt, uint8_t *sh, uint8_t *sa) {
    switch (type & 3u) {
        case 0:  if (amt == 0u) return false;
                 *sh = CI_SH_LSL; *sa = (uint8_t)amt; return true;
        case 1:  *sh = CI_SH_LSR; *sa = (uint8_t)(amt ? amt : 32u); return true;
        case 2:  *sh = CI_SH_ASR; *sa = (uint8_t)(amt ? amt : 32u); return true;
        default: if (amt == 0u) { *sh = CI_SH_RRX; *sa = 1u; return true; }
                 *sh = CI_SH_ROR; *sa = (uint8_t)amt; return true;
    }
}

static bool dp_writes_result(unsigned opc) { return opc < 8u || opc >= 12u; }
static bool dp_is_logical(unsigned opc) {
    return opc == 0u || opc == 1u || opc == 8u || opc == 9u || opc >= 12u;
}

/* A block transfer of the registers in `list` (at most r0..r15), lowest
 * address = base + start, base += delta afterwards (0: no writeback). The
 * caller has already routed every form whose reference result is not a
 * plain ascending transfer (S bit, empty list, UNPREDICTABLE writeback
 * orderings, STM of r15) to the reference. */
static ci_dec_t block_transfer(ci_op_t *op, bool load, unsigned rn, uint32_t list,
                               int start, int delta) {
    unsigned n = 0;
    for (uint32_t l = list; l; l &= l - 1u) n++;
    op->kind = load ? ((list >> 15) ? CI_K_LDM_PC : CI_K_LDM) : CI_K_STM;
    op->rn = (uint8_t)rn;
    op->rm = (uint8_t)n;
    op->imm = list;
    op->sa = (uint8_t)(int8_t)start;
    op->rs = (uint8_t)(int8_t)delta;
    return op->kind == CI_K_LDM_PC ? CI_DEC_END : CI_DEC_OP;
}

/* --------------------------------------------------------------- ARM --- */

static ci_dec_t decode_block(uint32_t insn, ci_op_t *op) {
    const bool P = (insn >> 24) & 1u, U = (insn >> 23) & 1u, S = (insn >> 22) & 1u;
    const bool W = (insn >> 21) & 1u, L = (insn >> 20) & 1u;
    const unsigned rn = (insn >> 16) & 0xfu;
    const uint32_t list = insn & 0xffffu;
    const bool pc_in = (list >> 15) & 1u;
    /* S: user-bank transfer or exception return. rn == 15 and an empty list
     * are UNDEFINED in the reference; W with Rn in the list is UNDEFINED or
     * stores a value only the reference orders correctly; STM of r15 stores
     * pc + 12. */
    if (S || rn == 15u || list == 0u || (W && ((list >> rn) & 1u)) || (!L && pc_in))
        return ref(op, L && pc_in);
    int n = 0;
    for (uint32_t l = list; l; l &= l - 1u) n++;
    int start = U ? (P ? 4 : 0) : (P ? -4 * n : -4 * n + 4);
    int delta = W ? (U ? 4 * n : -4 * n) : 0;
    return block_transfer(op, L, rn, list, start, delta);
}

static ci_dec_t decode_dp(uint32_t pc, uint32_t insn, bool v7, ci_op_t *op) {
    unsigned opc = (insn >> 21) & 0xfu;
    unsigned S   = (insn >> 20) & 1u;
    unsigned rn  = (insn >> 16) & 0xfu;
    unsigned rd  = (insn >> 12) & 0xfu;
    bool     I   = (insn >> 25) & 1u;

    /* Opcodes 8..11 without S are the miscellaneous/control space. */
    if (opc >= 8u && opc <= 11u && !S) {
        unsigned rm = insn & 0xfu;
        if ((insn & 0x0ffffff0u) == 0x012fff10u) {          /* BX Rm */
            if (rm == 15u) return ref(op, true);
            op->kind = CI_K_BX; op->rm = (uint8_t)rm;
            return CI_DEC_END;
        }
        if ((insn & 0x0ffffff0u) == 0x012fff30u) {          /* BLX Rm */
            if (rm == 15u) return ref(op, true);
            op->kind = CI_K_BLX_R; op->rm = (uint8_t)rm; op->imm = pc + 4u;
            return CI_DEC_END;
        }
        if ((insn & 0x0fff0ff0u) == 0x016f0f10u) {          /* CLZ */
            if (rm == 15u || rd == 15u) return ref(op, false);
            op->kind = CI_K_CLZ; op->rd = (uint8_t)rd; op->rm = (uint8_t)rm;
            return CI_DEC_OP;
        }
        if ((insn & 0x0fff0fffu) == 0x010f0000u) {          /* MRS Rd, CPSR */
            if (rd == 15u) return ref(op, false);
            op->kind = CI_K_MRS_CPSR; op->rd = (uint8_t)rd;
            return CI_DEC_OP;
        }
        /* ARMv7 hints are MSR-immediate encodings with an empty mask.
         * WFI waits for an interrupt, which advances device time, so it is
         * arm_step's; the others are no-ops in the reference. */
        if (v7 && (insn & 0x0fffff00u) == 0x0320f000u) {
            if ((insn & 0xffu) == 3u) return CI_DEC_STOP;
            op->kind = CI_K_NOP;
            return CI_DEC_OP;
        }
        return ref(op, false);        /* MSR, MRS SPSR, QADD..., MOVW/MOVT */
    }

    if (dp_writes_result(opc) && rd == 15u) {
        /* MOV pc, Rm: an ALU write of the PC, word aligned, no interworking
         * (ARM state, S clear). Every other form stays reference. */
        const unsigned rm = insn & 0xfu;
        if (opc == 13u && !S && !I && (insn & 0xff0u) == 0u && rm != 15u) {
            /* ARMv7's ALUWritePC interworks in ARM state: BX exactly,
             * including its refusal of a target with bits 1:0 == 10. */
            op->kind = v7 ? CI_K_BX : CI_K_JMP; op->rm = (uint8_t)rm; op->imm = 0u;
            return CI_DEC_END;
        }
        return ref(op, true);
    }

    unsigned form;
    op->rd = (uint8_t)rd;
    op->rn = (uint8_t)rn;
    if (I) {
        unsigned rot = ((insn >> 8) & 0xfu) * 2u;
        op->imm = ror32(insn & 0xffu, rot);
        if (rot) { op->sa = 1u; op->sh = (uint8_t)(op->imm >> 31); }
        form = CI_F_IMM;
        if (rn == 15u && opc != 13u && opc != 15u) {
            /* ADR idiom: ADD/SUB Rd, PC, #imm without flags folds to a
             * constant. Every other PC-based form stays reference. */
            if (!S && (opc == 4u || opc == 2u)) {
                uint32_t base = pc + 8u;
                opc = 13u;                                   /* MOV */
                op->imm = (insn >> 21 & 0xfu) == 4u ? base + op->imm
                                                    : base - op->imm;
                op->rn = 0u; op->sa = 0u; op->sh = 0u;
            } else {
                return ref(op, false);
            }
        }
    } else {
        unsigned rm = insn & 0xfu;
        unsigned type = (insn >> 5) & 3u;
        if (insn & (1u << 4)) {                               /* shift by Rs */
            unsigned rs = (insn >> 8) & 0xfu;
            if (rd == 15u || rn == 15u || rm == 15u || rs == 15u)
                return ref(op, false);                        /* UNDEFINED */
            form = CI_F_SHR;
            op->rm = (uint8_t)rm; op->rs = (uint8_t)rs; op->sh = (uint8_t)type;
        } else {
            if (rm == 15u) return ref(op, false);
            if (rn == 15u && opc != 13u && opc != 15u) {
                /* The PIC idiom ADD Rd, PC, Rm (and SUB) without flags or a
                 * shift folds the PC to a constant: Rm + (pc+8), or
                 * (pc+8) - Rm as RSB. Every other PC-based form stays
                 * reference. */
                if (!S && (opc == 4u || opc == 2u) && (insn & 0xff0u) == 0u) {
                    op->kind = CI_DP_KIND(opc == 4u ? 4u : 3u, CI_F_IMM, 0u);
                    op->rd = (uint8_t)rd; op->rn = (uint8_t)rm;
                    op->imm = pc + 8u;
                    return CI_DEC_OP;
                }
                return ref(op, false);
            }
            op->rm = (uint8_t)rm;
            form = norm_imm_shift(type, (insn >> 7) & 0x1fu, &op->sh, &op->sa)
                       ? CI_F_SHI : CI_F_REG;
        }
    }
    if (opc == 13u || opc == 15u) op->rn = 0u;               /* MOV/MVN: no Rn */
    op->kind = CI_DP_KIND(opc, form, S);
    (void)dp_is_logical;
    return CI_DEC_OP;
}

static ci_dec_t decode_single(uint32_t pc, uint32_t insn, ci_op_t *op) {
    bool I = (insn >> 25) & 1u, P = (insn >> 24) & 1u, U = (insn >> 23) & 1u;
    bool B = (insn >> 22) & 1u, W = (insn >> 21) & 1u, L = (insn >> 20) & 1u;
    unsigned rn = (insn >> 16) & 0xfu, rd = (insn >> 12) & 0xfu;
    unsigned rm = insn & 0xfu;
    bool writeback = !P || W;

    if (!P && W) return ref(op, L && rd == 15u);       /* LDRT/STRT family */
    if (writeback && (rn == 15u || rn == rd)) return ref(op, false);
    if (I && rm == 15u) return ref(op, false);
    if (rd == 15u) {                                    /* LDR pc / STR pc */
        if (!L || B) return ref(op, L);
        /* Writeback with a PC base and a PC offset register were refused
         * above, as the reference refuses them. */
        const unsigned mode = P ? (W ? CI_M_PRE : CI_M_OFF) : CI_M_POST;
        op->kind = CI_K_LDR_PC;
        op->rn = (uint8_t)rn;
        op->rs = (uint8_t)(mode | (I ? 4u : 0u) | (rn == 15u ? 8u : 0u));
        if (!I) {
            const uint32_t off = insn & 0xfffu;
            op->imm = U ? off : 0u - off;
        } else {
            op->rm = (uint8_t)rm;
            if (!norm_imm_shift((insn >> 5) & 3u, (insn >> 7) & 0x1fu, &op->sh, &op->sa)) {
                op->sh = CI_SH_LSL; op->sa = 0u;
            }
            if (!U) op->sh |= CI_SH_SUB;
        }
        return CI_DEC_END;
    }

    if (rn == 15u) {                                    /* PC base, no writeback */
        if (!I && L && !B) {
            uint32_t off = insn & 0xfffu;
            op->kind = CI_K_LDR_LIT; op->rd = (uint8_t)rd;
            op->imm = U ? pc + 8u + off : pc + 8u - off;
            return CI_DEC_OP;
        }
        return ref(op, false);
    }

    unsigned acc = L ? (B ? CI_A_LDRB : CI_A_LDR) : (B ? CI_A_STRB : CI_A_STR);
    unsigned mode = P ? (W ? CI_M_PRE : CI_M_OFF) : CI_M_POST;
    op->rd = (uint8_t)rd; op->rn = (uint8_t)rn;
    if (!I) {
        uint32_t off = insn & 0xfffu;
        op->imm = U ? off : 0u - off;
        op->kind = CI_MEM_KIND(acc, CI_S_IMM, mode);
    } else {
        op->rm = (uint8_t)rm;
        if (!norm_imm_shift((insn >> 5) & 3u, (insn >> 7) & 0x1fu, &op->sh, &op->sa)) {
            op->sh = CI_SH_LSL; op->sa = 0u;
        }
        if (!U) op->sh |= CI_SH_SUB;
        op->kind = CI_MEM_KIND(acc, CI_S_REG, mode);
    }
    return CI_DEC_OP;
}

static ci_dec_t decode_extra(uint32_t insn, ci_op_t *op) {
    bool P = (insn >> 24) & 1u, U = (insn >> 23) & 1u, I = (insn >> 22) & 1u;
    bool W = (insn >> 21) & 1u, L = (insn >> 20) & 1u;
    unsigned rn = (insn >> 16) & 0xfu, rd = (insn >> 12) & 0xfu;
    unsigned sh = (insn >> 5) & 3u, rm = insn & 0xfu;
    bool writeback = !P || W;

    if (!L && sh != 1u) return ref(op, false);          /* LDRD/STRD */
    if (rd == 15u || (!P && W)) return ref(op, false);
    if (writeback && (rn == 15u || rn == rd)) return ref(op, false);
    if (!I && ((((insn >> 8) & 0xfu) != 0u) || rm == 15u)) return ref(op, false);
    if (rn == 15u) return ref(op, false);

    unsigned acc = !L ? CI_A_STRH
                      : (sh == 1u ? CI_A_LDRH : sh == 2u ? CI_A_LDRSB : CI_A_LDRSH);
    unsigned mode = P ? (W ? CI_M_PRE : CI_M_OFF) : CI_M_POST;
    op->rd = (uint8_t)rd; op->rn = (uint8_t)rn;
    if (I) {
        uint32_t off = (((insn >> 8) & 0xfu) << 4) | (insn & 0xfu);
        op->imm = U ? off : 0u - off;
        op->kind = CI_MEM_KIND(acc, CI_S_IMM, mode);
    } else {
        op->rm = (uint8_t)rm;
        op->sh = (uint8_t)(CI_SH_LSL | (U ? 0u : CI_SH_SUB));
        op->sa = 0u;
        op->kind = CI_MEM_KIND(acc, CI_S_REG, mode);
    }
    return CI_DEC_OP;
}

static ci_dec_t decode_media(uint32_t insn, ci_op_t *op) {
    if ((insn & 0x0f8003f0u) == 0x06800070u) {           /* extend family */
        unsigned opx = (insn >> 20) & 0xfu, rn = (insn >> 16) & 0xfu;
        unsigned rd = (insn >> 12) & 0xfu, rm = insn & 0xfu;
        if (rd == 15u || rm == 15u || rn != 15u) return ref(op, false);
        switch (opx) {
            case 0xa: op->kind = CI_K_SXTB; break;
            case 0xb: op->kind = CI_K_SXTH; break;
            case 0xe: op->kind = CI_K_UXTB; break;
            case 0xf: op->kind = CI_K_UXTH; break;
            default:  return ref(op, false);
        }
        op->rd = (uint8_t)rd; op->rm = (uint8_t)rm;
        op->sa = (uint8_t)(((insn >> 10) & 3u) * 8u);
        return CI_DEC_OP;
    }
    {
        unsigned rd = (insn >> 12) & 0xfu, rm = insn & 0xfu;
        uint8_t k = 0;
        if ((insn & 0x0fff0ff0u) == 0x06bf0f30u) k = CI_K_REV;
        else if ((insn & 0x0fff0ff0u) == 0x06bf0fb0u) k = CI_K_REV16;
        else if ((insn & 0x0fff0ff0u) == 0x06ff0fb0u) k = CI_K_REVSH;
        if (k && rd != 15u && rm != 15u) {
            op->kind = k; op->rd = (uint8_t)rd; op->rm = (uint8_t)rm;
            return CI_DEC_OP;
        }
    }
    return ref(op, false);
}

/* A cp10/cp11 instruction: CI_K_VFP, upgraded to one of the decoded-once
 * forms when it is one (see CI_K_VFP_DP). Every refusal the reference makes
 * at decode time -- PC as a VMOV/VMSR register, d16-d31 -- stays CI_K_VFP,
 * so the fast forms only ever gate on run-time state. */
static ci_dec_t decode_vfp(uint32_t pc, uint32_t insn, ci_op_t *op) {
    unsigned form, d, n, m;
    bool dbl;
    const unsigned rt = (insn >> 12) & 0xfu;
    op->kind = CI_K_VFP;
    if (vfp_fast_decode_dp(insn, &form, &dbl, &d, &n, &m)) {
        op->kind = CI_K_VFP_DP;
        op->sa = (uint8_t)form;
        op->sh = dbl ? 1u : 0u;
        op->rd = (uint8_t)d; op->rn = (uint8_t)n; op->rm = (uint8_t)m;
    } else if ((insn & 0x0fe00f7fu) == 0x0e000a10u && rt != 15u) {     /* VMOV Sn, Rt */
        op->kind = CI_K_VFP_MOV;
        op->rd = (uint8_t)rt;
        op->rn = (uint8_t)((((insn >> 16) & 0xfu) << 1) | ((insn >> 7) & 1u));
        op->sh = (uint8_t)((insn >> 20) & 1u);
    } else if ((insn & 0x0fef0fffu) == 0x0ee10a10u &&                  /* VMRS/VMSR FPSCR */
               (((insn >> 20) & 1u) || rt != 15u)) {
        op->kind = CI_K_VFP_SYS;
        op->rd = (uint8_t)rt;
        op->sh = (uint8_t)((insn >> 20) & 1u);
    } else if ((insn & 0x0f200e00u) == 0x0d000a00u &&                  /* VLDR/VSTR */
               !(((insn >> 8) & 1u) && ((insn >> 22) & 1u))) {
        const bool wide = (insn >> 8) & 1u, up = (insn >> 23) & 1u;
        const unsigned vd = (insn >> 12) & 0xfu, rn = (insn >> 16) & 0xfu;
        const uint32_t off = (insn & 0xffu) * 4u;
        op->kind = CI_K_VFP_LS;
        op->rd = (uint8_t)(wide ? vd * 2u : (vd << 1) | ((insn >> 22) & 1u));
        op->rn = (uint8_t)rn;
        op->sh = (uint8_t)(((insn >> 20) & 1u) | (wide ? 2u : 0u) | (rn == 15u ? 4u : 0u));
        if (rn == 15u) op->imm = up ? pc + 8u + off : pc + 8u - off;
        else           op->imm = up ? off : 0u - off;
    }
    return CI_DEC_OP;
}

ci_dec_t ci_decode_arm(uint32_t pc, uint32_t insn, bool v7, ci_op_t *op) {
    memset(op, 0, sizeof *op);
    op->raw = insn;
    op->cond = (uint8_t)(insn >> 28);

    if (op->cond == 0xfu) {                               /* unconditional space */
        op->cond = CI_COND_AL;
        if (insn == 0xf57ff01fu) { op->kind = CI_K_CLREX; return CI_DEC_OP; }
        if ((insn & 0x0c00f000u) == 0x0400f000u) { op->kind = CI_K_NOP; return CI_DEC_OP; }
        if ((insn & 0xfe000000u) == 0xfa000000u) {        /* BLX <imm> */
            int32_t off = (int32_t)(insn << 8) >> 6;
            op->kind = CI_K_BLX_I;
            op->imm = (pc + 8u + (uint32_t)off + (((insn >> 24) & 1u) << 1)) & ~1u;
            return CI_DEC_END;
        }
        /* CPS: the reference runs it in-block; a mode change still ends the
         * run there, and a mask-only change continues unless it makes an
         * interrupt deliverable (arm_ci.c, the ref path). */
        if ((insn & 0xfff1fe20u) == 0xf1000000u) return ref(op, false);
        return CI_DEC_STOP;           /* SRS, RFE, SETEND, SIMD, undefined */
    }

    /* Hoisted data processing, exactly arm_step's guard. */
    if ((insn & 0x0c000000u) == 0x00000000u &&
        (insn & 0x01900000u) != 0x01000000u &&
        !((insn & 0x0e000000u) == 0x00000000u &&
          (insn & 0x00000090u) == 0x00000090u))
        return decode_dp(pc, insn, v7, op);

    if ((insn & 0x0e000000u) == 0x0a000000u) {            /* B / BL */
        int32_t off = (int32_t)(insn << 8) >> 6;
        op->kind = (insn & (1u << 24)) ? CI_K_BL : CI_K_B;
        op->imm = (pc + 8u + (uint32_t)off) & ~3u;
        return CI_DEC_END;
    }
    if ((insn & 0x0fc000f0u) == 0x00000090u) {            /* MUL / MLA */
        bool A = (insn >> 21) & 1u, S = (insn >> 20) & 1u;
        unsigned rd = (insn >> 16) & 0xfu, rn = (insn >> 12) & 0xfu;
        unsigned rs = (insn >> 8) & 0xfu, rm = insn & 0xfu;
        if (rd == 15u || rs == 15u || rm == 15u || (A && rn == 15u))
            return ref(op, false);
        op->kind = A ? (S ? CI_K_MLAS : CI_K_MLA) : (S ? CI_K_MULS : CI_K_MUL);
        op->rd = (uint8_t)rd; op->rn = (uint8_t)rn;
        op->rs = (uint8_t)rs; op->rm = (uint8_t)rm;
        return CI_DEC_OP;
    }
    if ((insn & 0x0f8000f0u) == 0x00800090u) {            /* long multiplies */
        bool sgn = (insn >> 22) & 1u, A = (insn >> 21) & 1u, S = (insn >> 20) & 1u;
        unsigned hi = (insn >> 16) & 0xfu, lo = (insn >> 12) & 0xfu;
        unsigned rs = (insn >> 8) & 0xfu, rm = insn & 0xfu;
        if (S || hi == 15u || lo == 15u || rs == 15u || rm == 15u || hi == lo)
            return ref(op, false);
        op->kind = sgn ? (A ? CI_K_SMLAL : CI_K_SMULL) : (A ? CI_K_UMLAL : CI_K_UMULL);
        op->rd = (uint8_t)hi; op->rn = (uint8_t)lo;
        op->rs = (uint8_t)rs; op->rm = (uint8_t)rm;
        return CI_DEC_OP;
    }
    if ((insn & 0x0f900090u) == 0x01000080u) return ref(op, false);   /* DSP mul */
    if ((insn & 0x0e000000u) == 0x06000000u && (insn & 0x10u))
        return decode_media(insn, op);
    if ((insn & 0x0c000000u) == 0x04000000u) return decode_single(pc, insn, op);
    if ((insn & 0x0e000000u) == 0x08000000u)              /* LDM / STM */
        return decode_block(insn, op);
    if ((insn & 0x0e000000u) == 0x00000000u &&
        (insn & 0x00000090u) == 0x00000090u &&
        (insn & 0x00000060u) != 0x00000000u)
        return decode_extra(insn, op);
    if ((insn & 0x0fb00ff0u) == 0x01000090u ||            /* SWP/SWPB */
        (insn & 0x0f800ff0u) == 0x01800f90u)              /* LDREX/STREX family */
        return ref(op, false);
    if ((insn & 0x0ff000f0u) == 0x00400090u ||            /* UMAAL */
        (v7 && (insn & 0x0ff000f0u) == 0x00600090u))      /* MLS (ARMv6T2) */
        return ref(op, false);
    if ((insn & 0x0e000000u) == 0x00000000u &&
        (insn & 0x00000090u) == 0x00000090u)
        return CI_DEC_STOP;                               /* reserved: UNDEFINED */
    if ((insn & 0x0f000010u) == 0x0e000010u) {            /* MCR / MRC */
        unsigned cp = (insn >> 8) & 0xfu;
        if (cp == 10u || cp == 11u) return decode_vfp(pc, insn, op);
        if (cp == 15u) {
            const bool L = (insn >> 20) & 1u;
            const unsigned opc1 = (insn >> 21) & 7u, crn = (insn >> 16) & 0xfu;
            const unsigned rd = (insn >> 12) & 0xfu, opc2 = (insn >> 5) & 7u;
            const unsigned crm = insn & 0xfu;
            /* c7 writes other than WFI (MCR p15,0,Rd,c7,c0,4): the memory
             * barriers and cache maintenance, a no-op in the reference in
             * every mode (User mode may issue c7). */
            if (!L && crn == 7u && !(opc1 == 0u && opc2 == 4u && crm == 0u)) {
                op->kind = CI_K_NOP;
                return CI_DEC_OP;
            }
            /* c13 opc2 2..4: TPIDRURW/URO/PRW, plain registers in the
             * reference. Opc2 1 (CONTEXTIDR) flushes the TLB and stays on
             * arm_step, as does anything naming r15. */
            if (crn == 13u && rd != 15u && opc2 >= 2u && opc2 <= 4u) {
                op->kind = L ? CI_K_MRC_TID : CI_K_MCR_TID;
                op->rd = (uint8_t)rd;
                op->sa = (uint8_t)opc2;
                op->rm = (uint8_t)crm;
                return CI_DEC_OP;
            }
        }
        return CI_DEC_STOP;
    }
    if ((insn & 0x0f000e10u) == 0x0e000a00u ||            /* VFP CDP */
        (insn & 0x0e000e00u) == 0x0c000a00u)              /* VFP LDC/STC/MCRR */
        return decode_vfp(pc, insn, op);
    if ((insn & 0x0f000000u) == 0x0f000000u) return CI_DEC_STOP;       /* SVC */
    if ((insn & 0x0c000000u) == 0x00000000u) return decode_dp(pc, insn, v7, op);
    return CI_DEC_STOP;
}

/* -------------------------------------------------------------- Thumb --- */

#define TB(n) ((unsigned)((insn >> (n)) & 7u))

static ci_dec_t thumb_dp(ci_op_t *op, unsigned opc, unsigned form, unsigned S,
                         unsigned rd, unsigned rn, unsigned rm, uint32_t imm) {
    op->kind = CI_DP_KIND(opc, form, S);
    op->rd = (uint8_t)rd; op->rn = (uint8_t)rn; op->rm = (uint8_t)rm;
    op->imm = imm;
    return CI_DEC_OP;
}

ci_dec_t ci_decode_thumb(uint32_t pc, uint16_t insn, ci_op_t *op) {
    const uint32_t pc4 = pc + 4u;
    memset(op, 0, sizeof *op);
    op->raw = insn;
    op->cond = CI_COND_AL;

    switch (insn >> 12) {
    case 0x0: case 0x1:
        if ((insn & 0xf800u) == 0x1800u) {                 /* ADD/SUB reg/imm3 */
            unsigned opc = (insn & (1u << 9)) ? 2u : 4u;    /* SUB : ADD */
            if (insn & (1u << 10))
                return thumb_dp(op, opc, CI_F_IMM, 1u, TB(0), TB(3), 0u, TB(6));
            return thumb_dp(op, opc, CI_F_REG, 1u, TB(0), TB(3), TB(6), 0u);
        }
        /* LSL/LSR/ASR #imm == MOVS Rd, Rm, <shift> #imm */
        if (norm_imm_shift((insn >> 11) & 3u, (insn >> 6) & 0x1fu, &op->sh, &op->sa))
            return thumb_dp(op, 13u, CI_F_SHI, 1u, TB(0), 0u, TB(3), 0u);
        return thumb_dp(op, 13u, CI_F_REG, 1u, TB(0), 0u, TB(3), 0u);
    case 0x2: case 0x3: {                                  /* MOV/CMP/ADD/SUB imm8 */
        unsigned rd = (insn >> 8) & 7u;
        static const uint8_t opcs[4] = { 13u, 10u, 4u, 2u };
        return thumb_dp(op, opcs[(insn >> 11) & 3u], CI_F_IMM, 1u, rd, rd, 0u,
                        insn & 0xffu);
    }
    case 0x4:
        if ((insn & 0xfc00u) == 0x4000u) {                 /* ALU operations */
            unsigned rd = TB(0), rs = TB(3);
            switch ((insn >> 6) & 0xfu) {
                case 0x0: return thumb_dp(op, 0u,  CI_F_REG, 1u, rd, rd, rs, 0u);
                case 0x1: return thumb_dp(op, 1u,  CI_F_REG, 1u, rd, rd, rs, 0u);
                case 0x2: case 0x3: case 0x4: case 0x7: {  /* shifts by Rs */
                    static const uint8_t t[8] = { 0, 0, CI_SH_LSL, CI_SH_LSR,
                                                  CI_SH_ASR, 0, 0, CI_SH_ROR };
                    op->sh = t[(insn >> 6) & 7u];
                    op->rs = (uint8_t)rs;
                    return thumb_dp(op, 13u, CI_F_SHR, 1u, rd, 0u, rd, 0u);
                }
                case 0x5: return thumb_dp(op, 5u,  CI_F_REG, 1u, rd, rd, rs, 0u);
                case 0x6: return thumb_dp(op, 6u,  CI_F_REG, 1u, rd, rd, rs, 0u);
                case 0x8: return thumb_dp(op, 8u,  CI_F_REG, 1u, 0u, rd, rs, 0u);
                case 0x9: return thumb_dp(op, 3u,  CI_F_IMM, 1u, rd, rs, 0u, 0u); /* NEG */
                case 0xa: return thumb_dp(op, 10u, CI_F_REG, 1u, 0u, rd, rs, 0u);
                case 0xb: return thumb_dp(op, 11u, CI_F_REG, 1u, 0u, rd, rs, 0u);
                case 0xc: return thumb_dp(op, 12u, CI_F_REG, 1u, rd, rd, rs, 0u);
                case 0xd: op->kind = CI_K_MULS; op->rd = (uint8_t)rd;
                          op->rm = (uint8_t)rd; op->rs = (uint8_t)rs;
                          return CI_DEC_OP;
                case 0xe: return thumb_dp(op, 14u, CI_F_REG, 1u, rd, rd, rs, 0u);
                default:  return thumb_dp(op, 15u, CI_F_REG, 1u, rd, 0u, rs, 0u);
            }
        }
        if ((insn & 0xfc00u) == 0x4400u) {                 /* hi-register ops */
            unsigned rd = TB(0) | ((insn >> 4) & 8u);
            unsigned rs = TB(3) | ((insn >> 3) & 8u);
            /* Reading the PC here gives the instruction's address + 4, not
             * word aligned; writing it branches within Thumb state. */
            switch ((insn >> 8) & 3u) {
                case 0:                                     /* ADD */
                    if (rd == 15u) {
                        if (rs == 15u) {
                            op->kind = CI_K_B; op->imm = (pc4 + pc4) & ~1u;
                            return CI_DEC_END;
                        }
                        op->kind = CI_K_JMP; op->rm = (uint8_t)rs; op->imm = pc4;
                        return CI_DEC_END;
                    }
                    if (rs == 15u)                           /* PIC: ADD Rd, PC */
                        return thumb_dp(op, 4u, CI_F_IMM, 0u, rd, rd, 0u, pc4);
                    return thumb_dp(op, 4u, CI_F_REG, 0u, rd, rd, rs, 0u);
                case 1:
                    if (rd == 15u || rs == 15u) return ref(op, false);
                    return thumb_dp(op, 10u, CI_F_REG, 1u, 0u, rd, rs, 0u);
                case 2:                                     /* MOV */
                    if (rd == 15u) {
                        if (rs == 15u) {
                            op->kind = CI_K_B; op->imm = pc4 & ~1u;
                            return CI_DEC_END;
                        }
                        op->kind = CI_K_JMP; op->rm = (uint8_t)rs; op->imm = 0u;
                        return CI_DEC_END;
                    }
                    if (rs == 15u)
                        return thumb_dp(op, 13u, CI_F_IMM, 0u, rd, 0u, 0u, pc4);
                    return thumb_dp(op, 13u, CI_F_REG, 0u, rd, 0u, rs, 0u);
                default:
                    if (rs == 15u) return ref(op, true);
                    op->rm = (uint8_t)rs;
                    if (insn & (1u << 7)) {
                        op->kind = CI_K_BLX_R; op->imm = (pc + 2u) | 1u;
                    } else {
                        op->kind = CI_K_BX;
                    }
                    return CI_DEC_END;
            }
        }
        op->kind = CI_K_LDR_LIT;                            /* LDR Rd,[PC,#imm8*4] */
        op->rd = (uint8_t)((insn >> 8) & 7u);
        op->imm = (pc4 & ~3u) + ((uint32_t)(insn & 0xffu) << 2);
        return CI_DEC_OP;
    case 0x5: {                                            /* register offset */
        unsigned acc;
        if (insn & (1u << 9)) {
            static const uint8_t a[4] = { CI_A_STRH, CI_A_LDRSB, CI_A_LDRH, CI_A_LDRSH };
            acc = a[(insn >> 10) & 3u];
        } else {
            bool load = (insn >> 11) & 1u, byte = (insn >> 10) & 1u;
            acc = load ? (byte ? CI_A_LDRB : CI_A_LDR) : (byte ? CI_A_STRB : CI_A_STR);
        }
        op->kind = CI_MEM_KIND(acc, CI_S_REG, CI_M_OFF);
        op->rd = (uint8_t)TB(0); op->rn = (uint8_t)TB(3); op->rm = (uint8_t)TB(6);
        op->sh = CI_SH_LSL; op->sa = 0u;
        return CI_DEC_OP;
    }
    case 0x6: case 0x7: {                                  /* word/byte imm5 */
        bool byte = (insn >> 12) & 1u, load = (insn >> 11) & 1u;
        uint32_t off = (insn >> 6) & 0x1fu;
        unsigned acc = load ? (byte ? CI_A_LDRB : CI_A_LDR) : (byte ? CI_A_STRB : CI_A_STR);
        op->kind = CI_MEM_KIND(acc, CI_S_IMM, CI_M_OFF);
        op->rd = (uint8_t)TB(0); op->rn = (uint8_t)TB(3);
        op->imm = byte ? off : off << 2;
        return CI_DEC_OP;
    }
    case 0x8:                                              /* halfword imm5 */
        op->kind = CI_MEM_KIND((insn & (1u << 11)) ? CI_A_LDRH : CI_A_STRH,
                               CI_S_IMM, CI_M_OFF);
        op->rd = (uint8_t)TB(0); op->rn = (uint8_t)TB(3);
        op->imm = ((insn >> 6) & 0x1fu) << 1;
        return CI_DEC_OP;
    case 0x9:                                              /* SP-relative */
        op->kind = CI_MEM_KIND((insn & (1u << 11)) ? CI_A_LDR : CI_A_STR,
                               CI_S_IMM, CI_M_OFF);
        op->rd = (uint8_t)((insn >> 8) & 7u); op->rn = 13u;
        op->imm = (uint32_t)(insn & 0xffu) << 2;
        return CI_DEC_OP;
    case 0xa: {                                            /* ADD Rd, PC/SP, #imm */
        unsigned rd = (insn >> 8) & 7u;
        uint32_t imm = (uint32_t)(insn & 0xffu) << 2;
        if (insn & (1u << 11)) return thumb_dp(op, 4u, CI_F_IMM, 0u, rd, 13u, 0u, imm);
        return thumb_dp(op, 13u, CI_F_IMM, 0u, rd, 0u, 0u, (pc4 & ~3u) + imm);
    }
    case 0xb:
        if ((insn & 0xff00u) == 0xb000u)                   /* ADD/SUB SP, #imm7*4 */
            return thumb_dp(op, (insn & 0x80u) ? 2u : 4u, CI_F_IMM, 0u, 13u, 13u, 0u,
                            (uint32_t)(insn & 0x7fu) << 2);
        if ((insn & 0xf600u) == 0xb400u) {                 /* PUSH / POP */
            bool load = (insn >> 11) & 1u, extra = (insn >> 8) & 1u;
            uint32_t list = insn & 0xffu;
            if (list == 0u && !extra) return ref(op, false);
            int n = extra ? 1 : 0;
            for (uint32_t l = list; l; l &= l - 1u) n++;
            if (load)                                       /* LDMIA sp!, {.., pc} */
                return block_transfer(op, true, 13u, list | (extra ? 0x8000u : 0u),
                                      0, 4 * n);
            return block_transfer(op, false, 13u,           /* STMDB sp!, {.., lr} */
                                  list | (extra ? 0x4000u : 0u), -4 * n, -4 * n);
        }
        if ((insn & 0xff00u) == 0xb200u) {                 /* SXTH/SXTB/UXTH/UXTB */
            static const uint8_t k[4] = { CI_K_SXTH, CI_K_SXTB, CI_K_UXTH, CI_K_UXTB };
            op->kind = k[(insn >> 6) & 3u];
            op->rd = (uint8_t)TB(0); op->rm = (uint8_t)TB(3); op->sa = 0u;
            return CI_DEC_OP;
        }
        if ((insn & 0xff00u) == 0xba00u) {                 /* REV family */
            static const uint8_t k[4] = { CI_K_REV, CI_K_REV16, 0, CI_K_REVSH };
            unsigned sel = (insn >> 6) & 3u;
            if (sel == 2u) return CI_DEC_STOP;
            op->kind = k[sel]; op->rd = (uint8_t)TB(0); op->rm = (uint8_t)TB(3);
            return CI_DEC_OP;
        }
        if ((insn & 0xffe0u) == 0xb660u || (insn & 0xfff7u) == 0xb650u)
            return ref(op, false);                          /* CPS, SETEND */
        return CI_DEC_STOP;                                 /* BKPT, undefined */
    case 0xc: {                                            /* LDMIA/STMIA Rb! */
        unsigned rb = (insn >> 8) & 7u;
        uint32_t list = insn & 0xffu;
        bool load = (insn >> 11) & 1u, in = (list >> rb) & 1u;
        if (list == 0u) return ref(op, false);
        /* STMIA stores Rb's old value only when it is the lowest register. */
        if (!load && in && (list & ((1u << rb) - 1u))) return ref(op, false);
        int n = 0;
        for (uint32_t l = list; l; l &= l - 1u) n++;
        /* LDMIA with Rb in the list: the loaded value wins, no writeback. */
        return block_transfer(op, load, rb, list, 0, (load && in) ? 0 : 4 * n);
    }
    case 0xd: {
        unsigned cond = (insn >> 8) & 0xfu;
        if (cond >= 0xeu) return CI_DEC_STOP;               /* undefined / SWI */
        op->kind = CI_K_B; op->cond = (uint8_t)cond;
        op->imm = pc4 + ((uint32_t)(int32_t)(int8_t)(insn & 0xffu) << 1);
        return CI_DEC_END;
    }
    case 0xe:
        if (insn & 0x0800u) return ref(op, true);           /* BLX suffix */
        op->kind = CI_K_B;
        op->imm = pc4 + (uint32_t)((int32_t)((uint32_t)(insn & 0x7ffu) << 21) >> 20);
        return CI_DEC_END;
    default:                                               /* BL/BLX pair */
        if (!(insn & (1u << 11))) {                         /* prefix: LR = const */
            int32_t off = (int32_t)((uint32_t)(insn & 0x7ffu) << 21) >> 9;
            return thumb_dp(op, 13u, CI_F_IMM, 0u, 14u, 0u, 0u, pc4 + (uint32_t)off);
        }
        op->kind = CI_K_TBL2;
        op->imm = (uint32_t)(insn & 0x7ffu) << 1;
        return CI_DEC_END;
    }
}

#undef TB

/* ----------------------------------------------------- ARMv7 Thumb --- */

/*
 * The ARMv7 Thumb decoder. What it specialises it maps onto the records the
 * ARM and ARM1176 Thumb decoders already produce wherever the operation is
 * the same, so the executor's handlers (and their fallbacks) are shared: the
 * data-processing, memory and block-transfer families, the multiplies, the
 * extends and byte reverses, B with a condition, LDR literal. What it cannot
 * express exactly -- ORN, PKH, bit-field and saturating ops, LDRD/STRD, the
 * exclusives, TBB/TBH, anything with PC in an unusual place, anything the
 * reference refuses -- is a CI_K_REF record, and the reference decides.
 *
 * Every instruction an IT covers, and the IT itself, is a reference record:
 * the reference keeps ITSTATE exactly, and a specialised record never runs
 * with ITSTATE set (the engine also refuses to enter a block inside an IT
 * block; see arm_ci_run). Such records are marked rd = 1, which the executor
 * uses to leave the block if ITSTATE turns out live anywhere else.
 */

static uint32_t t2_raw(uint16_t hw1, uint16_t hw2) {
    return (uint32_t)hw1 | ((uint32_t)hw2 << 16);
}

/* Must run on arm_step: CP14/CP15 in any form, and WFI (it waits, which
 * advances device time). SVC is 16-bit only and handled by ci_decode_thumb. */
static bool t2_must_stop(uint16_t hw1, uint16_t hw2) {
    if ((hw1 & 0xec00u) == 0xec00u) {                     /* coprocessor space */
        const unsigned cp = (hw2 >> 8) & 0xfu;
        return (hw1 & 0xef00u) != 0xef00u && (cp == 14u || cp == 15u);
    }
    return hw1 == 0xf3afu && hw2 == 0x8003u;              /* WFI.W */
}

/* Thumb data-processing opcode (hw1[8:5], both immediate and register
 * forms) to the ARM opcode the DP records use, or -1 when there is none:
 * ORN has no ARM form, and PKH lives in the same space. rd == 15 with S is
 * the test form of AND/EOR/ADD/SUB; rn == 15 the move form of ORR/ORN. */
static int t2_dp_opcode(unsigned op, bool test, bool move) {
    switch (op) {
        case 0x0: return test ? 8 : 0;          /* TST / AND */
        case 0x1: return 14;                    /* BIC       */
        case 0x2: return move ? 13 : 12;        /* MOV / ORR */
        case 0x3: return move ? 15 : -1;        /* MVN / ORN */
        case 0x4: return test ? 9 : 1;          /* TEQ / EOR */
        case 0x8: return test ? 11 : 4;         /* CMN / ADD */
        case 0xa: return 5;                     /* ADC       */
        case 0xb: return 6;                     /* SBC       */
        case 0xd: return test ? 10 : 2;         /* CMP / SUB */
        case 0xe: return 3;                     /* RSB       */
        default:  return -1;
    }
}

static ci_dec_t t2_dp(ci_op_t *op, uint16_t hw1, uint16_t hw2, bool imm_form,
                      uint32_t imm, bool rotated) {
    const unsigned op4 = (hw1 >> 5) & 0xfu, S = (hw1 >> 4) & 1u;
    const unsigned rn = hw1 & 0xfu, rd = (hw2 >> 8) & 0xfu, rm = hw2 & 0xfu;
    const bool test = rd == 15u && S &&
                      (op4 == 0x0u || op4 == 0x4u || op4 == 0x8u || op4 == 0xdu);
    const bool move = rn == 15u && (op4 == 0x2u || op4 == 0x3u);
    const int opc = t2_dp_opcode(op4, test, move);
    if (opc < 0 || (rd == 15u && !test) || (rn == 15u && !move))
        return ref(op, false);
    unsigned form = CI_F_IMM;
    if (imm_form) {
        op->imm = imm;
        if (rotated) { op->sa = 1u; op->sh = (uint8_t)(imm >> 31); }
    } else {
        const unsigned imm5 = ((hw2 >> 10) & 0x1cu) | ((hw2 >> 6) & 3u);
        if (rm == 15u) return ref(op, false);
        form = norm_imm_shift((hw2 >> 4) & 3u, imm5, &op->sh, &op->sa) ? CI_F_SHI
                                                                       : CI_F_REG;
        op->rm = (uint8_t)rm;
    }
    op->kind = CI_DP_KIND((unsigned)opc, form, S);
    op->rd = (uint8_t)(test ? 0u : rd);
    op->rn = (uint8_t)(move ? 0u : rn);
    return CI_DEC_OP;
}

/* ThumbExpandImm_C. False for the UNPREDICTABLE zero replications. */
static bool t2_expand_imm(uint32_t imm12, uint32_t *out, bool *rotated) {
    const uint32_t imm8 = imm12 & 0xffu;
    *rotated = (imm12 & 0xc00u) != 0u;
    if (*rotated) {
        *out = ror32(0x80u | (imm12 & 0x7fu), (imm12 >> 7) & 0x1fu);
        return true;
    }
    switch ((imm12 >> 8) & 3u) {
        case 0:  *out = imm8; return true;
        case 1:  *out = imm8 | (imm8 << 16); break;
        case 2:  *out = (imm8 << 8) | (imm8 << 24); break;
        default: *out = imm8 * 0x01010101u; break;
    }
    return imm8 != 0u;
}

static ci_dec_t t2_plain_imm(uint32_t pc, ci_op_t *op, uint16_t hw1, uint16_t hw2) {
    const unsigned op5 = (hw1 >> 4) & 0x1fu, rn = hw1 & 0xfu;
    const unsigned rd = (hw2 >> 8) & 0xfu;
    const uint32_t imm12 = ((uint32_t)(hw1 & 0x400u) << 1) |
                           ((uint32_t)(hw2 >> 4) & 0x700u) | (hw2 & 0xffu);
    if (rd == 15u) return ref(op, false);
    switch (op5) {
        case 0x00: case 0x0a:                              /* ADDW / SUBW / ADR */
            if (rn == 15u) {
                const uint32_t base = (pc + 4u) & ~3u;
                op->kind = CI_DP_KIND(13u, CI_F_IMM, 0u);
                op->imm = op5 ? base - imm12 : base + imm12;
            } else {
                op->kind = CI_DP_KIND(op5 ? 2u : 4u, CI_F_IMM, 0u);
                op->rn = (uint8_t)rn;
                op->imm = imm12;
            }
            op->rd = (uint8_t)rd;
            return CI_DEC_OP;
        case 0x04:                                          /* MOVW */
            op->kind = CI_DP_KIND(13u, CI_F_IMM, 0u);
            op->rd = (uint8_t)rd;
            op->imm = ((uint32_t)(hw1 & 0xfu) << 12) | imm12;
            return CI_DEC_OP;
        case 0x0c:                                          /* MOVT */
            op->kind = CI_K_MOVT;
            op->rd = (uint8_t)rd;
            op->imm = (((uint32_t)(hw1 & 0xfu) << 12) | imm12) << 16;
            return CI_DEC_OP;
        default:
            return ref(op, false);
    }
}

static ci_dec_t t2_branch_misc(uint32_t pc, ci_op_t *op, uint16_t hw1, uint16_t hw2) {
    const unsigned op1 = (hw2 >> 12) & 7u, op7 = (hw1 >> 4) & 0x7fu;
    const uint32_t S = (hw1 >> 10) & 1u;
    const uint32_t J1 = (hw2 >> 13) & 1u, J2 = (hw2 >> 11) & 1u;
    if (op1 & 5u) {                                         /* B.W, BL, BLX */
        const uint32_t I1 = (J1 ^ S) ^ 1u, I2 = (J2 ^ S) ^ 1u;
        const uint32_t imm = (S << 24) | (I1 << 23) | (I2 << 22) |
                             ((uint32_t)(hw1 & 0x3ffu) << 12) |
                             ((uint32_t)(hw2 & 0x7ffu) << 1);
        const uint32_t off = (imm ^ 0x01000000u) - 0x01000000u;
        switch (op1 & 5u) {
            case 1:  op->kind = CI_K_B;   op->imm = pc + 4u + off; break;
            case 5:  op->kind = CI_K_TBL; op->imm = pc + 4u + off; break;
            default:
                if (hw2 & 1u) return ref(op, true);
                op->kind = CI_K_TBLX;
                op->imm = ((pc + 4u) & ~3u) + off;
                break;
        }
        return CI_DEC_END;
    }
    if ((op7 & 0x38u) != 0x38u) {                           /* B<c>.W */
        const uint32_t imm = (S << 20) | (J2 << 19) | (J1 << 18) |
                             ((uint32_t)(hw1 & 0x3fu) << 12) |
                             ((uint32_t)(hw2 & 0x7ffu) << 1);
        op->kind = CI_K_B;
        op->cond = (uint8_t)((hw1 >> 6) & 0xfu);
        op->imm = pc + 4u + ((imm ^ 0x00100000u) - 0x00100000u);
        return CI_DEC_END;
    }
    /* Exactly the encodings the reference accepts as a hint, CLREX or a
     * barrier; every other system form is the reference's. */
    if (hw1 == 0xf3afu && hw2 <= 0x8004u) {                /* NOP YIELD WFE SEV */
        op->kind = CI_K_NOP;                                /* (WFI stopped above) */
        return CI_DEC_OP;
    }
    if (hw1 == 0xf3bfu && (hw2 & 0xff00u) == 0x8f00u) {
        const unsigned sel = (hw2 >> 4) & 0xfu;
        if (sel == 2u && (hw2 & 0xfu) == 0xfu) { op->kind = CI_K_CLREX; return CI_DEC_OP; }
        if (sel >= 4u && sel <= 6u) { op->kind = CI_K_NOP; return CI_DEC_OP; }
    }
    return ref(op, false);
}

static ci_dec_t t2_load_store(uint32_t pc, ci_op_t *op, uint16_t hw1, uint16_t hw2) {
    const bool load = (hw1 >> 4) & 1u, sgn = (hw1 >> 8) & 1u;
    const unsigned size = (hw1 >> 5) & 3u, rn = hw1 & 0xfu, rt = hw2 >> 12;
    unsigned acc, src = CI_S_IMM, mode = CI_M_OFF;
    bool hint_ok;

    if (size == 3u || (sgn && size == 2u) || (!load && sgn)) return ref(op, false);
    if (load) {
        static const uint8_t u[3] = { CI_A_LDRB, CI_A_LDRH, CI_A_LDR };
        static const uint8_t s[2] = { CI_A_LDRSB, CI_A_LDRSH };
        acc = sgn ? s[size] : u[size];
    } else {
        static const uint8_t st[3] = { CI_A_STRB, CI_A_STRH, CI_A_STR };
        acc = st[size];
    }
    if (rn == 15u) {                                        /* literal */
        const uint32_t base = (pc + 4u) & ~3u, imm = hw2 & 0xfffu;
        if (!load) return ref(op, false);
        if (rt == 15u) {
            if (size == 2u) return ref(op, true);            /* LDR pc: interworks */
            op->kind = CI_K_NOP;                             /* PLD / PLI literal */
            return CI_DEC_OP;
        }
        if (acc != CI_A_LDR) return ref(op, false);
        op->kind = CI_K_LDR_LIT;
        op->rd = (uint8_t)rt;
        op->imm = (hw1 & 0x80u) ? base + imm : base - imm;
        return CI_DEC_OP;
    }
    if (hw1 & 0x80u) {                                      /* [Rn, #imm12] */
        op->imm = hw2 & 0xfffu;
        hint_ok = true;
    } else if (hw2 & 0x800u) {                              /* the imm8 forms */
        const bool P = (hw2 >> 10) & 1u, U = (hw2 >> 9) & 1u, W = (hw2 >> 8) & 1u;
        const uint32_t imm8 = hw2 & 0xffu;
        if ((!P && !W) || (P && U && !W)) return ref(op, false);  /* T forms too */
        if (W && rn == rt) return ref(op, false);
        mode = P ? (W ? CI_M_PRE : CI_M_OFF) : CI_M_POST;
        op->imm = U ? imm8 : 0u - imm8;
        hint_ok = P && !U && !W;
    } else if ((hw2 & 0xfc0u) == 0u) {                      /* [Rn, Rm, LSL #n] */
        const unsigned rm = hw2 & 0xfu;
        if (rm == 13u || rm == 15u) return ref(op, false);
        src = CI_S_REG;
        op->rm = (uint8_t)rm;
        op->sh = CI_SH_LSL;
        op->sa = (uint8_t)((hw2 >> 4) & 3u);
        hint_ok = true;
    } else {
        return ref(op, false);
    }
    if (rt == 15u) {
        if (load && size != 2u && hint_ok) {                 /* PLD, PLDW, PLI */
            op->kind = CI_K_NOP;
            op->imm = 0u; op->rm = 0u; op->sh = 0u; op->sa = 0u;
            return CI_DEC_OP;
        }
        return ref(op, load);
    }
    op->kind = CI_MEM_KIND(acc, src, mode);
    op->rd = (uint8_t)rt;
    op->rn = (uint8_t)rn;
    return CI_DEC_OP;
}

static ci_dec_t t2_block(ci_op_t *op, uint16_t hw1, uint16_t hw2) {
    const unsigned opx = (hw1 >> 7) & 3u, rn = hw1 & 0xfu;
    const bool W = (hw1 >> 5) & 1u, load = (hw1 >> 4) & 1u;
    const uint32_t list = hw2;
    int n = 0;
    for (uint32_t l = list; l; l &= l - 1u) n++;
    /* SRS/RFE, and every form the reference refuses. */
    if (opx == 0u || opx == 3u || rn == 15u || (list & 0x2000u) || n < 2 ||
        (!load && (list & 0x8000u)) || (load && (list & 0xc000u) == 0xc000u) ||
        (W && ((list >> rn) & 1u)))
        return ref(op, load && (list & 0x8000u));
    if (opx == 1u)                                          /* IA */
        return block_transfer(op, load, rn, list, 0, W ? 4 * n : 0);
    return block_transfer(op, load, rn, list, -4 * n, W ? -4 * n : 0);   /* DB */
}

static ci_dec_t t2_dp_register(ci_op_t *op, uint16_t hw1, uint16_t hw2) {
    const unsigned op1 = (hw1 >> 4) & 0xfu, op2 = (hw2 >> 4) & 0xfu;
    const unsigned rn = hw1 & 0xfu, rd = (hw2 >> 8) & 0xfu, rm = hw2 & 0xfu;
    if ((hw2 & 0xf000u) != 0xf000u || rd == 15u || rm == 15u) return ref(op, false);
    if (op1 < 8u && op2 == 0u) {                            /* LSL/LSR/ASR/ROR Rs */
        if (rn == 15u) return ref(op, false);
        op->sh = (uint8_t)(op1 >> 1);
        op->rs = (uint8_t)rm;
        return thumb_dp(op, 13u, CI_F_SHR, op1 & 1u, rd, 0u, rn, 0u);
    }
    if (rn == 15u && (op2 & 0xcu) == 0x8u &&
        (op1 == 0u || op1 == 1u || op1 == 4u || op1 == 5u)) {  /* extends */
        static const uint8_t k[6] = { CI_K_SXTH, CI_K_UXTH, 0, 0, CI_K_SXTB, CI_K_UXTB };
        op->kind = k[op1];
        op->rd = (uint8_t)rd; op->rm = (uint8_t)rm;
        op->sa = (uint8_t)((op2 & 3u) * 8u);
        return CI_DEC_OP;
    }
    if ((op1 & 0xcu) == 0x8u && (op2 & 0xcu) == 0x8u && rn == rm) {
        const unsigned a = op1 & 3u, b = op2 & 3u;
        static const uint8_t rev[4] = { CI_K_REV, CI_K_REV16, 0, CI_K_REVSH };
        if (a == 1u && b != 2u) op->kind = rev[b];
        else if (a == 3u && b == 0u) op->kind = CI_K_CLZ;
        else return ref(op, false);
        op->rd = (uint8_t)rd; op->rm = (uint8_t)rm;
        return CI_DEC_OP;
    }
    return ref(op, false);
}

static ci_dec_t t2_multiply(ci_op_t *op, uint16_t hw1, uint16_t hw2) {
    const unsigned rn = hw1 & 0xfu, rm = hw2 & 0xfu;
    if (!(hw1 & 0x80u)) {                                   /* 32-bit results */
        const unsigned ra = hw2 >> 12, rd = (hw2 >> 8) & 0xfu;
        if ((hw1 & 0x70u) || (hw2 & 0xf0u) || rd == 15u || rn == 15u || rm == 15u)
            return ref(op, false);
        op->kind = ra == 15u ? CI_K_MUL : CI_K_MLA;         /* MUL / MLA */
        op->rd = (uint8_t)rd; op->rm = (uint8_t)rn; op->rs = (uint8_t)rm;
        op->rn = (uint8_t)(ra == 15u ? 0u : ra);
        return CI_DEC_OP;
    }
    const unsigned op1 = (hw1 >> 4) & 7u, lo = hw2 >> 12, hi = (hw2 >> 8) & 0xfu;
    static const uint8_t k[8] = { CI_K_SMULL, 0, CI_K_UMULL, 0, CI_K_SMLAL, 0, CI_K_UMLAL, 0 };
    if ((hw2 & 0xf0u) || !k[op1] || lo == 15u || hi == 15u || lo == hi ||
        rn == 15u || rm == 15u)
        return ref(op, false);
    op->kind = k[op1];
    op->rd = (uint8_t)hi; op->rn = (uint8_t)lo;
    op->rm = (uint8_t)rn; op->rs = (uint8_t)rm;
    return CI_DEC_OP;
}

ci_dec_t ci_decode_thumb2(uint32_t pc, uint16_t hw1, uint16_t hw2, bool in_it,
                          unsigned *it_covers, ci_op_t *op) {
    if (!ci_thumb_is_wide(hw1)) {
        if ((hw1 & 0xff00u) == 0xbf00u) {                   /* IT and the hints */
            memset(op, 0, sizeof *op);
            op->raw = hw1;
            op->cond = CI_COND_AL;
            const unsigned mask = hw1 & 0xfu, sel = (hw1 >> 4) & 0xfu;
            if (mask == 0u) {
                if (sel == 3u) return CI_DEC_STOP;           /* WFI */
                if (!in_it) { op->kind = CI_K_NOP; return CI_DEC_OP; }
            } else if (!in_it) {
                unsigned n = 4u;                             /* 4 - ctz(mask) */
                for (unsigned m = mask; !(m & 1u); m >>= 1) n--;
                *it_covers = n;
            }
            op->rd = 1u;
            return ref(op, false);
        }
        if ((hw1 & 0xff00u) == 0x4700u && (hw1 & 7u)) return CI_DEC_STOP;  /* (0) bits */
        if (!in_it && (hw1 & 0xf500u) == 0xb100u) {          /* CBZ / CBNZ */
            memset(op, 0, sizeof *op);
            op->raw = hw1;
            op->cond = CI_COND_AL;
            op->kind = CI_K_CBZ;
            op->rn = (uint8_t)(hw1 & 7u);
            op->sa = (uint8_t)((hw1 >> 11) & 1u);
            op->imm = pc + 4u + (((uint32_t)hw1 >> 3) & 0x40u) + (((uint32_t)hw1 >> 2) & 0x3eu);
            return CI_DEC_END;
        }
        const ci_dec_t d = ci_decode_thumb(pc, hw1, op);
        if (d == CI_DEC_STOP || !in_it) return d;
        memset(op, 0, sizeof *op);                           /* covered: reference */
        op->raw = hw1;
        op->cond = CI_COND_AL;
        op->rd = 1u;
        return ref(op, false);
    }

    memset(op, 0, sizeof *op);
    op->raw = t2_raw(hw1, hw2);
    op->cond = CI_COND_AL;
    if (t2_must_stop(hw1, hw2)) return CI_DEC_STOP;
    if (in_it) { op->rd = 1u; return ref(op, false); }

    switch ((hw1 >> 11) & 3u) {
    case 1:                                                  /* 11101 */
        if ((hw1 & 0x0600u) == 0u)
            return (hw1 & 0x40u) ? ref(op, false) : t2_block(op, hw1, hw2);
        if ((hw1 & 0x0600u) == 0x0200u) {
            if (hw2 & 0x8000u) return ref(op, false);
            return t2_dp(op, hw1, hw2, false, 0u, false);
        }
        return ref(op, false);                               /* coprocessor */
    case 2: {                                                /* 11110 */
        if (hw2 & 0x8000u) return t2_branch_misc(pc, op, hw1, hw2);
        if (hw1 & 0x0200u) return t2_plain_imm(pc, op, hw1, hw2);
        uint32_t imm;
        bool rotated;
        const uint32_t imm12 = ((uint32_t)(hw1 & 0x400u) << 1) |
                               ((uint32_t)(hw2 >> 4) & 0x700u) | (hw2 & 0xffu);
        if (!t2_expand_imm(imm12, &imm, &rotated)) return ref(op, false);
        return t2_dp(op, hw1, hw2, true, imm, rotated);
    }
    default:                                                 /* 11111 */
        if (hw1 & 0x0400u) return ref(op, false);            /* coprocessor */
        if ((hw1 & 0x0600u) == 0u) return t2_load_store(pc, op, hw1, hw2);
        if ((hw1 & 0x0700u) == 0x0200u) return t2_dp_register(op, hw1, hw2);
        if ((hw1 & 0x0700u) == 0x0300u) return t2_multiply(op, hw1, hw2);
        return ref(op, false);
    }
}

/* ------------------------------------------------------- diagnostics --- */

unsigned ci_stop_cause(uint32_t insn, bool thumb) {
    if (thumb) {
        if ((insn & 0xff00u) == 0xdf00u) return ARM_CI_STEP_SVC;
        return ARM_CI_STEP_OTHER;
    }
    if ((insn >> 28) == 0xfu) return ARM_CI_STEP_OTHER;
    if ((insn & 0x0f000000u) == 0x0f000000u) return ARM_CI_STEP_SVC;
    if ((insn & 0x0c000000u) == 0x0c000000u) {               /* coprocessor */
        const unsigned cp = (insn >> 8) & 0xfu;
        if (cp == 15u) {
            const bool xfer = (insn & 0x0f000010u) == 0x0e000010u;  /* MCR/MRC */
            const bool mcr = ((insn >> 20) & 1u) == 0u;
            const unsigned crn = (insn >> 16) & 0xfu, crm = insn & 0xfu;
            const unsigned opc2 = (insn >> 5) & 7u;
            if (xfer && crn == 13u) return ARM_CI_STEP_CP15_TLS;
            if (xfer && mcr && crn == 7u && crm == 0u && opc2 == 4u)
                return ARM_CI_STEP_WFI;
            return ARM_CI_STEP_CP15;
        }
        if (cp == 14u) return ARM_CI_STEP_CP14;
    }
    return ARM_CI_STEP_OTHER;
}

unsigned ci_stop_cause_v7_arm(uint32_t insn) {
    if ((insn >> 28) != 0xfu && (insn & 0x0fffffffu) == 0x0320f003u)
        return ARM_CI_STEP_WFI;                              /* the WFI hint */
    return ci_stop_cause(insn, false);
}

unsigned ci_stop_cause_t2(uint32_t raw) {
    const uint16_t hw1 = (uint16_t)raw, hw2 = (uint16_t)(raw >> 16);
    if (!ci_thumb_is_wide(hw1)) {
        if (hw1 == 0xbf30u) return ARM_CI_STEP_WFI;
        return ci_stop_cause(hw1, true);
    }
    if (hw1 == 0xf3afu && hw2 == 0x8003u) return ARM_CI_STEP_WFI;
    if ((hw1 & 0xec00u) == 0xec00u && (hw1 & 0xef00u) != 0xef00u) {
        const unsigned cp = (hw2 >> 8) & 0xfu;
        /* The T == 0 coprocessor encodings are the ARM AL word hw1:hw2. */
        if (cp == 15u || cp == 14u)
            return ci_stop_cause(((uint32_t)hw1 << 16) | hw2, false);
    }
    return ARM_CI_STEP_OTHER;
}

unsigned ci_ref_class_t2(uint32_t raw) {
    const uint16_t hw1 = (uint16_t)raw, hw2 = (uint16_t)(raw >> 16);
    if (!ci_thumb_is_wide(hw1)) return ci_ref_class(hw1, true);
    if ((hw1 & 0xec00u) == 0xec00u || (hw1 & 0xff10u) == 0xf900u) {
        const unsigned cp = (hw2 >> 8) & 0xfu;
        return (cp == 10u || cp == 11u || (hw1 & 0xef00u) == 0xef00u ||
                (hw1 & 0xff10u) == 0xf900u) ? ARM_CI_REF_VFP : ARM_CI_REF_OTHER;
    }
    switch ((hw1 >> 11) & 3u) {
    case 1:
        if ((hw1 & 0x0640u) == 0u) return ARM_CI_REF_BLOCK;        /* LDM/STM   */
        if ((hw1 & 0x0640u) == 0x0040u)                             /* TBB/TBH   */
            return (hw1 & 0xfff0u) == 0xe8d0u && (hw2 & 0xffe0u) == 0xf000u
                 ? ARM_CI_REF_PC : ARM_CI_REF_MEM;                  /* else dual/excl */
        return ARM_CI_REF_OTHER;                                    /* ORN, PKH  */
    case 2:
        if (hw2 & 0x8000u) return ARM_CI_REF_STATUS;               /* system    */
        return (hw1 & 0x0200u) ? ARM_CI_REF_MEDIA : ARM_CI_REF_OTHER;  /* bit-field, sat */
    default:
        if ((hw1 & 0x0600u) == 0u)
            return (hw2 >> 12) == 15u ? ARM_CI_REF_PC : ARM_CI_REF_MEM;
        return ARM_CI_REF_MEDIA;                                    /* DSP, mul  */
    }
}

unsigned ci_ref_class(uint32_t insn, bool thumb) {
    if (thumb) {
        const uint32_t t = insn & 0xffffu;
        if ((t & 0xfc00u) == 0x4400u) return ARM_CI_REF_PC;   /* hi regs, BX, BLX */
        if ((t & 0xf600u) == 0xb400u || (t & 0xf000u) == 0xc000u)
            return ARM_CI_REF_BLOCK;                          /* PUSH/POP, LDMIA/STMIA */
        if ((t & 0xf800u) == 0xe800u) return ARM_CI_REF_PC;   /* BLX suffix */
        if ((t & 0xffe0u) == 0xb660u || (t & 0xfff7u) == 0xb650u)
            return ARM_CI_REF_STATUS;                         /* CPS, SETEND */
        if ((t & 0xf800u) == 0x4800u || ((t >> 12) >= 5u && (t >> 12) <= 9u))
            return ARM_CI_REF_MEM;
        return ARM_CI_REF_OTHER;
    }
    if ((insn >> 28) == 0xfu) return ARM_CI_REF_OTHER;
    const unsigned top = (insn >> 25) & 7u;
    const bool rd_pc = ((insn >> 12) & 0xfu) == 15u;
    if ((insn & 0x0c000000u) == 0x0c000000u) {
        const unsigned cp = (insn >> 8) & 0xfu;
        return (cp == 10u || cp == 11u) ? ARM_CI_REF_VFP : ARM_CI_REF_OTHER;
    }
    if (top == 4u) return ARM_CI_REF_BLOCK;
    if (top == 3u && (insn & 0x10u)) return ARM_CI_REF_MEDIA;
    if (top == 2u || top == 3u) return rd_pc ? ARM_CI_REF_PC : ARM_CI_REF_MEM;
    if (top == 0u && (insn & 0x90u) == 0x90u) {
        if ((insn & 0x60u) != 0u) return rd_pc ? ARM_CI_REF_PC : ARM_CI_REF_MEM;
        if ((insn & 0x0f800ff0u) == 0x01800f90u || (insn & 0x0fb00ff0u) == 0x01000090u)
            return ARM_CI_REF_MEM;                            /* exclusives, SWP */
        return ARM_CI_REF_MEDIA;                              /* multiplies */
    }
    const unsigned opc = (insn >> 21) & 0xfu;
    const bool S = (insn >> 20) & 1u;
    if (opc >= 8u && opc <= 11u && !S) {
        if ((insn & 0x0f900090u) == 0x01000080u) return ARM_CI_REF_MEDIA;  /* DSP */
        if ((insn & 0x0ffffff0u) == 0x012fff10u || (insn & 0x0ffffff0u) == 0x012fff30u)
            return ARM_CI_REF_PC;                             /* BX/BLX pc */
        return ARM_CI_REF_STATUS;
    }
    return rd_pc ? ARM_CI_REF_PC : ARM_CI_REF_OTHER;
}
