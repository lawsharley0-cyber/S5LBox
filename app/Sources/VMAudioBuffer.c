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

void vm_audio_buffer_init(vm_audio_buffer_t *buf) {
    if (!buf) return;
    memset(buf, 0, sizeof *buf);
    atomic_init(&buf->head, 0u);
    atomic_init(&buf->tail, 0u);
    atomic_init(&buf->frames_produced, 0u);
    atomic_init(&buf->frames_consumed, 0u);
    atomic_init(&buf->underflows, 0u);
    atomic_init(&buf->overflows, 0u);
    atomic_init(&buf->last_word, 0u);
    buf->initialized = true;
}

void vm_audio_buffer_reset(vm_audio_buffer_t *buf) {
    if (!buf || !buf->initialized) return;
    /* Caller must stop both producer and consumer before resetting. */
    atomic_store(&buf->head, 0u);
    atomic_store(&buf->tail, 0u);
    atomic_store(&buf->frames_produced, 0u);
    atomic_store(&buf->frames_consumed, 0u);
    atomic_store(&buf->underflows, 0u);
    atomic_store(&buf->overflows, 0u);
    atomic_store(&buf->last_word, 0u);
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

    uint32_t h = atomic_load_explicit(&buf->head, memory_order_relaxed);
    uint32_t t = atomic_load_explicit(&buf->tail, memory_order_acquire);

    uint32_t occupancy = (h - t) & VM_AUDIO_BUFFER_FRAME_MASK;
    if (occupancy >= VM_AUDIO_BUFFER_CAPACITY_FRAMES - 1u) {
        /* Never overwrite a slot the consumer may still be reading. */
        atomic_fetch_add_explicit(&buf->overflows, 1u, memory_order_relaxed);
        return;
    }

    buf->frames[h] = word;
    atomic_store_explicit(&buf->head,
                          (h + 1u) & VM_AUDIO_BUFFER_FRAME_MASK,
                          memory_order_release);

    atomic_fetch_add_explicit(&buf->frames_produced, 1u, memory_order_relaxed);
    atomic_store_explicit(&buf->last_word, word, memory_order_relaxed);
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

    uint32_t h = atomic_load_explicit(&buf->head, memory_order_acquire);
    uint32_t t = atomic_load_explicit(&buf->tail, memory_order_relaxed);

    uint32_t available = (h - t) & VM_AUDIO_BUFFER_FRAME_MASK;
    uint32_t to_read = available < max_frames ? available : max_frames;

    for (uint32_t i = 0; i < to_read; i++) {
        uint32_t word = buf->frames[t];
        t = (t + 1u) & VM_AUDIO_BUFFER_FRAME_MASK;
        /* Interleaved 16-bit stereo: Left = bits 0-15, Right = bits 16-31. */
        dst[i * 2u + 0u] = (int16_t)(word & 0xffffu);
        dst[i * 2u + 1u] = (int16_t)((word >> 16) & 0xffffu);
    }
    atomic_store_explicit(&buf->tail, t, memory_order_release);

    if (to_read < max_frames) {
        /* Underflow: pad remaining with silence. */
        uint32_t remaining = max_frames - to_read;
        memset(&dst[to_read * 2u], 0, remaining * sizeof(int16_t) * 2u);
        atomic_fetch_add_explicit(&buf->underflows, 1u, memory_order_relaxed);
    }
    atomic_fetch_add_explicit(&buf->frames_consumed,
                              (uint64_t)to_read, memory_order_relaxed);
    return to_read;
}

/* Telemetry snapshot — safe from any thread, values may be slightly stale. */
void vm_audio_buffer_telemetry(vm_audio_buffer_t *buf, vm_audio_telemetry_t *out) {
    if (!buf || !buf->initialized || !out) return;
    uint32_t h = atomic_load_explicit(&buf->head, memory_order_acquire);
    uint32_t t = atomic_load_explicit(&buf->tail, memory_order_acquire);
    out->count      = (h - t) & VM_AUDIO_BUFFER_FRAME_MASK;
    out->produced   = atomic_load_explicit(&buf->frames_produced,
                                           memory_order_relaxed);
    out->consumed   = atomic_load_explicit(&buf->frames_consumed,
                                           memory_order_relaxed);
    out->underflows = atomic_load_explicit(&buf->underflows,
                                           memory_order_relaxed);
    out->overflows  = atomic_load_explicit(&buf->overflows,
                                           memory_order_relaxed);
    out->last_word  = atomic_load_explicit(&buf->last_word,
                                           memory_order_relaxed);
}
