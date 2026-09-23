/*
 * S5LBox — interfaces between the reference interpreter (arm_interp.c) and
 * the cached interpreter (arm_ci.c). Internal to core/src/arm; not installed.
 *
 * Everything here is reference semantics, reached without a fetch. The cached
 * interpreter uses it for every instruction it does not specialise and for
 * every slow path of the ones it does, so both engines share one definition of
 * what an instruction means.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_ARM_INTERNAL_H
#define S5LBOX_ARM_INTERNAL_H

#include "arm.h"

/* Execute one already fetched instruction at `pc` exactly as arm_step() would
 * after its fetch: cycles++, condition, execution, data-abort completion,
 * undefined-instruction handling and the r15 update. Interrupt sampling and
 * the fetch itself (with its prefetch abort) are NOT performed; the caller
 * owns them. */
arm_status_t arm_exec_arm_insn(arm_cpu_t *c, uint32_t pc, uint32_t insn);
arm_status_t arm_exec_thumb_insn(arm_cpu_t *c, uint32_t pc, uint16_t insn);

/* The same for an ARM-state VFP instruction (cp10/cp11 MCR/MRC, CDP,
 * LDC/STC, MCRR/MRRC; condition field not 0xF) whose condition the caller
 * has already found to pass: arm_exec_arm_insn() minus the decode tree that
 * leads to the VFP unit. cycles++, the unit, data-abort completion, the
 * lazy-enable Undefined discrimination and the r15 update are identical. */
arm_status_t arm_exec_vfp_insn(arm_cpu_t *c, uint32_t pc, uint32_t insn);

/* The interpreter's data accessors, with every rule they implement: alignment
 * faults (SCTLR.A), ARMv6 unaligned support (SCTLR.U), legacy rotation,
 * page-crossing splits, translation and the dread/dwrite caches. A fault is
 * latched in c->abort_pending (the value read is then meaningless). */
uint32_t arm_mem_read32(arm_cpu_t *c, uint32_t va, bool priv);
uint32_t arm_mem_read16(arm_cpu_t *c, uint32_t va, bool priv);
uint32_t arm_mem_read8 (arm_cpu_t *c, uint32_t va, bool priv);
void     arm_mem_write32(arm_cpu_t *c, uint32_t va, uint32_t v, bool priv);
void     arm_mem_write16(arm_cpu_t *c, uint32_t va, uint32_t v, bool priv);
void     arm_mem_write8 (arm_cpu_t *c, uint32_t va, uint32_t v, bool priv);

/* Complete a latched data abort for the instruction at `pc` (LR_abt = pc+8,
 * DFSR/DFAR, ABT mode, vector) and clear abort_pending. */
void arm_take_data_abort(arm_cpu_t *c, uint32_t pc);

#endif /* S5LBOX_ARM_INTERNAL_H */
