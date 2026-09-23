/*
 * S5LBox — the two on-device diagnostics: the unmodelled-access log (which
 * register is a driver waiting on?) and the cached interpreter's counters
 * (why does an instruction leave the engine?).
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "soc.h"
#include "arm_ci.h"

#include <stdio.h>
#include <string.h>

#define RAM_BASE 0x08000000u
#define RAM_SIZE (4u << 20)
#define AMC_REG  0x30000400u   /* between the NOR window and arm-io: assigned to nothing */

static s5l8900_t g_m;
static int g_fail;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__);             \
                       printf(__VA_ARGS__); printf("\n"); g_fail++; }          \
    } while (0)

static const s5l_access_entry_t *find(uint32_t pc, uint32_t addr, bool write) {
    for (unsigned i = 0; i < S5L_ACCESS_LOG; i++) {
        const s5l_access_entry_t *e = &g_m.unmodelled[i];
        if (e->seq && e->pc == pc && e->addr == addr && e->write == (uint8_t)write)
            return e;
    }
    return NULL;
}

static void test_unmodelled_log(void) {
    char text[4096];
    arm_bus_t *bus = &g_m.bus;

    g_m.cpu.r[15] = 0xc0717434u;
    (void)bus->read32(bus->ctx, AMC_REG);
    (void)bus->read32(bus->ctx, AMC_REG);
    bus->write32(bus->ctx, AMC_REG, 5u);
    g_m.cpu.r[15] = 0xc0010000u;
    (void)bus->read32(bus->ctx, S5L8900_CLOCK_BASE + 0x10u);   /* clkrstgen stub */

    const s5l_access_entry_t *r = find(0xc0717434u, AMC_REG, false);
    const s5l_access_entry_t *w = find(0xc0717434u, AMC_REG, true);
    const s5l_access_entry_t *st = find(0xc0010000u, S5L8900_CLOCK_BASE + 0x10u, false);
    CHECK(r && r->count == 2u && r->kind == S5L_ACCESS_UNMAPPED && !r->region,
          "unmapped read entry wrong");
    CHECK(w && w->count == 1u && w->value == 5u, "unmapped write entry wrong");
    CHECK(st && st->kind == S5L_ACCESS_STUB && st->region && !strcmp(st->region, "clkrstgen") &&
          st->value == 0xffffffffu, "stub read entry wrong");

    size_t n = s5l_access_log_describe(g_m.unmodelled, S5L_ACCESS_LOG, 8u,
                                       text, sizeof text);
    CHECK(n == strlen(text) && n > 0u, "describe length");
    /* Most recent first: the stub read, then the write, then the reads. */
    const char *first = strstr(text, "clkrstgen"), *wr = strstr(text, "W 30000400");
    const char *rd = strstr(text, "R 30000400");
    CHECK(first && wr && rd && first < wr && wr < rd, "describe order:\n%s", text);
    CHECK(strstr(text, "x2") && strstr(text, "(unmapped)"), "describe fields:\n%s", text);

    /* The all-device table sees the same accesses, and modelled devices too
     * (which the unmodelled table must not). */
    g_m.cpu.r[15] = 0xc0020000u;
    (void)bus->read32(bus->ctx, S5L8900_POWER_BASE + POWER_STATE);
    const s5l_access_entry_t *pw = NULL, *amc = NULL;
    for (unsigned i = 0; i < S5L_ACCESS_LOG; i++) {
        const s5l_access_entry_t *e = &g_m.mmio_recent[i];
        if (e->seq && e->pc == 0xc0020000u) pw = e;
        if (e->seq && e->pc == 0xc0717434u && e->addr == AMC_REG && !e->write) amc = e;
    }
    CHECK(pw && pw->kind == S5L_ACCESS_DEVICE && pw->region && !strcmp(pw->region, "power"),
          "modelled device missing from the all-device table");
    CHECK(amc && amc->count == 2u && amc->kind == S5L_ACCESS_UNMAPPED,
          "unmapped access missing from the all-device table");
    CHECK(find(0xc0020000u, S5L8900_POWER_BASE + POWER_STATE, false) == NULL,
          "modelled device in the unmodelled table");
    n = s5l_access_log_describe(g_m.mmio_recent, S5L_ACCESS_LOG, 4u, text, sizeof text);
    CHECK(strstr(text, "device power") != NULL, "device kind not described:\n%s", text);

    /* The table keeps the most recent distinct accesses: 40 more pcs evict
     * the oldest entries, never the newest. */
    for (uint32_t i = 0; i < 40u; i++) {
        g_m.cpu.r[15] = 0x1000u + i * 4u;
        (void)bus->read32(bus->ctx, AMC_REG);
    }
    CHECK(find(0x1000u + 39u * 4u, AMC_REG, false) != NULL, "newest evicted");
    CHECK(find(0xc0717434u, AMC_REG, false) == NULL, "oldest kept");

    /* Tiny buffers truncate but stay terminated. */
    char tiny[8];
    n = s5l_access_log_describe(g_m.unmodelled, S5L_ACCESS_LOG, 8u, tiny, sizeof tiny);
    CHECK(n == strlen(tiny) && n < sizeof tiny, "truncation");
}

