/*
 * S5LBox — differential fuzzer: cached interpreter vs reference interpreter.
 *
 * Two identical machines run the same random instruction sequences from the
 * same random state -- one on arm_step (S5L8900_CPU_BACKEND_INTERPRETER), one
 * on the cached interpreter -- through s5l8900_run() with the MMU on, and
 * every architectural bit is compared afterwards: all registers in every
 * bank, CPSR/SPSRs, the retired-instruction count, fault registers, the
 * exclusive monitor, VFP state, run status and retired count, and every byte
 * of the data pages.
 *
 * The generator is biased towards what breaks interpreters: all sixteen
 * conditions, boundary operand values, every shifter corner (#32, RRX,
 * register shifts >= 32), PC as an operand, writeback and base/destination
 * overlap, unaligned and page-straddling addresses, accesses to unmapped and
 * read-only pages (data aborts), branches in and out of Thumb, mode
 * variety, and code rewritten between cases (self-modifying code: the engine
 * must never run a stale block).
 *
 *   test_ci_diff [cases-per-isa] [seed]
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "soc.h"
#include "arm_ci.h"
#include "vfp.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAM_BASE   0x08000000u
#define RAM_SIZE   (4u << 20)
#define L1_PA      (RAM_BASE + 0x00300000u)     /* 16 KiB aligned */
#define L2_PA      (L1_PA + 0x4000u)
/* Virtual layout (all inside the first MiB, one coarse table):
 *   0x00000000 vectors (NOP sled)          RW
 *   0x00010000 code, 4 pages               RW
 *   0x00020000 data, 4 pages               RW
 *   0x00024000 read-only data              user RO / priv RW (AP=2)
 *   0x00025000 unmapped                    (fault)
 *   0x00026000 VIC0 registers              device (MMIO)
 *   0x00027000 timer registers             device (MMIO)
 */
#define VEC_VA     0x00000000u
#define CODE_VA    0x00010000u
#define DATA_VA    0x00020000u
#define DATA_BYTES 0x4000u
#define RO_VA      0x00024000u
#define HOLE_VA    0x00025000u
#define VIC_VA     0x00026000u
#define TIMER_VA   0x00027000u
#define PHYS(va)   (RAM_BASE + 0x00100000u + (va))

static uint64_t g_rng;
static uint32_t rnd(void) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 16);
}
static uint32_t rnd_n(uint32_t n) { return rnd() % n; }
static bool chance(unsigned pct) { return rnd_n(100u) < pct; }

static void poke32(s5l8900_t *m, uint32_t pa, uint32_t v) { m->bus.write32(m->bus.ctx, pa, v); }

static void map_page(s5l8900_t *m, uint32_t va, uint32_t ap_bits) {
    poke32(m, L2_PA + ((va >> 12) & 0xffu) * 4u, (PHYS(va) & 0xfffff000u) | ap_bits | 0x2u);
}

static bool machine_setup(s5l8900_t *m, bool cached) {
    if (!s5l8900_init(m, RAM_BASE, RAM_SIZE)) return false;
    (void)s5l8900_set_direct_ram_writes(m, true);
    if (cached && !s5l8900_set_cpu_backend(m, S5L8900_CPU_BACKEND_CACHED_BLOCK))
        return false;
    for (uint32_t i = 0; i < 4096u; i++) poke32(m, L1_PA + i * 4u, 0u);
    poke32(m, L1_PA, L2_PA | 0x1u);                     /* coarse table, domain 0 */
    for (uint32_t i = 0; i < 256u; i++) poke32(m, L2_PA + i * 4u, 0u);
    map_page(m, VEC_VA, 0x30u);
    for (uint32_t p = 0; p < 4u; p++) map_page(m, CODE_VA + p * 0x1000u, 0x30u);
    for (uint32_t p = 0; p < 4u; p++) map_page(m, DATA_VA + p * 0x1000u, 0x30u);
    map_page(m, RO_VA, 0x20u);                         /* AP=2: user read-only */
    /* Real devices: loads and stores here are MMIO (level_dirty), and a guest
     * that enables a VIC line and unmasks IRQ takes a real interrupt. */
    poke32(m, L2_PA + ((VIC_VA >> 12) & 0xffu) * 4u, S5L8900_VIC0_BASE | 0x30u | 0x2u);
    poke32(m, L2_PA + ((TIMER_VA >> 12) & 0xffu) * 4u, S5L8900_TIMER_BASE | 0x30u | 0x2u);
    for (uint32_t i = 0; i < 1024u; i++)               /* vector page: NOPs */
        poke32(m, PHYS(VEC_VA) + i * 4u, 0xe1a00000u);
    return true;
}

/* ------------------------------------------------------------ operands --- */

static uint32_t interesting(void) {
    static const uint32_t v[] = {
        0u, 1u, 2u, 31u, 32u, 33u, 0x7fu, 0x80u, 0xffu, 0x100u, 0x7fffu, 0x8000u,
        0xffffu, 0x7fffffffu, 0x80000000u, 0x80000001u, 0xfffffffeu, 0xffffffffu,
    };
    return v[rnd_n((uint32_t)(sizeof v / sizeof v[0]))];
}

static uint32_t operand_value(void) {
    switch (rnd_n(7u)) {
        case 6: return chance(50) ? rnd_n(0x40u) * 4u                     /* table index */
                                  : CODE_VA + (rnd_n(0x400u) & ~3u) + rnd_n(4u);
        case 0: return interesting();
        case 1: return rnd_n(64u);
        case 2: return DATA_VA + rnd_n(DATA_BYTES);                 /* any alignment */
        case 3: return DATA_VA + (rnd_n(DATA_BYTES) & ~3u);         /* word aligned */
        case 4: switch (rnd_n(4u)) {
                    case 0:  return RO_VA + (rnd_n(0x1000u) & ~3u);
                    case 1:  return HOLE_VA + (rnd_n(0x1000u) & ~3u);
                    case 2:  return VIC_VA + (rnd_n(0x40u) & ~3u);
                    default: return TIMER_VA + (rnd_n(0x100u) & ~3u);
                }
        default: return rnd();
    }
}

static unsigned reg_field(void) {
    unsigned r = rnd_n(100u);
    if (r < 60u) return rnd_n(8u);
    if (r < 80u) return 8u + rnd_n(5u);
    if (r < 90u) return 13u + rnd_n(2u);
    return 15u;
}

