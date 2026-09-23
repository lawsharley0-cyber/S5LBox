# iCube and Dolphin: how they execute without a JIT, verified against source

Research record for the S5LBox cached interpreter. Every claim below was read
in source, not taken from descriptions. It supersedes
`docs/ICUBE_TRANSLATION_RESEARCH.md` (added by `75ad89f`), which contains
statements the source contradicts: Dolphin has no `InterpreterOpInfo` cache,
`PPCCache` is the emulated instruction cache rather than the cached
interpreter, and Dolphin's JITs do not lift to an IR.

Sources examined (shallow clones, 2026-09-23):

| Project | Commit | Date |
|---|---|---|
| Dolphin upstream `dolphin-emu/dolphin` | `233b2dfe6d64` | 2026-09-21 |
| iCube `Provenance-Emu/iCube` | `0a5a3d5e41a4` | 2026-09-23 |

No code was copied. Dolphin and iCube are GPL-2.0-or-later; S5LBox is MIT.
Only architectural ideas are reused, re-expressed for ARM and for C11.

---

## 1. Upstream Dolphin

### 1.1 Plain interpreter
`Source/Core/Core/PowerPC/Interpreter/Interpreter.cpp:118`
`Interpreter::SingleStepInner()`: per instruction it checks HLE hooks, reads
the opcode through the MMU (`m_mmu.Read_Opcode`), looks the opcode up in
`PPCTables`, handles FPU-unavailable, and calls the handler through a function
table. PowerPC fields sit at fixed bit positions, so "decode" is cheap; the
per-instruction cost is the fetch/translation, the table lookup and the
exception checks.

### 1.2 Cached interpreter (callback tape)
`CachedInterpreter/CachedInterpreter.cpp`, `CachedInterpreterEmitter.{h,cpp}`
(479 + 107 + 40 lines).

- A block is compiled into a **byte tape of records**: a callback pointer
  followed by that callback's operand struct, written inline
  (`CachedInterpreterEmitter::Write`). No per-instruction heap allocation.
