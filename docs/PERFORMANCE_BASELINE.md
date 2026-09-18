# Performance Baseline

Recorded 2026-09-18, before any performance work in this effort. Read this
alongside [`docs/hotpath.md`](hotpath.md) (the full experiment log) and
[`docs/device-benchmark.md`](device-benchmark.md) (physical-device methodology) —
this file is a snapshot, those are the source of truth.

## Repository state

- Upstream: https://github.com/j0shua-SYSON/S5LBox
- Working copy: `origin` = https://github.com/lawsharley0-cyber/S5LBox.git (a fork/clone of upstream)
- Branch: `main`
- Commit: `6f203ba550b49afadee008c7eb55373a838eed33` — "Render measured TA streams atomically" (2026-08-13)
- Working tree: clean, 985 commits on `main`

Three branches carry unmerged work not reflected below or in `main`:
`fix/audio-buffer-integrity` (7 commits), `product/ios3-experience` (28 commits,
superset of the above plus graphics/app-install/checkpoint work),
`publish/ios3-experience`. See `docs/PERFORMANCE_PLAN.md` §7.

## Host environment (this session)

- Host OS: Windows 11 Home 10.0.26200, x86-64 — **this is a development
  workstation, not the target platform.**
- Compiler / CMake / Xcode: **none installed locally.** `cmake`, `cc`, `gcc`,
  `clang`, `cl` all absent from PATH; no Visual Studio installation found via
  `vswhere`. No local build has been performed by this session.
- Physical iPhone: **none attached to this session.**

Because of the above, every result below is either (a) a GitHub Actions
hosted-runner result, independently verified against the exact commit above,
or (b) a historical physical-device number already recorded in this repo's
own docs from a prior session. **Nothing in this file was freshly measured on
physical iPhone hardware by this session.**

## Build / test status — verified via CI at commit `6f203ba`

Checked directly against https://github.com/lawsharley0-cyber/S5LBox/actions
(not assumed from README badges):

| Workflow | Run | Result | Duration | Coverage |
|---|---|---|---|---|
| `core-tests` | [#1](https://github.com/lawsharley0-cyber/S5LBox/actions/runs/35282438567) | **Success** | 8m25s | build+test on windows-latest, ubuntu-latest, macos-latest; asan+ubsan; warnings-as-errors; jit matrix on ubuntu-latest, macos-15, macos-14 |
| `ios-build` | [#1](https://github.com/lawsharley0-cyber/S5LBox/actions/runs/35282409253) | **Success** | 1m31s | ad-hoc-signed `S5LBox.ipa` artifact, 1.14 MB |

This satisfies Milestone 0 ("build succeeds, tests run") via CI evidence at
HEAD. It does **not** substitute for a local build, which is still worth
having for fast iteration (see open question in the accompanying report).

Historical local test counts recorded in [`docs/QUALITY.md`](QUALITY.md)
(not reproduced this session — may have drifted): full Release suite 23/23
test executables; `test_soc` 5,504 assertions/0 failures; `test_snapshot`
469/0; `test_vfp` 488/0; `test_arm` 810/0; `test_jit` 347/0.

## Build configuration

Default per README: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`. Both
AArch64 acceleration mechanisms are **off by default** and gated by CMake
options that are never default-ON:

- `S5LBOX_JIT` (true runtime translator, `core/src/jit/*`) — OFF
- `S5LBOX_STATIC_A64_ENGINE` (build-time static AArch64 codegen inputs,
  `tools/a64_static.c` / `gen_a64_static.py`) — OFF, and additionally
  self-gates to only compile when the *host* CMake processor is arm64/aarch64
  (so it never compiles on this x86-64 Windows box regardless of the flag)

## Baseline emulator performance (historical, not reproduced this session)

All physical-device numbers below are from prior work already recorded in
`docs/device-benchmark.md` and `docs/hotpath.md`, on a jailbroken **iPhone 6s
Plus (A9, iOS 15)** unless noted. A later, separate confirmation ran the
*app* (not a benchmark) on a **stock iPhone 17 (iPhone17,2, iOS 26.1)**,
reporting `CS_DEBUGGED: no` — i.e. no debugger/JIT entitlement was granted or
needed.

- Interpreter throughput: **6.5–6.96 Minsn/s** (real A9 hardware, signed-static engine disabled)
- Rejected signed-static AOT engine: **0.963–1.125 Minsn/s** — **6.17x slower** than the interpreter on the same hardware. Product policy disables it; only the interpreter (plus the newer bounded compact AArch64 engine, see below) ships.
- Visible cadence: user-reported foreground navigation can fall to **0–4 fps**. Latest exact physical-A9 replay of the compact engine's three control arms: **9, 13, 15 fps** endpoint, 1.8–3.3 changed-scanouts/s, 2.2–3.2s max changed-pixel gap. **No arm has reached 30 fps acceptance.**
- Required throughput for the 30 fps target (per `hotpath.md`'s corrected framing, ~16.7M guest instructions per composited frame): **~500M insn/s needed vs. ~25M insn/s measured — a ~20x gap.**
- **The dominant measured costs are RSA/crypto (~40.6% of an interactive-swipe sample, under `Security.framework`, `_mulg_common`) and CoreAnimation's software rasterizer (`CA::OGL::sw_scanline`, because the MBX GPU is unmodeled) — not raw interpreter dispatch overhead**, which was specifically profiled and refuted as the primary cost (see `docs/hotpath.md` and `PERFORMANCE_PLAN.md` §2).

## Firmware / guest configuration

No firmware is present in this workspace (`firmware/` is git-ignored and
absent, as required). No guest boot, SpringBoard run, or audio/app test was
performed this session. The project requires the user's own iPhone OS 3.1.3
(7E18) kernel/devicetree/rootfs; exact accepted SHA-256 hashes are pinned in
`README.md`.

## Desktop vs. physical-iPhone — explicit tagging

| Claim | Source |
|---|---|
| `core-tests` / `ios-build` green at `6f203ba` | **CI hosted runners** (Linux/macOS/Windows), verified this session |
| Interpreter 6.5–6.96 Minsn/s | **Physical iPhone 6s Plus (A9)**, prior session, recorded in `docs/device-benchmark.md` |
| Signed-static AOT 6.17x regression | **Physical iPhone 6s Plus (A9)**, prior session |
| 9/13/15 fps compact-engine arms | **Physical iPhone 6s Plus (A9)**, prior session, "exact ... replay" |
| App runs without JIT/debug entitlement | **Physical iPhone 17, iOS 26.1**, prior session |
| Everything else in this file | Static analysis of docs/source in this session — no execution |
