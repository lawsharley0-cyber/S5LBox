/*
 * S5LBox — Advanced SIMD (NEON) on the Cortex-A8 profile.
 *
 * WHY THIS EXISTS
 * ---------------
 * iOS 6 is built for ARMv7 with NEON, and uses it well outside maths code:
 * libSystem's memcpy and string routines move data through Q registers, and
 * CoreGraphics, ImageIO and the audio stack are full of it. The iPhone 3GS's
 * Cortex-A8 has it; the ARM1176 does not, and on that profile every encoding
 * here stays refused (the interpreter only calls in for an ARMv7 core).
 *
 * WHAT IS MODELLED
 * ----------------
 * Every Advanced SIMD data-processing instruction of ARMv7 (ARM ARM A7.4):
 * three registers of the same and of different lengths, two registers and a
 * scalar, two registers and a shift amount, one register and a modified
 * immediate, the miscellaneous two-register group, VEXT, VTBL/VTBX and VDUP
 * (scalar). The element and structure loads and stores (A7.7) are
 * neon_ldst(). Instructions are given in their ARM encoding; the Thumb
 * encodings differ only in the top byte and arm_interp.c maps them.
 *
 * Integer semantics are exact by construction (64-bit host arithmetic, with
 * the 64-bit element cases written out so nothing overflows). FPSCR.QC is set
 * by every saturating operation that saturates, and nothing else writes it.
 *
 * Floating point is binary32 only, as ARMv7 NEON is, and runs under ARM's
 * StandardFPSCRValue(): flush-to-zero and default NaN on, round to nearest,
 * whatever FPSCR says -- except that the cumulative exception flags do
 * accumulate in FPSCR. Every rounding step is the VFP unit's own
 * (vfp_std_f32), so NEON inherits its flag sampling, its NaN rules and its
 * refusal of the one flush-to-zero case the host cannot decide. VMLA and
 * VMLS are not fused: a multiply, rounded, then an add, rounded.
 *
 * WHAT IS REFUSED (vfp_refuse: the machine stops and says why)
 * ------------------------------------------------------------
 *   - UNDEFINED encodings, as the VFP unit does: this emulator stops on an
 *     encoding the guest cannot have meant rather than handing it an
 *     Undefined exception its own handler would treat as a crash.
 *   - UNPREDICTABLE forms: a VTBL table running past d31, VZIP/VUZP/VTRN
 *     with one register as both operands, a zero modified immediate where
 *     the encoding requires a nonzero one.
 *   - FPSCR.Len or Stride nonzero. Advanced SIMD does not use VFP short
 *     vectors, and a guest running NEON code inside VFP vector mode is
 *     outside anything this model was checked against.
 *   - The half-precision conversions and VFMA/VFMS: the Cortex-A8 has
 *     neither (MVFR1).
 *
 * HOW IT WAS CHECKED
 * ------------------
 * tools/unicorn_neon_diff.py runs random encodings from every group against
 * Unicorn's Cortex-A8, in ARM and Thumb state, comparing the core registers,
 * FPSCR and d0-d31. core/tests/test_armv7.c pins known answers.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "vfp.h"
#include "vfp_private.h"

#include <math.h>
#include <string.h>

#define BIT(i) ((insn >> (i)) & 1u)

/* D register numbers: the extra bit is the TOP bit, as for VFPv3 doubles. */
#define VD (((insn >> 18) & 0x10u) | ((insn >> 12) & 0xfu))
#define VN (((insn >> 3) & 0x10u)  | ((insn >> 16) & 0xfu))
#define VM (((insn >> 1) & 0x10u)  | (insn & 0xfu))

static arm_status_t undef(uint32_t pc, uint32_t insn, const char *why) {
    return vfp_refuse(pc, insn, why);
}

/* ============================================================ elements == */

static inline uint64_t rdd(const arm_cpu_t *c, unsigned n) { return vfp_get_d(c, n); }
static inline void wrd(arm_cpu_t *c, unsigned n, uint64_t v) { vfp_set_d(c, n, v); }

static inline uint64_t emask(unsigned esize) {
    return esize >= 64u ? ~0ull : (1ull << esize) - 1u;
}
static inline uint64_t el(uint64_t v, unsigned e, unsigned esize) {
    return esize >= 64u ? v : (v >> (e * esize)) & emask(esize);
}
static inline uint64_t setel(uint64_t v, unsigned e, unsigned esize, uint64_t x) {
    if (esize >= 64u) return x;
    const unsigned sh = e * esize;
    const uint64_t m = emask(esize) << sh;
    return (v & ~m) | ((x << sh) & m);
}
/* Sign-extend the low `esize` bits. */
static inline int64_t sx(uint64_t x, unsigned esize) {
    if (esize >= 64u) return (int64_t)x;
    const uint64_t sign = 1ull << (esize - 1u);
    x &= emask(esize);
    return (int64_t)((x ^ sign) - sign);
}
/* The element as a signed or unsigned integer, widened to 64 bits. */
static inline int64_t ext(uint64_t x, unsigned esize, bool uns) {
    return uns ? (int64_t)(x & emask(esize)) : sx(x, esize);
}

/* ---------------------------------------------------------- saturation --
 * For esize < 64 the exact result always fits an int64, so saturating is a
 * clamp. The 64-bit element forms have their own helpers below. */
static uint64_t sat_s(int64_t v, unsigned esize, bool *qc) {
    const int64_t hi = (int64_t)(emask(esize) >> 1), lo = -hi - 1;
    if (v > hi) { *qc = true; v = hi; }
    else if (v < lo) { *qc = true; v = lo; }
    return (uint64_t)v & emask(esize);
}
static uint64_t sat_u(int64_t v, unsigned esize, bool *qc) {
    if (v < 0) { *qc = true; return 0; }
    if ((uint64_t)v > emask(esize)) { *qc = true; return emask(esize); }
    return (uint64_t)v;
}
static uint64_t sat_uu(uint64_t v, unsigned esize, bool *qc) {
    if (v > emask(esize)) { *qc = true; return emask(esize); }
    return v;
}
static uint64_t qadd64(uint64_t a, uint64_t b, bool uns, bool *qc) {
    const uint64_t r = a + b;
    if (uns) {
        if (r < a) { *qc = true; return ~0ull; }
        return r;
    }
    if (((a ^ r) & (b ^ r)) >> 63) {
        *qc = true;
        return (a >> 63) ? 0x8000000000000000ull : 0x7fffffffffffffffull;
    }
    return r;
}
static uint64_t qsub64(uint64_t a, uint64_t b, bool uns, bool *qc) {
    const uint64_t r = a - b;
    if (uns) {
        if (a < b) { *qc = true; return 0; }
        return r;
    }
    if (((a ^ b) & (a ^ r)) >> 63) {
        *qc = true;
        return (a >> 63) ? 0x8000000000000000ull : 0x7fffffffffffffffull;
    }
    return r;
}

/* ------------------------------------------------------------- shifts --
 *
 * The one shift the architecture's Advanced SIMD pseudocode keeps using:
 * an esize-bit element, signed or unsigned, shifted LEFT by sh (a negative
 * sh shifts right), optionally rounding the right shift and optionally
 * saturating the left one. Shift amounts run from -128 to 127 in the
 * register forms, so every out-of-range case is spelled out. The result is
 * truncated to esize bits; right shifts never overflow and never saturate.
 *
 * The rounding identity used: (x + 2^(k-1)) >> k == (x >> k) + bit k-1 of x,
 * for floor (arithmetic) shifts, which is what keeps the 64-bit case exact.
 */
static uint64_t shift_el(uint64_t v, int sh, unsigned esize, bool uns,
                         bool round, bool sat, bool *qc) {
    const uint64_t mask = emask(esize);
    if (sh == 0) return v & mask;
    if (sh > 0) {
        bool over;
        uint64_t r;
        if (uns) {
            const uint64_t x = v & mask;
            if ((unsigned)sh >= esize) { over = x != 0; r = 0; }
            else { over = (x >> (esize - (unsigned)sh)) != 0; r = (x << sh) & mask; }
            if (sat && over) { *qc = true; return mask; }
            return r;
        } else {
            const int64_t x = sx(v, esize);
            if ((unsigned)sh >= esize) { over = x != 0; r = 0; }
            else {
                r = ((uint64_t)x << sh) & mask;
                over = (sx(r, esize) >> sh) != x;
            }
            if (sat && over) {
                *qc = true;
                return (x < 0 ? ~(mask >> 1) : (mask >> 1)) & mask;
            }
            return r;
        }
    }
    {
        const unsigned k = (unsigned)-sh;
        if (uns) {
            const uint64_t x = v & mask;
            if (k > 64u) return 0;
            if (k == 64u) return round ? (x >> 63) : 0;
            return ((x >> k) + (round ? ((x >> (k - 1u)) & 1u) : 0u)) & mask;
        } else {
            const int64_t x = sx(v, esize);
            if (k >= 64u) return round ? 0 : (x < 0 ? mask : 0);
            return (uint64_t)((x >> k) + (round ? ((x >> (k - 1u)) & 1) : 0)) & mask;
        }
    }
}

/* Polynomial (carry-less) multiply of two bytes; up to 15 bits. */
static uint64_t pmul8(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    for (unsigned i = 0; i < 8u; i++)
        if ((b >> i) & 1u) r ^= (a & 0xffu) << i;
    return r;
}

/* ============================================================ float ===== */

static inline float    u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline bool isnan32(uint32_t u) { return (u & 0x7fffffffu) > 0x7f800000u; }
static inline bool snan32(uint32_t u) { return isnan32(u) && !(u & 0x00400000u); }
static inline bool iszero32(uint32_t u) { return (u & 0x7fffffffu) == 0u; }
static inline bool isinf32(uint32_t u) { return (u & 0x7fffffffu) == 0x7f800000u; }
#define F32_DEFAULT_NAN 0x7fc00000u

/*
 * FPCompareEQ/GE/GT under the standard FPSCR: operands unpacked (flushed,
 * IDC), a NaN compares false, and raises IOC if it is signalling (EQ) or at
 * all (GE, GT: signalling comparisons).
 */
enum { CMP_EQ, CMP_GE, CMP_GT };
static bool fcmp(unsigned kind, uint32_t a, uint32_t b, uint32_t *exc) {
    a = vfp_std_flush32(a, exc);
    b = vfp_std_flush32(b, exc);
    if (isnan32(a) || isnan32(b)) {
        if (kind != CMP_EQ || snan32(a) || snan32(b)) *exc |= ARM_FPSCR_IOC;
        return false;
    }
    const float x = u2f(a), y = u2f(b);
    return kind == CMP_EQ ? x == y : kind == CMP_GE ? x >= y : x > y;
}

