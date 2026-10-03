/*
 * NEON -- fail-closed iOS 6.1.6 (10B500, iPhone2,1) kernel compatibility gate.
 *
 * The iPhone 3GS machine (core/include/n88.h) serves the root filesystem as
 * the memory disk /dev/md0 through the memory-disk bridge. The 10B500 kernel
 * needs four small changes for that, all in its own md path, and each is
 * pinned to this one kernel build: the LC_UUID must match, and every site
 * must hold exactly the expected bytes before anything is written
 * (guest_patch.c validates the whole manifest first, then writes, then reads
 * back). A different kernel is refused, not guessed at.
 *
 *   IOFindBSDRoot (0x80270684) turns the device tree's RAMDisk entry into md0
 *   with mdevadd(-1, ml_static_ptovirt(pa) >> 12, size >> 12, phys = 0): a
 *   VIRTUAL disk, which mdevstrategy then reads with plain bcopy. Two patches
 *   make it a PHYSICAL disk at the entry's own address instead:
 *     0x80270822  bl ml_static_ptovirt  ->  nop; nop  (keep the physical base)
 *     0x80270828  movs r1, #0 (phys)    ->  movs r1, #1
 *   mdevstrategy (0x8009765c) then copies one page at a time with
 *   bcopy_phys(src64, dst64, len-on-stack), the call the bridge services:
 *     0x800977d2  bl bcopy_phys (read)  ->  svc #0xe1; mov r8, r8
 *     0x80097882  bl bcopy_phys (write) ->  svc #0xe2; mov r8, r8
 *   LR is dead after both calls (the next thing on each path is another bl
 *   or the epilogue's pop {..., pc}), which the bridge requires because it
 *   preserves every register.
 *
 * The same shape as the iPhone OS 3 gate (ios3_kernel_patch.c), whose kernel
 * passed the RAMDisk address without ptovirt and needed only the phys flag.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef NEON_IOS6_KERNEL_PATCH_H
#define NEON_IOS6_KERNEL_PATCH_H

#include "guest_patch.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IOS6_KERNEL_PATCH_VIRT_BASE     UINT32_C(0x80000000)
#define IOS6_KERNEL_PATCH_RAM_BASE      UINT64_C(0x40000000)

#define IOS6_KERNEL_PATCH_PTOVIRT_VA    UINT32_C(0x80270822)
#define IOS6_KERNEL_PATCH_PHYS_FLAG_VA  UINT32_C(0x80270828)
#define IOS6_KERNEL_PATCH_MD_READ_VA    UINT32_C(0x800977d2)
#define IOS6_KERNEL_PATCH_MD_WRITE_VA   UINT32_C(0x80097882)

#define IOS6_KERNEL_PATCH_UUID_LENGTH   16u
extern const uint8_t ios6_kernel_patch_expected_uuid[IOS6_KERNEL_PATCH_UUID_LENGTH];

/* True when `kernel` (the decompressed kernelcache Mach-O) is the 10B500
 * iPhone2,1 kernel this table was written against, by its LC_UUID. */
bool ios6_kernel_patch_identify(const uint8_t *kernel, size_t length);

/* Patch the kernel as loaded in guest RAM (`ram` holds `ram_size` bytes at
 * physical IOS6_KERNEL_PATCH_RAM_BASE; the kernel is linked at
 * IOS6_KERNEL_PATCH_VIRT_BASE). All four sites or none. */
guest_patch_status_t ios6_kernel_patch_apply(uint8_t *ram, size_t ram_size,
                                             guest_patch_report_t *report);

#endif /* NEON_IOS6_KERNEL_PATCH_H */
