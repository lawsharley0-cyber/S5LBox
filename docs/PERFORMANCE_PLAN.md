# Performance Plan

Summary of prior performance work found in the repository, read before
proposing anything new, per the project's own convention of not repeating a
rejected experiment. Source: `docs/hotpath.md` (~500 numbered experiment
runs), `docs/dynarec.md`, `docs/device-benchmark.md`, `docs/ARCHITECTURE.md`.
Primary sources are cited; this file is a summary, not a replacement.

## 1. What has already been attempted

- Release build flags (`-O3`)
- Deferred/lazy device tick scheduling
- TLB index de-aliasing
- Instruction-fetch block cache
- A "direct-write" memory-access contract
- Signed/monolithic build-time AArch64 static translation (whole-function AOT)
- A newer, bounded "compact" AArch64 engine generated at build time, with
  user-mode window continuation, an opt-in privileged-continuation mode, and
  an opt-in repeated-window cache
- Packed NZCV flag representation
- A smaller opcode-class dispatcher
- A decoded-block interpreter (caching decode metadata per block)
- A true runtime JIT translator (`core/src/jit/*`) — built and unit-tested,
  never wired into the boot loop, excluded from the iOS app target
- Deep call-stack profiling (to get past an opaque `[userspace]` sampling bucket)
- Refuted-hypothesis testing: ARM decode-chain cost, MMU walk cost, data-read
  caching, Thumb decoder overhead, CoreGraphics leaf HLE as the fps lever,
  and the claim that a reference emulator "reaches 60fps without a JIT"

## 2. What improved performance

| Change | Measured effect | Source |
|---|---|---|
| `-O3` Release build | 3.0x | `docs/dynarec.md:290-297` |
| Deferred device tick | closed to ~3.4% residual cost after landing (was profiled as a "6.3x opportunity" before landing — see §4) | `core/src/soc/machine.c:1204`, `docs/hotpath.md:360-386` |
| TLB index de-alias + instruction-fetch block cache | ~8% wall-clock | `docs/hotpath.md` (~line 283) |
| "Direct-write" memory-access contract | 4.14% | `docs/device-benchmark.md:73-97` |

## 3. What made performance worse (rejected — do not repeat without a new hypothesis)

| Change | Measured effect | Why rejected | Source |
|---|---|---|---|
| Signed/monolithic static AArch64 engine | **6.17x slower** than interpreter on real A9 | Confirmed on physical hardware under real-firmware replay, not just synthetic | `docs/hotpath.md:7568`, `docs/device-benchmark.md:85-93` |
| Packed NZCV flags | wins microbenchmarks | **Loses on the real-guest gate** — classic synthetic/real divergence | `docs/hotpath.md:3944` |
| Smaller opcode-class dispatcher | **-22.87%** | Regression | `docs/hotpath.md:4137-4184` |
| Repeated-window / privileged continuation cache | -9.54% average on a 160M-instruction interval on real A9, despite **cutting C fast refills ~42%** | Cache-hit-rate wins did not translate to wall-clock wins; worsened median changed-pixel gap too. Stays off by default. | `docs/hotpath.md:8644-8698`, README "Current status" |
| Decoded-block interpreter | rejected (r470-471) | — | `docs/hotpath.md:4323` |

**Before proposing any interpreter-dispatch or block/decode-caching
optimization, read the full context around these four rows in
`docs/hotpath.md` — this project has already tried the obvious version of
that idea and measured it as a net loss on real hardware.**

## 4. What has only been tested synthetically

- The deferred-device-tick "6.3x opportunity" figure was a **profiling
  estimate before landing**; the real, shipped effect was ~3.4% residual
  cost — a concrete example of this project's own synthetic-vs-real gap.
- Packed NZCV's "microbenchmark win" (see §3) is explicitly synthetic-only;
  it lost once measured against the real guest gate.
- CI-hosted-runner numbers (macOS/Linux/Windows GitHub Actions) are **not**
  physical-iPhone timing and are never presented as such in this repo's own
  docs — `fetch-refill-perf.yml`'s own header says hosted-runner throughput
  is reported "without a pass/fail speed threshold because shared runners
  are noisy and are not physical-iPhone FPS measurements."
- The true runtime JIT (`core/src/jit/*`) is exercised only by macOS-arm64 CI
  tests; "blocks executed by a machine boot: zero." (`docs/dynarec.md:216`)

## 5. What has been tested on physical iPhones

- Interpreter throughput (6.5–6.96 Minsn/s) and the signed-static-engine
  6.17x regression — both on a jailbroken **iPhone 6s Plus (A9, iOS 15)**,
  via the `insnbench` lab-only benchmark harness and full-firmware replay
  (`docs/device-benchmark.md`).
