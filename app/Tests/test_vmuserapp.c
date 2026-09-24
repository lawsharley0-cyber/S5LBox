/* Synthetic IPA fixtures only: no Apple binaries or third-party apps. */
#include "VMUserApp.h"
#include "VMFirmwareFixtures.h"
#include "VMFirmwareTest.h"

#include <stdlib.h>

static const char plist[] =
    "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>"
    "<key>CFBundleIdentifier</key><string>org.example.Tiny</string>"
    "<key>CFBundleExecutable</key><string>Tiny</string>"
    "<key>MinimumOSVersion</key><string>3.1.3</string>"
    "<key>CFBundlePackageType</key><string>APPL</string></dict></plist>";

static bool parse(void *context, const uint8_t *bytes, size_t size,
                   vm_user_app_metadata_t *out, char *detail, size_t capacity) {
    (void)context; (void)detail; (void)capacity;
    vmfw_plist_t p;
    char type[16];
    if (vmfw_plist_init(&p, bytes, size) != VMFW_PLIST_OK ||
        vmfw_plist_get_string(&p, "CFBundleIdentifier", out->identifier, sizeof out->identifier) != VMFW_PLIST_OK ||
        vmfw_plist_get_string(&p, "CFBundleExecutable", out->executable, sizeof out->executable) != VMFW_PLIST_OK ||
        vmfw_plist_get_string(&p, "MinimumOSVersion", out->minimum_os, sizeof out->minimum_os) != VMFW_PLIST_OK ||
        vmfw_plist_get_string(&p, "CFBundlePackageType", type, sizeof type) != VMFW_PLIST_OK) return false;
    out->iphone_application = !strcmp(type, "APPL");
    (void)snprintf(out->display_name, sizeof out->display_name, "Tiny");
    return true;
}

static void make_macho(uint8_t bytes[64]) {
    memset(bytes, 0, 64u);
    fx_w32le(bytes, 0xfeedfaceu);
    fx_w32le(bytes + 4u, 12u);
    fx_w32le(bytes + 8u, 6u);
    fx_w32le(bytes + 12u, 2u);
    fx_w32le(bytes + 16u, 2u);
    fx_w32le(bytes + 20u, 36u);
    fx_w32le(bytes + 28u, 0x25u);
    fx_w32le(bytes + 32u, 16u);
    fx_w32le(bytes + 36u, 0x00030103u);
    fx_w32le(bytes + 44u, 0x21u);
    fx_w32le(bytes + 48u, 20u);
}

static size_t make_ipa(uint8_t *out, size_t capacity, const char *extra,
                        uint8_t executable[64], fx_zip_layout_t *layout) {
    static const uint8_t resource[] = {1u, 2u, 3u};
    fx_zip_member_t entries[] = {
        {"Payload/Tiny.app/Info.plist", (const uint8_t *)plist, sizeof plist - 1u, true, true},
        {"Payload/Tiny.app/Tiny", executable, 64u, false, false},
        {extra ? extra : "Payload/Tiny.app/en.lproj/note.txt", resource, sizeof resource, false, false}
    };
    return fx_zip_build(out, capacity, entries, 3u, layout);
}

static vm_user_app_plan_t *open_ipa(uint8_t *bytes, size_t size, char detail[256]) {
    fx_blob_t blob = {bytes, size, 0u, false};
    return vm_user_app_plan_open(fx_blob_pread, &blob, size, parse, NULL, detail, 256u);
}

