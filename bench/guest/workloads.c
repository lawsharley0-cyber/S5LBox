/*
 * S5LBox — guest CPU benchmark workloads. See workloads.h for the rules that
 * let this one file run as ARMv6 guest code and as host reference code.
 *
 * The shapes are chosen from what the real guest measurably spends time on
 * (docs/hotpath.md): big-number multiply-accumulate (_mulg_common), hashing
 * (_SHA1Init), span rasterising (CA::OGL::sw_scanline), and the ordinary
 * compiled-C mix of calls, branches, table dispatch and memory traffic.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "workloads.h"

#if defined(__arm__)
/* Guest: freestanding, so no <math.h>; with -fno-math-errno this is VSQRT. */
#  define WL_SQRTF(x) __builtin_sqrtf(x)
#else
#  include <math.h>
#  define WL_SQRTF(x) sqrtf(x)
#endif

#if defined(__GNUC__) || defined(__clang__)
#  define WL_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#  define WL_NOINLINE __declspec(noinline)
#else
#  define WL_NOINLINE
#endif

/* The guest image is built with -ffreestanding; these come from runtime.c
 * there and are ordinary static helpers here, so neither build depends on a
 * C library. */
static void wl_memcpy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if ((((uintptr_t)d | (uintptr_t)s) & 3u) == 0u) {
        while (n >= 16u) {
            uint32_t a = ((const uint32_t *)s)[0], b = ((const uint32_t *)s)[1];
            uint32_t c = ((const uint32_t *)s)[2], e = ((const uint32_t *)s)[3];
            ((uint32_t *)d)[0] = a; ((uint32_t *)d)[1] = b;
            ((uint32_t *)d)[2] = c; ((uint32_t *)d)[3] = e;
            d += 16; s += 16; n -= 16u;
        }
        while (n >= 4u) {
            *(uint32_t *)d = *(const uint32_t *)s;
            d += 4; s += 4; n -= 4u;
        }
    }
    while (n--) *d++ = *s++;
}

static void wl_memset(void *dst, uint8_t v, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    uint32_t w = v * 0x01010101u;
    while (n && ((uintptr_t)d & 3u)) { *d++ = v; n--; }
    while (n >= 4u) { *(uint32_t *)d = w; d += 4; n -= 4u; }
    while (n--) *d++ = v;
}

static uint32_t mix(uint32_t h, uint32_t v) {
    return (h ^ v) * 16777619u;
}

static uint32_t lcg(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

/* ------------------------------------------------------------- bignum --- */
#define BN_WORDS 32u   /* 1024-bit operands, like an RSA-1024 limb vector */

static WL_NOINLINE void bn_mul(uint32_t *r, const uint32_t *a,
                               const uint32_t *b, uint32_t n) {
    for (uint32_t i = 0; i < 2u * n; i++) r[i] = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t carry = 0;
        uint32_t ai = a[i];
        for (uint32_t j = 0; j < n; j++) {
            uint64_t t = (uint64_t)ai * b[j] + r[i + j] + carry;
            r[i + j] = (uint32_t)t;
            carry = t >> 32;
        }
        r[i + n] = (uint32_t)carry;
    }
}

static uint32_t wl_bignum(uint32_t scale, uint8_t *arena) {
    uint32_t *a = (uint32_t *)arena;
    uint32_t *b = a + BN_WORDS;
    uint32_t *r = b + BN_WORDS;
    uint32_t seed = 0x12345678u, h = 2166136261u;
    for (uint32_t i = 0; i < BN_WORDS; i++) { a[i] = lcg(&seed); b[i] = lcg(&seed); }
    for (uint32_t k = 0; k < scale; k++) {
        bn_mul(r, a, b, BN_WORDS);
        for (uint32_t i = 0; i < BN_WORDS; i++) a[i] ^= r[i + (k & 7u)];
        h = mix(h, r[BN_WORDS - 1u]);
        h = mix(h, r[2u * BN_WORDS - 1u]);
    }
    return h;
}

