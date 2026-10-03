# Performance baseline and measurement plan

What is measured, how, on which machine, and which numbers exist today. The
rule this repository already follows is kept: **a number without its host,
build, workload and method is not a result, and no number is estimated or
invented.** Complements `PERFORMANCE_BASELINE.md` (2026-09-18 snapshot of the
historical physical-device figures) and `hotpath.md` (the experiment log).

## 1. Baseline identity

| Item | Value |
|---|---|
| Baseline tag | `baseline-pre-cached-interpreter` → `4a8480f9db6a` |
| Last commit before the unsound tiers | `326ae10` (reference interpreter identical in speed, §3) |
| Build | CMake Release (`-O3 -DNDEBUG`), no LTO, Ninja |
| Session host | Linux 6.18 x86-64 cloud container, Intel Xeon @ 2.80 GHz, 4 vCPU, 15 GiB — **not** the user's Windows 11 PC and not a phone |
| Compilers available | GCC 13.3.0 (numbers below), Clang 18.1.3, lld 18 (ARM cross-compilation of guest workloads) |
| Windows evidence | CI `windows-latest`, MSVC, run `35906727171`: build + 77 tests pass |
| Guest firmware | **none available to this session** (user-supplied, never downloaded) |
| Guest device profile | iPhone1,2 (S5L8900, ARM1176JZF-S), iPhone OS 3.1.3 7E18 |

## 2. Baseline behaviour (from the repository's own evidence; not re-run here)

Real-firmware runs need the user's firmware, so this session cannot re-run
them. The state recorded by the project (README, `QUALITY.md`, `BOOTLOG.md`):

| Question | Recorded answer |
|---|---|
| Does the guest boot? | Yes: XNU boots, drivers start, root FS mounts, `launchd` and daemons run |
| How far? | Lock screen and home screen; slide-to-unlock and a tap work |
| SpringBoard? | Yes (software compositor; MBX experimental) |
| Touch/input? | One finger end to end; buttons; two-finger reaches userspace only |
| Graphics? | CLCD scanout of guest-composited frames; 0–4 fps foreground on device; no 30 fps arm |
| Audio? | Modelled; not audibly established; AMC/BSU freeze on device |
| Apps launch? | Stock apps (e.g. Settings) launch; third-party compatibility uncharacterised |
| Crashes | See `CURRENT_ARCHITECTURE.md` §8 |

## 3. Baseline numbers measured in this session

`ctest`: **77/77 pass** at `4a8480f` (3.5 s wall). Pre-tier `326ae10`: 72/72.

`insnbench --insns 20000000 --reps 3` at `4a8480f` (median M guest
instructions/s; synthetic loops; see `tools/insnbench.c` for exact code):

| Row | Median |
|---|---:|
| alu/branch, MMU off, no tick (literal `arm_step`) | 85.34 |
| load/store, MMU off, no tick | 68.35 |
| load/store, MMU off, tick | 49.13 |
| load/store, 1 MB sections | 64.47 |
| load/store, 4 KB pages | 64.24 |
| load/store, 4 KB pages, tick | 47.84 |
| **alu/branch, 4 KB pages, `s5l8900_run`** | **52.88** |
| **load/store, 4 KB pages, `s5l8900_run`** | **37.42** |
| alu/branch, 4 KB, `s5l8900_run` in User mode (tick batching) | 65.51 |
| load/store, 4 KB, `s5l8900_run` in User mode | 50.89 |
| alu/branch Thumb, MMU off | 97.44 |
| alu/branch Thumb, 4 KB, tick | 66.42 |
| mixed ×10, MMU off / 4 KB + tick | 62.39 / 41.73 |
| VFP mul/add/cvt, MMU off / 4 KB + tick | 47.21 / 38.61 |
| VFP ldr/str, MMU off / 4 KB + tick | 54.45 / 38.22 |
| ldm/stm ×4, MMU off / 4 KB + tick | 33.32 / 25.74 |

Pre-tier vs HEAD, `tick=run` rows, interleaved twice: alu/branch 51.13/52.23
vs 51.63/48.66; load/store 40.09/41.57 vs 41.21/41.99 — no difference beyond
noise. (The `CACHED`/`IR` rows insnbench also prints are omitted: they run a
known-incorrect engine outside the machine loop; `CURRENT_ARCHITECTURE.md` §7.)

The historical real-guest figures (≈15–17 M instr/s restored SpringBoard on
desktop, 6.5–7 M instr/s literal on an A9, 0–4 fps foreground on device) are
in `PERFORMANCE_BASELINE.md` and `hotpath.md` and are not re-stated as new.