/* -------------------------------------------------------------- ARM gen --- */

static uint32_t arm_cond(void) {
    if (chance(70)) return 0xeu;
    return rnd_n(15u);
}

/* The VFP-heavy phase: most instructions from the VFP generator, so the
 * engine's decoded VFP forms meet NaNs, flush-to-zero, short vectors and trap
 * enables thousands of times rather than a few. */
static bool g_vfp_heavy;
static bool g_neon;         /* the Cortex-A8 passes: Advanced SIMD in the mix */

/* An Advanced SIMD instruction in its ARM encoding: data processing with
 * every field random, or an element/structure load or store from a
 * register the state fill points into data. The Thumb pass maps it. */
static uint32_t gen_neon_arm(void) {
    if (chance(70)) return 0xf2000000u | (rnd() & 0x01ffffffu);
    const unsigned rn = reg_field(), rm = chance(40) ? 15u : chance(50) ? 13u : reg_field();
    return 0xf4000000u | (rnd() & 0x00e0fff0u) | (rn << 16) | rm;
}

static uint32_t gen_arm(unsigned idx, unsigned len) {
    if (g_neon && chance(15)) return gen_neon_arm();
    uint32_t c = arm_cond() << 28;
    unsigned rd = reg_field(), rn = reg_field(), rm = reg_field(), rs = reg_field();
    switch (g_vfp_heavy && chance(60) ? 24u : rnd_n(29u)) {
        case 0: case 1: case 2:                                         /* DP imm */
            return c | 0x02000000u | (rnd_n(16u) << 21) | (rnd_n(2u) << 20) |
                   (rn << 16) | (rd << 12) | (rnd_n(16u) << 8) |
                   (chance(50) ? rnd_n(256u) : (interesting() & 0xffu));
        case 3: case 4: case 5:                                         /* DP shift imm */
            return c | (rnd_n(16u) << 21) | (rnd_n(2u) << 20) | (rn << 16) | (rd << 12) |
                   ((chance(30) ? 0u : rnd_n(32u)) << 7) | (rnd_n(4u) << 5) | rm;
        case 6:                                                          /* DP shift reg */
            return c | (rnd_n(16u) << 21) | (rnd_n(2u) << 20) | (rn << 16) | (rd << 12) |
                   (rs << 8) | (rnd_n(4u) << 5) | 0x10u | rm;
        case 7:                                                          /* MUL/MLA */
            return c | (rnd_n(4u) << 20) | (rd << 16) | (rn << 12) | (rs << 8) | 0x90u | rm;
        case 8:                                                          /* long mul */
            return c | 0x00800000u | (rnd_n(8u) << 20) | (rd << 16) | (rn << 12) |
                   (rs << 8) | 0x90u | rm;
        case 9: case 10: case 11:                                        /* LDR/STR imm */
            return c | 0x04000000u | (rnd_n(32u) << 20) | (rn << 16) | (rd << 12) |
                   (chance(80) ? rnd_n(64u) : rnd_n(4096u));
        case 12:                                                         /* LDR/STR reg */
            return c | 0x06000000u | (rnd_n(32u) << 20) | (rn << 16) | (rd << 12) |
                   (rnd_n(32u) << 7) | (rnd_n(4u) << 5) | rm;
        case 13: {                                                       /* extra ld/st */
            uint32_t sh = 1u + rnd_n(3u);
            return c | (rnd_n(2u) << 24) | (rnd_n(2u) << 23) | (rnd_n(2u) << 22) |
                   (rnd_n(2u) << 21) | (rnd_n(2u) << 20) | (rn << 16) | (rd << 12) |
                   (rnd_n(16u) << 8) | 0x90u | (sh << 5) | (chance(50) ? rnd_n(16u) : rm);
        }
        case 14: {                                                       /* LDM/STM */
            /* Half random lists, half short ones like compiled code's (which
             * reach the engine's single-block fast path more often). */
            uint32_t list = chance(50) ? (rnd() & 0xffffu)
                          : (1u << rnd_n(16u)) | (1u << rnd_n(13u)) |
                            (chance(30) ? 0x4000u : 0u) | (chance(20) ? 0x8000u : 0u);
            return c | 0x08000000u | (rnd_n(4u) << 23) | ((chance(10) ? 1u : 0u) << 22) |
                   (rnd_n(4u) << 20) | (rn << 16) | list;
        }
        case 15: {                                                       /* B/BL */
            int32_t off = (int32_t)rnd_n(len + 2u) - (int32_t)idx - 2;
            return c | 0x0a000000u | (rnd_n(2u) << 24) | ((uint32_t)off & 0xffffffu);
        }
        case 16:                                                         /* BX/BLX */
            return c | 0x012fff10u | (rnd_n(2u) << 5) | rm;
        case 17:                                                         /* MRS / MSR */
            if (chance(50)) return c | 0x010f0000u | (rnd_n(2u) << 22) | (rd << 12);
            return c | 0x0320f000u | (rnd_n(2u) << 22) | ((rnd_n(2u) ? 8u : rnd_n(16u)) << 16) |
                   (rnd_n(16u) << 8) | rnd_n(256u);
        case 18: {                                                       /* media */
            static const uint32_t ext[] = { 0x06af0070u, 0x06bf0070u, 0x06ef0070u, 0x06ff0070u };
            static const uint32_t other[] = { 0x06bf0f30u, 0x06bf0fb0u, 0x06ff0fb0u, 0x016f0f10u };
            if (chance(60)) {
                uint32_t f = ext[rnd_n(4u)];
                if (chance(20)) f = (f & ~0x000f0000u) | (rn << 16);     /* accumulate form */
                return c | f | (rd << 12) | (rnd_n(4u) << 10) | rm;
            }
            return c | other[rnd_n(4u)] | (rd << 12) | rm;
        }
        case 19:                                                         /* SWP/exclusives */
            if (chance(50)) return c | 0x01000090u | (rnd_n(2u) << 22) | (rn << 16) | (rd << 12) | rm;
            return c | 0x01800f90u | (rnd_n(8u) << 20) | (rn << 16) | (rd << 12) | rm;
        case 20: {                                                       /* unconditional */
            static const uint32_t forms[] = { 0xf57ff01fu, 0xf5d0f000u, 0xfa000000u, 0xf1010000u };
            uint32_t f = forms[rnd_n(4u)];
            if (f == 0xfa000000u) f |= rnd_n(2u) << 24 | (rnd_n(len + 1u) & 0xffffffu);
            if (f == 0xf5d0f000u) f |= rn << 16 | rnd_n(4096u);
            return f;
        }
        case 21: case 22: {                                              /* CP15 MCR/MRC */
            /* c7 (barriers, cache maintenance, sometimes WFI), c13 (thread
             * and context IDs), c0 (identification): the forms the engine
             * specialises or must leave to arm_step. Never c1-c3, which would
             * wreck this harness's own translation set-up. */
            static const uint8_t crns[] = { 7u, 7u, 13u, 13u, 13u, 0u };
            unsigned crn = crns[rnd_n(6u)];
            unsigned opc2 = crn == 13u ? rnd_n(8u) : rnd_n(8u);
            unsigned crm = crn == 7u ? (chance(10) ? 0u : rnd_n(16u)) : (chance(80) ? 0u : rnd_n(2u));
            unsigned L = crn == 0u ? 1u : rnd_n(2u);
            unsigned r = chance(5) ? 15u : rnd_n(15u);
            return c | 0x0e000f10u | (L << 20) | (crn << 16) | (r << 12) | (opc2 << 5) | crm |
                   ((chance(10) ? rnd_n(8u) : 0u) << 21);
        }
        case 23: {                                                       /* CPS */
            static const uint32_t modes[] = { ARM_MODE_USR, ARM_MODE_SVC, ARM_MODE_IRQ,
                                              ARM_MODE_SYS, 0x1bu, 0x05u };
            unsigned imod = chance(80) ? 2u + rnd_n(2u) : rnd_n(2u);
            unsigned M = chance(25) ? 1u : 0u;
            return 0xf1000000u | (imod << 18) | (M << 17) | (rnd_n(8u) << 6) |
                   (M ? modes[rnd_n(6u)] : 0u);
        }
        case 24: case 25: {                                              /* VFP */
            /* Every group the unit decodes (CDP, VLDR/VSTR, VLDM/VSTM,
             * VMOV core<->single and VMRS/VMSR, VMOV core pair <-> two
             * singles or a double), with fields random inside the group. */
            unsigned sz = rnd_n(2u), vd = rnd_n(16u), imm8 = rnd_n(256u);
            switch (rnd_n(6u)) {
                case 0:                                                  /* CDP */
                    return c | 0x0e000a00u | (rnd() & 0x00fff0efu) | (sz << 8);
                case 1: {
                    /* The forms compiled code uses, with random registers:
                     * the arithmetic (op 0-4, both alt forms), and in the
                     * op-7 group VMOV/VABS/VNEG/VSQRT, VCMP/VCMPE against a
                     * register or #0, and the conversions. */
                    static const uint8_t opc2s[] = { 0u, 1u, 4u, 5u, 5u, 4u, 7u, 8u, 12u, 13u };
                    uint32_t w = c | 0x0e000a00u | (sz << 8) | (rnd() & 0x0040f0afu);
                    if (chance(55)) {
                        unsigned op = rnd_n(5u);
                        w |= ((op >> 2) & 1u) << 23 | ((op >> 1) & 1u) << 21 | (op & 1u) << 20 |
                             (rnd() & 0x000f0000u) | (rnd_n(2u) << 6);
                    } else {
                        unsigned opc2 = opc2s[rnd_n(10u)];
                        w |= 0x00b00040u | (opc2 << 16);
                        if (opc2 == 5u && chance(80)) w &= ~0x0000002fu;   /* VCMP #0.0 */
                    }
                    return w;
                }
                case 2:                                                  /* VLDR/VSTR */
                    return c | 0x0d000a00u | (rnd_n(2u) << 23) | (rnd_n(2u) << 22) |
                           (rnd_n(2u) << 20) | (rn << 16) | (vd << 12) | (sz << 8) |
                           (chance(70) ? rnd_n(16u) : imm8);
                case 3:                                                  /* VLDM/VSTM */
                    return c | 0x0c000a00u | (rnd_n(4u) << 23) | (rnd_n(2u) << 22) |
                           (rnd_n(4u) << 20) | (rn << 16) | (vd << 12) | (sz << 8) |
                           (chance(80) ? 1u + rnd_n(8u) : imm8);
                case 4: {                                                /* 32-bit transfer */
                    static const uint8_t sys[] = { 0u, 1u, 1u, 1u, 8u, 6u };
                    unsigned L = rnd_n(2u);
                    if (chance(50))                                      /* VMOV sN <-> rd */
                        return c | 0x0e000a10u | (L << 20) | (vd << 16) | (rd << 12) |
                               (rnd_n(2u) << 7);
                    unsigned reg = sys[rnd_n(6u)];                       /* VMRS/VMSR */
                    unsigned r = (L && reg == 1u && chance(40)) ? 15u : rd;
                    return c | 0x0ee00a10u | (L << 20) | (reg << 16) | (r << 12);
                }
                default:                                                 /* 64-bit transfer */
                    return c | 0x0c400a10u | (rnd_n(2u) << 20) | (rn << 16) | (rd << 12) |
                           (sz << 8) | (rnd_n(2u) << 5) | rnd_n(16u);
            }
        }
        case 26: case 27:                                                /* PC writes */
            /* The forms compiled code uses to jump through memory or a
             * register (stubs, returns, switch tables) and the PIC address
             * idiom, with the bases and data words (see the data fill) that
             * make their targets land in code. */
            switch (rnd_n(6u)) {
                case 0: case 1: {                                        /* LDR pc, imm */
                    unsigned P = chance(70) ? 1u : 0u, W = P && chance(30) ? 1u : 0u;
                    unsigned base = chance(50) ? 13u : chance(50) ? 15u : rn;
                    return c | 0x04100000u | (P << 24) | (rnd_n(2u) << 23) | (W << 21) |
                           (base << 16) | (15u << 12) | (rnd_n(4u) * 4u);
                }
                case 2:                                                  /* LDR pc, [Rn, Rm, LSL #2] */
                    return c | 0x07100000u | (rnd_n(2u) << 23) | ((chance(30) ? 15u : rn) << 16) |
                           (15u << 12) | (2u << 7) | rm;
                case 3:                                                  /* MOV pc, Rm */
                    return c | 0x01a0f000u | rm;
                case 4:                                                  /* ADD/SUB Rd, PC, Rm */
                    return c | (chance(50) ? 0x008f0000u : 0x004f0000u) | (rd << 12) | rm;
                default:                                                 /* ADD pc, pc, Rm, LSL #2 */
                    return c | 0x008ff100u | rm;
            }
        default:                                                         /* anything */
            return (rnd() & 0x0fffffffu) | c;
    }
}