/* FPMax / FPMin: NaN in, default NaN out (IOC if signalling); +0 and -0
 * are ordered, max(+0,-0) = +0 and min(+0,-0) = -0. */
static uint32_t fmaxmin(uint32_t a, uint32_t b, bool max, uint32_t *exc) {
    a = vfp_std_flush32(a, exc);
    b = vfp_std_flush32(b, exc);
    if (isnan32(a) || isnan32(b)) {
        if (snan32(a) || snan32(b)) *exc |= ARM_FPSCR_IOC;
        return F32_DEFAULT_NAN;
    }
    if (iszero32(a) && iszero32(b))
        return max ? (a & b) : (a | b);                   /* the sign bits */
    const float x = u2f(a), y = u2f(b);
    return ((x > y) == max) ? a : b;
}

/*
 * FPRecipStep (VRECPS, 2 - a*b) and FPRSqrtStep (VRSQRTS, (3 - a*b) / 2).
 * The product of an infinity and a zero is taken as +0 rather than an
 * Invalid Operation, so the step returns exactly 2.0 or 1.5.
 */
static uint32_t frecps(uint32_t a, uint32_t b, bool rsqrt, uint32_t *exc) {
    a = vfp_std_flush32(a, exc);
    b = vfp_std_flush32(b, exc);
    if (isnan32(a) || isnan32(b)) {
        if (snan32(a) || snan32(b)) *exc |= ARM_FPSCR_IOC;
        return F32_DEFAULT_NAN;
    }
    uint32_t product;
    if ((isinf32(a) && iszero32(b)) || (iszero32(a) && isinf32(b)))
        product = 0u;
    else
        product = vfp_std_f32(VFP_OP_MUL, a, b, exc);
    if (!rsqrt) return vfp_std_f32(VFP_OP_SUB, 0x40000000u, product, exc);
    /* (3 - p) / 2 with one rounding: the subtraction rounds, and halving a
     * result this far from the denormal range is exact. */
    const uint32_t diff = vfp_std_f32(VFP_OP_SUB, 0x40400000u, product, exc);
    return vfp_std_f32(VFP_OP_MUL, diff, 0x3f000000u, exc);
}

/*
 * ARM ARM recip_estimate() and recip_sqrt_estimate(): the C functions the
 * pseudocode itself calls, returning a multiple of 1/256 in [1, 2) as
 * s = 256 * that. Host double arithmetic is IEEE and sqrt is correctly
 * rounded, so these are exact on every host this builds for.
 */
static unsigned recip_estimate_256(double a) {
    const int q = (int)(a * 512.0);
    const double r = 1.0 / (((double)q + 0.5) / 512.0);
    return (unsigned)(int)(256.0 * r + 0.5);
}
static unsigned rsqrt_estimate_256(double a) {
    double r;
    if (a < 0.5) {
        const int q0 = (int)(a * 512.0);
        r = 1.0 / sqrt(((double)q0 + 0.5) / 512.0);
    } else {
        const int q1 = (int)(a * 256.0);
        r = 1.0 / sqrt(((double)q1 + 0.5) / 256.0);
    }
    return (unsigned)(int)(256.0 * r + 0.5);
}
/* A binary64 in [0.25, 1) from explicit exponent and fraction bits. */
static double mkdouble(uint64_t exp11, uint64_t frac52) {
    const uint64_t u = (exp11 << 52) | frac52;
    double d;
    memcpy(&d, &u, 8);
    return d;
}

/* FPRecipEstimate (ARMv7, standard FPSCR). */
static uint32_t frecpe(uint32_t a, uint32_t *exc) {
    a = vfp_std_flush32(a, exc);
    if (isnan32(a)) {
        if (snan32(a)) *exc |= ARM_FPSCR_IOC;
        return F32_DEFAULT_NAN;
    }
    if (isinf32(a)) return a & 0x80000000u;
    if (iszero32(a)) { *exc |= ARM_FPSCR_DZC; return (a & 0x80000000u) | 0x7f800000u; }
    const unsigned exp = (a >> 23) & 0xffu;
    if (exp >= 253u) { *exc |= ARM_FPSCR_UFC; return a & 0x80000000u; }
    const unsigned s = recip_estimate_256(
        mkdouble(1022u, (uint64_t)(a & 0x7fffffu) << 29));
    return (a & 0x80000000u) | ((253u - exp) << 23) | ((s - 256u) << 15);
}

/* FPRSqrtEstimate (ARMv7, standard FPSCR). */
static uint32_t frsqrte(uint32_t a, uint32_t *exc) {
    a = vfp_std_flush32(a, exc);
    if (isnan32(a)) {
        if (snan32(a)) *exc |= ARM_FPSCR_IOC;
        return F32_DEFAULT_NAN;
    }
    if (iszero32(a)) { *exc |= ARM_FPSCR_DZC; return (a & 0x80000000u) | 0x7f800000u; }
    if (a & 0x80000000u) { *exc |= ARM_FPSCR_IOC; return F32_DEFAULT_NAN; }
    if (isinf32(a)) return 0u;
    const unsigned exp = (a >> 23) & 0xffu;
    /* An even exponent (operand<23> clear) scales into [0.5, 1), an odd
     * one into [0.25, 0.5), so the square root's exponent halves exactly. */
    const double scaled = mkdouble((exp & 1u) ? 1021u : 1022u,
                                   (uint64_t)(a & 0x7fffffu) << 29);
    const unsigned s = rsqrt_estimate_256(scaled);
    return (((380u - exp) / 2u) << 23) | ((s - 256u) << 15);
}

/* UnsignedRecipEstimate / UnsignedRSqrtEstimate (VRECPE.U32, VRSQRTE.U32). */
static uint32_t urecpe(uint32_t a) {
    if (!(a & 0x80000000u)) return 0xffffffffu;
    const unsigned s = recip_estimate_256(
        mkdouble(1022u, (uint64_t)(a & 0x7fffffffu) << 21));
    return 0x80000000u | ((s - 256u) << 23);
}
static uint32_t ursqrte(uint32_t a) {
    if (!(a & 0xc0000000u)) return 0xffffffffu;
    const double d = (a & 0x80000000u)
        ? mkdouble(1022u, (uint64_t)(a & 0x7fffffffu) << 21)
        : mkdouble(1021u, (uint64_t)(a & 0x3fffffffu) << 22);
    return 0x80000000u | ((rsqrt_estimate_256(d) - 256u) << 23);
}

/* Float to 32-bit fixed point (round toward zero, saturating) and back
 * (round to nearest), the NEON VCVT forms; frac is 0 for the integer ones. */
static uint32_t f2fixed(uint32_t a, unsigned frac, bool uns, uint32_t *exc) {
    a = vfp_std_flush32(a, exc);
    return vfp_std_to_fixed32((double)u2f(a), frac, !uns, exc);
}
static uint32_t fixed2f(uint32_t a, unsigned frac, bool uns, uint32_t *exc) {
    const double v = uns ? (double)a : (double)(int32_t)a;
    return vfp_std_round32(ldexp(v, -(int)frac), exc);
}

/* Commit the exceptions a float instruction raised, or refuse it if one
 * element hit the flush-to-zero case the host cannot decide. Nothing has
 * been written yet when this is asked. */
static bool fp_ambiguous(uint32_t exc) { return (exc & VFP_FZ_AMBIGUOUS) != 0u; }
static void fp_commit(arm_cpu_t *c, uint32_t exc) {
    c->vfp_fpscr |= exc & (ARM_FPSCR_IDC | ARM_FPSCR_IXC | ARM_FPSCR_UFC |
                           ARM_FPSCR_OFC | ARM_FPSCR_DZC | ARM_FPSCR_IOC);
}
static void qc_commit(arm_cpu_t *c, bool qc) { if (qc) c->vfp_fpscr |= ARM_FPSCR_QC; }

/* ================================================ three registers, same == */

enum {
    I_HADD, I_QADD, I_RHADD, I_HSUB, I_QSUB, I_CGT, I_CGE, I_SHL, I_QSHL,
    I_RSHL, I_QRSHL, I_MAX, I_MIN, I_ABD, I_ABA, I_ADD, I_SUB, I_TST, I_CEQ,
    I_MLA, I_MLS, I_MUL, I_PMUL, I_QDMULH, I_QRDMULH, I_PADD, I_PMAX, I_PMIN
};

/* One integer element. a is Dn's element and b Dm's, except for the shifts,
 * where a is the value (Dm) and b the shift (Dn). acc is Dd's element. */
static uint64_t int_op(unsigned k, uint64_t a, uint64_t b, uint64_t acc,
                       unsigned esize, bool uns, bool *qc) {
    const uint64_t mask = emask(esize);
    if (esize == 64u) {
        switch (k) {
        case I_QADD: return qadd64(a, b, uns, qc);
        case I_QSUB: return qsub64(a, b, uns, qc);
        case I_ADD:  return a + b;
        case I_SUB:  return a - b;
        default: break;           /* the shifts, below */
        }
    }
    switch (k) {
    case I_SHL:   return shift_el(a, (int8_t)b, esize, uns, false, false, qc);
    case I_QSHL:  return shift_el(a, (int8_t)b, esize, uns, false, true, qc);
    case I_RSHL:  return shift_el(a, (int8_t)b, esize, uns, true, false, qc);
    case I_QRSHL: {
        const int sh = (int8_t)b;
        return shift_el(a, sh, esize, uns, sh < 0, sh > 0, qc);
    }
    case I_ADD: case I_PADD: return (a + b) & mask;
    case I_SUB:  return (a - b) & mask;
    case I_TST:  return (a & b & mask) ? mask : 0;
    case I_CEQ:  return ((a & mask) == (b & mask)) ? mask : 0;
    case I_MLA:  return (acc + a * b) & mask;
    case I_MLS:  return (acc - a * b) & mask;
    case I_MUL:  return (a * b) & mask;
    case I_PMUL: return pmul8(a, b) & mask;
    default: break;
    }
    const int64_t x = ext(a, esize, uns), y = ext(b, esize, uns);
    switch (k) {
    case I_HADD:  return (uint64_t)((x + y) >> 1) & mask;
    case I_RHADD: return (uint64_t)((x + y + 1) >> 1) & mask;
    case I_HSUB:  return (uint64_t)((x - y) >> 1) & mask;
    case I_QADD:  return uns ? sat_u(x + y, esize, qc) : sat_s(x + y, esize, qc);
    case I_QSUB:  return uns ? sat_u(x - y, esize, qc) : sat_s(x - y, esize, qc);
    case I_CGT:   return x > y ? mask : 0;
    case I_CGE:   return x >= y ? mask : 0;
    case I_MAX: case I_PMAX: return (uint64_t)(x > y ? x : y) & mask;
    case I_MIN: case I_PMIN: return (uint64_t)(x < y ? x : y) & mask;
    case I_ABD:   return (uint64_t)(x > y ? x - y : y - x) & mask;
    case I_ABA:   return (acc + (uint64_t)(x > y ? x - y : y - x)) & mask;
    case I_QDMULH: case I_QRDMULH: {
        /* 2*x*y >> esize, rounded for VQRDMULH. x*y fits an int64 for
         * esize <= 32; only min*min saturates. */
        int64_t p = x * y;
        if (k == I_QRDMULH) p += (int64_t)1 << (esize - 2u);
        return sat_s(p >> (esize - 1u), esize, qc);
    }
    default: return 0;
    }
}

