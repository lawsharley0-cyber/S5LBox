/*
 * S5LBox — Micro-Op Intermediate Representation (IR) Execution Engine.
 *
 * Implements Phase 8 & 9:
 * High-performance execution of optimized micro-op IR blocks. Executes
 * atomic micro-operations with host-RAM fast paths and seamless fallback
 * to reference interpreter for non-accelerated operations.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "arm_ir.h"
#include <string.h>

static inline uint32_t read_vreg(arm_cpu_t *cpu, const uint32_t *tmps,
                                 arm_ir_vreg_t reg, uint32_t imm,
                                 uint32_t guest_pc, bool thumb) {
    if (reg <= VREG_R14) return cpu->r[reg];
    if (reg == VREG_R15) return thumb ? ((guest_pc + 4) & ~3u) : (guest_pc + 8);
    if (reg == VREG_CPSR) return cpu->cpsr;
    if (reg >= VREG_TMP0 && reg <= VREG_TMP7) return tmps[reg - VREG_TMP0];
    if (reg == VREG_CONST) return imm;
    return 0;
}

static inline void write_vreg(arm_cpu_t *cpu, uint32_t *tmps,
                              arm_ir_vreg_t reg, uint32_t val, bool thumb) {
    if (reg <= VREG_R14) {
        cpu->r[reg] = val;
    } else if (reg == VREG_R15) {
        cpu->r[15] = val & (thumb ? ~1u : ~3u);
    } else if (reg == VREG_CPSR) {
        cpu->cpsr = val;
    } else if (reg >= VREG_TMP0 && reg <= VREG_TMP7) {
        tmps[reg - VREG_TMP0] = val;
    }
}

static inline uint32_t ir_read32(arm_cpu_t *c, uint32_t addr) {
    bool priv = (c->cpsr & ARM_CPSR_MODE_MASK) != ARM_MODE_USR;
    return arm_fastmem_read32(c, addr, priv);
}

static inline void ir_write32(arm_ir_block_t *block, arm_cpu_t *c, uint32_t addr, uint32_t val) {
    if (block && block->cache && arm_ir_cache_is_page_code(block->cache, addr)) {
        arm_ir_cache_invalidate_page(block->cache, addr);
    }
    bool priv = (c->cpsr & ARM_CPSR_MODE_MASK) != ARM_MODE_USR;
    arm_fastmem_write32(c, addr, val, priv);
}

static inline uint16_t ir_read16(arm_cpu_t *c, uint32_t addr) {
    bool priv = (c->cpsr & ARM_CPSR_MODE_MASK) != ARM_MODE_USR;
    return arm_fastmem_read16(c, addr, priv);
}

static inline void ir_write16(arm_ir_block_t *block, arm_cpu_t *c, uint32_t addr, uint16_t val) {
    if (block && block->cache && arm_ir_cache_is_page_code(block->cache, addr)) {
        arm_ir_cache_invalidate_page(block->cache, addr);
    }
    bool priv = (c->cpsr & ARM_CPSR_MODE_MASK) != ARM_MODE_USR;
    arm_fastmem_write16(c, addr, val, priv);
}

static inline uint8_t ir_read8(arm_cpu_t *c, uint32_t addr) {
    bool priv = (c->cpsr & ARM_CPSR_MODE_MASK) != ARM_MODE_USR;
    return arm_fastmem_read8(c, addr, priv);
}

static inline void ir_write8(arm_ir_block_t *block, arm_cpu_t *c, uint32_t addr, uint8_t val) {
    if (block && block->cache && arm_ir_cache_is_page_code(block->cache, addr)) {
        arm_ir_cache_invalidate_page(block->cache, addr);
    }
    bool priv = (c->cpsr & ARM_CPSR_MODE_MASK) != ARM_MODE_USR;
    arm_fastmem_write8(c, addr, val, priv);
}

static inline void set_nz(arm_cpu_t *c, uint32_t val) {
    c->cpsr &= ~(ARM_CPSR_N | ARM_CPSR_Z);
    if (val & 0x80000000u) c->cpsr |= ARM_CPSR_N;
    if (val == 0u)         c->cpsr |= ARM_CPSR_Z;
}

static inline void set_nzcv_add(arm_cpu_t *c, uint32_t a, uint32_t b, uint32_t cin, uint32_t res) {
    uint64_t u = (uint64_t)a + (uint64_t)b + (uint64_t)cin;
    c->cpsr &= ~(ARM_CPSR_N | ARM_CPSR_Z | ARM_CPSR_C | ARM_CPSR_V);
    if (res & 0x80000000u) c->cpsr |= ARM_CPSR_N;
    if (res == 0u)         c->cpsr |= ARM_CPSR_Z;
    if ((u >> 32) & 1u)    c->cpsr |= ARM_CPSR_C;
    if (~(a ^ b) & (a ^ res) & 0x80000000u) c->cpsr |= ARM_CPSR_V;
}

static inline void set_nzcv_sub(arm_cpu_t *c, uint32_t a, uint32_t b, uint32_t cin, uint32_t res) {
    uint64_t u = (uint64_t)a + (uint64_t)(~b) + (uint64_t)cin;
    c->cpsr &= ~(ARM_CPSR_N | ARM_CPSR_Z | ARM_CPSR_C | ARM_CPSR_V);
    if (res & 0x80000000u) c->cpsr |= ARM_CPSR_N;
    if (res == 0u)         c->cpsr |= ARM_CPSR_Z;
    if ((u >> 32) & 1u)    c->cpsr |= ARM_CPSR_C;
    if ((a ^ b) & (a ^ res) & 0x80000000u)  c->cpsr |= ARM_CPSR_V;
}

arm_status_t arm_ir_exec(arm_cpu_t *cpu, arm_ir_block_t *block, unsigned *retired_out) {
    if (!cpu || !block || !block->valid) {
        if (retired_out) *retired_out = 0;
        return ARM_UNDEFINED;
    }

    block->exec_count++;
    uint32_t tmps[8] = {0};
    bool thumb = (cpu->cpsr & ARM_CPSR_T) != 0;
    unsigned retired = 0;
    uint32_t last_guest_pc = 0xffffffffu;

    for (unsigned i = 0; i < block->insn_count; i++) {
        arm_ir_insn_t *insn = &block->insns[i];
        if (insn->dead) continue;

        /* Condition check */
        if (!insn->cond_always && !arm_cond_passed(cpu, insn->cond)) {
            uint32_t skip_pc = insn->guest_pc;
            while (i + 1 < block->insn_count && block->insns[i + 1].guest_pc == skip_pc) {
                i++;
            }
            cpu->r[15] = skip_pc + (thumb ? 2 : 4);
            retired++;
            continue;
        }

        /* Instruction retirement tracking */
        if (insn->guest_pc != last_guest_pc) {
            if (last_guest_pc != 0xffffffffu) {
                retired++;
            }
            last_guest_pc = insn->guest_pc;
        }

        switch (insn->op) {
            case IR_NOP:
                break;

            case IR_MOV: {
                uint32_t val = read_vreg(cpu, tmps, insn->src1, insn->imm, insn->guest_pc, thumb);
                write_vreg(cpu, tmps, insn->dst, val, thumb);
                break;
            }

            case IR_LI: {
                write_vreg(cpu, tmps, insn->dst, insn->imm, thumb);
                break;
            }

            case IR_ADD: {
                uint32_t s1 = read_vreg(cpu, tmps, insn->src1, insn->imm, insn->guest_pc, thumb);
                uint32_t s2 = read_vreg(cpu, tmps, insn->src2, insn->imm, insn->guest_pc, thumb);
                write_vreg(cpu, tmps, insn->dst, s1 + s2, thumb);
                break;
            }

            case IR_SUB: {
                uint32_t s1 = read_vreg(cpu, tmps, insn->src1, insn->imm, insn->guest_pc, thumb);
                uint32_t s2 = read_vreg(cpu, tmps, insn->src2, insn->imm, insn->guest_pc, thumb);
                write_vreg(cpu, tmps, insn->dst, s1 - s2, thumb);
                break;
            }

            case IR_RSB: {
                uint32_t s1 = read_vreg(cpu, tmps, insn->src1, insn->imm, insn->guest_pc, thumb);
                uint32_t s2 = read_vreg(cpu, tmps, insn->src2, insn->imm, insn->guest_pc, thumb);
                write_vreg(cpu, tmps, insn->dst, s2 - s1, thumb);
                break;
            }

            case IR_ADC: {
                uint32_t s1 = read_vreg(cpu, tmps, insn->src1, insn->imm, insn->guest_pc, thumb);
                uint32_t s2 = read_vreg(cpu, tmps, insn->src2, insn->imm, insn->guest_pc, thumb);
                uint32_t cin = (cpu->cpsr & ARM_CPSR_C) ? 1u : 0u;
                write_vreg(cpu, tmps, insn->dst, s1 + s2 + cin, thumb);
                break;
            }

            case IR_SBC: {
                uint32_t s1 = read_vreg(cpu, tmps, insn->src1, insn->imm, insn->guest_pc, thumb);
                uint32_t s2 = read_vreg(cpu, tmps, insn->src2, insn->imm, insn->guest_pc, thumb);
                uint32_t cin = (cpu->cpsr & ARM_CPSR_C) ? 1u : 0u;
                write_vreg(cpu, tmps, insn->dst, s1 - s2 - (!cin), thumb);
                break;
            }

            case IR_RSC: {
                uint32_t s1 = read_vreg(cpu, tmps, insn->src1, insn->imm, insn->guest_pc, thumb);
                uint32_t s2 = read_vreg(cpu, tmps, insn->src2, insn->imm, insn->guest_pc, thumb);
                uint32_t cin = (cpu->cpsr & ARM_CPSR_C) ? 1u : 0u;
                write_vreg(cpu, tmps, insn->dst, s2 - s1 - (!cin), thumb);
                break;
            }

            case IR_MUL: {
                uint32_t s1 = read_vreg(cpu, tmps, insn->src1, insn->imm, insn->guest_pc, thumb);
                uint32_t s2 = read_vreg(cpu, tmps, insn->src2, insn->imm, insn->guest_pc, thumb);
                write_vreg(cpu, tmps, insn->dst, s1 * s2, thumb);
                break;
            }

            case IR_MLA: {
                uint32_t s1 = read_vreg(cpu, tmps, insn->src1, insn->imm, insn->guest_pc, thumb);
                uint32_t s2 = read_vreg(cpu, tmps, insn->src2, insn->imm, insn->guest_pc, thumb);
                uint32_t acc = read_vreg(cpu, tmps, (arm_ir_vreg_t)insn->imm, 0, insn->guest_pc, thumb);
                write_vreg(cpu, tmps, insn->dst, s1 * s2 + acc, thumb);
                break;
            }

            case IR_AND: {
                uint32_t s1 = read_vreg(cpu, tmps, insn->src1, insn->imm, insn->guest_pc, thumb);
                uint32_t s2 = read_vreg(cpu, tmps, insn->src2, insn->imm, insn->guest_pc, thumb);
                write_vreg(cpu, tmps, insn->dst, s1 & s2, thumb);
                break;
            }

            case IR_ORR: {
                uint32_t s1 = read_vreg(cpu, tmps, insn->src1, insn->imm, insn->guest_pc, thumb);
                uint32_t s2 = read_vreg(cpu, tmps, insn->src2, insn->imm, insn->guest_pc, thumb);
                write_vreg(cpu, tmps, insn->dst, s1 | s2, thumb);
                break;
            }

            case IR_EOR: {
                uint32_t s1 = read_vreg(cpu, tmps, insn->src1, insn->imm, insn->guest_pc, thumb);
                uint32_t s2 = read_vreg(cpu, tmps, insn->src2, insn->imm, insn->guest_pc, thumb);
                write_vreg(cpu, tmps, insn->dst, s1 ^ s2, thumb);
                break;
            }

            case IR_BIC: {
                uint32_t s1 = read_vreg(cpu, tmps, insn->src1, insn->imm, insn->guest_pc, thumb);
                uint32_t s2 = read_vreg(cpu, tmps, insn->src2, insn->imm, insn->guest_pc, thumb);
                write_vreg(cpu, tmps, insn->dst, s1 & ~s2, thumb);
                break;
            }

            case IR_MVN: {
                uint32_t s1 = read_vreg(cpu, tmps, insn->src1, insn->imm, insn->guest_pc, thumb);
                write_vreg(cpu, tmps, insn->dst, ~s1, thumb);
                break;
            }

            case IR_LSL: {
                uint32_t val = read_vreg(cpu, tmps, insn->src1, insn->imm, insn->guest_pc, thumb);
                uint32_t amt = read_vreg(cpu, tmps, insn->src2, insn->imm, insn->guest_pc, thumb);
                uint32_t res = (amt < 32) ? (val << amt) : 0u;
                write_vreg(cpu, tmps, insn->dst, res, thumb);
                break;
            }

            case IR_LSR: {
                uint32_t val = read_vreg(cpu, tmps, insn->src1, insn->imm, insn->guest_pc, thumb);
                uint32_t amt = read_vreg(cpu, tmps, insn->src2, insn->imm, insn->guest_pc, thumb);
                uint32_t res = (amt < 32) ? (val >> amt) : 0u;
                write_vreg(cpu, tmps, insn->dst, res, thumb);
                break;
            }

            case IR_ASR: {
                uint32_t val = read_vreg(cpu, tmps, insn->src1, insn->imm, insn->guest_pc, thumb);
                uint32_t amt = read_vreg(cpu, tmps, insn->src2, insn->imm, insn->guest_pc, thumb);
                uint32_t res = (amt < 32) ? (uint32_t)((int32_t)val >> amt) : ((val & 0x80000000u) ? 0xffffffffu : 0u);
                write_vreg(cpu, tmps, insn->dst, res, thumb);
                break;
            }

            case IR_ROR: {
                uint32_t val = read_vreg(cpu, tmps, insn->src1, insn->imm, insn->guest_pc, thumb);
                uint32_t amt = read_vreg(cpu, tmps, insn->src2, insn->imm, insn->guest_pc, thumb) & 31u;
                uint32_t res = amt ? ((val >> amt) | (val << (32 - amt))) : val;
                write_vreg(cpu, tmps, insn->dst, res, thumb);
                break;
            }

            case IR_UPDATE_NZ: {
                uint32_t val = read_vreg(cpu, tmps, insn->src1, insn->imm, insn->guest_pc, thumb);
                set_nz(cpu, val);
                break;
            }

            case IR_UPDATE_NZCV_ADD: {
                uint32_t res = read_vreg(cpu, tmps, insn->dst, insn->imm, insn->guest_pc, thumb);
                uint32_t a   = read_vreg(cpu, tmps, insn->src1, insn->imm, insn->guest_pc, thumb);
                uint32_t b   = read_vreg(cpu, tmps, insn->src2, insn->imm, insn->guest_pc, thumb);
                set_nzcv_add(cpu, a, b, 0, res);
                break;
            }

            case IR_UPDATE_NZCV_SUB: {
                uint32_t res = read_vreg(cpu, tmps, insn->dst, insn->imm, insn->guest_pc, thumb);
                uint32_t a   = read_vreg(cpu, tmps, insn->src1, insn->imm, insn->guest_pc, thumb);
                uint32_t b   = read_vreg(cpu, tmps, insn->src2, insn->imm, insn->guest_pc, thumb);
                set_nzcv_sub(cpu, a, b, 1, res);
                break;
            }

            case IR_LOAD32: {
                uint32_t base = read_vreg(cpu, tmps, insn->src1, insn->imm, insn->guest_pc, thumb);
                uint32_t val = ir_read32(cpu, base + insn->imm);
                write_vreg(cpu, tmps, insn->dst, val, thumb);
                break;
            }

            case IR_LOAD16: {
                uint32_t base = read_vreg(cpu, tmps, insn->src1, insn->imm, insn->guest_pc, thumb);
                uint32_t val = ir_read16(cpu, base + insn->imm);
                write_vreg(cpu, tmps, insn->dst, val, thumb);
                break;
            }

            case IR_LOAD8: {
                uint32_t base = read_vreg(cpu, tmps, insn->src1, insn->imm, insn->guest_pc, thumb);
                uint32_t val = ir_read8(cpu, base + insn->imm);
                write_vreg(cpu, tmps, insn->dst, val, thumb);
                break;
            }

            case IR_STORE32: {
                uint32_t base = read_vreg(cpu, tmps, insn->src1, insn->imm, insn->guest_pc, thumb);
                uint32_t val  = read_vreg(cpu, tmps, insn->src2, insn->imm, insn->guest_pc, thumb);
                ir_write32(block, cpu, base + insn->imm, val);
                break;
            }

            case IR_STORE16: {
                uint32_t base = read_vreg(cpu, tmps, insn->src1, insn->imm, insn->guest_pc, thumb);
                uint32_t val  = read_vreg(cpu, tmps, insn->src2, insn->imm, insn->guest_pc, thumb);
                ir_write16(block, cpu, base + insn->imm, (uint16_t)val);
                break;
            }

            case IR_STORE8: {
                uint32_t base = read_vreg(cpu, tmps, insn->src1, insn->imm, insn->guest_pc, thumb);
                uint32_t val  = read_vreg(cpu, tmps, insn->src2, insn->imm, insn->guest_pc, thumb);
                ir_write8(block, cpu, base + insn->imm, (uint8_t)val);
                break;
            }

            case IR_BRANCH: {
                uint32_t target = (insn->src1 == VREG_CONST)
                                ? insn->imm
                                : read_vreg(cpu, tmps, insn->src1, insn->imm, insn->guest_pc, thumb);
                if (insn->src1 != VREG_CONST) {
                    if (target & 1) cpu->cpsr |= ARM_CPSR_T;
                    else            cpu->cpsr &= ~ARM_CPSR_T;
                    cpu->r[15] = target & ~1u;
                } else {
                    cpu->r[15] = target;
                }
                retired++;
                goto done;
            }

            case IR_BRANCH_COND: {
                if (arm_cond_passed(cpu, insn->cond)) {
                    cpu->r[15] = insn->imm;
                } else {
                    cpu->r[15] = insn->guest_pc + (thumb ? 2 : 4);
                }
                retired++;
                goto done;
            }

            case IR_SYNC_REG: {
                uint32_t val = read_vreg(cpu, tmps, insn->src1, insn->imm, insn->guest_pc, thumb);
                write_vreg(cpu, tmps, insn->dst, val, thumb);
                break;
            }

            case IR_FALLBACK:
            default: {
                cpu->r[15] = insn->guest_pc;
                goto done;
            }
        }
    }

    if (last_guest_pc != 0xffffffffu) {
        retired++;
        cpu->r[15] = last_guest_pc + (thumb ? 2 : 4);
    }

done:
    cpu->cycles += retired;
    if (retired_out) *retired_out = retired;
    return ARM_OK;
}
