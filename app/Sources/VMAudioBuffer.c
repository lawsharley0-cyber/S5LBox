/*
 * S5LBox — lock-free SPSC PCM audio circular ring buffer implementation.
 *
 * One producer and one consumer own separate cursors. Full buffers drop new
 * words without stalling the emulated DMA or touching consumer-owned data.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "VMAudioBuffer.h"
#include <string.h>

/*
 * Portable atomic helpers, parameterised by the same relaxed/acquire/release
 * intent as the stdatomic calls this file always used. On Windows every
 * order maps to a full-barrier Interlocked* op (stronger than requested,
 * which is always safe, never incorrect) because MSVC does not enable C11
 * atomics for this project's C mode — see the header and VMFrameTelemetry.c.
 * Everywhere else the order is passed straight through to stdatomic, so the
 * shipping (non-Windows) path's memory model is unchanged from before.
 */
typedef enum { VM_ORDER_RELAXED, VM_ORDER_ACQUIRE, VM_ORDER_RELEASE } vm_order_t;

#if defined(_WIN32)
static uint32_t vm_atomic_load_u32(vm_atomic_u32_t *p, vm_order_t order) {
    (void)order;
    return (uint32_t)InterlockedCompareExchange(p, 0, 0);
}
static void vm_atomic_store_u32(vm_atomic_u32_t *p, uint32_t v, vm_order_t order) {
    (void)order;
    (void)InterlockedExchange(p, (LONG)v);
}
static uint32_t vm_atomic_fetch_add_u32(vm_atomic_u32_t *p, uint32_t v, vm_order_t order) {
    (void)order;
    return (uint32_t)InterlockedExchangeAdd(p, (LONG)v);
}
static uint64_t vm_atomic_load_u64(vm_atomic_u64_t *p, vm_order_t order) {
    (void)order;
    return (uint64_t)InterlockedCompareExchange64(p, 0, 0);
}
static void vm_atomic_store_u64(vm_atomic_u64_t *p, uint64_t v, vm_order_t order) {
    (void)order;
    (void)InterlockedExchange64(p, (LONG64)v);
}
static uint64_t vm_atomic_fetch_add_u64(vm_atomic_u64_t *p, uint64_t v, vm_order_t order) {
    (void)order;
    return (uint64_t)InterlockedExchangeAdd64(p, (LONG64)v);
}
#else
static memory_order vm_order(vm_order_t order) {
    switch (order) {
    case VM_ORDER_ACQUIRE: return memory_order_acquire;
    case VM_ORDER_RELEASE: return memory_order_release;
    default:               return memory_order_relaxed;
    }
}
static uint32_t vm_atomic_load_u32(vm_atomic_u32_t *p, vm_order_t order) {
    return atomic_load_explicit(p, vm_order(order));
}
static void vm_atomic_store_u32(vm_atomic_u32_t *p, uint32_t v, vm_order_t order) {
    atomic_store_explicit(p, v, vm_order(order));
}
static uint32_t vm_atomic_fetch_add_u32(vm_atomic_u32_t *p, uint32_t v, vm_order_t order) {
    return atomic_fetch_add_explicit(p, v, vm_order(order));
}
static uint64_t vm_atomic_load_u64(vm_atomic_u64_t *p, vm_order_t order) {
    return atomic_load_explicit(p, vm_order(order));
}
static void vm_atomic_store_u64(vm_atomic_u64_t *p, uint64_t v, vm_order_t order) {
    atomic_store_explicit(p, v, vm_order(order));
}
static uint64_t vm_atomic_fetch_add_u64(vm_atomic_u64_t *p, uint64_t v, vm_order_t order) {
    return atomic_fetch_add_explicit(p, v, vm_order(order));
}
#endif

void vm_audio_buffer_init(vm_audio_buffer_t *buf) {
    if (!buf) return;
    memset(buf, 0, sizeof *buf);
    vm_atomic_store_u32(&buf->head, 0u, VM_ORDER_RELAXED);
    vm_atomic_store_u32(&buf->tail, 0u, VM_ORDER_RELAXED);
    vm_atomic_store_u64(&buf->frames_produced, 0u, VM_ORDER_RELAXED);
    vm_atomic_store_u64(&buf->frames_consumed, 0u, VM_ORDER_RELAXED);
    vm_atomic_store_u64(&buf->underflows, 0u, VM_ORDER_RELAXED);
    vm_atomic_store_u64(&buf->overflows, 0u, VM_ORDER_RELAXED);
    vm_atomic_store_u32(&buf->last_word, 0u, VM_ORDER_RELAXED);
    buf->initialized = true;
}

void vm_audio_buffer_reset(vm_audio_buffer_t *buf) {
    if (!buf || !buf->initialized) return;
    /* Caller must stop both producer and consumer before resetting. */
    vm_atomic_store_u32(&buf->head, 0u, VM_ORDER_RELAXED);
    vm_atomic_store_u32(&buf->tail, 0u, VM_ORDER_RELAXED);
    vm_atomic_store_u64(&buf->frames_produced, 0u, VM_ORDER_RELAXED);
    vm_atomic_store_u64(&buf->frames_consumed, 0u, VM_ORDER_RELAXED);
    vm_atomic_store_u64(&buf->underflows, 0u, VM_ORDER_RELAXED);
    vm_atomic_store_u64(&buf->overflows, 0u, VM_ORDER_RELAXED);
    vm_atomic_store_u32(&buf->last_word, 0u, VM_ORDER_RELAXED);
}

