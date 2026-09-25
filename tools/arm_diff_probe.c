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
 *         (18 hex tokens, no "0x" prefix; insn is the 32-bit word to place
 *         at `pc` before stepping once, in RAM that is otherwise zero.)
 * Output: status r0 r1 r2 r3 r4 r5 r6 r7 r8 r9 r10 r11 r12 r13 r14 r15 cpsr
 *         (18 hex tokens after the status) after exactly one arm_step().
 *
 * Extended form (tools/unicorn_thumb2_diff.py):
 *   ... pc insn arch steps [word ...]
 * `arch` is an arm_arch_t value, set before arm_reset so the reset applies
 * it; `steps` is how many arm_step() calls to make (stopping early at a
 * non-OK status); further words follow `insn` at pc+4, pc+8, ... so a Thumb
 * sequence (an IT block, say) is packed two halfwords to a word. RAM starts
 * as fill_pattern() instead of zero, so loads see data, and the output gains
 * a final token: the FNV-1a hash of the first PATTERN_BYTES of RAM, so
 * stores are compared too.
 *
 * VFP form (tools/unicorn_neon_diff.py): the line starts with "v", and the
 * extended form's tokens are followed, BEFORE any further code words, by
 *   fpscr d0lo d0hi d1lo d1hi ... d31lo d31hi
 * (65 tokens). CPACR grants CP10/CP11 full access and FPEXC.EN is set before
 * the step. The output gains fpscr and the same 64 words after the hash.
 *
 * A malformed input line prints "ERR" and is skipped.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "arm.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

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

/* The extended form's initial RAM, and the span its hash covers. The driver
 * computes the same bytes for its reference, so keep the two in step. */
#define PATTERN_BYTES 0x10000u
static uint8_t pattern_byte(uint32_t i) {
    return (uint8_t)((i * 167u + 13u) ^ (i >> 8));
}

#define MAX_TOKENS 160
#define VFP_TOKENS 65

int main(void) {
    static char line[4096];
    while (fgets(line, sizeof line, stdin)) {
        uint32_t v[MAX_TOKENS];
        int n = 0;
        char *p = line;
        const bool vfp = *p == 'v';
        if (vfp) p++;
        for (; n < MAX_TOKENS; ) {
            char *end;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '\n' || *p == '\0') break;
            unsigned long x = strtoul(p, &end, 16);
            if (end == p) { n = -1; break; }
            v[n++] = (uint32_t)x;
            p = end;
        }
        const bool extended = n >= 20;
        uint32_t vfp_in[VFP_TOKENS];
        if (vfp) {
            if (n < 20 + VFP_TOKENS) { printf("ERR\n"); continue; }
            memcpy(vfp_in, &v[20], sizeof vfp_in);
            memmove(&v[20], &v[20 + VFP_TOKENS],
                    (size_t)(n - 20 - VFP_TOKENS) * sizeof v[0]);
            n -= VFP_TOKENS;
        }
        if (n != 18 && !extended) {
            if (line[0] != '\n' && line[0] != '\0') printf("ERR\n");
            continue;
        }
        if (extended) {
            for (uint32_t i = 0; i < PATTERN_BYTES; i++) g_ram[i] = pattern_byte(i);
            memset(g_ram + PATTERN_BYTES, 0, sizeof g_ram - PATTERN_BYTES);
        } else {
            memset(g_ram, 0, sizeof g_ram);
        }
        arm_cpu_t c = {0};
        if (extended) c.arch = (arm_arch_t)v[18];
        arm_reset(&c, &g_bus);
        /* CPSR (and the mode it selects) must be set BEFORE r13/r14: those
         * two are banked per mode, and arm_reset() defaults to SVC, not
         * whatever mode the caller wants. Writing them first would silently
         * land the values in the SVC bank instead of the requested one. */
        c.cpsr = v[15];
        for (int i = 0; i < 15; i++) c.r[i] = v[i];
        uint32_t pc = v[16];
        c.r[15] = pc;
        if (vfp) {
            c.cp15.cpacr = 0x00f00000u;
            c.vfp_fpexc  = ARM_FPEXC_EN;
            c.vfp_fpscr  = vfp_in[0];
            memcpy(c.vfp_s, &vfp_in[1], sizeof c.vfp_s);
        }
        m_w32(NULL, pc, v[17]);
        unsigned steps = 1u;
        if (extended) {
            steps = v[19];
            for (int i = 20; i < n; i++)
                m_w32(NULL, pc + 4u * (uint32_t)(i - 19), v[i]);
        }
        arm_status_t status = ARM_OK;
        for (unsigned i = 0; i < steps && status == ARM_OK; i++) {
            const uint32_t mode = c.cpsr & 0x1fu;
            status = arm_step(&c);
            /* Stop at an exception entry, so the report shows the vector
             * rather than whatever the rest of the steps found there. */
            if (c.r[15] < 0x20u && (c.cpsr & 0x1fu) != mode) break;
        }
        printf("%d", (int)status);
        for (int i = 0; i < 16; i++) printf(" %08x", c.r[i]);
        printf(" %08x", c.cpsr);
        if (extended) {
            uint32_t h = 2166136261u;
            for (uint32_t i = 0; i < PATTERN_BYTES; i++)
                h = (h ^ g_ram[i]) * 16777619u;
            printf(" %08x", h);
        }
        if (vfp) {
            printf(" %08x", c.vfp_fpscr);
            for (int i = 0; i < 64; i++) printf(" %08x", c.vfp_s[i]);
        }
        printf("\n");
        fflush(stdout);
    }
    return 0;
}
