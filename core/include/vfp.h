/*
 * S5LBox — the VFP coprocessor and Advanced SIMD, public interface.
 *
 * The ARM1176JZF-S carries a VFP11 unit implementing VFPv2: 32 single-precision
 * registers s0-s31 aliased onto 16 double-precision registers d0-d15. There is
 * no d16-d31 and there is no Advanced SIMD/NEON on that part. The Cortex-A8
 * profile (ARM_ARCH_V7_A8) has VFPv3-D32, d0-d31, and NEON.
 *
 * Everything about WHY this exists, and every floating-point semantic this
 * implementation does and does not model, is documented at the top of
 * core/src/arm/vfp.c (and, for Advanced SIMD, core/src/arm/neon.c). Read
 * those before using anything here.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_VFP_H
#define S5LBOX_VFP_H

#include "arm.h"

/* ---------------------------------------------------------------- FPSCR ---
 * The VFP status and control register. Bit positions are architectural
 * (ARM DDI 0100I, C1.2 / ARM DDI 0301H, 20.1.2).
 */
#define ARM_FPSCR_N      (1u << 31)  /* comparison: less-than or unordered   */
#define ARM_FPSCR_Z      (1u << 30)  /* comparison: equal                    */
#define ARM_FPSCR_C      (1u << 29)  /* comparison: >=, or unordered         */
#define ARM_FPSCR_V      (1u << 28)  /* comparison: unordered                */
#define ARM_FPSCR_NZCV   0xf0000000u
#define ARM_FPSCR_QC     (1u << 27)  /* ARMv7: Advanced SIMD saturation     */
#define ARM_FPSCR_AHP    (1u << 26)  /* ARMv7: alternative half precision   */
#define ARM_FPSCR_DN     (1u << 25)  /* default NaN mode                     */
#define ARM_FPSCR_FZ     (1u << 24)  /* flush-to-zero mode                   */
#define ARM_FPSCR_RMODE  (3u << 22)  /* 00 RN, 01 RP, 10 RM, 11 RZ           */
#define ARM_FPSCR_STRIDE (3u << 20)  /* short-vector stride                  */
#define ARM_FPSCR_LEN    (7u << 16)  /* short-vector length minus one        */
/* Trap enables. Setting one asks the VFP to *bounce* the instruction to
 * support code rather than set the matching cumulative bit. */
#define ARM_FPSCR_IDE    (1u << 15)
#define ARM_FPSCR_IXE    (1u << 12)
#define ARM_FPSCR_UFE    (1u << 11)
#define ARM_FPSCR_OFE    (1u << 10)
#define ARM_FPSCR_DZE    (1u <<  9)
#define ARM_FPSCR_IOE    (1u <<  8)
#define ARM_FPSCR_ENABLES (ARM_FPSCR_IDE | ARM_FPSCR_IXE | ARM_FPSCR_UFE | \
                           ARM_FPSCR_OFE | ARM_FPSCR_DZE | ARM_FPSCR_IOE)
/* Cumulative (sticky) exception flags. Set by hardware, cleared by software. */
#define ARM_FPSCR_IDC    (1u <<  7)  /* input denormal                       */
#define ARM_FPSCR_IXC    (1u <<  4)  /* inexact                              */
#define ARM_FPSCR_UFC    (1u <<  3)  /* underflow                            */
#define ARM_FPSCR_OFC    (1u <<  2)  /* overflow                             */
#define ARM_FPSCR_DZC    (1u <<  1)  /* division by zero                     */
#define ARM_FPSCR_IOC    (1u <<  0)  /* invalid operation                    */

/*
 * The bits VFPv2 defines. Everything else is reserved and reads as zero on the
 * ARM1176, so writes to those bits are dropped rather than stored — a guest
 * that writes 0xffffffff and reads it back must see hardware's answer.
 */
