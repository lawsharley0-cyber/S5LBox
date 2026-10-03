# No-JIT execution: what other emulators do, and what NEON should do next

The question: can an ARMv6/ARMv7 iPhone CPU be emulated at or near its real
speed on a modern ARM64 Apple device **without** JIT, `MAP_JIT`, RWX pages,
a jailbreak, or any machine code generated at run time? This study reads
nine projects for techniques, compares them with NEON's current engine, and
turns the result into a staged, measurable plan.

Inspected 2026-10-02. All numbers quoted from other projects are theirs and
were not reproduced here unless marked; NEON numbers are from
`docs/BENCHMARK_RESULTS.md` and this repository's own runs.

| Project | Commit | License | Guest → host |
|---|---|---|---|
| [jjMao/ish-arm64](https://github.com/jjMao/ish-arm64) | `6cbd274` | GPLv3 | AArch64 Linux → AArch64 |
| [ish-app/ish](https://github.com/ish-app/ish) | `8334836` | GPLv3 | i386 Linux → AArch64/x86-64 |
| [dnakov/litter-ish](https://github.com/dnakov/litter-ish) (ios-linuxkit) | `c8e9dcb` | GPLv3 | AArch64 Linux → AArch64 |
| [Provenance-Emu/iCube](https://github.com/Provenance-Emu/iCube) | `ae374fd` | GPLv2+ (Dolphin) | PowerPC → AArch64 |
| [MuffinFluffin/Fin](https://github.com/MuffinFluffin/Fin) | `b5bc607` | GPLv2+ (Dolphin) | PowerPC → AArch64 |
| [mgba-emu/mgba](https://github.com/mgba-emu/mgba) | `c3c8e5e` | MPL-2.0 | ARM7TDMI → any |
| [LIJI32/SameBoy](https://github.com/LIJI32/SameBoy) | `213a12c` | MIT (Expat) | SM83 → any |
| [utmapp/UTM](https://github.com/utmapp/UTM) + [utmapp/qemu](https://github.com/utmapp/qemu) `utm-edition` | `7eadb05`, `b795d6d` | Apache-2.0 / GPLv2 | any (via TCG) → AArch64 |
| [qemu/qemu](https://github.com/qemu/qemu) | `9ded45a` | GPLv2 | any → any |

**Licensing.** NEON is MIT. Every CPU engine that matters here (iSH family,
QEMU/TCTI, Dolphin forks) is GPL, so nothing is copied: this document
records techniques, and NEON implements its own. mGBA (MPL-2.0, file-level)
and SameBoy (MIT) could legally contribute files, but neither has a file
that fits NEON's engine as is.

---

## 1. The short answer

1. **NEON already has the core of the proposed design.** Its cached
   interpreter (`core/src/arm/arm_ci.c`) decodes each guest block once into
   16-byte op records, caches blocks by virtual address, chains blocks,
   dispatches with replicated computed `goto` (threaded code), and resolves
   loads/stores through a per-privilege software TLB that returns a host
   pointer. Stages 3, 4, 5 and most of 7 of the requested plan exist and are
   measured: 2.56x geomean over the reference interpreter, 115–193 M guest
   instructions/s on integer workloads on a 2.8 GHz Xeon host.
2. **The "precompiled ARM64 handler" step (iSH / TCTI style) is a smaller
   win than it looks.** NEON's handlers *are* precompiled native code; the
   compiler builds them. What hand-written gadgets add is (a) direct
   threading (no table lookup), (b) guest state in pinned host registers, and
   (c) host flag instructions. (a) and (c) can be had in C. (b) needs either
   a register-specialised gadget per operand combination (TCTI: **2,016,326
   gadgets, ~33 MiB of code**, measured below) or a per-block register
   allocator, which is a JIT.
3. **The next real gains are optimisations over the cached records**, all
   proven elsewhere without code generation: superinstructions / fusion
   (litter-ish, iCube), dead-flag elimination (iCube), direct threading,
   host NZCV for ARM flags (AArch64's NZCV sits in bits 31:28 exactly where
   AArch32's CPSR has them), a return-address cache (ish-arm64), and cheaper
   device time (SameBoy, mGBA).
4. **Full speed is plausible for the iPhone 3G and unlikely for the 3GS
   from CPU interpretation alone.** Estimates in §6; the one number that
   would settle it, NEON's cached interpreter on a current A-series phone,
   has not been measured yet and is Stage 1 below.
5. **Where the CPU cannot catch up, replace work instead of speeding it.**
   Byte-verified native versions of the guest's hottest functions (NEON's
   `tools/ios3_hle.c` already does this for iPhone OS 3's rasteriser;
   Podium measured 94% of iOS 6 backboardd time in four span functions) beat
   any interpreter improvement on those paths.

---

## 2. Each project

### 2.1 iSH (`asbestos/`)

- **Execution:** "Asbestos" (formerly "jit"): each guest basic block becomes
  an array (`fiber_block.code[]`) of gadget addresses with inline
  immediates. Gadgets are hand-written assembly (`gadgets-aarch64/*.S`),
  expanded per operand by assembler macros. Each ends with
  `ldr x8, [_ip, #n]!; br x8`: fetch the next gadget address from the data
  array and jump. No code is generated at run time; the array is data.
- **Registers:** the eight i386 registers are pinned in host w20–w27 for
  the whole time translated code runs; `_cpu` (x1), `_tlb` (x2), `_ip`
  (x28), `_tmp`, `_addr` are fixed too. C helpers are called with a
  save/restore sequence.
- **Memory:** a software TLB lookup inlined into every memory gadget
  (page compare, then `host = entry.data_minus_addr + guest`), with a
  cross-page path and a C miss handler.
- **Blocks:** hashed by guest address; listed per page for invalidation
  (`asbestos_invalidate_page`); freed lazily through a "jetsam" list.
- **Chaining:** the jump gadget's target slot is patched to the successor
  block's `code[]`. **This is a data write, which is why chaining works
  without executable memory.**
- **Flags:** x86 flags are kept lazily (result/operands saved, computed on
  demand).

### 2.2 ish-arm64 (`asbestos/guest-arm64/`)

- Same engine, new guest: **AArch64**, not AArch32. Roughly 300 gadgets in
  `math.S`, 90 in `memory.S`, 25 in `control.S` (counts of `.gadget` lines).
- Gadgets are **not** register-specialised: `adds_reg` unpacks rd/rn/rm/
  shift/size from one 64-bit operand word at run time, loads guest registers
  from `cpu->x[]`, branches on shift type, executes the host `adds`, and
  stores flags with `mrs x17, nzcv; str w17, [cpu, #nzcv]`. It is, in
  effect, a cached interpreter written in assembly, plus native flags.
- A **return cache**: `BL` records the code-stream position keyed by return
  address; `RET` jumps straight there instead of a hash lookup.
- Its benchmark (`benchmark/BENCHMARK_PERF.md`): compute-bound C about
  6.5x slower than native; "2–12x faster than x86" is against iSH's own
  i386 guest, not against a C cached interpreter.
- Debug switches to disable block chaining / the return cache: a good
  pattern for bisecting engine bugs.

### 2.3 litter-ish / ios-linuxkit

- Same engine as ish-arm64, explicitly "no runtime code generation, RWX
  memory, or `MAP_JIT`".
- Its contribution is **disciplined gadget fusion** (`docs/ARM64_GADGET_FUSION_PLAN.md`):
  CMP/SUBS+B.cond, ADRP+ADD, ADRP+LDR, ADD/SUB+LDR/STR, LDR+CBZ/CBNZ, each
  only for adjacent same-page instructions, never across SVC, barriers,
  atomics or indirect branches, each with a fixture for both the success
  and the **fault** path (the faulting instruction's PC is recorded before
  the access, and earlier side effects stay visible). Fusion candidates are
  chosen from measured counters, behind a default-off hot-trace recorder.
- This is the cleanest reference for *how to add fusions safely*.

### 2.4 iCube (`Source/Core/Core/PowerPC/CachedInterpreter/`)

- Dolphin's Cached Interpreter: a block is a tape of
  `(function pointer, operands)` records, no machine code. iCube adds a
  typed IR engine (`CachedInterpreterIR.h`, milestones M0–M6): block
  linking (direct-branch terminals carry a resolved pointer to the next
  block's IR, followed without a dispatcher round trip), **dead condition-
  register-flag elimination**, constant fusion (`lis`+`addi` → one
  constant write), micro-op fusion (a run of simple ALU ops dispatched once),
  and direct-pointer load/store fast paths.
- **Every optimisation ships with a "validate" twin op** that runs the
  optimised and unoptimised forms on a state snapshot and asserts identical
  results. That is the strongest correctness discipline in the set.
- Selectable as a separate engine beside the shipping one, so it can be
  A/B-tested without touching it.

### 2.5 Fin

- Dolphin-derived; its Cached Interpreter is close to upstream
  (`CachedInterpreter.cpp` 1.8 k lines against iCube's 7.5 k + 3.5 k IR) with
  a small `InlineCachedInterpreter`. As a conceptual reference it is the
  simpler of the two and shows the baseline Dolphin design; iCube shows how
  far that design can be pushed. NEON's earlier study of the same design is
  `docs/ICUBE_DOLPHIN_RESEARCH.md`.

### 2.6 mGBA (`src/arm/`)

- A straight interpreter: fetch, then a **4096-entry function-pointer
  table** indexed by opcode bits [27:20] and [7:4] (`_armTable`), Thumb by
  bits [15:6]. Conditions through a 16-entry table of 16-bit masks over
  NZCV (`conditionLut`), the same idea as NEON's `ci_cond_table`.
- Instruction fetch through `activeRegion` + `activeMask`: a host pointer
  for the current memory region, updated only on branches, so sequential
  fetch is one masked load. (No MMU on a GBA; NEON's equivalent is the
  1 KiB fetch-block host pointer.)
- Timing: one `cycles < nextEvent` compare per instruction
  (`ARMRunLoop`), events processed only when it fails.
- Lesson: mGBA's speed comes from having almost nothing in the hot path,
  not from caching; NEON's ARMv7 MMU and exception model put more in the
  hot path than a GBA ever needs.

### 2.7 SameBoy (`Core/timing.c`, `display.c`)

- Cycle-exact: devices are advanced after CPU memory accesses
  (`GB_advance_cycles`). Each device is a resumable state machine
  (`GB_STATE_MACHINE` / `GB_SLEEP`) holding a countdown, so most calls are
  one subtraction; real work happens only when the countdown expires.
- Lesson for NEON: the device graph should cost nothing between events.
  NEON's measured device refresh (18–24% of host time in §4 of
  `BENCHMARK_RESULTS.md`, partly addressed by the "event horizon", §9) is
  the target.

### 2.8 UTM and its QEMU fork: TCTI

- UTM runs QEMU in an in-app thread on iOS; JIT-less mode uses the
  **TCG Tiny-Code Threaded Interpreter** (`tcg/aarch64-tcti/`) in its QEMU
  fork. TCTI is a TCG *backend*: QEMU's front ends translate any guest to
  TCG ops as usual, and the backend emits, instead of machine code, an
  array of gadget addresses + immediates, run with
  `ldr x27, [x28], #8; br x27`.
- TCG virtual registers live in host x1–x15. Every gadget is generated by
  `tcti-gadget-gen.py` **for every register combination**: `add_i32` is
  4,096 gadgets (16 × 16 × 16).
- Guest memory: the gadget inlines QEMU's softmmu TLB fast path (index,
  compare, add the entry's addend) and calls C only on a miss.
- **Measured here:** running its generator produces **2,016,326 gadgets,
  8,653,019 instructions (~33 MiB of code)** from 445 MB of generated C.
  That is the price of putting guest values in host registers without a
  register allocator.
- Generality: TCTI gets every QEMU guest for free, because it works on
  TCG ops. For NEON that generality is not needed (one guest ISA), and
  TCG's ARM front end (thousands of lines of QEMU) would replace NEON's own,
  differential-tested decoder.

### 2.9 QEMU (`tcg/`, `accel/tcg/`, `target/arm/tcg/`)

- Translation blocks (TBs) of TCG IR keyed by (pc, flags, cs_base), a
  per-CPU jump cache in front of a hash table, direct chaining through
  `goto_tb` (patched jump), `goto_ptr` with a lookup for indirect jumps.
- **TCI** (`tcg/tci.c`): the portable interpreter of TCG bytecode, a `switch
  (opc)` loop; slow (no register mapping, generic operand decoding).
- softmmu: per-MMU-index TLB entries holding `addend = host − guest`; the
  fast path is index, compare tag, add. NEON's `ci_tlb_t` is the same
  idea at 1 KiB granularity.
- Useful as the reference for TB invalidation on code writes (page flags)
  and for ARM exception/MMU semantics; not a codebase to embed.

---

## 3. Comparison

| | Execution | Runtime codegen | No JIT | Block cache | Dispatch | Memory translation | Complexity |
|---|---|---|---|---|---|---|---|
| ish-arm64 | threaded gadgets (asm), generic operands | no | yes | hashed blocks, page lists, chained, ret cache | `ldr x8,[pc],#8; br x8` | inline TLB in gadget, C on miss | high (28 k lines asm+C for the ARM64 guest) |
| iSH | threaded gadgets (asm), per-register variants | no | yes | as above | same | inline TLB | high |
| litter-ish | as ish-arm64 + fusion | no | yes | as above + fused gadgets | same | same | high |
| iCube | cached interpreter + typed IR passes | no | yes | Dolphin JIT block cache, linked IR | switch over IR records | fastmem / direct-pointer fast path | high |
| Fin | Dolphin cached interpreter | no | yes | Dolphin block cache | function-pointer tape | Dolphin MMU | medium |
| mGBA | interpreter, table decode | no | yes | none | function pointer per instruction | region pointer, no MMU | low |
| SameBoy | interpreter, cycle-exact | no | yes | none | switch | flat | low |
| UTM TCTI | TCG IR → threaded gadgets | no | yes | QEMU TBs, chained | `ldr x27,[x28],#8; br x27` | inline softmmu TLB | very high (2 M generated gadgets) |
| QEMU TCI | TCG IR interpreter | no | yes | QEMU TBs | `switch (opc)` | softmmu helpers | very high (whole QEMU) |
| QEMU TCG | JIT | **yes** | no | TBs, patched direct jumps | native code | inline TLB | very high |
| **NEON** | cached interpreter, 16-byte op records | no | yes | VA-keyed blocks, chained, per-1 KiB code generations | replicated computed `goto` per handler | per-privilege 4096-entry TLB → host pointer | medium (2.5 k lines engine) |

**Applicable to NEON:**
fusion with exact fault PCs (litter-ish); dead-flag elimination and validate
twins (iCube); direct threading (iSH, TCTI); return-address cache
(ish-arm64); native NZCV via `adds`/`mrs` (ish-arm64); `nextEvent` and
countdown device scheduling (mGBA, SameBoy); data-pointer chaining and
page-listed invalidation (iSH); debug switches that turn each optimisation
off (ish-arm64).

**Not applicable:**
whole-program register pinning via per-register gadgets (TCTI: code-size
explosion; iSH x86 can do it because i386 has 8 registers); embedding QEMU
or TCG (GPL, size, replaces a tested decoder); Dolphin's PowerPC specifics
(paired singles, Gekko caches); mGBA's no-MMU fetch shortcut (NEON must
honour the MMU); SameBoy's per-access device stepping (too costly at
hundreds of MHz); any JIT tier (dynarmic, QEMU TCG) in the product.

**Expected advantages** (relative to NEON's cached interpreter, estimates
to be measured, from the projects' reported gains):
fusion of the top pairs 5–20% on integer code; dead-flag elimination 5–15%
on Thumb-heavy userland (Thumb-1 ALU ops always set flags, most are never
read); direct threading 2–5%; native NZCV a few % on flag-setting code;
return cache a few % on call-heavy code (`calls` and `interp` are NEON's
slowest integer rows); device-time work up to the 18–24% the device refresh now costs; native
replacement of measured hot guest functions: large (e.g. 70 interpreted
instructions per pixel → one native call) but only on those functions.

---

## 4. NEON today

| Part | Where | State |
|---|---|---|
| Reference interpreter | `core/src/arm/arm_interp.c` (4.8 k lines) | ARMv6 + ARMv7, ARM, Thumb-1, Thumb-2; the specification, differential-tested against Unicorn |
| Decoder for the engine | `arm_ci_decode.c` (1.2 k) | ARM, ARM1176 Thumb, ARMv7 Thumb-2 → `ci_op_t` (16 bytes) |
| Cached interpreter | `arm_ci.c` (1.35 k) | blocks ≤ 64 ops, VA-keyed map, chained inside the executor, computed `goto` dispatch, reference fallback per op (`CI_K_REF`), verify mode |
| VFP / NEON SIMD | `vfp.c`, `neon.c` | common VFP forms decoded once; the rest through the reference |
| MMU | `core/src/mmu.c` | ARMv6 + ARMv7 short descriptors, 4096-entry TLB at 1 KiB, generation-flushed |
| RAM access | `arm_ci.c` `mem_rd`/`mem_wr` | separate read/write TLBs per privilege return host pointers; code writes bump a per-1 KiB generation |
| Static AArch64 engine | `a64_static_engine.c`, `tools/a64_static.c` | build-time generated, 3G machine, user mode, opt-in in the app |
| Runtime JIT | `core/src/jit/` | development only, excluded from the app |
| Devices / timing | `soc/machine.c`, `soc/n88.c` | exact timebase edges, event horizon, WFI fast-forward |
| Graphics | `soc/clcd.c` + app `VMFramePublication.c` | guest composites on the CPU; scanout to a UIKit view; MBX model experimental (3G); no SGX (3GS) |
| Audio | `soc/i2s.c`, `wm8991.c`, `pl080.c` → `VMAudioOutput.m` | 3G path to AudioQueue; 3GS codec not modelled |
| Boot | `boot/bringup.c`, `n88_boot` | direct kernel hand-off |

Measured (Xeon 2.8 GHz host, `BENCHMARK_RESULTS.md` §3): cached
115–193 M instr/s on integer rows, 35 on `mmu`, 42 on `vfp`; reference
44–63. iOS 6 boot on the 3GS: ~122 M instr/s, 8.6% of instructions on the
reference path. On an A9 (iPhone 6s): 85 M (ALU) / 66 M (load/store)
instr/s with the static engine. **No measurement on an A15 or newer.**

---

## 5. ARM on ARM64: what maps directly

AArch32 and AArch64 differ in encoding but agree on most integer
semantics, which a C engine can exploit through inline assembly or
compiler builtins on arm64 hosts (with a portable C path elsewhere).

| Guest | AArch64 | Exact? |
|---|---|---|
| `ADD/SUB/RSB/ADC/SBC` (no S) | `add/sub/adc/sbc` on W registers | yes |
| `ADDS/SUBS/CMP/CMN/ADCS/SBCS` | `adds/subs/adcs/sbcs` on W, then `mrs nzcv` | **yes: same NZCV, same borrow convention, same bit positions (31:28)** |
| `ANDS/TST/EORS/TEQ/ORRS/MOVS/MVNS/BICS` | `ands`/`tst` exist only for AND/BIC | no: AArch32 sets C from the shifter and keeps V; AArch64 `ands` clears C and V. Compute N,Z natively, merge C and V |
| `LSL/LSR/ASR #imm` operands | `lsl/lsr/asr` immediate | yes (except shifts by 32 and RRX, which the decoder normalises) |
| shifts **by register** | `lslv/lsrv/asrv` | no: AArch64 uses amount mod 32; AArch32 uses the bottom byte (32..255 → 0 or sign). Clamp first |
| `ROR` | `ror`/`rorv` | yes for the value; carry-out separate |
| `MUL/MLA/UMULL/SMULL/UMLAL/SMLAL` | `mul/madd/umull/smull/umaddl/smaddl` | yes (value); MULS flags N,Z only |
| `CLZ/REV/REV16/REVSH/RBIT/SXTB/UXTH…` | same mnemonics | yes |
| `SDIV/UDIV` (ARMv7-R/VE; not A8) | `sdiv/udiv` | n/a on the 3GS |
| Conditions EQ…LE | identical codes and NZCV meaning | yes: `msr nzcv` then `b.cond`/`csel` evaluates any guest condition |
| `LDR/STR/LDRB/LDRH/LDRSB/LDRSH` | `ldr/str/ldrb/...` through the TLB host pointer | yes (little-endian both; unaligned allowed on the host; guest alignment checks stay in the engine) |
| `LDM/STM`, `PUSH/POP` | `ldp/stp` pairs | yes when the run stays on one page |
| `B/BL/BX` | engine control flow | n/a (stay in the threaded dispatcher) |

**Registers.** Keep the guest's 16 registers in `arm_cpu_t.r[]` addressed
from one pinned base. Each access is an L1 hit, and the compiler already
keeps the base and the op pointer in registers inside `exec_block`.
Pinning guest registers into host registers is what makes TCTI's gadget
count explode (§2.8) or needs a register allocator (a JIT). Not worth it.

**Flags.** Two complementary changes. (1) On arm64 hosts, compute
arithmetic flags with the host instruction and one `mrs`, replacing the
64-bit add and the overflow expression in `add_flags`. (2) Better: do not
compute flags nobody reads. A block-level backward pass marks, for each
flag-setting op, whether N/Z/C/V are read before being overwritten within
the block; ops whose flags are dead at a block exit that ends in a direct
branch to a block that overwrites them first can skip them. Exits to
unknown code must keep flags live. iCube's validate twin is the test.

---

## 6. Is "full speed" reachable?

Guest rates (estimates, not measurements):

- iPhone 3G, ARM1176 at 412 MHz, single issue: at most 412 M instr/s,
  realistically ~200–350 M on real code (cache misses, multi-cycle loads).
- iPhone 3GS, Cortex-A8 at 600 MHz, dual issue: peak 1.2 G, realistically
  ~400–700 M.

The guest is idle much of the time (WFI is fast-forwarded), so what matters
is the rate while busy. Evidence for the host side:

- NEON cached interpreter: 115–193 M/s on integer code on a 2.8 GHz Xeon;
  ~122 M/s through the iOS 6 boot.
- ish-arm64 (same-architecture threaded code): ~6.5x slower than native on
  compute-bound C, as reported.
- A current A-series P-core runs integer code roughly 2x faster per core
  than this Xeon (published single-thread scores, not measured here).

So the cached interpreter on a current phone is plausibly ~230–400 M/s on
integer code: **about the iPhone 3G's busy rate, below the 3GS's.** The
uncertainty is large and the device number has to be measured (Stage 1).
Closing the remaining 3GS gap without JIT means the optimisations below
*plus* native replacement of the hottest guest functions, which removes
interpreted work instead of speeding it up.

---

## 7. Staged plan

Every stage keeps the reference interpreter as the oracle and fallback,
adds its change behind a switch that can turn it off, passes `ctest` and
the strict build, re-runs the 3G SpringBoard boot and the 3GS 10B500 boot
to the same console milestones, and lands as its own commit with an A/B
measurement in `BENCHMARK_RESULTS.md`.

| Stage | Status | Next work | Accept when |
|---|---|---|---|
| 1. Instrument and benchmark | **mostly done**: `cpubench`, engine stats, `boot3gs` stats, profiler | Run `cpubench` and the boot on a current iPhone (A15+) with the shipping build; record M instr/s per row and per boot phase | Device numbers in `device-benchmark.md` |
| 2. Find bottlenecks | **done for host** (§4 of `BENCHMARK_RESULTS.md`) | Repeat the profile at the current head and on the 3GS boot; op-kind pair counts for fusion candidates | Ranked list of op pairs and reference-path classes |
| 3. Decoded instruction cache | **done** (`ci_op_t`) | Move the remaining hot `CI_K_REF` classes (8.6% in the iOS 6 boot) to decoded handlers, starting with the most frequent | Reference share halved on the boot |
| 4. Basic-block cache | **done** (VA map, generations, chaining) | Return-address cache for `BX LR` / `POP {..., PC}` / `LDR PC` returns | `calls` row +X%, no digest change |
| 5. Threaded dispatch | **done** (replicated computed `goto`) | Direct threading: store the handler address in the op record at decode; fusion of the top pairs (CMP+Bcc, LDR+CMP, ADD+LDR, MOV+MOV) with exact fault PCs; dead-flag elimination with a validate mode | Each with an A/B gain and a validate run over the boot |
| 6. Precompiled ARM64 handlers | experiment | Native NZCV in the arithmetic handlers via inline asm on arm64 (C fallback elsewhere); optionally a small set of hand-written assembly handlers for the top 10 ops, *not* register-specialised | Measured gain on an arm64 host ≥ 5%, or dropped |
| 7. Faster memory translation | **mostly done** (host-pointer TLB) | LDM/STM page-run fast path; larger TLB measured (§12 shows the trade-off); fetch-side same-page continuation across chained blocks | `mmu` row gain |
| 8. Device scheduling | partly (event horizon) | Countdown-style device state (SameBoy): skip re-deriving unchanged levels; a single `nextEvent` compare in the run loop | Device refresh share < 10% |
| 9. Host-native subsystems | started (raster HLE on 3G, disk bridge, audio ring) | Prologue-verified native versions of the iOS 6 compositor's hot spans; later GL ES → Metal at the framework boundary | Frame time halved on the measured SpringBoard frames |
| 10. Compare with real hardware | not started | Time the same guest workloads (boot phases, a SpringBoard animation, a CPU benchmark app) on a real 3G/3GS and in NEON on a current phone | Ratio per workload published |

Order of value, by evidence: Stage 1 device numbers first (they decide how
far the CPU work needs to go), then Stage 5 fusion and dead flags, then
Stage 8, then Stage 9 for the compositor. Stage 6 is the speculative one.

---

## 8. High-level emulation, briefly

Already in NEON: the root disk as a host block device (SVC bridge), audio
into a host ring and AudioQueue, touch and buttons from UIKit queues, frame
publication to a UIKit view. Candidates, in order of expected payoff:

1. **The software compositor's inner loops** (QuartzCore span samplers and
   blends): native, byte-verified, pixel-exact, falling back to guest code
   on any fault. Biggest measured CPU sink in an iOS 6 UI with no GPU.
2. **Display scanout to Metal** instead of a UIKit image view, once frames
   arrive at full rate.
3. **Timers** stay modelled (the kernel's scheduler depends on exact
   behaviour); the host clock already paces them.
4. **GL ES → Metal** at the OpenGL ES framework boundary: large, only
   worth it after the SGX question is settled (`docs/IOS6_GRAPHICS.md`).
5. **Networking**: a host-socket backend behind the existing PPP/NAT core.