/* ------------------------------------------------------------ Thumb gen --- */

static uint16_t gen_thumb(unsigned idx, unsigned len, bool *pair) {
    *pair = false;
    switch (rnd_n(16u)) {
        case 0: return (uint16_t)(0x0000u | (rnd_n(3u) << 11) | (rnd_n(32u) << 6) | rnd_n(64u));
        case 1: return (uint16_t)(0x1800u | (rnd() & 0x07ffu));
        case 2: return (uint16_t)(0x2000u | (rnd() & 0x1fffu));
        case 3: return (uint16_t)(0x4000u | (rnd() & 0x03ffu));
        case 4: return (uint16_t)(0x4400u | (rnd() & 0x03ffu));
        case 5: return (uint16_t)(0x4800u | (rnd() & 0x07ffu));
        case 6: return (uint16_t)(0x5000u | (rnd() & 0x0fffu));
        case 7: return (uint16_t)(0x6000u | (rnd() & 0x1fffu));
        case 8: return (uint16_t)(0x8000u | (rnd() & 0x0fffu));
        case 9: return (uint16_t)(0x9000u | (rnd() & 0x0fffu));
        case 10: return (uint16_t)(0xa000u | (rnd() & 0x0fffu));
        case 11: {
            static const uint16_t base[] = { 0xb000u, 0xb400u, 0xbc00u, 0xb200u, 0xba00u, 0xb660u, 0xb650u };
            uint16_t b = base[rnd_n((uint32_t)(sizeof base / sizeof base[0]))];
            return (uint16_t)(b | (b == 0xb660u ? (rnd() & 0x17u) : b == 0xb650u ? (rnd() & 0x8u)
                                   : (rnd() & 0x01ffu)));
        }
        case 12: return (uint16_t)(0xc000u | (rnd() & 0x0fffu));
        case 13: {
            int32_t off = (int32_t)rnd_n(len + 2u) - (int32_t)idx - 2;
            return (uint16_t)(0xd000u | (rnd_n(15u) << 8) | ((uint32_t)off & 0xffu));
        }
        case 14: {
            int32_t off = (int32_t)rnd_n(len + 2u) - (int32_t)idx - 2;
            return (uint16_t)(0xe000u | ((uint32_t)off & 0x7ffu));
        }
        default:
            *pair = true;                                  /* BL/BLX prefix */
            return (uint16_t)(0xf000u | (chance(50) ? 0x7ffu : 0u));
    }
}

