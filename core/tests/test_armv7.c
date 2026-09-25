/*
 * S5LBox — ARMv7 profile tests: Thumb-2 and the IT block on the Cortex-A8
 * profile, the ARM-state ARMv7 additions, and the rule that every one of them
 * stays refused on the ARM1176.
 *
 * The instruction semantics are compared against an independent engine by
 * tools/unicorn_thumb2_diff.py, and compiled Thumb-2 code runs in
 * test_guest_workloads. What neither reaches is here: state that lives across
 * an exception (ITSTATE in the SPSR, the Thumb undefined-instruction link), a
 * 32-bit instruction whose halves sit in different pages, the lazy-VFP trap
 * from Thumb, and where the two profiles must differ.
 *
 * Encodings are from llvm-mc -triple=thumbv7a / armv7a -mcpu=cortex-a8.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "arm.h"
#include "arm_ci.h"
#include "vfp.h"
#include <stdio.h>
#include <string.h>

#define RAM_SIZE (1u << 20)
static uint8_t g_ram[RAM_SIZE];
static unsigned g_wfi_calls;

static uint32_t m_r32(void *ctx, uint32_t a) { (void)ctx; uint32_t v; memcpy(&v, &g_ram[a & (RAM_SIZE - 1)], 4); return v; }
static uint16_t m_r16(void *ctx, uint32_t a) { (void)ctx; uint16_t v; memcpy(&v, &g_ram[a & (RAM_SIZE - 1)], 2); return v; }
static uint8_t  m_r8 (void *ctx, uint32_t a) { (void)ctx; return g_ram[a & (RAM_SIZE - 1)]; }
static void m_w32(void *ctx, uint32_t a, uint32_t v) { (void)ctx; memcpy(&g_ram[a & (RAM_SIZE - 1)], &v, 4); }
static void m_w16(void *ctx, uint32_t a, uint16_t v) { (void)ctx; memcpy(&g_ram[a & (RAM_SIZE - 1)], &v, 2); }
static void m_w8 (void *ctx, uint32_t a, uint8_t  v) { (void)ctx; g_ram[a & (RAM_SIZE - 1)] = v; }
static uint8_t *m_host_ram(void *ctx, uint32_t a, uint32_t len) {
    (void)ctx;
    if (!len || (uint64_t)a + len > RAM_SIZE) return NULL;
    return &g_ram[a];
}
static bool m_wfi(void *ctx) { (void)ctx; g_wfi_calls++; return false; }

static const arm_bus_t g_bus = {
    .ctx     = NULL,
    .read32  = m_r32, .read16  = m_r16, .read8  = m_r8,
    .write32 = m_w32, .write16 = m_w16, .write8 = m_w8,
    .wait_for_interrupt = m_wfi,
};
/* The same, plus host RAM, which turns on the 1 KB fetch-block fast path. */
static const arm_bus_t g_bus_fast = {
    .ctx     = NULL,
    .read32  = m_r32, .read16  = m_r16, .read8  = m_r8,
    .write32 = m_w32, .write16 = m_w16, .write8 = m_w8,
    .host_ram = m_host_ram,
};

static int g_pass, g_fail;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static void put16(uint32_t a, uint16_t v) { m_w16(NULL, a, v); }

