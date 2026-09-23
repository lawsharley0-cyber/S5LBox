/*
 * S5LBox — Cached Interpreter Basic Block Execution Engine.
 *
 * Executes pre-decoded instruction records in tight loops without repeating
 * bitfield parsing or decode switch trees. Falls back gracefully to the
 * reference interpreter for any unhandled operations.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "arm_block.h"
#include <string.h>

static inline void set_nz(arm_cpu_t *c, uint32_t val) {
    c->cpsr &= ~(ARM_CPSR_N | ARM_CPSR_Z);
    if (val & 0x80000000u) c->cpsr |= ARM_CPSR_N;
    if (val == 0u)         c->cpsr |= ARM_CPSR_Z;
}

static inline void set_nzcv_add(arm_cpu_t *c, uint32_t a, uint32_t b, uint32_t res) {
    c->cpsr &= ~(ARM_CPSR_N | ARM_CPSR_Z | ARM_CPSR_C | ARM_CPSR_V);
    if (res & 0x80000000u) c->cpsr |= ARM_CPSR_N;
    if (res == 0u)         c->cpsr |= ARM_CPSR_Z;
    if (res < a)           c->cpsr |= ARM_CPSR_C;
    if (~(a ^ b) & (a ^ res) & 0x80000000u) c->cpsr |= ARM_CPSR_V;
}

static inline void set_nzcv_sub(arm_cpu_t *c, uint32_t a, uint32_t b, uint32_t res) {
    c->cpsr &= ~(ARM_CPSR_N | ARM_CPSR_Z | ARM_CPSR_C | ARM_CPSR_V);
    if (res & 0x80000000u) c->cpsr |= ARM_CPSR_N;
    if (res == 0u)         c->cpsr |= ARM_CPSR_Z;
    if (a >= b)            c->cpsr |= ARM_CPSR_C; /* No borrow */
    if ((a ^ b) & (a ^ res) & 0x80000000u)  c->cpsr |= ARM_CPSR_V;
}

static inline uint32_t eval_shift(arm_cpu_t *c, const arm_decoded_insn_t *di, bool *carry_out) {
    uint32_t val = (di->rm == 15) ? (di->is_thumb ? ((di->pc + 4) & ~3u) : (di->pc + 8)) : c->r[di->rm];
    uint32_t shift = di->shift_by_reg ? (c->r[di->rs] & 0xffu) : di->shift_imm;
    *carry_out = (c->cpsr & ARM_CPSR_C) != 0;

    if (shift == 0) return val;

    switch (di->shift_type) {
        case ARM_SHIFT_LSL:
            if (shift < 32) {
                *carry_out = (val >> (32 - shift)) & 1;
                return val << shift;
            } else if (shift == 32) {
                *carry_out = val & 1;
                return 0;
            } else {
                *carry_out = 0;
                return 0;
            }
        case ARM_SHIFT_LSR:
            if (shift < 32) {
                *carry_out = (val >> (shift - 1)) & 1;
                return val >> shift;
            } else if (shift == 32) {
                *carry_out = (val >> 31) & 1;
                return 0;
            } else {
                *carry_out = 0;
                return 0;
            }
        case ARM_SHIFT_ASR:
            if (shift < 32) {
                *carry_out = (val >> (shift - 1)) & 1;
                return (uint32_t)((int32_t)val >> shift);
            } else {
                *carry_out = (val >> 31) & 1;
                return (val & 0x80000000u) ? 0xffffffffu : 0;
            }
        case ARM_SHIFT_ROR:
            shift &= 31;
            if (shift == 0) return val;
            *carry_out = (val >> (shift - 1)) & 1;
            return (val >> shift) | (val << (32 - shift));
        default:
            return val;
    }
}

static inline uint32_t read32(arm_cpu_t *c, uint32_t addr) {
    bool priv = (c->cpsr & ARM_CPSR_MODE_MASK) != ARM_MODE_USR;
    return arm_fastmem_read32(c, addr, priv);
}

static inline void write32(arm_basic_block_t *block, arm_cpu_t *c, uint32_t addr, uint32_t val) {
    if (block && block->cache && arm_block_cache_is_page_code(block->cache, addr)) {
        arm_block_cache_invalidate_page(block->cache, addr);
    }
    bool priv = (c->cpsr & ARM_CPSR_MODE_MASK) != ARM_MODE_USR;
    arm_fastmem_write32(c, addr, val, priv);
}

static inline uint8_t read8(arm_cpu_t *c, uint32_t addr) {
    bool priv = (c->cpsr & ARM_CPSR_MODE_MASK) != ARM_MODE_USR;
    return arm_fastmem_read8(c, addr, priv);
}

