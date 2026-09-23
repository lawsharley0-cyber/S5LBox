/*
 * S5LBox — guest CPU benchmark workloads, shared by the guest image and the
 * host harness.
 *
 * The same workloads.c is compiled twice: once by Clang for a bare-metal
 * ARMv6 guest (ARM and Thumb images, see tools/gen_guest_bench.py) and once
 * by the host compiler inside tools/guest_bench.c. The host copy computes the
 * checksum the guest must report, so every benchmark run is also a
 * correctness check that needs no stored golden values.
 *
 * Everything here must therefore mean the same thing on a 32-bit ARM guest and
 * a 64-bit host: fixed-width types only, no plain `char` (signed on x86,
 * unsigned on ARM), no pointer values in results, no undefined or
 * implementation-defined arithmetic, floats only in the IEEE-exact operations
 * (+ - * / sqrt, conversions) with contraction disabled.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_BENCH_GUEST_WORKLOADS_H
#define S5LBOX_BENCH_GUEST_WORKLOADS_H

#include <stdint.h>

typedef enum {
    WL_BIGNUM = 0,  /* multi-precision multiply-accumulate (UMULL/UMLAL)   */
    WL_CRC32,       /* table CRC-32: byte loads, shifts, table lookups     */
    WL_SHA1,        /* SHA-1 compression: rotates, adds, word loads        */
    WL_MEMOPS,      /* copy/fill/scan loops, struct copies (LDM/STM)       */
    WL_SORT,        /* quicksort + insertion sort: branches, recursion     */
    WL_RASTER,      /* fixed-point span fill with alpha blend (32 bpp)     */
    WL_CALLS,       /* recursion, indirect calls, tiny leaf functions      */
    WL_MMU,         /* page-strided walks and cross-page pointer chasing   */
    WL_INTERP,      /* bytecode interpreter: switch jump tables            */
    WL_VFP,         /* scalar VFPv2 kernels (ARM image only)               */
    WL_SVC,         /* SVC round trips through an exception handler        */
    WL_COUNT
} wl_id_t;

/* Scratch memory the workloads own. The guest links it in .bss; the host
 * harness allocates one. 6 MiB: large enough that WL_MMU spans hundreds of
 * 4 KiB pages. Sub-regions are carved out by offset inside workloads.c. */
#define WL_ARENA_BYTES (6u << 20)

/* Run workload `id` for `scale` units of work over `arena`. Returns the
 * checksum. `svc_count` points at the counter the guest SVC handler bumps;
 * the host passes its own and emulates the handler (see workloads.c). */
uint32_t wl_run(uint32_t id, uint32_t scale, uint8_t *arena,
                volatile uint32_t *svc_count);

/* Short names used by the harness and in reports. */
static inline const char *wl_name(uint32_t id) {
    static const char *const names[WL_COUNT] = {
        "bignum", "crc32", "sha1", "memops", "sort", "raster",
        "calls", "mmu", "interp", "vfp", "svc",
    };
    return id < WL_COUNT ? names[id] : "?";
}

#endif /* S5LBOX_BENCH_GUEST_WORKLOADS_H */
