/*
 * S5LBox — thread-safe PCM audio circular ring buffer implementation.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "VMAudioBuffer.h"
#include <string.h>

void vm_audio_buffer_init(vm_audio_buffer_t *buf) {
    if (!buf) return;
    memset(buf, 0, sizeof *buf);
    pthread_mutex_init(&buf->lock, NULL);
    buf->initialized = true;
}

void vm_audio_buffer_reset(vm_audio_buffer_t *buf) {
    if (!buf || !buf->initialized) return;
    pthread_mutex_lock(&buf->lock);
    buf->head = 0;
    buf->tail = 0;
    buf->count = 0;
    buf->frames_produced = 0;
    buf->frames_consumed = 0;
    buf->underflows = 0;
    buf->overflows = 0;
    buf->last_word = 0;
    pthread_mutex_unlock(&buf->lock);
}

void vm_audio_buffer_destroy(vm_audio_buffer_t *buf) {
    if (!buf || !buf->initialized) return;
    pthread_mutex_destroy(&buf->lock);
    buf->initialized = false;
}

void vm_audio_buffer_push_word(vm_audio_buffer_t *buf, uint32_t word) {
    if (!buf || !buf->initialized) return;
    pthread_mutex_lock(&buf->lock);
    if (buf->count >= VM_AUDIO_BUFFER_CAPACITY_FRAMES) {
        buf->overflows++;
        /* Overwrite oldest frame to prevent permanently blocking producer */
        buf->tail = (buf->tail + 1u) & VM_AUDIO_BUFFER_FRAME_MASK;
        buf->count--;
    }
    buf->frames[buf->head] = word;
    buf->head = (buf->head + 1u) & VM_AUDIO_BUFFER_FRAME_MASK;
    buf->count++;
    buf->frames_produced++;
    buf->last_word = word;
    pthread_mutex_unlock(&buf->lock);
}

bool vm_audio_buffer_ready_for_more(vm_audio_buffer_t *buf) {
    if (!buf || !buf->initialized) return false;
    pthread_mutex_lock(&buf->lock);
    bool ready = buf->count < VM_AUDIO_BUFFER_HIGH_WATER;
    pthread_mutex_unlock(&buf->lock);
    return ready;
}

uint32_t vm_audio_buffer_read_frames(vm_audio_buffer_t *buf,
                                     int16_t *dst,
                                     uint32_t max_frames) {
    if (!buf || !buf->initialized || !dst || max_frames == 0) return 0;
    pthread_mutex_lock(&buf->lock);
    uint32_t available = buf->count;
    uint32_t to_read = available < max_frames ? available : max_frames;

    for (uint32_t i = 0; i < to_read; i++) {
        uint32_t word = buf->frames[buf->tail];
        buf->tail = (buf->tail + 1u) & VM_AUDIO_BUFFER_FRAME_MASK;
        /* Interleaved 16-bit stereo: Left = bits 0-15, Right = bits 16-31 */
        dst[i * 2u + 0u] = (int16_t)(word & 0xffffu);
        dst[i * 2u + 1u] = (int16_t)((word >> 16) & 0xffffu);
    }
    buf->count -= to_read;
    buf->frames_consumed += to_read;

    if (to_read < max_frames) {
        /* Underflow: Pad remaining frames with silence */
        uint32_t remaining = max_frames - to_read;
        memset(&dst[to_read * 2u], 0, remaining * sizeof(int16_t) * 2u);
        buf->underflows++;
    }
    pthread_mutex_unlock(&buf->lock);
    return to_read;
}

void vm_audio_buffer_telemetry(vm_audio_buffer_t *buf, vm_audio_telemetry_t *out) {
    if (!buf || !buf->initialized || !out) return;
    pthread_mutex_lock(&buf->lock);
    out->count = buf->count;
    out->produced = buf->frames_produced;
    out->consumed = buf->frames_consumed;
    out->underflows = buf->underflows;
    out->overflows = buf->overflows;
    out->last_word = buf->last_word;
    pthread_mutex_unlock(&buf->lock);
}