/* ------------------------------------------------------ Thumb-2 gen --- */

/*
 * ARMv7 Thumb for the Cortex-A8 profile: the ARM1176 generator's 16-bit
 * forms (less its BL halves, which are 32-bit here), IT with every
 * condition and mask, CBZ/CBNZ, the hints, and 32-bit instructions from
 * every encoding group with random low bits. Registers come from reg_field(),
 * so PC and SP turn up in every operand position and the engine has to agree
 * with the reference about each refusal too. Returns the halfwords written.
 */
static uint16_t t2_reg4(void) { return (uint16_t)reg_field(); }

static unsigned gen_thumb2(unsigned idx, unsigned len, uint16_t *h) {
    const int32_t fwd = (int32_t)rnd_n(len + 4u) - (int32_t)idx - 2;  /* halfwords */
    if (g_neon && chance(12)) {                         /* Advanced SIMD */
        const uint32_t a = gen_neon_arm();
        const uint32_t t = (a >> 24) == 0xf4u ? 0xf9000000u | (a & 0x00ffffffu)
                         : 0xef000000u | ((a & 0x01000000u) << 4) | (a & 0x00ffffffu);
        h[0] = (uint16_t)(t >> 16);
        h[1] = (uint16_t)t;
        return 2u;
    }
    switch (rnd_n(24u)) {
    case 0: case 1: case 2: case 3: case 4: case 5: {
        bool pair;
        uint16_t t = gen_thumb(idx, len, &pair);
        if (pair) t = (uint16_t)(0xe000u | ((uint32_t)fwd & 0x7ffu));   /* B instead */
        h[0] = t;
        return 1u;
    }
    case 6: {                                           /* IT, any cond and mask */
        unsigned first = rnd_n(chance(90) ? 14u : 16u), mask = 1u + rnd_n(15u);
        h[0] = (uint16_t)(0xbf00u | (first << 4) | mask);
        return 1u;
    }
    case 7:                                             /* CBZ/CBNZ, hints */
        if (chance(60)) {
            uint32_t off = rnd_n(64u);                  /* halfwords, forward */
            h[0] = (uint16_t)(0xb100u | (rnd_n(2u) << 11) | ((off >> 5) << 9) |
                              ((off & 0x1fu) << 3) | rnd_n(8u));
        } else {
            h[0] = (uint16_t)(0xbf00u | (rnd_n(chance(90) ? 5u : 16u) << 4));
        }
        return 1u;
    case 8: case 9: {                                   /* DP modified immediate */
        h[0] = (uint16_t)(0xf000u | (rnd_n(2u) << 10) | (rnd_n(16u) << 5) |
                          (rnd_n(2u) << 4) | t2_reg4());
        h[1] = (uint16_t)((rnd() & 0x7000u) | (t2_reg4() << 8) | (rnd() & 0xffu));
        return 2u;
    }
    case 10: {                                          /* plain immediate */
        static const uint8_t ops[] = { 0x00, 0x04, 0x0a, 0x0c, 0x10, 0x12, 0x14,
                                       0x16, 0x18, 0x1a, 0x1c };
        unsigned op = ops[rnd_n((uint32_t)sizeof ops)];
        h[0] = (uint16_t)(0xf200u | (rnd_n(2u) << 10) | (op << 4) | t2_reg4());
        h[1] = (uint16_t)((rnd() & 0x70ffu) | (t2_reg4() << 8));
        if (op >= 0x10u) h[1] &= (uint16_t)~0x20u;
        return 2u;
    }
    case 11: case 12: {                                 /* DP shifted register */
        h[0] = (uint16_t)(0xea00u | (rnd_n(16u) << 5) | (rnd_n(2u) << 4) | t2_reg4());
        h[1] = (uint16_t)((rnd() & 0x70f0u) | (t2_reg4() << 8) | t2_reg4());
        return 2u;
    }
    case 13: case 14: {                                 /* load/store single */
        unsigned form = rnd_n(4u), size = rnd_n(chance(90) ? 3u : 4u);
        bool load = chance(60), sgn = load && size < 2u && chance(30);
        h[0] = (uint16_t)(0xf800u | (sgn ? 0x100u : 0u) | (size << 5) |
                          (load ? 0x10u : 0u) | (form == 0u ? 0x80u : 0u) |
                          (chance(8) ? 15u : t2_reg4()));
        uint16_t lo;
        switch (form) {
            case 0:  lo = (uint16_t)(rnd() & 0xfffu); break;              /* imm12 */
            case 1:  lo = (uint16_t)(0x800u | (rnd() & 0x7ffu)); break;   /* imm8  */
            case 2:  lo = (uint16_t)((rnd_n(4u) << 4) | t2_reg4()); break; /* reg */
            default: lo = (uint16_t)(rnd() & 0xfffu); break;
        }
        h[1] = (uint16_t)((t2_reg4() << 12) | lo);
        return 2u;
    }
    case 15: {                                          /* LDM/STM, PUSH/POP.W */
        h[0] = (uint16_t)(0xe800u | ((1u + rnd_n(2u)) << 7) | (rnd_n(2u) << 5) |
                          (rnd_n(2u) << 4) | (chance(40) ? 13u : t2_reg4()));
        h[1] = (uint16_t)(rnd() & (chance(80) ? 0x5fffu : 0xffffu));
        return 2u;
    }
    case 16: {                                          /* dual, exclusive, TBB */
        h[0] = (uint16_t)(0xe840u | (rnd_n(4u) << 7) | (rnd_n(2u) << 5) |
                          (rnd_n(2u) << 4) | t2_reg4());
        h[1] = (uint16_t)((t2_reg4() << 12) | (t2_reg4() << 8) | (rnd() & 0xffu));
        if (chance(30)) h[1] = (uint16_t)(0xf000u | (rnd_n(2u) << 4) | rnd_n(8u));  /* TBB/TBH */
        return 2u;
    }
    case 17: {                                          /* branches */
        const uint32_t off = (uint32_t)fwd * 2u;        /* bytes, from pc + 4 - 4 */
        const uint32_t S = (off >> 24) & 1u, I1 = (off >> 23) & 1u, I2 = (off >> 22) & 1u;
        switch (rnd_n(4u)) {
        case 0: case 1: {                               /* B.W / BL / BLX */
            static const uint16_t op1[] = { 0x9000u, 0xd000u, 0xc000u };
            unsigned k = rnd_n(3u);
            h[0] = (uint16_t)(0xf000u | (S << 10) | ((off >> 12) & 0x3ffu));
            h[1] = (uint16_t)(op1[k] | (((I1 ^ S) ^ 1u) << 13) | (((I2 ^ S) ^ 1u) << 11) |
                              ((off >> 1) & 0x7ffu));
            if (k == 2u) h[1] &= (uint16_t)~1u;          /* BLX: H must be 0 */
            break;
        }
        default:                                        /* B<c>.W */
            h[0] = (uint16_t)(0xf000u | (((off >> 20) & 1u) << 10) | (rnd_n(14u) << 6) |
                              ((off >> 12) & 0x3fu));
            h[1] = (uint16_t)(0x8000u | (((off >> 18) & 1u) << 13) |
                              (((off >> 19) & 1u) << 11) | ((off >> 1) & 0x7ffu));
            break;
        }
        return 2u;
    }
    case 18: {                                          /* system */
        switch (rnd_n(6u)) {
            case 0:  h[0] = (uint16_t)(0xf3efu | (rnd_n(2u) << 4));
                     h[1] = (uint16_t)(0x8000u | (t2_reg4() << 8)); break;        /* MRS */
            case 1:  h[0] = (uint16_t)(0xf380u | (rnd_n(2u) << 4) | t2_reg4());
                     h[1] = (uint16_t)(0x8000u | (rnd_n(16u) << 8)); break;        /* MSR */
            case 2:  h[0] = 0xf3afu;
                     h[1] = (uint16_t)(0x8000u | (rnd_n(4u) << 9) | (rnd_n(2u) << 8) |
                                       (rnd_n(8u) << 5) | (chance(50) ? ARM_MODE_SYS : rnd_n(32u)));
                     break;                                                        /* CPS */
            case 3:  h[0] = 0xf3afu;
                     h[1] = (uint16_t)(0x8000u | rnd_n(chance(90) ? 5u : 256u)); break; /* hints */
            case 4:  h[0] = 0xf3bfu;
                     h[1] = (uint16_t)(0x8f00u | (rnd_n(8u) << 4) | rnd_n(16u)); break; /* barriers */
            default: h[0] = (uint16_t)(0xf380u | (rnd() & 0x7fu));
                     h[1] = (uint16_t)(0x8000u | (rnd() & 0x7fffu)); break;
        }
        return 2u;
    }
    case 19: case 20: {                                 /* DP register */
        h[0] = (uint16_t)(0xfa00u | (rnd_n(16u) << 4) | t2_reg4());
        h[1] = (uint16_t)((chance(95) ? 0xf000u : (rnd() & 0xf000u)) | (t2_reg4() << 8) |
                          (rnd_n(16u) << 4) | t2_reg4());
        return 2u;
    }
    case 21: {                                          /* multiplies */
        h[0] = (uint16_t)(0xfb00u | (rnd_n(16u) << 4) | t2_reg4());
        h[1] = (uint16_t)((t2_reg4() << 12) | (t2_reg4() << 8) |
                          ((chance(70) ? 0u : rnd_n(16u)) << 4) | t2_reg4());
        return 2u;
    }
    case 22: {                                          /* VFP in Thumb state */
        uint32_t w = gen_arm(idx, len) & 0x0fffffffu;
        if ((w & 0x0c000e00u) != 0x0c000a00u)            /* want cp10/cp11 */
            w = 0x0e300a00u | (rnd() & 0x00cff0efu);      /* VADD/VSUB family */
        w |= 0xe0000000u;
        h[0] = (uint16_t)(w >> 16);
        h[1] = (uint16_t)w;
        return 2u;
    }
    default:                                            /* anything 32-bit */
        h[0] = (uint16_t)(0xe800u + rnd_n(0x1800u));
        h[1] = (uint16_t)rnd();
        return 2u;
    }
}

