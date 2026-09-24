# CPU benchmark results

Measured results only. Method: `docs/PERFORMANCE_BASELINE_PLAN.md`. Every row
below was produced by `cpubench` (compiled ARMv6 guest workloads through
`s5l8900_run()`, MMU on with 4 KiB pages and VA ≠ PA, device ticks live,
direct RAM writes on, 100 000-instruction chunks) and verified: the guest's
checksum equals the host-computed one, and all backends in a row end in the
same architectural state (CPU + 16 MiB of guest DRAM digest).

Host for every table in this file unless stated: Linux x86-64 cloud container,
Intel Xeon @ 2.80 GHz, 4 vCPU; GCC 13.3.0, CMake Release (`-O3`), no LTO.
These are **desktop numbers**; they are not phone numbers and not FPS.

## 1. Baseline: reference interpreter (`arm_step`), commit `7143034`

`cpubench --reps 3 --mode user,svc` — median M guest instructions/s.

| Workload | ARM User | ARM SVC | Thumb User | Thumb SVC |
|---|---:|---:|---:|---:|
| bignum | 50.90 | 39.18 | 54.31 | 41.48 |
| crc32 | 55.49 | 44.15 | 64.49 | 50.38 |
| sha1 | 55.33 | 45.17 | 56.87 | 42.37 |
| memops | 56.52 | 43.98 | 60.40 | 44.49 |
| sort | 52.51 | 44.39 | 56.68 | 43.03 |
| raster | 55.91 | 42.75 | 53.70 | 43.79 |
| calls | 42.37 | 33.97 | 52.31 | 40.92 |
| mmu | 22.49 | 20.79 | 30.51 | 26.05 |
| interp | 47.83 | 40.69 | 59.51 | 45.37 |
| vfp | 36.65 | 30.91 | — | — |
| svc | 34.68 | — | 36.93 | — |

*Corrected 2026-09-23:* the first version of this table (commit `554c587`)
showed each row's **best** repetition under a "median" heading. The values
above are the medians of the same run.

SVC-mode rows are slower because the reference run loop batches device ticks
only in User mode (`interpreter_tick_batch_limit`): privileged code pays one
`s5l8900_tick()` per instruction.

## 2. Where the reference interpreter's host time goes

`gprof` flat profile (`-pg`, Release) over one full `cpubench --reps 1 --mode
user,svc` run (1.42 G `arm_step` calls). Self time by function, grouped.
`-pg` instrumentation inflates small, frequently called functions, so treat
the split as indicative, not exact.

| Group | Functions (self %) | Share |
|---|---|---:|
| Instruction interpretation | `arm_step` 29.8, `thumb_step` 13.4, `exec_data_processing` 5.3, `barrel_shift` 1.3, `arm_cond_passed` 1.2, `exec_block_transfer` 0.8, VFP (`f32_do`, `vfp_execute_inner`) 1.4, other 0.2 | ≈ 53 % |
| Device time (`s5l8900_tick` and the refresh graph) | `s5l8900_tick` 9.9, `s5l_vic_set_line` 2.7, GPIO-IC 2.6, buttons 0.7, GPIO 0.6, DMA/SPI/PMU/TV-out ≈ 1.8 | ≈ 18 % |
| Run loop | `s5l8900_run` 9.4 | ≈ 9 % |
| Memory and MMU | `bus_read` 6.3, `mem_r32_as` 3.4, `mem_w32_as` 1.8, `arm_mmu_translate` 1.5, `mem_r8_as` 0.5, other 0.9 | ≈ 14 % |
| Harness (host checksum, digest; outside timed region) | `wl_run`, `gb_run` | ≈ 3 % |

`s5l8900_tick` was called 713 M times for 1.42 G instructions. Consequences
for the engine design: removing per-instruction decode/dispatch addresses
roughly half of the time; the per-instruction tick in privileged mode and the
run-loop overhead are a further quarter, which an engine that honours the
exact batch limit in *all* modes (not only User) can also remove.

## 3. Cached interpreter vs reference, current head `a50b124`