#define ARM_FPSCR_WMASK  0xf3f79f9fu
/*
 * The Cortex-A8's FPSCR, as Unicorn 2.1.4's Cortex-A8 model reads it back
 * after "VMSR FPSCR, 0xffffffff": QC and AHP join, the trap enables are gone
 * (MVFR0 advertises no exception trapping, so they read as zero and no mode
 * check here can ever see one set) and bits 6:5 and 19 stay reserved. AHP is
 * stored though the A8 has no half-precision conversions to consult it.
 */
#define ARM_FPSCR_WMASK_V7 0xfff7009fu

static inline uint32_t vfp_fpscr_wmask(const arm_cpu_t *c) {
    return arm_arch_is_v7(c->arch) ? ARM_FPSCR_WMASK_V7 : ARM_FPSCR_WMASK;
}

/* ------------------------------------------------------- register file --- */
/*
 * s0-s31 as raw bit patterns, in register-number order. dN occupies the pair
 * (s[2N], s[2N+1]) with s[2N] holding the LOW-order word — the little-endian
 * layout the ARM1176 uses, and the reason a VSTM of {d0} and a VSTM of
 * {s0,s1} write the same four words in the same order.
 */
static inline uint32_t vfp_get_s(const arm_cpu_t *c, unsigned n) {
    return c->vfp_s[n & 31u];
}
static inline void vfp_set_s(arm_cpu_t *c, unsigned n, uint32_t v) {
    c->vfp_s[n & 31u] = v;
}
/* d0-d31. d16-d31 exist only on ARMv7; the decoders refuse them on the
 * ARM1176 before a register number ever reaches here. */
static inline uint64_t vfp_get_d(const arm_cpu_t *c, unsigned n) {
    n = (n & 31u) * 2u;
    return (uint64_t)c->vfp_s[n] | ((uint64_t)c->vfp_s[n + 1u] << 32);
}
static inline void vfp_set_d(arm_cpu_t *c, unsigned n, uint64_t v) {
    n = (n & 31u) * 2u;
    c->vfp_s[n]      = (uint32_t)v;
    c->vfp_s[n + 1u] = (uint32_t)(v >> 32);
}

/* ------------------------------------------------------------ the unit --- */

/*
 * Data-side memory, supplied by the interpreter. These are the interpreter's
 * own translating accessors: they walk the MMU with the current privilege and
 * latch a fault on the CPU (cpu->abort_pending), which arm_step turns into a
 * data abort once the instruction finishes. VFP load/store must go through
 * them and must check abort_pending between words for exactly that reason.
 */
typedef struct vfp_bus {
    uint32_t (*read32 )(arm_cpu_t *c, uint32_t va);
    void     (*write32)(arm_cpu_t *c, uint32_t va, uint32_t v);
    /*
     * Advanced SIMD element accesses (neon.c): `bytes` of 1, 2 or 4, at any
     * alignment the ordinary load/store rules allow (ARM ARM MemU: unaligned
     * is fine unless SCTLR.A), little-endian, faults latched the same way.
     * align_fault latches the alignment fault an explicit alignment
     * qualifier raises. NULL on a bus that serves only the VFP.
     */
    uint32_t (*read_el )(arm_cpu_t *c, uint32_t va, unsigned bytes);
    void     (*write_el)(arm_cpu_t *c, uint32_t va, unsigned bytes, uint32_t v);
    void     (*align_fault)(arm_cpu_t *c, uint32_t va, bool write);
} vfp_bus_t;

/*
 * True when CP15 CPACR grants the current mode access to CP10 and CP11.
 * Lives here rather than in the interpreter because it is half of the
 * lazy-enable gate that vfp_execute also applies.
 */
bool vfp_cpacr_permits(const arm_cpu_t *c);

/* True when FPEXC.EN is set, i.e. VFP is enabled for the running thread. */
bool vfp_enabled(const arm_cpu_t *c);

/*
 * Execute one VFP encoding. `insn` must already have been identified as a
 * cp10/cp11 encoding by the caller and its condition code must already have
 * passed. Returns ARM_OK; ARM_UNDEFINED for anything not implemented or for a
 * disabled-unit access (the lazy-enable trap); or ARM_GUEST_UNDEFINED for an
 * architecturally denied access. arm_step recognizes the disabled-unit
 * ARM_UNDEFINED separately and routes it plus ARM_GUEST_UNDEFINED to the
 * guest's handler while keeping capability gaps fail-closed.
 *
 * VFP never writes r15, so the caller's `next` is unaffected.
 */