/* -------------------------------------------------------------- crc32 --- */
static uint32_t wl_crc32(uint32_t scale, uint8_t *arena) {
    uint32_t *table = (uint32_t *)arena;
    uint8_t *buf = arena + 1024u;
    const uint32_t len = 16384u;
    uint32_t seed = 0xc0ffeeu, h = 2166136261u;
    for (uint32_t i = 0; i < 256u; i++) {
        uint32_t c = i;
        for (uint32_t k = 0; k < 8u; k++)
            c = (c & 1u) ? 0xedb88320u ^ (c >> 1) : c >> 1;
        table[i] = c;
    }
    for (uint32_t i = 0; i < len; i++) buf[i] = (uint8_t)(lcg(&seed) >> 24);
    for (uint32_t k = 0; k < scale; k++) {
        uint32_t crc = 0xffffffffu;
        for (uint32_t i = 0; i < len; i++)
            crc = table[(crc ^ buf[i]) & 0xffu] ^ (crc >> 8);
        crc ^= 0xffffffffu;
        buf[k % len] ^= (uint8_t)crc;
        h = mix(h, crc);
    }
    return h;
}

/* --------------------------------------------------------------- sha1 --- */
static uint32_t rol(uint32_t x, uint32_t n) {
    return (x << n) | (x >> (32u - n));
}

