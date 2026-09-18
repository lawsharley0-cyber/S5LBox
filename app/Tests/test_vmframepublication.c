#include "VMFramePublication.h"

#include <stdio.h>
#include <string.h>

static unsigned checks;
static unsigned failed;

#define CHECK(expr) do { \
    checks++; \
    if (!(expr)) { \
        failed++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    } \
} while (0)

static void test_publication_lifecycle(void) {
    uint8_t snapshot[128];
    uint8_t pixels[128] = {0};
    memset(snapshot, 0x5a, sizeof snapshot);
    vm_frame_publication_layout_t published = {0};
    vm_frame_publication_layout_t next = {4, 4, 16, false, false};
    bool pending = false;

    CHECK(vm_frame_publication_update(snapshot, sizeof snapshot, &published,
          &pending, pixels, &next) == VM_FRAME_PUBLICATION_CHANGED);
    CHECK(pending);
    CHECK(published.width == 4 && published.height == 4);
    CHECK(memcmp(snapshot, pixels, sizeof pixels) == 0);

    /* An unread frame must survive arbitrarily many identical publications. */
    CHECK(vm_frame_publication_update(snapshot, sizeof snapshot, &published,
          &pending, pixels, &next) == VM_FRAME_PUBLICATION_UNCHANGED);
    CHECK(pending);
    pending = false; /* the consumer took the snapshot */
    CHECK(vm_frame_publication_update(snapshot, sizeof snapshot, &published,
          &pending, pixels, &next) == VM_FRAME_PUBLICATION_UNCHANGED);
    CHECK(!pending);

    /* Every byte, including tiny edits between sampled-hash positions, counts. */
    for (size_t i = 0; i < 64; i++) {
        pixels[i] ^= 0xffu;
        pending = false;
        CHECK(vm_frame_publication_update(snapshot, sizeof snapshot, &published,
              &pending, pixels, &next) == VM_FRAME_PUBLICATION_CHANGED);
        CHECK(pending && snapshot[i] == pixels[i]);
    }

    /* Change the last byte while the preceding image remains unread. */
    pixels[63] ^= 0xffu;
    CHECK(vm_frame_publication_update(snapshot, sizeof snapshot, &published,
          &pending, pixels, &next) == VM_FRAME_PUBLICATION_CHANGED);
    CHECK(pending && snapshot[63] == pixels[63]);

    pending = false;
    next.argb = true;
    CHECK(vm_frame_publication_update(snapshot, sizeof snapshot, &published,
          &pending, pixels, &next) == VM_FRAME_PUBLICATION_CHANGED);
    CHECK(pending && published.argb);

    pending = false;
    next.width = 2; next.height = 8; next.stride = 8;
    CHECK(vm_frame_publication_update(snapshot, sizeof snapshot, &published,
          &pending, pixels, &next) == VM_FRAME_PUBLICATION_CHANGED);
    CHECK(pending && published.height == 8);

    /* A same-size pitch change also changes what the image means. */
    pending = false;
    next.width = 1; next.height = 4; next.stride = 16;
    CHECK(vm_frame_publication_update(snapshot, sizeof snapshot, &published,
          &pending, pixels, &next) == VM_FRAME_PUBLICATION_CHANGED);
    CHECK(pending && published.stride == 16);

    /* Smaller frames cannot retain the previous frame's unused tail. */
    next = (vm_frame_publication_layout_t){1, 1, 4, false, false};
    CHECK(vm_frame_publication_update(snapshot, sizeof snapshot, &published,
          &pending, pixels, &next) == VM_FRAME_PUBLICATION_CHANGED);
    for (size_t i = 4; i < sizeof snapshot; i++) CHECK(snapshot[i] == 0);
}

static void test_blank_transitions(void) {
    uint8_t snapshot[16] = {0};
    uint8_t black[16] = {0};
    vm_frame_publication_layout_t published = {2, 2, 8, false, false};
    vm_frame_publication_layout_t next = published;
    bool pending = false;

    next.blank = true;
    CHECK(vm_frame_publication_update(snapshot, sizeof snapshot, &published,
          &pending, black, &next) == VM_FRAME_PUBLICATION_CHANGED);
    CHECK(pending && published.blank);
    pending = false;
    CHECK(vm_frame_publication_update(snapshot, sizeof snapshot, &published,
          &pending, black, &next) == VM_FRAME_PUBLICATION_UNCHANGED);
    CHECK(!pending);
    next.blank = false;
    CHECK(vm_frame_publication_update(snapshot, sizeof snapshot, &published,
          &pending, black, &next) == VM_FRAME_PUBLICATION_CHANGED);
    CHECK(pending && !published.blank);
}

static void test_invalid_frames(void) {
    uint8_t snapshot[32], before[32], pixels[32] = {0};
    memset(snapshot, 0x5a, sizeof snapshot);
    memcpy(before, snapshot, sizeof snapshot);
    const vm_frame_publication_layout_t initial = {2, 2, 8, false, true};
    vm_frame_publication_layout_t published = initial;
    const vm_frame_publication_layout_t invalid[] = {
        {0, 2, 8, false, false},
        {2, 0, 8, false, false},
        {2, 2, 7, false, false},
        {2, 2, 0, false, false},
        {2, 5, 8, false, false},
        {UINT32_MAX, 2, UINT32_MAX, false, false},
        {1, UINT32_MAX, UINT32_MAX, false, false}
    };
    for (unsigned p = 0; p < 2; p++) {
        bool pending = p != 0;
        for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
            CHECK(vm_frame_publication_update(snapshot, sizeof snapshot,
                  &published, &pending, pixels, &invalid[i]) ==
                  VM_FRAME_PUBLICATION_INVALID);
            CHECK(pending == (p != 0));
            CHECK(memcmp(snapshot, before, sizeof snapshot) == 0);
            CHECK(published.width == initial.width && published.blank);
        }
        CHECK(vm_frame_publication_update(snapshot, sizeof snapshot,
              &published, &pending, NULL, &initial) ==
              VM_FRAME_PUBLICATION_INVALID);
        CHECK(pending == (p != 0));
    }
    bool pending = false;
    CHECK(vm_frame_publication_update(NULL, sizeof snapshot, &published,
          &pending, pixels, &initial) == VM_FRAME_PUBLICATION_INVALID);
    CHECK(vm_frame_publication_update(snapshot, sizeof snapshot, NULL,
          &pending, pixels, &initial) == VM_FRAME_PUBLICATION_INVALID);
    CHECK(vm_frame_publication_update(snapshot, sizeof snapshot, &published,
          NULL, pixels, &initial) == VM_FRAME_PUBLICATION_INVALID);
    CHECK(vm_frame_publication_update(snapshot, sizeof snapshot, &published,
          &pending, pixels, NULL) == VM_FRAME_PUBLICATION_INVALID);
}

int main(void) {
    test_publication_lifecycle();
    test_blank_transitions();
    test_invalid_frames();
    printf("frame publication: %u checks, %u failures\n", checks, failed);
    return failed ? 1 : 0;
}
