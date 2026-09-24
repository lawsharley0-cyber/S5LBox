/* User-owned legacy IPA validation and a bounded, create-only HFS plan.
 * No downloads, DRM decryption, host execution, or guest API substitution.
 * MIT licensed; see the repository LICENSE. */
#ifndef S5LBOX_VM_USER_APP_H
#define S5LBOX_VM_USER_APP_H

#include "VMFirmwareFormats.h"
#include "rootfs_work.h"

#define VM_USER_APP_MAX_ARCHIVE (128u * 1024u * 1024u)
#define VM_USER_APP_MAX_CONTENT (64u * 1024u * 1024u)
#define VM_USER_APP_MAX_PLIST (1024u * 1024u)
#define VM_USER_APP_DETAIL_CAPACITY 256u

typedef struct {
    char identifier[160];
    char executable[160];
    char display_name[160];
    char minimum_os[32];
    /* The host parser must require CFBundlePackageType=APPL and, when
     * CFBundleSupportedPlatforms is present, iPhoneOS. */
    bool iphone_application;
} vm_user_app_metadata_t;

/* The iOS adapter uses NSPropertyListSerialization for XML and binary plists.
 * The exact bytes extracted from the archive are passed here and retained
 * unchanged in the guest plan. Return false for missing/wrong-typed fields. */
typedef bool (*vm_user_app_plist_fn)(void *context, const uint8_t *bytes,
                                    size_t size, vm_user_app_metadata_t *out,
                                    char *detail, size_t detail_capacity);

typedef struct vm_user_app_plan vm_user_app_plan_t;

vm_user_app_plan_t *vm_user_app_plan_open(
    vmfw_pread_fn read, void *read_context, uint64_t archive_size,
    vm_user_app_plist_fn parse_plist, void *plist_context,
    char *detail, size_t detail_capacity);
void vm_user_app_plan_close(vm_user_app_plan_t **plan);
const vm_user_app_metadata_t *vm_user_app_plan_metadata(const vm_user_app_plan_t *plan);
const rootfs_work_entry_t *vm_user_app_plan_entries(const vm_user_app_plan_t *plan);
size_t vm_user_app_plan_entry_count(const vm_user_app_plan_t *plan);
uint64_t vm_user_app_plan_content_bytes(const vm_user_app_plan_t *plan);
bool vm_user_app_plan_digest(const vm_user_app_plan_t *plan, uint8_t digest[32]);

/*
 * A regular file of the bundle by its path relative to the .app (for example
 * "Icon.png"), matched without regard to ASCII case the way the guest's
 * HFS+ volume looks names up. The bytes stay owned by the plan.
 */
bool vm_user_app_plan_file(const vm_user_app_plan_t *plan, const char *relative,
                           const uint8_t **bytes, size_t *size);
/*
 * Replace that file's bytes with `content` (malloc'd; the plan takes it only
 * when this returns true) and recompute the plan's size and digest, so the
 * install transaction records what is actually written. Used to give an icon
 * the rounded corners iPhone OS 3 does not add to apps in /Applications.
 */
bool vm_user_app_plan_replace_file(vm_user_app_plan_t *plan, const char *relative,
                                   uint8_t *content, size_t size);

/* Public for focused tests and future non-UIKit frontends. */
bool vm_user_app_validate_macho(const uint8_t *bytes, size_t size, bool main_executable,
                               char *detail, size_t detail_capacity);
bool vm_user_app_validate_version(const char *version);

#endif
