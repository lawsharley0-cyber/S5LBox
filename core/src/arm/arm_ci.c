/*
 * S5LBox — cached interpreter: block cache, invalidation, run loop, executor.
 * See core/include/arm_ci.h for the contract and docs/IMPLEMENTATION_PLAN.md
 * for the design and the evidence behind it.
 *
 * THE ONE RULE EVERY HANDLER FOLLOWS: a specialised handler either completes
 * its instruction on a fast path whose result is exactly the reference result,
 * or -- before changing any state -- jumps to `ref`, which runs the same
 * instruction through arm_exec_*_insn(). Misaligned accesses, translation
 * misses that cannot be cached, device memory, interworking corner cases and
 * faults therefore never need a second implementation here.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "arm_ci_priv.h"
#include "arm_internal.h"
#include "vfp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#  define CI_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define CI_UNLIKELY(x) __builtin_expect(!!(x), 0)
#  define CI_NOINLINE    __attribute__((noinline))
#  define CI_INLINE      static inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#  define CI_LIKELY(x)   (x)
#  define CI_UNLIKELY(x) (x)
#  define CI_NOINLINE    __declspec(noinline)
#  define CI_INLINE      static __forceinline
#else
#  define CI_LIKELY(x)   (x)
#  define CI_UNLIKELY(x) (x)
#  define CI_NOINLINE
#  define CI_INLINE      static inline
#endif

/* Sized from a real boot: iPhone OS 3 plus SpringBoard kept ~26 k blocks
 * live between flushes of the old 32 k pool and flushed it 924 times in
 * 8.8 G instructions. Buckets chain, so a colliding block is found rather
 * than rebuilt; the pools are allocated once and touched only as used. */
#define CI_HASH_BITS  17u
#define CI_HASH_SIZE  (1u << CI_HASH_BITS)
#define CI_MAX_BLOCKS 131072u
#define CI_MAX_OPS    (CI_MAX_BLOCKS * 8u)       /* 16 MiB of records */

#define CI_FLAG_C     ARM_CPSR_C
/* CPSR bits whose change needs the machine: mode, I/F/A masks, E. N/Z/C/V/Q,
 * GE and T are ordinary execution state. */
#define CI_CTRL_MASK  (ARM_CPSR_MODE_MASK | ARM_CPSR_I | ARM_CPSR_F | \
                       ARM_CPSR_A | ARM_CPSR_E)

/* ------------------------------------------------------------ creation --- */

arm_ci_t *arm_ci_create(const arm_ci_config_t *cfg) {
    if (!cfg || !cfg->ram || cfg->ram_size < 1024u) return NULL;
    arm_ci_t *ci = calloc(1, sizeof *ci);
    if (!ci) return NULL;
    ci->cfg = *cfg;
    ci->regions = cfg->ram_size >> 10;
    ci->region_gen = calloc(ci->regions, sizeof *ci->region_gen);
    ci->code_map = calloc((ci->regions + 31u) / 32u, sizeof *ci->code_map);
    ci->table = calloc(CI_HASH_SIZE, sizeof *ci->table);
    ci->blocks = malloc(CI_MAX_BLOCKS * sizeof *ci->blocks);
    ci->ops = malloc(CI_MAX_OPS * sizeof *ci->ops);
    if (!ci->region_gen || !ci->code_map || !ci->table || !ci->blocks || !ci->ops) {
        arm_ci_destroy(ci);
        return NULL;
    }
    return ci;
}

void arm_ci_destroy(arm_ci_t *ci) {
    if (!ci) return;
    free(ci->region_gen);
    free(ci->code_map);
    free(ci->table);
    free(ci->blocks);
    free(ci->ops);
    free(ci);
}

void arm_ci_set_verify(arm_ci_t *ci, bool on) { if (ci) ci->verify = on; }

void arm_ci_get_stats(const arm_ci_t *ci, arm_ci_stats_t *out) {
    if (!out) return;
    if (!ci) { memset(out, 0, sizeof *out); return; }
    *out = ci->st;
}

void arm_ci_reset_stats(arm_ci_t *ci) {
    if (ci) memset(&ci->st, 0, sizeof ci->st);
}

/* ----------------------------------------------------- code tracking --- */

/* Drop everything derived from guest translations: the host TLBs and the
 * VA-keyed block map. Blocks themselves are keyed by physical offset and
 * survive. */
static void purge_translations(arm_ci_t *ci) {
    memset(ci->rtlb, 0, sizeof ci->rtlb);
    memset(ci->wtlb, 0, sizeof ci->wtlb);
    memset(ci->fast, 0, sizeof ci->fast);
}

void arm_ci_flush(arm_ci_t *ci) {
    if (!ci) return;
    purge_translations(ci);
    memset(ci->table, 0, CI_HASH_SIZE * sizeof *ci->table);
    memset(ci->code_map, 0, ((ci->regions + 31u) / 32u) * sizeof *ci->code_map);
    ci->nblocks = 0;
    ci->nops = 0;
    ci->code_written = true;
    ci->st.flushes++;
}

static bool region_is_code(const arm_ci_t *ci, uint32_t r) {
    return (ci->code_map[r >> 5] >> (r & 31u)) & 1u;
}

bool arm_ci_range_has_code(const arm_ci_t *ci, uint32_t pa, uint32_t len) {
    if (!ci || !len) return false;
    uint64_t lo = pa, hi = (uint64_t)pa + len;
    uint64_t base = ci->cfg.ram_base, end = base + ci->cfg.ram_size;
    if (hi <= base || lo >= end) return false;
    if (lo < base) lo = base;
    if (hi > end) hi = end;
    for (uint64_t r = (lo - base) >> 10; r <= (hi - 1u - base) >> 10; r++)
        if (region_is_code(ci, (uint32_t)r)) return true;
    return false;
}

void arm_ci_note_ram_write(arm_ci_t *ci, uint32_t pa, uint32_t len) {
    if (!ci || !len) return;
    uint64_t lo = pa, hi = (uint64_t)pa + len;
    uint64_t base = ci->cfg.ram_base, end = base + ci->cfg.ram_size;
    if (hi <= base || lo >= end) return;
    if (lo < base) lo = base;
    if (hi > end) hi = end;
    for (uint64_t r64 = (lo - base) >> 10; r64 <= (hi - 1u - base) >> 10; r64++) {
        uint32_t r = (uint32_t)r64;
        if (!region_is_code(ci, r)) continue;
        ci->code_map[r >> 5] &= ~(1u << (r & 31u));
        ci->region_gen[r]++;
        ci->code_written = true;
        ci->st.invalidations++;
    }
}

/* A block is about to be cached from 1 KiB region r. Every host pointer that
 * could store into it without passing the bus must go: the engine's write TLB
 * and the interpreter's dwrite cache. The machine refuses new ones for as
 * long as the region is marked (arm_ci_range_has_code). */
static void mark_code(arm_ci_t *ci, arm_cpu_t *c, uint32_t r) {
    if (region_is_code(ci, r)) return;
    ci->code_map[r >> 5] |= 1u << (r & 31u);
    memset(ci->wtlb, 0, sizeof ci->wtlb);
    const uint8_t *lo = ci->cfg.ram + ((size_t)r << 10), *hi = lo + 1024u;
    for (unsigned i = 0; i < ARM_DREAD_ENTRIES; i++) {
        const uint8_t *h = c->dwrite[i].host;
        if (h && h >= lo && h < hi) c->dwrite[i].host = NULL;
    }
}

/* ---------------------------------------------------------- host TLB --- */

