/*
 * NEON -- the iPhone 3GS's iPhone OS 3.1.3 (7E18) kernel gate, on synthetic inputs.
 *
 * No kernel bytes: a synthetic RAM image holds only the three sites' expected
 * instructions, and a synthetic Mach-O carries only a UUID. What is checked:
 * identification by LC_UUID alone, the three replacements landing at the right
 * physical addresses, and the all-or-nothing refusal when one site differs.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "ios3_n88_kernel_patch.h"
#include "macho.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass, g_fail;

#define CHECK(cond, ...) do {                                               \
    if (cond) g_pass++;                                                     \
    else {                                                                  \
        g_fail++;                                                           \
        printf("  FAIL %s:%d: ", __func__, __LINE__);                       \
        printf(__VA_ARGS__);                                                \
        printf("\n");                                                       \
    }                                                                       \
} while (0)

#define RAM_SIZE ((size_t)0x00400000u)          /* 4 MB covers every site */

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint8_t *at(uint8_t *ram, uint32_t va) {
    return ram + (va - IOS3_N88_KERNEL_PATCH_VIRT_BASE);
}

static void seed(uint8_t *ram) {
    static const uint8_t phys[] = {0x23, 0x46};
    static const uint8_t rd[] = {0xef, 0xf7, 0xcd, 0xf9};
    static const uint8_t wr[] = {0xef, 0xf7, 0x73, 0xf9};
    memset(ram, 0x5a, RAM_SIZE);
    memcpy(at(ram, IOS3_N88_KERNEL_PATCH_PHYS_FLAG_VA), phys, sizeof phys);
    memcpy(at(ram, IOS3_N88_KERNEL_PATCH_MD_READ_VA), rd, sizeof rd);
    memcpy(at(ram, IOS3_N88_KERNEL_PATCH_MD_WRITE_VA), wr, sizeof wr);
}

static void test_apply(void) {
    uint8_t *ram = malloc(RAM_SIZE), *before = malloc(RAM_SIZE);
    if (!ram || !before) { CHECK(0, "allocation"); free(ram); free(before); return; }
    seed(ram);
    memcpy(before, ram, RAM_SIZE);
    guest_patch_report_t rep;
    CHECK(ios3_n88_kernel_patch_apply(ram, RAM_SIZE, &rep) == GUEST_PATCH_STATUS_OK,
          "applies (%s)", guest_patch_status_string(rep.status));
    CHECK(memcmp(at(ram, IOS3_N88_KERNEL_PATCH_PHYS_FLAG_VA), "\x01\x23", 2) == 0,
          "movs r3, #1: phys = 1");
    CHECK(memcmp(at(ram, IOS3_N88_KERNEL_PATCH_MD_READ_VA), "\xe1\xdf\xc0\x46", 4) == 0,
          "read site is SVC #0xe1");
    CHECK(memcmp(at(ram, IOS3_N88_KERNEL_PATCH_MD_WRITE_VA), "\xe2\xdf\xc0\x46", 4) == 0,
          "write site is SVC #0xe2");
    size_t changed = 0;
    for (size_t i = 0; i < RAM_SIZE; i++) changed += ram[i] != before[i];
    CHECK(changed <= 10u && changed >= 6u, "only the sites changed (%zu bytes)", changed);

    /* Applying again finds the replacements, not the expected bytes. */
    CHECK(ios3_n88_kernel_patch_apply(ram, RAM_SIZE, &rep) == GUEST_PATCH_STATUS_EXPECTED_MISMATCH,
          "a second application is refused");

    /* One site different: nothing is written anywhere. */
    seed(ram);
    at(ram, IOS3_N88_KERNEL_PATCH_MD_WRITE_VA)[2] ^= 1u;
    memcpy(before, ram, RAM_SIZE);
    CHECK(ios3_n88_kernel_patch_apply(ram, RAM_SIZE, &rep) == GUEST_PATCH_STATUS_EXPECTED_MISMATCH,
          "a differing site is refused");
    CHECK(memcmp(ram, before, RAM_SIZE) == 0, "and no site was written");

    CHECK(ios3_n88_kernel_patch_apply(ram, 0x1000u, &rep) != GUEST_PATCH_STATUS_OK,
          "RAM too small to hold the sites is refused");
    free(ram);
    free(before);
}

/* A Mach-O with one LC_UUID and nothing else. */
static size_t uuid_macho(uint8_t *img, const uint8_t uuid[16]) {
    memset(img, 0, 64);
    put32(img + 0, MH_MAGIC_32); put32(img + 4, MH_CPU_TYPE_ARM);
    put32(img + 8, 9); put32(img + 12, MH_EXECUTE);
    put32(img + 16, 1); put32(img + 20, 24);
    put32(img + 28, LC_UUID); put32(img + 32, 24);
    memcpy(img + 36, uuid, 16);
    return 52;
}

static void test_identify(void) {
    uint8_t img[64];
    size_t n = uuid_macho(img, ios3_n88_kernel_patch_expected_uuid);
    CHECK(ios3_n88_kernel_patch_identify(img, n), "the 3GS 7E18 UUID is recognised");
    uint8_t other[16];
    memcpy(other, ios3_n88_kernel_patch_expected_uuid, 16);
    other[15] ^= 1u;
    n = uuid_macho(img, other);
    CHECK(!ios3_n88_kernel_patch_identify(img, n), "any other UUID is not");
    memset(img, 0, sizeof img);
    CHECK(!ios3_n88_kernel_patch_identify(img, sizeof img), "not a Mach-O");
    CHECK(!ios3_n88_kernel_patch_identify(NULL, 0), "nothing");
}

int main(void) {
    test_apply();
    test_identify();
    printf("ios3_n88_kernel_patch: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
