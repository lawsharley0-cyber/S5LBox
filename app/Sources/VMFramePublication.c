#include "VMFramePublication.h"

#include <string.h>

static bool frame_bytes(const vm_frame_publication_layout_t *layout,
                        size_t capacity, size_t *bytes) {
    if (!layout || layout->width == 0u || layout->height == 0u ||
        layout->width > layout->stride / 4u ||
        (size_t)layout->stride > SIZE_MAX / (size_t)layout->height)
        return false;
    *bytes = (size_t)layout->stride * (size_t)layout->height;
    return *bytes <= capacity;
}

vm_frame_publication_result_t vm_frame_publication_update(
        void *snapshot, size_t capacity,
        vm_frame_publication_layout_t *published, bool *pending,
        const void *pixels, const vm_frame_publication_layout_t *next) {
    size_t bytes = 0u;
    if (!snapshot || !published || !pending || !pixels ||
        !frame_bytes(next, capacity, &bytes))
        return VM_FRAME_PUBLICATION_INVALID;

    if (published->width == next->width &&
        published->height == next->height &&
        published->stride == next->stride &&
        published->argb == next->argb &&
        published->blank == next->blank &&
        memcmp(snapshot, pixels, bytes) == 0)
        return VM_FRAME_PUBLICATION_UNCHANGED;

    memcpy(snapshot, pixels, bytes);
    if (bytes < capacity)
        memset((uint8_t *)snapshot + bytes, 0, capacity - bytes);
    *published = *next;
    *pending = true;
    return VM_FRAME_PUBLICATION_CHANGED;
}
