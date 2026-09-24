/*
 * S5LBox -- the cached interpreter's event horizon keeps the per-edge device
 * timeline.
 *
 * s5l8900_run() lets a cached-interpreter run continue past timebase edges
 * while no enabled interrupt source can fire, and brings device time up to
 * each device access inside the run (s5l8900_set_ci_horizon). This test runs
 * one guest program on three machines -- the reference interpreter (one tick
 * per instruction), the engine cut at every edge, and the engine with the
 * horizon -- and requires the same architectural state, the same RAM and the
 * same timer, VIC and timebase state after every chunk.
 *
 * The program (ci_timeline_vectors.S and ci_timeline_main.S beside this file,
 * assembled with llvm-mc --triple=armv6-none-eabi -mcpu=arm1176jzf-s; the
 * words below are that output): a periodic timer-4 interrupt through the VIC,
 * whose handler acknowledges it, logs the tick counter it reads and the main
 * loop's progress, and every 8th time reprograms the period from the counter;
 * and a main loop of varying length that reads the 64-bit tick counter and
 * the live down-count (timer reads do not end an engine run, so these are the
 * reads the catch-up must get right), reads the VIC's raw lines (which do end
 * it), idles in WFI, and masks and unmasks interrupts around a delay.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "soc.h"
#include "arm_ci.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAM_BASE   0x08000000u
#define RAM_SIZE   (4u << 20)
#define L1_PA      (RAM_BASE + 0x00300000u)
#define L2_PA      (L1_PA + 0x4000u)
#define VEC_VA     0x00000000u
#define IRQ_OFF    0x100u
#define CODE_VA    0x00010000u
#define DATA_VA    0x00020000u
#define DATA_BYTES 0x4000u
#define VIC_VA     0x00026000u
#define TIMER_VA   0x00027000u
#define PHYS(va)   (RAM_BASE + 0x00100000u + (va))

#define RUN_INSNS  3000000u
#define CHUNK      100000u

/* ci_timeline_vectors.S: the eight vectors (IRQ -> +0x100, the rest park). */
static const uint32_t k_vectors[] = {
    0xeafffffeu, 0xeafffffeu, 0xeafffffeu, 0xeafffffeu,
    0xeafffffeu, 0xeafffffeu, 0xea000038u, 0xeafffffeu,
};
/* ci_timeline_vectors.S: the IRQ handler at +0x100. */
static const uint32_t k_irq[] = {
    0xe92d401fu, 0xe3a00a27u, 0xe59010f8u, 0xe58010f4u, 0xe5902084u, 0xe3a03802u,
    0xe5934000u, 0xe4842004u, 0xe484a004u, 0xe3540a22u, 0x23a04b81u, 0xe5834000u,
    0xe5931004u, 0xe2811001u, 0xe5831004u, 0xe3110007u, 0x1a000002u, 0xe202203fu,
    0xe2822017u, 0xe58020a8u, 0xe8bd401fu, 0xe25ef004u,
};
/* ci_timeline_main.S */
static const uint32_t k_main[] = {
    0xe321f0d2u, 0xe3a0da23u, 0xe321f0d3u, 0xe3a0db8eu, 0xe3a03802u, 0xe3a01b81u,
    0xe5831000u, 0xe3a01000u, 0xe5831004u, 0xe3a00a26u, 0xe3a01080u, 0xe5801010u,
    0xe3a00a27u, 0xe3a01025u, 0xe58010a8u, 0xe3a01003u, 0xe58010a4u, 0xe3a0a000u,
    0xe3a06001u, 0xe3a07002u, 0xe3a08000u, 0xe3a09000u, 0xf1080080u, 0xe20a50ffu,
    0xe2855032u, 0xe0866005u, 0xe02771e6u, 0xe2555001u, 0x1afffffbu, 0xe31a0003u,
    0x1a000006u, 0xe3a00a27u, 0xe5901084u, 0xe0888001u, 0xe5902080u, 0xe02883e2u,
    0xe59010b4u, 0xe0888181u, 0xe31a000fu, 0x1a000002u, 0xe3a00a26u, 0xe5901008u,
    0xe0899001u, 0xe31a007fu, 0x1a000001u, 0xe3a00000u, 0xee070f90u, 0xe31a0c03u,
    0x1a000004u, 0xf10c0080u, 0xe3a010c8u, 0xe2511001u, 0x1afffffdu, 0xf1080080u,
    0xe28aa001u, 0xeaffffdeu,
};

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; printf("FAIL: "); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static void poke32(s5l8900_t *m, uint32_t pa, uint32_t v) {
    m->bus.write32(m->bus.ctx, pa, v);
}

