/*
 * S5LBox — lock-free SPSC PCM audio circular ring buffer.
 *
 * Sits between the emulator thread (sole producer: PL080 DMA / I2S TX FIFO)
 * and the host audio callback (sole consumer: iOS AudioQueue).
 *
 * WHY THIS WAS REWRITTEN
 *
 * The original design called pthread_mutex_lock/unlock on every single
 * vm_audio_buffer_push_word() call.  Because PL080 DMA delivers audio one
 * 32-bit word at a time, that meant tens of thousands of mutex round-trips
 * per second on the emulator thread — cutting guest instruction throughput
 * by ~40-60% and turning a ~1-minute boot into a many-minute crawl.
 *
 * This replacement is a textbook lock-free SPSC (single-producer,
 * single-consumer) ring buffer using C11 _Atomic indices:
 *
 *   head — written ONLY by the producer (emulator thread).
 *   tail — written ONLY by the consumer (AudioQueue callback thread).
 *
 * The producer does a plain (non-atomic) write of the frame data, then an
 * atomic release-store of head.  The consumer does an atomic acquire-load
 * of head, reads the frame, then a release-store of tail.  This acquire/
 * release pair is the only synchronisation on the hot path — no syscall,
 * no kernel involvement, no cache-line ping-pong on a shared lock word.
 *
 * The only remaining mutex (telem_lock) guards the diagnostic counters
 * (frames_produced, overflows, etc.) which are updated infrequently and
 * only read when the user opens the Performance report.
 *
 * Properties:
 *  - Interleaved 16-bit signed stereo PCM (4 bytes per frame).
 *  - Fixed power-of-two capacity (16,384 frames ~ 371 ms at 44.1 kHz).
 *  - Overflow policy: oldest frame overwritten; producer never stalls.
 *  - Underflow policy: silence padding so AudioQueue never glitches.
 *  - Telemetry counters read-only from performance report path.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_APP_VMAUDIOBUFFER_H
#define S5LBOX_APP_VMAUDIOBUFFER_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
     * The buffer treats occupancy == CAPACITY as full (one wasted slot keeps
     * full/empty distinguishable without a separate count field).
     *
     * Memory order:
     *   Producer: write frame data (relaxed), then release-store head.
     *   Consumer: acquire-load head, read frame data, release-store tail.
     */
    _Atomic uint32_t head;          /* producer-owned write cursor */
    _Atomic uint32_t tail;          /* consumer-owned read cursor  */

    /*
     * Telemetry counters — _Atomic so push_word and read_frames can update
     * them with relaxed stores, with no mutex on the hot path at all.
     * vm_audio_buffer_telemetry() reads them with relaxed loads; a slightly
     * stale diagnostic count is perfectly fine for a UI report.
     */
    _Atomic uint64_t frames_produced;
    _Atomic uint64_t frames_consumed;
    _Atomic uint64_t underflows;
    _Atomic uint64_t overflows;
    _Atomic uint32_t last_word;
    bool             initialized;
} vm_audio_buffer_t;

/* Lifecycle */
void vm_audio_buffer_init(vm_audio_buffer_t *buf);
void vm_audio_buffer_reset(vm_audio_buffer_t *buf);
void vm_audio_buffer_destroy(vm_audio_buffer_t *buf);

/*
 * Producer (emulator thread only): push one 32-bit stereo word.
 * Low 16 bits = left channel, high 16 bits = right channel.
 * Lock-free.  If the buffer is full the oldest frame is silently overwritten.
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