/* A fresh core of the given profile in System mode, RAM cleared. */
static void boot(arm_cpu_t *c, const arm_bus_t *bus, arm_arch_t arch,
                 uint32_t pc, bool thumb) {
    memset(g_ram, 0, sizeof g_ram);
    memset(c, 0, sizeof *c);
    c->arch = arch;                 /* before reset: reset reads it */
    arm_reset(c, bus);
    c->cpsr = (c->cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_SYS;
    if (thumb) c->cpsr |= ARM_CPSR_T;
    c->r[15] = pc;
}

static uint32_t itstate(uint32_t psr) {
    return ((psr >> 8) & 0xfcu) | ((psr >> 25) & 3u);
}

static void enable_vfp(arm_cpu_t *c) {
    c->cp15.cpacr |= 0xfu << ARM_CPACR_CP10_SHIFT;
    c->vfp_fpexc |= ARM_FPEXC_EN;
}

/* ------------------------------------------------------------------------ */

static void test_profile_predicates(void) {
    CHECK(!arm_arch_is_v7(ARM_ARCH_V6_ARM1176), "ARM1176 is not ARMv7");
    CHECK(arm_arch_is_v7(ARM_ARCH_V7_A8) && arm_arch_is_v7(ARM_ARCH_V7_SWIFT),
          "both ARMv7 cores are ARMv7");
    /* The reason for predicates rather than an ordering: A8 > SWIFT in the
     * enum, and the A8 has no divider. */
    CHECK(!arm_arch_has_divide(ARM_ARCH_V7_A8), "the Cortex-A8 has no divider");
    CHECK(arm_arch_has_divide(ARM_ARCH_V7_SWIFT), "Swift divides");
    CHECK(!arm_arch_has_divide(ARM_ARCH_V6_ARM1176), "the ARM1176 does not");
}

static void test_divide_follows_the_core(void) {
    static const arm_arch_t ARCH[] = { ARM_ARCH_V6_ARM1176, ARM_ARCH_V7_A8,
                                       ARM_ARCH_V7_SWIFT };
    for (unsigned i = 0; i < 3; i++) {
        arm_cpu_t c;
        const bool has = arm_arch_has_divide(ARCH[i]);
        /* ARM: SDIV r2, r1, r0 */
        boot(&c, &g_bus, ARCH[i], 0, false);
        m_w32(NULL, 0, 0xe712f011u);
        c.r[1] = 100u; c.r[0] = 7u;
        arm_status_t st = arm_step(&c);
        CHECK(has ? (st == ARM_OK && c.r[2] == 14u) : st == ARM_UNDEFINED,
              "arch %u ARM SDIV: status %d r2=%u", (unsigned)ARCH[i], (int)st, c.r[2]);
        /* Thumb: SDIV r2, r1, r0 (0xfb91 0xf2f0). The ARM1176 has no Thumb-2
         * at all, so only the ARMv7 cores are asked. */
        if (!arm_arch_is_v7(ARCH[i])) continue;
        boot(&c, &g_bus, ARCH[i], 0x100, true);
        put16(0x100, 0xfb91); put16(0x102, 0xf2f0);
        c.r[1] = 100u; c.r[0] = 7u;
        st = arm_step(&c);
        CHECK(has ? (st == ARM_OK && c.r[2] == 14u) : st == ARM_UNDEFINED,
              "arch %u Thumb SDIV: status %d r2=%u", (unsigned)ARCH[i], (int)st, c.r[2]);
    }
}

static void test_bl_is_one_instruction_on_armv7(void) {
    /* BL .+0x100 = 0xf000 0xf87e. The ARM1176 runs the halves as two 16-bit
     * instructions, exactly as it always has; the A8 runs one 32-bit one. */
    arm_cpu_t c;
    boot(&c, &g_bus, ARM_ARCH_V6_ARM1176, 0x100, true);
    put16(0x100, 0xf000); put16(0x102, 0xf87e);
    CHECK(arm_step(&c) == ARM_OK && c.r[15] == 0x102u,
          "ARM1176: the first half alone moves pc to 0x102, got %08x", c.r[15]);
    CHECK(arm_step(&c) == ARM_OK && c.r[15] == 0x200u && c.r[14] == 0x105u,
          "ARM1176: the second half branches, pc=%08x lr=%08x", c.r[15], c.r[14]);
    CHECK(c.cycles == 2u, "ARM1176: two retirements, got %llu",
          (unsigned long long)c.cycles);

    boot(&c, &g_bus, ARM_ARCH_V7_A8, 0x100, true);
    put16(0x100, 0xf000); put16(0x102, 0xf87e);
    CHECK(arm_step(&c) == ARM_OK && c.r[15] == 0x200u && c.r[14] == 0x105u,
          "A8: one step branches, pc=%08x lr=%08x", c.r[15], c.r[14]);
    CHECK(c.cycles == 1u, "A8: one retirement, got %llu", (unsigned long long)c.cycles);
}

static void test_sctlr_u_and_xp_read_as_one(void) {
    arm_cpu_t c;
    boot(&c, &g_bus, ARM_ARCH_V7_A8, 0, false);
    const uint32_t both = ARM_SCTLR_U | ARM_SCTLR_XP;
    CHECK((c.cp15.sctlr & both) == both, "A8 reset: sctlr=%08x", c.cp15.sctlr);
    m_w32(NULL, 0, 0xee010f10u);          /* MCR p15, 0, r0, c1, c0, 0 */
    c.r[0] = 0u;
    CHECK(arm_step(&c) == ARM_OK && (c.cp15.sctlr & both) == both,
          "A8: writing 0 leaves U and XP set, sctlr=%08x", c.cp15.sctlr);

    boot(&c, &g_bus, ARM_ARCH_V6_ARM1176, 0, false);
    CHECK(c.cp15.sctlr == 0u, "ARM1176 reset: sctlr=%08x", c.cp15.sctlr);
}

static void test_it_block_conditions_and_flags(void) {
    /* ITE EQ; ADDEQ r0, r0, r1 (16-bit, sets flags only outside IT);
     * MOVNE r0, #2 (likewise). */
    for (int z = 0; z <= 1; z++) {
        arm_cpu_t c;
        boot(&c, &g_bus, ARM_ARCH_V7_A8, 0x100, true);
        put16(0x100, 0xbf0c); put16(0x102, 0x1840); put16(0x104, 0x2002);
        c.r[0] = 5u; c.r[1] = 7u;
        c.cpsr |= ARM_CPSR_N | (z ? ARM_CPSR_Z : 0u);
        for (int i = 0; i < 3; i++) arm_step(&c);
        CHECK(c.r[15] == 0x106u, "z=%d: pc=%08x", z, c.r[15]);
        CHECK(c.r[0] == (z ? 12u : 2u), "z=%d: r0=%u", z, c.r[0]);
        CHECK((c.cpsr & ARM_CPSR_N) && ((c.cpsr & ARM_CPSR_Z) != 0) == (z != 0),
              "z=%d: flags changed inside the IT block, cpsr=%08x", z, c.cpsr);
        CHECK(itstate(c.cpsr) == 0u, "z=%d: ITSTATE left %02x", z, itstate(c.cpsr));
    }
    /* Outside an IT block the same 16-bit ADD sets flags. */
    arm_cpu_t c;
    boot(&c, &g_bus, ARM_ARCH_V7_A8, 0x100, true);
    put16(0x100, 0x1840);
    c.r[0] = 0xffffffffu; c.r[1] = 1u;
    arm_step(&c);
    CHECK(c.r[0] == 0u && (c.cpsr & ARM_CPSR_Z) && (c.cpsr & ARM_CPSR_C),
          "ADDS outside IT: r0=%08x cpsr=%08x", c.r[0], c.cpsr);
}

static void test_svc_in_it_block_stacks_the_next_state(void) {
    /* ITT EQ; SVCEQ #5; MOVEQ r0, #1. The SVC's return address is the MOVEQ,
     * so the SPSR must carry the MOVEQ's ITSTATE, and the handler's return
     * must resume the block. Vector 0x08: MOVS pc, lr. */
    arm_cpu_t c;
    boot(&c, &g_bus, ARM_ARCH_V7_A8, 0x100, true);
    m_w32(NULL, ARM_VEC_SWI, 0xe1b0f00eu);
    put16(0x100, 0xbf04); put16(0x102, 0xdf05); put16(0x104, 0x2001);
    put16(0x106, 0x2109);                               /* MOVS r1, #9 */
    c.cpsr |= ARM_CPSR_Z;

    arm_step(&c);                                       /* ITT EQ */
    CHECK(itstate(c.cpsr) == 0x04u, "after IT: ITSTATE %02x", itstate(c.cpsr));
    arm_step(&c);                                       /* SVCEQ */
    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_SVC && c.r[15] == ARM_VEC_SWI,
          "SVC taken: cpsr=%08x pc=%08x", c.cpsr, c.r[15]);
    CHECK(itstate(c.cpsr) == 0u && !(c.cpsr & ARM_CPSR_T),
          "the handler runs outside any IT block, in ARM state: cpsr=%08x", c.cpsr);
    CHECK(c.r[14] == 0x104u, "LR_svc=%08x expect 104", c.r[14]);
    const uint32_t spsr = c.spsr[arm_bank_of_mode(ARM_MODE_SVC)];
    CHECK(itstate(spsr) == 0x08u && (spsr & ARM_CPSR_T),
          "SPSR_svc carries the MOVEQ's state 08, got %02x (spsr=%08x)",
          itstate(spsr), spsr);
    arm_step(&c);                                       /* MOVS pc, lr */
    CHECK(c.r[15] == 0x104u && itstate(c.cpsr) == 0x08u && (c.cpsr & ARM_CPSR_T),
          "back in the block: pc=%08x cpsr=%08x", c.r[15], c.cpsr);
    c.r[0] = 0u;
    c.cpsr &= ~ARM_CPSR_Z;                              /* now EQ fails ... */
    arm_step(&c);
    CHECK(c.r[0] == 0u && itstate(c.cpsr) == 0u,
          "... so MOVEQ is skipped and the block ends: r0=%u IT=%02x",
          c.r[0], itstate(c.cpsr));
    arm_step(&c);                                       /* MOVS, outside */
    CHECK(c.r[1] == 9u && (c.cpsr & ARM_CPSR_Z) == 0u, "after the block r1=%u", c.r[1]);

    /* With EQ failing from the start the SVC is not taken at all. */
    boot(&c, &g_bus, ARM_ARCH_V7_A8, 0x100, true);
    put16(0x100, 0xbf04); put16(0x102, 0xdf05); put16(0x104, 0x2001);
    for (int i = 0; i < 3; i++) arm_step(&c);
    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_SYS && c.r[15] == 0x106u,
          "a skipped SVC takes no exception: cpsr=%08x pc=%08x", c.cpsr, c.r[15]);
}

