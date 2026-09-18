#include "VMUserAppInstall.h"
#include "VMSnapshotStore.h"

#include <stdio.h>
#include <string.h>

static bool refusal(char *detail, size_t capacity, const char *text) {
    if (detail && capacity) (void)snprintf(detail, capacity, "%s", text);
    return false;
}

bool vm_user_app_install(const char *work, const vm_user_app_plan_t *plan,
                         void (*progress)(void *, uint64_t, uint64_t), void *context,
                         vm_guest_install_result_t *result,
                         char *detail, size_t capacity) {
    if (result) memset(result, 0, sizeof *result);
    if (!work || !*work || !plan || !result)
        return refusal(detail, capacity, "The selected machine or app is unavailable.");
    vm_guest_install_result_t privilege, storage;
    if (vm_guest_maintenance_recover(work, &privilege, &storage, detail, capacity) != VM_GUEST_INSTALL_OK)
        return false;
    char snapshot_dir[VM_GUEST_INSTALL_PATH_CAPACITY];
    vm_snapshot_info_t snapshots[VM_SNAPSHOT_MAX];
    size_t count = 0u;
    if (vm_snapshot_dir(work, snapshot_dir, sizeof snapshot_dir) != VM_SNAPSHOT_OK ||
        vm_snapshot_list(snapshot_dir, snapshots, VM_SNAPSHOT_MAX, &count, detail, capacity) != VM_SNAPSHOT_OK)
        return refusal(detail, capacity, "The machine's historical snapshots could not be checked safely.");
    if (count)
        return refusal(detail, capacity, "This machine has named snapshots tied to its old disk. Remove those snapshots explicitly before importing an app.");
    char live[VM_GUEST_INSTALL_PATH_CAPACITY], stage[VM_GUEST_INSTALL_PATH_CAPACITY];
    int written = snprintf(live, sizeof live, "%s/%s", work, VM_GUEST_INSTALL_LIVE_FILE);
    if (written < 0 || (size_t)written >= sizeof live ||
        !vm_guest_apps_stage_image_path(stage, sizeof stage, work))
        return refusal(detail, capacity, "The guest disk path is too long.");
    rootfs_work_result_t disk;
    rootfs_work_status_t status = rootfs_work_validate_source(live, &disk);
    if (status != ROOTFS_WORK_OK) {
        if (strstr(disk.detail, "not cleanly unmounted"))
            return refusal(detail, capacity, "Shut down iPhone OS first: hold Power, slide to power off, wait for it to halt, then return to Machines. Pausing or closing S5LBox does not cleanly unmount the guest disk.");
        return refusal(detail, capacity, disk.detail[0] ? disk.detail : rootfs_work_status_name(status));
    }
    if (vm_guest_apps_prepare_stage(work, result, detail, capacity) != VM_GUEST_INSTALL_OK) return false;
    rootfs_work_options_t options;
    memset(&options, 0, sizeof options);
    options.preserve_fstab = true;
    options.entries = vm_user_app_plan_entries(plan);
    options.entry_count = vm_user_app_plan_entry_count(plan);
    options.progress = progress;
    options.progress_ctx = context;
    status = rootfs_work_create(live, stage, &options, &disk);
    if (status != ROOTFS_WORK_OK || !disk.published) {
        if (status == ROOTFS_WORK_PROVISION_EXISTS)
            return refusal(detail, capacity, "This app's bundle identifier is already installed. Replacing or uninstalling apps is not supported yet; the existing disk was preserved.");
        return refusal(detail, capacity, disk.detail[0] ? disk.detail : rootfs_work_status_name(status));
    }
    uint8_t digest[32];
    if (!vm_user_app_plan_digest(plan, digest)) return refusal(detail, capacity, "The app plan lost its identity.");
    if (vm_guest_apps_publish(work, digest, result, detail, capacity) != VM_GUEST_INSTALL_OK || !result->committed)
        return false;
    return true;
}