arm_status_t vfp_execute(arm_cpu_t *c, uint32_t pc, uint32_t insn,
                         const vfp_bus_t *bus);

/*
 * Execute one Advanced SIMD (NEON) encoding on an ARMv7 core: data
 * processing (ARM 0xF2/0xF3) or an element/structure load or store (0xF4),
 * given in its ARM encoding (the Thumb forms map onto it). The same status
 * contract as vfp_execute, including the silent ARM_UNDEFINED of the
 * lazy-enable trap. core/src/arm/neon.c.
 */
arm_status_t neon_execute(arm_cpu_t *c, uint32_t pc, uint32_t insn,
                          const vfp_bus_t *bus);

/* ------------------------------------------- the cached interpreter's path --
 *
 * The cached interpreter decodes a VFP instruction once, when it builds a
 * block, and runs the common scalar forms without vfp_execute's decode tree.
 * These are the only entry points it uses; everything they refuse goes to
 * vfp_execute, which is the definition. A refusal changes nothing, so the
 * fallback sees exactly the state the fast path saw.
 */

/* vfp_cpacr_permits() && vfp_enabled(), inline: the gate every fast form
 * applies before touching VFP state. */
static inline bool vfp_usable(const arm_cpu_t *c) {
    unsigned cp10 = (c->cp15.cpacr >> ARM_CPACR_CP10_SHIFT) & 3u;
    unsigned cp11 = (c->cp15.cpacr >> ARM_CPACR_CP11_SHIFT) & 3u;
    unsigned acc  = cp10 < cp11 ? cp10 : cp11;
    if ((c->vfp_fpexc & ARM_FPEXC_EN) == 0u) return false;
    if (acc == 3u) return true;
    return acc == 1u && (c->cpsr & ARM_CPSR_MODE_MASK) != ARM_MODE_USR;
}

/* Scalar data-processing forms (VFP_FAST_*), single or double precision. */
enum {
    VFP_FAST_ADD, VFP_FAST_SUB, VFP_FAST_MUL, VFP_FAST_NMUL, VFP_FAST_DIV,
    VFP_FAST_MLA, VFP_FAST_MLS, VFP_FAST_NMLA, VFP_FAST_NMLS,
    VFP_FAST_CPY, VFP_FAST_ABS, VFP_FAST_NEG, VFP_FAST_SQRT,
    VFP_FAST_CMP, VFP_FAST_CMPE, VFP_FAST_CMPZ, VFP_FAST_CMPEZ,
    VFP_FAST_COUNT
};

/*
 * Decode `insn` (a CDP on cp10/cp11) into one of the forms above, with its
 * register numbers (S numbers for single precision, D numbers for double).
 * false for every other encoding and for every encoding vfp_execute would
 * refuse, so a decoded form is always one the unit defines.
 */
bool vfp_fast_decode_dp(uint32_t insn, unsigned *form, bool *dbl,
                        unsigned *d, unsigned *n, unsigned *m);

/*
 * Execute a decoded form. false, with nothing changed, unless VFP is usable
 * and FPSCR selects no trap enable, no directed rounding and no short vector
 * (LEN/STRIDE), or when flush-to-zero leaves the result ambiguous; the caller
 * then runs vfp_execute. Results, FPSCR flags and NZCV are vfp_execute's own:
 * the same f32_do/f64_do rounding steps and the same compare.
 */
bool vfp_fast_dp(arm_cpu_t *c, unsigned form, bool dbl,
                 unsigned d, unsigned n, unsigned m);

/*
 * Why the last refused access came back, as a short human-readable phrase, or
 * NULL if the last vfp_execute did not trap. Unsupported instructions are also
 * printed to stderr; architectural guest exceptions are intentionally quiet.
 */
const char *vfp_trap_reason(void);

#endif /* S5LBOX_VFP_H */
