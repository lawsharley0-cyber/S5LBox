/*
 * S5LBox — what the VFP unit (vfp.c) shares with Advanced SIMD (neon.c).
 *
 * Not a public interface. NEON's floating-point arithmetic is the VFP's own
 * rounding step with a fixed FPSCR (ARM ARM StandardFPSCRValue: flush-to-zero
 * and default NaN on, round to nearest), and its refusals should read the same
 * way and be reported through the same vfp_trap_reason(). One copy of each
 * of those is the only safe number, so neon.c calls into vfp.c for them.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_VFP_PRIVATE_H
#define S5LBOX_VFP_PRIVATE_H

#include "vfp.h"

/*
 * A private marker carried alongside the FPSCR exception bits. It is NOT an
 * FPSCR bit (6 is reserved) and never reaches FPSCR: seeing it means
 * flush-to-zero cannot be decided from the host's rounded result and the
 * instruction must trap. See fz_out32 in vfp.c.
 */
#define VFP_FZ_AMBIGUOUS (1u << 6)
#define VFP_FZ_AMBIGUOUS_WHY                                                  \
    "flush-to-zero cannot be decided: the exact result straddles the smallest " \
    "normal, and the architecture tests it before rounding while the host " \
    "reports it after"

/* ARM ARM StandardFPSCRValue(): what Advanced SIMD arithmetic runs under. */
#define VFP_STANDARD_FPSCR (ARM_FPSCR_DN | ARM_FPSCR_FZ)

typedef enum { VFP_OP_ADD, VFP_OP_SUB, VFP_OP_MUL, VFP_OP_DIV, VFP_OP_SQRT } vfp_op_t;

/* Start an instruction: clears the reason vfp_trap_reason() reports. */
void vfp_begin(void);

/* Refuse an encoding: the same message, reason and status as the VFP's. */
arm_status_t vfp_refuse(uint32_t pc, uint32_t insn, const char *why);

/* One binary32 rounding step under the standard FPSCR, flags into *exc
 * (VFP_FZ_AMBIGUOUS included). b is unused for VFP_OP_SQRT. */
uint32_t vfp_std_f32(vfp_op_t op, uint32_t a, uint32_t b, uint32_t *exc);

/* FPUnpack's flush of a denormal input to a signed zero, setting IDC. */
uint32_t vfp_std_flush32(uint32_t a, uint32_t *exc);

/* An exact binary64 value rounded to binary32, round to nearest, host
 * flags into *exc (the integer and fixed-point to float conversions). */
uint32_t vfp_std_round32(double exact, uint32_t *exc);

/* FPToFixed, round toward zero, saturating to 32 bits (see fp_to_fixed). */
uint32_t vfp_std_to_fixed32(double v, unsigned frac, bool is_signed, uint32_t *exc);

#endif /* S5LBOX_VFP_PRIVATE_H */