static void map_page(s5l8900_t *m, uint32_t va, uint32_t pa) {
    poke32(m, L2_PA + ((va >> 12) & 0xffu) * 4u, (pa & 0xfffff000u) | 0x30u | 0x2u);
}

typedef enum { V_INTERP, V_EDGE, V_HORIZON } variant_t;

static bool setup(s5l8900_t *m, variant_t v) {
    if (!s5l8900_init(m, RAM_BASE, RAM_SIZE)) return false;
    if (v != V_INTERP &&
        !s5l8900_set_cpu_backend(m, S5L8900_CPU_BACKEND_CACHED_BLOCK))
        return false;
    (void)s5l8900_set_ci_horizon(m, v == V_HORIZON);
    for (uint32_t i = 0; i < 4096u; i++) poke32(m, L1_PA + i * 4u, 0u);
    poke32(m, L1_PA, L2_PA | 0x1u);
    for (uint32_t i = 0; i < 256u; i++) poke32(m, L2_PA + i * 4u, 0u);
    map_page(m, VEC_VA, PHYS(VEC_VA));
    map_page(m, CODE_VA, PHYS(CODE_VA));
    for (uint32_t p = 0; p < DATA_BYTES; p += 0x1000u)
        map_page(m, DATA_VA + p, PHYS(DATA_VA + p));
    map_page(m, VIC_VA, S5L8900_VIC0_BASE);
    map_page(m, TIMER_VA, S5L8900_TIMER_BASE);
    for (uint32_t i = 0; i < 0x1000u; i += 4u) poke32(m, PHYS(VEC_VA) + i, 0u);
    for (size_t i = 0; i < sizeof k_vectors / 4u; i++)
        poke32(m, PHYS(VEC_VA) + (uint32_t)i * 4u, k_vectors[i]);
    for (size_t i = 0; i < sizeof k_irq / 4u; i++)
        poke32(m, PHYS(VEC_VA) + IRQ_OFF + (uint32_t)i * 4u, k_irq[i]);
    for (size_t i = 0; i < sizeof k_main / 4u; i++)
        poke32(m, PHYS(CODE_VA) + (uint32_t)i * 4u, k_main[i]);
    for (uint32_t i = 0; i < DATA_BYTES; i += 4u) poke32(m, PHYS(DATA_VA) + i, 0u);

    arm_cpu_t *c = &m->cpu;
    c->cp15.ttbr0 = L1_PA;
    c->cp15.ttbcr = 0;
    c->cp15.dacr = 0x1u;
    c->cp15.sctlr |= ARM_SCTLR_M | ARM_SCTLR_XP | ARM_SCTLR_U;
    arm_set_mode(c, ARM_MODE_SVC);
    c->cpsr = ARM_MODE_SVC | ARM_CPSR_I | ARM_CPSR_F;
    c->r[15] = CODE_VA;
    arm_mmu_tlb_flush(c);
    s5l8900_tick(m, 0u);
    return true;
}

static int compare(const char *what, uint64_t at, const s5l8900_t *a,
                   const s5l8900_t *b) {
    const arm_cpu_t *x = &a->cpu, *y = &b->cpu;
    int bad = 0;
#define SAME(field, fmt) do { if ((x->field) != (y->field)) { bad++; \
        printf("  %s at %" PRIu64 ": " #field " " fmt " vs " fmt "\n", what, at, \
               x->field, y->field); } } while (0)
    for (int i = 0; i < 16; i++) SAME(r[i], "%08x");
    SAME(cpsr, "%08x");
    for (int i = 0; i < ARM_BANK_COUNT; i++) {
        SAME(spsr[i], "%08x");
        SAME(bank_r13[i], "%08x");
        SAME(bank_r14[i], "%08x");
    }
    SAME(cycles, "%" PRIu64);
    SAME(irq_line, "%d");
    SAME(fiq_line, "%d");