static arm_status_t neon_3same_float(arm_cpu_t *c, uint32_t pc, uint32_t insn) {
    const unsigned op = (insn >> 8) & 15u, d = VD, n = VN, m = VM;
    const bool b = BIT(4), u = BIT(24), q = BIT(6), op21 = BIT(21);
    const unsigned regs = q ? 2u : 1u;
    enum { F_ADD, F_SUB, F_PADD, F_ABD, F_MLA, F_MLS, F_MUL, F_CEQ, F_CGE,
           F_CGT, F_ACGE, F_ACGT, F_MAX, F_MIN, F_PMAX, F_PMIN, F_RECPS,
           F_RSQRTS } k;

    if (BIT(20)) return undef(pc, insn, "Advanced SIMD float with sz=1 is UNDEFINED");
    switch (op) {
    case 12u:
        return undef(pc, insn, (b && !u)
            ? "VFMA/VFMS: VFPv4, which the Cortex-A8 does not have"
            : "UNDEFINED Advanced SIMD three-register encoding");
    case 13u:
        if (!b) k = u ? (op21 ? F_ABD : F_PADD) : (op21 ? F_SUB : F_ADD);
        else if (!u) k = op21 ? F_MLS : F_MLA;
        else if (!op21) k = F_MUL;
        else return undef(pc, insn, "UNDEFINED Advanced SIMD three-register encoding");
        break;
    case 14u:
        if (!b && !u && !op21) k = F_CEQ;
        else if (!b && u) k = op21 ? F_CGT : F_CGE;
        else if (b && u) k = op21 ? F_ACGT : F_ACGE;
        else return undef(pc, insn, "UNDEFINED Advanced SIMD three-register encoding");
        break;
    default:
        if (!b) k = u ? (op21 ? F_PMIN : F_PMAX) : (op21 ? F_MIN : F_MAX);
        else if (!u) k = op21 ? F_RSQRTS : F_RECPS;
        else return undef(pc, insn, "UNDEFINED Advanced SIMD three-register encoding");
        break;
    }
    const bool pairwise = k == F_PADD || k == F_PMAX || k == F_PMIN;
    if (pairwise && q) return undef(pc, insn, "pairwise operation on Q registers is UNDEFINED");

    uint64_t N[2], M[2], D[2], R[2];
    uint32_t exc = 0;
    for (unsigned r = 0; r < regs; r++) {
        N[r] = rdd(c, n + r); M[r] = rdd(c, m + r); D[r] = rdd(c, d + r);
    }
    for (unsigned r = 0; r < regs; r++) {
        R[r] = 0;
        for (unsigned e = 0; e < 2u; e++) {
            uint32_t x = (uint32_t)el(N[r], e, 32), y = (uint32_t)el(M[r], e, 32);
            const uint32_t acc = (uint32_t)el(D[r], e, 32);
            uint32_t res;
            if (pairwise) {                /* D only: Dn's pair, then Dm's */
                const uint64_t src = e ? M[0] : N[0];
                x = (uint32_t)src; y = (uint32_t)(src >> 32);
            }
            switch (k) {
            case F_ADD: case F_PADD: res = vfp_std_f32(VFP_OP_ADD, x, y, &exc); break;
            case F_SUB:  res = vfp_std_f32(VFP_OP_SUB, x, y, &exc); break;
            case F_ABD:  res = vfp_std_f32(VFP_OP_SUB, x, y, &exc) & 0x7fffffffu; break;
            case F_MUL:  res = vfp_std_f32(VFP_OP_MUL, x, y, &exc); break;
            case F_MLA: case F_MLS: {
                uint32_t p = vfp_std_f32(VFP_OP_MUL, x, y, &exc);
                if (k == F_MLS) p ^= 0x80000000u;               /* FPNeg */
                res = vfp_std_f32(VFP_OP_ADD, acc, p, &exc);
                break;
            }
            case F_CEQ:  res = fcmp(CMP_EQ, x, y, &exc) ? ~0u : 0u; break;
            case F_CGE:  res = fcmp(CMP_GE, x, y, &exc) ? ~0u : 0u; break;
            case F_CGT:  res = fcmp(CMP_GT, x, y, &exc) ? ~0u : 0u; break;
            case F_ACGE: res = fcmp(CMP_GE, x & 0x7fffffffu, y & 0x7fffffffu, &exc) ? ~0u : 0u; break;
            case F_ACGT: res = fcmp(CMP_GT, x & 0x7fffffffu, y & 0x7fffffffu, &exc) ? ~0u : 0u; break;
            case F_MAX: case F_PMAX: res = fmaxmin(x, y, true, &exc); break;
            case F_MIN: case F_PMIN: res = fmaxmin(x, y, false, &exc); break;
            case F_RECPS:  res = frecps(x, y, false, &exc); break;
            default:       res = frecps(x, y, true, &exc); break;
            }
            R[r] = setel(R[r], e, 32, res);
        }
    }
    if (fp_ambiguous(exc)) return undef(pc, insn, VFP_FZ_AMBIGUOUS_WHY);
    for (unsigned r = 0; r < regs; r++) wrd(c, d + r, R[r]);
    fp_commit(c, exc);
    return ARM_OK;
}

static arm_status_t neon_3same(arm_cpu_t *c, uint32_t pc, uint32_t insn) {
    const unsigned op = (insn >> 8) & 15u, size = (insn >> 20) & 3u;
    const bool b = BIT(4), u = BIT(24), q = BIT(6);
    const unsigned d = VD, n = VN, m = VM, regs = q ? 2u : 1u;
    uint64_t N[2], M[2], D[2], R[2];
    unsigned k;

    if (q && ((d | n | m) & 1u))
        return undef(pc, insn, "Q register operand with an odd D number is UNDEFINED");
    if (op >= 12u) return neon_3same_float(c, pc, insn);
    for (unsigned r = 0; r < regs; r++) {
        N[r] = rdd(c, n + r); M[r] = rdd(c, m + r); D[r] = rdd(c, d + r);
    }

    if (op == 1u && b) {                                    /* bitwise */
        for (unsigned r = 0; r < regs; r++) {
            const uint64_t x = N[r], y = M[r], z = D[r];
            switch ((u << 2) | size) {
            case 0: R[r] = x & y;  break;                   /* VAND */
            case 1: R[r] = x & ~y; break;                   /* VBIC */
            case 2: R[r] = x | y;  break;                   /* VORR */
            case 3: R[r] = x | ~y; break;                   /* VORN */
            case 4: R[r] = x ^ y;  break;                   /* VEOR */
            case 5: R[r] = (x & z) | (y & ~z); break;       /* VBSL */
            case 6: R[r] = (z & ~y) | (x & y); break;       /* VBIT */
            default: R[r] = (z & y) | (x & ~y); break;      /* VBIF */
            }
        }
        for (unsigned r = 0; r < regs; r++) wrd(c, d + r, R[r]);
        return ARM_OK;
    }

    switch (op) {
    case 0:  k = b ? I_QADD : I_HADD; break;
    case 1:  k = I_RHADD; break;
    case 2:  k = b ? I_QSUB : I_HSUB; break;
    case 3:  k = b ? I_CGE : I_CGT; break;
    case 4:  k = b ? I_QSHL : I_SHL; break;
    case 5:  k = b ? I_QRSHL : I_RSHL; break;
    case 6:  k = b ? I_MIN : I_MAX; break;
    case 7:  k = b ? I_ABA : I_ABD; break;
    case 8:  k = b ? (u ? I_CEQ : I_TST) : (u ? I_SUB : I_ADD); break;
    case 9:  k = b ? (u ? I_PMUL : I_MUL) : (u ? I_MLS : I_MLA); break;
    case 10: k = b ? I_PMIN : I_PMAX; break;
    default:
        if (!b) k = u ? I_QRDMULH : I_QDMULH;
        else if (!u) k = I_PADD;
        else return undef(pc, insn, "UNDEFINED Advanced SIMD three-register encoding");
        break;
    }
    const bool any64 = k == I_QADD || k == I_QSUB || k == I_SHL || k == I_QSHL ||
                       k == I_RSHL || k == I_QRSHL || k == I_ADD || k == I_SUB;
    if (size == 3u && !any64)
        return undef(pc, insn, "64-bit elements are UNDEFINED for this operation");
    if (k == I_PMUL && size != 0u)
        return undef(pc, insn, "VMUL.P with elements other than 8 bits is UNDEFINED");
    if ((k == I_QDMULH || k == I_QRDMULH) && (size == 0u || size == 3u))
        return undef(pc, insn, "VQDMULH/VQRDMULH take 16- or 32-bit elements");
    const bool pairwise = k == I_PADD || k == I_PMAX || k == I_PMIN;
    if (pairwise && q) return undef(pc, insn, "pairwise operation on Q registers is UNDEFINED");

    const unsigned esize = 8u << size, elems = 64u / esize;
    const bool shift = k == I_SHL || k == I_QSHL || k == I_RSHL || k == I_QRSHL;
    /* U is signedness for most of this group, but for VQDMULH/VQRDMULH it
     * picks the rounding form and both are signed. */
    const bool uns = u && k != I_QDMULH && k != I_QRDMULH;
    bool qc = false;
    for (unsigned r = 0; r < regs; r++) {
        R[r] = 0;
        for (unsigned e = 0; e < elems; e++) {
            uint64_t x, y;
            if (pairwise) {
                const unsigned h = elems / 2u;
                const uint64_t src = e < h ? N[0] : M[0];
                const unsigned i = (e < h ? e : e - h) * 2u;
                x = el(src, i, esize); y = el(src, i + 1u, esize);
            } else if (shift) {
                x = el(M[r], e, esize); y = el(N[r], e, esize);
            } else {
                x = el(N[r], e, esize); y = el(M[r], e, esize);
            }
            R[r] = setel(R[r], e, esize,
                         int_op(k, x, y, el(D[r], e, esize), esize, uns, &qc));
        }
    }
    for (unsigned r = 0; r < regs; r++) wrd(c, d + r, R[r]);
    qc_commit(c, qc);
    return ARM_OK;
}