- Each callback returns the byte distance to the next record, or 0 to leave
  the block (`CachedInterpreterEmitter.h`, "32-bit return values seem to
  perform better than 64-bit ones").
- `ExecuteOneBlock()` walks the tape, with a direct compare for the most common
  callback (`Interpret<false>`) before the indirect call.
- Per instruction the tape usually holds `Interpret<>`, which **calls the same
  interpreter function** with the instruction word pre-fetched: the savings are
  fetch/translate, table lookup, per-instruction timing and exception checks —
  not decode.
- `EndBlock` subtracts the block's precomputed cycle cost from `downcount` and
  updates performance monitors once per block.
- Exceptions: loads/stores are wrapped in `InterpretAndCheckExceptions` only
  when `jo.memcheck` (MMU titles) requires it; `CheckFPU` is emitted once per
  block before the first FP instruction.
- `jo.enableBlocklink = false` in `CachedInterpreter::Init()`:
  **upstream's cached interpreter does not link blocks.**

### 1.3 Block discovery
`PPCAnalyst.cpp:806 PPCAnalyzer::Analyze()` walks from the entry PC, builds
`CodeOp`s (register use, flags, `canEndBlock`), and ends the block at branches
unless `OPTION_BRANCH_FOLLOW` / `OPTION_CONDITIONAL_CONTINUE`
(`PPCAnalyst.h:147-176`) are set. The cached interpreter sets none of these
options, so every upstream cached-interpreter block ends at its first branch.

### 1.4 Block cache and invalidation
`JitCommon/JitCache.{h,cpp}` (shared by all JIT back ends):
- Blocks are keyed by **effective address + `feature_flags`** (MSR IR/DR/FP
  etc.) and record their physical address (`JitBlockData`).
- `Dispatch()` (`JitCache.cpp:231`) uses a huge flat `fast_block_map`
  (`FAST_BLOCK_MAP_SIZE`, 4 GiB/4 × 8 × pointer, reserved address space) or a
  64 K-entry fallback indexed by `(flags<<30)|(pc>>2)`, then verifies the tag.
- **Invalidation is driven by the guest's `icbi` and by DMA**, through
  `InvalidateICache()` → `InvalidateICacheInternal()` (`JitCache.cpp:276/305`),
  using `ValidBlockBitSet` — one bit per 32-byte physical line — to make the
  common "no code here" case one bit test. PowerPC's instruction cache is not
  coherent with data writes, so correct guests must `icbi` after writing code;
  Dolphin relies on that rather than trapping every store.

### 1.5 Memory and fastmem
JIT back ends use a host "fastmem arena" (reserved 4 GiB mappings, faults
caught by a signal/exception handler) when `jo.fastmem`
(`JitCommon/JitBase.h:88`). The cached interpreter does not use it; it calls
the interpreter's memory functions.

### 1.6 Timing
`CoreTiming::Advance()` schedules events; the CPU runs whole blocks while
`downcount > 0` (`CachedInterpreter::Run`). Events therefore fire at **block**
granularity, not instruction granularity — acceptable for Dolphin, **not** for
S5LBox, whose runs are bit-exact against the reference interpreter's
instruction-exact device ticks.

---

## 2. iCube (what it inherited, what it changed)

iCube is a Dolphin fork with an iOS/tvOS app. Its jitless engine ("CPUCore 5")
is Dolphin's cached interpreter grown from ~1 k to **~13.4 k lines**
(`CachedInterpreter.cpp` 7,538; `CachedInterpreterIR.cpp` 3,483). The history
is in commits tagged `perf(ci)` / `perf(cir)` (2026-09-17 … 09-23) and in
`docs/superpowers/plans/2026-09-17-jitless-perf-plan.md` and
`docs/handoff-2026-09-23.md`.

**Inherited unchanged:** the callback-tape model, `JitBaseBlockCache` keyed by
effective address + flags with physical-address invalidation, PPCAnalyst,
CoreTiming, the Interpreter functions as the semantic fallback.

**Changed (verified in `CachedInterpreter.cpp`):**

| # | Technique | Source | What it does |
|---|---|---|---|
| I1 | Specialized load/store handlers | `LoadStoreFast` (l. 2867), `GetLoadStoreFastCallback`, `CI_ClassifyLoadStore` | One handler per (access kind, D/X form, update, write_pc) chosen **at emit time**; no opcode switch at run time; page lookup through a host-pointer table (null = MMIO). Plan doc: `lwz` went from ~40 host instructions to 24 |
| I2 | Integer micro-ops without decode | commits `620ef7a`, `73ea5ab`; `MicroOpHandlers` (l. 3509) | `li/lis/addi/…`, compares, multiplies, `mfspr/mtspr` LR/CTR executed from pre-extracted operands |
| I3 | Tail-call threading | `CI_MUSTTAIL` = `[[clang::musttail]]` (l. 189), `CI_CHAIN_EXIT` (l. 204) | Each record tail-calls the next, so every handler ends in its **own** indirect branch. The plan doc records why: LLVM compiled their computed-`goto` version to *one shared* `br` (it will not tail-duplicate an indirect branch with > 16 predecessors). Clang-only; other compilers fall back to returning to the loop |
| I4 | Record chaining | `WriteChainable`, tag bit in the callback slot, `s_chain_base` | Consecutive chain-capable records run without returning to `ExecuteOneBlock` |
| I5 | Fused pairs | `MicroOpHandlers::GetPair`, commit `99b1b39` (compare + conditional branch), `84c5517` | Common adjacent pairs become one record |
| I6 | Static block linking | `LinkBlock` (l. 2095), commits `6979b4b`, `14b5126`, `4ae253c` | A terminal record tail-calls the successor block when valid |
| I7 | Dynamic link inline cache | `LinkBlockOperands::dyn_*`, `BumpDynLinkGeneration` (l. 2315) | Per exit site: last successor for `blr/bctr`, validated by pc + flags + a global generation bumped on every block destruction |
| I8 | Global indirect-target cache | `CIRTargetCacheEntry`, `kCIRTargetCacheBits = 12` (l. 454) | Direct-mapped `(pc, flags) → entry` table consulted when the per-site slot misses; `blr` hit rate 73 % → 99.6 % (commit `94fea7a`) |
| I9 | Long blocks | `ContinueIfNpc` (l. 3239), commit `6ab05ee` | Turns on `OPTION_CONDITIONAL_CONTINUE` + `OPTION_BRANCH_FOLLOW`; a not-taken conditional branch continues inside the block |
| I10 | Hot-block profiler, dispatch census | commits `8ed6f3a`, `f80f1e9` | Guest hot blocks, per-opcode generic-dispatch counts, link hit/miss counters — off by default because it costs ~85 % |
| I11 | IR tier ("engine 6") | `CachedInterpreterIR.{h,cpp}` | Typed IR nodes executed by an IR interpreter. **Their own measurement: ~1.7× slower than the specialized cached interpreter; "do not invest there."** |
| I12 | Game-specific fusions | gather-pipe copy fusion (`9d8abe6`), paired-single quantization | GameCube hardware idioms |
| I13 | JIT via debugger/TXM | `Jit/StikDebugLauncher.swift`, `MemoryUtil_iOS_LuckTXM.cpp` | Needs a debugger attached; not an interpreter technique |

**iCube's measured results (their documents, on device):** dynamic block links
+5 % to +8 %; indirect-target cache 73 % → 99.6 % hit rate but **only ~+1.1 %
throughput**; after specialization their four slow titles reached full speed
with "0.00 % generic dispatch". Their 2026-09-23 conclusion: *"stop optimizing
the interpreter"* — the remaining candidates are smaller than their ~1.5 %
measurement error. Their method (same-session device A/B, thermal control,
before/after engine verification) is the same discipline this repository
already enforces in `hotpath.md`.

**The central lesson, consistent across both projects and with S5LBox's own
rejected experiments (`hotpath.md` r470–r484):** caching decode and calling
the same generic per-instruction helpers yields little. iCube's large wins came
from *specialized handlers with pre-extracted operands* for the hot
instruction forms (I1, I2), and from not returning to a central dispatcher
(I3–I6). Linking and target caches are second-order.

---

## 3. Applicability to an ARMv6 guest (ARM1176, iPhone OS 3)

Differences that matter:
- ARM has **conditional execution on almost every instruction** and a barrel
  shifter in every data-processing operand; PowerPC does not. Specialization
  must cover condition × shifter forms without exploding handler count.
- ARM **reads PC as an operand** (`pc+8` / `pc+4`), so PC-relative operands
  can be folded to constants at decode time.
- **Two instruction sets** (ARM/Thumb) switch at run time (`BX`, `BLX`, `LDR pc`,
  `POP {pc}`, exception return). The cache key must include `CPSR.T`.
- ARM1176 has **no coherence** between D-side writes and the I-cache either,
  but S5LBox's reference interpreter *is* coherent (it fetches from RAM every
  time), and bit-exactness against it is the acceptance criterion. So S5LBox
  must detect code writes itself (write tracking), and may use the guest's
  I-cache maintenance (`MCR p15, c7`) only as an additional safety net.
