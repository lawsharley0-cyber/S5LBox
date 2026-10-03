/*
 * NEON -- fail-closed iPhone OS 3.1.3 (7E18, iPhone2,1) kernel gate for the
 * iPhone 3GS machine (core/include/n88.h).
 *
 * The 3GS's 7E18 kernel is its own ARMv7 build (linked at 0xC0000000, boot
 * epoch 4), not the iPhone 3G's (tools/ios3_kernel_patch.h). Like the iOS 6
 * gate (tools/ios6_kernel_patch.h) it gets only what the memory-disk bridge
 * needs to serve the root filesystem as /dev/md0, pinned to this one build by
 * its LC_UUID and by the exact bytes at every site (guest_patch.c validates
 * all of them before writing any):
 *
 *   IOFindBSDRoot (0xc019c080) turns the device tree's RAMDisk entry into md0
 *   through a literal-pool call (ldr r4, =mdevadd; blx r4 at 0xc019c6d2):
 *   mdevadd(-1, pa >> 12, size >> 12, phys), the address already physical
 *   (no ml_static_ptovirt in this build) and phys taken from r4, which is 0:
 *     0xc019c6d0  mov r3, r4           ->  movs r3, #1
 *   (the flags it sets are rewritten by the lsrs before the call).
 *   mdevstrategy (static, after mdevadd at 0xc0071e74) then copies each page
 *   with bcopy_phys(src64, dst64, len-on-stack):
 *     0xc007238e  bl bcopy_phys (disk -> buffer, read)   ->  svc #0xe1; mov r8, r8
 *     0xc0072442  bl bcopy_phys (buffer -> disk, write)  ->  svc #0xe2; mov r8, r8
 *   LR is dead after both: each is followed by another bl or by code that
 *   reaches one before the epilogue's pop {..., pc}.
 *
 * Nothing else is changed: no timing, code-signing or security behaviour.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef NEON_IOS3_N88_KERNEL_PATCH_H
#define NEON_IOS3_N88_KERNEL_PATCH_H

#include "guest_patch.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IOS3_N88_KERNEL_PATCH_VIRT_BASE     UINT32_C(0xc0000000)
#define IOS3_N88_KERNEL_PATCH_RAM_BASE      UINT64_C(0x40000000)
#define IOS3_N88_KERNEL_PATCH_PHYS_FLAG_VA  UINT32_C(0xc019c6d0)
#define IOS3_N88_KERNEL_PATCH_MD_READ_VA    UINT32_C(0xc007238e)
#define IOS3_N88_KERNEL_PATCH_MD_WRITE_VA   UINT32_C(0xc0072442)
#define IOS3_N88_KERNEL_PATCH_UUID_LENGTH   16u
/* pe_identify_machine (0xc01a292e) wants boot_args version 4. */
#define IOS3_N88_KERNEL_BOOT_ARGS_VERSION   4u

extern const uint8_t ios3_n88_kernel_patch_expected_uuid[IOS3_N88_KERNEL_PATCH_UUID_LENGTH];

/* True when `kernel` (the decompressed kernelcache Mach-O) is the 7E18
 * iPhone2,1 kernel this table was written against, by its LC_UUID. */
bool ios3_n88_kernel_patch_identify(const uint8_t *kernel, size_t length);

/* Patch the kernel as loaded in guest RAM (`ram` holds `ram_size` bytes at
 * physical IOS3_N88_KERNEL_PATCH_RAM_BASE; the kernel is linked at
 * IOS3_N88_KERNEL_PATCH_VIRT_BASE). All three sites or none. */
guest_patch_status_t ios3_n88_kernel_patch_apply(uint8_t *ram, size_t ram_size,
                                                 guest_patch_report_t *report);

#endif /* NEON_IOS3_N88_KERNEL_PATCH_H */