/* ======================================== one register, modified imm == */

static arm_status_t neon_1reg_imm(arm_cpu_t *c, uint32_t pc, uint32_t insn) {
    const unsigned cmode = (insn >> 8) & 15u, d = VD;
    const bool op = BIT(5), q = BIT(6);
    const uint64_t imm8 = (BIT(24) << 7) | (((insn >> 16) & 7u) << 4) | (insn & 15u);
    uint64_t imm64;
    bool testimm8 = true;

    if (q && (d & 1u)) return undef(pc, insn, "Q register operand with an odd D number is UNDEFINED");
    switch (cmode >> 1) {                           /* AdvSIMDExpandImm */
    case 0: testimm8 = false; imm64 = imm8 * 0x0000000100000001ull; break;
    case 1: imm64 = (imm8 << 8)  * 0x0000000100000001ull; break;
    case 2: imm64 = (imm8 << 16) * 0x0000000100000001ull; break;
    case 3: imm64 = (imm8 << 24) * 0x0000000100000001ull; break;
    case 4: testimm8 = false; imm64 = imm8 * 0x0001000100010001ull; break;
    case 5: imm64 = (imm8 << 8) * 0x0001000100010001ull; break;
    case 6:
        imm64 = ((cmode & 1u) ? ((imm8 << 16) | 0xffffu) : ((imm8 << 8) | 0xffu))
                * 0x0000000100000001ull;
        break;
    default:
        testimm8 = false;
        if (!(cmode & 1u) && !op) {
            imm64 = imm8 * 0x0101010101010101ull;
        } else if (!(cmode & 1u)) {
            imm64 = 0;
            for (unsigned i = 0; i < 8u; i++)
                if ((imm8 >> i) & 1u) imm64 |= 0xffull << (8u * i);
        } else if (!op) {
            const uint64_t b6 = (imm8 >> 6) & 1u;
            const uint64_t f = ((imm8 >> 7) << 31) | ((b6 ^ 1u) << 30)
                             | ((b6 ? 0x1fu : 0u) << 25) | ((imm8 & 0x3fu) << 19);
            imm64 = f * 0x0000000100000001ull;
        } else {
            return undef(pc, insn, "UNDEFINED Advanced SIMD modified immediate (op=1, cmode=1111)");
        }
        break;
    }
    if (testimm8 && imm8 == 0u)
        return undef(pc, insn, "a zero modified immediate here is UNPREDICTABLE");

    const bool orr_bic = (cmode & 1u) && cmode < 12u;
    for (unsigned r = 0; r < (q ? 2u : 1u); r++) {
        uint64_t v = rdd(c, d + r);
        if (orr_bic)                v = op ? (v & ~imm64) : (v | imm64);  /* VBIC/VORR */
        else if (!op || cmode == 14u) v = imm64;                          /* VMOV */
        else                        v = ~imm64;                           /* VMVN */
        wrd(c, d + r, v);
    }
    return ARM_OK;
}

/* ================================== two registers and a shift amount == */

static arm_status_t neon_2reg_shift(arm_cpu_t *c, uint32_t pc, uint32_t insn) {
    const unsigned op = (insn >> 8) & 15u, imm6 = (insn >> 16) & 63u;
    const bool L = BIT(7), q = BIT(6), u = BIT(24);
    const unsigned d = VD, m = VM;
    unsigned esize;
    bool qc = false;

    if (L) esize = 64u;
    else if (imm6 & 32u) esize = 32u;
    else if (imm6 & 16u) esize = 16u;
    else esize = 8u;
    /* Right shifts count down from 2*esize (L=0) or 64 (L=1); left shifts
     * count up from esize (L=0) or 0 (L=1). */
    const unsigned rshift = (L ? 64u : 2u * esize) - imm6;
    const unsigned lshift = L ? imm6 : imm6 - esize;

    if (op <= 7u) {                               /* whole-width, D or Q */
        const unsigned regs = q ? 2u : 1u;
        if (q && ((d | m) & 1u)) return undef(pc, insn, "Q register operand with an odd D number is UNDEFINED");
        if (op == 4u && !u) return undef(pc, insn, "UNDEFINED Advanced SIMD shift (op 0100, U=0)");
        if (op == 6u && !u) return undef(pc, insn, "UNDEFINED Advanced SIMD shift (VQSHLU with U=0)");
        uint64_t M[2], D[2], R[2];
        for (unsigned r = 0; r < regs; r++) { M[r] = rdd(c, m + r); D[r] = rdd(c, d + r); }
        const unsigned elems = 64u / esize;
        const uint64_t mask = emask(esize);
        for (unsigned r = 0; r < regs; r++) {
            R[r] = 0;
            for (unsigned e = 0; e < elems; e++) {
                const uint64_t x = el(M[r], e, esize), acc = el(D[r], e, esize);
                uint64_t res;
                switch (op) {
                case 0: res = shift_el(x, -(int)rshift, esize, u, false, false, &qc); break;
                case 1: res = acc + shift_el(x, -(int)rshift, esize, u, false, false, &qc); break;
                case 2: res = shift_el(x, -(int)rshift, esize, u, true, false, &qc); break;
                case 3: res = acc + shift_el(x, -(int)rshift, esize, u, true, false, &qc); break;
                case 4: {                                    /* VSRI */
                    const uint64_t keep = rshift >= esize ? 0 : (mask >> rshift);
                    res = (acc & ~keep) | (shift_el(x, -(int)rshift, esize, true, false, false, &qc) & keep);
                    break;
                }
                case 5:
                    if (!u) res = shift_el(x, (int)lshift, esize, true, false, false, &qc);
                    else {                                   /* VSLI */
                        const uint64_t ins = (mask << lshift) & mask;
                        res = (acc & ~ins) | ((x << lshift) & ins);
                    }
                    break;
                case 6: {                                    /* VQSHLU */
                    const int64_t sv = sx(x, esize);
                    if (sv < 0) { qc = true; res = 0; }
                    else res = shift_el(x, (int)lshift, esize, true, false, true, &qc);
                    break;
                }
                default: res = shift_el(x, (int)lshift, esize, u, false, true, &qc); break;
                }
                R[r] = setel(R[r], e, esize, res & mask);
            }
        }
        for (unsigned r = 0; r < regs; r++) wrd(c, d + r, R[r]);
        qc_commit(c, qc);
        return ARM_OK;
    }

    if (L) return undef(pc, insn, "UNDEFINED Advanced SIMD shift with L=1");

    if (op == 8u || op == 9u) {                   /* narrowing, Qm -> Dd */
        const bool round = q;                     /* bit 6 selects rounding here */
        const unsigned w = 2u * esize, elems = 64u / esize;
        if (m & 1u) return undef(pc, insn, "narrowing shift from an odd Q register is UNDEFINED");
        const uint64_t M0 = rdd(c, m), M1 = rdd(c, m + 1u);
        uint64_t R = 0;
        for (unsigned e = 0; e < elems; e++) {
            const unsigned per = 64u / w;
            const uint64_t src = el(e < per ? M0 : M1, e % per, w);
            /* Which form: op 8 U=0 VSHRN; op 8 U=1 VQSHRUN (signed in,
             * unsigned out); op 9 VQSHRN, signed or unsigned per U. */
            const bool src_uns = op == 9u && u;
            const uint64_t sh = shift_el(src, -(int)rshift, w, src_uns, round, false, &qc);
            uint64_t res;
            if (op == 8u && !u) res = sh & emask(esize);
            else if (op == 8u)  res = sat_u(sx(sh, w), esize, &qc);
            else if (u)         res = sat_uu(sh, esize, &qc);
            else                res = sat_s(sx(sh, w), esize, &qc);
            R = setel(R, e, esize, res);
        }
        wrd(c, d, R);
        qc_commit(c, qc);
        return ARM_OK;
    }

    if (op == 10u) {                              /* VSHLL / VMOVL, Dm -> Qd */
        if (q) return undef(pc, insn, "UNDEFINED Advanced SIMD shift (op 1010, B=1)");
        if (d & 1u) return undef(pc, insn, "lengthening shift into an odd Q register is UNDEFINED");
        const uint64_t M0 = rdd(c, m);
        const unsigned w = 2u * esize, elems = 64u / esize, per = 64u / w;
        uint64_t R[2] = { 0, 0 };
        for (unsigned e = 0; e < elems; e++) {
            const uint64_t v = (uint64_t)ext(el(M0, e, esize), esize, u) << lshift;
            R[e / per] = setel(R[e / per], e % per, w, v & emask(w));
        }
        wrd(c, d, R[0]); wrd(c, d + 1u, R[1]);
        return ARM_OK;
    }

    if (op >= 14u) {                              /* VCVT fixed <-> float */
        const bool to_fixed = (op & 1u) != 0u;
        const unsigned regs = q ? 2u : 1u, frac = 64u - imm6;
        uint64_t M[2], R[2];
        uint32_t exc = 0;
        if (!(imm6 & 32u)) return undef(pc, insn, "VCVT fixed-point with imm6 < 32 is UNDEFINED");
        if (q && ((d | m) & 1u)) return undef(pc, insn, "Q register operand with an odd D number is UNDEFINED");
        for (unsigned r = 0; r < regs; r++) M[r] = rdd(c, m + r);
        for (unsigned r = 0; r < regs; r++) {
            R[r] = 0;
            for (unsigned e = 0; e < 2u; e++) {
                const uint32_t x = (uint32_t)el(M[r], e, 32);
                R[r] = setel(R[r], e, 32, to_fixed ? f2fixed(x, frac, u, &exc)
                                                   : fixed2f(x, frac, u, &exc));
            }
        }
        if (fp_ambiguous(exc)) return undef(pc, insn, VFP_FZ_AMBIGUOUS_WHY);
        for (unsigned r = 0; r < regs; r++) wrd(c, d + r, R[r]);
        fp_commit(c, exc);
        return ARM_OK;
    }
    return undef(pc, insn, "UNDEFINED Advanced SIMD two-register shift encoding");
}

