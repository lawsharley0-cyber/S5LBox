/*
 * NEON — iPhone OS 3.1.3 on the iPhone 3GS machine (see n88_ios3.h).
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "n88_ios3.h"

#include "ios3_n88_kernel_patch.h"

#include <string.h>

const char *const n88_ios3_unmatch[N88_IOS3_UNMATCH_COUNT] = {
    "arm-io/iop",               /* the IOP firmware is not modelled           */
    "baseband",                 /* no modem: CommCenter would retry forever   */
    "arm-io/spi2",              /* the modem's SPI bus                        */
    "arm-io/sgx",               /* QuartzCore draws in software instead       */
    "arm-io/usb-otg",           /* findMaxEndpoints panics on its registers   */
    "arm-io/isp",               /* the camera's ISP never answers its mailbox */
    "arm-io/amc",               /* the hardware audio decoder is not modelled */
    "arm-io/tv-out",            /* closing its framebuffer hangs SpringBoard  */
};

bool n88_ios3_identify(const uint8_t *kernel, size_t len) {
    return kernel && ios3_n88_kernel_patch_identify(kernel, len);
}

rootfs_work_status_t n88_ios3_make_work_image(const char *pristine, const char *work,
                                              bool activate,
                                              void (*progress)(void *ctx, uint64_t done,
                                                               uint64_t total),
                                              void *progress_ctx,
                                              rootfs_work_result_t *result) {
    rootfs_work_options_t ro;
    rootfs_work_result_t local;
    rootfs_work_result_t *rr = result ? result : &local;
    rootfs_work_entry_t entries[8];
    memset(&ro, 0, sizeof ro);
    memset(rr, 0, sizeof *rr);
    ro.growth_bytes = N88_IOS3_WORK_GROWTH;
    ro.progress = progress;
    ro.progress_ctx = progress_ctx;
    if (activate) {
        const size_t n = rootfs_work_standard_entries(true, false, entries,
                                                      sizeof entries / sizeof entries[0]);
        if (!n || n > sizeof entries / sizeof entries[0]) return ROOTFS_WORK_INVALID_ARGUMENT;
        ro.entries = entries;
        ro.entry_count = n;
    }
    return rootfs_work_create(pristine, work, &ro, rr);
}

void n88_ios3_boot_request(n88_boot_t *req, const uint8_t *kernel, size_t kernel_size,
                           const uint8_t *tree, size_t tree_size, const vm_block_t *root) {
    memset(req, 0, sizeof *req);
    req->kernel = kernel;
    req->kernel_size = kernel_size;
    req->devicetree = tree;
    req->devicetree_size = tree_size;
    req->cmdline = N88_ROOT_CMDLINE;
    req->unmatch = n88_ios3_unmatch;
    req->unmatch_count = N88_IOS3_UNMATCH_COUNT;
    req->root = root;
    req->md_read_site_pc = IOS3_N88_KERNEL_PATCH_MD_READ_VA;
    req->md_write_site_pc = IOS3_N88_KERNEL_PATCH_MD_WRITE_VA;
    req->boot_args_version = (uint8_t)IOS3_N88_KERNEL_BOOT_ARGS_VERSION;
}

guest_patch_status_t n88_ios3_patch(n88_t *m, guest_patch_report_t *report) {
    return ios3_n88_kernel_patch_apply(m->ram, N88_DRAM_SIZE, report);
}
