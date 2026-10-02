/*
 * NEON -- fail-closed iPhone OS 3.1.3 (7E18, iPhone2,1) kernel gate for the
 * iPhone 3GS machine. See the header for what each patch does and why.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "ios3_n88_kernel_patch.h"

#include "macho.h"

#include <string.h>

const uint8_t ios3_n88_kernel_patch_expected_uuid[IOS3_N88_KERNEL_PATCH_UUID_LENGTH] = {
    0x39u, 0xccu, 0x64u, 0xa9u, 0xc1u, 0x62u, 0x91u, 0x53u,
    0x11u, 0x6bu, 0xafu, 0x84u, 0x59u, 0x42u, 0xa7u, 0xb0u
};

static const guest_patch_entry_t kernel_patches[] = {
    {   /* IOFindBSDRoot: mov r3, r4 (mdevadd's phys, 0) -> movs r3, #1 */
        .virtual_address = IOS3_N88_KERNEL_PATCH_PHYS_FLAG_VA,
        .length = 2u,
        .expected = {0x23u, 0x46u},
        .replacement = {0x01u, 0x23u}
    },
    {   /* mdevstrategy read: bl _bcopy_phys -> svc #0xe1; mov r8, r8 */
        .virtual_address = IOS3_N88_KERNEL_PATCH_MD_READ_VA,
        .length = 4u,
        .expected = {0xefu, 0xf7u, 0xcdu, 0xf9u},
        .replacement = {0xe1u, 0xdfu, 0xc0u, 0x46u}
    },
    {   /* mdevstrategy write: bl _bcopy_phys -> svc #0xe2; mov r8, r8 */
        .virtual_address = IOS3_N88_KERNEL_PATCH_MD_WRITE_VA,
        .length = 4u,
        .expected = {0xefu, 0xf7u, 0x73u, 0xf9u},
        .replacement = {0xe2u, 0xdfu, 0xc0u, 0x46u}
    },
};

bool ios3_n88_kernel_patch_identify(const uint8_t *kernel, size_t length) {
    macho_t m;
    if (!kernel || macho_parse(kernel, length, &m) != MACHO_OK || !m.has_uuid)
        return false;
    return memcmp(m.uuid, ios3_n88_kernel_patch_expected_uuid,
                  IOS3_N88_KERNEL_PATCH_UUID_LENGTH) == 0;
}

guest_patch_status_t ios3_n88_kernel_patch_apply(uint8_t *ram, size_t ram_size,
                                                 guest_patch_report_t *report) {
    guest_patch_manifest_t manifest;
    memset(&manifest, 0, sizeof manifest);
    manifest.ram = ram;
    manifest.ram_size = ram_size;
    manifest.ram_base = IOS3_N88_KERNEL_PATCH_RAM_BASE;
    manifest.virt_base = IOS3_N88_KERNEL_PATCH_VIRT_BASE;
    manifest.entries = kernel_patches;
    manifest.entry_count = sizeof kernel_patches / sizeof kernel_patches[0];
    return guest_patch_apply(&manifest, report);
}
