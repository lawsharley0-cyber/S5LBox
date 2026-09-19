/*
 * S5LBox — single-instruction ARM interpreter probe, for differential testing
 * against an independent reference engine (Unicorn) from a driver script.
 *
 * Not part of the product or the CI test suite. Reuses the exact flat-RAM
 * arm_bus_t harness core/tests/test_arm.c already uses, so this measures the
 * same arm_step() the real tests do -- no separate code path to drift from it.
 *
 * Protocol: one test case per stdin line, one result per stdout line.
 * Input:  r0 r1 r2 r3 r4 r5 r6 r7 r8 r9 r10 r11 r12 r13 r14 cpsr pc insn
 *         (18 hex tokens, no "0x" prefix; insn is the 32-bit encoding to
 *         place at `pc` before stepping once; ARM mode only in this version.)
 * Output: status r0 r1 r2 r3 r4 r5 r6 r7 r8 r9 r10 r11 r12 r13 r14 r15 cpsr
 *         (19 hex tokens) after exactly one arm_step().
 * A malformed input line prints "ERR" and is skipped.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "arm.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define RAM_SIZE (1u << 20)
static uint8_t g_ram[RAM_SIZE];

static uint32_t m_r32(void *ctx, uint32_t a){ (void)ctx; uint32_t v; memcpy(&v,&g_ram[a&(RAM_SIZE-1)],4); return v; }
static uint16_t m_r16(void *ctx, uint32_t a){ (void)ctx; uint16_t v; memcpy(&v,&g_ram[a&(RAM_SIZE-1)],2); return v; }
static uint8_t  m_r8 (void *ctx, uint32_t a){ (void)ctx; return g_ram[a&(RAM_SIZE-1)]; }
static void m_w32(void *ctx, uint32_t a, uint32_t v){ (void)ctx; memcpy(&g_ram[a&(RAM_SIZE-1)],&v,4); }
static void m_w16(void *ctx, uint32_t a, uint16_t v){ (void)ctx; memcpy(&g_ram[a&(RAM_SIZE-1)],&v,2); }
static void m_w8 (void *ctx, uint32_t a, uint8_t  v){ (void)ctx; g_ram[a&(RAM_SIZE-1)]=v; }

/* host_ram/host_ram_write deliberately left NULL, matching test_arm.c's own
 * g_bus exactly -- unset is a documented-correct answer that keeps every
 * access on the read32/write32 slow path, which is what this probe wants:
 * predictable behavior with no fast-path divergence to account for. */
static const arm_bus_t g_bus = {
    .ctx     = NULL,
    .read32  = m_r32, .read16  = m_r16, .read8  = m_r8,
    .write32 = m_w32, .write16 = m_w16, .write8 = m_w8,
};

int main(void) {
    char line[512];
    while (fgets(line, sizeof line, stdin)) {
        uint32_t v[18];
        int n = sscanf(line,
            "%x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x",
            &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
            &v[8], &v[9], &v[10], &v[11], &v[12], &v[13], &v[14],
            &v[15], &v[16], &v[17]);
        if (n != 18) {
            if (line[0] != '\n' && line[0] != '\0') printf("ERR\n");
            continue;
        }
        memset(g_ram, 0, sizeof g_ram);
        arm_cpu_t c;
        arm_reset(&c, &g_bus);
        /* CPSR (and the mode it selects) must be set BEFORE r13/r14: those
         * two are banked per mode, and arm_reset() defaults to SVC, not
         * whatever mode the caller wants. Writing them first would silently
         * land the values in the SVC bank instead of the requested one. */
        c.cpsr = v[15];
        for (int i = 0; i < 15; i++) c.r[i] = v[i];
        uint32_t pc = v[16];
        uint32_t insn = v[17];
        c.r[15] = pc;
        m_w32(NULL, pc, insn);
        arm_status_t status = arm_step(&c);
        printf("%d", (int)status);
        for (int i = 0; i < 16; i++) printf(" %08x", c.r[i]);
        printf(" %08x\n", c.cpsr);
        fflush(stdout);
    }
    return 0;
}
