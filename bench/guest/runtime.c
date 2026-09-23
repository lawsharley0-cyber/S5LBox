/*
 * S5LBox — guest benchmark image: the freestanding runtime.
 *
 * The mailbox the harness reads and writes, the workload arena, and the few
 * library routines Clang emits calls to for ARMv6 (no hardware divide; Thumb-1
 * has no long multiply). Written plainly: they are part of the measured guest
 * code, the way libgcc/compiler-rt helpers are part of real iPhone OS code.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include <stddef.h>
#include <stdint.h>
#include "workloads.h"

struct mailbox {
    uint32_t id, scale, user, result, done;
};

__attribute__((section(".mbox"), used))
volatile struct mailbox mbox;

volatile uint32_t svc_counter;

__attribute__((aligned(4096)))
uint8_t wl_arena[WL_ARENA_BYTES];

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d < s) { while (n--) *d++ = *s++; }
    else       { d += n; s += n; while (n--) *--d = *--s; }
    return dst;
}

void *memset(void *dst, int v, size_t n) {
    uint8_t *d = dst;
    while (n--) *d++ = (uint8_t)v;
    return dst;
}

void __aeabi_memcpy(void *d, const void *s, size_t n)  { memcpy(d, s, n); }
void __aeabi_memcpy4(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void __aeabi_memcpy8(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void __aeabi_memmove(void *d, const void *s, size_t n) { memmove(d, s, n); }
void __aeabi_memset(void *d, size_t n, int v)  { memset(d, v, n); }
void __aeabi_memset4(void *d, size_t n, int v) { memset(d, v, n); }
void __aeabi_memclr(void *d, size_t n)  { memset(d, 0, n); }
void __aeabi_memclr4(void *d, size_t n) { memset(d, 0, n); }
void __aeabi_memclr8(void *d, size_t n) { memset(d, 0, n); }

/* Restoring shift-subtract division, one quotient bit per step. */
static uint32_t udiv32(uint32_t n, uint32_t d, uint32_t *rem) {
    uint32_t q = 0, r = 0;
    if (d == 0u) { *rem = n; return 0xffffffffu; }
    for (int i = 31; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= d) { r -= d; q |= 1u << i; }
    }
    *rem = r;
    return q;
}

uint32_t __aeabi_uidiv(uint32_t n, uint32_t d) {
    uint32_t r;
    return udiv32(n, d, &r);
}

/* AAPCS returns {quotient, remainder} in r0/r1: a uint64_t, low word first. */
uint64_t __aeabi_uidivmod(uint32_t n, uint32_t d) {
    uint32_t r, q = udiv32(n, d, &r);
    return (uint64_t)q | ((uint64_t)r << 32);
}

int32_t __aeabi_idiv(int32_t n, int32_t d) {
    uint32_t r;
    uint32_t un = n < 0 ? 0u - (uint32_t)n : (uint32_t)n;
    uint32_t ud = d < 0 ? 0u - (uint32_t)d : (uint32_t)d;
    uint32_t q = udiv32(un, ud, &r);
    return (int32_t)(((n < 0) != (d < 0)) ? 0u - q : q);
}

uint64_t __aeabi_idivmod(int32_t n, int32_t d) {
    uint32_t r;
    uint32_t un = n < 0 ? 0u - (uint32_t)n : (uint32_t)n;
    uint32_t ud = d < 0 ? 0u - (uint32_t)d : (uint32_t)d;
    uint32_t q = udiv32(un, ud, &r);
    if ((n < 0) != (d < 0)) q = 0u - q;
    if (n < 0) r = 0u - r;
    return (uint64_t)q | ((uint64_t)r << 32);
}

/* 64 x 64 -> 64 multiply from 16-bit pieces, so that compiling it can never
 * recurse into itself on Thumb-1. */
uint64_t __aeabi_lmul(uint64_t a, uint64_t b) {
    uint32_t al = (uint32_t)a, ah = (uint32_t)(a >> 32);
    uint32_t bl = (uint32_t)b, bh = (uint32_t)(b >> 32);
    uint32_t a0 = al & 0xffffu, a1 = al >> 16, b0 = bl & 0xffffu, b1 = bl >> 16;
    uint32_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint32_t mid = (p00 >> 16) + (p01 & 0xffffu) + (p10 & 0xffffu);
    uint32_t lo = (p00 & 0xffffu) | (mid << 16);
    uint32_t hi = p11 + (p01 >> 16) + (p10 >> 16) + (mid >> 16);
    hi += al * bh + ah * bl;
    return ((uint64_t)hi << 32) | lo;   /* constant shift: expanded inline */
}