CI_INLINE unsigned tlb_slot(uint32_t va, bool priv) {
    return ((va >> 10) + (priv ? CI_TLB_ENTRIES / 2u : 0u)) & (CI_TLB_ENTRIES - 1u);
}
CI_INLINE uint32_t tlb_tag(uint32_t va, bool priv) {
    return (va & ~0x3ffu) | (priv ? 1u : 0u);
}

static CI_NOINLINE uint8_t *tlb_fill(arm_ci_t *ci, arm_cpu_t *c, uint32_t va,
                                     bool priv, bool write) {
    uint32_t pa = 0;
    ci->st.mem_slow++;
    if (arm_mmu_translate(c, va, write ? ARM_ACCESS_WRITE : ARM_ACCESS_READ,
                          priv, &pa) != 0u)
        return NULL;
    uint8_t *(*get)(void *, uint32_t, uint32_t) =
        write ? c->bus->host_ram_write : c->bus->host_ram;
    if (!get) return NULL;
    uint8_t *h = get(c->bus->ctx, pa & ~0x3ffu, 0x400u);
    if (!h) return NULL;
    ci_tlb_t *e = write ? &ci->wtlb[tlb_slot(va, priv)] : &ci->rtlb[tlb_slot(va, priv)];
    e->tag = tlb_tag(va, priv);
    e->gen = c->tlb_gen;           /* read after the walk: it may have flushed */
    e->host = h;
    return h + (va & 0x3ffu);
}

CI_INLINE uint8_t *mem_rd(arm_ci_t *ci, arm_cpu_t *c, uint32_t va, bool priv) {
    const ci_tlb_t *e = &ci->rtlb[tlb_slot(va, priv)];
    if (CI_LIKELY(e->tag == tlb_tag(va, priv) && e->gen == c->tlb_gen))
        return e->host + (va & 0x3ffu);
    return tlb_fill(ci, c, va, priv, false);
}

CI_INLINE uint8_t *mem_wr(arm_ci_t *ci, arm_cpu_t *c, uint32_t va, bool priv) {
    const ci_tlb_t *e = &ci->wtlb[tlb_slot(va, priv)];
    if (CI_LIKELY(e->tag == tlb_tag(va, priv) && e->gen == c->tlb_gen))
        return e->host + (va & 0x3ffu);
    return tlb_fill(ci, c, va, priv, true);
}

CI_INLINE uint32_t ld32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
CI_INLINE uint32_t ld16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
CI_INLINE void st32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
CI_INLINE void st16(uint8_t *p, uint32_t v) { uint16_t h = (uint16_t)v; memcpy(p, &h, 2); }

/* ------------------------------------------------------- ALU helpers --- */

/* Condition truth table: bit (NZCV) of ci_cond_table[cond] says whether the
 * condition passes for that flag nibble. Built from the same definitions as
 * arm_cond_passed(). */
static uint16_t g_cond_table[16];
static bool g_cond_ready;

static void cond_table_init(void) {
    for (unsigned cond = 0; cond < 16u; cond++) {
        uint16_t bits = 0;
        for (unsigned f = 0; f < 16u; f++) {
            bool N = f & 8u, Z = f & 4u, C = f & 2u, V = f & 1u, pass;
            switch (cond) {
                case 0x0: pass = Z; break;             case 0x1: pass = !Z; break;
                case 0x2: pass = C; break;             case 0x3: pass = !C; break;
                case 0x4: pass = N; break;             case 0x5: pass = !N; break;
                case 0x6: pass = V; break;             case 0x7: pass = !V; break;
                case 0x8: pass = C && !Z; break;       case 0x9: pass = !C || Z; break;
                case 0xa: pass = N == V; break;        case 0xb: pass = N != V; break;
                case 0xc: pass = !Z && N == V; break;  case 0xd: pass = Z || N != V; break;
                default:  pass = true; break;
            }
            if (pass) bits |= (uint16_t)(1u << f);
        }
        g_cond_table[cond] = bits;
    }
    g_cond_ready = true;
}

CI_INLINE bool cond_pass(uint32_t cpsr, unsigned cond) {
    return (g_cond_table[cond] >> (cpsr >> 28)) & 1u;
}

CI_INLINE uint32_t nz(uint32_t r) {
    return (r & 0x80000000u) | (r ? 0u : ARM_CPSR_Z);
}

CI_INLINE uint32_t carry(const arm_cpu_t *c) { return (c->cpsr >> 29) & 1u; }

/* N, Z, C from the shifter, V preserved. */
CI_INLINE void set_logic(arm_cpu_t *c, uint32_t r, uint32_t cy) {
    c->cpsr = (c->cpsr & 0x1fffffffu) | nz(r) | (cy << 29);
}

/* a + b + cin with all four flags, exactly alu_add(). Subtraction is
 * a + ~b + cin, exactly alu_sub(). */
CI_INLINE uint32_t add_flags(arm_cpu_t *c, uint32_t a, uint32_t b, uint32_t cin) {
    uint64_t u = (uint64_t)a + b + cin;
    uint32_t r = (uint32_t)u;
    uint32_t v = (~(a ^ b) & (a ^ r)) & 0x80000000u;
    c->cpsr = (c->cpsr & 0x0fffffffu) | nz(r) | ((uint32_t)(u >> 32) << 29) | (v >> 3);
    return r;
}

/* Immediate-shift operand (decoder-normalised), value only. */
CI_INLINE uint32_t shi_val(const ci_op_t *op, uint32_t v, uint32_t cpsr) {
    unsigned sa = op->sa;
    switch (op->sh & 7u) {
        case CI_SH_LSL: return v << sa;
        case CI_SH_LSR: return sa >= 32u ? 0u : v >> sa;
        case CI_SH_ASR: return (uint32_t)((int32_t)v >> (sa >= 32u ? 31u : sa));
        case CI_SH_ROR: return (v >> sa) | (v << (32u - sa));
        default:        return (v >> 1) | ((cpsr & CI_FLAG_C) << 2);   /* RRX */
    }
}

/* ...with the shifter carry-out. sa is 1..32 here (0 is the REG form). */
CI_INLINE uint32_t shi_carry(const ci_op_t *op, uint32_t v, uint32_t cpsr, uint32_t *cy) {
    unsigned sa = op->sa;
    switch (op->sh & 7u) {
        case CI_SH_LSL: *cy = (v >> (32u - sa)) & 1u; return v << sa;
        case CI_SH_LSR: *cy = (v >> (sa - 1u)) & 1u; return sa >= 32u ? 0u : v >> sa;
        case CI_SH_ASR: *cy = (v >> (sa - 1u)) & 1u;
                        return (uint32_t)((int32_t)v >> (sa >= 32u ? 31u : sa));
        case CI_SH_ROR: *cy = (v >> (sa - 1u)) & 1u; return (v >> sa) | (v << (32u - sa));
        default:        *cy = v & 1u; return (v >> 1) | ((cpsr & CI_FLAG_C) << 2);
    }
}

/* Register-specified shift, exactly barrel_shift(..., reg_amount = true).
 * *cy must hold the current C on entry (the "unaffected" cases keep it). */
CI_INLINE uint32_t shr_carry(unsigned type, uint32_t v, uint32_t amt, uint32_t *cy) {
    if (amt == 0u) return v;
    switch (type & 3u) {
        case 0:
            if (amt < 32u) { *cy = (v >> (32u - amt)) & 1u; return v << amt; }
            *cy = amt == 32u ? (v & 1u) : 0u; return 0u;
        case 1:
            if (amt < 32u) { *cy = (v >> (amt - 1u)) & 1u; return v >> amt; }
            *cy = amt == 32u ? (v >> 31) : 0u; return 0u;
        case 2:
            if (amt < 32u) { *cy = (v >> (amt - 1u)) & 1u; return (uint32_t)((int32_t)v >> amt); }
            *cy = v >> 31; return (v >> 31) ? 0xffffffffu : 0u;
        default:
            amt &= 31u;
            if (amt == 0u) { *cy = v >> 31; return v; }
            *cy = (v >> (amt - 1u)) & 1u;
            return (v >> amt) | (v << (32u - amt));
    }
}

