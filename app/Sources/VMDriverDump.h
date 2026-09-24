/*
 * S5LBox — what a guest driver calls, for reading its code off the device.
 *
 * "Save Full Test Report" carries the whole kext that touched a device we do
 * not model yet (the audio block, first), so its logic can be read offline in
 * one pass instead of one excerpt per test session. Prelinked kexts carry no
 * symbols of their own (docs/debugging.md §2), so the one thing that makes such
 * a dump readable is knowing which KERNEL functions it calls. This finds those
 * references in the kext's own bytes; the caller names them with ksyms.h.
 *
 * Plain C over caller-owned bytes, host-tested in app/Tests/test_vmdriverdump.c.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_VM_DRIVER_DUMP_H
#define S5LBOX_VM_DRIVER_DUMP_H

#include <stddef.h>
#include <stdint.h>

enum {
    VM_DRIVER_REF_CALL = 1u,   /* ARM BL/BLX or a Thumb BL/BLX pair      */
    VM_DRIVER_REF_WORD = 2u,   /* an aligned 32-bit word (vtable, literal) */
};

typedef struct {
    uint32_t target;   /* Thumb bit cleared                                 */
    uint32_t count;    /* how many sites                                    */
    uint8_t  kinds;    /* VM_DRIVER_REF_* bits                              */
} vm_driver_ref_t;

/*
 * Every reference in `code` (the `len` bytes mapped at `va`) to an address in
 * [range_lo, range_hi) and outside the code itself: ARM BL/BLX immediates at
 * each word, Thumb BL/BLX pairs at each halfword, and each aligned word whose
 * value lies in the range. Decoding every alignment means data can read as a
 * branch; the caller keeps only targets that name a function. Unique by
 * target and sorted by it. Returns the number stored (at most `cap`); *total,
 * if given, receives how many distinct targets were found.
 */
size_t vm_driver_collect_refs(const uint8_t *code, uint32_t va, uint32_t len,
                              uint32_t range_lo, uint32_t range_hi,
                              vm_driver_ref_t *out, size_t cap, size_t *total);

#endif /* S5LBOX_VM_DRIVER_DUMP_H */