void vm_audio_buffer_destroy(vm_audio_buffer_t *buf) {
    if (!buf || !buf->initialized) return;
    buf->initialized = false;
}

/*
 * HOT PATH — emulator thread, called on every I2S DMA word.
 *
 * Wasted-slot convention: full when (head - tail) & MASK == CAPACITY - 1.
 * On overflow we drop the incoming word; only the consumer writes tail.
 *
 * Memory model:
 *   1. Write frame data with a plain (non-atomic) store — only the producer
 *      touches frames[head], so no race is possible before the head advance.
 *   2. Release-store head — the consumer's acquire-load of head pairs with
 *      this, guaranteeing it sees step 1's data before acting on the new head.
 *   3. Relaxed-store telemetry counters — these are diagnostic only; the
 *      consumer never uses them to decide what to read.
 */
void vm_audio_buffer_push_word(vm_audio_buffer_t *buf, uint32_t word) {
    if (!buf || !buf->initialized) return;

    uint32_t h = vm_atomic_load_u32(&buf->head, VM_ORDER_RELAXED);
    uint32_t t = vm_atomic_load_u32(&buf->tail, VM_ORDER_ACQUIRE);

    uint32_t occupancy = (h - t) & VM_AUDIO_BUFFER_FRAME_MASK;
    if (occupancy >= VM_AUDIO_BUFFER_CAPACITY_FRAMES - 1u) {
        /* Never overwrite a slot the consumer may still be reading. */
        vm_atomic_fetch_add_u64(&buf->overflows, 1u, VM_ORDER_RELAXED);
        return;
    }

    buf->frames[h] = word;
    vm_atomic_store_u32(&buf->head,
                        (h + 1u) & VM_AUDIO_BUFFER_FRAME_MASK,
                        VM_ORDER_RELEASE);

    vm_atomic_fetch_add_u64(&buf->frames_produced, 1u, VM_ORDER_RELAXED);
    vm_atomic_store_u32(&buf->last_word, word, VM_ORDER_RELAXED);
}

/* Legacy — DMA back-pressure removed in b7d0324. Always returns true. */
bool vm_audio_buffer_ready_for_more(vm_audio_buffer_t *buf) {
    (void)buf;
    return true;
}

/*
 * CONSUMER PATH — AudioQueue callback thread.
 *
 * Acquire-load head pairs with the producer's release-store of head, so we
 * see all frame data written before that store.
 * Release-store tail pairs with the producer's acquire-load of tail, so the
 * producer sees the freed slots.
 */
uint32_t vm_audio_buffer_read_frames(vm_audio_buffer_t *buf,
                                     int16_t *dst,
                                     uint32_t max_frames) {
    if (!buf || !buf->initialized || !dst || max_frames == 0) return 0;

    uint32_t h = vm_atomic_load_u32(&buf->head, VM_ORDER_ACQUIRE);
    uint32_t t = vm_atomic_load_u32(&buf->tail, VM_ORDER_RELAXED);

    uint32_t available = (h - t) & VM_AUDIO_BUFFER_FRAME_MASK;
    uint32_t to_read = available < max_frames ? available : max_frames;

    for (uint32_t i = 0; i < to_read; i++) {
        uint32_t word = buf->frames[t];
        t = (t + 1u) & VM_AUDIO_BUFFER_FRAME_MASK;
        /* Interleaved 16-bit stereo: Left = bits 0-15, Right = bits 16-31. */
        dst[i * 2u + 0u] = (int16_t)(word & 0xffffu);
        dst[i * 2u + 1u] = (int16_t)((word >> 16) & 0xffffu);
    }
    vm_atomic_store_u32(&buf->tail, t, VM_ORDER_RELEASE);

    if (to_read < max_frames) {
        /* Underflow: pad remaining with silence. */
        uint32_t remaining = max_frames - to_read;
        memset(&dst[to_read * 2u], 0, remaining * sizeof(int16_t) * 2u);
        vm_atomic_fetch_add_u64(&buf->underflows, 1u, VM_ORDER_RELAXED);
    }
    vm_atomic_fetch_add_u64(&buf->frames_consumed,
                            (uint64_t)to_read, VM_ORDER_RELAXED);
    return to_read;
}

/* Telemetry snapshot — safe from any thread, values may be slightly stale. */
void vm_audio_buffer_telemetry(vm_audio_buffer_t *buf, vm_audio_telemetry_t *out) {
    if (!buf || !buf->initialized || !out) return;
    uint32_t h = vm_atomic_load_u32(&buf->head, VM_ORDER_ACQUIRE);
    uint32_t t = vm_atomic_load_u32(&buf->tail, VM_ORDER_ACQUIRE);
    out->count      = (h - t) & VM_AUDIO_BUFFER_FRAME_MASK;
    out->produced   = vm_atomic_load_u64(&buf->frames_produced, VM_ORDER_RELAXED);
    out->consumed   = vm_atomic_load_u64(&buf->frames_consumed, VM_ORDER_RELAXED);
    out->underflows = vm_atomic_load_u64(&buf->underflows, VM_ORDER_RELAXED);
    out->overflows  = vm_atomic_load_u64(&buf->overflows, VM_ORDER_RELAXED);
    out->last_word  = vm_atomic_load_u32(&buf->last_word, VM_ORDER_RELAXED);
}
