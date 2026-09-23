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

#define CI_HASH_BITS  16u
#define CI_HASH_SIZE  (1u << CI_HASH_BITS)
#define CI_MAX_BLOCKS 32768u
#define CI_MAX_OPS    (CI_MAX_BLOCKS * 8u)       /* 4 MiB of records */

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

void arm_ci_flush(arm_ci_t *ci) {
    if (!ci) return;
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
    for (uint32_t off = 0; n < CI_BLOCK_MAX_OPS && off + isz <= room; off += isz) {
        const uint8_t *p = host + off;
        ci_dec_t d = thumb
            ? ci_decode_thumb(pc + off, (uint16_t)(p[0] | (p[1] << 8)), &ops[n])
            : ci_decode_arm(pc + off, (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                                      ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24),
                            &ops[n]);
        if (d == CI_DEC_STOP) break;
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
    b->ops = ops;
    ci->nblocks++;
    ci->nops += n;
    ci->table[hash_slot(pa_off, thumb)] = b;
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
        case CI_DP_KIND(OPC, F, 0): {                                          \
            uint32_t a = R[op->rn], b2 = (B); (void)a;                         \
            R[op->rd] = (EXPR);                                                \
            goto next;                                                         \
        }                                                                      \
        case CI_DP_KIND(OPC, F, 1): {                                          \
            uint32_t cy, b2 = BC(cy), a = R[op->rn]; (void)a;                  \
            uint32_t r = (EXPR);                                               \
            R[op->rd] = r;                                                     \
            set_logic(c, r, cy);                                               \
            goto next;                                                         \
        }
#define TESTOP(OPC, F, BC, EXPR)                                                \
        case CI_DP_KIND(OPC, F, 1): {                                          \
            uint32_t cy, b2 = BC(cy), a = R[op->rn];                           \
            set_logic(c, (EXPR), cy);                                          \
            goto next;                                                         \
        }
#define ARITH(OPC, F, B, VAL, X, Y, CIN)                                        \
        case CI_DP_KIND(OPC, F, 0): {                                          \
            uint32_t a = R[op->rn], b2 = (B);                                  \
            R[op->rd] = (VAL);                                                 \
            goto next;                                                         \
        }                                                                      \
        case CI_DP_KIND(OPC, F, 1): {                                          \
            uint32_t a = R[op->rn], b2 = (B);                                  \
            R[op->rd] = add_flags(c, (X), (Y), (CIN));                         \
            goto next;                                                         \
        }
#define CMPOP(OPC, F, B, X, Y, CIN)                                             \
        case CI_DP_KIND(OPC, F, 1): {                                          \
            uint32_t a = R[op->rn], b2 = (B);                                  \
            (void)add_flags(c, (X), (Y), (CIN));                               \
            goto next;                                                         \
        }
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

        DP_FORM(CI_F_IMM, B_I, BC_I)
        DP_FORM(CI_F_REG, B_R, BC_R)
        DP_FORM(CI_F_SHI, B_H, BC_H)
        DP_FORM(CI_F_SHR, B_X, BC_X)

#undef DP_FORM
#undef CMPOP
#undef ARITH
#undef TESTOP
#undef LOGIC

        /* --------------------------------------------------- multiplies */
        case CI_K_MUL:  R[op->rd] = R[op->rm] * R[op->rs]; goto next;
        case CI_K_MULS: {
            uint32_t r = R[op->rm] * R[op->rs];
            R[op->rd] = r;
            c->cpsr = (c->cpsr & 0x3fffffffu) | nz(r);
            goto next;
        }
        case CI_K_MLA:  R[op->rd] = R[op->rm] * R[op->rs] + R[op->rn]; goto next;
        case CI_K_MLAS: {
            uint32_t r = R[op->rm] * R[op->rs] + R[op->rn];
            R[op->rd] = r;
            c->cpsr = (c->cpsr & 0x3fffffffu) | nz(r);
            goto next;
        }
        case CI_K_UMULL: {
            uint64_t u = (uint64_t)R[op->rm] * R[op->rs];
            R[op->rn] = (uint32_t)u; R[op->rd] = (uint32_t)(u >> 32);
            goto next;
        }
        case CI_K_UMLAL: {
            uint64_t u = (uint64_t)R[op->rm] * R[op->rs];
            u += ((uint64_t)R[op->rd] << 32) | R[op->rn];
            R[op->rn] = (uint32_t)u; R[op->rd] = (uint32_t)(u >> 32);
            goto next;
        }
        case CI_K_SMULL: {
            uint64_t u = (uint64_t)((int64_t)(int32_t)R[op->rm] * (int64_t)(int32_t)R[op->rs]);
            R[op->rn] = (uint32_t)u; R[op->rd] = (uint32_t)(u >> 32);
            goto next;
        }
        case CI_K_SMLAL: {
            uint64_t u = (uint64_t)((int64_t)(int32_t)R[op->rm] * (int64_t)(int32_t)R[op->rs]);
            u += ((uint64_t)R[op->rd] << 32) | R[op->rn];
            R[op->rn] = (uint32_t)u; R[op->rd] = (uint32_t)(u >> 32);
            goto next;
        }

        /* ------------------------------------------------ miscellaneous */
        case CI_K_NOP: goto next;
        case CI_K_CLREX: c->excl_valid = false; goto next;
        case CI_K_MRS_CPSR: R[op->rd] = c->cpsr; goto next;
        case CI_K_CLZ: R[op->rd] = clz32(R[op->rm]); goto next;
        case CI_K_SXTB: R[op->rd] = (uint32_t)(int32_t)(int8_t)(uint8_t)ror32(R[op->rm], op->sa); goto next;
        case CI_K_SXTH: R[op->rd] = (uint32_t)(int32_t)(int16_t)(uint16_t)ror32(R[op->rm], op->sa); goto next;
        case CI_K_UXTB: R[op->rd] = ror32(R[op->rm], op->sa) & 0xffu; goto next;
        case CI_K_UXTH: R[op->rd] = ror32(R[op->rm], op->sa) & 0xffffu; goto next;
        case CI_K_REV: {
            uint32_t v = R[op->rm];
            R[op->rd] = (v << 24) | ((v & 0xff00u) << 8) | ((v >> 8) & 0xff00u) | (v >> 24);
            goto next;
        }
        case CI_K_REV16: {
            uint32_t v = R[op->rm];
            R[op->rd] = ((v & 0x00ff00ffu) << 8) | ((v >> 8) & 0x00ff00ffu);
            goto next;
        }
        case CI_K_REVSH: {
            uint32_t v = R[op->rm];
            uint16_t h = (uint16_t)(((v & 0xffu) << 8) | ((v >> 8) & 0xffu));
            R[op->rd] = (uint32_t)(int32_t)(int16_t)h;
            goto next;
        }

        /* ------------------------------------------------------- memory */
        case CI_K_LDR_LIT: {
            uint32_t a = op->imm;
            const uint8_t *h;
            if ((a & 3u) || !(h = mem_rd(ci, c, a, priv))) goto ref;
            R[op->rd] = ld32(h);
            goto next;
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
        case CI_MEM_KIND(ACC, SRC, MODE): {                                    \
            ADDR(OFF);                                                         \
            const uint8_t *h;                                                  \
            if ((a & (ALIGN)) || !(h = mem_rd(ci, c, a, priv))) goto ref;      \
            R[op->rd] = (READ);                                                \
            WB;                                                                \
            goto next;                                                         \
        }
#define STORE(ACC, SRC, MODE, ADDR, OFF, WB, ALIGN, WRITE)                      \
        case CI_MEM_KIND(ACC, SRC, MODE): {                                    \
            ADDR(OFF);                                                         \
            uint8_t *h;                                                        \
            if ((a & (ALIGN)) || !(h = mem_wr(ci, c, a, priv))) goto ref;      \
            WRITE;                                                             \
            WB;                                                                \
            goto next;                                                         \
        }
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

        MEM_MODES(CI_S_IMM, OFF_IMM)
        MEM_MODES(CI_S_REG, OFF_REG)

#undef MEM_MODES
#undef STORE
#undef LOAD

        /* ------------------------------------------------- control flow */
        case CI_K_B:
            next_pc = op->imm;
            op++;
            goto out;
        case CI_K_BL:
            R[14] = PC_OF(op) + 4u;
            next_pc = op->imm;
            op++;
            goto out;
        case CI_K_TBL2: {
            uint32_t t = R[14] + op->imm;
            R[14] = (PC_OF(op) + 2u) | 1u;
            next_pc = t & ~1u;
            op++;
            goto out;
        }
        case CI_K_BX: {
            uint32_t t = R[op->rm];
            if ((t & 3u) == 2u) goto ref;
            c->cpsr = (c->cpsr & ~ARM_CPSR_T) | ((t & 1u) << 5);
            next_pc = t & ~1u;
            op++;
            goto out;
        }
        case CI_K_BLX_R: {
            uint32_t t = R[op->rm];
            if ((t & 3u) == 2u) goto ref;
            R[14] = op->imm;
            c->cpsr = (c->cpsr & ~ARM_CPSR_T) | ((t & 1u) << 5);
            next_pc = t & ~1u;
            op++;
            goto out;
        }
        case CI_K_BLX_I:
            R[14] = PC_OF(op) + 4u;
            c->cpsr |= ARM_CPSR_T;
            next_pc = op->imm;
            op++;
            goto out;

        case CI_K_REF:
        default:
            goto ref;
        }

    next:
        if (++op == end) { next_pc = PC_OF(op); goto out; }
        continue;

    ref: {
            /* The reference semantics for this one instruction. Cycles for
             * the specialised ops before it are published first, because
             * arm_exec_*_insn counts its own. */
            const uint32_t pc = PC_OF(op);
            const uint32_t ctrl = c->cpsr & CI_CTRL_MASK;
            c->cycles += (uint64_t)(op - flushed);
            R[15] = pc;
            arm_status_t st = b->thumb ? arm_exec_thumb_insn(c, pc, (uint16_t)op->raw)
                                       : arm_exec_arm_insn(c, pc, op->raw);
            flushed = op + 1;
            if (CI_UNLIKELY(st != ARM_OK)) {
                *status = st;
                *stop = ARM_CI_STOP_STATUS;
                *retired_out = (unsigned)(op - base);
                return EXEC_STOP;
            }
            ci->st.ref_retired++;
            op++;
            if (CI_UNLIKELY((c->cpsr & CI_CTRL_MASK) != ctrl ||
                            ci->code_written ||
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
            continue;
        }
    }

out:
    c->cycles += (uint64_t)(op - flushed);
    R[15] = next_pc;
    *retired_out = (unsigned)(op - base);
    return result;
#undef PC_OF
}

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

    if (c->abort_pending || !arm_mode_is_valid(c->cpsr) ||
        (c->fiq_line && !(c->cpsr & ARM_CPSR_F)) ||
        (c->irq_line && !(c->cpsr & ARM_CPSR_I))) {
        stop_local = ARM_CI_STOP_STEP;
        goto done;
    }

    const bool priv = (c->cpsr & ARM_CPSR_MODE_MASK) != ARM_MODE_USR;
    const uint8_t *const ram = ci->cfg.ram;
    const uint8_t *const ram_end = ram + ci->cfg.ram_size;

    while (retired < budget) {
        const uint32_t pc = c->r[15];
        const bool thumb = (c->cpsr & ARM_CPSR_T) != 0u;
        if (pc & (thumb ? 1u : 3u)) { stop_local = ARM_CI_STOP_STEP; break; }
        const uint8_t *h = fetch(c, pc, priv);
        if (!h || h < ram || h >= ram_end) { stop_local = ARM_CI_STOP_STEP; break; }
        const uint32_t pa_off = (uint32_t)(h - ram);

        ci->st.lookups++;
        ci_block_t *b = ci->table[hash_slot(pa_off, thumb)];
        if (b && b->pa_off == pa_off && b->va == pc && b->thumb == (uint8_t)thumb) {
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
        if (b->n == 0u) { stop_local = ARM_CI_STOP_STEP; break; }

        unsigned n = 0;
        ci->st.block_execs++;
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