#undef SAME
#define MSAME(field, fmt) do { if ((a->field) != (b->field)) { bad++; \
        printf("  %s at %" PRIu64 ": " #field " " fmt " vs " fmt "\n", what, at, \
               a->field, b->field); } } while (0)
    MSAME(tb_accum, "%" PRIu64);
    MSAME(timer.ticks, "%" PRIu64);
    MSAME(timer.t4_value, "%u");
    MSAME(timer.t4_count, "%u");
    MSAME(timer.t4_state, "%u");
    MSAME(timer.irqlatch, "%08x");
    MSAME(vic[0].raw, "%08x");
    MSAME(vic[0].enable, "%08x");
#undef MSAME
    const uint8_t *da = a->ram + (PHYS(DATA_VA) - RAM_BASE);
    const uint8_t *db = b->ram + (PHYS(DATA_VA) - RAM_BASE);
    if (memcmp(da, db, DATA_BYTES) != 0) {
        bad++;
        for (uint32_t i = 0; i < DATA_BYTES; i += 4u) {
            uint32_t p, q;
            memcpy(&p, da + i, 4);
            memcpy(&q, db + i, 4);
            if (p != q) {
                printf("  %s at %" PRIu64 ": data+%04x %08x vs %08x\n", what, at,
                       i, p, q);
                break;
            }
        }
    }
    return bad;
}

int main(void) {
    static s5l8900_t mi, me, mh;
    if (!setup(&mi, V_INTERP) || !setup(&me, V_EDGE) || !setup(&mh, V_HORIZON)) {
        printf("FAIL: machine setup\n");
        return 1;
    }

    /* The same chunks on all three, compared after every chunk, so a
     * divergence is reported near where it happened. */
    uint64_t total = 0;
    int reported = 0;
    while (total < RUN_INSNS) {
        arm_status_t si = ARM_OK, se = ARM_OK, sh = ARM_OK;
        const unsigned ni = s5l8900_run(&mi, CHUNK, &si);
        const unsigned ne = s5l8900_run(&me, CHUNK, &se);
        const unsigned nh = s5l8900_run(&mh, CHUNK, &sh);
        total += ni;
        if (si != ARM_OK || se != ARM_OK || sh != ARM_OK || ni != ne || ni != nh) {
            CHECK(0, "at %" PRIu64 ": retired %u %u %u, status %d %d %d", total,
                  ni, ne, nh, (int)si, (int)se, (int)sh);
            break;
        }
        if (reported < 3) {
            const int e = compare("per edge", total, &mi, &me);
            const int h = compare("horizon", total, &mi, &mh);
            CHECK(e == 0, "the engine cut at every edge diverged by %" PRIu64, total);
            CHECK(h == 0, "the engine with the event horizon diverged by %" PRIu64,
                  total);
            if (e || h) reported++;
        }
        if (ni == 0u) break;
    }

    /* The program must have done what it claims, or equality proves little. */
    uint32_t irqs = 0, logp = 0;
    memcpy(&logp, mi.ram + (PHYS(DATA_VA) - RAM_BASE), 4);
    memcpy(&irqs, mi.ram + (PHYS(DATA_VA) - RAM_BASE) + 4u, 4);
    CHECK(total >= RUN_INSNS, "only %" PRIu64 " instructions ran", total);
    CHECK(irqs > 200u, "only %u timer interrupts were taken", irqs);
    CHECK(logp > DATA_VA + 0x400u, "the interrupt log is empty");
    CHECK(mi.cpu.r[10] > 1000u, "the main loop ran %u times", mi.cpu.r[10]);

    /* And the horizon must actually have lengthened the engine's runs. */
    arm_ci_stats_t est, hst;
    arm_ci_get_stats(me.ci, &est);
    arm_ci_get_stats(mh.ci, &hst);
    const double edge_len = est.runs ? (double)est.retired / (double)est.runs : 0.0;
    const double hor_len = hst.runs ? (double)hst.retired / (double)hst.runs : 0.0;
    printf("  %u interrupts, main loop %u; engine runs: per edge %" PRIu64
           " (%.1f insns each), horizon %" PRIu64 " (%.1f insns each)\n",
           irqs, mi.cpu.r[10], est.runs, edge_len, hst.runs, hor_len);
    CHECK(edge_len < 80.0 && hor_len > 3.0 * edge_len,
          "the horizon did not lengthen runs (%.1f vs %.1f)", hor_len, edge_len);

    s5l8900_free(&mi);
    s5l8900_free(&me);
    s5l8900_free(&mh);
    printf("ci timeline: %s\n", g_fail ? "FAILED" : "identical on all three machines");
    return g_fail ? 1 : 0;
}
