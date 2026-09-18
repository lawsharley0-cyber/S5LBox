/*
 * S5LBox — thread-safe PCM audio circular ring buffer.
 *
 * Sits between the emulator thread (producer: PL080 DMA / I2S TX FIFO)
 * and the host audio callback (consumer: iOS AudioQueue).
 *
 * Properties:
 *  - Interleaved 16-bit signed stereo PCM (4 bytes per frame).
 *  - Fixed power-of-two capacity (16,384 frames ~ 371 ms at 44.1 kHz).
 *  - Hardware backpressure thresholding for DMA pacing.
 *  - Underflow protection: pads with silence on starve to prevent glitching.
 *  - Telemetry tracking for diagnostics and performance reports.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_APP_VMAUDIOBUFFER_H
#define S5LBOX_APP_VMAUDIOBUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#define VM_AUDIO_BUFFER_CAPACITY_FRAMES 16384u
#define VM_AUDIO_BUFFER_FRAME_MASK      (VM_AUDIO_BUFFER_CAPACITY_FRAMES - 1u)
#define VM_AUDIO_BUFFER_HIGH_WATER      (VM_AUDIO_BUFFER_CAPACITY_FRAMES - 2048u)

typedef struct {
    uint32_t        frames[VM_AUDIO_BUFFER_CAPACITY_FRAMES];
    uint32_t        head;  /* write position */
    uint32_t        tail;  /* read position */
    uint32_t        count; /* stored frames */
    pthread_mutex_t lock;

    /* Telemetry counters */
    uint64_t        frames_produced;
    uint64_t        frames_consumed;
    uint64_t        underflows;
    uint64_t        overflows;
    uint32_t        last_word;
    bool            initialized;
} vm_audio_buffer_t;

/* Initialize or reset the ring buffer. */
void vm_audio_buffer_init(vm_audio_buffer_t *buf);
void vm_audio_buffer_reset(vm_audio_buffer_t *buf);
void vm_audio_buffer_destroy(vm_audio_buffer_t *buf);

/* Producer: Push one 32-bit stereo word (low 16 Left, high 16 Right). */
void vm_audio_buffer_push_word(vm_audio_buffer_t *buf, uint32_t word);

/* Flow control query: returns true if buffer has sufficient space for DMA bursts. */
bool vm_audio_buffer_ready_for_more(vm_audio_buffer_t *buf);

/*
 * Consumer: Read up to max_frames into dst (interleaved 16-bit signed stereo).
 * If buffer has fewer frames than max_frames, pads the rest with silence (0).
 * Returns actual number of valid guest frames read before padding.
 */
uint32_t vm_audio_buffer_read_frames(vm_audio_buffer_t *buf,
                                     int16_t *dst,
                                     uint32_t max_frames);

/* Snapshot of buffer telemetry. */
typedef struct {
    uint32_t count;
    uint64_t produced;
    uint64_t consumed;
    uint64_t underflows;
    uint64_t overflows;
    uint32_t last_word;
} vm_audio_telemetry_t;

void vm_audio_buffer_telemetry(vm_audio_buffer_t *buf, vm_audio_telemetry_t *out);

#endif /* S5LBOX_APP_VMAUDIOBUFFER_H */
