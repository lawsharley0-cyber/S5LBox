# Implementation plan: a JIT-free cached interpreter for the ARM1176 core

Inputs: `CURRENT_ARCHITECTURE.md` (what exists), `ICUBE_DOLPHIN_RESEARCH.md`
(what works elsewhere), `hotpath.md` r470–r484 (what already failed here),
`PERFORMANCE_BASELINE_PLAN.md` (how it will be judged).

## 0. Decisions

1. **The reference interpreter (`arm_step`) stays the specification and the
   default.** The new engine is opt-in (`S5L8900_CPU_BACKEND_CACHED_BLOCK`)
   until it passes real-guest gates on the user's hardware.
2. **The `75ad89f` tiers are removed**, not repaired: they are unsound in ways
   that touch every layer (decode, timing, invalidation, memory semantics;
   `CURRENT_ARCHITECTURE.md` §7), and one of their tools fabricates profiles.
   The public enum values are kept so the app and harness compile unchanged;
   `CACHED_BLOCK` selects the new engine, `IR_OPTIMIZED`/`JIT` are retired
   aliases that select it too (documented), `INTERPRETER` is unchanged.
3. **Exactness is the acceptance criterion**: for any program, the engine must
   leave the same architectural CPU state, RAM, cycle count and device-visible
   timing as `arm_step` + the machine's run loop. Derived host counters (TLB
   hit counts) may differ.
4. **The main lever is specialization, not caching** (research §2, §4): hot
   instruction forms get dedicated handlers with pre-extracted operands and
   never call the generic `exec_*` helpers.
5. **Portable first**: C11, `switch` dispatch everywhere, computed-`goto`
   dispatch as a compile-time option on GCC/Clang. ARM64-specific paths are
   optional and must pass the same tests.
6. No JIT, no executable memory, no new entitlements.

## 1. Engine structure (`core/src/arm/arm_ci*.c`, `core/include/arm_ci.h`)

```
 s5l8900_run()  ── budget = exact retirement batch limit (next timebase edge,
      │                    MMIO/input dirtiness, active-clock cap)
      ▼
 arm_ci_run(ci, cpu, budget)          ← returns retired count + exit reason
      │  loop while budget > 0:
      │   1. pending IRQ/FIQ / abort?          → exit (arm_step takes it)
      │   2. fetch-translate PC (1 KB fetch cache / TLB)
      │        fault or not RAM                → exit NEEDS_STEP
      │   3. lookup block (pa, va, T); miss → decode+build
      │   4. execute ops (≤ budget), specialized handlers
      │        MMIO access / CPSR control change / exception / abort
      │                                        → exit after that instruction
      │        terminator-before (SVC, CP15, WFI, undefined, …)
      │                                        → exit NEEDS_STEP
      ▼
 machine ticks `retired` exactly as its batch path does; NEEDS_STEP runs one
 ordinary arm_step() + tick(1).
```

**Op record** (16 bytes, arena-allocated, no per-instruction heap
allocation): handler kind, condition, up to four register fields, a shift
kind/amount byte, flags byte, pc offset within the block, 32-bit immediate
(rotated immediate, offset, branch target or folded PC-relative constant) and
the raw instruction word (for reference fallback).

**Blocks** start at a guest PC, never cross a 1 KB fetch subpage (the
granularity of fetch permission in the reference, so one translation covers the
whole block), hold ≤ 64 ops, and end after a branch/PC write, before a
"terminator-before" instruction, or at the subpage end. Conditional
instructions stay inside blocks (ARM predication); a conditional branch ends
the block with two exits.

**Cache key**: physical address + virtual address + `CPSR.T`. Decoding is
privilege-independent (privilege-dependent behaviour is decided at run time
or via reference fallback), and physical keying survives address-space
switches without flushing.

**Instruction classes** (each ≥ 1 handler):