/* ======================================= three registers, different == */

/* The long-integer result of one widening multiply, add or difference. */
static arm_status_t neon_3diff(arm_cpu_t *c, uint32_t pc, uint32_t insn) {
    const unsigned op = (insn >> 8) & 15u, size = (insn >> 20) & 3u;
    const bool u = BIT(24);
    const unsigned d = VD, n = VN, m = VM;
    const unsigned esize = 8u << size, w = 2u * esize, elems = 64u / esize;
    const unsigned per = 64u / w;                 /* wide elements per D */
    bool qc = false;

    if (op == 4u || op == 6u) {                   /* VADDHN/VSUBHN (+R): Qn,Qm -> Dd */
        if ((n | m) & 1u) return undef(pc, insn, "narrowing operation from an odd Q register is UNDEFINED");
        const uint64_t Nw[2] = { rdd(c, n), rdd(c, n + 1u) };
        const uint64_t Mw[2] = { rdd(c, m), rdd(c, m + 1u) };
        uint64_t R = 0;
        for (unsigned e = 0; e < elems; e++) {
            const uint64_t x = el(Nw[e / per], e % per, w), y = el(Mw[e / per], e % per, w);
            uint64_t s = (op == 4u ? x + y : x - y) & emask(w);
            if (u) s = (s + (1ull << (esize - 1u))) & emask(w);
            R = setel(R, e, esize, s >> esize);
        }
        wrd(c, d, R);
        return ARM_OK;
    }

    if (d & 1u) return undef(pc, insn, "long operation into an odd Q register is UNDEFINED");
    const bool wide = op <= 3u && (op & 1u);      /* VADDW/VSUBW: Qn source */
    if (wide && (n & 1u)) return undef(pc, insn, "wide operation from an odd Q register is UNDEFINED");
    if ((op == 9u || op == 11u || op == 13u) && (u || size == 0u))
        return undef(pc, insn, "UNDEFINED saturating doubling long multiply encoding");
    if (op == 14u && (u || size != 0u))
        return undef(pc, insn, "VMULL.P takes 8-bit elements and U=0");
    if (op == 15u) return undef(pc, insn, "UNDEFINED Advanced SIMD three-register encoding");

    const uint64_t N0 = rdd(c, n), N1 = wide ? rdd(c, n + 1u) : 0, M0 = rdd(c, m);
    const uint64_t Dw[2] = { rdd(c, d), rdd(c, d + 1u) };
    uint64_t R[2] = { 0, 0 };
    for (unsigned e = 0; e < elems; e++) {
        const int64_t y = ext(el(M0, e, esize), esize, u);
        const int64_t x = wide ? ext(el(e < per ? N0 : N1, e % per, w), w, u)
                               : ext(el(N0, e, esize), esize, u);
        const uint64_t acc = el(Dw[e / per], e % per, w);
        uint64_t res;
        switch (op) {
        case 0: case 1: res = (uint64_t)x + (uint64_t)y; break;
        case 2: case 3: res = (uint64_t)x - (uint64_t)y; break;
        case 5:  res = acc + (uint64_t)(x > y ? x - y : y - x); break;
        case 7:  res = (uint64_t)(x > y ? x - y : y - x); break;
        case 8:  res = acc + (uint64_t)x * (uint64_t)y; break;
        case 10: res = acc - (uint64_t)x * (uint64_t)y; break;
        case 12: res = (uint64_t)x * (uint64_t)y; break;
        case 14: res = pmul8((uint64_t)x, (uint64_t)y); break;
        default: {                                /* VQDMLAL/VQDMLSL/VQDMULL */
            /* 2*x*y saturated to w bits, then (for the accumulating forms)
             * a saturating add or subtract. Only min*min saturates the
             * product. */
            const int64_t p = x * y;
            uint64_t prod;
            if (w == 64u) {
                if (p == ((int64_t)1 << 62)) { qc = true; prod = 0x7fffffffffffffffull; }
                else prod = (uint64_t)p << 1;
            } else {
                prod = sat_s(p * 2, w, &qc);
            }
            if (op == 13u) { res = prod; break; }
            if (w == 64u) {
                res = op == 9u ? qadd64(acc, prod, false, &qc) : qsub64(acc, prod, false, &qc);
            } else {
                const int64_t a = sx(acc, w), pv = sx(prod, w);
                res = sat_s(op == 9u ? a + pv : a - pv, w, &qc);
            }
            break;
        }
        }
        R[e / per] = setel(R[e / per], e % per, w, res & emask(w));
    }
    wrd(c, d, R[0]); wrd(c, d + 1u, R[1]);
    qc_commit(c, qc);
    return ARM_OK;
}

/* ===================================== two registers and a scalar ==== */

static arm_status_t neon_2reg_scalar(arm_cpu_t *c, uint32_t pc, uint32_t insn) {
    const unsigned op = (insn >> 8) & 15u, size = (insn >> 20) & 3u;
    const bool bit24 = BIT(24), F = BIT(8);
    const unsigned d = VD, n = VN;
    unsigned dm, index;
    bool qc = false;

    if (size == 0u) return undef(pc, insn, "Advanced SIMD by-scalar with 8-bit elements is UNDEFINED");
    if (size == 1u) { dm = insn & 7u; index = (BIT(5) << 1) | BIT(3); }
    else            { dm = insn & 15u; index = BIT(5); }
    const unsigned esize = 8u << size, elems = 64u / esize;
    const uint64_t scalar = el(rdd(c, dm), index, esize);

    const bool is_long = op == 2u || op == 3u || op == 6u || op == 7u ||
                         op == 10u || op == 11u;
    if (op >= 14u) return undef(pc, insn, "UNDEFINED Advanced SIMD by-scalar encoding");
    if ((op == 3u || op == 7u || op == 11u) && bit24)
        return undef(pc, insn, "UNDEFINED saturating doubling long multiply encoding");

    if (is_long) {                                /* Dn x scalar -> Qd */
        const bool u = bit24;
        const unsigned w = 2u * esize, per = 64u / w;
        if (d & 1u) return undef(pc, insn, "long operation into an odd Q register is UNDEFINED");
        const uint64_t N0 = rdd(c, n);
        const uint64_t Dw[2] = { rdd(c, d), rdd(c, d + 1u) };
        uint64_t R[2] = { 0, 0 };
        for (unsigned e = 0; e < elems; e++) {
            const int64_t x = ext(el(N0, e, esize), esize, u), y = ext(scalar, esize, u);
            const uint64_t acc = el(Dw[e / per], e % per, w);
            uint64_t res;
            if (op == 2u)       res = acc + (uint64_t)x * (uint64_t)y;          /* VMLAL */
            else if (op == 6u)  res = acc - (uint64_t)x * (uint64_t)y;          /* VMLSL */
            else if (op == 10u) res = (uint64_t)x * (uint64_t)y;                /* VMULL */
            else {                                     /* VQDMLAL/VQDMLSL/VQDMULL */
                const int64_t p = x * y;
                uint64_t prod;
                if (w == 64u) {
                    if (p == ((int64_t)1 << 62)) { qc = true; prod = 0x7fffffffffffffffull; }
                    else prod = (uint64_t)p << 1;
                } else prod = sat_s(p * 2, w, &qc);
                if (op == 11u) res = prod;
                else if (w == 64u)
                    res = op == 3u ? qadd64(acc, prod, false, &qc) : qsub64(acc, prod, false, &qc);
                else {
                    const int64_t a = sx(acc, w), pv = sx(prod, w);
                    res = sat_s(op == 3u ? a + pv : a - pv, w, &qc);
                }
            }
            R[e / per] = setel(R[e / per], e % per, w, res & emask(w));
        }
        wrd(c, d, R[0]); wrd(c, d + 1u, R[1]);
        qc_commit(c, qc);
        return ARM_OK;
    }

    /* VMLA, VMLS, VMUL, VQDMULH, VQRDMULH by scalar: bit 24 is Q. */
    const bool q = bit24;
    const unsigned regs = q ? 2u : 1u;
    const bool fp = (op <= 9u) && F;
    if (q && ((d | n) & 1u)) return undef(pc, insn, "Q register operand with an odd D number is UNDEFINED");
    if (fp && size != 2u) return undef(pc, insn, "Advanced SIMD float by-scalar needs 32-bit elements");
    uint64_t N[2], D[2], R[2];
    uint32_t exc = 0;
    for (unsigned r = 0; r < regs; r++) { N[r] = rdd(c, n + r); D[r] = rdd(c, d + r); }
    for (unsigned r = 0; r < regs; r++) {
        R[r] = 0;
        for (unsigned e = 0; e < elems; e++) {
            const uint64_t x = el(N[r], e, esize), acc = el(D[r], e, esize);
            uint64_t res;
            if (fp) {
                uint32_t p = vfp_std_f32(VFP_OP_MUL, (uint32_t)x, (uint32_t)scalar, &exc);
                if (op >= 8u) res = p;                                   /* VMUL */
                else {
                    if (op == 5u) p ^= 0x80000000u;                      /* VMLS */
                    res = vfp_std_f32(VFP_OP_ADD, (uint32_t)acc, p, &exc);
                }
            } else if (op == 0u) res = int_op(I_MLA, x, scalar, acc, esize, false, &qc);
            else if (op == 4u)   res = int_op(I_MLS, x, scalar, acc, esize, false, &qc);
            else if (op == 8u)   res = int_op(I_MUL, x, scalar, acc, esize, false, &qc);
            else if (op == 12u)  res = int_op(I_QDMULH, x, scalar, acc, esize, false, &qc);
            else                 res = int_op(I_QRDMULH, x, scalar, acc, esize, false, &qc);
            R[r] = setel(R[r], e, esize, res);
        }
    }
    if (fp_ambiguous(exc)) return undef(pc, insn, VFP_FZ_AMBIGUOUS_WHY);
    for (unsigned r = 0; r < regs; r++) wrd(c, d + r, R[r]);
    fp_commit(c, exc);
    qc_commit(c, qc);
    return ARM_OK;
}

/* ===================================== two registers, miscellaneous == */

static unsigned clz_n(uint64_t x, unsigned esize) {
    unsigned n = 0;
    for (int i = (int)esize - 1; i >= 0 && !((x >> i) & 1u); i--) n++;
    return n;
}