static WL_NOINLINE void sha1_block(uint32_t st[5], const uint8_t *p) {
    uint32_t w[80];
    for (uint32_t i = 0; i < 16u; i++)
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
               ((uint32_t)p[4 * i + 2] << 8) | (uint32_t)p[4 * i + 3];
    for (uint32_t i = 16; i < 80u; i++)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4];
    for (uint32_t i = 0; i < 80u; i++) {
        uint32_t f, k;
        if (i < 20u)      { f = (b & c) | (~b & d);           k = 0x5a827999u; }
        else if (i < 40u) { f = b ^ c ^ d;                    k = 0x6ed9eba1u; }
        else if (i < 60u) { f = (b & c) | (b & d) | (c & d);  k = 0x8f1bbcdcu; }
        else              { f = b ^ c ^ d;                    k = 0xca62c1d6u; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e;
}

static uint32_t wl_sha1(uint32_t scale, uint8_t *arena) {
    uint8_t *buf = arena;
    const uint32_t blocks = 64u;
    uint32_t seed = 0x5a5a5a5au, h = 2166136261u;
    uint32_t st[5] = { 0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u };
    for (uint32_t i = 0; i < blocks * 64u; i++) buf[i] = (uint8_t)(lcg(&seed) >> 16);
    for (uint32_t k = 0; k < scale; k++) {
        for (uint32_t b = 0; b < blocks; b++) sha1_block(st, buf + 64u * b);
        h = mix(h, st[k % 5u]);
    }
    for (uint32_t i = 0; i < 5u; i++) h = mix(h, st[i]);
    return h;
}

/* ------------------------------------------------------------- memops --- */
typedef struct { uint32_t w[8]; } wl_blk_t;

static WL_NOINLINE uint32_t wl_strlen(const uint8_t *s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

static uint32_t wl_memops(uint32_t scale, uint8_t *arena) {
    uint8_t *src = arena, *dst = arena + (512u << 10);
    wl_blk_t *bs = (wl_blk_t *)(arena + (1024u << 10));
    wl_blk_t *bd = bs + 2048u;
    const uint32_t len = 65536u;
    uint32_t seed = 77u, h = 2166136261u;
    for (uint32_t i = 0; i < len; i++) src[i] = (uint8_t)(1u + (lcg(&seed) >> 25));
    src[len - 1u] = 0;
    for (uint32_t i = 0; i < 2048u; i++)
        for (uint32_t j = 0; j < 8u; j++) bs[i].w[j] = lcg(&seed);
    for (uint32_t k = 0; k < scale; k++) {
        uint32_t off = k & 15u;
        wl_memcpy(dst + off, src, len - 16u);          /* unaligned tail path */
        wl_memcpy(dst, src, len);                       /* word path */
        wl_memset(dst + 3u, (uint8_t)k, 4096u + off);
        h = mix(h, wl_strlen(dst + (k & 255u)));
        for (uint32_t i = 0; i < 2048u; i++) bd[i] = bs[(i + k) & 2047u];
        h = mix(h, bd[k & 2047u].w[k & 7u]);
        h = mix(h, dst[(k * 131u) & (len - 1u)]);
    }
    return h;
}

/* --------------------------------------------------------------- sort --- */
static void insertion_sort(uint32_t *v, int32_t lo, int32_t hi) {
    for (int32_t i = lo + 1; i <= hi; i++) {
        uint32_t x = v[i];
        int32_t j = i - 1;
        while (j >= lo && v[j] > x) { v[j + 1] = v[j]; j--; }
        v[j + 1] = x;
    }
}

static WL_NOINLINE void quick_sort(uint32_t *v, int32_t lo, int32_t hi) {
    while (hi - lo > 16) {
        int32_t mid = lo + (hi - lo) / 2;
        uint32_t a = v[lo], b = v[mid], c = v[hi];
        uint32_t pivot = a < b ? (b < c ? b : (a < c ? c : a))
                               : (a < c ? a : (b < c ? c : b));
        int32_t i = lo, j = hi;
        while (i <= j) {
            while (v[i] < pivot) i++;
            while (v[j] > pivot) j--;
            if (i <= j) { uint32_t t = v[i]; v[i] = v[j]; v[j] = t; i++; j--; }
        }
        if (j - lo < hi - i) { quick_sort(v, lo, j); lo = i; }
        else                 { quick_sort(v, i, hi); hi = j; }
    }
    insertion_sort(v, lo, hi);
}

static uint32_t wl_sort(uint32_t scale, uint8_t *arena) {
    uint32_t *v = (uint32_t *)arena;
    const uint32_t n = 4096u;
    uint32_t seed = 1u, h = 2166136261u;
    for (uint32_t k = 0; k < scale; k++) {
        for (uint32_t i = 0; i < n; i++) v[i] = lcg(&seed) >> (k & 7u);
        quick_sort(v, 0, (int32_t)n - 1);
        for (uint32_t i = 1; i < n; i++)
            if (v[i - 1] > v[i]) return 0xbad0bad0u;   /* not sorted */
        h = mix(h, v[k % n]);
        h = mix(h, v[n / 2u]);
    }
    return h;
}

/* ------------------------------------------------------------- raster --- */
#define FB_W 320u
#define FB_H 480u

static WL_NOINLINE void span_blend(uint32_t *row, uint32_t x0, uint32_t x1,
                                   uint32_t color, uint32_t alpha) {
    uint32_t src_rb = (color & 0x00ff00ffu) * alpha;
    uint32_t src_g  = (color & 0x0000ff00u) * alpha;
    uint32_t inv = 256u - alpha;
    for (uint32_t x = x0; x < x1; x++) {
        uint32_t d = row[x];
        uint32_t rb = ((src_rb + (d & 0x00ff00ffu) * inv) >> 8) & 0x00ff00ffu;
        uint32_t g  = ((src_g  + (d & 0x0000ff00u) * inv) >> 8) & 0x0000ff00u;
        row[x] = 0xff000000u | rb | g;
    }
}

static uint32_t wl_raster(uint32_t scale, uint8_t *arena) {
    uint32_t *fb = (uint32_t *)arena;
    uint32_t seed = 99u, h = 2166136261u;
    wl_memset(fb, 0, FB_W * FB_H * 4u);
    for (uint32_t k = 0; k < scale; k++) {
        /* A trapezoid with 16.16 fixed-point edges. */
        int32_t y0 = (int32_t)(lcg(&seed) % (FB_H - 64u));
        int32_t hgt = 16 + (int32_t)(lcg(&seed) % 48u);
        int32_t xl = (int32_t)(lcg(&seed) % 160u) << 16;
        int32_t xr = xl + ((32 + (int32_t)(lcg(&seed) % 128u)) << 16);
        int32_t dl = (int32_t)(lcg(&seed) % 65536u) - 32768;
        int32_t dr = (int32_t)(lcg(&seed) % 65536u) - 32768;
        uint32_t color = lcg(&seed), alpha = 32u + (lcg(&seed) & 191u);
        for (int32_t y = y0; y < y0 + hgt; y++) {
            int32_t a = xl >> 16, b = xr >> 16;
            if (a < 0) a = 0;
            if (b > (int32_t)FB_W) b = (int32_t)FB_W;
            if (a < b) span_blend(fb + (uint32_t)y * FB_W, (uint32_t)a, (uint32_t)b, color, alpha);
            xl += dl; xr += dr;
        }
        h = mix(h, fb[(uint32_t)y0 * FB_W + 80u]);
    }
    for (uint32_t i = 0; i < FB_W * FB_H; i += 61u) h = mix(h, fb[i]);
    return h;
}

/* -------------------------------------------------------------- calls --- */
static WL_NOINLINE uint32_t fib(uint32_t n) {
    return n < 2u ? n : fib(n - 1u) + fib(n - 2u);
}
static WL_NOINLINE uint32_t f_add(uint32_t a, uint32_t b) { return a + b; }
static WL_NOINLINE uint32_t f_sub(uint32_t a, uint32_t b) { return a - b; }
static WL_NOINLINE uint32_t f_xor(uint32_t a, uint32_t b) { return a ^ (b << 3); }
static WL_NOINLINE uint32_t f_mul(uint32_t a, uint32_t b) { return a * (b | 1u); }
static WL_NOINLINE uint32_t f_rot(uint32_t a, uint32_t b) { return rol(a, (b & 30u) + 1u); }
static WL_NOINLINE uint32_t f_min(uint32_t a, uint32_t b) { return a < b ? a : b; }
static WL_NOINLINE uint32_t f_max(uint32_t a, uint32_t b) { return a > b ? a : b; }
static WL_NOINLINE uint32_t f_avg(uint32_t a, uint32_t b) { return (a >> 1) + (b >> 1); }

typedef uint32_t (*wl_binop_t)(uint32_t, uint32_t);

static uint32_t wl_calls(uint32_t scale, uint8_t *arena) {
    static const wl_binop_t ops[8] = { f_add, f_sub, f_xor, f_mul,
                                       f_rot, f_min, f_max, f_avg };
    uint32_t seed = 4242u, acc = 1u, h = 2166136261u;
    (void)arena;
    for (uint32_t k = 0; k < scale; k++) {
        h = mix(h, fib(12u + (k & 3u)));
        for (uint32_t i = 0; i < 256u; i++) {
            uint32_t r = lcg(&seed);
            acc = ops[r >> 29](acc, r);
        }
        h = mix(h, acc);
    }
    return h;
}

/* ---------------------------------------------------------------- mmu --- */
static uint32_t wl_mmu(uint32_t scale, uint8_t *arena) {
    const uint32_t pages = WL_ARENA_BYTES / 4096u;
    uint32_t seed = 31337u, h = 2166136261u;
    /* A random single cycle through every page: next[p] lives in page p. */
    for (uint32_t p = 0; p < pages; p++) *(uint32_t *)(arena + p * 4096u) = p;
    for (uint32_t p = pages - 1u; p > 0; p--) {
        uint32_t q = lcg(&seed) % (p + 1u);
        uint32_t *a = (uint32_t *)(arena + p * 4096u), *b = (uint32_t *)(arena + q * 4096u);
        uint32_t t = *a; *a = *b; *b = t;
    }
    /* Convert the permutation into a successor table stored at word 1. */
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t cur = *(uint32_t *)(arena + i * 4096u);
        uint32_t nxt = *(uint32_t *)(arena + ((i + 1u) % pages) * 4096u);
        *(uint32_t *)(arena + cur * 4096u + 4u) = nxt;
    }
    for (uint32_t k = 0; k < scale; k++) {
        uint32_t off = 8u + ((k * 68u) & 4087u & ~3u);
        uint32_t sum = 0;
        for (uint32_t p = 0; p < pages; p++) {           /* strided walk */
            uint32_t *w = (uint32_t *)(arena + p * 4096u + off);
            sum += *w;
            *w = sum ^ p;
        }
        uint32_t p = k % pages;
        for (uint32_t i = 0; i < pages; i++)              /* pointer chase */
            p = *(uint32_t *)(arena + p * 4096u + 4u);
        h = mix(h, sum);
        h = mix(h, p);
    }
    return h;
}

/* ------------------------------------------------------------- interp --- */
enum { OP_PUSH, OP_ADD, OP_SUB, OP_MUL, OP_DUP, OP_SWAP, OP_DROP, OP_LD,
       OP_ST, OP_DEC, OP_JNZ, OP_XOR, OP_SHL, OP_SHR, OP_HALT };

static WL_NOINLINE uint32_t vm_run(const uint8_t *code, uint32_t *regs) {
    uint32_t stack[32];
    uint32_t sp = 0, pc = 0;
    for (;;) {
        uint8_t op = code[pc++];
        switch (op) {
            case OP_PUSH: stack[sp++] = code[pc++]; break;
            case OP_ADD:  sp--; stack[sp - 1] += stack[sp]; break;
            case OP_SUB:  sp--; stack[sp - 1] -= stack[sp]; break;
            case OP_MUL:  sp--; stack[sp - 1] *= stack[sp]; break;
            case OP_DUP:  stack[sp] = stack[sp - 1]; sp++; break;
            case OP_SWAP: { uint32_t t = stack[sp - 1]; stack[sp - 1] = stack[sp - 2]; stack[sp - 2] = t; break; }
            case OP_DROP: sp--; break;
            case OP_LD:   stack[sp++] = regs[code[pc++] & 7u]; break;
            case OP_ST:   regs[code[pc++] & 7u] = stack[--sp]; break;
            case OP_DEC:  stack[sp - 1] -= 1u; break;
            case OP_JNZ:  { uint8_t t = code[pc++]; if (stack[--sp]) pc = t; break; }
            case OP_XOR:  sp--; stack[sp - 1] ^= stack[sp]; break;
            case OP_SHL:  stack[sp - 1] <<= (code[pc++] & 31u); break;
            case OP_SHR:  stack[sp - 1] >>= (code[pc++] & 31u); break;
            default:      return regs[0];
        }
    }
}

static uint32_t wl_interp(uint32_t scale, uint8_t *arena) {
    /* r0 = sum of i*i ^ (acc << 1) for i = n..1 */
    static const uint8_t prog[] = {
        OP_LD, 1,                      /* 0: i                      */
        OP_DUP, OP_MUL,                /* 2: i*i                    */
        OP_LD, 0, OP_PUSH, 1, OP_SWAP, /* 4: acc                    */
        OP_DROP, OP_SHL, 1,            /* 9: acc<<1                 */
        OP_XOR,                        /* 12                        */
        OP_LD, 0, OP_ADD, OP_ST, 0,    /* 13: acc += ...            */
        OP_LD, 1, OP_DEC, OP_DUP, OP_ST, 1, /* 18: i--              */
        OP_JNZ, 0,                     /* 24                        */
        OP_HALT
    };
    uint32_t regs[8] = { 0 }, h = 2166136261u;
    (void)arena;
    for (uint32_t k = 0; k < scale; k++) {
        regs[0] = k; regs[1] = 200u + (k & 63u);
        h = mix(h, vm_run(prog, regs));
    }
    return h;
}

/* ---------------------------------------------------------------- vfp --- */
#if defined(__arm__) && defined(__thumb__) && !defined(__thumb2__)
/* Thumb-1 has no VFP encodings; the ARM image carries this workload. */
static uint32_t wl_vfp(uint32_t scale, uint8_t *arena) {
    (void)scale; (void)arena;
    return 0u;
}
#else
static WL_NOINLINE float vfp_dot(const float *a, const float *b, uint32_t n) {
    float s = 0.0f;
    for (uint32_t i = 0; i < n; i++) s += a[i] * b[i];
    return s;
}

static WL_NOINLINE void vfp_mat4(float *r, const float *a, const float *b) {
    for (uint32_t i = 0; i < 4u; i++)
        for (uint32_t j = 0; j < 4u; j++) {
            float s = 0.0f;
            for (uint32_t k = 0; k < 4u; k++) s += a[4 * i + k] * b[4 * k + j];
            r[4 * i + j] = s;
        }
}

static uint32_t wl_vfp(uint32_t scale, uint8_t *arena) {
    float *a = (float *)arena, *b = a + 256, *m = b + 256, *r = m + 16;
    uint32_t seed = 555u, h = 2166136261u;
    for (uint32_t i = 0; i < 256u; i++) {
        a[i] = (float)(int32_t)(lcg(&seed) >> 20) * 0.001953125f - 1.0f;
        b[i] = (float)(int32_t)(lcg(&seed) >> 20) * 0.00048828125f + 0.25f;
    }
    for (uint32_t i = 0; i < 16u; i++) m[i] = (float)(int32_t)(i * 3u + 1u) * 0.125f;
    /* Every value stays bounded: n >= |r[i]| keeps |m| <= 1.0625, and
     * n >= |d|/256 keeps |a| <= 256, so |d| <= 147456 and both conversions
     * below stay far inside int32. A float-to-int of an overflowed value is
     * undefined in C and differs between host SSE and guest VFP, so the
     * bounds are part of the benchmark's correctness, not decoration. */
    for (uint32_t k = 0; k < scale; k++) {
        float d = vfp_dot(a, b, 256u);
        vfp_mat4(r, m, m);
        float rr = 1.0f;
        for (uint32_t i = 0; i < 16u; i++) rr += r[i] * r[i];
        float n = WL_SQRTF(d * (d * 0.0000152587890625f) + rr);
        for (uint32_t i = 0; i < 16u; i++) m[i] = r[i] / n + 0.0625f;
        a[k & 255u] = d / (n + 2.0f);
        h = mix(h, (uint32_t)(int32_t)(d * 1024.0f));
        h = mix(h, (uint32_t)(int32_t)(n * 65536.0f));
    }
    return h;
}
#endif

/* ---------------------------------------------------------------- svc --- */
static WL_NOINLINE void do_svc(volatile uint32_t *svc_count) {
#if defined(__arm__)
    (void)svc_count;
    __asm__ volatile("svc #0" ::: "memory");
#else
    *svc_count += 1u;   /* the host's stand-in for the guest handler */
#endif
}

static uint32_t wl_svc(uint32_t scale, uint8_t *arena, volatile uint32_t *svc_count) {
    uint32_t h = 2166136261u, seed = 8u;
    (void)arena;
    for (uint32_t k = 0; k < scale; k++) {
        do_svc(svc_count);
        h = mix(h, *svc_count ^ lcg(&seed));
    }
    return h;
}

uint32_t wl_run(uint32_t id, uint32_t scale, uint8_t *arena,
                volatile uint32_t *svc_count) {
    switch (id) {
        case WL_BIGNUM: return wl_bignum(scale, arena);
        case WL_CRC32:  return wl_crc32(scale, arena);
        case WL_SHA1:   return wl_sha1(scale, arena);
        case WL_MEMOPS: return wl_memops(scale, arena);
        case WL_SORT:   return wl_sort(scale, arena);
        case WL_RASTER: return wl_raster(scale, arena);
        case WL_CALLS:  return wl_calls(scale, arena);
        case WL_MMU:    return wl_mmu(scale, arena);
        case WL_INTERP: return wl_interp(scale, arena);
        case WL_VFP:    return wl_vfp(scale, arena);
        case WL_SVC:    return wl_svc(scale, arena, svc_count);
        default:        return 0xdeadbeefu;
    }
}