static void test_abort_in_it_block_stacks_its_own_state(void) {
    /* ITE EQ; LDMEQ.W r1, {r2, r3}; MOVNE r0, #2. r1 is misaligned, which
     * ARMv7 faults for LDM whatever SCTLR.A says. The LDM re-executes after
     * the handler, so the SPSR must carry the LDM's own ITSTATE (0c), not the
     * MOVNE's. */
    arm_cpu_t c;
    boot(&c, &g_bus, ARM_ARCH_V7_A8, 0x100, true);
    put16(0x100, 0xbf0c); put16(0x102, 0xe891); put16(0x104, 0x000c);
    put16(0x106, 0x2002);
    c.cpsr |= ARM_CPSR_Z;
    c.r[1] = 0x2002u; c.r[2] = 0x22u; c.r[3] = 0x33u;
    arm_step(&c);
    arm_step(&c);
    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_ABT && c.r[15] == ARM_VEC_DATA_ABORT,
          "data abort: cpsr=%08x pc=%08x", c.cpsr, c.r[15]);
    CHECK((c.cp15.dfsr & 0xfu) == ARM_FSR_ALIGNMENT && c.cp15.dfar == 0x2002u,
          "alignment fault at 2002: dfsr=%08x dfar=%08x", c.cp15.dfsr, c.cp15.dfar);
    CHECK(c.r[14] == 0x10au, "LR_abt=%08x expect the LDM + 8", c.r[14]);
    const uint32_t spsr = c.spsr[arm_bank_of_mode(ARM_MODE_ABT)];
    CHECK(itstate(spsr) == 0x0cu, "SPSR_abt ITSTATE %02x expect 0c", itstate(spsr));
    CHECK(itstate(c.cpsr) == 0u, "handler ITSTATE %02x", itstate(c.cpsr));
    CHECK(c.r[2] == 0x22u && c.r[3] == 0x33u, "the LDM wrote nothing");
}

static void test_lazy_vfp_trap_from_thumb(void) {
    /* IT EQ; VADDEQ.F32 s0, s1, s2 (0xee30 0x0a81), VFP off. This is the
     * encoding the kernel expects to trap and enable VFP for: an Undefined
     * exception with LR = the instruction + 2 (the FIRST halfword + 2, even
     * for a 32-bit instruction), T and the VADD's own ITSTATE in the SPSR. */
    arm_cpu_t c;
    boot(&c, &g_bus, ARM_ARCH_V7_A8, 0x100, true);
    put16(0x100, 0xbf08); put16(0x102, 0xee30); put16(0x104, 0x0a81);
    c.cpsr |= ARM_CPSR_Z;
    arm_step(&c);
    arm_status_t st = arm_step(&c);
    CHECK(st == ARM_OK && (c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_UND &&
          c.r[15] == ARM_VEC_UNDEFINED,
          "the lazy-enable trap is vectored: status %d cpsr=%08x pc=%08x",
          (int)st, c.cpsr, c.r[15]);
    CHECK(c.r[14] == 0x104u, "LR_und=%08x expect 104", c.r[14]);
    const uint32_t spsr = c.spsr[arm_bank_of_mode(ARM_MODE_UND)];
    CHECK((spsr & ARM_CPSR_T) && itstate(spsr) == 0x08u,
          "SPSR_und: T and ITSTATE 08, got %08x", spsr);

    /* With VFP on, the same instruction computes. */
    boot(&c, &g_bus, ARM_ARCH_V7_A8, 0x100, true);
    enable_vfp(&c);
    put16(0x100, 0xbf08); put16(0x102, 0xee30); put16(0x104, 0x0a81);
    c.cpsr |= ARM_CPSR_Z;
    c.vfp_s[1] = 0x3fc00000u;                           /* 1.5 */
    c.vfp_s[2] = 0x40100000u;                           /* 2.25 */
    arm_step(&c);
    st = arm_step(&c);
    CHECK(st == ARM_OK && c.vfp_s[0] == 0x40700000u && c.r[15] == 0x106u,
          "VADD in Thumb: s0=%08x (3.75 is 40700000) pc=%08x", c.vfp_s[0], c.r[15]);

    /* And an undefined non-VFP Thumb-2 encoding still stops the machine
     * rather than reaching the guest: UDF.W (0xf7f0 0xa000). */
    boot(&c, &g_bus, ARM_ARCH_V7_A8, 0x100, true);
    put16(0x100, 0xf7f0); put16(0x102, 0xa000);
    CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[15] == 0x100u,
          "UDF.W halts at its own pc, got %08x", c.r[15]);
}

static void test_vldr_literal_uses_the_thumb_pc(void) {
    /* VLDR s0, [pc, #4] (0xed9f 0x0a01) at 0x102: Thumb forms the literal
     * address as Align(0x102 + 4, 4) + 4 = 0x108. The ARM rule (pc + 8)
     * would read 0x10e. */
    arm_cpu_t c;
    boot(&c, &g_bus, ARM_ARCH_V7_A8, 0x102, true);
    enable_vfp(&c);
    put16(0x102, 0xed9f); put16(0x104, 0x0a01);
    m_w32(NULL, 0x108, 0x11111111u);
    m_w32(NULL, 0x10c, 0x22222222u);
    CHECK(arm_step(&c) == ARM_OK && c.vfp_s[0] == 0x11111111u && c.r[15] == 0x106u,
          "s0=%08x expect the word at 108; pc=%08x", c.vfp_s[0], c.r[15]);
}

/* Map VA page 0x80000000 to pa0 and 0x80001000 to pa1 (0: unmapped), with
 * an identity section for the translation tables themselves. */
static void two_pages(arm_cpu_t *c, uint32_t pa0, uint32_t pa1) {
    const uint32_t l1 = 0x4000, l2 = 0x5000;
    m_w32(NULL, l1 + (0x000u << 2), (3u << 10) | 8u | 2u);
    m_w32(NULL, l1 + (0x800u << 2), l2 | 1u);
    m_w32(NULL, l2 + 0u, pa0 | (3u << 4) | 8u | 2u);
    if (pa1) m_w32(NULL, l2 + 4u, pa1 | (3u << 4) | 8u | 2u);
    c->cp15.ttbr0 = l1;
    c->cp15.dacr = 1u;
    c->cp15.sctlr |= ARM_SCTLR_M;
    arm_mmu_tlb_flush(c);
}

