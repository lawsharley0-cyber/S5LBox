/*
 * S5LBox — VMAudioBuffer unit tests.
 *
 * Tests the thread-safe circular ring buffer connecting the emulator
 * producer thread and the host audio consumer thread.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "VMAudioBuffer.h"

#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <pthread.h>
#include <sched.h>
#endif

static int g_pass, g_fail;
#define CHECK(cond, ...) do { \
    if (cond) g_pass++; \
    else { \
        g_fail++; \
        printf("  FAIL %s:%d: ", __func__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

static uint32_t buffer_count(vm_audio_buffer_t *buf) {
    vm_audio_telemetry_t telemetry;
    vm_audio_buffer_telemetry(buf, &telemetry);
    return telemetry.count;
}

static void test_audio_buffer_init_and_push_read(void) {
    vm_audio_buffer_t buf;
    vm_audio_buffer_init(&buf);

    CHECK(buffer_count(&buf) == 0, "initial count is non-zero");
    CHECK(vm_audio_buffer_ready_for_more(&buf), "initial buffer not ready");

    /* Push 4 stereo frames: (Left, Right) */
    /* Frame 0: L=0x1234, R=0x5678 */
    vm_audio_buffer_push_word(&buf, 0x56781234u);
    /* Frame 1: L=0x0001, R=0x0002 */
    vm_audio_buffer_push_word(&buf, 0x00020001u);
    /* Frame 2: L=-10 (0xfff6), R=-20 (0xffec) */
    vm_audio_buffer_push_word(&buf, 0xffecfff6u);
    /* Frame 3: L=0, R=0 */
    vm_audio_buffer_push_word(&buf, 0x00000000u);

    CHECK(buffer_count(&buf) == 4, "buffer count = %u, expected 4", buffer_count(&buf));

    int16_t out[8];
    memset(out, 0xaa, sizeof out);
    uint32_t valid = vm_audio_buffer_read_frames(&buf, out, 4);

    CHECK(valid == 4, "read_frames returned %u valid frames, expected 4", valid);
    CHECK(buffer_count(&buf) == 0, "buffer count after read = %u, expected 0", buffer_count(&buf));

    CHECK(out[0] == (int16_t)0x1234 && out[1] == (int16_t)0x5678,
          "frame 0 mismatch: L=%d R=%d", out[0], out[1]);
    CHECK(out[2] == 1 && out[3] == 2,
          "frame 1 mismatch: L=%d R=%d", out[2], out[3]);
    CHECK(out[4] == -10 && out[5] == -20,
          "frame 2 mismatch: L=%d R=%d", out[4], out[5]);
    CHECK(out[6] == 0 && out[7] == 0,
          "frame 3 mismatch: L=%d R=%d", out[6], out[7]);

    vm_audio_telemetry_t telem;
    vm_audio_buffer_telemetry(&buf, &telem);
    CHECK(telem.produced == 4, "produced telemetry = %llu", (unsigned long long)telem.produced);
    CHECK(telem.consumed == 4, "consumed telemetry = %llu", (unsigned long long)telem.consumed);
    CHECK(telem.underflows == 0, "underflow count = %llu", (unsigned long long)telem.underflows);

    vm_audio_buffer_destroy(&buf);
}

static void test_audio_buffer_underflow_padding(void) {
    vm_audio_buffer_t buf;
    vm_audio_buffer_init(&buf);

    /* Push 2 frames */
    vm_audio_buffer_push_word(&buf, 0x00200010u);
    vm_audio_buffer_push_word(&buf, 0x00400030u);

    /* Request 6 frames (underflow by 4 frames) */
    int16_t out[12];
    memset(out, 0x55, sizeof out);
    uint32_t valid = vm_audio_buffer_read_frames(&buf, out, 6);

    CHECK(valid == 2, "valid frames = %u, expected 2", valid);
    CHECK(out[0] == 0x0010 && out[1] == 0x0020, "frame 0 corrupt");
    CHECK(out[2] == 0x0030 && out[3] == 0x0040, "frame 1 corrupt");

    /* Padded frames must be silent zeroes */
    for (unsigned i = 4; i < 12; i++) {
        CHECK(out[i] == 0, "underflow padding sample %u = %d, expected 0", i, out[i]);
    }

    vm_audio_telemetry_t telem;
    vm_audio_buffer_telemetry(&buf, &telem);
    CHECK(telem.underflows == 1, "underflow telemetry = %llu, expected 1",
          (unsigned long long)telem.underflows);

    vm_audio_buffer_destroy(&buf);
}