CI_INLINE uint32_t shr_val(unsigned type, uint32_t v, uint32_t amt) {
    uint32_t cy = 0;
    return shr_carry(type, v, amt, &cy);
}

/* Register memory offset: optional immediate shift, then the U sign. */
CI_INLINE uint32_t mem_reg_off(const ci_op_t *op, uint32_t v, uint32_t cpsr) {
    uint32_t o = op->sa ? shi_val(op, v, cpsr) : v;
    return (op->sh & CI_SH_SUB) ? 0u - o : o;
}

CI_INLINE uint32_t ror32(uint32_t v, unsigned n) {
    n &= 31u;
    return n ? (v >> n) | (v << (32u - n)) : v;
}

static uint32_t clz32(uint32_t v) {
#if defined(__GNUC__) || defined(__clang__)
    return v ? (uint32_t)__builtin_clz(v) : 32u;
#else
    uint32_t n = 0;
    if (!v) return 32u;
    while (!(v & 0x80000000u)) { v <<= 1; n++; }
    return n;
#endif
}

CI_INLINE unsigned ctz32(uint32_t v) {          /* v != 0 */
#if defined(__GNUC__) || defined(__clang__)
    return (unsigned)__builtin_ctz(v);
#else
    unsigned n = 0;
    while (!(v & 1u)) { v >>= 1; n++; }
    return n;
#endif
}

/* ------------------------------------------------------------- fetch --- */

/* Exactly arm_step()'s fetch: the same 1 KiB fetch cache, the same refill.
 * NULL means "let arm_step do it" (fault -> prefetch abort, or not DRAM). */
static const uint8_t *fetch(arm_cpu_t *c, uint32_t pc, bool priv) {
    const uint32_t blk = pc & ~0x3ffu;
    if (c->fetch_host && c->fetch_blk == blk && c->fetch_gen == c->tlb_gen &&
        c->fetch_priv == priv)
        return c->fetch_host + (pc - blk);
    uint32_t pa = 0;
    if (arm_mmu_translate(c, pc, ARM_ACCESS_FETCH, priv, &pa) != 0u) return NULL;
    if (!c->bus->host_ram) return NULL;
    uint8_t *h = c->bus->host_ram(c->bus->ctx, pa & ~0x3ffu, 0x400u);
    if (!h) return NULL;
    c->fetch_host = h;
    c->fetch_blk  = blk;
    c->fetch_gen  = c->tlb_gen;
    c->fetch_priv = priv;
    return h + (pc - blk);
}

/* ------------------------------------------------------------ blocks --- */

CI_INLINE uint32_t hash_slot(uint32_t pa_off, bool thumb) {
    uint32_t h = (pa_off >> 1) ^ (pa_off >> (CI_HASH_BITS + 1u)) ^ (thumb ? 0x5555u : 0u);
    return h & (CI_HASH_SIZE - 1u);
}

