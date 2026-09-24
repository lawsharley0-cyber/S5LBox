/*
 * S5LBox — VMDriverDump: the kernel references a kext's bytes make.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "VMDriverDump.h"

#include <stdio.h>
#include <string.h>

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define VA 0xc0700000u

static void put16(uint8_t *b, uint32_t off, uint32_t v) {
    b[off] = (uint8_t)v; b[off + 1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *b, uint32_t off, uint32_t v) {
    put16(b, off, v & 0xffffu); put16(b, off + 2, v >> 16);
}
static uint32_t arm_bl(uint32_t off, uint32_t target, uint32_t cond_op) {
    return cond_op | (((target - (VA + off + 8u)) >> 2) & 0x00ffffffu);
}
static void thumb_bl(uint8_t *b, uint32_t off, uint32_t target, int blx) {
    const uint32_t d = target - (VA + off + 4u);
    put16(b, off, 0xf000u | ((d >> 12) & 0x7ffu));
    put16(b, off + 2, (blx ? 0xe800u : 0xf800u) | ((d >> 1) & 0x7ffu));
}

static const vm_driver_ref_t *find(const vm_driver_ref_t *r, size_t n, uint32_t t) {
    for (size_t i = 0; i < n; i++) if (r[i].target == t) return &r[i];
    return NULL;
}

int main(void) {
    uint8_t code[64];
    memset(code, 0, sizeof code);   /* zero words decode as nothing we keep */
    put32(code, 0, arm_bl(0, 0xc0174000u, 0xeb000000u));           /* BL        */
    put32(code, 4, arm_bl(4, VA + 0x30u, 0xeb000000u));            /* internal  */
    /* An ARMv6 Thumb BL pair reaches +-4 MiB, so Thumb kext code reaches the
     * kernel only through a literal; these targets are within range. */
    thumb_bl(code, 10, 0xc0500775u, 0);                            /* Thumb BL  */
    put32(code, 16, 0xc0500775u);                                  /* vtable    */
    put32(code, 20, 0x12345678u);                                  /* not ours  */
    put32(code, 24, VA + 8u);                                      /* internal  */
    thumb_bl(code, 30, 0xc0400002u, 1);                            /* Thumb BLX */
    put32(code, 36, arm_bl(36, 0xc0200004u, 0xfb000000u));         /* BLX H=1   */
    put32(code, 40, arm_bl(40, 0xc0180000u, 0x1a000000u));         /* BNE tail  */

    vm_driver_ref_t r[16];
    size_t total = 0;
    size_t n = vm_driver_collect_refs(code, VA, sizeof code, 0xc0008000u, 0xc0600000u,
                                      r, 16, &total);
    CHECK(n == total && n == 5u, "5 distinct kernel targets, got %zu (total %zu)", n, total);
    for (size_t i = 1; i < n; i++)
        CHECK(r[i - 1].target < r[i].target, "sorted at %zu", i);
    const vm_driver_ref_t *x = find(r, n, 0xc0174000u);
    CHECK(x && x->kinds == VM_DRIVER_REF_CALL, "ARM BL");
    x = find(r, n, 0xc0500774u);
    CHECK(x && x->kinds == (VM_DRIVER_REF_CALL | VM_DRIVER_REF_WORD) && x->count == 2u,
          "Thumb BL and a vtable word merge: kinds %u count %u",
          x ? x->kinds : 0u, x ? x->count : 0u);
    x = find(r, n, 0xc0400000u);
    CHECK(x && x->kinds == VM_DRIVER_REF_CALL, "Thumb BLX lands word-aligned");
    x = find(r, n, 0xc0200006u);
    CHECK(x && x->kinds == VM_DRIVER_REF_CALL, "ARM BLX with H set lands on +2");
    x = find(r, n, 0xc0180000u);
    CHECK(x && x->kinds == VM_DRIVER_REF_CALL, "a conditional branch out is a tail call");
    CHECK(!find(r, n, VA + 0x30u) && !find(r, n, VA + 8u), "references inside the kext are dropped");

    n = vm_driver_collect_refs(code, VA, sizeof code, 0xc0008000u, 0xc0600000u, r, 2, &total);
    CHECK(n == 2u && total == 5u, "a short output keeps the lowest targets and counts all");
    CHECK(vm_driver_collect_refs(code, VA, 3u, 0xc0008000u, 0xc0600000u, r, 16, &total) == 0u &&
          total == 0u, "too short to hold a word");
    CHECK(vm_driver_collect_refs(NULL, VA, 64u, 0xc0008000u, 0xc0600000u, r, 16, NULL) == 0u,
          "no bytes");

    printf("driver dump: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