/* The 128- or 256-bit concatenation Dd..(:)Dm.. as an array of elements,
 * for the permutations (VZIP, VUZP, VTRN), which the architecture defines
 * on whole operands rather than per D register. */
static arm_status_t neon_permute(arm_cpu_t *c, uint32_t pc, uint32_t insn,
                                 unsigned which, unsigned size) {
    const bool q = BIT(6);
    const unsigned d = VD, m = VM, regs = q ? 2u : 1u;
    const unsigned esize = 8u << size, per = 64u / esize, count = per * regs;
    uint64_t Dv[2], Mv[2], ed[16] = {0}, em[16] = {0}, rd[16], rm[16];

    if (d == m) return undef(pc, insn, "VZIP/VUZP/VTRN with one register as both operands is UNPREDICTABLE");
    for (unsigned r = 0; r < regs; r++) { Dv[r] = rdd(c, d + r); Mv[r] = rdd(c, m + r); }
    for (unsigned i = 0; i < count; i++) {
        ed[i] = el(Dv[i / per], i % per, esize);
        em[i] = el(Mv[i / per], i % per, esize);
    }
    for (unsigned i = 0; i < count; i++) {
        if (which == 1u) {                        /* VTRN */
            if (i & 1u) { rd[i] = em[i - 1u]; rm[i] = em[i]; }
            else        { rd[i] = ed[i];      rm[i] = ed[i + 1u]; }
        } else if (which == 2u) {                 /* VUZP: Dd = evens, Dm = odds */
            const unsigned j = 2u * i;
            rd[i] = j < count ? ed[j] : em[j - count];
            rm[i] = j + 1u < count ? ed[j + 1u] : em[j + 1u - count];
        } else {                                  /* VZIP: interleave */
            const unsigned j = i / 2u, h = count / 2u;
            rd[i] = (i & 1u) ? em[j] : ed[j];
            rm[i] = (i & 1u) ? em[h + j] : ed[h + j];
        }
    }
    for (unsigned r = 0; r < regs; r++) { Dv[r] = 0; Mv[r] = 0; }
    for (unsigned i = 0; i < count; i++) {
        Dv[i / per] = setel(Dv[i / per], i % per, esize, rd[i]);
        Mv[i / per] = setel(Mv[i / per], i % per, esize, rm[i]);
    }
    for (unsigned r = 0; r < regs; r++) { wrd(c, d + r, Dv[r]); wrd(c, m + r, Mv[r]); }
    return ARM_OK;
}

static arm_status_t neon_2reg_misc(arm_cpu_t *c, uint32_t pc, uint32_t insn) {
    const unsigned A = (insn >> 16) & 3u, B = (insn >> 6) & 31u;
    const unsigned size = (insn >> 18) & 3u;
    const bool q = BIT(6);
    const unsigned d = VD, m = VM, regs = q ? 2u : 1u;
    const unsigned esize = 8u << size, elems = 64u / esize;
    const unsigned opB = B >> 1;                  /* B without the Q bit */
    uint64_t M[2], D[2], R[2];
    bool qc = false;
    uint32_t exc = 0;

    if (A == 2u) {
        if (opB <= 3u) {                          /* VSWP, VTRN, VUZP, VZIP */
            if (q && ((d | m) & 1u)) return undef(pc, insn, "Q register operand with an odd D number is UNDEFINED");
            if (opB == 0u) {
                if (size != 0u) return undef(pc, insn, "VSWP with size != 0 is UNDEFINED");
                for (unsigned r = 0; r < regs; r++) {
                    const uint64_t t = rdd(c, d + r);
                    wrd(c, d + r, rdd(c, m + r)); wrd(c, m + r, t);
                }
                return ARM_OK;
            }
            if (size == 3u) return undef(pc, insn, "64-bit elements are UNDEFINED for VTRN/VUZP/VZIP");
            if (opB >= 2u && !q && size == 2u)
                return undef(pc, insn, "VUZP/VZIP of 32-bit elements in D registers is UNDEFINED");
            return neon_permute(c, pc, insn, opB, size);
        }
        if (opB == 4u || opB == 5u) {             /* VMOVN, VQMOVUN, VQMOVN */
            const unsigned w = 2u * esize, per = 64u / w, sub = B & 3u;
            if (size == 3u) return undef(pc, insn, "narrowing from 128-bit elements is UNDEFINED");
            if (m & 1u) return undef(pc, insn, "narrowing from an odd Q register is UNDEFINED");
            const uint64_t M0 = rdd(c, m), M1 = rdd(c, m + 1u);
            uint64_t Rn = 0;
            for (unsigned e = 0; e < elems; e++) {
                const uint64_t src = el(e < per ? M0 : M1, e % per, w);
                uint64_t res;
                if (sub == 0u)      res = src & emask(esize);             /* VMOVN */
                else if (sub == 1u) res = sat_u(sx(src, w), esize, &qc);  /* VQMOVUN */
                else if (sub == 2u) res = sat_s(sx(src, w), esize, &qc);  /* VQMOVN.S */
                else                res = sat_uu(src, esize, &qc);        /* VQMOVN.U */
                Rn = setel(Rn, e, esize, res);
            }
            wrd(c, d, Rn);
            qc_commit(c, qc);
            return ARM_OK;
        }
        if (B == 12u) {                           /* VSHLL, shift == esize */
            const unsigned w = 2u * esize, per = 64u / w;
            if (size == 3u) return undef(pc, insn, "VSHLL of 64-bit elements is UNDEFINED");
            if (d & 1u) return undef(pc, insn, "lengthening into an odd Q register is UNDEFINED");
            const uint64_t M0 = rdd(c, m);
            uint64_t Rw[2] = { 0, 0 };
            for (unsigned e = 0; e < elems; e++)
                Rw[e / per] = setel(Rw[e / per], e % per, w, el(M0, e, esize) << esize);
            wrd(c, d, Rw[0]); wrd(c, d + 1u, Rw[1]);
            return ARM_OK;
        }
        if ((B & 0x1bu) == 0x18u)
            return undef(pc, insn, "VCVT between half and single precision: the Cortex-A8 has no half precision");
        return undef(pc, insn, "UNDEFINED Advanced SIMD two-register miscellaneous encoding");
    }

    if (q && ((d | m) & 1u)) return undef(pc, insn, "Q register operand with an odd D number is UNDEFINED");
    for (unsigned r = 0; r < regs; r++) { M[r] = rdd(c, m + r); D[r] = rdd(c, d + r); }

    if (A == 0u) {
        const unsigned op = opB;                  /* bits 10:7 */
        if (op <= 2u) {                           /* VREV64, VREV32, VREV16 */
            const unsigned groupbits = 64u >> op; /* 64, 32, 16 */
            if (op + size >= 3u) return undef(pc, insn, "VREV with elements not smaller than the group is UNDEFINED");
            const unsigned per_group = groupbits / esize;
            for (unsigned r = 0; r < regs; r++) {
                R[r] = 0;
                for (unsigned e = 0; e < elems; e++) {
                    const unsigned g = e / per_group, i = e % per_group;
                    const unsigned src = g * per_group + (per_group - 1u - i);
                    R[r] = setel(R[r], e, esize, el(M[r], src, esize));
                }
            }
        } else if (op == 4u || op == 5u || op == 12u || op == 13u) {  /* VPADDL/VPADAL */
            const bool uns = BIT(7), acc = op >= 12u;
            const unsigned w = 2u * esize;
            if (size == 3u) return undef(pc, insn, "pairwise long add of 64-bit elements is UNDEFINED");
            for (unsigned r = 0; r < regs; r++) {
                R[r] = 0;
                for (unsigned e = 0; e < elems / 2u; e++) {
                    uint64_t s = (uint64_t)ext(el(M[r], 2u * e, esize), esize, uns)
                               + (uint64_t)ext(el(M[r], 2u * e + 1u, esize), esize, uns);
                    if (acc) s += el(D[r], e, w);
                    R[r] = setel(R[r], e, w, s & emask(w));
                }
            }
        } else if (op == 8u || op == 9u) {        /* VCLS, VCLZ */
            if (size == 3u) return undef(pc, insn, "VCLS/VCLZ of 64-bit elements is UNDEFINED");
            for (unsigned r = 0; r < regs; r++) {
                R[r] = 0;
                for (unsigned e = 0; e < elems; e++) {
                    const uint64_t x = el(M[r], e, esize);
                    uint64_t res;
                    if (op == 9u) res = clz_n(x, esize);
                    else {
                        const uint64_t top = (x >> (esize - 1u)) & 1u;
                        const uint64_t y = top ? (~x & emask(esize)) : x;
                        res = clz_n(y, esize) - 1u;
                    }
                    R[r] = setel(R[r], e, esize, res);
                }
            }
        } else if (op == 10u || op == 11u) {      /* VCNT, VMVN */
            if (size != 0u) return undef(pc, insn, "VCNT/VMVN with size != 0 is UNDEFINED");
            for (unsigned r = 0; r < regs; r++) {
                if (op == 11u) { R[r] = ~M[r]; continue; }
                R[r] = 0;
                for (unsigned e = 0; e < 8u; e++) {
                    uint64_t x = el(M[r], e, 8), n = 0;
                    while (x) { n += x & 1u; x >>= 1; }
                    R[r] = setel(R[r], e, 8, n);
                }
            }
        } else if (op == 14u || op == 15u) {      /* VQABS, VQNEG */
            if (size == 3u) return undef(pc, insn, "VQABS/VQNEG of 64-bit elements is UNDEFINED");
            for (unsigned r = 0; r < regs; r++) {
                R[r] = 0;
                for (unsigned e = 0; e < elems; e++) {
                    const int64_t x = sx(el(M[r], e, esize), esize);
                    const int64_t v = op == 14u ? (x < 0 ? -x : x) : -x;
                    R[r] = setel(R[r], e, esize, sat_s(v, esize, &qc));
                }
            }
        } else {
            return undef(pc, insn, "UNDEFINED Advanced SIMD two-register miscellaneous encoding");
        }
        for (unsigned r = 0; r < regs; r++) wrd(c, d + r, R[r]);
        qc_commit(c, qc);
        return ARM_OK;
    }

    if (A == 1u) {
        const bool F = BIT(10);
        const unsigned op = (insn >> 7) & 7u;
        if (op == 5u) return undef(pc, insn, "UNDEFINED Advanced SIMD two-register miscellaneous encoding");
        if (F && size != 2u) return undef(pc, insn, "Advanced SIMD float needs 32-bit elements");
        if (!F && size == 3u) return undef(pc, insn, "64-bit elements are UNDEFINED for this operation");
        for (unsigned r = 0; r < regs; r++) {
            R[r] = 0;
            for (unsigned e = 0; e < elems; e++) {
                const uint64_t x = el(M[r], e, esize);
                uint64_t res;
                if (F) {
                    const uint32_t f = (uint32_t)x;
                    bool t;
                    switch (op) {
                    case 0: t = fcmp(CMP_GT, f, 0u, &exc); break;          /* VCGT #0 */
                    case 1: t = fcmp(CMP_GE, f, 0u, &exc); break;          /* VCGE #0 */
                    case 2: t = fcmp(CMP_EQ, f, 0u, &exc); break;          /* VCEQ #0 */
                    case 3: t = fcmp(CMP_GE, 0u, f, &exc); break;          /* VCLE #0 */
                    case 4: t = fcmp(CMP_GT, 0u, f, &exc); break;          /* VCLT #0 */
                    default: t = false; break;
                    }
                    if (op == 6u)      res = f & 0x7fffffffu;              /* VABS */
                    else if (op == 7u) res = f ^ 0x80000000u;              /* VNEG */
                    else               res = t ? 0xffffffffu : 0u;
                } else {
                    const int64_t v = sx(x, esize);
                    switch (op) {
                    case 0: res = v > 0 ? emask(esize) : 0; break;
                    case 1: res = v >= 0 ? emask(esize) : 0; break;
                    case 2: res = v == 0 ? emask(esize) : 0; break;
                    case 3: res = v <= 0 ? emask(esize) : 0; break;
                    case 4: res = v < 0 ? emask(esize) : 0; break;
                    case 6: res = (uint64_t)(v < 0 ? -v : v) & emask(esize); break;
                    default: res = (uint64_t)-v & emask(esize); break;
                    }
                }
                R[r] = setel(R[r], e, esize, res);
            }
        }
        if (fp_ambiguous(exc)) return undef(pc, insn, VFP_FZ_AMBIGUOUS_WHY);
        for (unsigned r = 0; r < regs; r++) wrd(c, d + r, R[r]);
        fp_commit(c, exc);
        return ARM_OK;
    }

    /* A == 3: VRECPE, VRSQRTE, VCVT float <-> integer; 32-bit only. */
    if (size != 2u) return undef(pc, insn, "VRECPE/VRSQRTE/VCVT need 32-bit elements");
    if (B & 0x10u) {                  /* B = 10xxx estimates, 11xxx VCVT */
        for (unsigned r = 0; r < regs; r++) {
            R[r] = 0;
            for (unsigned e = 0; e < 2u; e++) {
                const uint32_t x = (uint32_t)el(M[r], e, 32);
                uint32_t res;
                if ((B & 0x18u) == 0x18u) {       /* VCVT, op = bits 8:7 */
                    const unsigned cop = (insn >> 7) & 3u;
                    if (cop & 2u) res = f2fixed(x, 0u, cop & 1u, &exc);
                    else          res = fixed2f(x, 0u, cop & 1u, &exc);
                } else {
                    const bool F = BIT(8), rsqrt = BIT(7);
                    if (F) res = rsqrt ? frsqrte(x, &exc) : frecpe(x, &exc);
                    else   res = rsqrt ? ursqrte(x) : urecpe(x);
                }
                R[r] = setel(R[r], e, 32, res);
            }
        }
        if (fp_ambiguous(exc)) return undef(pc, insn, VFP_FZ_AMBIGUOUS_WHY);
        for (unsigned r = 0; r < regs; r++) wrd(c, d + r, R[r]);
        fp_commit(c, exc);
        return ARM_OK;
    }
    return undef(pc, insn, "UNDEFINED Advanced SIMD two-register miscellaneous encoding");
}