`cpubench --backend interp,cached --reps 5 --mode user,svc`, same host and
build as §1. Each cell: reference → cached median M guest instructions/s
(ratio). Every row reported the host checksum and the **same final
architectural state digest (CPU + 16 MiB DRAM) on both backends**; the run
exits non-zero otherwise (`CPUBENCH-DONE failures=0`).

| Workload | ARM User | ARM SVC | Thumb User | Thumb SVC |
|---|---:|---:|---:|---:|
| bignum | 50.0 → 153.5 (3.07×) | 43.0 → 158.1 (3.68×) | 57.3 → 164.9 (2.88×) | 47.5 → 156.3 (3.29×) |
| crc32 | 54.9 → 180.2 (3.28×) | 46.2 → 176.6 (3.82×) | 63.3 → 159.2 (2.51×) | 52.8 → 156.3 (2.96×) |
| sha1 | 51.9 → 169.8 (3.27×) | 44.5 → 170.4 (3.83×) | 61.3 → 159.6 (2.60×) | 47.8 → 154.0 (3.23×) |
| memops | 53.2 → 160.5 (3.02×) | 45.5 → 156.3 (3.44×) | 62.0 → 157.3 (2.54×) | 52.4 → 151.3 (2.89×) |
| sort | 53.1 → 136.2 (2.57×) | 43.6 → 133.7 (3.07×) | 60.4 → 143.6 (2.38×) | 49.7 → 140.8 (2.83×) |
| raster | 55.9 → 193.5 (3.46×) | 47.5 → 193.3 (4.07×) | 59.5 → 164.1 (2.76×) | 46.2 → 161.8 (3.50×) |
| calls | 44.4 → 119.0 (2.68×) | 37.5 → 117.3 (3.13×) | 55.3 → 120.0 (2.17×) | 46.2 → 115.5 (2.50×) |
| mmu | 24.3 → 35.1 (1.45×) | 21.5 → 33.0 (1.53×) | 31.7 → 44.1 (1.39×) | 28.5 → 43.6 (1.53×) |
| interp | 45.8 → 98.8 (2.16×) | 38.7 → 94.8 (2.45×) | 60.7 → 115.7 (1.91×) | 45.4 → 110.5 (2.44×) |
| vfp | 37.0 → 42.0 (1.13×) | 32.8 → 43.1 (1.32×) | — | — |
| svc | 32.7 → 75.6 (2.31×) | — | 37.9 → 82.3 (2.17×) | — |

**Geomean over the 40 rows: 2.564×.** The reference interpreter did not
regress: the geomean of its 40 rows was 43.66 M/s in the baseline run at
`7143034` and 44.83, 44.49, 44.57 and 45.42 M/s in the runs at `962aa8b`,
`373d674`, `e06c8e7` and `a50b124`. Single rows move by up to about ±5 %
between runs of the same code on this shared host.

Where it gains least, and why (profiles in §4): `vfp` runs every VFP
instruction through the reference in-block; `mmu` is page-walk bound (a
host-TLB miss costs a full `arm_mmu_translate`); `interp` and `calls` have
short blocks and many indirect branches.

### Step by step

Each step was measured against the one before in the same session with
interleaved runs; all rows kept identical state digests.

| Commit | Change | Geomean vs reference | Step gain (A/B method) |
|---|---|---:|---|
| `962aa8b` | engine: predecoded blocks, specialised DP / load-store / branch / multiply / extend handlers, reference in-block for the rest, exact run-loop budget | 2.072× (2.152× in an earlier run) | — |
| `373d674` | LDM/STM, Thumb PUSH/POP/LDMIA/STMIA fast path | 2.353× | calls ARM User 1.37× → 2.28× (same-binary ratio) |
| `e06c8e7` | VA-keyed block map (skip fetch translation + hash) | 2.456× | binary A/B vs `373d674`, 3×3 reps: calls ARM +15 %, sha1 +6–7 %, mmu +1–4 % |
| `a50b124` | threaded dispatch (computed goto) on GCC/Clang | 2.564× (5 reps) | binary A/B vs `e06c8e7`, 2×3 reps, 40 rows: geomean ×1.053; switch form ×0.999 |