static void test_audio_buffer_overflow(void) {
    vm_audio_buffer_t buf;
    vm_audio_buffer_init(&buf);
    for (uint32_t i = 0; i < VM_AUDIO_BUFFER_CAPACITY_FRAMES - 1; ++i)
        vm_audio_buffer_push_word(&buf, i + 1);
    for (uint32_t i = 0; i < 100; ++i)
        vm_audio_buffer_push_word(&buf, 0xdeadbeef);
    CHECK(vm_audio_buffer_ready_for_more(&buf), "full host buffer must not block guest DMA");
    int16_t frame[2];
    for (uint32_t i = 0; i < VM_AUDIO_BUFFER_CAPACITY_FRAMES - 1; ++i) {
        CHECK(vm_audio_buffer_read_frames(&buf, frame, 1) == 1, "missing retained frame");
        uint32_t word = (uint16_t)frame[0] | ((uint32_t)(uint16_t)frame[1] << 16);
        CHECK(word == i + 1, "overflow corrupted retained audio");
    }
    vm_audio_telemetry_t telem;
    vm_audio_buffer_telemetry(&buf, &telem);
    CHECK(telem.overflows == 100, "incorrect dropped word count");
    CHECK(telem.count == 0, "queue did not drain");
    vm_audio_buffer_push_word(&buf, 123);
    CHECK(vm_audio_buffer_read_frames(&buf, frame, 1) == 1 && frame[0] == 123,
          "queue did not recover after overflow");
    vm_audio_buffer_destroy(&buf);
}

static void test_audio_buffer_wrap_around(void) {
    vm_audio_buffer_t buf;
    vm_audio_buffer_init(&buf);

    /* Push and pop chunks of 500 frames across 2 full cycles of capacity.
     * CHUNK must be a true compile-time constant (not just a const local) —
     * it sizes a stack array below, and MSVC has no C99 VLA support. */
    enum { CHUNK = 500u };
    const unsigned CYCLES = (VM_AUDIO_BUFFER_CAPACITY_FRAMES * 2u) / CHUNK;
    uint32_t sample_counter = 0;

    for (unsigned c = 0; c < CYCLES; c++) {
        for (unsigned i = 0; i < CHUNK; i++) {
            uint32_t w = sample_counter++;
            vm_audio_buffer_push_word(&buf, w);
        }
        int16_t out[CHUNK * 2];
        uint32_t valid = vm_audio_buffer_read_frames(&buf, out, CHUNK);
        CHECK(valid == CHUNK, "cycle %u: read %u valid, expected %u", c, valid, CHUNK);
    }

    CHECK(buffer_count(&buf) == 0, "residual count after wrap-around cycles: %u", buffer_count(&buf));
    vm_audio_telemetry_t telem;
    vm_audio_buffer_telemetry(&buf, &telem);
    CHECK(telem.produced == sample_counter, "produced = %llu, expected %u",
          (unsigned long long)telem.produced, sample_counter);
    CHECK(telem.consumed == sample_counter, "consumed = %llu, expected %u",
          (unsigned long long)telem.consumed, sample_counter);
    CHECK(telem.overflows == 0, "unexpected overflows: %llu",
          (unsigned long long)telem.overflows);

    vm_audio_buffer_destroy(&buf);
}

#ifndef _WIN32
/* Deliberately overflow while the callback reads. Accepted samples must stay
 * ordered and untorn; drops are allowed, duplicate or backwards words are not. */
static vm_audio_buffer_t concurrent_buffer;
static _Atomic bool producer_done;
static void *produce_audio(void *unused) {
    (void)unused;
    for (uint32_t i = 1; i <= 1000000; ++i)
        vm_audio_buffer_push_word(&concurrent_buffer, i);
    atomic_store_explicit(&producer_done, true, memory_order_release);
    return NULL;
}
static void test_audio_buffer_concurrent(void) {
    vm_audio_buffer_init(&concurrent_buffer);
    atomic_init(&producer_done, false);
    pthread_t producer;
    int err = pthread_create(&producer, NULL, produce_audio, NULL);
    CHECK(err == 0, "could not start audio producer");
    if (err) return;
    uint32_t previous = 0;
    bool ordered = true;
    for (;;) {
        int16_t frames[514];
        uint32_t n = vm_audio_buffer_read_frames(&concurrent_buffer, frames, 257);
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t word = (uint16_t)frames[2*i] | ((uint32_t)(uint16_t)frames[2*i+1] << 16);
            if (word <= previous || word > 1000000) ordered = false;
            previous = word;
        }
        if (atomic_load_explicit(&producer_done, memory_order_acquire)
            && buffer_count(&concurrent_buffer) == 0) break;
        sched_yield();
    }
    pthread_join(producer, NULL);
    vm_audio_telemetry_t t;
    vm_audio_buffer_telemetry(&concurrent_buffer, &t);
    CHECK(ordered, "concurrent audio duplicated, reordered, or tore a sample");
    CHECK(t.produced == t.consumed && t.produced + t.overflows == 1000000,
          "concurrent sample accounting did not balance");
    vm_audio_buffer_destroy(&concurrent_buffer);
}
#endif

int main(void) {
    printf("S5LBox VMAudioBuffer unit tests\n");
    test_audio_buffer_init_and_push_read();
    test_audio_buffer_underflow_padding();
    test_audio_buffer_overflow();
    test_audio_buffer_wrap_around();
#ifndef _WIN32
    test_audio_buffer_concurrent();
#endif
    printf("  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