- The compact AArch64 engine's three control arms (9/13/15 fps endpoint) —
  "exact physical-A9 Settings replay" (README "Current status").
- Repeated-window cache's -9.54% regression — also physical-A9.
- The app itself running with no JIT/debug/private entitlement — on a
  **stock iPhone 17 (iPhone17,2, iOS 26.1)**, `CS_DEBUGGED: no`.
- Touch/multitouch end-to-end (Z2 digitizer HBPP firmware download, unlock
  gesture) — via the offline `bootkernel` CLI harness, not yet the shipping
  app (see `docs/BOOT_CHAIN.md` caveat in `PERFORMANCE_BASELINE.md`).

## 6. What remains untested

- **30 fps has never been reached by any measured arm**, synthetic or physical.
- Whether the true runtime JIT translator would work at all *on an actual
  iPhone* is explicitly unknown — the app excludes `core/src/jit/**` entirely,
  and whether iOS even permits the executable-memory operations it would need
  is unmeasured on-device (a `VMJitProbe.c` diagnostic exists for this but has
  no recorded on-device result). This is moot for product purposes since
  runtime JIT is out of scope by requirement, but worth knowing if someone
  proposes reviving it.
- On-device audible audio output — `docs/audio.md` states the guest has
  **never once written an I2S register in any recorded run**, so the blocker
  documented there is "the guest doesn't get far enough to try," not
  "the audio hardware model is wrong." **However**, unmerged branch
  `fix/audio-buffer-integrity` / `product/ios3-experience` contains I2S0 DMA
  streaming, host `AudioQueue` output, and a lock-free ring buffer — work that
  postdates and is not reflected in `docs/audio.md`. Its actual on-device
  audible-output status is unverified by this session (its CI — `core-tests`,
  `ios-build` — has run green multiple times on that branch, which is not the
  same as confirming sound was heard). **This needs a direct check before
  treating audio as either "not started" (per `docs/audio.md`) or "done"
  (per branch commit messages) — both framings are currently unverified.**
- Installed third-party application compatibility: `app/Sources/VMGuestInstall*.c`
  and `VMGuestPackageManifest.c` / `VMGuestPackageFile.c` suggest an existing
  app-install-into-guest mechanism, and `product/ios3-experience` adds
  "validate and stage user-owned legacy IPA imports" — but no crash-diagnostic
  pipeline, compatibility matrix, or test-application results were found in
  `docs/`. This track appears essentially unstarted against the task's own
  Milestone 5/6 bar (diagnostics, register dumps, compatibility matrix).
- CommCenter's outstanding port check-in and device-level acceptance testing
  on the iPhone 6s Plus are both listed as **open** items in
  `docs/QUALITY.md`'s own promotion-gate checklist.

## 7. Unmerged work not on `main` (found this session, not previously summarized anywhere)

| Branch | Ahead of `main` | Contents |
|---|---|---|
| `fix/audio-buffer-integrity` | 7 commits | I2S0 DMA audio streaming + host `AudioQueue` output, boot chime + test-sound menu, DMA/host-buffer decoupling fix (boot-freeze fix), lock-free SPSC ring buffer replacing mutex-per-sample, audio-queue-ownership fix |
| `product/ios3-experience` | 28 commits (superset of the above) | + legacy IPA import/staging, immersive controls + unchanged-frame-upload avoidance, MBX2D/graphics work (TA stream rendering, framebuffer-flip tracking, scanout correlation, ARGB1555 decode), powered-off checkpoint cold-boot recovery, PMU/GPIO interrupt wiring, guest disk growth to 2 GiB, Cydia privilege migration/repair |
| `publish/ios3-experience` | — | not diffed this session |

Both audio-carrying branches have green `core-tests` and `ios-build` CI runs
at their tips (verified via GitHub Actions UI this session, run IDs recorded
in commit history). This is materially different from what `docs/audio.md`
and the task's framing assume ("audio currently does not work," greenfield
investigation) — **substantial unmerged work already exists** and should be
reviewed before starting new audio or app-install work from scratch.

## 8. Fresh profiling this session — confirms, does not extend, prior findings

Ran `bootkernel --sequence-profile --restore` from a saved checkpoint
(instruction 3.9B, just before the r181-style unlock drag) on this
session's own verified `iPhone1,2` 3.1.3 firmware, on this Windows x86-64
desktop host — desktop-only, not physical-iPhone, tagged per this repo's
own convention.

**Exact confirmation of the RSA claim, at PC granularity, not just an
aggregate percentage:** a single hot instruction head at `va=0x3145ad4c`
(userspace) alone accounts for **20.9% of all 500M dynamic instructions**
in this window; the top 10 heads cover **61.9%**; the top 100 cover
**92.4%** (`work/profile01.log`). The instruction mix at the hottest sites
(LDR, MOV+shift, ADD, CMP, extend, backward branch closing a ~24-instruction
loop) is ordinary ARM code — nothing exotic — consistent with a bignum
multiply/accumulate inner loop, matching `hotpath.md`'s and `ROADMAP.md`'s
existing identification of `Security.framework`'s RSA bignum routine
(`_mulg_common`) as the dominant cost, not interpreter dispatch overhead.

