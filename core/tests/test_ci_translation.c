/*
 * S5LBox — the cached interpreter's translation-derived caches (host TLBs and
 * the VA-keyed block map) must never outlive the translation they were filled
 * under, including across the discontinuities cpu->tlb_gen alone cannot show.
 *
 *   1. arm_reset() sets tlb_gen back to 1. A guest (or host) that then remaps
 *      and flushes once is back at the generation the pre-reset entries carry.
 *   2. A snapshot-style restore does the same to the CPU.
 *   3. A host that clears SCTLR.M directly changes every translation without
 *      a flush.
 *
 * Each case loads through the same virtual address before and after, with a
 * different physical page behind it, and checks the value the engine read.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "soc.h"

#include <stdio.h>

#define RAM_BASE  0x08000000u
#define RAM_SIZE  (8u << 20)
#define CODE      RAM_BASE                    /* identity-mapped section     */
#define L1_PA     (RAM_BASE + 0x4000u)        /* 16 KiB aligned, same MiB    */
#define DATA_VA   (RAM_BASE + 0x00100010u)    /* section 0x081               */
#define PA_IDENT  (RAM_BASE + 0x00100000u)    /* what DATA_VA is with M off  */
#define PA_A      (RAM_BASE + 0x00200000u)
#define PA_B      (RAM_BASE + 0x00300000u)
#define PA_C      (RAM_BASE + 0x00400000u)

static s5l8900_t g_m;
static int g_fail;

static void poke32(uint32_t pa, uint32_t v) { g_m.bus.write32(g_m.bus.ctx, pa, v); }

/* Section descriptor, AP = 3 (full access), domain 0, XP format. */
static void map_section(uint32_t va, uint32_t pa) {
    poke32(L1_PA + (va >> 20) * 4u, (pa & 0xfff00000u) | 0xc00u | 0x2u);
}

static void enter(bool mmu) {
    arm_cpu_t *c = &g_m.cpu;
    c->cp15.sctlr = (mmu ? ARM_SCTLR_M : 0u) | ARM_SCTLR_XP;
    c->cp15.ttbr0 = L1_PA;
    c->cp15.ttbcr = 0u;
    c->cp15.dacr = 0x1u;
    c->cpsr = ARM_MODE_SVC | ARM_CPSR_I | ARM_CPSR_F;
    c->r[0] = 0u;
    c->r[1] = DATA_VA;
    c->r[15] = CODE;
}

static void run_and_check(const char *what, uint32_t want) {
    arm_status_t st = ARM_OK;
    (void)s5l8900_run(&g_m, 16u, &st);
    if (st != ARM_OK || g_m.cpu.r[0] != want) {
        printf("FAIL %s: r0=0x%08x want 0x%08x status=%d\n", what, g_m.cpu.r[0], want, (int)st);
        g_fail++;
    } else {
        printf("ok   %s\n", what);
    }
}

int main(void) {
    if (!s5l8900_init(&g_m, RAM_BASE, RAM_SIZE) ||
        !s5l8900_set_cpu_backend(&g_m, S5L8900_CPU_BACKEND_CACHED_BLOCK)) {
        printf("FAIL setup\n");
        return 1;
    }
    (void)s5l8900_set_direct_ram_writes(&g_m, true);

    poke32(CODE + 0u, 0xe5910000u);           /* ldr r0, [r1] */
    poke32(CODE + 4u, 0xeafffffdu);           /* b CODE: the load repeats, so it
                                                 * runs in the engine, not only in
                                                 * the first single step */
    poke32(PA_IDENT + 0x10u, 0x11111111u);
    poke32(PA_A + 0x10u, 0xaaaaaaaau);
    poke32(PA_B + 0x10u, 0xbbbbbbbbu);
    poke32(PA_C + 0x10u, 0xccccccccu);
    for (uint32_t i = 0; i < 4096u; i++) poke32(L1_PA + i * 4u, 0u);
    map_section(CODE, CODE);
    map_section(DATA_VA, PA_A);

    /* Fill the engine's entries: DATA_VA -> PA_A. */
    enter(true);
    run_and_check("mapping A", 0xaaaaaaaau);
    const uint32_t gen_a = g_m.cpu.tlb_gen;

    /* 1. Reset, remap to PA_B, and flush until tlb_gen is back where the
     *    engine's entries were filled. */
    arm_reset(&g_m.cpu, &g_m.bus);
    map_section(DATA_VA, PA_B);
    enter(true);
    arm_mmu_sync_stamp(&g_m.cpu);             /* so the engine's own check won't flush */
    while (g_m.cpu.tlb_gen < gen_a) arm_mmu_tlb_flush(&g_m.cpu);
    if (g_m.cpu.tlb_gen != gen_a) { printf("FAIL setup: generation\n"); g_fail++; }
    run_and_check("after reset, same generation, mapping B", 0xbbbbbbbbu);

    /* 2. A restore: the CPU's own caches are cleared, tlb_gen goes back to 1
     *    and the restore path tells the machine (s5l8900_ram_replaced). */
    const uint32_t gen_b = g_m.cpu.tlb_gen;
    map_section(DATA_VA, PA_C);
    arm_reset(&g_m.cpu, &g_m.bus);            /* the CPU-side effect of a restore */
    s5l8900_ram_replaced(&g_m);
    enter(true);
    arm_mmu_sync_stamp(&g_m.cpu);
    while (g_m.cpu.tlb_gen < gen_b) arm_mmu_tlb_flush(&g_m.cpu);
    if (g_m.cpu.tlb_gen != gen_b) { printf("FAIL setup: generation\n"); g_fail++; }
    run_and_check("after restore, same generation, mapping C", 0xccccccccu);

    /* 3. The host turns the MMU off behind the CPU's back: no flush, the
     *    generation is unchanged, DATA_VA is now its own physical address. */
    enter(false);
    run_and_check("after host clears SCTLR.M", 0x11111111u);

    /* ...and back on: stamp mismatch, a real flush, mapping C again. */
    enter(true);
    run_and_check("after host sets SCTLR.M again", 0xccccccccu);

    s5l8900_free(&g_m);
    printf("ci translation: %d failure(s)\n", g_fail);
    return g_fail ? 1 : 0;
}