/* ------------------------------------------------------------ one case --- */

typedef struct {
    uint32_t r[16], cpsr, spsr[ARM_BANK_COUNT], b13[ARM_BANK_COUNT], b14[ARM_BANK_COUNT];
    uint32_t fiq[5], usr[5], sctlr_extra, fpscr, fpexc, cpacr, s[64];
    uint64_t cycles;
    bool excl_valid;
    uint32_t excl_addr;
    uint32_t tid[3];      /* TPIDRURW, TPIDRURO, TPIDRPRW */
    bool irq_pending;     /* a VIC software interrupt asserted before the run */
} state_t;

static void random_state(state_t *s, bool thumb) {
    static const uint32_t modes[] = { ARM_MODE_USR, ARM_MODE_SVC, ARM_MODE_IRQ,
                                      ARM_MODE_FIQ, ARM_MODE_ABT, ARM_MODE_UND,
                                      ARM_MODE_SYS };
    memset(s, 0, sizeof *s);
    for (int i = 0; i < 15; i++) s->r[i] = operand_value();
    s->r[13] = DATA_VA + 0x2000u + (rnd_n(0x800u) & ~3u);
    s->cpsr = (rnd() & 0xf8000000u) | modes[rnd_n(7u)] |
              (chance(80) ? ARM_CPSR_I : 0u) | ARM_CPSR_F |
              (thumb ? ARM_CPSR_T : 0u);
    for (int i = 0; i < ARM_BANK_COUNT; i++) {
        s->spsr[i] = (rnd() & 0xf0000000u) | modes[rnd_n(7u)] | ARM_CPSR_I | ARM_CPSR_F |
                     (chance(30) ? ARM_CPSR_T : 0u);
        s->b13[i] = DATA_VA + 0x2000u + (rnd_n(0x800u) & ~3u);
        s->b14[i] = CODE_VA + (rnd_n(0x400u) & ~1u) + (chance(30) ? 1u : 0u);
    }
    for (int i = 0; i < 5; i++) { s->fiq[i] = operand_value(); s->usr[i] = operand_value(); }
    s->sctlr_extra = chance(80) ? ARM_SCTLR_U : (chance(50) ? ARM_SCTLR_A : 0u);
    /* VFP registers: random bits, the integer corner cases, and the
     * floating-point ones -- signalling and quiet NaNs, infinities,
     * denormals, signed zeros -- as singles and as the high word of a
     * double, where the classification lives. */
    static const uint32_t fp_special[] = {
        0x7f800001u, 0xffa00000u, 0x7fc00000u, 0xffc00001u, 0x7f800000u, 0xff800000u,
        0x00000001u, 0x807fffffu, 0x00800000u, 0x80000000u, 0x3f800000u, 0x7f7fffffu,
        0x7ff00000u, 0x7ff40000u, 0x7ff80000u, 0xfff00000u, 0x00080000u, 0x80000000u,
    };
    for (int i = 0; i < 64; i++)                     /* s0-s31, then d16-d31 */
        s->s[i] = chance(40) ? rnd() : chance(50) ? interesting()
                : fp_special[rnd_n((uint32_t)(sizeof fp_special / sizeof fp_special[0]))];
    /* Mostly the default FP environment; sometimes flags, a directed
     * rounding mode, flush-to-zero or default NaN; rarely VFP switched off,
     * which takes the lazy-enable Undefined path. */
    s->fpscr = chance(60) ? 0u : (rnd() & (0xf0000000u | ARM_FPSCR_RMODE | (3u << 24)));
    /* Occasionally a short vector, a stride or a trap enable, one at a time
     * so each reaches the arithmetic on its own: the engine's decoded VFP
     * forms must hand every one of these to the reference. */
    if (chance(12)) switch (rnd_n(4u)) {
        case 0:  s->fpscr |= rnd() & ARM_FPSCR_LEN; break;                 /* stride 1 */
        case 1:  s->fpscr |= (rnd() & ARM_FPSCR_LEN) | (3u << 20); break;  /* stride 2 */
        case 2:  s->fpscr |= rnd() & ARM_FPSCR_STRIDE; break;              /* no LEN   */
        default: s->fpscr |= (1u << (8u + rnd_n(8u))) & ARM_FPSCR_ENABLES; break;
    }
    s->fpexc = chance(90) ? ARM_FPEXC_EN : 0u;
    /* Mostly full CP10/CP11 access; sometimes privileged-only, denied or
     * the reserved encoding, in either half. */
    s->cpacr = chance(90) ? 0xfu : rnd_n(16u);
    s->cycles = rnd();
    s->excl_valid = chance(30);
    s->excl_addr = DATA_VA + (rnd_n(DATA_BYTES) & ~3u);
    for (int i = 0; i < 3; i++) s->tid[i] = rnd();
    s->irq_pending = chance(25);
}

