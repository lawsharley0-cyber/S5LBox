/*
 * S5LBox — compiled guest workload benchmark.
 *
 * insnbench measures hand-written ten-instruction loops. This measures real
 * compiler output (bench/guest/workloads.c built with Clang for ARMv6 ARM and
 * Thumb-1) running through the app-facing s5l8900_run() with the MMU on (4 KiB
 * pages, VA != PA) and device ticks live. It is a MEASUREMENT: it has no speed
 * threshold. It exits non-zero only when a run is wrong -- a checksum that
 * differs from the host-computed one, a guest fault, a non-OK run status, or
 * two backends ending in different architectural states.
 *
 *   cpubench [--backend interp,cached] [--workload all|name,...]
 *            [--isa arm,thumb] [--mode user,svc] [--reps N] [--div N]
 *            [--scale N] [--no-direct-writes]
 *
 * Repetitions are interleaved across backends (rep 1: A B, rep 2: A B, ...),
 * so host drift cannot masquerade as a backend difference. The headline is the
 * median rate in M guest instructions per second.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "guest_bench.h"
#include "../bench/guest/workloads.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BACKENDS 4
#define MAX_REPS     64

typedef struct { const char *name; s5l8900_cpu_backend_t id; } backend_t;

static const backend_t g_backend_names[] = {
    { "interp", S5L8900_CPU_BACKEND_INTERPRETER },
    { "cached", S5L8900_CPU_BACKEND_CACHED_BLOCK },
};

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static bool list_has(const char *list, const char *item) {
    size_t n = strlen(item);
    for (const char *p = list; p && *p;) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == n && strncmp(p, item, n) == 0) return true;
        p = e ? e + 1 : NULL;
    }
    return false;
}

static void usage(const char *argv0) {
    printf("usage: %s [--backend interp,cached] [--workload all|name,...]\n"
           "          [--isa arm,thumb] [--mode user,svc] [--reps N]\n"
           "          [--div N] [--scale N] [--no-direct-writes]\n\n"
           "workloads:", argv0);
    for (uint32_t w = 0; w < WL_COUNT; w++) printf(" %s", wl_name(w));
    printf("\n\nPrints median/best/worst M guest instructions/s per row. Exits\n"
           "non-zero if any run is incorrect or backends disagree.\n");
}

int main(int argc, char **argv) {
    const char *backends_arg = "interp", *workloads_arg = "all";
    const char *isa_arg = "arm,thumb", *mode_arg = "user";
    unsigned reps = 3u, div = 1u;
    uint32_t fixed_scale = 0u;
    bool direct_writes = true;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(argv[0]); return 0; }
        else if (!strcmp(argv[i], "--backend") && i + 1 < argc) backends_arg = argv[++i];
        else if (!strcmp(argv[i], "--workload") && i + 1 < argc) workloads_arg = argv[++i];
        else if (!strcmp(argv[i], "--isa") && i + 1 < argc) isa_arg = argv[++i];
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) mode_arg = argv[++i];
        else if (!strcmp(argv[i], "--reps") && i + 1 < argc) reps = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--div") && i + 1 < argc) div = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) fixed_scale = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--no-direct-writes")) direct_writes = false;
        else { fprintf(stderr, "unknown argument: %s\n", argv[i]); usage(argv[0]); return 2; }
    }
    if (reps == 0u || reps > MAX_REPS || div == 0u) { usage(argv[0]); return 2; }

    backend_t backends[MAX_BACKENDS];
    unsigned nb = 0;
    for (size_t i = 0; i < sizeof g_backend_names / sizeof g_backend_names[0]; i++)
        if (list_has(backends_arg, g_backend_names[i].name) && nb < MAX_BACKENDS)
            backends[nb++] = g_backend_names[i];
    if (nb == 0u) { fprintf(stderr, "no known backend in '%s'\n", backends_arg); return 2; }

    printf("CPUBENCH-METHOD reps=%u order=interleaved headline=median unit=Minsn/s "
           "path=s5l8900_run chunk=100000 mmu=4K-pages va!=pa ticks=live direct_writes=%s\n",
           reps, direct_writes ? "on" : "off");
#if defined(NDEBUG)
    printf("CPUBENCH-BUILD optimised (NDEBUG)\n");
#else
    printf("CPUBENCH-BUILD NOT optimised -- numbers are not comparable\n");
#endif

    int failures = 0;
    double log_ratio_sum[MAX_BACKENDS] = { 0 };
    unsigned ratio_rows = 0;

    for (uint32_t w = 0; w < WL_COUNT; w++) {
        if (strcmp(workloads_arg, "all") != 0 && !list_has(workloads_arg, wl_name(w)))
            continue;
        for (int isa = 0; isa < GB_ISA_COUNT; isa++) {
            if (!list_has(isa_arg, gb_isa_name((gb_isa_t)isa))) continue;
            if (!gb_workload_supported(w, (gb_isa_t)isa)) continue;
            for (int user = 1; user >= 0; user--) {
                if (!list_has(mode_arg, user ? "user" : "svc")) continue;
                /* SVC from SVC mode would clobber the caller's own LR. */
                if (w == WL_SVC && !user) continue;
                uint32_t scale = fixed_scale ? fixed_scale : gb_default_scale(w) / div;
                if (scale == 0u) scale = 1u;

                double rates[MAX_BACKENDS][MAX_REPS];
                uint64_t retired[MAX_BACKENDS] = { 0 };
                uint64_t digest0 = 0;
                bool row_ok = true;
                for (unsigned r = 0; r < reps; r++) {
                    for (unsigned b = 0; b < nb; b++) {
                        gb_config_t cfg = {
                            .workload = w, .scale = scale, .isa = (gb_isa_t)isa,
                            .user = user != 0, .backend = backends[b].id,
                            .direct_writes = direct_writes,
                        };
                        gb_result_t res;
                        bool ok = gb_run(&cfg, &res);
                        if (!ok) {
                            printf("CPUBENCH-FAIL wl=%s isa=%s mode=%s backend=%s "
                                   "completed=%d done=0x%x status=%d result=0x%08x "
                                   "expected=0x%08x pc=0x%08x cpsr=0x%08x\n",
                                   wl_name(w), gb_isa_name((gb_isa_t)isa),
                                   user ? "user" : "svc", backends[b].name,
                                   res.completed, res.done_code, (int)res.status,
                                   res.result, res.expected, res.final_pc,
                                   res.final_cpsr);
                            row_ok = false;
                            continue;
                        }
                        if (r == 0u && b == 0u) digest0 = res.state_digest;
                        else if (res.state_digest != digest0) {
                            printf("CPUBENCH-FAIL wl=%s isa=%s mode=%s backend=%s "
                                   "state digest %016" PRIx64 " differs from %016" PRIx64 "\n",
                                   wl_name(w), gb_isa_name((gb_isa_t)isa),
                                   user ? "user" : "svc", backends[b].name,
                                   res.state_digest, digest0);
                            row_ok = false;
                        }
                        retired[b] = res.retired;
                        rates[b][r] = res.seconds > 0.0
                                          ? (double)res.retired / res.seconds / 1e6 : 0.0;
                    }
                }
                if (!row_ok) { failures++; continue; }

                double med[MAX_BACKENDS];
                for (unsigned b = 0; b < nb; b++) {
                    double s[MAX_REPS];
                    memcpy(s, rates[b], reps * sizeof s[0]);
                    qsort(s, reps, sizeof s[0], cmp_double);
                    med[b] = (reps & 1u) ? s[reps / 2u] : 0.5 * (s[reps / 2u - 1u] + s[reps / 2u]);
                    printf("CPUBENCH wl=%-6s isa=%-5s mode=%-4s backend=%-6s "
                           "median=%8.2f best=%8.2f worst=%8.2f Minsn/s insns=%" PRIu64
                           " scale=%u",
                           wl_name(w), gb_isa_name((gb_isa_t)isa), user ? "user" : "svc",
                           backends[b].name, med[b], s[reps - 1u], s[0], retired[b], scale);
                    if (b > 0u && med[0] > 0.0)
                        printf(" vs_%s=%.3fx", backends[0].name, med[b] / med[0]);
                    printf("\n");
                }
                if (nb > 1u && med[0] > 0.0) {
                    for (unsigned b = 1; b < nb; b++)
                        log_ratio_sum[b] += log(med[b] / med[0]);
                    ratio_rows++;
                }
            }
        }
    }
    for (unsigned b = 1; b < nb && ratio_rows; b++)
        printf("CPUBENCH-SUMMARY backend=%s geomean_vs_%s=%.3fx rows=%u\n",
               backends[b].name, backends[0].name,
               exp(log_ratio_sum[b] / ratio_rows), ratio_rows);
    printf("CPUBENCH-DONE failures=%d\n", failures);
    return failures ? 1 : 0;
}