static void test_wide_instruction_across_a_page(void) {
    /* MOVW r0, #0x1234 = 0xf241 0x2034 at VA 0x80000ffe: the halves are in
     * different pages, which map to physical pages that are not adjacent. */
    arm_cpu_t c;
    boot(&c, &g_bus, ARM_ARCH_V7_A8, 0x80000ffeu, true);
    two_pages(&c, 0x10000u, 0x30000u);
    put16(0x10ffe, 0xf241); put16(0x30000, 0x2034);
    put16(0x11000, 0xffff);                 /* what a wrong "pa + 2" would read */
    CHECK(arm_step(&c) == ARM_OK && c.r[0] == 0x1234u && c.r[15] == 0x80001002u,
          "straddling MOVW: r0=%08x pc=%08x", c.r[0], c.r[15]);

    /* Second page unmapped: a prefetch abort on the whole instruction, with
     * IFAR naming the half that failed and the link the usual pc + 4. */
    boot(&c, &g_bus, ARM_ARCH_V7_A8, 0x80000ffeu, true);
    two_pages(&c, 0x10000u, 0u);
    put16(0x10ffe, 0xf241);
    c.r[0] = 0x5a5au;
    CHECK(arm_step(&c) == ARM_OK && c.r[15] == ARM_VEC_PREFETCH &&
          (c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_ABT,
          "prefetch abort: pc=%08x cpsr=%08x", c.r[15], c.cpsr);
    CHECK(c.cp15.ifar == 0x80001000u, "IFAR=%08x expect 80001000", c.cp15.ifar);
    CHECK(c.r[14] == 0x80001002u, "LR_abt=%08x expect 80001002", c.r[14]);
    CHECK(c.r[0] == 0x5a5au, "nothing executed: r0=%08x", c.r[0]);
}

static void test_wide_instruction_across_a_fetch_block(void) {
    /* Host RAM on: the first halfword comes through the 1 KB fetch block and
     * the second is the first halfword of the next block. Run it twice so the
     * second pass starts with the block cached. */
    arm_cpu_t c;
    boot(&c, &g_bus_fast, ARM_ARCH_V7_A8, 0x3fe, true);
    put16(0x3fe, 0xf241); put16(0x400, 0x2034);
    put16(0x402, 0xe7fc);                   /* B .-4 -> 0x3fe */
    arm_step(&c);
    CHECK(c.r[0] == 0x1234u && c.r[15] == 0x402u, "first pass r0=%08x pc=%08x",
          c.r[0], c.r[15]);
    c.r[0] = 0u;
    arm_step(&c);
    CHECK(c.r[15] == 0x3feu, "branch back: pc=%08x", c.r[15]);
    arm_step(&c);
    CHECK(c.r[0] == 0x1234u && c.r[15] == 0x402u, "second pass r0=%08x pc=%08x",
          c.r[0], c.r[15]);
}

static void test_msr_mrs_and_the_execution_state_bits(void) {
    /* MSR CPSR_fsxc, r0 (0xf380 0x8f00) cannot clear T or set IT or J; the
     * SPSR, a saved copy, takes them. */
    arm_cpu_t c;
    boot(&c, &g_bus, ARM_ARCH_V7_A8, 0x100, true);
    put16(0x100, 0xf380); put16(0x102, 0x8f00);
    c.r[0] = 0x0700fc00u | ARM_MODE_SYS | ARM_CPSR_I;       /* IT, J set; T clear */
    CHECK(arm_step(&c) == ARM_OK, "MSR executes");
    CHECK((c.cpsr & ARM_CPSR_T) && itstate(c.cpsr) == 0u && !(c.cpsr & ARM_CPSR_J),
          "CPSR keeps T and gains no IT/J: cpsr=%08x", c.cpsr);
    CHECK((c.cpsr & ARM_CPSR_I) && !(c.cpsr & ARM_CPSR_F),
          "the control bits are written: cpsr=%08x", c.cpsr);

    boot(&c, &g_bus, ARM_ARCH_V7_A8, 0x100, true);
    c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_SVC;
    put16(0x100, 0xf390); put16(0x102, 0x8f00);             /* MSR SPSR_fsxc, r0 */
    c.r[0] = 0x0600fc20u | ARM_MODE_USR;
    CHECK(arm_step(&c) == ARM_OK &&
          c.spsr[arm_bank_of_mode(ARM_MODE_SVC)] == (0x0600fc20u | ARM_MODE_USR),
          "SPSR takes T and IT: %08x", c.spsr[arm_bank_of_mode(ARM_MODE_SVC)]);

    /* MRS inside an IT block: ITSTATE is live in the CPSR but reads as 0,
     * and so does T. ITT EQ; MRSEQ r1, APSR; MOVEQ r0, #1. */
    boot(&c, &g_bus, ARM_ARCH_V7_A8, 0x100, true);
    put16(0x100, 0xbf04); put16(0x102, 0xf3ef); put16(0x104, 0x8100);
    put16(0x106, 0x2001);
    c.cpsr |= ARM_CPSR_Z;
    arm_step(&c);
    arm_step(&c);
    CHECK(itstate(c.cpsr) != 0u, "the MOVEQ is still pending: cpsr=%08x", c.cpsr);
    CHECK(c.r[1] == (c.cpsr & ~(ARM_CPSR_IT_MASK | ARM_CPSR_J | ARM_CPSR_T)),
          "MRS read %08x of cpsr %08x", c.r[1], c.cpsr);
}

static void test_sctlr_te_selects_thumb_handlers(void) {
    /* SVC #0 in ARM state with SCTLR.TE set: the A8 enters its handler in
     * Thumb; the ARM1176 has no TE and stays in ARM. */
    static const arm_arch_t ARCH[] = { ARM_ARCH_V6_ARM1176, ARM_ARCH_V7_A8 };
    for (unsigned i = 0; i < 2; i++) {
        arm_cpu_t c;
        boot(&c, &g_bus, ARCH[i], 0x100, false);
        m_w32(NULL, 0x100, 0xef000000u);
        c.cp15.sctlr |= ARM_SCTLR_TE;
        arm_step(&c);
        const bool thumb = (c.cpsr & ARM_CPSR_T) != 0u;
        CHECK(c.r[15] == ARM_VEC_SWI && thumb == arm_arch_is_v7(ARCH[i]),
              "arch %u: pc=%08x T=%d", (unsigned)ARCH[i], c.r[15], (int)thumb);
    }
}

static void test_arm_state_armv7_additions(void) {
    static const struct {
        uint32_t insn, r0, r1, r2, r3, expect;
        const char *what;
    } CASES[] = {
        { 0xe0603291u, 0, 0x10u, 3u, 100u, 100u - 0x30u, "MLS r0, r1, r2, r3" },
        { 0xe7e70251u, 0, 0x12345a78u, 0, 0, 0xa7u,        "UBFX r0, r1, #4, #8" },
        { 0xe7a70251u, 0, 0x12345a78u, 0, 0, 0xffffffa7u,  "SBFX r0, r1, #4, #8" },
        { 0xe7cb0411u, 0xffffffffu, 0x12345a78u, 0, 0, 0xfffff8ffu,
                                                       "BFI r0, r1, #8, #4" },
        { 0xe7cb041fu, 0xffffffffu, 0, 0, 0, 0xfffff0ffu,  "BFC r0, #8, #4" },
        { 0xe6ff0f31u, 0, 0x00000001u, 0, 0, 0x80000000u,  "RBIT r0, r1" },
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        arm_cpu_t c;
        boot(&c, &g_bus, ARM_ARCH_V6_ARM1176, 0, false);
        m_w32(NULL, 0, CASES[i].insn);
        CHECK(arm_step(&c) == ARM_UNDEFINED, "%s must be UNDEFINED on the ARM1176",
              CASES[i].what);
        boot(&c, &g_bus, ARM_ARCH_V7_A8, 0, false);
        m_w32(NULL, 0, CASES[i].insn);
        c.r[0] = CASES[i].r0; c.r[1] = CASES[i].r1;
        c.r[2] = CASES[i].r2; c.r[3] = CASES[i].r3;
        CHECK(arm_step(&c) == ARM_OK && c.r[0] == CASES[i].expect,
              "%s on the A8: r0=%08x expect %08x", CASES[i].what, c.r[0],
              CASES[i].expect);
    }

    /* UMAAL is ARMv6: both cores. The largest operands cannot overflow. */
    static const arm_arch_t BOTH[] = { ARM_ARCH_V6_ARM1176, ARM_ARCH_V7_A8 };
    for (unsigned i = 0; i < 2; i++) {
        arm_cpu_t c;
        boot(&c, &g_bus, BOTH[i], 0, false);
        m_w32(NULL, 0, 0xe0410392u);                  /* UMAAL r0, r1, r2, r3 */
        c.r[0] = c.r[1] = c.r[2] = c.r[3] = 0xffffffffu;
        CHECK(arm_step(&c) == ARM_OK && c.r[0] == 0xffffffffu && c.r[1] == 0xffffffffu,
              "arch %u UMAAL: %08x:%08x", (unsigned)BOTH[i], c.r[1], c.r[0]);
        boot(&c, &g_bus, BOTH[i], 0, false);
        m_w32(NULL, 0, 0xe0410392u);
        c.r[0] = 5u; c.r[1] = 7u; c.r[2] = 0x10000u; c.r[3] = 0x10000u;
        CHECK(arm_step(&c) == ARM_OK && c.r[0] == 12u && c.r[1] == 1u,
              "arch %u UMAAL: %08x:%08x expect 1:c", (unsigned)BOTH[i], c.r[1], c.r[0]);
    }
}

static void test_alu_write_to_pc_interworks_on_armv7(void) {
    /* MOV pc, r0 with r0 odd: ARMv7 ALUWritePC switches to Thumb like BX;
     * the ARM1176 does not interwork here. */
    static const arm_arch_t BOTH[] = { ARM_ARCH_V6_ARM1176, ARM_ARCH_V7_A8 };
    for (unsigned i = 0; i < 2; i++) {
        arm_cpu_t c;
        boot(&c, &g_bus, BOTH[i], 0, false);
        m_w32(NULL, 0, 0xe1a0f000u);
        c.r[0] = 0x101u;
        arm_step(&c);
        const bool thumb = (c.cpsr & ARM_CPSR_T) != 0u;
        CHECK(c.r[15] == 0x100u && thumb == arm_arch_is_v7(BOTH[i]),
              "arch %u: pc=%08x T=%d", (unsigned)BOTH[i], c.r[15], (int)thumb);
    }
}

static void test_wfi_hint_waits_when_privileged(void) {
    arm_cpu_t c;
    struct { arm_arch_t arch; bool thumb, wide, user; unsigned calls; const char *what; }
    CASES[] = {
        { ARM_ARCH_V7_A8,      false, false, false, 1u, "ARM WFI hint, A8"      },
        { ARM_ARCH_V7_A8,      true,  false, false, 1u, "Thumb WFI, A8"         },
        { ARM_ARCH_V7_A8,      true,  true,  false, 1u, "Thumb WFI.W, A8"       },
        { ARM_ARCH_V7_A8,      true,  false, true,  0u, "Thumb WFI in User"     },
        { ARM_ARCH_V6_ARM1176, false, false, false, 0u, "ARM1176: MSR nothing"  },
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        boot(&c, &g_bus, CASES[i].arch, 0x100, CASES[i].thumb);
        if (!CASES[i].thumb) m_w32(NULL, 0x100, 0xe320f003u);
        else if (!CASES[i].wide) put16(0x100, 0xbf30);
        else { put16(0x100, 0xf3af); put16(0x102, 0x8003); }
        if (CASES[i].user) c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_USR;
        g_wfi_calls = 0;
        arm_status_t st = arm_step(&c);
        CHECK(st == ARM_OK && g_wfi_calls == CASES[i].calls,
              "%s: status %d, %u waits", CASES[i].what, (int)st, g_wfi_calls);
    }
}

/* One arm_ci_run over a fresh engine; returns the number retired. */
static unsigned ci_run_once(arm_cpu_t *c, unsigned budget, arm_ci_stop_t *stop,
                            arm_ci_stats_t *s) {
    arm_ci_config_t cfg = { .ram = g_ram, .ram_base = 0, .ram_size = RAM_SIZE };
    arm_ci_t *ci = arm_ci_create(&cfg);
    CHECK(ci != NULL, "arm_ci_create");
    if (!ci) return 0u;
    arm_status_t st = ARM_OK;
    unsigned ran = arm_ci_run(ci, c, budget, &st, stop);
    CHECK(st == ARM_OK, "engine status %d", (int)st);
    arm_ci_get_stats(ci, s);
    arm_ci_destroy(ci);
    return ran;
}

static void test_cached_interpreter_runs_armv7(void) {
    arm_ci_stop_t stop;
    arm_ci_stats_t s;
    arm_cpu_t c;

    /* A Thumb-2 block mixing 16- and 32-bit records: MOVW r0, #0x1234;
     * ADD.W r1, r0, #1; MOVT r0, #0xbeef; LDR.W r3, [r4, #8]; MOVS r5, #9.
     * Every PC after the first depends on the halfword table. */
    boot(&c, &g_bus_fast, ARM_ARCH_V7_A8, 0x100, true);
    put16(0x100, 0xf241); put16(0x102, 0x2034);
    put16(0x104, 0xf100); put16(0x106, 0x0101);
    put16(0x108, 0xf6cb); put16(0x10a, 0x60ef);
    put16(0x10c, 0xf8d4); put16(0x10e, 0x3008);
    put16(0x110, 0x2509);
    m_w32(NULL, 0x2008, 0x5a5a1234u);
    c.r[4] = 0x2000u;
    unsigned ran = ci_run_once(&c, 5u, &stop, &s);
    CHECK(ran == 5u && c.r[15] == 0x112u, "ran %u pc=%08x", ran, c.r[15]);
    CHECK(c.r[0] == 0xbeef1234u && c.r[1] == 0x1235u && c.r[3] == 0x5a5a1234u &&
          c.r[5] == 9u, "r0=%08x r1=%08x r3=%08x r5=%u", c.r[0], c.r[1], c.r[3], c.r[5]);
    CHECK(s.retired == 5u && s.ref_retired == 0u,
          "all five specialised: retired %llu via reference %llu",
          (unsigned long long)s.retired, (unsigned long long)s.ref_retired);
    CHECK(c.cycles == 5u, "cycles %llu", (unsigned long long)c.cycles);

    /* An IT and what it covers run through the reference, then the block
     * goes on: IT EQ; MOVEQ r2, #7; MOVS r5, #9. */
    boot(&c, &g_bus_fast, ARM_ARCH_V7_A8, 0x100, true);
    put16(0x100, 0xbf08); put16(0x102, 0x2207); put16(0x104, 0x2509);
    c.cpsr |= ARM_CPSR_Z;
    ran = ci_run_once(&c, 3u, &stop, &s);
    CHECK(ran == 3u && c.r[2] == 7u && c.r[5] == 9u && c.r[15] == 0x106u &&
          (c.cpsr & ARM_CPSR_IT_MASK) == 0u,
          "IT block: ran %u r2=%u r5=%u pc=%08x cpsr=%08x", ran, c.r[2], c.r[5],
          c.r[15], c.cpsr);
    CHECK(s.ref_retired == 2u, "IT and MOVEQ via reference: %llu",
          (unsigned long long)s.ref_retired);

    /* Entered with an IT block in progress: arm_step's, not the engine's. */
    boot(&c, &g_bus_fast, ARM_ARCH_V7_A8, 0x102, true);
    put16(0x102, 0x2207);
    c.cpsr |= 0x08u << 8;                 /* ITSTATE 08: EQ, last instruction */
    ran = ci_run_once(&c, 1u, &stop, &s);
    CHECK(ran == 0u && stop == ARM_CI_STOP_STEP && s.step_cause[ARM_CI_STEP_IT] == 1u,
          "inside IT: ran %u stop %d it-steps %llu", ran, (int)stop,
          (unsigned long long)s.step_cause[ARM_CI_STEP_IT]);

    /* ARM state: MOV pc, r0 with r0 odd interworks on the A8 (and the engine
     * agrees with the reference); the WFI hint is left to arm_step. */
    boot(&c, &g_bus_fast, ARM_ARCH_V7_A8, 0, false);
    m_w32(NULL, 0, 0xe1a0f000u);
    c.r[0] = 0x201u;
    ran = ci_run_once(&c, 1u, &stop, &s);
    CHECK(ran == 1u && c.r[15] == 0x200u && (c.cpsr & ARM_CPSR_T),
          "A8 MOV pc: ran %u pc=%08x cpsr=%08x", ran, c.r[15], c.cpsr);
    boot(&c, &g_bus_fast, ARM_ARCH_V6_ARM1176, 0, false);
    m_w32(NULL, 0, 0xe1a0f000u);
    c.r[0] = 0x201u;
    ran = ci_run_once(&c, 1u, &stop, &s);
    CHECK(ran == 1u && c.r[15] == 0x200u && !(c.cpsr & ARM_CPSR_T),
          "ARM1176 MOV pc: ran %u pc=%08x cpsr=%08x", ran, c.r[15], c.cpsr);
    boot(&c, &g_bus_fast, ARM_ARCH_V7_A8, 0, false);
    m_w32(NULL, 0, 0xe320f003u);
    ran = ci_run_once(&c, 1u, &stop, &s);
    CHECK(ran == 0u && s.step_cause[ARM_CI_STEP_WFI] == 1u,
          "A8 WFI hint: ran %u wfi-steps %llu", ran,
          (unsigned long long)s.step_cause[ARM_CI_STEP_WFI]);
}

/*
 * An exception return that lands, in the same mode, on the very next
 * instruction with an IT block in progress: SUBS PC, LR, #0 with LR = the
 * next instruction and SPSR_svc = SVC | T | ITSTATE (EQ, one instruction).
 * The next instruction is a 16-bit ADDS, which outside an IT block the
 * engine runs as a specialised record that always executes and sets flags.
 * Inside this IT block, with Z clear, it must be skipped. The engine only
 * gets that right by leaving the block when ITSTATE turns live under a record
 * the decoder did not know an IT covers; this checks it against the
 * reference, step for step.
 */
static void test_engine_leaves_a_block_when_itstate_turns_live(void) {
    arm_cpu_t ref, eng;
    for (int k = 0; k < 2; k++) {
        arm_cpu_t *c = k ? &eng : &ref;
        boot(c, &g_bus_fast, ARM_ARCH_V7_A8, 0x100, true);
        put16(0x100, 0xf3de); put16(0x102, 0x8f00);     /* SUBS PC, LR, #0 */
        put16(0x104, 0x1840);                           /* ADDS r0, r0, r1 */
        put16(0x106, 0x2509);                           /* MOVS r5, #9     */
        c->cpsr = (c->cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_SVC;
        c->r[14] = 0x104u;
        c->r[0] = 5u; c->r[1] = 7u;
        c->spsr[arm_bank_of_mode(ARM_MODE_SVC)] =
            ARM_MODE_SVC | ARM_CPSR_T | (c->cpsr & (ARM_CPSR_I | ARM_CPSR_F | ARM_CPSR_A)) |
            ARM_CPSR_N | (0x08u << 8);                  /* ITSTATE: EQ, last */
    }
    for (int i = 0; i < 3; i++) arm_step(&ref);

    arm_ci_config_t cfg = { .ram = g_ram, .ram_base = 0, .ram_size = RAM_SIZE };
    arm_ci_t *ci = arm_ci_create(&cfg);
    CHECK(ci != NULL, "arm_ci_create");
    if (!ci) return;
    unsigned done = 0;
    for (int guard = 0; done < 3u && guard < 8; guard++) {
        arm_status_t st = ARM_OK;
        arm_ci_stop_t stop;
        unsigned ran = arm_ci_run(ci, &eng, 3u - done, &st, &stop);
        done += ran;
        if (done < 3u && stop == ARM_CI_STOP_STEP) { arm_step(&eng); done++; }
    }
    arm_ci_destroy(ci);
    CHECK(ref.r[0] == 5u && ref.r[5] == 9u && ref.r[15] == 0x108u,
          "reference: ADDS skipped, r0=%u r5=%u pc=%08x", ref.r[0], ref.r[5], ref.r[15]);
    CHECK(eng.r[0] == ref.r[0] && eng.r[5] == ref.r[5] && eng.r[15] == ref.r[15] &&
          eng.cpsr == ref.cpsr && eng.cycles == ref.cycles,
          "engine r0=%u r5=%u pc=%08x cpsr=%08x cycles=%llu; reference cpsr=%08x cycles=%llu",
          eng.r[0], eng.r[5], eng.r[15], eng.cpsr, (unsigned long long)eng.cycles,
          ref.cpsr, (unsigned long long)ref.cycles);
}

/* Run one ARM-state word at 0x100 on a fresh core of the given profile. */
static arm_status_t arm_one(arm_cpu_t *c, arm_arch_t arch, uint32_t insn,
                            bool keep) {
    if (!keep) {
        boot(c, &g_bus, arch, 0x100, false);
        enable_vfp(c);
    }
    c->r[15] = 0x100;
    m_w32(NULL, 0x100, insn);
    return arm_step(c);
}

static uint64_t dreg(const arm_cpu_t *c, unsigned n) {
    return (uint64_t)c->vfp_s[2 * n] | ((uint64_t)c->vfp_s[2 * n + 1] << 32);
}
static void set_dreg(arm_cpu_t *c, unsigned n, uint64_t v) {
    c->vfp_s[2 * n] = (uint32_t)v;
    c->vfp_s[2 * n + 1] = (uint32_t)(v >> 32);
}

/*
 * VFPv3 on the Cortex-A8 profile: d16-d31, VMOV (immediate), VCVT between
 * floating and fixed point, the ID registers, and the Advanced SIMD scalar
 * transfers. tools/unicorn_neon_diff.py compares all of these against
 * Unicorn's Cortex-A8 at random; these pin one known answer each, and that
 * the ARM1176 still refuses every one. Encodings from llvm-mc -mattr=+neon.
 */
static void test_vfpv3_on_the_cortex_a8(void) {
    arm_cpu_t c;

    /* vldr d17, [r0] (0xedd01b00): d16-d31 exist. */
    boot(&c, &g_bus, ARM_ARCH_V7_A8, 0x100, false);
    enable_vfp(&c);
    c.r[0] = 0x2000;
    m_w32(NULL, 0x2000, 0x44332211u); m_w32(NULL, 0x2004, 0x88776655u);
    CHECK(arm_one(&c, ARM_ARCH_V7_A8, 0xedd01b00u, true) == ARM_OK &&
          dreg(&c, 17) == 0x8877665544332211ull,
          "vldr d17: %016llx", (unsigned long long)dreg(&c, 17));
    CHECK(arm_one(&c, ARM_ARCH_V6_ARM1176, 0xedd01b00u, false) == ARM_UNDEFINED,
          "vldr d17 must stay refused on the ARM1176");

    /* vmov.f64 d20, #1.0 (0xeef74b00) and vmov.f32 s1, #-2.0 (0xeef80a00). */
    CHECK(arm_one(&c, ARM_ARCH_V7_A8, 0xeef74b00u, false) == ARM_OK &&
          dreg(&c, 20) == 0x3ff0000000000000ull,
          "vmov.f64 d20, #1.0: %016llx", (unsigned long long)dreg(&c, 20));
    CHECK(arm_one(&c, ARM_ARCH_V7_A8, 0xeef80a00u, false) == ARM_OK &&
          c.vfp_s[1] == 0xc0000000u, "vmov.f32 s1, #-2.0: %08x", c.vfp_s[1]);
    CHECK(arm_one(&c, ARM_ARCH_V6_ARM1176, 0xeef80a00u, false) == ARM_UNDEFINED,
          "VMOV (immediate) must stay refused on the ARM1176");

    /* vcvt.s32.f32 s2, s2, #16 (0xeebe1ac8): 1.5 is 0x18000 in 16.16. */
    boot(&c, &g_bus, ARM_ARCH_V7_A8, 0x100, false);
    enable_vfp(&c);
    c.vfp_s[2] = 0x3fc00000u;
    CHECK(arm_one(&c, ARM_ARCH_V7_A8, 0xeebe1ac8u, true) == ARM_OK &&
          c.vfp_s[2] == 0x00018000u, "vcvt.s32.f32 #16: %08x", c.vfp_s[2]);
    /* vcvt.f32.u16 s3, s3, #8 (0xeefb1a44): only the low 16 bits count,
     * 0x0180 / 256 = 1.5. */
    c.vfp_s[3] = 0xffff0180u;
    CHECK(arm_one(&c, ARM_ARCH_V7_A8, 0xeefb1a44u, true) == ARM_OK &&
          c.vfp_s[3] == 0x3fc00000u, "vcvt.f32.u16 #8: %08x", c.vfp_s[3]);

    /* The ID registers, privileged, and readable with VFP disabled. */
    boot(&c, &g_bus, ARM_ARCH_V7_A8, 0x100, false);
    c.cp15.cpacr |= 0xfu << ARM_CPACR_CP10_SHIFT;           /* FPEXC.EN clear */
    CHECK(arm_one(&c, ARM_ARCH_V7_A8, 0xeef70a10u, true) == ARM_OK &&
          c.r[0] == CORTEX_A8_MVFR0, "vmrs r0, mvfr0: %08x", c.r[0]);
    CHECK(arm_one(&c, ARM_ARCH_V7_A8, 0xeef61a10u, true) == ARM_OK &&
          c.r[1] == CORTEX_A8_MVFR1, "vmrs r1, mvfr1: %08x", c.r[1]);
    CHECK(arm_one(&c, ARM_ARCH_V7_A8, 0xeef02a10u, true) == ARM_OK &&
          c.r[2] == CORTEX_A8_FPSID, "vmrs r2, fpsid: %08x", c.r[2]);
    c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_USR;
    enable_vfp(&c);
    CHECK(arm_one(&c, ARM_ARCH_V7_A8, 0xeef70a10u, true) == ARM_OK &&
          (c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_UND,
          "vmrs mvfr0 from User mode is the guest's Undefined exception");

    /* FPEXC keeps EN alone; FPSCR keeps the A8's bits. */
    boot(&c, &g_bus, ARM_ARCH_V7_A8, 0x100, false);
    enable_vfp(&c);
    c.r[3] = 0xffffffffu;
    arm_one(&c, ARM_ARCH_V7_A8, 0xeee83a10u, true);         /* vmsr fpexc, r3 */
    CHECK(c.vfp_fpexc == ARM_FPEXC_EN, "FPEXC after writing all ones: %08x", c.vfp_fpexc);
    arm_one(&c, ARM_ARCH_V7_A8, 0xeee13a10u, true);         /* vmsr fpscr, r3 */
    CHECK(c.vfp_fpscr == 0xfff7009fu, "FPSCR after writing all ones: %08x", c.vfp_fpscr);

    /* Scalar transfers and VDUP. */
    boot(&c, &g_bus, ARM_ARCH_V7_A8, 0x100, false);
    enable_vfp(&c);
    set_dreg(&c, 17, 0x8877665544332211ull);
    CHECK(arm_one(&c, ARM_ARCH_V7_A8, 0xeef10bb0u, true) == ARM_OK &&
          c.r[0] == 0x66u, "vmov.u8 r0, d17[5]: %08x", c.r[0]);
    CHECK(arm_one(&c, ARM_ARCH_V7_A8, 0xee711bf0u, true) == ARM_OK &&
          c.r[1] == 0xffffff88u, "vmov.s8 r1, d17[7]: %08x", c.r[1]);
    c.r[3] = 0xdeadbeefu;
    CHECK(arm_one(&c, ARM_ARCH_V7_A8, 0xee213bb0u, true) == ARM_OK &&
          dreg(&c, 17) == 0x8877beef44332211ull,
          "vmov.16 d17[2], r3: %016llx", (unsigned long long)dreg(&c, 17));
    c.r[4] = 0x01020304u;
    CHECK(arm_one(&c, ARM_ARCH_V7_A8, 0xee214b90u, true) == ARM_OK &&
          dreg(&c, 17) == 0x0102030444332211ull,
          "vmov.32 d17[1], r4: %016llx", (unsigned long long)dreg(&c, 17));
    c.r[2] = 0x1234abcdu;
    CHECK(arm_one(&c, ARM_ARCH_V7_A8, 0xeea22bb0u, true) == ARM_OK &&
          dreg(&c, 18) == 0xabcdabcdabcdabcdull && dreg(&c, 19) == 0xabcdabcdabcdabcdull,
          "vdup.16 q9, r2: %016llx %016llx",
          (unsigned long long)dreg(&c, 18), (unsigned long long)dreg(&c, 19));
    CHECK(arm_one(&c, ARM_ARCH_V6_ARM1176, 0xeea22bb0u, false) == ARM_UNDEFINED,
          "VDUP must stay refused on the ARM1176");
}

/*
 * Advanced SIMD on the Cortex-A8 profile. tools/unicorn_neon_diff.py compares
 * every group against Unicorn at random; these pin one answer each for the
 * paths that matter most, and the ones the differential cannot show:
 * alignment faults (QEMU 5 does not raise them), the lazy-enable trap, the
 * ARM1176 refusing, and the cached engine agreeing on a VLD with Vd = 15,
 * which has the shape of an ARMv6 PLD and was once decoded as one.
 */
static void test_neon_on_the_cortex_a8(void) {
    arm_cpu_t c;
    arm_ci_stop_t stop;
    arm_ci_stats_t s;

    /* vadd.i32 q0, q1, q2 (0xf2220844). */
    boot(&c, &g_bus, ARM_ARCH_V7_A8, 0x100, false);
    enable_vfp(&c);
    set_dreg(&c, 2, 0x0000000200000001ull); set_dreg(&c, 3, 0xffffffff00000003ull);
    set_dreg(&c, 4, 0x0000001000000010ull); set_dreg(&c, 5, 0x0000000100000010ull);
    CHECK(arm_one(&c, ARM_ARCH_V7_A8, 0xf2220844u, true) == ARM_OK &&
          dreg(&c, 0) == 0x0000001200000011ull && dreg(&c, 1) == 0x0000000000000013ull &&
          c.r[15] == 0x104u,
          "vadd.i32 q0: %016llx %016llx pc=%08x", (unsigned long long)dreg(&c, 0),
          (unsigned long long)dreg(&c, 1), c.r[15]);

    /* vqadd.s8 d0, d1, d2 (0xf2010012): 0x7f + 1 saturates and sets QC. */
    set_dreg(&c, 1, 0x000000000000017full); set_dreg(&c, 2, 0x0000000000000101ull);
    c.vfp_fpscr = 0;
    CHECK(arm_one(&c, ARM_ARCH_V7_A8, 0xf2010012u, true) == ARM_OK &&
          dreg(&c, 0) == 0x000000000000027full && (c.vfp_fpscr & ARM_FPSCR_QC),
          "vqadd.s8: %016llx fpscr %08x", (unsigned long long)dreg(&c, 0), c.vfp_fpscr);

    /* vld1.32 {d16, d17}, [r0]! (0xf4600a8d): writeback by the transfer size. */
    boot(&c, &g_bus, ARM_ARCH_V7_A8, 0x100, false);
    enable_vfp(&c);
    c.r[0] = 0x2000;
    for (unsigned i = 0; i < 4u; i++) m_w32(NULL, 0x2000 + 4u * i, 0x11111111u * (i + 1u));
    CHECK(arm_one(&c, ARM_ARCH_V7_A8, 0xf4600a8du, true) == ARM_OK &&
          dreg(&c, 16) == 0x2222222211111111ull && dreg(&c, 17) == 0x4444444433333333ull &&
          c.r[0] == 0x2010u,
          "vld1.32 {d16,d17}: %016llx %016llx r0=%08x", (unsigned long long)dreg(&c, 16),
          (unsigned long long)dreg(&c, 17), c.r[0]);

    /* vst1.64 {d0, d1}, [r0:128] (0xf4000aef) from an address that is only
     * 8-aligned: an alignment fault, with nothing stored, whatever SCTLR.A. */
    boot(&c, &g_bus, ARM_ARCH_V7_A8, 0x100, false);
    enable_vfp(&c);
    c.r[0] = 0x2008;
    set_dreg(&c, 0, 0x0123456789abcdefull);
    CHECK(arm_one(&c, ARM_ARCH_V7_A8, 0xf4000aefu, true) == ARM_OK &&
          (c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_ABT &&
          (c.cp15.dfsr & 0x40fu) == ARM_FSR_ALIGNMENT && c.cp15.dfar == 0x2008u &&
          m_r32(NULL, 0x2008) == 0u,
          "vst1 [r0:128] misaligned: mode %02x dfsr %08x dfar %08x",
          c.cpsr & ARM_CPSR_MODE_MASK, c.cp15.dfsr, c.cp15.dfar);

    /* The same vadd in Thumb (0xef22 0x0844). */
    boot(&c, &g_bus, ARM_ARCH_V7_A8, 0x100, true);
    enable_vfp(&c);
    put16(0x100, 0xef22); put16(0x102, 0x0844);
    set_dreg(&c, 2, 5u); set_dreg(&c, 4, 7u);
    CHECK(arm_step(&c) == ARM_OK && dreg(&c, 0) == 12u && c.r[15] == 0x104u,
          "Thumb vadd.i32: %016llx pc=%08x", (unsigned long long)dreg(&c, 0), c.r[15]);

    /* With FPEXC.EN clear it is the lazy-enable trap, from ARM state too. */
    boot(&c, &g_bus, ARM_ARCH_V7_A8, 0x100, false);
    c.cp15.cpacr |= 0xfu << ARM_CPACR_CP10_SHIFT;
    CHECK(arm_one(&c, ARM_ARCH_V7_A8, 0xf2220844u, true) == ARM_OK &&
          (c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_UND && c.r[15] == ARM_VEC_UNDEFINED,
          "NEON with VFP off: mode %02x pc %08x", c.cpsr & ARM_CPSR_MODE_MASK, c.r[15]);

    /* And the ARM1176 has no NEON at all: the machine stops. */
    CHECK(arm_one(&c, ARM_ARCH_V6_ARM1176, 0xf2220844u, false) == ARM_UNDEFINED,
          "NEON must stay refused on the ARM1176");

    /* vld1.32 {d15}, [r0] (0xf420f78f) through the engine: bits 27:26 = 01
     * and Vd = 15 is the ARMv6 PLD shape. Both engines must load d15. */
    boot(&c, &g_bus_fast, ARM_ARCH_V7_A8, 0x100, false);
    enable_vfp(&c);
    m_w32(NULL, 0x100, 0xf420f78fu);
    m_w32(NULL, 0x104, 0xe3a05009u);                          /* mov r5, #9 */
    m_w32(NULL, 0x2000, 0xdeadbeefu); m_w32(NULL, 0x2004, 0x01234567u);
    c.r[0] = 0x2000;
    unsigned ran = ci_run_once(&c, 2u, &stop, &s);
    CHECK(ran == 2u && dreg(&c, 15) == 0x01234567deadbeefull && c.r[5] == 9u &&
          s.ref_retired == 1u,
          "engine vld1 {d15}: ran %u d15=%016llx r5=%u via reference %llu", ran,
          (unsigned long long)dreg(&c, 15), c.r[5], (unsigned long long)s.ref_retired);
}

int main(void) {
    printf("S5LBox ARMv7 profile tests\n");
    test_profile_predicates();
    test_divide_follows_the_core();
    test_bl_is_one_instruction_on_armv7();
    test_sctlr_u_and_xp_read_as_one();
    test_it_block_conditions_and_flags();
    test_svc_in_it_block_stacks_the_next_state();
    test_abort_in_it_block_stacks_its_own_state();
    test_lazy_vfp_trap_from_thumb();
    test_vldr_literal_uses_the_thumb_pc();
    test_wide_instruction_across_a_page();
    test_wide_instruction_across_a_fetch_block();
    test_msr_mrs_and_the_execution_state_bits();
    test_sctlr_te_selects_thumb_handlers();
    test_arm_state_armv7_additions();
    test_alu_write_to_pc_interworks_on_armv7();
    test_wfi_hint_waits_when_privileged();
    test_cached_interpreter_runs_armv7();
    test_engine_leaves_a_block_when_itstate_turns_live();
    test_vfpv3_on_the_cortex_a8();
    test_neon_on_the_cortex_a8();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
