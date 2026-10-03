/*
 * NEON — the iPhone OS 3.1.3 boot recipe for the iPhone 3GS machine
 * (tools/n88_ios3.h): what it asks n88_boot for, the devices it un-matches,
 * and that a working root filesystem is never made over an existing one.
 */
#include "n88_ios3.h"

#include <stdio.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(cond, ...) do { \
    if (cond) g_pass++; \
    else { g_fail++; printf("  FAIL %s:%d: ", __func__, __LINE__); \
           printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static void test_request(void) {
    static const uint8_t kernel[16], tree[16];
    vm_block_t root;
    memset(&root, 0, sizeof root);
    n88_boot_t req;
    memset(&req, 0xa5, sizeof req);
    n88_ios3_boot_request(&req, kernel, sizeof kernel, tree, sizeof tree, &root);
    CHECK(req.kernel == kernel && req.kernel_size == sizeof kernel &&
          req.devicetree == tree && req.devicetree_size == sizeof tree, "the firmware");
    CHECK(req.root == &root && req.md_read_site_pc == 0xc007238eu &&
          req.md_write_site_pc == 0xc0072442u, "the memory disk at the patched sites");
    CHECK(req.cmdline && !strcmp(req.cmdline, N88_ROOT_CMDLINE), "the root boot-args");
    CHECK(req.boot_args_version == 4u, "boot_args revision 4");
    CHECK(req.unmatch == n88_ios3_unmatch && req.unmatch_count == N88_IOS3_UNMATCH_COUNT,
          "the un-match list");
}

static void test_unmatch_list(void) {
    static const char *const want[] = {
        "arm-io/iop", "baseband", "arm-io/spi2", "arm-io/sgx", "arm-io/usb-otg",
        "arm-io/isp", "arm-io/amc", "arm-io/tv-out",
    };
    CHECK(N88_IOS3_UNMATCH_COUNT == sizeof want / sizeof want[0], "eight devices");
    for (unsigned i = 0; i < N88_IOS3_UNMATCH_COUNT; i++)
        CHECK(n88_ios3_unmatch[i] && !strcmp(n88_ios3_unmatch[i], want[i]),
              "device %u is %s", i, want[i]);
}

static void test_identify_and_work_image(const char *scratch) {
    static const uint8_t junk[4096];
    CHECK(!n88_ios3_identify(junk, sizeof junk) && !n88_ios3_identify(NULL, 0),
          "only the 7E18 3GS kernel");
    /* A destination that exists is refused, whatever the source. */
    char path[512];
    snprintf(path, sizeof path, "%s/n88_ios3_existing.img", scratch);
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL, "scratch file");
    if (!f) return;
    fputs("guest data", f);
    fclose(f);
    rootfs_work_result_t rr;
    const rootfs_work_status_t rs = n88_ios3_make_work_image(path, path, true, NULL, NULL, &rr);
    CHECK(rs != ROOTFS_WORK_OK, "an existing work image is not replaced (%s)",
          rootfs_work_status_name(rs));
    f = fopen(path, "rb");
    char back[16] = {0};
    if (f) { if (!fgets(back, sizeof back, f)) back[0] = '\0'; fclose(f); }
    CHECK(!strcmp(back, "guest data"), "and is left as it was");
    remove(path);
}

int main(int argc, char **argv) {
    printf("NEON iPhone OS 3.1.3 on the 3GS: the boot recipe\n");
    test_request();
    test_unmatch_list();
    test_identify_and_work_image(argc > 1 ? argv[1] : ".");
    printf("  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
