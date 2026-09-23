/*
 * S5LBox — the cached interpreter: a JIT-free block engine for the ARM1176.
 *
 * WHAT IT IS. A layer over the reference interpreter (arm_step) that decodes
 * each guest basic block once into compact 16-byte operation records, keeps
 * them in a cache keyed by physical address, virtual address and CPSR.T, and
 * executes them with handlers specialised for the hot instruction forms. It
 * generates no host code: records are data, and every handler is ordinary
 * compiled C, so it runs unchanged on Windows, Linux, macOS and stock iOS.
 * Design, evidence and rejected alternatives: docs/IMPLEMENTATION_PLAN.md.
 *
 * WHAT IT PROMISES. For every program, the same architectural state, the same
 * guest RAM, the same retired-instruction count and the same device-visible
 * timing as arm_step() driven by s5l8900_run(). Instructions it does not
 * specialise run through the reference semantics (arm_exec_*_insn) inside the
 * block; instructions that can advance device time or need the machine (SVC,
 * coprocessor 14/15 incl. WFI, BKPT, the unconditional space except
 * BLX/PLD/CLREX, undefined encodings) are never executed here: the engine
 * stops in front of them and the caller runs one arm_step().
 *
 * WHAT THE CALLER OWNS. The budget: arm_ci_run() never retires more
 * instructions than asked, so the machine passes the exact distance to the
 * next device-time edge. Interrupt entry: the engine returns instead of taking
 * an asserted, unmasked IRQ/FIQ. Invalidation of host writes to guest RAM that
 * bypass the bus (arm_ci_note_ram_write / arm_ci_flush).
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_ARM_CI_H
#define S5LBOX_ARM_CI_H

#include "arm.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct arm_ci arm_ci_t;

typedef struct {
    /* Guest DRAM: host pointer, guest physical base and size. Blocks are only
     * built from this range; code outside it runs on arm_step. */
    uint8_t  *ram;
    uint32_t  ram_base;
    uint32_t  ram_size;
    /* The machine's "a device was touched" flag (s5l8900_t::level_dirty).
     * The engine stops after any instruction that sets it. May be NULL. */
    const bool *level_dirty;
} arm_ci_config_t;

/* Why arm_ci_run() returned. */
typedef enum {
    ARM_CI_STOP_BUDGET = 0, /* retired exactly the budget                     */
    ARM_CI_STOP_STEP,       /* the next instruction must go through arm_step */
    ARM_CI_STOP_EVENT,      /* stopped after an instruction the machine must
                               see: MMIO, CPSR control change, exception,
                               write to cached code                           */
    ARM_CI_STOP_STATUS      /* an instruction returned a non-OK status        */
} arm_ci_stop_t;

/* Why an instruction was handed to arm_step instead of the engine (a STEP
 * stop), for arm_ci_stats_t.step_cause. */
typedef enum {
    ARM_CI_STEP_SVC = 0,        /* SVC / SWI                                   */
    ARM_CI_STEP_CP15_TLS,       /* MRC/MCR p15 c13 (thread / context ID)       */
    ARM_CI_STEP_WFI,            /* MCR p15, 0, rX, c7, c0, 4                    */
    ARM_CI_STEP_CP15,           /* any other CP15 (caches, TLB, barriers, ...)  */
    ARM_CI_STEP_CP14,
    ARM_CI_STEP_OTHER,          /* BKPT, undefined, CPS/SRS/RFE, reserved      */
    ARM_CI_STEP_EXCEPTION,      /* pending IRQ/FIQ/abort or invalid mode        */
    ARM_CI_STEP_FETCH,          /* PC misaligned, not in RAM, or fetch fault    */
    ARM_CI_STEP_CAUSES
} arm_ci_step_cause_t;

/* Instructions decoded as "run the reference" (CI_K_REF), by class, for
 * arm_ci_stats_t.ref_class. */