static ci_block_t *build(arm_ci_t *ci, arm_cpu_t *c, const uint8_t *host,
                         uint32_t pc, uint32_t pa_off, bool thumb) {
    if (ci->nblocks >= CI_MAX_BLOCKS || ci->nops + CI_BLOCK_MAX_OPS > CI_MAX_OPS)
        arm_ci_flush(ci);
    ci_block_t *b = &ci->blocks[ci->nblocks];
    ci_op_t *ops = &ci->ops[ci->nops];
    const unsigned isz = thumb ? 2u : 4u;
    const uint32_t room = 0x400u - (pa_off & 0x3ffu);
    unsigned n = 0;
    uint8_t stop_cause = ARM_CI_STEP_OTHER;
    for (uint32_t off = 0; n < CI_BLOCK_MAX_OPS && off + isz <= room; off += isz) {
        const uint8_t *p = host + off;
        const uint32_t word = thumb
            ? (uint32_t)(p[0] | (p[1] << 8))
            : (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
              ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        ci_dec_t d = thumb ? ci_decode_thumb(pc + off, (uint16_t)word, &ops[n])
                           : ci_decode_arm(pc + off, word, &ops[n]);
        if (d == CI_DEC_STOP) {
            stop_cause = (uint8_t)ci_stop_cause(word, thumb);
            break;
        }
        /* A reference record uses only kind and raw; its class rides in sa
         * for the statistics. */
        if (ops[n].kind == CI_K_REF || ops[n].kind == CI_K_VFP)
            ops[n].sa = (uint8_t)ci_ref_class(word, thumb);
        n++;
        if (d == CI_DEC_END) break;
    }
    const uint32_t r = pa_off >> 10;
    mark_code(ci, c, r);
    b->va = pc;
    b->pa_off = pa_off;
    b->gen = ci->region_gen[r];
    b->n = (uint16_t)n;
    b->thumb = thumb ? 1u : 0u;
    b->stop_cause = stop_cause;
    b->ops = ops;
    ci->nblocks++;
    ci->nops += n;
    /* Newest first: a rebuilt (stale-generation) block shadows its old copy,
     * which stays unreachable in the chain until the next flush. */
    const uint32_t slot = hash_slot(pa_off, thumb);
    b->next = ci->table[slot];
    ci->table[slot] = b;
    ci->st.builds++;
    ci->st.build_ops += n;
    return b;
}

/* Verify mode: the block's instruction words must still be RAM's. */
static bool block_matches_ram(const ci_block_t *b, const uint8_t *host) {
    const unsigned isz = b->thumb ? 2u : 4u;
    for (unsigned i = 0; i < b->n; i++) {
        const uint8_t *p = host + i * isz;
        uint32_t w = b->thumb ? (uint32_t)(p[0] | (p[1] << 8))
                              : (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                                ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        if (w != b->ops[i].raw) return false;
    }
    return true;
}

/* ---------------------------------------------------------- executor --- */

typedef enum { EXEC_CONTINUE, EXEC_STOP } exec_result_t;

/*
 * Dispatch. With GCC and Clang every handler ends in its own copy of the
 * dispatch -- condition check, then a computed jump through k_dispatch --
 * so the host predicts each guest op's successor from where it is, not from
 * one shared indirect jump. Elsewhere (MSVC) the same handlers are the cases
 * of a switch. Both forms run the same handler bodies; the choice is
 * measured in docs/BENCHMARK_RESULTS.md. S5LBOX_CI_SWITCH_DISPATCH forces the
 * switch so it can be tested on any compiler.
 */
#if (defined(__GNUC__) || defined(__clang__)) && !defined(S5LBOX_CI_SWITCH_DISPATCH)
#  define CI_THREADED 1
#else
#  define CI_THREADED 0
#endif

#if CI_THREADED
#  define CI_H(label, kind)  case kind: L_##label:
#  define CI_DISPATCH()                                                       \
        do {                                                                  \
            if (op->cond != CI_COND_AL && !cond_pass(c->cpsr, op->cond))      \
                goto next;                                                    \
            goto *k_dispatch[op->kind];                                       \
        } while (0)
#  define CI_NEXT()                                                           \
        do {                                                                  \
            if (++op == end) { next_pc = PC_OF(op); goto out; }               \
            CI_DISPATCH();                                                    \
        } while (0)
#else
#  define CI_H(label, kind)  case kind:
#  define CI_DISPATCH()      continue
#  define CI_NEXT()          goto next
#endif
#define CI_HK(name) CI_H(name, CI_K_##name)

/* Every specialised kind with a handler of its own, for the dispatch table. */
#define CI_SIMPLE_KINDS(X)                                                    \
    X(MUL) X(MULS) X(MLA) X(MLAS) X(UMULL) X(UMLAL) X(SMULL) X(SMLAL)         \
    X(NOP) X(CLREX) X(MRS_CPSR) X(CLZ) X(SXTB) X(SXTH) X(UXTB) X(UXTH)        \
    X(REV) X(REV16) X(REVSH) X(LDR_LIT) X(LDM) X(LDM_PC) X(STM)               \
    X(MRC_TID) X(MCR_TID) X(LDR_PC) X(JMP)                                    \
    X(VFP_DP) X(VFP_MOV) X(VFP_SYS) X(VFP_LS)                                 \
    X(B) X(BL) X(TBL2) X(BX) X(BLX_R) X(BLX_I)

/* The data-processing and memory families, shared by the handlers and the
 * table: each form expands the per-opcode / per-access macro below. */
#define DP_FORM(F, B, BC)                                                       \
        LOGIC(0,  F, B, BC, a & b2)                                            \
        LOGIC(1,  F, B, BC, a ^ b2)                                            \
        ARITH(2,  F, B, a - b2,             a,  ~b2, 1u)                       \
        ARITH(3,  F, B, b2 - a,             b2, ~a,  1u)                       \
        ARITH(4,  F, B, a + b2,             a,  b2,  0u)                       \
        ARITH(5,  F, B, a + b2 + carry(c),  a,  b2,  carry(c))                 \
        ARITH(6,  F, B, a + ~b2 + carry(c), a,  ~b2, carry(c))                 \
        ARITH(7,  F, B, b2 + ~a + carry(c), b2, ~a,  carry(c))                 \
        TESTOP(8, F, BC, a & b2)                                               \
        TESTOP(9, F, BC, a ^ b2)                                               \
        CMPOP(10, F, B, a, ~b2, 1u)                                            \
        CMPOP(11, F, B, a, b2,  0u)                                            \
        LOGIC(12, F, B, BC, a | b2)                                            \
        LOGIC(13, F, B, BC, b2)                                                \
        LOGIC(14, F, B, BC, a & ~b2)                                           \
        LOGIC(15, F, B, BC, ~b2)
#define DP_ALL_FORMS                                                            \
        DP_FORM(CI_F_IMM, B_I, BC_I)                                           \
        DP_FORM(CI_F_REG, B_R, BC_R)                                           \
        DP_FORM(CI_F_SHI, B_H, BC_H)                                           \
        DP_FORM(CI_F_SHR, B_X, BC_X)
#define MEM_MODES(SRC, OFF)                                                     \
        LOAD(CI_A_LDR,   SRC, CI_M_OFF,  ADDR_OFF,  OFF, WB_OFF,  3u, ld32(h))  \
        LOAD(CI_A_LDR,   SRC, CI_M_PRE,  ADDR_PRE,  OFF, WB_PRE,  3u, ld32(h))  \
        LOAD(CI_A_LDR,   SRC, CI_M_POST, ADDR_POST, OFF, WB_POST, 3u, ld32(h))  \
        LOAD(CI_A_LDRB,  SRC, CI_M_OFF,  ADDR_OFF,  OFF, WB_OFF,  0u, h[0])     \
        LOAD(CI_A_LDRB,  SRC, CI_M_PRE,  ADDR_PRE,  OFF, WB_PRE,  0u, h[0])     \
        LOAD(CI_A_LDRB,  SRC, CI_M_POST, ADDR_POST, OFF, WB_POST, 0u, h[0])     \
        LOAD(CI_A_LDRH,  SRC, CI_M_OFF,  ADDR_OFF,  OFF, WB_OFF,  1u, ld16(h))  \
        LOAD(CI_A_LDRH,  SRC, CI_M_PRE,  ADDR_PRE,  OFF, WB_PRE,  1u, ld16(h))  \
        LOAD(CI_A_LDRH,  SRC, CI_M_POST, ADDR_POST, OFF, WB_POST, 1u, ld16(h))  \
        LOAD(CI_A_LDRSB, SRC, CI_M_OFF,  ADDR_OFF,  OFF, WB_OFF,  0u, (uint32_t)(int32_t)(int8_t)h[0])  \
        LOAD(CI_A_LDRSB, SRC, CI_M_PRE,  ADDR_PRE,  OFF, WB_PRE,  0u, (uint32_t)(int32_t)(int8_t)h[0])  \
        LOAD(CI_A_LDRSB, SRC, CI_M_POST, ADDR_POST, OFF, WB_POST, 0u, (uint32_t)(int32_t)(int8_t)h[0])  \
        LOAD(CI_A_LDRSH, SRC, CI_M_OFF,  ADDR_OFF,  OFF, WB_OFF,  1u, (uint32_t)(int32_t)(int16_t)ld16(h)) \
        LOAD(CI_A_LDRSH, SRC, CI_M_PRE,  ADDR_PRE,  OFF, WB_PRE,  1u, (uint32_t)(int32_t)(int16_t)ld16(h)) \
        LOAD(CI_A_LDRSH, SRC, CI_M_POST, ADDR_POST, OFF, WB_POST, 1u, (uint32_t)(int32_t)(int16_t)ld16(h)) \
        STORE(CI_A_STR,  SRC, CI_M_OFF,  ADDR_OFF,  OFF, WB_OFF,  3u, st32(h, R[op->rd]))  \
        STORE(CI_A_STR,  SRC, CI_M_PRE,  ADDR_PRE,  OFF, WB_PRE,  3u, st32(h, R[op->rd]))  \
        STORE(CI_A_STR,  SRC, CI_M_POST, ADDR_POST, OFF, WB_POST, 3u, st32(h, R[op->rd]))  \
        STORE(CI_A_STRB, SRC, CI_M_OFF,  ADDR_OFF,  OFF, WB_OFF,  0u, h[0] = (uint8_t)R[op->rd]) \
        STORE(CI_A_STRB, SRC, CI_M_PRE,  ADDR_PRE,  OFF, WB_PRE,  0u, h[0] = (uint8_t)R[op->rd]) \
        STORE(CI_A_STRB, SRC, CI_M_POST, ADDR_POST, OFF, WB_POST, 0u, h[0] = (uint8_t)R[op->rd]) \
        STORE(CI_A_STRH, SRC, CI_M_OFF,  ADDR_OFF,  OFF, WB_OFF,  1u, st16(h, R[op->rd]))  \
        STORE(CI_A_STRH, SRC, CI_M_PRE,  ADDR_PRE,  OFF, WB_PRE,  1u, st16(h, R[op->rd]))  \
        STORE(CI_A_STRH, SRC, CI_M_POST, ADDR_POST, OFF, WB_POST, 1u, st16(h, R[op->rd]))
#define MEM_ALL_MODES                                                           \
        MEM_MODES(CI_S_IMM, OFF_IMM)                                           \
        MEM_MODES(CI_S_REG, OFF_REG)

#if CI_THREADED
/* Labels as values, and a table whose unlisted kinds default to the
 * reference: GNU extensions, used only in this compiler branch. */
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wpedantic"
#  if defined(__clang__)
#    pragma GCC diagnostic ignored "-Winitializer-overrides"
#  else
#    pragma GCC diagnostic ignored "-Woverride-init"
#  endif
#endif

/*
 * Run ops of one block, retiring at most `budget`. Returns the number
 * retired. EXEC_CONTINUE: control reached a new PC through an ordinary path
 * (r15 written) and the caller may look up the next block.
 */
static exec_result_t exec_block(arm_ci_t *ci, arm_cpu_t *c, const ci_block_t *b,
                                unsigned budget, bool priv, unsigned *retired_out,
                                arm_ci_stop_t *stop, arm_status_t *status) {
    uint32_t *const R = c->r;
    const ci_op_t *const base = b->ops;
    const ci_op_t *op = base;
    const ci_op_t *const end = base + (b->n < budget ? b->n : budget);
    const ci_op_t *flushed = base;         /* cycles accounted up to here */
    const unsigned shift = b->thumb ? 1u : 2u;
    uint32_t next_pc = 0;
    exec_result_t result = EXEC_CONTINUE;

#define PC_OF(o) (b->va + ((uint32_t)((o) - base) << shift))

#if CI_THREADED
#define LOGIC(OPC, F, B, BC, EXPR)                                              \
        [CI_DP_KIND(OPC, F, 0)] = &&L_DP_##OPC##_##F##_0,                       \
        [CI_DP_KIND(OPC, F, 1)] = &&L_DP_##OPC##_##F##_1,
#define ARITH(OPC, F, B, VAL, X, Y, CIN) LOGIC(OPC, F, B, B, VAL)
#define TESTOP(OPC, F, BC, EXPR) [CI_DP_KIND(OPC, F, 1)] = &&L_DP_##OPC##_##F##_1,
#define CMPOP(OPC, F, B, X, Y, CIN) TESTOP(OPC, F, B, X)
#define LOAD(ACC, SRC, MODE, ADDR, OFF, WB, ALIGN, READ)                        \
        [CI_MEM_KIND(ACC, SRC, MODE)] = &&L_M_##ACC##_##SRC##_##MODE,
#define STORE LOAD
#define X(name) [CI_K_##name] = &&L_##name,
    static const void *const k_dispatch[256] = {
        [0 ... 255] = &&L_REF,
        CI_SIMPLE_KINDS(X)
        DP_ALL_FORMS
        MEM_ALL_MODES
    };
#undef X
#undef STORE
#undef LOAD
#undef CMPOP
#undef TESTOP
#undef ARITH
#undef LOGIC
#endif

    for (;;) {
        if (op->cond != CI_COND_AL && !cond_pass(c->cpsr, op->cond)) goto next;

        switch (op->kind) {

        /* ---------------------------------------------- data processing */
#define B_I          (op->imm)
#define B_R          (R[op->rm])
#define B_H          shi_val(op, R[op->rm], c->cpsr)
#define B_X          shr_val(op->sh, R[op->rm], R[op->rs] & 0xffu)
#define BC_I(cy)     ((cy) = op->sa ? op->sh : carry(c), op->imm)
#define BC_R(cy)     ((cy) = carry(c), R[op->rm])
#define BC_H(cy)     shi_carry(op, R[op->rm], c->cpsr, &(cy))
#define BC_X(cy)     ((cy) = carry(c), shr_carry(op->sh, R[op->rm], R[op->rs] & 0xffu, &(cy)))

#define LOGIC(OPC, F, B, BC, EXPR)                                              \
        CI_H(DP_##OPC##_##F##_0, CI_DP_KIND(OPC, F, 0)) {                       \
            uint32_t a = R[op->rn], b2 = (B); (void)a;                         \
            R[op->rd] = (EXPR);                                                \
            CI_NEXT();                                                         \
        }                                                                      \
        CI_H(DP_##OPC##_##F##_1, CI_DP_KIND(OPC, F, 1)) {                       \
            uint32_t cy, b2 = BC(cy), a = R[op->rn]; (void)a;                  \
            uint32_t r = (EXPR);                                               \
            R[op->rd] = r;                                                     \
            set_logic(c, r, cy);                                               \
            CI_NEXT();                                                         \
        }
#define TESTOP(OPC, F, BC, EXPR)                                                \
        CI_H(DP_##OPC##_##F##_1, CI_DP_KIND(OPC, F, 1)) {                       \
            uint32_t cy, b2 = BC(cy), a = R[op->rn];                           \
            set_logic(c, (EXPR), cy);                                          \
            CI_NEXT();                                                         \
        }
#define ARITH(OPC, F, B, VAL, X, Y, CIN)                                        \
        CI_H(DP_##OPC##_##F##_0, CI_DP_KIND(OPC, F, 0)) {                       \
            uint32_t a = R[op->rn], b2 = (B);                                  \
            R[op->rd] = (VAL);                                                 \
            CI_NEXT();                                                         \
        }                                                                      \
        CI_H(DP_##OPC##_##F##_1, CI_DP_KIND(OPC, F, 1)) {                       \
            uint32_t a = R[op->rn], b2 = (B);                                  \
            R[op->rd] = add_flags(c, (X), (Y), (CIN));                         \
            CI_NEXT();                                                         \
        }
#define CMPOP(OPC, F, B, X, Y, CIN)                                             \
        CI_H(DP_##OPC##_##F##_1, CI_DP_KIND(OPC, F, 1)) {                       \
            uint32_t a = R[op->rn], b2 = (B);                                  \
            (void)add_flags(c, (X), (Y), (CIN));                               \
            CI_NEXT();                                                         \
        }
        DP_ALL_FORMS

#undef CMPOP
#undef ARITH
#undef TESTOP
#undef LOGIC

        /* --------------------------------------------------- multiplies */
        CI_HK(MUL)  R[op->rd] = R[op->rm] * R[op->rs]; CI_NEXT();
        CI_HK(MULS) {
            uint32_t r = R[op->rm] * R[op->rs];
            R[op->rd] = r;
            c->cpsr = (c->cpsr & 0x3fffffffu) | nz(r);
            CI_NEXT();
        }
        CI_HK(MLA)  R[op->rd] = R[op->rm] * R[op->rs] + R[op->rn]; CI_NEXT();
        CI_HK(MLAS) {
            uint32_t r = R[op->rm] * R[op->rs] + R[op->rn];
            R[op->rd] = r;
            c->cpsr = (c->cpsr & 0x3fffffffu) | nz(r);
            CI_NEXT();
        }
        CI_HK(UMULL) {
            uint64_t u = (uint64_t)R[op->rm] * R[op->rs];
            R[op->rn] = (uint32_t)u; R[op->rd] = (uint32_t)(u >> 32);
            CI_NEXT();
        }
        CI_HK(UMLAL) {
            uint64_t u = (uint64_t)R[op->rm] * R[op->rs];
            u += ((uint64_t)R[op->rd] << 32) | R[op->rn];
            R[op->rn] = (uint32_t)u; R[op->rd] = (uint32_t)(u >> 32);
            CI_NEXT();
        }
        CI_HK(SMULL) {
            uint64_t u = (uint64_t)((int64_t)(int32_t)R[op->rm] * (int64_t)(int32_t)R[op->rs]);
            R[op->rn] = (uint32_t)u; R[op->rd] = (uint32_t)(u >> 32);
            CI_NEXT();
        }
        CI_HK(SMLAL) {
            uint64_t u = (uint64_t)((int64_t)(int32_t)R[op->rm] * (int64_t)(int32_t)R[op->rs]);
            u += ((uint64_t)R[op->rd] << 32) | R[op->rn];
            R[op->rn] = (uint32_t)u; R[op->rd] = (uint32_t)(u >> 32);
            CI_NEXT();
        }

        /* ------------------------------------------------ miscellaneous */
        CI_HK(NOP) CI_NEXT();
        CI_HK(CLREX) c->excl_valid = false; CI_NEXT();
        CI_HK(MRS_CPSR) R[op->rd] = c->cpsr; CI_NEXT();
        CI_HK(CLZ) R[op->rd] = clz32(R[op->rm]); CI_NEXT();
        CI_HK(SXTB) R[op->rd] = (uint32_t)(int32_t)(int8_t)(uint8_t)ror32(R[op->rm], op->sa); CI_NEXT();
        CI_HK(SXTH) R[op->rd] = (uint32_t)(int32_t)(int16_t)(uint16_t)ror32(R[op->rm], op->sa); CI_NEXT();
        CI_HK(UXTB) R[op->rd] = ror32(R[op->rm], op->sa) & 0xffu; CI_NEXT();
        CI_HK(UXTH) R[op->rd] = ror32(R[op->rm], op->sa) & 0xffffu; CI_NEXT();
        CI_HK(REV) {
            uint32_t v = R[op->rm];
            R[op->rd] = (v << 24) | ((v & 0xff00u) << 8) | ((v >> 8) & 0xff00u) | (v >> 24);
            CI_NEXT();
        }
        CI_HK(REV16) {
            uint32_t v = R[op->rm];
            R[op->rd] = ((v & 0x00ff00ffu) << 8) | ((v >> 8) & 0x00ff00ffu);
            CI_NEXT();
        }
        CI_HK(REVSH) {
            uint32_t v = R[op->rm];
            uint16_t h = (uint16_t)(((v & 0xffu) << 8) | ((v >> 8) & 0xffu));
            R[op->rd] = (uint32_t)(int32_t)(int16_t)h;
            CI_NEXT();
        }

        /* ------------------------------------------------------- memory */
        CI_HK(LDR_LIT) {
            uint32_t a = op->imm;
            const uint8_t *h;
            if ((a & 3u) || !(h = mem_rd(ci, c, a, priv))) goto ref;
            R[op->rd] = ld32(h);
            CI_NEXT();
        }

#define ADDR_OFF(OFF)   uint32_t a = R[op->rn] + (OFF), wb = 0; (void)wb
#define ADDR_PRE(OFF)   uint32_t a = R[op->rn] + (OFF), wb = a
#define ADDR_POST(OFF)  uint32_t a = R[op->rn], wb = a + (OFF)
#define WB_OFF          (void)0
#define WB_PRE          R[op->rn] = wb
#define WB_POST         R[op->rn] = wb
#define OFF_IMM         (op->imm)
#define OFF_REG         mem_reg_off(op, R[op->rm], c->cpsr)

#define LOAD(ACC, SRC, MODE, ADDR, OFF, WB, ALIGN, READ)                        \
        CI_H(M_##ACC##_##SRC##_##MODE, CI_MEM_KIND(ACC, SRC, MODE)) {          \
            ADDR(OFF);                                                         \
            const uint8_t *h;                                                  \
            if ((a & (ALIGN)) || !(h = mem_rd(ci, c, a, priv))) goto ref;      \
            R[op->rd] = (READ);                                                \
            WB;                                                                \
            CI_NEXT();                                                         \
        }
#define STORE(ACC, SRC, MODE, ADDR, OFF, WB, ALIGN, WRITE)                      \
        CI_H(M_##ACC##_##SRC##_##MODE, CI_MEM_KIND(ACC, SRC, MODE)) {          \
            ADDR(OFF);                                                         \
            uint8_t *h;                                                        \
            if ((a & (ALIGN)) || !(h = mem_wr(ci, c, a, priv))) goto ref;      \
            WRITE;                                                             \
            WB;                                                                \
            CI_NEXT();                                                         \
        }
        MEM_ALL_MODES

#undef STORE
#undef LOAD

        /* Block transfers. The reference translates every word; the fast
         * path takes only transfers that are word aligned and stay inside
         * one 1 KiB block, where one host-TLB hit stands for all of them. */
        CI_HK(LDM) CI_HK(LDM_PC) {
            const uint32_t rnv = R[op->rn];
            const uint32_t a = rnv + (uint32_t)(int32_t)(int8_t)op->sa;
            const uint32_t last = a + ((uint32_t)op->rm - 1u) * 4u;
            const uint8_t *h;
            if ((a & 3u) || ((a ^ last) & ~0x3ffu) || !(h = mem_rd(ci, c, a, priv)))
                goto ref;
            if (op->kind == CI_K_LDM_PC) {
                const uint32_t t = ld32(h + (last - a));
                if ((t & 3u) == 2u) goto ref;       /* not representable */
                for (uint32_t l = op->imm & 0x7fffu; l; l &= l - 1u, h += 4)
                    R[ctz32(l)] = ld32(h);
                if (op->rs) R[op->rn] = rnv + (uint32_t)(int32_t)(int8_t)op->rs;
                c->cpsr = (c->cpsr & ~ARM_CPSR_T) | ((t & 1u) << 5);
                next_pc = t & ((t & 1u) ? ~1u : ~3u);
                op++;
                goto out;
            }
            for (uint32_t l = op->imm; l; l &= l - 1u, h += 4)
                R[ctz32(l)] = ld32(h);
            if (op->rs) R[op->rn] = rnv + (uint32_t)(int32_t)(int8_t)op->rs;
            CI_NEXT();
        }
        CI_HK(STM) {
            const uint32_t rnv = R[op->rn];
            const uint32_t a = rnv + (uint32_t)(int32_t)(int8_t)op->sa;
            const uint32_t last = a + ((uint32_t)op->rm - 1u) * 4u;
            uint8_t *h;
            if ((a & 3u) || ((a ^ last) & ~0x3ffu) || !(h = mem_wr(ci, c, a, priv)))
                goto ref;
            for (uint32_t l = op->imm; l; l &= l - 1u, h += 4)
                st32(h, R[ctz32(l)]);
            if (op->rs) R[op->rn] = rnv + (uint32_t)(int32_t)(int8_t)op->rs;
            CI_NEXT();
        }

        /* CP15 c13, exactly the reference's rules: User mode may read
         * TPIDRURW and TPIDRURO and write TPIDRURW, with CRm 0; privileged
         * modes may do anything here. The rest is the reference's to refuse. */
        CI_HK(MRC_TID) {
            if (!priv && !(op->rm == 0u && (op->sa == 2u || op->sa == 3u))) goto ref;
            R[op->rd] = op->sa == 2u ? c->cp15.tpidrurw
                      : op->sa == 3u ? c->cp15.tpidruro : c->cp15.tpidrprw;
            CI_NEXT();
        }
        CI_HK(MCR_TID) {
            if (!priv && !(op->rm == 0u && op->sa == 2u)) goto ref;
            if (op->sa == 2u) c->cp15.tpidrurw = R[op->rd];
            else if (op->sa == 3u) c->cp15.tpidruro = R[op->rd];
            else c->cp15.tpidrprw = R[op->rd];
            CI_NEXT();
        }

        /* ------------------------------------------------- VFP, decoded */
        CI_HK(VFP_DP)
            if (!vfp_fast_dp(c, op->sa, op->sh != 0u, op->rd, op->rn, op->rm)) goto ref;
            CI_NEXT();
        CI_HK(VFP_MOV)
            if (!vfp_usable(c)) goto ref;
            if (op->sh) R[op->rd] = c->vfp_s[op->rn];
            else c->vfp_s[op->rn] = R[op->rd];
            CI_NEXT();
        CI_HK(VFP_SYS)
            if (!vfp_usable(c)) goto ref;
            if (!op->sh) c->vfp_fpscr = R[op->rd] & ARM_FPSCR_WMASK;
            else if (op->rd == 15u)
                c->cpsr = (c->cpsr & 0x0fffffffu) | (c->vfp_fpscr & ARM_FPSCR_NZCV);
            else R[op->rd] = c->vfp_fpscr;
            CI_NEXT();
        CI_HK(VFP_LS) {
            /* One host-TLB lookup for a whole double, when both words are in
             * one 1 KiB block; anything else (unaligned, straddling, not
             * plain RAM, a fault) is the reference's. */
            if (!vfp_usable(c)) goto ref;
            const uint32_t a = (op->sh & 4u) ? op->imm : R[op->rn] + op->imm;
            const bool wide = (op->sh & 2u) != 0u;
            if ((a & 3u) || (wide && (a & 0x3ffu) > 0x3f8u)) goto ref;
            if (op->sh & 1u) {
                const uint8_t *h = mem_rd(ci, c, a, priv);
                if (!h) goto ref;
                c->vfp_s[op->rd] = ld32(h);
                if (wide) c->vfp_s[op->rd + 1u] = ld32(h + 4);
            } else {
                uint8_t *h = mem_wr(ci, c, a, priv);
                if (!h) goto ref;
                st32(h, c->vfp_s[op->rd]);
                if (wide) st32(h + 4, c->vfp_s[op->rd + 1u]);
            }
            CI_NEXT();
        }

        /* ------------------------------------------------- control flow */
        CI_HK(B)
            next_pc = op->imm;
            op++;
            goto out;
        CI_HK(BL)
            R[14] = PC_OF(op) + 4u;
            next_pc = op->imm;
            op++;
            goto out;
        CI_HK(TBL2) {
            uint32_t t = R[14] + op->imm;
            R[14] = (PC_OF(op) + 2u) | 1u;
            next_pc = t & ~1u;
            op++;
            goto out;
        }
        CI_HK(LDR_PC) {
            /* exec_single_transfer's LDR pc: aligned, RAM, a representable
             * target; anything else (alignment fault, UNPREDICTABLE, device,
             * translation fault) is the reference's. */
            const uint32_t rnv = (op->rs & 8u) ? PC_OF(op) + 8u : R[op->rn];
            const uint32_t off = (op->rs & 4u) ? mem_reg_off(op, R[op->rm], c->cpsr)
                                               : op->imm;
            const unsigned mode = op->rs & 3u;
            const uint32_t a = mode == CI_M_POST ? rnv : rnv + off;
            const uint8_t *h;
            if ((a & 3u) || !(h = mem_rd(ci, c, a, priv))) goto ref;
            const uint32_t t = ld32(h);
            if ((t & 3u) == 2u) goto ref;
            if (mode == CI_M_POST) R[op->rn] = rnv + off;
            else if (mode == CI_M_PRE) R[op->rn] = a;
            c->cpsr = (c->cpsr & ~ARM_CPSR_T) | ((t & 1u) << 5);
            next_pc = t & ((t & 1u) ? ~1u : ~3u);
            op++;
            goto out;
        }
        CI_HK(JMP)
            next_pc = (R[op->rm] + op->imm) & (b->thumb ? ~1u : ~3u);
            op++;
            goto out;
        CI_HK(BX) {
            uint32_t t = R[op->rm];
            if ((t & 3u) == 2u) goto ref;
            c->cpsr = (c->cpsr & ~ARM_CPSR_T) | ((t & 1u) << 5);
            next_pc = t & ~1u;
            op++;
            goto out;
        }
        CI_HK(BLX_R) {
            uint32_t t = R[op->rm];
            if ((t & 3u) == 2u) goto ref;
            R[14] = op->imm;
            c->cpsr = (c->cpsr & ~ARM_CPSR_T) | ((t & 1u) << 5);
            next_pc = t & ~1u;
            op++;
            goto out;
        }
        CI_HK(BLX_I)
            R[14] = PC_OF(op) + 4u;
            c->cpsr |= ARM_CPSR_T;
            next_pc = op->imm;
            op++;
            goto out;

        CI_HK(REF)
        default:
            goto ref;
        }

    next:
        if (++op == end) { next_pc = PC_OF(op); goto out; }
        CI_DISPATCH();

    ref: {
            /* The reference semantics for this one instruction. Cycles for
             * the specialised ops before it are published first, because
             * arm_exec_*_insn counts its own. */
            const uint32_t pc = PC_OF(op);
            const uint32_t ctrl = c->cpsr & CI_CTRL_MASK;
            c->cycles += (uint64_t)(op - flushed);
            ci->run_position = ci->run_block_base + (unsigned)(op - base);
            R[15] = pc;
            const bool vfp = op->kind == CI_K_VFP ||
                             (op->kind >= CI_K_VFP_DP && op->kind <= CI_K_VFP_LS);
            arm_status_t st = vfp ? arm_exec_vfp_insn(c, pc, op->raw)
                            : b->thumb ? arm_exec_thumb_insn(c, pc, (uint16_t)op->raw)
                                       : arm_exec_arm_insn(c, pc, op->raw);
            flushed = op + 1;
            if (CI_UNLIKELY(st != ARM_OK)) {
                *status = st;
                *stop = ARM_CI_STOP_STATUS;
                *retired_out = (unsigned)(op - base);
                return EXEC_STOP;
            }
            ci->st.ref_retired++;
            if (op->kind == CI_K_REF || op->kind == CI_K_VFP) ci->st.ref_class[op->sa]++;
            else ci->st.ref_fallback++;
            op++;
            /* A mode or endianness change always needs the machine. A change
             * of the interrupt masks alone does only when it makes something
             * deliverable: the lines move only in the machine's tick, never
             * inside a run, so masking, or unmasking with nothing pending,
             * leaves the next instruction boundary exactly as it was. */
            const uint32_t now = c->cpsr & CI_CTRL_MASK;
            if (CI_UNLIKELY(now != ctrl) &&
                (((now ^ ctrl) & ~(ARM_CPSR_I | ARM_CPSR_F | ARM_CPSR_A)) != 0u ||
                 c->abort_pending ||
                 (c->irq_line && !(c->cpsr & ARM_CPSR_I)) ||
                 (c->fiq_line && !(c->cpsr & ARM_CPSR_F)))) {
                *stop = ARM_CI_STOP_EVENT;
                *retired_out = (unsigned)(op - base);
                return EXEC_STOP;
            }
            if (CI_UNLIKELY(ci->code_written ||
                            (ci->cfg.level_dirty && *ci->cfg.level_dirty))) {
                *stop = ARM_CI_STOP_EVENT;
                *retired_out = (unsigned)(op - base);
                return EXEC_STOP;
            }
            /* Branched, or switched instruction set without branching (MSR
             * may write CPSR.T): the rest of this block was decoded for the
             * wrong PC or the wrong state, so continue through a lookup. */
            if (R[15] != pc + (1u << shift) ||
                ((c->cpsr >> 5) & 1u) != b->thumb) {
                next_pc = R[15];
                goto out;
            }
            if (op == end) { next_pc = PC_OF(op); goto out; }
            CI_DISPATCH();
        }
    }

out:
    c->cycles += (uint64_t)(op - flushed);
    R[15] = next_pc;
    *retired_out = (unsigned)(op - base);
    return result;
#undef PC_OF
}

#if CI_THREADED
#  pragma GCC diagnostic pop
#endif

/* ------------------------------------------------------------ run loop --- */

unsigned arm_ci_run(arm_ci_t *ci, arm_cpu_t *c, unsigned budget,
                    arm_status_t *status, arm_ci_stop_t *stop) {
    arm_status_t st_local = ARM_OK;
    arm_ci_stop_t stop_local = ARM_CI_STOP_BUDGET;
    unsigned retired = 0;

    if (!g_cond_ready) cond_table_init();
    if (!ci || !c || !budget) goto done;
    ci->st.runs++;
    ci->code_written = false;

    /* Direct host mutation of the translation registers is only noticed by a
     * walk; the host TLB below must not outlive it (see tlb_stamp in arm.h). */
    arm_mmu_sync_stamp(c);
    {
        const bool mmu = (c->cp15.sctlr & ARM_SCTLR_M) != 0u;
        if (c->tlb_gen < ci->seen_gen || c->reset_epoch != ci->seen_epoch ||
            mmu != ci->seen_mmu)
            purge_translations(ci);
        ci->seen_gen = c->tlb_gen;
        ci->seen_epoch = c->reset_epoch;
        ci->seen_mmu = mmu;
    }

    if (c->abort_pending || !arm_mode_is_valid(c->cpsr) ||
        (c->fiq_line && !(c->cpsr & ARM_CPSR_F)) ||
        (c->irq_line && !(c->cpsr & ARM_CPSR_I))) {
        stop_local = ARM_CI_STOP_STEP;
        ci->st.step_cause[ARM_CI_STEP_EXCEPTION]++;
        goto done;
    }

    const bool priv = (c->cpsr & ARM_CPSR_MODE_MASK) != ARM_MODE_USR;
    const uint8_t *const ram = ci->cfg.ram;
    const uint8_t *const ram_end = ram + ci->cfg.ram_size;

    while (retired < budget) {
        const uint32_t pc = c->r[15];
        const bool thumb = (c->cpsr & ARM_CPSR_T) != 0u;
        if (pc & (thumb ? 1u : 3u)) {
            stop_local = ARM_CI_STOP_STEP;
            ci->st.step_cause[ARM_CI_STEP_FETCH]++;
            break;
        }
        const uint32_t ctx = (thumb ? 1u : 0u) | (priv ? 2u : 0u);

        ci->st.lookups++;
        ci_fast_t *f = &ci->fast[((pc >> 1) ^ (pc >> (CI_FAST_BITS + 1u))) &
                                 (CI_FAST_SIZE - 1u)];
        ci_block_t *b = f->b;
        if (CI_LIKELY(b && f->va == pc && f->ctx == ctx && f->gen == c->tlb_gen &&
                      b->gen == ci->region_gen[b->pa_off >> 10] && !ci->verify)) {
            ci->st.hits++;
        } else {
            const uint8_t *h = fetch(c, pc, priv);
            if (!h || h < ram || h >= ram_end) {
                stop_local = ARM_CI_STOP_STEP;
                ci->st.step_cause[ARM_CI_STEP_FETCH]++;
                break;
            }
            const uint32_t pa_off = (uint32_t)(h - ram);
            b = ci->table[hash_slot(pa_off, thumb)];
            while (b && !(b->pa_off == pa_off && b->va == pc && b->thumb == (uint8_t)thumb))
                b = b->next;
            if (b) {
                if (b->gen != ci->region_gen[pa_off >> 10]) {
                    ci->st.stale++;
                    b = build(ci, c, h, pc, pa_off, thumb);
                } else {
                    ci->st.hits++;
                    if (ci->verify && !block_matches_ram(b, h)) {
                        ci->st.verify_mismatch++;
                        b = build(ci, c, h, pc, pa_off, thumb);
                    }
                }
            } else {
                b = build(ci, c, h, pc, pa_off, thumb);
            }
            /* build() may have flushed the whole cache, map included. */
            f->va = pc;
            f->ctx = ctx;
            f->gen = c->tlb_gen;
            f->b = b;
        }
        if (b->n == 0u) {
            stop_local = ARM_CI_STOP_STEP;
            ci->st.step_cause[b->stop_cause]++;
            break;
        }

        unsigned n = 0;
        ci->st.block_execs++;
        ci->run_block_base = retired;
        exec_result_t r = exec_block(ci, c, b, budget - retired, priv, &n,
                                     &stop_local, &st_local);
        retired += n;
        if (r == EXEC_STOP) break;
    }

    ci->st.retired += retired;
done:
    if (ci) ci->st.stop[stop_local]++;
    if (status) *status = st_local;
    if (stop) *stop = stop_local;
    return retired;
}

/* ------------------------------------------------------------- report --- */

static double pct(uint64_t part, uint64_t whole) {
    return whole ? 100.0 * (double)part / (double)whole : 0.0;
}

/* 1234 -> "1234", 12345678 -> "12.3M": compact enough for a phone alert. */
static const char *qty(char buf[16], uint64_t v) {
    if (v < 10000u) snprintf(buf, 16, "%llu", (unsigned long long)v);
    else if (v < 10000000u) snprintf(buf, 16, "%.1fk", (double)v / 1e3);
    else if (v < UINT64_C(10000000000)) snprintf(buf, 16, "%.1fM", (double)v / 1e6);
    else snprintf(buf, 16, "%.1fG", (double)v / 1e9);
    return buf;
}

unsigned arm_ci_run_position(const arm_ci_t *ci) {
    return ci ? ci->run_position : 0u;
}

size_t arm_ci_describe_stats(const arm_ci_stats_t *st, uint64_t total_retired,
                             char *out, size_t cap) {
    if (!out || !cap) return 0;
    out[0] = '\0';
    if (!st) return 0;
    char q[20][16];            /* one buffer per argument of one snprintf */
    const uint64_t *sc = st->step_cause, *rc = st->ref_class;
    int w = snprintf(out, cap,
        "Engine retired %s instructions%s%.1f%%%s; via reference %.1f%% "
        "(decoded %.1f%%, fallback %.1f%%)\n"
        "Runs: %s (budget %s, step %s, event %s, status %s)\n"
        "Blocks: %s run, %s built, %s stale, %s invalidations, %s flushes\n"
        "Handed to arm_step: SVC %s, CP15 c13 %s, WFI %s, CP15 %s, CP14 %s, "
        "other %s, interrupt/abort %s, fetch %s\n",
        qty(q[0], st->retired),
        total_retired ? " (" : "", pct(st->retired, total_retired),
        total_retired ? " of all)" : "",
        pct(st->ref_retired, st->retired),
        pct(st->ref_retired - st->ref_fallback, st->retired),
        pct(st->ref_fallback, st->retired),
        qty(q[14], st->runs), qty(q[15], st->stop[ARM_CI_STOP_BUDGET]),
        qty(q[16], st->stop[ARM_CI_STOP_STEP]), qty(q[17], st->stop[ARM_CI_STOP_EVENT]),
        qty(q[18], st->stop[ARM_CI_STOP_STATUS]),
        qty(q[1], st->block_execs), qty(q[2], st->builds), qty(q[3], st->stale),
        qty(q[4], st->invalidations), qty(q[5], st->flushes),
        qty(q[6], sc[ARM_CI_STEP_SVC]), qty(q[7], sc[ARM_CI_STEP_CP15_TLS]),
        qty(q[8], sc[ARM_CI_STEP_WFI]), qty(q[9], sc[ARM_CI_STEP_CP15]),
        qty(q[10], sc[ARM_CI_STEP_CP14]), qty(q[11], sc[ARM_CI_STEP_OTHER]),
        qty(q[12], sc[ARM_CI_STEP_EXCEPTION]), qty(q[13], sc[ARM_CI_STEP_FETCH]));
    if (w < 0) return 0;
    size_t len = (size_t)w < cap ? (size_t)w : cap - 1u;
    w = snprintf(out + len, cap - len,
        "Reference by class: VFP %s, block %s, status %s, memory %s, "
        "media %s, PC %s, other %s\n",
        qty(q[0], rc[ARM_CI_REF_VFP]), qty(q[1], rc[ARM_CI_REF_BLOCK]),
        qty(q[2], rc[ARM_CI_REF_STATUS]), qty(q[3], rc[ARM_CI_REF_MEM]),
        qty(q[4], rc[ARM_CI_REF_MEDIA]), qty(q[5], rc[ARM_CI_REF_PC]),
        qty(q[6], rc[ARM_CI_REF_OTHER]));
    if (w > 0) len += (size_t)w < cap - len ? (size_t)w : cap - len - 1u;
    return len;
}
