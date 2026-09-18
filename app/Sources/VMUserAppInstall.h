#ifndef S5LBOX_VM_USER_APP_INSTALL_H
#define S5LBOX_VM_USER_APP_INSTALL_H
#include "VMUserApp.h"
#include "VMGuestInstall.h"

/* The caller must hold the machine stopped, with every engine/media handle
 * closed, throughout this operation. Never call from a running guest screen.
 * Named snapshots and dirty HFS volumes are refused; automatic resume authority
 * is invalidated durably by the shared transaction before either disk rename. */
bool vm_user_app_install(const char *work_directory, const vm_user_app_plan_t *plan,
                         void (*progress)(void *, uint64_t, uint64_t), void *progress_context,
                         vm_guest_install_result_t *result,
                         char *detail, size_t detail_capacity);
#endif