Rejected after measurement: force-inlining the block executor into the run
loop (neutral to −6 % on `sort`/`calls`/`interp`; not committed).

## 4. Where the cached interpreter's host time goes

callgrind (instruction counts, `-O3 -g`), `cpubench --backend cached --reps 1
--div 10`, one workload each. Harness set-up (`gb_run`, `memset` of guest
RAM) excluded from the shares below.

| Workload | `exec_block` | `arm_ci_run` (block lookup) | device refresh (`s5l8900_tick` + VIC/GPIO-IC/DMA/SPI/buttons) | MMU walk (`arm_mmu_translate`, `tlb_fill`, `bus_read`) |
|---|---:|---:|---:|---:|
| calls ARM User | 43 % | 28 % | ≈ 18 % | ≈ 4 % |
| sha1 Thumb SVC | 54 % | 14 % | ≈ 24 % | < 1 % |
| mmu ARM User | 25 % | 11 % | ≈ 6 % | ≈ 49 % |

These were taken at `373d674`, before the block map and threaded dispatch,
which target the first two columns. The device refresh runs once per
timebase edge (every ≈68 instructions at 412:6 MHz) and is untouched by this
work; it is now the largest single cost outside the executor. Reducing it is
device-model work (skipping idempotent re-derivation when no input changed)
and needs its own proof that no interrupt edge moves.

## 5. Other hosts (CI runners, commit `a50b124`)

The `core-tests` workflow's ungated `CPU engine A/B` step: `cpubench
--backend interp,cached --reps 1 --div 4 --mode user,svc`. Shared, virtualised
runners and one repetition: the ratios are indicative, the absolute rates are
not comparable across rows. All three reported `failures=0` (identical state
digests on every row).

| Runner | Compiler / dispatch | Geomean vs reference |
|---|---|---:|
| `ubuntu-latest` x86-64 | GCC, computed goto | 2.659× |
| `windows-latest` x86-64 | MSVC (VS generator), switch | 2.501× |
| `macos-latest` Apple Silicon arm64 | Apple clang, computed goto | 2.936× |

The arm64 runner is the closest available proxy for an iPhone's CPU core; a
phone measurement is still required (`MAC_VALIDATION.md` §4) and nothing here
is a frame rate.

## 6. With the active host clock (`--active-clock`)

> **Correction (2026-09-24).** This section first said the iOS app runs this
> way. It does not: `app/project.yml` deliberately leaves
> `S5LBOX_IOS_ACTIVE_REALTIME_CLOCK` undefined (a same-checkpoint A/B showed
> slow unlock work losing a race with the guest's own sleep deadline), so the
> app runs **exact** device time plus WFI pacing, the mode of §3, and a device
> report confirmed it: 114.6 M engine runs of 68 instructions on average. The
> numbers below are what the active clock gives; §9 is what the app now gets.

`s5l8900_set_active_host_clock` makes guest time follow the host's monotonic
clock, the engine's batches are up to 256 instructions, and the device graph
is refreshed per host-clock sample (every 4,096 instructions) or on a device
access, instead of at every timebase edge. `cpubench --active-clock` runs
the same workloads that way. Same host and build as §3, commit after
`3a9ddef`, `--reps 3 --mode user,svc`, all 40 rows `failures=0`:

**Geomean 3.295× over the reference** (per row 1.14× `vfp` … 5.79× `raster`
ARM SVC; e.g. bignum ARM User 58.7 → 280.6, sha1 ARM User 62.1 → 313.8,
calls ARM User 48.9 → 173.8, mmu ARM User 25.3 → 37.2 M instr/s).

The gain is larger than in exact mode (§3, 2.56×) because the per-edge
device refresh that dominated there is mostly gone. On the phone, the
default ("Standard") backend is the reference interpreter plus the compact
build-time AArch64 engine, which `hotpath.md` measured about 6 % faster
than the interpreter alone; the cached interpreter replaces both. A real
boot also spends time the workloads do not: exceptions, SVCs, CP15 work,
device accesses and host-side frame work. The app's Performance & Sound
Details now reports the engine's share and why instructions leave it
(`arm_ci_describe_stats`), which is the measurement that decides the next
optimisation.

## 7. VFP straight to the unit (commit after `4b4fb61`)

The engine used to hand every VFP instruction to `arm_exec_arm_insn`, whose
decode tree (about 24 % of host time in the `vfp` workload, callgrind) only
leads to `vfp_execute`. VFP records are now `CI_K_VFP` and call
`arm_exec_vfp_insn`, the reference's own tail for that path (cycles, the
unit, data-abort completion, the lazy-enable Undefined rule, r15). The
differential fuzzer gained VFP generators (every group the unit decodes, a
varied FPSCR and sometimes VFP disabled): 400,000 runs, 0 mismatches; two
deliberate bugs in the new helper were caught (242 and 1,166 mismatches in
40,000 runs). Same host as §3, `cpubench --workload vfp --isa arm`, medians
of 7 (exact) and 5 (active clock), M instr/s:

| Row | Before | After |
|---|---:|---:|
| vfp ARM User, exact | 44.8 | 55.8 |
| vfp ARM SVC, exact | 46.8 | 56.4 |
| vfp ARM User, `--active-clock` | 51.3 | 64.8 |
| vfp ARM SVC, `--active-clock` | 51.8 | 65.5 |

About +22–26 %. What remains per VFP instruction (~250 host instructions)
is the unit itself: `vfp_execute_inner` 31 %, `f32_do` 7 %, and VLDR/VLDM
reaching memory through `mem_r32_as` (9 %, no host-TLB fast path yet).

## 8. Writes to the PC and the PIC idioms in-engine

Four forms that compiled iPhone OS code uses constantly were reference
records: `LDR pc, [...]` (library-call stubs, `ldr pc, [sp], #4` returns,
`ldrls pc, [pc, rN, lsl #2]` switch tables), `MOV pc, Rm`, Thumb `ADD/MOV
pc, Rm`, and the PIC address idioms ARM `ADD/SUB Rd, pc, Rm` and Thumb
`ADD/MOV Rd, pc`. They now run in the engine (`CI_K_LDR_PC`, `CI_K_JMP`, and
constant-operand data processing); alignment faults, UNPREDICTABLE targets,
device or unmapped addresses and every other form still go to the reference.