/* ================================================ VEXT, VTBL, VDUP ===== */

static arm_status_t neon_vext(arm_cpu_t *c, uint32_t pc, uint32_t insn) {
    const bool q = BIT(6);
    const unsigned d = VD, n = VN, m = VM, imm4 = (insn >> 8) & 15u;
    const unsigned regs = q ? 2u : 1u;
    uint8_t bytes[32];
    if (!q && (imm4 & 8u)) return undef(pc, insn, "VEXT of D registers with imm4 > 7 is UNDEFINED");
    if (q && ((d | n | m) & 1u)) return undef(pc, insn, "Q register operand with an odd D number is UNDEFINED");
    for (unsigned r = 0; r < regs; r++) {
        const uint64_t lo = rdd(c, n + r), hi = rdd(c, m + r);
        for (unsigned i = 0; i < 8u; i++) {
            bytes[8u * r + i] = (uint8_t)(lo >> (8u * i));
            bytes[8u * (regs + r) + i] = (uint8_t)(hi >> (8u * i));
        }
    }
    for (unsigned r = 0; r < regs; r++) {
        uint64_t v = 0;
        for (unsigned i = 0; i < 8u; i++)
            v |= (uint64_t)bytes[imm4 + 8u * r + i] << (8u * i);
        wrd(c, d + r, v);
    }
    return ARM_OK;
}

static arm_status_t neon_vtbl(arm_cpu_t *c, uint32_t pc, uint32_t insn) {
    const unsigned d = VD, n = VN, m = VM, len = ((insn >> 8) & 3u) + 1u;
    const bool tbx = BIT(6);
    uint8_t table[32];
    if (n + len > 32u) return undef(pc, insn, "VTBL/VTBX table past d31 is UNPREDICTABLE");
    for (unsigned r = 0; r < len; r++) {
        const uint64_t v = rdd(c, n + r);
        for (unsigned i = 0; i < 8u; i++) table[8u * r + i] = (uint8_t)(v >> (8u * i));
    }
    const uint64_t idx = rdd(c, m);
    uint64_t res = rdd(c, d);
    for (unsigned i = 0; i < 8u; i++) {
        const unsigned j = (unsigned)(idx >> (8u * i)) & 0xffu;
        if (j < 8u * len) res = setel(res, i, 8, table[j]);
        else if (!tbx)    res = setel(res, i, 8, 0);
    }
    wrd(c, d, res);
    return ARM_OK;
}

static arm_status_t neon_vdup_scalar(arm_cpu_t *c, uint32_t pc, uint32_t insn) {
    const unsigned imm4 = (insn >> 16) & 15u, d = VD, m = VM;
    const bool q = BIT(6);
    unsigned esize, index;
    if (imm4 & 1u)      { esize = 8u;  index = imm4 >> 1; }
    else if (imm4 & 2u) { esize = 16u; index = imm4 >> 2; }
    else if (imm4 & 4u) { esize = 32u; index = imm4 >> 3; }
    else return undef(pc, insn, "VDUP (scalar) with imm4 = x000 is UNDEFINED");
    if (q && (d & 1u)) return undef(pc, insn, "Q register operand with an odd D number is UNDEFINED");
    const uint64_t x = el(rdd(c, m), index, esize);
    uint64_t v = 0;
    for (unsigned e = 0; e < 64u / esize; e++) v = setel(v, e, esize, x);
    wrd(c, d, v);
    if (q) wrd(c, d + 1u, v);
    return ARM_OK;
}

/* ============================================= element load and store == */

/*
 * ARM ARM A7.7: 1111 0100 A D L 0 Rn Vd B size/index_align Rm.
 *
 * Every form reduces to a list of (register, lane) slots visited in memory
 * order, `ebytes` apart from Rn: VLD1/VST1 fill one register after another,
 * VLD2-4/VST2-4 interleave the registers of a structure element by element,
 * the single-lane forms touch one lane of each register, and the all-lanes
 * forms read one structure and replicate it. So one transfer loop serves
 * all of them, and the decode below is only the architecture's tables.
 *
 * Alignment is checked before anything moves: an explicit alignment
 * qualifier that the address does not meet is an alignment fault whatever
 * SCTLR.A says, and with SCTLR.A set each element must be naturally aligned.
 * A load commits nothing, registers or writeback, unless every access
 * succeeded; a store that faults part way has written what it wrote, which
 * is what an abort part way through a store leaves on hardware too.
 *
 * Rn == PC, and a register list that runs past d31, are UNPREDICTABLE and
 * refused.
 */
#define LDST_MAX 32u

