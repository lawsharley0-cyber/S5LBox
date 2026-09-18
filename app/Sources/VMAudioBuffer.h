/*
 * S5LBox — lock-free SPSC PCM audio circular ring buffer.
 *
 * Sits between the emulator thread (sole producer: PL080 DMA / I2S TX FIFO)
 * and the host audio callback (sole consumer: iOS AudioQueue).
 *
 * One producer owns head; one consumer owns tail. Release/acquire cursor
 * updates protect the non-atomic sample storage. Capacity is 16,383 frames
 * (one slot reserved). Overflow drops new words without blocking guest DMA;
 * underflow pads with silence. Neither policy establishes guest audio timing.
 * PCM interpretation is currently an experimental 16-bit stereo assumption.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_APP_VMAUDIOBUFFER_H
#define S5LBOX_APP_VMAUDIOBUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * MSVC does not enable C11 atomics for the C mode used by this project (see
 * VMFrameTelemetry.c). volatile LONG/LONG64 plus Interlocked* keeps the
 * Windows desktop test build portable without weakening the stock-iOS path
 * or requiring an experimental compiler switch.
 */
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
typedef volatile LONG   vm_atomic_u32_t;
typedef volatile LONG64 vm_atomic_u64_t;
#else
#include <stdatomic.h>
typedef _Atomic uint32_t vm_atomic_u32_t;
typedef _Atomic uint64_t vm_atomic_u64_t;
#endif

#define VM_AUDIO_BUFFER_CAPACITY_FRAMES 16384u
#define VM_AUDIO_BUFFER_FRAME_MASK      (VM_AUDIO_BUFFER_CAPACITY_FRAMES - 1u)
/* Legacy constant — kept so any remaining isReadyForMore() callers compile. */
#define VM_AUDIO_BUFFER_HIGH_WATER      (VM_AUDIO_BUFFER_CAPACITY_FRAMES - 2048u)

typedef struct {
    uint32_t         frames[VM_AUDIO_BUFFER_CAPACITY_FRAMES];

    /*
     * Lock-free SPSC indices (C11 atomics).
     *
     * head — next slot the producer writes.  Only the producer modifies this.
     * tail — next slot the consumer reads.   Only the consumer modifies this.
     *
     * Logical occupancy = (head - tail) & FRAME_MASK (unsigned wrap-around).
     * The buffer treats occupancy == CAPACITY - 1 as full (one wasted slot keeps
     * full/empty distinguishable without a separate count field).
     *
     * Memory order:
     *   Producer: write frame data (relaxed), then release-store head.
     *   Consumer: acquire-load head, read frame data, release-store tail.
     */
    vm_atomic_u32_t head;          /* producer-owned write cursor */
    vm_atomic_u32_t tail;          /* consumer-owned read cursor  */

    /*
     * Telemetry counters — atomic so push_word and read_frames can update
     * them with relaxed stores, with no mutex on the hot path at all.
     * vm_audio_buffer_telemetry() reads them with relaxed loads; a slightly
     * stale diagnostic count is perfectly fine for a UI report.
     */
    vm_atomic_u64_t frames_produced;
    vm_atomic_u64_t frames_consumed;
    vm_atomic_u64_t underflows;
    vm_atomic_u64_t overflows;
    vm_atomic_u32_t last_word;
    bool             initialized;
} vm_audio_buffer_t;

/* Lifecycle: both producer and consumer must be stopped. */
void vm_audio_buffer_init(vm_audio_buffer_t *buf);
void vm_audio_buffer_reset(vm_audio_buffer_t *buf);
void vm_audio_buffer_destroy(vm_audio_buffer_t *buf);

/*
 * Producer (emulator thread only): push one 32-bit stereo word.
 * Low 16 bits = left channel, high 16 bits = right channel.
 * Lock-free.  If the buffer is full the incoming frame is dropped and counted.
 */
void vm_audio_buffer_push_word(vm_audio_buffer_t *buf, uint32_t word);

/*
 * Flow-control query (legacy — DMA back-pressure path removed in b7d0324).
 * Returns true unconditionally so any remaining caller compiles safely.
 */
bool vm_audio_buffer_ready_for_more(vm_audio_buffer_t *buf);

/*
 * Consumer (AudioQueue callback thread only): read up to max_frames into
 * dst as interleaved signed-16 stereo.  Pads remaining frames with silence.
 * Lock-free on the fast path.  Returns valid guest frames read before padding.
 */
uint32_t vm_audio_buffer_read_frames(vm_audio_buffer_t *buf,
                                     int16_t *dst,
                                     uint32_t max_frames);

/* Diagnostic snapshot — safe to call from any thread at any time. */
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