| Class | ARM | Thumb-1 |
|---|---|---|
| Data processing | imm, reg, reg-shift-imm (LSL/LSR/ASR/ROR/RRX and the #32 forms), reg-shift-reg; S and non-S; PC operands folded | shift-imm, add/sub reg/imm3, mov/cmp/add/sub imm8, 16 ALU ops, hi-reg add/cmp/mov, ADR/ADD SP |
| Loads/stores | LDR/STR/LDRB/STRB imm/reg (P/U/W), LDRH/STRH/LDRSB/LDRSH, LDR literal | reg/imm/SP/PC-relative forms |
| Multiple | LDM/STM (no `^`), PUSH/POP | PUSH/POP, LDMIA/STMIA |
| Branch | B, BL, BX, BLX (reg, imm), LDR pc, LDM {pc}, MOV pc | B<cond>, B, BL pair, BX/BLX, POP {pc} |
| Multiply | MUL/MLA/UMULL/UMLAL/SMULL/SMLAL | MUL |
| Media | UXT*/SXT*, REV*, CLZ | SXTH/SXTB/UXTH/UXTB, REV/REV16/REVSH (16-bit ARMv6 encodings) |
| Reference-in-block | everything else that is safe (VFP, LDRD/STRD, exclusives, DSP, MRS, …) runs the reference single-instruction executor inside the block; the engine checks afterwards whether it changed control state |
| Terminator-before | SVC/SWI, CP14/CP15 (incl. WFI), BKPT, SETEND, undefined/unallocated, `cond==0xF` except BLX-imm/PLD/CLREX | same |

**Reference-in-block** is what makes coverage total without risk: the
interpreter's ARM body is factored into an always-inline function so
`arm_step` compiles as before, and the engine calls a non-inlined wrapper for
one pre-fetched instruction. `thumb_step` is already a separate function.

**Memory fast path**: the engine owns read and write host TLBs (1 KB
granularity like the reference's `dread`, 1024 entries each, tagged
`va>>10 | priv`, validated by `cpu->tlb_gen`), filled only after a successful
reference translation of plain RAM. Only naturally aligned, non-crossing
accesses use them; everything else calls the reference accessors
(`mem_rN_as`/`mem_wN_as`, exported through an internal header), preserving
alignment faults, `SCTLR.U`, legacy rotation and page-crossing splits. A
non-RAM access sets `level_dirty` in the machine bus, and the engine exits
after that instruction.

**Data aborts** inside a block: the handler restores base registers exactly as
the reference does, the engine calls the reference abort-entry routine, counts
the instruction as retired (as `arm_step` does) and exits.

## 2. Self-modifying code and invalidation

- One bit per 1 KB physical block of DRAM (`code_map`) and one generation word
  per 1 KB block (`region_gen`); a block stores the generation at build time
  and is dead when they differ (O(1) lazy invalidation).
- Marking a 1 KB block as code purges the reference `dwrite` entries and the
  engine's write-TLB entries that point into it.
- `machine_host_ram` refuses **write** host pointers for code blocks (so the
  reference `dwrite`, the engine write TLB and MBX direct writes all fall back
  to `bus_write`), and `bus_write`'s RAM path invalidates a code block it
  touches. All CPU, DMA and MBX stores therefore see invalidation.
- Host-side writers that `memcpy` into guest RAM (`md_bridge.c`,
  `md_raw_bridge.c`, loaders, `s5l8900_load`, snapshot restore, bring-up) call
  `s5l8900_note_ram_write()` / flush.
- Safety net: guest I-cache maintenance (`MCR p15,0,Rd,c7,c5,{0,1,2}` and
  `c7,c7,0`) invalidates (by line when possible), counted.
- Debug/CI mode `verify`: each block entry compares the block's raw words with
  RAM and aborts loudly on mismatch, so a missed writer is caught in tests and
  can be switched on for a real-guest run.

## 3. Timing, interrupts, determinism

- Budget = the same `run_retirement_batch_limit` the static engine uses: never
  past the next timebase edge (exact mode) or 256 instructions (active clock).
- IRQ/FIQ lines only change inside `s5l8900_tick`, so checking them at each
  block entry inside one budget is equivalent to checking before every
  instruction; any instruction that changes `CPSR.I/F`/mode exits the engine.
- Instructions that can advance device time (WFI, privileged host SVC) are
  terminator-before and execute on the ordinary path with `tick(1)`.
- Pre-step hooks (HLE) disable the engine for that run; the static AArch64
  engine is bypassed while the cached interpreter is selected (one engine at a
  time, so A/B comparisons are clean).

## 4. Dispatch

Per-op dispatch is the inner loop. Implemented as a `switch` in a loop
(portable, MSVC) and, on GCC/Clang, computed-`goto` threading: every handler
ends in its own copy of the dispatch (condition check, jump through a
256-entry label table whose unlisted kinds default to the reference). As
built: threaded is the GCC/Clang default because it measured ×1.053 geomean
over the switch (`BENCHMARK_RESULTS.md` §3); `-DS5LBOX_CI_SWITCH_DISPATCH`
forces the switch on any compiler, and the Windows CI job (MSVC) exercises
it. Clang `musttail` tail-call threading (iCube I3) is a later ARM64
experiment. Condition evaluation uses a 16×16-bit truth table indexed by
`cond` and `NZCV`; `AL` ops skip it.

## 5. Block chaining

The dispatch loop inside `arm_ci_run` already avoids returning to the machine
between blocks. Each block additionally caches up to two successor pointers
(taken / fall-through) validated by the successor's generation and by the
fetch translation still resolving to the same host block; indirect exits use
the hash lookup. Budget and IRQ checks remain at every block boundary.

## 6. Testing

| Test | What it proves |
|---|---|
| `test_cpu_diff` single-instruction fuzzer | Every specialized handler equals `arm_step` over millions of random encodings × random CPU states × modes × MMU on/off × aligned/unaligned addresses (full `arm_cpu_t` architectural comparison + memory digest) |
| Program-level lockstep | Random multi-block programs and the compiled workload suite run on two machines (reference vs engine), compared every block |
| Condition codes | All 15 conditions × 16 NZCV combinations |
| Exceptions | Undefined, SVC, prefetch abort, data abort, IRQ, FIQ, reset: mode, PC, LR, CPSR, SPSR, vector, identical in both engines |
| ARM/Thumb | BX/BLX/LDR pc/POP pc/exception return transitions; cache never runs an ARM block as Thumb |
| SMC | Store over a cached block (CPU, bus, DMA-style, host memcpy + note), I-cache maintenance, page remap |
| Timing | Timer IRQ taken at the identical instruction count in both engines |

## 7. Development order (maps the task's parts 12–29) and commits

1. `docs:` the five planning documents (this commit).
2. `cpu: remove the unsound 75ad89f tiers and the fabricated profiler` (+
   fixes the red CI job). Enum values kept.
3. `build: CMake presets and Windows build guide`.
4. `bench: compiled ARM/Thumb guest workload suite` + baseline numbers.
5. `cpu: factor arm_step's ARM body for single-instruction reuse` (with proof
   that `arm_step` throughput is unchanged).
6. `tests: single-instruction differential fuzzer` (reference vs reference to
   start: validates the harness).
7. `cpu: cached interpreter — decoder, block cache, reference-in-block ops`
   (correct first; all ops are reference ops).
8. `cpu: specialized ARM data-processing/branch/load-store handlers`
   (+ fuzzer coverage), then Thumb, then multiples/multiplies/media.
9. `machine: run-loop integration with exact budgets`, lockstep tests.
10. `cpu: code-page tracking and invalidation` + SMC tests.
11. `cpu: engine host TLB fast path`.
12. `cpu: threaded dispatch option`, `cpu: block chaining` — each kept only if
    measured faster.
13. Benchmarks after each of 8, 11, 12 (`BENCHMARK_RESULTS.md`).
14. Reports: `WINDOWS_VALIDATION.md`, `MAC_VALIDATION.md`, `KNOWN_ISSUES.md`,
    `IOS6_READINESS.md`.

## 8. Out of scope for this session (requires the user's hardware)

Real-firmware boots and restored-checkpoint A/B (needs the user's firmware),
MSVC runs other than CI, Xcode/iOS builds, physical-device performance and
thermals, ARM64 host profiling on a phone. Each has exact instructions in
`WINDOWS_BUILD.md` / `MAC_VALIDATION.md`, and nothing is claimed about them
until they are run.