static inline void write8(arm_basic_block_t *block, arm_cpu_t *c, uint32_t addr, uint8_t val) {
    if (block && block->cache && arm_block_cache_is_page_code(block->cache, addr)) {
        arm_block_cache_invalidate_page(block->cache, addr);
    }
    bool priv = (c->cpsr & ARM_CPSR_MODE_MASK) != ARM_MODE_USR;
    arm_fastmem_write8(c, addr, val, priv);
}

arm_status_t arm_block_exec(arm_cpu_t *cpu, arm_basic_block_t *block, unsigned *retired_out) {
    if (!cpu || !block || !block->valid) {
        if (retired_out) *retired_out = 0;
        return ARM_UNDEFINED;
    }

    block->exec_count++;

#if defined(S5LBOX_JIT)
    if (block->jit_compiled && block->cache && block->cache->jit_arena_valid && jit_host_can_execute()) {
        int exit_code = jit_enter(&block->cache->jit_arena, &block->jit_blk, cpu);
        if (exit_code == JIT_EXIT_NEXT) {
            unsigned native_retired = block->jit_blk.insn_count;
            cpu->cycles += native_retired;
            if (retired_out) *retired_out = native_retired;
            return ARM_OK;
        } else if (exit_code == JIT_EXIT_ABORT) {
            if (retired_out) *retired_out = 0;
            return ARM_OK;
        }
    }
#endif

    unsigned retired = 0;

    for (unsigned i = 0; i < block->insn_count; i++) {
        arm_decoded_insn_t *di = &block->insns[i];

        /* Condition test */
        if (!di->cond_always && !arm_cond_passed(cpu, di->cond)) {
            cpu->r[15] = di->next_pc;
            retired++;
            continue;
        }

        /* Safe fallback to reference interpreter */
        if (di->op == ARM_OP_FALLBACK || di->op == ARM_OP_SVC || di->op == ARM_OP_CPS) {
            cpu->r[15] = di->pc;
            if (retired_out) *retired_out = retired;
            return ARM_OK;
        }

        if (di->op == ARM_OP_NOP) {
            cpu->r[15] = di->next_pc;
            retired++;
            continue;
        }

        if (di->op == ARM_OP_CLREX) {
            cpu->excl_valid = false;
            cpu->r[15] = di->next_pc;
            retired++;
            continue;
        }

        /* Branches */
        if (di->op == ARM_OP_B) {
            cpu->r[15] = di->imm;
            retired++;
            break;
        }
        if (di->op == ARM_OP_BL) {
            cpu->r[14] = di->next_pc;
            cpu->r[15] = di->imm;
            retired++;
            break;
        }
        if (di->op == ARM_OP_BX) {
            uint32_t target = cpu->r[di->rm];
            if (target & 1) cpu->cpsr |= ARM_CPSR_T;
            else            cpu->cpsr &= ~ARM_CPSR_T;
            cpu->r[15] = target & ~1u;
            retired++;
            break;
        }
        if (di->op == ARM_OP_BLX_REG) {
            uint32_t target = cpu->r[di->rm];
            cpu->r[14] = di->next_pc;
            if (target & 1) cpu->cpsr |= ARM_CPSR_T;
            else            cpu->cpsr &= ~ARM_CPSR_T;
            cpu->r[15] = target & ~1u;
            retired++;
            break;
        }
        if (di->op == ARM_OP_BLX_IMM) {
            cpu->r[14] = di->next_pc;
            cpu->cpsr |= ARM_CPSR_T;
            cpu->r[15] = di->imm;
            retired++;
            break;
        }

        /* ALU and Multiplies */
        if (di->op >= ARM_OP_AND && di->op <= ARM_OP_MVN) {
            uint32_t op1 = (di->rn == 15) ? (di->is_thumb ? ((di->pc + 4) & ~3u) : (di->pc + 8)) : cpu->r[di->rn];
            bool shifter_carry = false;
            uint32_t op2 = di->is_imm ? di->imm : eval_shift(cpu, di, &shifter_carry);
            uint32_t res = 0;
            bool carry_in = (cpu->cpsr & ARM_CPSR_C) != 0;

            switch (di->op) {
                case ARM_OP_AND: res = op1 & op2; break;
                case ARM_OP_EOR: res = op1 ^ op2; break;
                case ARM_OP_SUB: res = op1 - op2; break;
                case ARM_OP_RSB: res = op2 - op1; break;
                case ARM_OP_ADD: res = op1 + op2; break;
                case ARM_OP_ADC: res = op1 + op2 + carry_in; break;
                case ARM_OP_SBC: res = op1 - op2 - (!carry_in); break;
                case ARM_OP_RSC: res = op2 - op1 - (!carry_in); break;
                case ARM_OP_TST: res = op1 & op2; break;
                case ARM_OP_TEQ: res = op1 ^ op2; break;
                case ARM_OP_CMP: res = op1 - op2; break;
                case ARM_OP_CMN: res = op1 + op2; break;
                case ARM_OP_ORR: res = op1 | op2; break;
                case ARM_OP_MOV: res = op2; break;
                case ARM_OP_BIC: res = op1 & ~op2; break;
                case ARM_OP_MVN: res = ~op2; break;
                default: break;
            }

            if (di->sets_flags) {
                if (di->op == ARM_OP_ADD || di->op == ARM_OP_CMN) {
                    set_nzcv_add(cpu, op1, op2, res);
                } else if (di->op == ARM_OP_SUB || di->op == ARM_OP_CMP) {
                    set_nzcv_sub(cpu, op1, op2, res);
                } else if (di->op == ARM_OP_RSB) {
                    set_nzcv_sub(cpu, op2, op1, res);
                } else {
                    set_nz(cpu, res);
                    if (!di->is_imm || di->is_thumb) {
                        if (shifter_carry) cpu->cpsr |= ARM_CPSR_C;
                        else               cpu->cpsr &= ~ARM_CPSR_C;
                    }
                }
            }

            if (di->op != ARM_OP_TST && di->op != ARM_OP_TEQ &&
                di->op != ARM_OP_CMP && di->op != ARM_OP_CMN) {
                if (di->rd == 15) {
                    cpu->r[15] = res & (di->is_thumb ? ~1u : ~3u);
                    retired++;
                    break;
                } else {
                    cpu->r[di->rd] = res;
                }
            }

            cpu->r[15] = di->next_pc;
            retired++;
            continue;
        }

        if (di->op == ARM_OP_MUL) {
            uint32_t res = cpu->r[di->rm] * cpu->r[di->rs];
            cpu->r[di->rd] = res;
            if (di->sets_flags) set_nz(cpu, res);
            cpu->r[15] = di->next_pc;
            retired++;
            continue;
        }

        if (di->op == ARM_OP_MLA) {
            uint32_t res = cpu->r[di->rm] * cpu->r[di->rs] + cpu->r[di->rn];
            cpu->r[di->rd] = res;
            if (di->sets_flags) set_nz(cpu, res);
            cpu->r[15] = di->next_pc;
            retired++;
            continue;
        }

        /* Memory operations */
        if (di->op == ARM_OP_LDR_IMM) {
            uint32_t base = (di->rn == 15) ? ((di->pc + (di->is_thumb ? 4 : 8)) & ~3u) : cpu->r[di->rn];
            uint32_t addr = base + di->imm;
            uint32_t val = read32(cpu, addr);
            if (cpu->abort_pending) {
                cpu->r[15] = di->pc;
                if (retired_out) *retired_out = retired;
                return ARM_OK;
            }
            if (di->rd == 15) {
                cpu->r[15] = val & (di->is_thumb ? ~1u : ~3u);
                retired++;
                break;
            } else {
                cpu->r[di->rd] = val;
            }
            cpu->r[15] = di->next_pc;
            retired++;
            continue;
        }

        if (di->op == ARM_OP_STR_IMM) {
            uint32_t base = cpu->r[di->rn];
            uint32_t addr = base + di->imm;
            uint32_t val = (di->rd == 15) ? (di->pc + 8) : cpu->r[di->rd];
            write32(block, cpu, addr, val);
            if (cpu->abort_pending) {
                cpu->r[15] = di->pc;
                if (retired_out) *retired_out = retired;
                return ARM_OK;
            }
            cpu->r[15] = di->next_pc;
            retired++;
            continue;
        }

        if (di->op == ARM_OP_LDRB_IMM) {
            uint32_t base = (di->rn == 15) ? ((di->pc + (di->is_thumb ? 4 : 8)) & ~3u) : cpu->r[di->rn];
            uint32_t addr = base + di->imm;
            uint8_t val = read8(cpu, addr);
            if (cpu->abort_pending) {
                cpu->r[15] = di->pc;
                if (retired_out) *retired_out = retired;
                return ARM_OK;
            }
            cpu->r[di->rd] = val;
            cpu->r[15] = di->next_pc;
            retired++;
            continue;
        }

        if (di->op == ARM_OP_STRB_IMM) {
            uint32_t base = cpu->r[di->rn];
            uint32_t addr = base + di->imm;
            write8(block, cpu, addr, (uint8_t)cpu->r[di->rd]);
            if (cpu->abort_pending) {
                cpu->r[15] = di->pc;
                if (retired_out) *retired_out = retired;
                return ARM_OK;
            }
            cpu->r[15] = di->next_pc;
            retired++;
            continue;
        }

        /* Any other instruction: update PC and fall back */
        cpu->r[15] = di->pc;
        if (retired_out) *retired_out = retired;
        return ARM_OK;
    }

    cpu->cycles += retired;
    if (retired_out) *retired_out = retired;
    return ARM_OK;
}