The fuzzer gained generators for these forms and seeds data and registers
with code addresses so the loads land in code: 800,000 runs, 0 mismatches
(computed-goto dispatch) and 100,000 with the `switch` dispatch. Eleven
deliberate bugs in the new paths are each caught (54–425 mismatches per
40,000 runs; two of them were missed before the generators were added).

`cpubench --backend cached --reps 3 --mode user --active-clock`, best of two
medians, before → after (M instr/s): **interp ARM 131.4 → 174.8 (1.33×),
interp Thumb 155.1 → 210.2 (1.36×)**, calls Thumb 176.4 → 186.2; every other
row within ±5 %, which is this host's run-to-run noise (`mmu` moved ±10 % in
both directions on reruns). `vfp` measured about 7 % lower on reruns with
an identical host instruction count (callgrind 648.50 M vs 648.54 M Ir), so
that is code placement in this binary, not added work.

## 9. The event horizon: exact device time without a stop at every edge

In exact mode (the app's) every engine run ended at the next 6 MHz timebase
edge, about every 68 instructions, and each edge paid a full device refresh
plus an engine re-entry: about 22 % and 14 % of host time (callgrind, calls
and sha1, before this change). `s5l8900_run` now lets a cached-interpreter
run continue to the next edge at which an **enabled** interrupt source can
fire (the wake-source table WFI already uses; its devices advance
algebraically), while no device is dirty and the PL080s and SPI ports are
idle, capped at 16,384 instructions. Inside the run, every device access
first ticks the devices up to the accessing instruction
(`arm_ci_run_position`), so every read and write sees the per-edge timeline;
non-timer accesses still end the run. `s5l8900_set_ci_horizon(m, false)`
restores the old runs.

Proof: `test_ci_timeline` runs a program with a periodic timer interrupt
through the VIC (handler acknowledges, logs the counter it reads,
reprograms the period), tick-counter and down-count reads mid-loop, VIC reads,
WFI, and interrupt masking, on the reference interpreter (tick per
instruction), the engine per edge, and the engine with the horizon, comparing
registers, RAM, timer, VIC and timebase state after every 100,000
instructions: identical over 3 M instructions and 770 interrupts, with runs
of 847 instructions instead of 64.5. Five deliberate bugs are each caught (no
catch-up; horizon ignoring wake sources; horizon one instruction long;
catch-up including the accessing instruction; run tick repeating caught-up
time). The fuzzer, guest workloads, strict and sanitizer suites pass.

`cpubench --backend cached --reps 3 --mode user,svc` (exact mode), best of
two medians, previous commit -> this one, same host: **geomean 1.62x over 40
rows, failures=0**; e.g. bignum ARM User 158.5 -> 297.3, sha1 ARM User 178.0
-> 331.6, raster ARM User 195.4 -> 396.3, calls ARM User 127.5 -> 196.3,
interp Thumb User 132.7 -> 237.9, vfp ARM User 52.5 -> 64.0, mmu 1.07-1.58x.
The `svc` rows first measured 0.87x (the horizon was recomputed on every
short run); the device half is now cached per refresh and they are
unchanged (74.3 -> 74.0, 80.9 -> 83.9). These workloads enable no interrupt,
so their runs reach the cap; a real guest's are bounded by its timer
deadline and display refresh and end at each non-timer device access, so the
gain on the phone will be smaller and has to be measured there (the report's
`Runs:` line shows the new run length).

## 10. VFP decoded once

Every VFP instruction used to go through `vfp_execute`'s decode tree on every
execution: about 250 host instructions each in the `vfp` workload (callgrind),
of which about 130 were decoding, and VFP loads took the interpreter's
translating accessor rather than the engine's host TLB. The engine now
decodes the common forms when it builds a block (`vfp_fast_decode_dp`,
`CI_K_VFP_DP/MOV/SYS/LS`): scalar VADD/VSUB/VMUL/VNMUL/VDIV/VMLA/VMLS/VNMLA/
VNMLS, VMOV/VABS/VNEG/VSQRT, VCMP/VCMPE (register and #0), single and double
precision; VMOV between a core and a single register; VMRS/VMSR of FPSCR
(including `VMRS APSR_nzcv`); and VLDR/VSTR, single or double, through the
host TLB. The arithmetic calls the same `f32_do`/`f64_do` rounding steps as
the reference. Every form falls back to `vfp_execute` whenever VFP is not
usable (FPEXC.EN, CPACR per mode), FPSCR selects a trap enable, a directed
rounding mode, a LEN or a STRIDE, flush-to-zero leaves the result ambiguous,
or an access is unaligned, straddles a 1 KiB block or is not plain RAM.

Proof: `test_ci_diff` gained a VFP-heavy phase (60 % of instructions from the
VFP generator, which now also emits the arithmetic, compare, unary and
conversion encodings compiled code uses), VFP registers seeded with NaNs,
infinities, denormals and signed zeros, one-at-a-time LEN/STRIDE/trap-enable
FPSCRs, and random CPACR access: 250,000 runs, 0 mismatches. Eight deliberate
bugs are each caught (NMLA/NMLS sign; gate ignoring trap enables; gate
ignoring LEN/STRIDE; VCMP quiet on a signalling NaN; double-load word order;
literal base PC+4; VMRS APSR_nzcv dropping N; CPACR privileged-only access
ignored in User mode); before the generator changes three of them survived.

Measured, `vfp` ARM User, 4.2 M guest instructions under callgrind: 855.3 M ->
389.3 M host instructions. Wall clock, `cpubench --backend cached --workload
vfp --reps 3`, previous commit and this one run alternately three times on
the same host: 62.8 / 60.9 / 63.7 -> 78.1 / 72.4 / 73.0 M insn/s (+17 %). On
this x86-64 host most of what remains is reading and clearing MXCSR around
each rounding step (`host_exceptions_clear`/`host_exceptions`), which
callgrind counts as single instructions; the phone reads its flags from FPSR
instead, which is why the instruction count, not this host's clock, is the
better guide to the phone. All other rows unchanged within noise.
