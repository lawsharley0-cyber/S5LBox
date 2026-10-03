/*
 * NEON -- fail-closed iOS 6.1.6 (10B500) kernel compatibility gate. See the
 * header for what each patch does and why.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "ios6_kernel_patch.h"

#include "macho.h"

#include <string.h>

const uint8_t ios6_kernel_patch_expected_uuid[IOS6_KERNEL_PATCH_UUID_LENGTH] = {
    0xaeu, 0x83u, 0xfeu, 0xf4u, 0x9au, 0x2du, 0x3bu, 0x63u,
    0x8fu, 0x64u, 0x67u, 0x68u, 0x09u, 0xccu, 0x2au, 0xf3u
};

static const guest_patch_entry_t kernel_patches[] = {
    {   /* IOFindBSDRoot: bl _ml_static_ptovirt -> nop; nop */
        .virtual_address = IOS6_KERNEL_PATCH_PTOVIRT_VA,
        .length = 4u,
        .expected = {0x17u, 0xf6u, 0x9du, 0xffu},
        .replacement = {0x00u, 0xbfu, 0x00u, 0xbfu}
    },
    {   /* IOFindBSDRoot: movs r1, #0 (mdevadd's phys) -> movs r1, #1 */
        .virtual_address = IOS6_KERNEL_PATCH_PHYS_FLAG_VA,
        .length = 2u,
        .expected = {0x00u, 0x21u},
        .replacement = {0x01u, 0x21u}
    },
    {   /* mdevstrategy read: bl _bcopy_phys -> svc #0xe1; mov r8, r8 */
        .virtual_address = IOS6_KERNEL_PATCH_MD_READ_VA,
        .length = 4u,
        .expected = {0xe7u, 0xf7u, 0xa1u, 0xfdu},
        .replacement = {0xe1u, 0xdfu, 0xc0u, 0x46u}
    },
    {   /* mdevstrategy write: bl _bcopy_phys -> svc #0xe2; mov r8, r8 */
        .virtual_address = IOS6_KERNEL_PATCH_MD_WRITE_VA,
        .length = 4u,
        .expected = {0xe7u, 0xf7u, 0x49u, 0xfdu},
        .replacement = {0xe2u, 0xdfu, 0xc0u, 0x46u}
    },
};

bool ios6_kernel_patch_identify(const uint8_t *kernel, size_t length) {
    macho_t m;
    if (!kernel || macho_parse(kernel, length, &m) != MACHO_OK || !m.has_uuid)
        return false;
    return memcmp(m.uuid, ios6_kernel_patch_expected_uuid,
                  IOS6_KERNEL_PATCH_UUID_LENGTH) == 0;
}

guest_patch_status_t ios6_kernel_patch_apply(uint8_t *ram, size_t ram_size,
                                             guest_patch_report_t *report) {
    guest_patch_manifest_t manifest;
    memset(&manifest, 0, sizeof manifest);
    manifest.ram = ram;
    manifest.ram_size = ram_size;
    manifest.ram_base = IOS6_KERNEL_PATCH_RAM_BASE;
    manifest.virt_base = IOS6_KERNEL_PATCH_VIRT_BASE;
    manifest.entries = kernel_patches;
    manifest.entry_count = sizeof kernel_patches / sizeof kernel_patches[0];
    return guest_patch_apply(&manifest, report);
}