- S5LBox's device time is **instruction-exact** (timebase edges every
  ≈ `cpu_hz/tb_hz` retirements). Dolphin-style block-granular downcount is not
  acceptable; blocks must be cut at the exact edge.
- Little-endian guest on little-endian hosts: no byte swapping.
- The guest's MMU (ARMv6 short descriptors, 1 KB legacy subpages, XN,
  domains, `TTBCR` split) replaces BATs; translations are already cached by a
  1 KB-granular software TLB validated by `tlb_gen`.

| Technique | Classification | Plan for S5LBox |
|---|---|---|
| Callback/record tape, no per-instruction allocation (Dolphin 1.2) | **DIRECTLY APPLICABLE** | Fixed-size 16-byte op records in a pooled arena |
| Pre-fetch + skip per-instruction translation inside a block | **DIRECTLY APPLICABLE** | One fetch translation per block; blocks never cross a 1 KB fetch subpage |
| Block-level cycle accounting | **ADAPTABLE** | Count retirements locally, but honour the machine's exact per-edge budget (block cut mid-way) |
| Blocks keyed by effective address + mode flags, physical address for invalidation (1.4) | **ADAPTABLE** | Key by physical address + VA + `CPSR.T`; decode is privilege-independent |
| `ValidBlockBitSet` physical-line bitmap (1.4) | **ADAPTABLE** | One bit per 1 KB physical block; used to refuse direct-write host pointers and to invalidate on bus writes |
| Invalidation via guest cache maintenance (`icbi`) | **ADAPTABLE** (safety net only) | `MCR c7` I-cache ops invalidate; primary mechanism is write tracking |
| Specialized handlers with pre-extracted operands (I1, I2) | **DIRECTLY APPLICABLE — the main lever** | ARM data processing (imm / shifted-reg forms, S/non-S), loads/stores, branches, multiplies, extends; Thumb equivalents |
| Host-pointer page lookup, null = MMIO (I1) | **DIRECTLY APPLICABLE** | Per-engine read/write host TLB at 1 KB granularity, tagged by `tlb_gen` + privilege; MMIO ends the block |
| Direct compare for the commonest callback (1.2) | **ADAPTABLE** | Superseded by threaded dispatch; measured, not assumed |
| Computed-`goto` threading | **ADAPTABLE** (GCC/Clang only) | Optional, with a `switch` fallback that MSVC always uses; both tested |
| Tail-call threading with `musttail` (I3) | **EXPERIMENTAL** | Clang-only; candidate ARM64/Apple optimisation after measurement |
| Fused pairs (I5) | **EXPERIMENTAL** | e.g. `CMP`+`B<cond>`; only if the profile shows the pair hot |
| Static block linking (I6) | **ADAPTABLE** | Link direct successors in the engine's internal dispatch loop, still checking IRQ/budget/translation per block |
| Dynamic link / global target cache (I7, I8) | **EXPERIMENTAL** | iCube measured ~1 %; only after profiling shows indirect-branch lookups hot |
| Long blocks / conditional continue (I9) | **EXPERIMENTAL** | ARM conditional instructions already continue in-block; branch following later |
| Hot-block profiler (I10) | **DIRECTLY APPLICABLE** | Counters behind a runtime flag; never fabricated |
| IR tier (I11) | **NOT APPLICABLE as a speed path** | Measured slower in iCube; S5LBox keeps the predecoded op record as its "IR" and benchmarks any IR experiment against it |
| Fastmem arena with signal handlers (1.5) | **NOT APPLICABLE** | Sandbox/portability risk; software host-TLB instead |
| Gather-pipe / paired-single fusions (I12) | **NOT APPLICABLE** | GameCube-specific |
| JIT via TXM/debugger (I13) | **NOT APPLICABLE** (out of scope; JIT must stay optional) | — |
| Block-granular timing (1.6) | **NOT APPLICABLE** | Would break instruction-exact device time |

## 4. What S5LBox's own history adds

`hotpath.md` r470–r484 rejected three portable block caches that called the
existing per-instruction semantic helpers (−4 % to −22 % on the restored
SpringBoard interval) and a memoised decode table (−20 %). The explanation
recorded there — branch-predicted decode chains are cheap; cache lookup,
validation, uop dispatch *plus* the same helper calls are not — is the same
lesson iCube's specialization work teaches. The S5LBox engine is therefore
designed so that **the hot instruction forms never call the generic helpers**,
the dispatcher stays inside the engine across blocks, and every experiment is
judged on app-shaped `s5l8900_run()` measurements, not on MMU-off loops.
