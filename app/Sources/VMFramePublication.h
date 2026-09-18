/* Exact host-frame publication bookkeeping. No guest state is changed here. */
#ifndef S5LBOX_APP_VMFRAMEPUBLICATION_H
#define S5LBOX_APP_VMFRAMEPUBLICATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    bool argb;
    bool blank;
} vm_frame_publication_layout_t;

typedef enum {
    VM_FRAME_PUBLICATION_INVALID = -1,
    VM_FRAME_PUBLICATION_UNCHANGED = 0,
    VM_FRAME_PUBLICATION_CHANGED = 1
} vm_frame_publication_result_t;

/* Compare all bytes and layout with the last publication, then copy only a
 * changed frame. An initial zero layout and transitions to/from a blank panel
 * always publish. Clear unused capacity on a changed frame so smaller layouts
 * cannot expose old pixels. The caller owns the buffers and synchronization;
 * snapshot and pixels must not overlap.
 *
 * A changed frame sets pending=true. An unchanged or invalid frame leaves
 * EVERYTHING untouched, including an unread pending publication. A sampled
 * signature is deliberately insufficient here: a one-pixel edit must arrive.
 */
vm_frame_publication_result_t vm_frame_publication_update(
    void *snapshot, size_t capacity,
    vm_frame_publication_layout_t *published, bool *pending,
    const void *pixels, const vm_frame_publication_layout_t *next);

#endif
