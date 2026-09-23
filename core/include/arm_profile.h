/*
 * S5LBox — CPU & System Performance Profiler.
 *
 * Detailed instrumentation to measure:
 * - Instructions executed per second (MIPS)
 * - Time breakdown: Interpreter decode, Execution, MMU, Memory, Exceptions,
 *   Graphics (CLCD/MBX), Audio (WM8991), Idle loops.
 * - Instruction & control-flow metrics: Branches (taken/not-taken), Syscalls (SVC),
 *   Exceptions (IRQ, FIQ, Aborts).
 * - Cache hit/miss rates: Software TLB, Fetch cache, Data-read/write caches.
 * - Top hottest guest basic blocks and execution addresses.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_ARM_PROFILE_H
#define S5LBOX_ARM_PROFILE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define ARM_PROFILE_HOT_BLOCKS_CAP 32

typedef struct arm_profile_hot_block {
    uint32_t pc;
    uint64_t count;
    bool     thumb;
} arm_profile_hot_block_t;

typedef struct arm_profiler {
    bool enabled;

    /* Retired instruction accounting */
    uint64_t total_instructions;
    double   elapsed_seconds;

    /* Time accounting (in fractional seconds or nanoseconds) */
    double time_decode;
    double time_exec;
    double time_mmu;
    double time_mem;
    double time_exceptions;
    double time_gpu;
    double time_audio;
    double time_idle;
    double time_other;

    /* Frequency counters */
    uint64_t branch_count;
    uint64_t branch_taken_count;
    uint64_t syscall_count;
    uint64_t exception_count;
    uint64_t irq_count;
    uint64_t fiq_count;
    uint64_t abort_count;
    uint64_t idle_loop_count;

    /* Memory / Cache events */
    uint64_t tlb_hits;
    uint64_t tlb_misses;
    uint64_t fetch_hits;
    uint64_t fetch_misses;
    uint64_t dread_hits;
    uint64_t dread_misses;
    uint64_t dwrite_hits;
    uint64_t dwrite_misses;

    /* Hottest execution blocks */
    arm_profile_hot_block_t hot_blocks[ARM_PROFILE_HOT_BLOCKS_CAP];
    size_t                  hot_blocks_count;
} arm_profiler_t;

/* Global or machine-level profiler API */
void arm_profiler_init(arm_profiler_t *prof);
void arm_profiler_reset(arm_profiler_t *prof);
void arm_profiler_record_pc(arm_profiler_t *prof, uint32_t pc, bool thumb);
void arm_profiler_format_report(const arm_profiler_t *prof, char *out, size_t out_size);
void arm_profiler_print_report(const arm_profiler_t *prof);

#endif /* S5LBOX_ARM_PROFILE_H */
