/*
 * S5LBox — Unit test for Performance Profiler.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "arm_profile.h"
#include <stdio.h>
#include <string.h>

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL: %s at %s:%d\n", msg, __FILE__, __LINE__); \
            return 1; \
        } \
    } while (0)

int main(void) {
    arm_profiler_t prof;
    arm_profiler_init(&prof);

    CHECK(prof.enabled, "Profiler should be enabled on init");

    prof.total_instructions = 1000000;
    prof.elapsed_seconds = 0.05; /* 20 MIPS */
    prof.time_exec = 0.030;
    prof.time_decode = 0.010;
    prof.time_mmu = 0.005;
    prof.time_mem = 0.003;
    prof.time_exceptions = 0.002;

    prof.branch_count = 150000;
    prof.branch_taken_count = 100000;
    prof.tlb_hits = 95000;
    prof.tlb_misses = 5000;
    prof.fetch_hits = 990000;
    prof.fetch_misses = 10000;

    arm_profiler_record_pc(&prof, 0xc0069040, false);
    arm_profiler_record_pc(&prof, 0xc0069040, false);
    arm_profiler_record_pc(&prof, 0xc0069040, false);
    arm_profiler_record_pc(&prof, 0xc0010200, true);

    CHECK(prof.hot_blocks_count == 2, "Should have 2 hot block records");
    CHECK(prof.hot_blocks[0].pc == 0xc0069040, "First hot block should be 0xc0069040");
    CHECK(prof.hot_blocks[0].count == 3, "0xc0069040 count should be 3");
    CHECK(prof.hot_blocks[1].pc == 0xc0010200, "Second hot block should be 0xc0010200");
    CHECK(prof.hot_blocks[1].count == 1, "0xc0010200 count should be 1");

    char report[2048];
    arm_profiler_format_report(&prof, report, sizeof(report));

    CHECK(strstr(report, "20.00 MIPS") != NULL, "Report should contain calculated MIPS");
    CHECK(strstr(report, "0xc0069040") != NULL, "Report should list hottest address");
    CHECK(strstr(report, "TLB Hit Rate:        95.0%") != NULL, "Report should compute TLB hit rate");

    printf("test_profiler: all checks passed!\n");
    return 0;
}
