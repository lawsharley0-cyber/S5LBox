/*
 * S5LBox — CPU & System Performance Profiler Implementation.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "arm_profile.h"
#include <stdio.h>
#include <string.h>

void arm_profiler_init(arm_profiler_t *prof) {
    if (!prof) return;
    memset(prof, 0, sizeof(*prof));
    prof->enabled = true;
}

void arm_profiler_reset(arm_profiler_t *prof) {
    if (!prof) return;
    bool was_enabled = prof->enabled;
    memset(prof, 0, sizeof(*prof));
    prof->enabled = was_enabled;
}

void arm_profiler_record_pc(arm_profiler_t *prof, uint32_t pc, bool thumb) {
    if (!prof || !prof->enabled) return;

    /* Search existing entries */
    for (size_t i = 0; i < prof->hot_blocks_count; i++) {
        if (prof->hot_blocks[i].pc == pc && prof->hot_blocks[i].thumb == thumb) {
            prof->hot_blocks[i].count++;
            /* Bubble up if larger than predecessor */
            while (i > 0 && prof->hot_blocks[i].count > prof->hot_blocks[i - 1].count) {
                arm_profile_hot_block_t tmp = prof->hot_blocks[i - 1];
                prof->hot_blocks[i - 1] = prof->hot_blocks[i];
                prof->hot_blocks[i] = tmp;
                i--;
            }
            return;
        }
    }

    /* Insert new entry if room */
    if (prof->hot_blocks_count < ARM_PROFILE_HOT_BLOCKS_CAP) {
        size_t idx = prof->hot_blocks_count++;
        prof->hot_blocks[idx].pc = pc;
        prof->hot_blocks[idx].thumb = thumb;
        prof->hot_blocks[idx].count = 1;
    } else if (prof->hot_blocks[ARM_PROFILE_HOT_BLOCKS_CAP - 1].count == 0) {
        prof->hot_blocks[ARM_PROFILE_HOT_BLOCKS_CAP - 1].pc = pc;
        prof->hot_blocks[ARM_PROFILE_HOT_BLOCKS_CAP - 1].thumb = thumb;
        prof->hot_blocks[ARM_PROFILE_HOT_BLOCKS_CAP - 1].count = 1;
    }
}

void arm_profiler_format_report(const arm_profiler_t *prof, char *out, size_t out_size) {
    if (!prof || !out || out_size == 0) return;

    double total_time = prof->time_decode + prof->time_exec + prof->time_mmu +
                        prof->time_mem + prof->time_exceptions + prof->time_gpu +
                        prof->time_audio + prof->time_idle + prof->time_other;
    if (total_time <= 0.0) total_time = 1.0;

    double p_decode = (prof->time_decode / total_time) * 100.0;
    double p_exec   = (prof->time_exec   / total_time) * 100.0;
    double p_mmu    = (prof->time_mmu    / total_time) * 100.0;
    double p_mem    = (prof->time_mem    / total_time) * 100.0;
    double p_exc    = (prof->time_exceptions / total_time) * 100.0;
    double p_gpu    = (prof->time_gpu    / total_time) * 100.0;
    double p_audio  = (prof->time_audio  / total_time) * 100.0;
    double p_idle   = (prof->time_idle   / total_time) * 100.0;
    double p_other  = (prof->time_other  / total_time) * 100.0;

    double mips = 0.0;
    if (prof->elapsed_seconds > 0.0) {
        mips = ((double)prof->total_instructions / prof->elapsed_seconds) / 1e6;
    }

    uint64_t tlb_total = prof->tlb_hits + prof->tlb_misses;
    double tlb_hit_rate = tlb_total ? ((double)prof->tlb_hits / (double)tlb_total) * 100.0 : 0.0;

    uint64_t fetch_total = prof->fetch_hits + prof->fetch_misses;
    double fetch_hit_rate = fetch_total ? ((double)prof->fetch_hits / (double)fetch_total) * 100.0 : 0.0;

    int n = snprintf(out, out_size,
        "=== S5LBox Performance Profile ===\n"
        "Instructions Retired: %llu (%.2f MIPS)\n"
        "Elapsed Time:         %.3f s\n\n"
        "CPU Execution Breakdown:\n"
        "  %5.1f%% ARM/Thumb Execution\n"
        "  %5.1f%% Instruction Decode\n"
        "  %5.1f%% MMU Translation\n"
        "  %5.1f%% Memory Bus Access\n"
        "  %5.1f%% Exception / Interrupt Handling\n"
        "  %5.1f%% Graphics / Display Sync (CLCD/MBX)\n"
        "  %5.1f%% Audio Processing (WM8991)\n"
        "  %5.1f%% Idle / Polling Loops\n"
        "  %5.1f%% Other\n\n"
        "Control Flow & Exceptions:\n"
        "  Total Branches:     %llu (Taken: %llu)\n"
        "  Syscalls (SVC):     %llu\n"
        "  Exceptions (IRQ):   %llu\n"
        "  Exceptions (FIQ):   %llu\n"
        "  Data/Pref Aborts:   %llu\n"
        "  Idle Loops:         %llu\n\n"
        "Cache & Memory Subsystem:\n"
        "  TLB Hit Rate:       %5.1f%% (%llu hits, %llu misses)\n"
        "  Fetch Hit Rate:     %5.1f%% (%llu hits, %llu misses)\n"
        "  Data Read Cache:    %llu hits, %llu misses\n"
        "  Data Write Cache:   %llu hits, %llu misses\n\n"
        "Hottest Guest Addresses / Basic Blocks:\n",
        (unsigned long long)prof->total_instructions, mips, prof->elapsed_seconds,
        p_exec, p_decode, p_mmu, p_mem, p_exc, p_gpu, p_audio, p_idle, p_other,
        (unsigned long long)prof->branch_count, (unsigned long long)prof->branch_taken_count,
        (unsigned long long)prof->syscall_count,
        (unsigned long long)prof->irq_count,
        (unsigned long long)prof->fiq_count,
        (unsigned long long)prof->abort_count,
        (unsigned long long)prof->idle_loop_count,
        tlb_hit_rate, (unsigned long long)prof->tlb_hits, (unsigned long long)prof->tlb_misses,
        fetch_hit_rate, (unsigned long long)prof->fetch_hits, (unsigned long long)prof->fetch_misses,
        (unsigned long long)prof->dread_hits, (unsigned long long)prof->dread_misses,
        (unsigned long long)prof->dwrite_hits, (unsigned long long)prof->dwrite_misses);

    if (n > 0 && (size_t)n < out_size) {
        size_t off = (size_t)n;
        for (size_t i = 0; i < prof->hot_blocks_count && i < 10; i++) {
            if (prof->hot_blocks[i].count == 0) break;
            int written = snprintf(out + off, out_size - off,
                "  [%2zu] 0x%08x (%s) -> %llu hits\n",
                i + 1,
                prof->hot_blocks[i].pc,
                prof->hot_blocks[i].thumb ? "Thumb" : "ARM",
                (unsigned long long)prof->hot_blocks[i].count);
            if (written <= 0 || (size_t)written >= out_size - off) break;
            off += (size_t)written;
        }
    }
}

void arm_profiler_print_report(const arm_profiler_t *prof) {
    char buf[2048];
    arm_profiler_format_report(prof, buf, sizeof(buf));
    printf("%s\n", buf);
}