## 4. Measurement rules for this effort

1. **Release builds only** (`-O3`, and LTO where the comparison is about the
   iOS binary, which is built `-O3` + LTO). Debug numbers are never compared.
2. **Same binary, runtime switch** for A/B wherever possible
   (`S5L8900_CPU_BACKEND_INTERPRETER` vs the cached interpreter), so layout
   and compiler differences cannot masquerade as engine differences.
3. **Interleaved repetitions, median headline**, as `insnbench` does; for
   separate processes use symmetric `old,new,new,old` ordering (`hotpath.md`
   r454).
4. **Every run is correctness-checked**: end-state witnesses for benchmarks;
   final CPU/RAM digest equality between engines for compiled workloads; for
   real-guest runs, work-image and framebuffer SHA-256 equality (the
   `hotpath.md` method).
5. **App-shaped path first**: numbers through `s5l8900_run()` with the MMU on
   and device ticks live decide; MMU-off literal loops are reported only as
   upper bounds.
6. **Never** present a desktop number as phone FPS, and never convert
   instructions/s to FPS.

## 5. Benchmarks to build (Phase: benchmark harness)

`insnbench` keeps its synthetic rows. A new **compiled guest workload suite**
(`bench/guest/*.c`, cross-compiled once with Clang to bare-metal ARMv6 ARM and
Thumb, committed as generated byte arrays so Windows needs no ARM toolchain)
runs through `s5l8900_run()` on a real `s5l8900_t` with the MMU on (4 KB
pages), SVC/User modes and live device ticks. Each workload self-checks and
reports a result word; both engines must produce identical CPU state, RAM
digest and cycle count.

| Workload | Emphasis | Why |
|---|---|---|
| `bignum` | 32×32→64 multiply-accumulate loops | shape of `_mulg_common`, the hottest measured guest code |
| `crc32` / `adler` | shifts, logic, byte loads | integer ALU + flag-free loops |
| `sha1` | rotates, adds, word loads | `_SHA1Init` boot cost |
| `memops` | `memcpy`/`memset`/`strlen` | loads/stores, LDM/STM |
| `sort` | branches, compares, calls | control flow, conditionals |
| `raster` | fixed-point scanline fill/blend | shape of `CA::OGL::sw_scanline` |
| `calls` | deep call/return, recursion | BL/BX/`POP {pc}` indirect returns |
| `mmu` | strided accesses across many 4 KB pages | TLB/host-TLB pressure |
| `svc` | SVC + handler + exception return in a loop | exception entry/exit |
| `vfp` | scalar float kernel (hardfloat VFPv2) | 17 % of the steady-state mix is VFP |

All built both `-marm` and `-mthumb` (Thumb-1, since the ARM1176 has no
Thumb-2).

## 6. Profiling plan

- **Engine counters** (runtime-enabled, zero-cost when off): blocks built,
  lookups, hits, misses, invalidations (by cause), average block length,
  executions per block, exits by reason (branch, indirect, budget, MMIO,
  state change, fallback-step), specialized vs reference-fallback
  instructions per class, host-TLB hits/misses, MMU walks, aborts, IRQs.
- **Hot guest PCs / blocks**: sampled counters in the engine; for real guests
  `bootkernel -W` and `--sequence-profile` already exist.
- **Host profiles**: Linux `perf` in this session; on Windows, Visual Studio
  Performance Profiler (CPU Usage) or WPR/WPA on a Release build of
  `cpubench.exe` / `bootkernel.exe --run-api`; on the phone, Instruments
  Time Profiler (see `MAC_VALIDATION.md`).
- **Subsystem split (CPU/MMU/GPU/audio/devices)**: taken from sampled host
  profiles by symbol, never from fixed ratios.

## 7. Real-guest A/B procedure (for the user's Windows PC with firmware)

From a Release build, with a restored checkpoint so boot variance is excluded
(the `hotpath.md` method):

```
bootkernel firmware/kernel.macho -d firmware/devicetree.bin ^
  --restore work/<checkpoint>.bin --run-api --fast -n <start+100000000> ^
  --cpu-backend interp
bootkernel ... same ... --cpu-backend cached
```

Run `interp, cached, cached, interp`; compare the reported timed rate; compare
work-image and framebuffer SHA-256 between all four runs (must be identical).
Exact flags are listed by `bootkernel --help`; `docs/WINDOWS_BUILD.md` has the
build commands.