static void apply_state(s5l8900_t *m, const state_t *s) {
    arm_cpu_t *c = &m->cpu;
    c->cpsr = (c->cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_SVC;
    arm_set_mode(c, s->cpsr);                         /* bank first, then values */
    c->cpsr = s->cpsr;
    for (int i = 0; i < 16; i++) c->r[i] = s->r[i];
    arm_bank_t cur = arm_bank_of_mode(s->cpsr);
    (void)cur;
    /* Every bank, live or parked, so nothing survives from an earlier case. */
    for (int i = 0; i < ARM_BANK_COUNT; i++) {
        c->spsr[i] = s->spsr[i];
        c->bank_r13[i] = s->b13[i];
        c->bank_r14[i] = s->b14[i];
    }
    for (int i = 0; i < 5; i++) { c->usr_r8_12[i] = s->usr[i]; c->fiq_r8_12[i] = s->fiq[i]; }
    c->cp15.sctlr = ARM_SCTLR_M | ARM_SCTLR_XP | s->sctlr_extra;
    c->cp15.ttbr0 = L1_PA; c->cp15.ttbcr = 0; c->cp15.dacr = 0x1u;
    c->cp15.cpacr = s->cpacr << ARM_CPACR_CP10_SHIFT;
    c->vfp_fpexc = s->fpexc;
    c->vfp_fpscr = s->fpscr;
    for (int i = 0; i < 64; i++) c->vfp_s[i] = s->s[i];
    c->cp15.dfsr = c->cp15.dfar = c->cp15.ifsr = c->cp15.ifar = 0;
    c->cycles = s->cycles;
    c->excl_valid = s->excl_valid;
    c->excl_addr = s->excl_addr;
    c->abort_pending = false;
    c->cp15.tpidrurw = s->tid[0];
    c->cp15.tpidruro = s->tid[1];
    c->cp15.tpidrprw = s->tid[2];
    c->cp15.context_id = 0;
    arm_mmu_tlb_flush(c);
    /* A real, level-asserted IRQ (VIC0 software interrupt, line 5) or none,
     * so unmasking with something pending is exercised. The previous case's
     * VIC state is cleared either way. */
    m->bus.write32(m->bus.ctx, S5L8900_VIC0_BASE + VIC_INTENCLEAR, 0xffffffffu);
    m->bus.write32(m->bus.ctx, S5L8900_VIC0_BASE + VIC_SOFTINTCLEAR, 0xffffffffu);
    m->bus.write32(m->bus.ctx, S5L8900_VIC0_BASE + VIC_INTSELECT, 0u);
    if (s->irq_pending) {
        m->bus.write32(m->bus.ctx, S5L8900_VIC0_BASE + VIC_INTENABLE, 1u << 5);
        m->bus.write32(m->bus.ctx, S5L8900_VIC0_BASE + VIC_SOFTINT, 1u << 5);
    }
    s5l8900_tick(m, 0u);
}

static int compare(const s5l8900_t *a, const s5l8900_t *b, char *why, size_t n) {
    const arm_cpu_t *x = &a->cpu, *y = &b->cpu;
#define DIFF(field, fmt) do { if (x->field != y->field) { \
        snprintf(why, n, #field " ref=" fmt " ci=" fmt, x->field, y->field); return 1; } } while (0)
    for (int i = 0; i < 16; i++)
        if (x->r[i] != y->r[i]) { snprintf(why, n, "r%d ref=%08x ci=%08x", i, x->r[i], y->r[i]); return 1; }
    DIFF(cpsr, "%08x");
    for (int i = 0; i < ARM_BANK_COUNT; i++) {
        DIFF(spsr[i], "%08x"); DIFF(bank_r13[i], "%08x"); DIFF(bank_r14[i], "%08x");
    }
    for (int i = 0; i < 5; i++) { DIFF(fiq_r8_12[i], "%08x"); DIFF(usr_r8_12[i], "%08x"); }
    DIFF(cycles, "%" PRIu64);
    DIFF(excl_valid, "%d"); DIFF(excl_addr, "%08x");
    DIFF(cp15.dfsr, "%08x"); DIFF(cp15.dfar, "%08x");
    DIFF(cp15.ifsr, "%08x"); DIFF(cp15.ifar, "%08x");
    DIFF(cp15.tpidrurw, "%08x"); DIFF(cp15.tpidruro, "%08x");
    DIFF(cp15.tpidrprw, "%08x"); DIFF(cp15.context_id, "%08x");
    DIFF(cp15.fcse_pid, "%08x"); DIFF(cp15.sctlr, "%08x");
    DIFF(irq_line, "%d"); DIFF(fiq_line, "%d");
    DIFF(abort_pending, "%d");
    DIFF(vfp_fpscr, "%08x"); DIFF(vfp_fpexc, "%08x");
    for (int i = 0; i < 64; i++) DIFF(vfp_s[i], "%08x");
#undef DIFF
    const uint8_t *ma = a->ram + (PHYS(DATA_VA) - RAM_BASE);
    const uint8_t *mb = b->ram + (PHYS(DATA_VA) - RAM_BASE);
    for (uint32_t i = 0; i < DATA_BYTES + 0x1000u; i++)
        if (ma[i] != mb[i]) {
            snprintf(why, n, "mem[%08x] ref=%02x ci=%02x", DATA_VA + i, ma[i], mb[i]);
            return 1;
        }
    return 0;
}

int main(int argc, char **argv) {
    unsigned cases = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 20000u;
    g_rng = argc > 2 ? strtoull(argv[2], NULL, 0) : 0x9e3779b97f4a7c15ull;
    static s5l8900_t ref, ci;
    if (!machine_setup(&ref, false) || !machine_setup(&ci, true)) {
        printf("machine setup failed\n");
        return 1;
    }
    unsigned failures = 0, runs = 0;
    uint64_t retired_total = 0;
    uint8_t data_init[DATA_BYTES + 0x1000u];

    /* ARM, Thumb, ARM again with the VFP-heavy mix (half as many), then the
     * Cortex-A8 profile: Thumb-2, and ARM state (half as many), where ARMv7
     * changes what an ALU write of PC and the hint space mean. */
    for (int isa = 0; isa < 5; isa++) {
        const bool thumb2 = isa == 3, armv7 = isa == 4;
        const bool thumb = isa == 1 || thumb2;
        g_vfp_heavy = isa == 2;
        g_neon = thumb2 || armv7;
        const unsigned count = (g_vfp_heavy || armv7) ? cases / 2u : cases;
        const arm_arch_t arch = (thumb2 || armv7) ? ARM_ARCH_V7_A8 : ARM_ARCH_V6_ARM1176;
        ref.cpu.arch = arch;
        ci.cpu.arch = arch;
        arm_ci_stats_t before;
        arm_ci_get_stats(ci.ci, &before);
        const uint64_t retired_before = retired_total;
        const unsigned failures_before = failures;
        for (unsigned k = 0; k < count; k++) {
            const uint64_t case_seed = g_rng;
            const unsigned len = 1u + rnd_n(12u);
            uint32_t words[64];
            unsigned nwords = 0;
            if (!thumb) {
                for (unsigned i = 0; i < len; i++) words[nwords++] = gen_arm(i, len);
            } else if (thumb2) {
                uint16_t h[40];
                unsigned nh = 0;
                while (nh < len + 4u && nh < 36u) nh += gen_thumb2(nh, len + 4u, h + nh);
                if (nh & 1u) h[nh++] = 0x46c0u;
                for (unsigned i = 0; i < nh; i += 2u) words[nwords++] = h[i] | ((uint32_t)h[i + 1u] << 16);
            } else {
                uint16_t h[40];
                unsigned nh = 0;
                while (nh < len) {
                    bool pair;
                    h[nh] = gen_thumb(nh, len, &pair);
                    nh++;
                    if (pair && nh < 39u) h[nh++] = (uint16_t)((chance(80) ? 0xf800u : 0xe800u) |
                                                               (rnd() & 0x7ffu));
                }
                if (nh & 1u) h[nh++] = 0x46c0u;               /* NOP to fill the word */
                for (unsigned i = 0; i < nh; i += 2u) words[nwords++] = h[i] | ((uint32_t)h[i + 1u] << 16);
            }
            /* Code after the sequence: more NOPs, then a branch to self. */
            uint32_t code_pa = PHYS(CODE_VA) + (rnd_n(0x300u) & ~3u);
            uint32_t pc0 = CODE_VA + (code_pa - PHYS(CODE_VA));
            for (uint32_t i = 0; i < 64u; i++) {
                uint32_t w = i < nwords ? words[i] : (thumb ? 0x46c046c0u : 0xe1a00000u);
                poke32(&ref, code_pa + i * 4u, w);
                poke32(&ci, code_pa + i * 4u, w);
            }
            /* Random stores may have hit the vector page; exceptions should
             * land on NOPs again (both machines, through the bus, so the
             * engine also has to drop any block it cached there). */
            for (uint32_t i = 0; i < 64u; i++) {
                poke32(&ref, PHYS(VEC_VA) + i * 4u, 0xe1a00000u);
                poke32(&ci, PHYS(VEC_VA) + i * 4u, 0xe1a00000u);
            }
            for (uint32_t i = 0; i < sizeof data_init; i++)
                data_init[i] = (uint8_t)(chance(30) ? interesting() : rnd());
            /* Some words are code addresses (ARM, Thumb, or misaligned), so
             * loads into the PC land somewhere meaningful. */
            for (uint32_t i = 0; i < sizeof data_init; i += 4u) {
                if (!chance(15)) continue;
                uint32_t w = CODE_VA + (rnd_n(0x400u) & ~3u) + rnd_n(4u);
                memcpy(data_init + i, &w, 4);
            }
            for (uint32_t i = 0; i < sizeof data_init; i += 4u) {
                uint32_t w;
                memcpy(&w, data_init + i, 4);
                poke32(&ref, PHYS(DATA_VA) + i, w);
                poke32(&ci, PHYS(DATA_VA) + i, w);
            }
            state_t st;
            random_state(&st, thumb);
            st.r[15] = pc0;
            if (thumb2 || armv7) st.sctlr_extra |= ARM_SCTLR_U;  /* ARMv7: RAO */
            if (thumb2) {
                /* Sometimes enter with an IT block in progress (as after an
                 * interrupt inside one), and give the SPSRs ITSTATE for
                 * exception returns to restore. */
                if (chance(15)) {
                    const uint32_t it = (rnd_n(14u) << 4) | (1u + rnd_n(15u));
                    st.cpsr |= ((it & 0xfcu) << 8) | ((it & 3u) << 25);
                }
                for (int i = 0; i < ARM_BANK_COUNT; i++)
                    if (chance(20) && (st.spsr[i] & ARM_CPSR_T)) {
                        const uint32_t it = (rnd_n(14u) << 4) | (1u + rnd_n(15u));
                        st.spsr[i] |= ((it & 0xfcu) << 8) | ((it & 3u) << 25);
                    }
            }
            apply_state(&ref, &st);
            apply_state(&ci, &st);

            unsigned steps = 1u + rnd_n(3u * len + 4u);
            arm_status_t sa = ARM_OK, sb = ARM_OK;
            unsigned na = s5l8900_run(&ref, steps, &sa);
            unsigned nb = s5l8900_run(&ci, steps, &sb);
            runs++;
            retired_total += na;
            char why[160] = "";
            int bad = (na != nb) || (sa != sb);
            if (bad) snprintf(why, sizeof why, "retired ref=%u ci=%u status ref=%d ci=%d",
                              na, nb, (int)sa, (int)sb);
            else bad = compare(&ref, &ci, why, sizeof why);
            if (bad) {
                failures++;
                if (failures <= 20u) {
                    printf("MISMATCH %s case %u seed=0x%016" PRIx64 " steps=%u: %s\n",
                           thumb2 ? "thumb2" : armv7 ? "arm-v7" : thumb ? "thumb"
                                  : g_vfp_heavy ? "arm-vfp" : "arm",
                           k, case_seed, steps, why);
                    printf("  code @%08x:", pc0);
                    for (unsigned i = 0; i < nwords; i++) printf(" %08x", words[i]);
                    printf("\n  cpsr0=%08x sctlr+=%08x\n", st.cpsr, st.sctlr_extra);
                }
            }
        }
        arm_ci_stats_t after;
        arm_ci_get_stats(ci.ci, &after);
        printf("  %-7s %6u runs, %7" PRIu64 " instructions, %u mismatches; engine %" PRIu64
               " retired (%" PRIu64 " via reference)\n",
               thumb2 ? "thumb2" : armv7 ? "arm-v7" : thumb ? "thumb"
                      : g_vfp_heavy ? "arm-vfp" : "arm",
               count, retired_total - retired_before, failures - failures_before,
               after.retired - before.retired, after.ref_retired - before.ref_retired);
    }
    arm_ci_stats_t cs;
    arm_ci_get_stats(ci.ci, &cs);
    printf("ci differential: %u runs, %" PRIu64 " instructions, %u mismatches "
           "(engine: %" PRIu64 " retired, %" PRIu64 " via reference, %" PRIu64
           " builds, %" PRIu64 " invalidations)\n",
           runs, retired_total, failures, cs.retired, cs.ref_retired, cs.builds,
           cs.invalidations);
    s5l8900_free(&ref);
    s5l8900_free(&ci);
    return failures ? 1 : 0;
}
