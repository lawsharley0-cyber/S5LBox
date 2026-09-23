/*
 * S5LBox — run the compiled guest benchmark workloads on a real machine.
 *
 * Shared by tools/cpubench.c (throughput) and core/tests/test_guest_workloads.c
 * (correctness). A run builds a fresh s5l8900_t with 32 MiB of DRAM, maps
 * guest VA 0..16 MiB onto DRAM with ARMv6 4 KiB small pages (so every access
 * is translated and VA != PA), loads one image from
 * bench/guest/generated/guest_images.h, enters it in SVC mode through the
 * app-facing s5l8900_run() in 100,000-instruction chunks with device ticks
 * live, and stops when the guest raises its mailbox flag.
 *
 * Every run is checked: the guest's result must equal the checksum computed by
 * compiling the same workloads.c for the host, and the final architectural
 * state is summarised in a digest so two CPU backends can be compared exactly.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_TOOLS_GUEST_BENCH_H
#define S5LBOX_TOOLS_GUEST_BENCH_H

#include "soc.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum { GB_ISA_ARM = 0, GB_ISA_THUMB = 1, GB_ISA_COUNT } gb_isa_t;

typedef struct {
    uint32_t              workload;   /* wl_id_t */
    uint32_t              scale;
    gb_isa_t              isa;
    bool                  user;       /* run the workload in User mode */
    s5l8900_cpu_backend_t backend;
    bool                  direct_writes; /* the app's direct-RAM-write contract */
    uint64_t              max_insns;  /* 0: a generous default */
} gb_config_t;

typedef struct {
    bool     completed;     /* the guest raised `done == 1` */
    uint32_t done_code;     /* raw done word: 1, or 0xf0x for a guest fault */
    arm_status_t status;    /* last s5l8900_run status */
    uint32_t result;        /* guest checksum */
    uint32_t expected;      /* host checksum for the same workload/scale */
    uint64_t retired;       /* instructions retired (cpu.cycles delta) */
    double   seconds;       /* wall time inside s5l8900_run() only */
    uint64_t state_digest;  /* architectural CPU state + guest DRAM */
    uint32_t final_pc, final_cpsr;
} gb_result_t;

/* True when the guest completed, reported the host checksum and every
 * s5l8900_run() call returned ARM_OK. */
bool gb_run(const gb_config_t *cfg, gb_result_t *out);

/* Host reference checksum for (workload, scale) as the given ISA's image
 * computes it (the Thumb image compiles the VFP workload out). */
uint32_t gb_expected(uint32_t workload, uint32_t scale, gb_isa_t isa);

/* Default benchmark scale for a workload: sized so that one run retires tens
 * of millions of instructions on the reference interpreter. */
uint32_t gb_default_scale(uint32_t workload);

bool gb_workload_supported(uint32_t workload, gb_isa_t isa);
const char *gb_isa_name(gb_isa_t isa);
double gb_now_seconds(void);

#endif /* S5LBOX_TOOLS_GUEST_BENCH_H */