static arm_status_t neon_ldst(arm_cpu_t *c, uint32_t pc, uint32_t insn,
                              const vfp_bus_t *bus) {
    const bool multiple = !BIT(23), load = BIT(21);
    const unsigned rn = (insn >> 16) & 15u, rm = insn & 15u, d = VD;
    const unsigned B = (insn >> 8) & 15u;
    uint8_t sreg[LDST_MAX], slane[LDST_MAX];
    unsigned count = 0, ebytes, alignment = 1u, transfer;
    bool replicate = false;              /* all lanes: slot k fills whole regs */
    unsigned rep_regs = 1u;

    if (!bus || !bus->read_el || !bus->write_el || !bus->align_fault)
        return undef(pc, insn, "Advanced SIMD load/store on a bus without element access");
    if (rn == 15u) return undef(pc, insn, "Advanced SIMD load/store with Rn = PC is UNPREDICTABLE");

    if (multiple) {
        const unsigned size = (insn >> 6) & 3u, align = (insn >> 4) & 3u;
        unsigned regs, n, inc;
        ebytes = 1u << size;
        switch (B) {
        case 7u: case 10u: case 6u: case 2u:                  /* VLD1/VST1 */
            n = 1u;
            regs = B == 7u ? 1u : B == 10u ? 2u : B == 6u ? 3u : 4u;
            inc = 1u;
            if ((regs == 1u || regs == 3u) && (align & 2u))
                return undef(pc, insn, "UNDEFINED VLD1/VST1 alignment");
            if (regs == 2u && align == 3u)
                return undef(pc, insn, "UNDEFINED VLD1/VST1 alignment");
            break;
        case 8u: case 9u: case 3u:                            /* VLD2/VST2 */
            n = 2u;
            regs = B == 3u ? 2u : 1u;
            inc = B == 8u ? 1u : 2u;
            if (size == 3u) return undef(pc, insn, "VLD2/VST2 of 64-bit elements is UNDEFINED");
            if (B != 3u && align == 3u) return undef(pc, insn, "UNDEFINED VLD2/VST2 alignment");
            break;
        case 4u: case 5u:                                     /* VLD3/VST3 */
            n = 3u; regs = 1u; inc = B == 4u ? 1u : 2u;
            if (size == 3u || (align & 2u))
                return undef(pc, insn, "UNDEFINED VLD3/VST3 size or alignment");
            break;
        case 0u: case 1u:                                     /* VLD4/VST4 */
            n = 4u; regs = 1u; inc = B == 0u ? 1u : 2u;
            if (size == 3u) return undef(pc, insn, "VLD4/VST4 of 64-bit elements is UNDEFINED");
            break;
        default:
            return undef(pc, insn, "UNDEFINED Advanced SIMD multiple-structure load/store");
        }
        if (n == 3u) alignment = (align & 1u) ? 8u : 1u;
        else if (align) alignment = 4u << align;
        /* The last register touched: VLD1's list, or the last member of the
         * last structure (for VLD2 with two pairs, d + inc + 1). */
        const unsigned last = n == 1u ? d + regs - 1u : d + (n - 1u) * inc + regs - 1u;
        if (last > 31u) return undef(pc, insn, "register list past d31 is UNPREDICTABLE");
        const unsigned elements = 8u / ebytes;
        if (n == 1u) {
            for (unsigned r = 0; r < regs; r++)
                for (unsigned e = 0; e < elements; e++) {
                    sreg[count] = (uint8_t)(d + r); slane[count++] = (uint8_t)e;
                }
        } else {
            for (unsigned r = 0; r < regs; r++)
                for (unsigned e = 0; e < elements; e++)
                    for (unsigned k = 0; k < n; k++) {
                        sreg[count] = (uint8_t)(d + k * inc + r);
                        slane[count++] = (uint8_t)e;
                    }
        }
        transfer = 8u * regs * n;
    } else {
        const unsigned size = (insn >> 10) & 3u, n = ((insn >> 8) & 3u) + 1u;
        const unsigned ia = (insn >> 4) & 15u;
        unsigned inc = 1u, index = 0;
        if (size == 3u) {                                     /* all lanes */
            const unsigned sz = (insn >> 6) & 3u;
            const bool T = BIT(5), a = BIT(4);
            if (!load) return undef(pc, insn, "UNDEFINED Advanced SIMD store to all lanes");
            ebytes = 1u << sz;
            switch (n) {
            case 1u:
                if (sz == 3u || (sz == 0u && a))
                    return undef(pc, insn, "UNDEFINED VLD1 (all lanes) size or alignment");
                rep_regs = T ? 2u : 1u;
                alignment = a ? ebytes : 1u;
                break;
            case 2u:
                if (sz == 3u) return undef(pc, insn, "UNDEFINED VLD2 (all lanes) size");
                inc = T ? 2u : 1u;
                alignment = a ? 2u * ebytes : 1u;
                break;
            case 3u:
                if (sz == 3u || a) return undef(pc, insn, "UNDEFINED VLD3 (all lanes) size or alignment");
                inc = T ? 2u : 1u;
                break;
            default:
                inc = T ? 2u : 1u;
                if (sz == 3u) {
                    if (!a) return undef(pc, insn, "UNDEFINED VLD4 (all lanes) size");
                    ebytes = 4u; alignment = 16u;
                } else if (a) {
                    alignment = sz == 2u ? 8u : 4u * ebytes;
                }
                break;
            }
            const unsigned last = n == 1u ? d + rep_regs - 1u : d + (n - 1u) * inc;
            if (last > 31u) return undef(pc, insn, "register list past d31 is UNPREDICTABLE");
            replicate = true;
            for (unsigned k = 0; k < n; k++) {
                sreg[count] = (uint8_t)(d + k * inc); slane[count++] = 0;
            }
            transfer = n * ebytes;
        } else {                                              /* one lane */
            ebytes = 1u << size;
            switch (n) {
            case 1u:
                if (size == 0u) { if (ia & 1u) goto bad_lane; index = ia >> 1; }
                else if (size == 1u) {
                    if (ia & 2u) goto bad_lane;
                    index = ia >> 2; alignment = (ia & 1u) ? 2u : 1u;
                } else {
                    if ((ia & 4u) || ((ia & 3u) != 0u && (ia & 3u) != 3u)) goto bad_lane;
                    index = ia >> 3; alignment = (ia & 3u) ? 4u : 1u;
                }
                break;
            case 2u:
                if (size == 0u) { index = ia >> 1; alignment = (ia & 1u) ? 2u : 1u; }
                else if (size == 1u) {
                    index = ia >> 2; inc = (ia & 2u) ? 2u : 1u;
                    alignment = (ia & 1u) ? 4u : 1u;
                } else {
                    if (ia & 2u) goto bad_lane;
                    index = ia >> 3; inc = (ia & 4u) ? 2u : 1u;
                    alignment = (ia & 1u) ? 8u : 1u;
                }
                break;
            case 3u:
                if (size == 0u) { if (ia & 1u) goto bad_lane; index = ia >> 1; }
                else if (size == 1u) {
                    if (ia & 1u) goto bad_lane;
                    index = ia >> 2; inc = (ia & 2u) ? 2u : 1u;
                } else {
                    if (ia & 3u) goto bad_lane;
                    index = ia >> 3; inc = (ia & 4u) ? 2u : 1u;
                }
                break;
            default:
                if (size == 0u) { index = ia >> 1; alignment = (ia & 1u) ? 4u : 1u; }
                else if (size == 1u) {
                    index = ia >> 2; inc = (ia & 2u) ? 2u : 1u;
                    alignment = (ia & 1u) ? 8u : 1u;
                } else {
                    if ((ia & 3u) == 3u) goto bad_lane;
                    index = ia >> 3; inc = (ia & 4u) ? 2u : 1u;
                    alignment = (ia & 3u) ? (4u << (ia & 3u)) : 1u;
                }
                break;
            }
            if (d + (n - 1u) * inc > 31u)
                return undef(pc, insn, "register list past d31 is UNPREDICTABLE");
            for (unsigned k = 0; k < n; k++) {
                sreg[count] = (uint8_t)(d + k * inc); slane[count++] = (uint8_t)index;
            }
            transfer = n * ebytes;
        }
    }

    const uint32_t base = c->r[rn];
    if ((base & (alignment - 1u)) != 0u ||
        ((c->cp15.sctlr & ARM_SCTLR_A) && (base & (ebytes - 1u)) != 0u)) {
        bus->align_fault(c, base, !load);
        return ARM_OK;
    }

    uint64_t val[LDST_MAX];
    uint32_t addr = base;
    for (unsigned i = 0; i < count; i++, addr += ebytes) {
        const unsigned esize = 8u * ebytes;
        if (load) {
            uint64_t v;
            if (ebytes == 8u) {
                const uint32_t lo = bus->read_el(c, addr, 4u);
                if (c->abort_pending) return ARM_OK;
                v = lo | ((uint64_t)bus->read_el(c, addr + 4u, 4u) << 32);
            } else {
                v = bus->read_el(c, addr, ebytes);
            }
            if (c->abort_pending) return ARM_OK;
            val[i] = v;
        } else {
            const uint64_t v = el(rdd(c, sreg[i]), slane[i], esize);
            if (ebytes == 8u) {
                bus->write_el(c, addr, 4u, (uint32_t)v);
                if (c->abort_pending) return ARM_OK;
                bus->write_el(c, addr + 4u, 4u, (uint32_t)(v >> 32));
            } else {
                bus->write_el(c, addr, ebytes, (uint32_t)v);
            }
            if (c->abort_pending) return ARM_OK;
        }
    }
    if (load) {
        const unsigned esize = 8u * ebytes;
        for (unsigned i = 0; i < count; i++) {
            if (replicate) {
                uint64_t v = 0;
                for (unsigned e = 0; e < 64u / esize; e++) v = setel(v, e, esize, val[i]);
                for (unsigned r = 0; r < rep_regs; r++) wrd(c, sreg[i] + r, v);
            } else {
                wrd(c, sreg[i], setel(rdd(c, sreg[i]), slane[i], esize, val[i]));
            }
        }
    }
    if (rm != 15u) c->r[rn] = base + (rm == 13u ? transfer : c->r[rm]);
    return ARM_OK;

bad_lane:
    return undef(pc, insn, "UNDEFINED Advanced SIMD single-lane index/alignment");
}

/* ============================================================ decode ==== */

/* ARM ARM A7.4, "Advanced SIMD data-processing instructions": A is bits
 * 23:19, B bits 11:8, C bits 7:4 and U bit 24. */
static arm_status_t neon_dp(arm_cpu_t *c, uint32_t pc, uint32_t insn) {
    const unsigned A = (insn >> 19) & 31u, B = (insn >> 8) & 15u, C = (insn >> 4) & 15u;
    const bool U = BIT(24);
    if (!(A & 0x10u)) return neon_3same(c, pc, insn);
    if ((C & 9u) == 1u) {
        if ((A & 0x17u) == 0x10u) return neon_1reg_imm(c, pc, insn);
        return neon_2reg_shift(c, pc, insn);
    }
    if ((C & 9u) == 9u) return neon_2reg_shift(c, pc, insn);
    if ((A & 0x16u) != 0x16u) {
        if ((C & 5u) == 0u) return neon_3diff(c, pc, insn);
        return neon_2reg_scalar(c, pc, insn);
    }
    if (!U) return neon_vext(c, pc, insn);
    if (!(B & 8u)) return neon_2reg_misc(c, pc, insn);
    if ((B & 12u) == 8u) return neon_vtbl(c, pc, insn);
    if (B == 12u && !(C & 8u)) return neon_vdup_scalar(c, pc, insn);
    return undef(pc, insn, "UNDEFINED Advanced SIMD data-processing encoding");
}

arm_status_t neon_execute(arm_cpu_t *c, uint32_t pc, uint32_t insn,
                          const vfp_bus_t *bus) {
    vfp_begin();
    /* FPEXC.EN and CPACR gate Advanced SIMD exactly as they gate VFP: a
     * disabled unit is the lazy-enable trap, silent, for the caller. */
    if (!vfp_cpacr_permits(c) || !vfp_enabled(c)) return ARM_UNDEFINED;
    if (!arm_arch_is_v7(c->arch))
        return undef(pc, insn, "Advanced SIMD; the ARM1176 has no NEON");
    if (c->vfp_fpscr & (ARM_FPSCR_LEN | ARM_FPSCR_STRIDE))
        return undef(pc, insn, "Advanced SIMD with FPSCR.Len or Stride nonzero");
    if ((insn & 0xfe000000u) == 0xf2000000u) return neon_dp(c, pc, insn);
    if ((insn & 0xff100000u) == 0xf4000000u) return neon_ldst(c, pc, insn, bus);
    return undef(pc, insn, "not an Advanced SIMD encoding");
}
