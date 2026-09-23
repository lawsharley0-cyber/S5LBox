/*
 * S5LBox — compiled guest workload runner. See guest_bench.h.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "guest_bench.h"
#include "../bench/guest/workloads.h"
#include "../bench/guest/generated/guest_images.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define GB_RAM_BASE   0x08000000u
#define GB_RAM_SIZE   (32u << 20)
#define GB_MAP_BYTES  (16u << 20)            /* VA 0 .. 16 MiB -> DRAM       */
#define GB_L1_PA      (GB_RAM_BASE + GB_MAP_BYTES)   /* 16 KiB aligned       */
#define GB_L2_PA      (GB_L1_PA + 0x4000u)            /* 16 x 1 KiB tables    */
#define GB_CHUNK      100000u                /* the iOS app's chunk size     */

/* Mailbox word offsets; must match bench/guest/start.S and runtime.c. */
#define MB_ID     0u
#define MB_SCALE  4u
#define MB_USER   8u
#define MB_RESULT 12u
#define MB_DONE   16u
#define MB_FIQ    20u
/* Device pages start.S uses for the optional timer FIQ. */
#define GB_TIMER_VA 0x00fe0000u
#define GB_VIC_VA   0x00ff0000u

typedef struct {
    const uint8_t *image;
    uint32_t size, entry, mbox, bss_end, fiq_counter;
} gb_image_t;

static const gb_image_t g_images[GB_ISA_COUNT] = {
    { guest_bench_arm_image, GUEST_BENCH_ARM_SIZE, GUEST_BENCH_ARM_START,
      GUEST_BENCH_ARM_MBOX, GUEST_BENCH_ARM_BSSEND, GUEST_BENCH_ARM_FIQCOUNTER },
    { guest_bench_thumb_image, GUEST_BENCH_THUMB_SIZE, GUEST_BENCH_THUMB_START,
      GUEST_BENCH_THUMB_MBOX, GUEST_BENCH_THUMB_BSSEND, GUEST_BENCH_THUMB_FIQCOUNTER },
};