typedef enum {
    ARM_CI_REF_OTHER = 0,
    ARM_CI_REF_VFP,             /* coprocessor 10/11                            */
    ARM_CI_REF_BLOCK,           /* LDM/STM forms without a fast path            */
    ARM_CI_REF_STATUS,          /* MSR, MRS, CPS, SETEND, other misc DP space   */
    ARM_CI_REF_MEM,             /* single/extra/exclusive load-store corner     */
    ARM_CI_REF_MEDIA,           /* media, saturating and DSP multiplies         */
    ARM_CI_REF_PC,              /* writes or reads PC in a special way          */
    ARM_CI_REF_CLASSES
} arm_ci_ref_class_t;

typedef struct {
    uint64_t runs;              /* arm_ci_run calls                          */
    uint64_t retired;           /* instructions retired by the engine        */
    uint64_t ref_retired;       /* ...of which through reference semantics   */
    uint64_t block_execs;       /* blocks entered                            */
    uint64_t lookups, hits;     /* block cache                               */
    uint64_t builds;            /* blocks decoded                            */
    uint64_t build_ops;         /* ops decoded                               */
    uint64_t stale;             /* lookups that found an invalidated block   */
    uint64_t flushes;           /* whole-cache flushes                       */
    uint64_t invalidations;     /* code regions invalidated by writes        */
    uint64_t verify_mismatch;   /* verify mode: cached code != RAM (a bug)   */
    uint64_t stop[4];           /* by arm_ci_stop_t                          */
    uint64_t mem_fast, mem_slow;/* data accesses by path                     */
    uint64_t step_cause[ARM_CI_STEP_CAUSES];  /* STEP stops, by cause        */
    uint64_t ref_class[ARM_CI_REF_CLASSES];   /* decoded-REF ops retired     */
    uint64_t ref_fallback;      /* specialised ops that took the reference
                                   path at run time (TLB miss, MMIO, ...)     */
} arm_ci_stats_t;

/* One human-readable summary of the counters. total_retired, when non-zero,
 * is every instruction the machine retired (engine plus arm_step), so the
 * engine's share can be shown. Always NUL-terminates when cap > 0. */
size_t arm_ci_describe_stats(const arm_ci_stats_t *st, uint64_t total_retired,
                             char *out, size_t cap);

arm_ci_t *arm_ci_create(const arm_ci_config_t *cfg);
void      arm_ci_destroy(arm_ci_t *ci);

/*
 * Retire up to `budget` instructions. Returns the number retired (0 when the
 * very next instruction must go through arm_step). *status is ARM_OK unless
 * an instruction returned otherwise, exactly as arm_step would have; that
 * instruction is not counted. Never takes interrupts, never executes SVC,
 * coprocessor 14/15 or anything that can advance device time.
 */
unsigned arm_ci_run(arm_ci_t *ci, arm_cpu_t *cpu, unsigned budget,
                    arm_status_t *status, arm_ci_stop_t *stop);

/* Guest RAM at [pa, pa+len) was written by something other than the CPU's
 * own store paths (DMA done without the bus, host memcpy, loaders). */
void arm_ci_note_ram_write(arm_ci_t *ci, uint32_t pa, uint32_t len);
/* Forget every cached block (snapshot restore, wholesale RAM replacement). */
void arm_ci_flush(arm_ci_t *ci);

/* True when [pa, pa+len) overlaps cached code. The machine refuses direct
 * host write pointers into such ranges, so every store to cached code goes
 * through the bus, where arm_ci_note_ram_write invalidates it. */
bool arm_ci_range_has_code(const arm_ci_t *ci, uint32_t pa, uint32_t len);

/* Verify mode: before running a cached block, compare its instruction words
 * with RAM. A mismatch is counted in verify_mismatch and the block rebuilt.
 * For tests and for hunting a missed RAM writer on a real guest. */
void arm_ci_set_verify(arm_ci_t *ci, bool on);

void arm_ci_get_stats(const arm_ci_t *ci, arm_ci_stats_t *out);
void arm_ci_reset_stats(arm_ci_t *ci);

#endif /* S5LBOX_ARM_CI_H */
