/*
 * S5LBox — compiled guest workloads as a correctness test.
 *
 * Every workload in bench/guest runs at a small scale, in all three images
 * (ARM and Thumb-1 on the ARM1176, Thumb-2 on the Cortex-A8 profile) and both
 * privilege levels, through s5l8900_run() with the MMU on and
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
                uint64_t fiq_digest = 0, fiq_retired = 0;
                uint32_t fiq_count = 0;
                for (size_t b = 0; b < sizeof k_backends / sizeof k_backends[0]; b++) {
                    for (int variant = 0; variant < 3; variant++) {
                        /* 0/1: direct RAM writes off/on; 2: on, with a
                         * periodic timer FIQ every 7 timebase ticks. */
                        int direct = variant != 0;
                        gb_config_t cfg = {
                            .workload = w, .scale = scale, .isa = (gb_isa_t)isa,
                            .user = user != 0, .backend = k_backends[b].id,
                            .direct_writes = direct != 0,
                            .fiq_period = variant == 2 ? 7u : 0u,
                        };
                        gb_result_t res;
                        bool ok = gb_run(&cfg, &res);
                        runs++;
                        /* Timer interrupts change the executed path (the
                         * handler runs), so the FIQ variant has its own
                         * reference: the interpreter with the same period. */
                        bool first = (b == 0 && variant == 0);
                        if (first) {
                            ref_digest = res.state_digest;
                            ref_retired = res.retired;
                        }
                        if (b == 0 && variant == 2) {
                            fiq_digest = res.state_digest;
                            fiq_retired = res.retired;
                            fiq_count = res.fiqs;
                        }
                        uint64_t want_digest = variant == 2 ? fiq_digest : ref_digest;
                        uint64_t want_retired = variant == 2 ? fiq_retired : ref_retired;
                        bool same = res.state_digest == want_digest &&
                                    res.retired == want_retired &&
                                    (variant != 2 || (res.fiqs == fiq_count && res.fiqs > 0u));
                        if (!ok || !same) {
                            printf("FAIL %-6s %-5s %-4s %-6s variant=%d: completed=%d "
                                   "done=0x%x status=%d result=0x%08x expected=0x%08x "
                                   "retired=%" PRIu64 "/%" PRIu64 " digest %s pc=0x%08x\n",
                                   wl_name(w), gb_isa_name((gb_isa_t)isa),
                                   user ? "user" : "svc", k_backends[b].name, variant,
                                   res.completed, res.done_code, (int)res.status,
                                   res.result, res.expected, res.retired, want_retired,
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