#if defined(CLOCK_MONOTONIC)
double gb_now_seconds(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
#elif defined(TIME_UTC)
double gb_now_seconds(void) {
    struct timespec ts;
    if (timespec_get(&ts, TIME_UTC) != TIME_UTC) return -1.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
#else
double gb_now_seconds(void) {
    return (double)clock() / (double)CLOCKS_PER_SEC;
}
#endif

const char *gb_isa_name(gb_isa_t isa) {
    return isa == GB_ISA_THUMB ? "thumb" : "arm";
}

bool gb_workload_supported(uint32_t workload, gb_isa_t isa) {
    if (workload >= WL_COUNT) return false;
    return !(workload == WL_VFP && isa == GB_ISA_THUMB);
}

uint32_t gb_default_scale(uint32_t workload) {
    /* Sized so the ARM image retires roughly 25 M guest instructions per run
     * (measured with cpubench; the insns= column reports it). Thumb-1 retires
     * more for the same work, e.g. bignum calls __aeabi_lmul for every
     * 32x32->64 multiply because Thumb-1 has no UMULL. */
    switch (workload) {
        case WL_BIGNUM: return 2400u;
        case WL_CRC32:  return 200u;
        case WL_SHA1:   return 140u;
        case WL_MEMOPS: return 40u;
        case WL_SORT:   return 50u;
        case WL_RASTER: return 500u;
        case WL_CALLS:  return 2400u;
        case WL_MMU:    return 1600u;
        case WL_INTERP: return 500u;
        case WL_VFP:    return 9000u;
        case WL_SVC:    return 1200000u;
        default:        return 1u;
    }
}

uint32_t gb_expected(uint32_t workload, uint32_t scale, gb_isa_t isa) {
    if (!gb_workload_supported(workload, isa))
        return workload == WL_VFP ? 0u : 0xdeadbeefu;
    uint8_t *arena = calloc(WL_ARENA_BYTES, 1);
    volatile uint32_t svc = 0;
    if (!arena) return 0xffffffffu;
    uint32_t r = wl_run(workload, scale, arena, &svc);
    free(arena);
    return r;
}

static void poke32(s5l8900_t *m, uint32_t pa, uint32_t v) {
    m->bus.write32(m->bus.ctx, pa, v);
}

static uint32_t peek32(s5l8900_t *m, uint32_t pa) {
    return m->bus.read32(m->bus.ctx, pa);
}

/* ARMv6 extended (SCTLR.XP) small pages, AP=3 (read/write for all), domain 0
 * as a client so every access is permission-checked. */
static void build_tables(s5l8900_t *m) {
    for (uint32_t mb = 0; mb < GB_MAP_BYTES >> 20; mb++) {
        uint32_t l2 = GB_L2_PA + mb * 1024u;
        poke32(m, GB_L1_PA + mb * 4u, l2 | 0x001u);
        for (uint32_t p = 0; p < 256u; p++) {
            uint32_t va = (mb << 20) | (p << 12);
            poke32(m, l2 + p * 4u, (GB_RAM_BASE + va) | 0x032u);
        }
    }
    /* Two device pages for the optional timer FIQ: MMIO through the MMU. */
    poke32(m, GB_L2_PA + (GB_TIMER_VA >> 20) * 1024u + ((GB_TIMER_VA >> 12) & 0xffu) * 4u,
           S5L8900_TIMER_BASE | 0x032u);
    poke32(m, GB_L2_PA + (GB_VIC_VA >> 20) * 1024u + ((GB_VIC_VA >> 12) & 0xffu) * 4u,
           S5L8900_VIC0_BASE | 0x032u);
}

static uint64_t fnv64(uint64_t h, const void *data, size_t n) {
    const uint8_t *p = data;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static uint64_t fnv64_u32(uint64_t h, uint32_t v) {
    return fnv64(h, &v, sizeof v);
}

/* Architectural state only: host caches, TLB contents and counters are
 * excluded because they legitimately differ between backends. */
static uint64_t state_digest(s5l8900_t *m) {
    const arm_cpu_t *c = &m->cpu;
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 16; i++) h = fnv64_u32(h, c->r[i]);
    h = fnv64_u32(h, c->cpsr);
    for (int i = 0; i < ARM_BANK_COUNT; i++) {
        h = fnv64_u32(h, c->spsr[i]);
        h = fnv64_u32(h, c->bank_r13[i]);
        h = fnv64_u32(h, c->bank_r14[i]);
    }
    for (int i = 0; i < 5; i++) {
        h = fnv64_u32(h, c->fiq_r8_12[i]);
        h = fnv64_u32(h, c->usr_r8_12[i]);
    }
    h = fnv64(h, &c->cycles, sizeof c->cycles);
    h = fnv64_u32(h, c->excl_valid ? 1u : 0u);
    h = fnv64_u32(h, c->excl_addr);
    h = fnv64_u32(h, c->vfp_fpexc);
    h = fnv64_u32(h, c->vfp_fpscr);
    for (int i = 0; i < 32; i++) h = fnv64_u32(h, c->vfp_s[i]);
    h = fnv64(h, &c->cp15, sizeof c->cp15);
    /* Guest DRAM word by word: a 64-bit multiply per word keeps a 16 MiB
     * digest cheap enough to take after every test run. */
    const uint8_t *ram = m->bus.host_ram(m->bus.ctx, GB_RAM_BASE, GB_MAP_BYTES);
    if (ram) {
        for (uint32_t off = 0; off < GB_MAP_BYTES; off += 4u) {
            uint32_t w;
            memcpy(&w, ram + off, 4);
            h = (h ^ w) * 1099511628211ull;
        }
    }
    return h;
}

bool gb_run(const gb_config_t *cfg, gb_result_t *out) {
    memset(out, 0, sizeof *out);
    out->status = ARM_OK;
    if (!cfg || cfg->isa >= GB_ISA_COUNT ||
        !gb_workload_supported(cfg->workload, cfg->isa))
        return false;
    const gb_image_t *img = &g_images[cfg->isa];

    s5l8900_t *m = calloc(1, sizeof *m);
    if (!m) return false;
    if (!s5l8900_init(m, GB_RAM_BASE, GB_RAM_SIZE)) { free(m); return false; }
    (void)s5l8900_set_direct_ram_writes(m, cfg->direct_writes);
    s5l8900_set_cpu_backend(m, cfg->backend);

    s5l8900_load(m, GB_RAM_BASE, img->image, img->size);
    poke32(m, GB_RAM_BASE + img->mbox + MB_ID, cfg->workload);
    poke32(m, GB_RAM_BASE + img->mbox + MB_SCALE, cfg->scale);
    poke32(m, GB_RAM_BASE + img->mbox + MB_USER, cfg->user ? 1u : 0u);
    poke32(m, GB_RAM_BASE + img->mbox + MB_FIQ, cfg->fiq_period);
    build_tables(m);

    arm_cpu_t *c = &m->cpu;
    c->cp15.sctlr = ARM_SCTLR_M | ARM_SCTLR_XP | ARM_SCTLR_U;
    c->cp15.ttbr0 = GB_L1_PA;
    c->cp15.ttbcr = 0u;
    c->cp15.dacr  = 0x1u;                    /* domain 0: client */
    c->cp15.cpacr |= 0xfu << ARM_CPACR_CP10_SHIFT;
    c->vfp_fpexc |= ARM_FPEXC_EN;
    arm_mmu_tlb_flush(c);
    c->r[15] = img->entry;

    uint64_t limit = cfg->max_insns ? cfg->max_insns : 4000000000ull;
    uint64_t before = c->cycles;
    uint32_t done = 0;
    double seconds = 0.0;
    const uint32_t done_pa = GB_RAM_BASE + img->mbox + MB_DONE;
    while (done == 0u && c->cycles - before < limit) {
        arm_status_t st = ARM_OK;
        double t0 = gb_now_seconds();
        (void)s5l8900_run(m, GB_CHUNK, &st);
        seconds += gb_now_seconds() - t0;
        out->status = st;
        if (st != ARM_OK) break;
        done = peek32(m, done_pa);
    }

    out->done_code = done;
    out->completed = (done == 1u);
    out->result = peek32(m, GB_RAM_BASE + img->mbox + MB_RESULT);
    out->expected = gb_expected(cfg->workload, cfg->scale, cfg->isa);
    out->retired = c->cycles - before;
    out->seconds = seconds;
    out->final_pc = c->r[15];
    out->final_cpsr = c->cpsr;
    out->fiqs = peek32(m, GB_RAM_BASE + img->fiq_counter);
    out->state_digest = state_digest(m);

    s5l8900_free(m);
    free(m);
    return out->completed && out->status == ARM_OK &&
           out->result == out->expected;
}
