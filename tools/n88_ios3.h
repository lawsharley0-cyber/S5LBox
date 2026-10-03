/*
 * NEON — iPhone OS 3.1.3 (7E18) on the iPhone 3GS machine (core/include/n88.h):
 * the boot the desktop harness measured (tools/boot3gs.c, and
 * docs/IOS6_READINESS.md from the root filesystem to the home screen), as one
 * recipe that the harness and the app share, so the phone boots exactly what
 * was measured.
 *
 *   1. The kernel is the 7E18 3GS kernelcache, recognised by its LC_UUID
 *      (n88_ios3_identify; ios3_n88_kernel_patch.h).
 *   2. Each machine boots from a working copy of the IPSW's root filesystem,
 *      made once (n88_ios3_make_work_image): /etc/fstab mounts / from
 *      /dev/md0 instead of the NAND's disk0s1, the volume grows by
 *      N88_IOS3_WORK_GROWTH (Apple ships it with no free blocks; /var is a
 *      separate partition on the phone), and, unless the caller says not to,
 *      lockdownd's activation record is written so SpringBoard goes past its
 *      iTunes screen. The copy is the guest's disk from then on.
 *   3. The boot (n88_ios3_boot_request): the root filesystem served as
 *      /dev/md0 through the memory-disk bridge, the boot-args N88_ROOT_CMDLINE,
 *      boot_args revision IOS3_N88_KERNEL_BOOT_ARGS_VERSION, and the devices
 *      n88_ios3_unmatch lists un-matched.
 *   4. After n88_boot, the kernel's three memory-disk sites are patched
 *      (n88_ios3_patch); before running, the caller sets the clock
 *      (n88_set_rtc).
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef NEON_N88_IOS3_H
#define NEON_N88_IOS3_H

#include "guest_patch.h"
#include "n88.h"
#include "rootfs_work.h"
#include "vm_block.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define N88_IOS3_WORK_GROWTH  (UINT64_C(256) << 20)

/*
 * The devices iPhone OS 3.1.3 is better without on this machine: each was the
 * cause of a hang or a panic before it was un-matched (docs/IOS6_READINESS.md,
 * #55). Without them SpringBoard draws its screens.
 */
#define N88_IOS3_UNMATCH_COUNT 8u
extern const char *const n88_ios3_unmatch[N88_IOS3_UNMATCH_COUNT];

/* True for the iPhone OS 3.1.3 (7E18) iPhone 3GS kernelcache. */
bool n88_ios3_identify(const uint8_t *kernel, size_t len);

/*
 * Make `work` from the pristine root filesystem at `pristine` (step 2 above).
 * `work` must not exist: rootfs_work_create() refuses to replace one, which is
 * what keeps a second call from discarding the guest's writes. `progress`
 * (may be NULL) is called as the copy goes. `result` (may be NULL) gets the
 * details either way.
 */
rootfs_work_status_t n88_ios3_make_work_image(const char *pristine, const char *work,
                                              bool activate,
                                              void (*progress)(void *ctx, uint64_t done,
                                                               uint64_t total),
                                              void *progress_ctx,
                                              rootfs_work_result_t *result);

/* The boot request for `kernel` and `tree` with `root` as /dev/md0 (step 3).
 * The strings and the device list it points at are static. */
void n88_ios3_boot_request(n88_boot_t *req, const uint8_t *kernel, size_t kernel_size,
                           const uint8_t *tree, size_t tree_size, const vm_block_t *root);

/* Patch the booted kernel's memory-disk sites (step 4). */
guest_patch_status_t n88_ios3_patch(n88_t *m, guest_patch_report_t *report);

#endif /* NEON_N88_IOS3_H */