static void test_engine_counters(void) {
    static const uint32_t prog[] = {
        0xe3a00001u,   /* mov   r0, #1                 */
        0xee1d1f70u,   /* mrc   p15, 0, r1, c13, c0, 3 (thread ID: step) */
        0xe328f000u,   /* msr   cpsr_f, #0             (reference, status) */
        0xee102f10u,   /* mrc   p15, 0, r2, c0, c0, 0  (main ID: step) */
        0xeafffffau,   /* b     prog                   */
    };
    for (unsigned i = 0; i < sizeof prog / sizeof prog[0]; i++)
        g_m.bus.write32(g_m.bus.ctx, RAM_BASE + i * 4u, prog[i]);
    g_m.cpu.r[15] = RAM_BASE;
    arm_ci_reset_stats(g_m.ci);

    arm_status_t st = ARM_OK;
    unsigned ran = s5l8900_run(&g_m, 5000u, &st);
    CHECK(st == ARM_OK && ran == 5000u, "run status %d ran %u", (int)st, ran);

    arm_ci_stats_t cs;
    arm_ci_get_stats(g_m.ci, &cs);
    CHECK(cs.retired > 0u, "engine retired nothing");
    CHECK(cs.step_cause[ARM_CI_STEP_CP15_TLS] > 0u, "no CP15 c13 steps");
    CHECK(cs.step_cause[ARM_CI_STEP_CP15] > 0u, "no CP15 steps");
    CHECK(cs.step_cause[ARM_CI_STEP_SVC] == 0u, "phantom SVC steps");
    CHECK(cs.ref_class[ARM_CI_REF_STATUS] > 0u, "MSR not counted as status");
    CHECK(cs.ref_fallback == 0u, "unexpected fallback");

    char text[1024];
    size_t n = arm_ci_describe_stats(&cs, 5000u, text, sizeof text);
    CHECK(n == strlen(text) && strstr(text, "CP15 c13") && strstr(text, "of all"),
          "describe:\n%s", text);
    printf("%s", text);
}

/* A real guest load: the recorded pc must be the load's own address, on
 * the engine path (reference fallback for a non-RAM address) and on
 * arm_step alike. */
static void test_guest_pc(void) {
    const uint32_t code = RAM_BASE + 0x1000u;
    g_m.bus.write32(g_m.bus.ctx, code + 0u, 0xe5930000u);   /* ldr r0, [r3] */
    g_m.bus.write32(g_m.bus.ctx, code + 4u, 0xeafffffdu);   /* b code       */
    for (int backend = 0; backend < 2; backend++) {
        memset(g_m.unmodelled, 0, sizeof g_m.unmodelled);
        if (!s5l8900_set_cpu_backend(&g_m, backend ? S5L8900_CPU_BACKEND_CACHED_BLOCK
                                                   : S5L8900_CPU_BACKEND_INTERPRETER)) {
            CHECK(0, "backend switch");
            return;
        }
        g_m.cpu.r[3] = AMC_REG + 0x10u;
        g_m.cpu.r[15] = code;
        arm_status_t st = ARM_OK;
        (void)s5l8900_run(&g_m, 200u, &st);
        const s5l_access_entry_t *e = find(code, AMC_REG + 0x10u, false);
        CHECK(st == ARM_OK && e && e->count == 100u,
              "backend %d: load pc not recorded (count %u)", backend, e ? e->count : 0u);
    }
}

/* AppleAMC's "lock BSU" handshake writes 1 to +0x400 and reads it back;
 * the AMC block and the SRAM window are honest storage now. */
static void test_amc_storage(void) {
    arm_bus_t *bus = &g_m.bus;
    bus->write32(bus->ctx, S5L8900_AMC_BASE + 0x400u, 1u);
    CHECK(bus->read32(bus->ctx, S5L8900_AMC_BASE + 0x400u) == 1u, "AMC +0x400 did not read back");
    bus->write32(bus->ctx, S5L8900_AMC_BASE + 0x2000u, 3u);
    CHECK(bus->read32(bus->ctx, S5L8900_AMC_BASE + 0x2000u) == 3u, "AMC +0x2000 did not read back");
    bus->write16(bus->ctx, S5L8900_SRAM_BASE + 0x28000u, 0xbeefu);
    CHECK(bus->read16(bus->ctx, S5L8900_SRAM_BASE + 0x28000u) == 0xbeefu, "SRAM did not read back");
    CHECK(g_m.stub_declare_failures == 0u, "a stub failed to declare");
}

int main(void) {
    if (!s5l8900_init(&g_m, RAM_BASE, RAM_SIZE) ||
        !s5l8900_set_cpu_backend(&g_m, S5L8900_CPU_BACKEND_CACHED_BLOCK)) {
        printf("FAIL setup\n");
        return 1;
    }
    test_unmodelled_log();
    test_engine_counters();
    test_guest_pc();
    test_amc_storage();
    s5l8900_free(&g_m);
    printf("ci diagnostics: %d failure(s)\n", g_fail);
    return g_fail ? 1 : 0;
}