**This is confirmation, not a new lever.** A direct-mapped site-cache
simulation over this same trace shows very high hit rates (1024 entries:
98.5%, 65536 entries: 99.95%) — but `hotpath.md` §3 already shows that high
cache-hit-rate simulations do not reliably predict real wall-clock wins on
physical hardware (the repeated-window cache: -9.54% despite cutting C fast
refills ~42%). Proposing a decode/site cache from this data alone would be
repeating that already-rejected experiment without a new hypothesis for why
it would behave differently this time — it would not.

**Real scope boundary, not a workaround:** this profiling tool itself
reports `exact compact-raw instruction-semantic admission model:
unavailable: rebuild with S5LBOX_STATIC_A64_ENGINE=ON` — and that engine
self-gates to compile only on arm64 hosts (`core/CMakeLists.txt`). This
Windows x86-64 desktop cannot build or exercise the build-time AOT engine
at all, meaning **no question about AOT coverage or AOT-vs-interpreter
behavior can be investigated from this machine.** That work needs an
arm64 host — a real iPhone or an Apple Silicon Mac. Until then, the
honest state of the CPU-performance track is: bottleneck reconfirmed
precisely, no new implementable optimization identified from this
machine, next step is architecture-bound rather than effort-bound.

## 9. Rejected: the experimental graphics-HLE build, on real hardware

The `S5LBOX_IOS3_HLE_EXPERIMENT` build (`ios-build` workflow_dispatch,
`experimental_hle: true`) arms the three existing `IOS3_HLE_REPLACE` sites --
`sw_sample_nearest_BGRA8`, `sw_scanline`, `ogl_poly_scan` -- which is exactly
the change `README.md`/`ROADMAP.md` call "the only single change that could
reach 30 fps." That claim had never been tested end-to-end on a real phone;
this session built and ran it on the user's device (a real iPhone, the same
one used throughout this session) for the first time.

**Result: subjectively and clearly slower than the armed-off baseline, not
faster.** This is precisely the test `tools/ios3_hle.c`'s own header already
flagged as outstanding: *"a real armed/disarmed framebuffer oracle remains
mandatory before their pixels can be accepted... 39.8% of a profile is not
the same number as calls-times-cost."* That question now has a real,
on-device answer, and it is no.

**Why, most likely** (not independently measured this session, but consistent
with the code): these three functions are per-pixel/per-texel leaf routines
called an enormous number of times per frame. Each `IOS3_HLE_REPLACE` call
pays real per-call overhead -- every guest memory access goes through the
guest MMU via a callback indirection (`ios3_hle_mem_t.read`/`write`), not a
raw pointer, per contract item 4 in `tools/ios3_hle.h`. For a leaf function
that is itself only ~10-30 ARM instructions, that safety overhead can plausibly
exceed what skipping those instructions saves. Separately,
`vm_firmware_hle_enable()` installing pre-step-hook targets forces
`s5l8900_static_a64_invalidate_derived()` and constrains the build-time
compact AArch64 engine from translating straight through any block containing
a target PC -- fragmenting exactly the graphics-heavy hot region into smaller,
less-efficient execution windows.

**Do not re-arm these three REPLACE sites, or add new REPLACE-mode HLE sites
for high-call-count leaf functions, without a new hypothesis for why per-call
interception overhead would not dominate again.** This does not indict the
HLE mechanism in general -- `_mulg_common` (§8) is called far less often per
guest-visible unit of work (millions of instructions per call, not one call
per pixel), so the same overhead concern does not obviously apply there, but
that too now needs the same armed/disarmed on-device measurement before a
REPLACE is trusted, not just an OBSERVE count.

## 10. New: an independent CPU-correctness oracle (not a performance result)

`tools/arm_diff_probe.c` + `tools/unicorn_diff.py` differentially test
`arm_step()` against Unicorn (an independently-implemented ARM emulator with
no shared code or assumptions with this project). First run found the bug in
the new harness rather than the interpreter — see the tool's own commit
message — and after fixing it, 5,800 cases across 29 common
data-processing/multiply/load-store encodings came back with zero
mismatches. This doesn't move any performance number; it's recorded here
because any future CPU change (interpreter, compact engine, or the RSA/HLE
work in §8) should be checked against this before and after, the same way
`docs/audio.md`'s 2026-09-18 entry now needs a way to rule a CPU bug in or
out for the AMC freeze that this session could not resolve.