int main(void) {
    vmfw_test_t t = {0u, 0u, "version"};
    const char *accepted[] = {"1.0", "2", "3", "3.1", "3.1.3"};
    const char *refused[] = {"", "0", "3.1.4", "3.2", "4", "3.1.", "3.1.3.0", "3.1x", "99999999999999", "-1", ".3"};
    for (size_t i = 0; i < sizeof accepted / sizeof accepted[0]; i++)
        VMFW_T_CHECK(&t, vm_user_app_validate_version(accepted[i]), "accept %s", accepted[i]);
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++)
        VMFW_T_CHECK(&t, !vm_user_app_validate_version(refused[i]), "refuse %s", refused[i]);

    VMFW_T_SECTION(&t, "Mach-O");
    uint8_t executable[64], altered[64], fat[128];
    char detail[256] = {0};
    make_macho(executable);
    VMFW_T_CHECK(&t, vm_user_app_validate_macho(executable, sizeof executable, true, detail, sizeof detail), "valid ARMv6: %s", detail);
    for (size_t size = 0; size < sizeof executable; size++)
        VMFW_T_CHECK(&t, !vm_user_app_validate_macho(executable, size, true, detail, sizeof detail), "truncated at %zu", size);
    memcpy(altered, executable, sizeof altered); fx_w32le(altered + 8u, 9u);
    VMFW_T_CHECK(&t, !vm_user_app_validate_macho(altered, sizeof altered, true, detail, sizeof detail), "ARMv7 refused");
    memcpy(altered, executable, sizeof altered); fx_w32le(altered + 60u, 1u);
    VMFW_T_CHECK(&t, !vm_user_app_validate_macho(altered, sizeof altered, true, detail, sizeof detail) && strstr(detail, "encrypted"), "encrypted refused");
    memcpy(altered, executable, sizeof altered); fx_w32le(altered + 36u, 0x00040000u);
    VMFW_T_CHECK(&t, !vm_user_app_validate_macho(altered, sizeof altered, true, detail, sizeof detail), "iOS4 refused");
    memcpy(altered, executable, sizeof altered); fx_w32le(altered + 28u, 0x24u);
    VMFW_T_CHECK(&t, !vm_user_app_validate_macho(altered, sizeof altered, true, detail, sizeof detail), "macOS refused");
    memcpy(altered, executable, sizeof altered); fx_w32le(altered + 32u, UINT32_MAX);
    VMFW_T_CHECK(&t, !vm_user_app_validate_macho(altered, sizeof altered, true, detail, sizeof detail), "bad command size refused");
    memset(fat, 0, sizeof fat); fx_w32be(fat, 0xcafebabeu); fx_w32be(fat + 4u, 1u);
    fx_w32be(fat + 8u, 12u); fx_w32be(fat + 12u, 6u); fx_w32be(fat + 16u, 64u);
    fx_w32be(fat + 20u, 64u); fx_w32be(fat + 24u, 6u); memcpy(fat + 64u, executable, 64u);
    VMFW_T_CHECK(&t, vm_user_app_validate_macho(fat, sizeof fat, true, detail, sizeof detail), "universal ARMv6 accepted: %s", detail);
    fx_w32be(fat + 12u, 9u);
    VMFW_T_CHECK(&t, !vm_user_app_validate_macho(fat, sizeof fat, true, detail, sizeof detail), "universal without ARMv6 refused");

    VMFW_T_SECTION(&t, "IPA plan");
    uint8_t archive[8192]; fx_zip_layout_t layout;
    size_t size = make_ipa(archive, sizeof archive, NULL, executable, &layout);
    vm_user_app_plan_t *plan = open_ipa(archive, size, detail);
    VMFW_T_CHECK(&t, plan != NULL, "valid synthetic IPA: %s", detail);
    if (plan) {
        VMFW_T_EQ_U(&t, vm_user_app_plan_entry_count(plan), 5u, "parents synthesized");
        const rootfs_work_entry_t *entries = vm_user_app_plan_entries(plan);
        VMFW_T_EQ_STR(&t, entries[0].path, "/Applications/org.example.Tiny.app", "isolated bundle path");
        VMFW_T_EQ_U(&t, entries[2].permissions, 0755u, "executable mode");
        VMFW_T_EQ_U(&t, entries[2].existing_policy, ROOTFS_WORK_EXISTING_REFUSE, "no overwrite");
        VMFW_T_EQ_U(&t, entries[4].permissions, 0644u, "data mode");
        uint8_t digest[32];
        VMFW_T_CHECK(&t, vm_user_app_plan_digest(plan, digest), "digest exists");
        /* The plan owns bytes; archive lifetime cannot change the candidate. */
        memset(archive, 0, size);
        VMFW_T_EQ_MEM(&t, entries[2].content, executable, sizeof executable, "archive-independent content");

        const uint8_t *found = NULL;
        size_t found_size = 0u;
        VMFW_T_CHECK(&t, vm_user_app_plan_file(plan, "EN.LPROJ/Note.TXT", &found, &found_size) &&
                     found_size == 3u && found[2] == 3u, "file lookup ignores ASCII case");
        VMFW_T_CHECK(&t, !vm_user_app_plan_file(plan, "en.lproj", &found, &found_size),
                     "a directory is not a file");
        VMFW_T_CHECK(&t, !vm_user_app_plan_file(plan, "missing.png", &found, &found_size),
                     "a missing file is reported");
        const uint64_t before_bytes = vm_user_app_plan_content_bytes(plan);
        uint8_t *icon = malloc(10u);
        memset(icon, 0x5a, 10u);
        VMFW_T_CHECK(&t, vm_user_app_plan_replace_file(plan, "en.lproj/note.txt", icon, 10u),
                     "replace a file");
        uint8_t after[32];
        VMFW_T_CHECK(&t, vm_user_app_plan_digest(plan, after) && memcmp(after, digest, 32u) != 0,
                     "the digest follows the new bytes");
        VMFW_T_EQ_U(&t, vm_user_app_plan_content_bytes(plan), before_bytes + 7u, "size follows");
        VMFW_T_CHECK(&t, vm_user_app_plan_file(plan, "en.lproj/note.txt", &found, &found_size) &&
                     found == icon && found_size == 10u, "the plan now owns the new bytes");
        uint8_t *unused = malloc(4u);
        VMFW_T_CHECK(&t, !vm_user_app_plan_replace_file(plan, "missing.png", unused, 4u),
                     "replacing a missing file is refused");
        free(unused);
    }
    vm_user_app_plan_close(&plan);
    VMFW_T_CHECK(&t, plan == NULL, "close clears pointer");
    const char *unsafe[] = {"Payload/Tiny.app/../../escape", "/absolute", "Payload/Tiny.app/a\\b", "Payload/Tiny.app/a:b", "Payload/Tiny.app//x", "Payload/Tiny.app/./x", "Payload/Other.app/x", "Payload/Tiny.app/Tiny", "Payload/Tiny.app/Frameworks/X", "Payload/Tiny.app/PlugIns/X"};
    for (size_t i = 0; i < sizeof unsafe / sizeof unsafe[0]; i++) {
        size = make_ipa(archive, sizeof archive, unsafe[i], executable, &layout);
        plan = open_ipa(archive, size, detail);
        VMFW_T_CHECK(&t, !plan, "unsafe archive %s refused", unsafe[i]);
        vm_user_app_plan_close(&plan);
    }
    size = make_ipa(archive, sizeof archive, NULL, executable, &layout);
    archive[layout.central_offset[2] + 5u] = 3u;
    fx_w32le(archive + layout.central_offset[2] + 38u, 0120777u << 16);
    plan = open_ipa(archive, size, detail);
    VMFW_T_CHECK(&t, !plan && strstr(detail, "symlinks"), "symlink refused"); vm_user_app_plan_close(&plan);
    size = make_ipa(archive, sizeof archive, NULL, executable, &layout);
    archive[layout.local_offset[2] + 30u] = 'x';
    plan = open_ipa(archive, size, detail);
    VMFW_T_CHECK(&t, !plan, "local/central name mismatch refused"); vm_user_app_plan_close(&plan);
    size = make_ipa(archive, sizeof archive, NULL, executable, &layout);
    archive[layout.central_offset[2] + 16u] ^= 1u;
    plan = open_ipa(archive, size, detail);
    VMFW_T_CHECK(&t, !plan, "CRC mismatch refused"); vm_user_app_plan_close(&plan);
    size = make_ipa(archive, sizeof archive, NULL, executable, &layout);
    fx_w32le(archive + layout.central_offset[2] + 24u, ROOTFS_WORK_MAX_ENTRY_BYTES + 1u);
    plan = open_ipa(archive, size, detail);
    VMFW_T_CHECK(&t, !plan, "oversized member refused"); vm_user_app_plan_close(&plan);
    printf("user app import: %u checks, %u failures\n", t.checks, t.failures);
    return t.failures ? 1 : 0;
}
