/*
 * S5LBox — compiled guest workloads as a correctness test.
 *
 * Every workload in bench/guest runs at a small scale, in both images (ARM and
 * Thumb-1) and both privilege levels, through s5l8900_run() with the MMU on and
 * device ticks live. Each run must finish, report the checksum the host
 * computes from the same C source, and -- for every backend listed below --
 * end in exactly the architectural state the reference interpreter reaches.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "guest_bench.h"
#include "../../bench/guest/workloads.h"

#include <inttypes.h>
#include <stdio.h>

static const struct { s5l8900_cpu_backend_t id; const char *name; } k_backends[] = {
    { S5L8900_CPU_BACKEND_INTERPRETER,  "interp" },
    { S5L8900_CPU_BACKEND_CACHED_BLOCK, "cached" },
};

int main(void) {
    int failures = 0, runs = 0;
    for (uint32_t w = 0; w < WL_COUNT; w++) {
        for (int isa = 0; isa < GB_ISA_COUNT; isa++) {
            if (!gb_workload_supported(w, (gb_isa_t)isa)) continue;
            for (int user = 0; user <= 1; user++) {
                if (w == WL_SVC && !user) continue;
                uint32_t scale = gb_default_scale(w) / 200u;
                if (scale == 0u) scale = 1u;
                uint64_t ref_digest = 0, ref_retired = 0;
                for (size_t b = 0; b < sizeof k_backends / sizeof k_backends[0]; b++) {
                    for (int direct = 0; direct <= 1; direct++) {
                        gb_config_t cfg = {
                            .workload = w, .scale = scale, .isa = (gb_isa_t)isa,
                            .user = user != 0, .backend = k_backends[b].id,
                            .direct_writes = direct != 0,
                        };
                        gb_result_t res;
                        bool ok = gb_run(&cfg, &res);
                        runs++;
                        bool first = (b == 0 && direct == 0);
                        if (first) {
                            ref_digest = res.state_digest;
                            ref_retired = res.retired;
                        }
                        bool same = res.state_digest == ref_digest &&
                                    res.retired == ref_retired;
                        if (!ok || !same) {
                            printf("FAIL %-6s %-5s %-4s %-6s direct=%d: completed=%d "
                                   "done=0x%x status=%d result=0x%08x expected=0x%08x "
                                   "retired=%" PRIu64 "/%" PRIu64 " digest %s pc=0x%08x\n",
                                   wl_name(w), gb_isa_name((gb_isa_t)isa),
                                   user ? "user" : "svc", k_backends[b].name, direct,
                                   res.completed, res.done_code, (int)res.status,
                                   res.result, res.expected, res.retired, ref_retired,
                                   same ? "same" : "DIFFERS", res.final_pc);
                            failures++;
                        }
                    }
                }
            }
        }
    }
    printf("guest workloads: %d runs, %d failures\n", runs, failures);
    return failures ? 1 : 0;
}
