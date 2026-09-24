# Known issues (CPU engine work, 2026-09-23)

Scope: what is known to be wrong, missing or unverified after the cached
interpreter work on branch `claude/adoring-brahmagupta-bz1n78`. Emulator-wide
limitations that predate this work are listed last and are **not** caused by
it. Nothing here is a guess: every item names how it was found or how to
reproduce it.

## 1. Not validated yet (needs the owner's firmware or hardware)

| Item | Status | How to validate |
|---|---|---|
| A full iPhone OS 3.1.3 boot on the cached interpreter | **Not run.** No Apple firmware exists in this environment and none may be downloaded (repo policy). | `MAC_VALIDATION.md` §3 (the same on Windows): the same `bootkernel --run-api` boot with `--cpu-backend interp` and with `--cpu-backend cached --ci-verify`, each saving `--snapshot-at N`; the two machine states and RAM images must be identical. |
| Speed on a phone | **Not measured.** Every number in `BENCHMARK_RESULTS.md` is desktop x86-64 (plus the CI runners). | `MAC_VALIDATION.md` §4. |
| MSVC on a local Windows 11 machine | Built and tested only by the `windows-latest` CI runner (MSVC, Visual Studio generator). No local VS2022, Ninja+cl or clang-cl run has been made. | `WINDOWS_VALIDATION.md` §2. |
| The iOS app with the cached interpreter selected | The app compiles in CI (`ios-build`). Nobody has selected the backend on a device. | Settings → Diagnostics → CPU Execution Backend → Cached Interpreter, then compare with the reference as in `MAC_VALIDATION.md` §4. |

Because of the first row the **reference interpreter stays the default**
everywhere (app, `bootkernel`, `snapboot`); the engine is opt-in.

## 2. Cached interpreter: limitations by design

These are correct (the reference code runs them) but not fast:

- **Reference in-block (`CI_K_REF`)**: every coprocessor data transfer
  other than VFP, `LDM`/`STM` with the S bit (user-bank and exception return),
  `LDM`/`STM` crossing a 1 KiB boundary or missing the host TLB, `SWP`,
  exclusives, DSP multiplies, SIMD/saturating media, `MSR`, `MRS SPSR`, `CPS`,
  `SETEND`, PC-relative forms the reference treats specially (other than
  `LDR pc`, `MOV pc, Rm`, Thumb `ADD/MOV pc, Rm` and the PIC idioms, which
  run in the engine), and every access to device memory. VFP runs the reference's VFP unit directly
  (`CI_K_VFP`, no decode tree) but the unit itself and its memory path are
  unspecialised, so the `vfp` workload gains ~1.6× (`BENCHMARK_RESULTS.md`
  §7).
- **Single-stepped through `arm_step` (block stop)**: `SVC`, `BKPT`, CP14/CP15
  (including `WFI` and all cache/TLB maintenance), undefined encodings, and
  any instruction that faults on fetch.
- **Engine exits at every timebase edge** (≈68 instructions at 412:6 MHz) and
  on any device access, because device time stays exact. The device refresh
  at each edge is now ~18–24 % of host time in engine mode (callgrind,
  `BENCHMARK_RESULTS.md` §4). Making it cheaper is device-model work, not
  engine work, and has not been started.
- **Page-walk-bound code** (`mmu` workload) gains ~1.4×: the engine's host
  TLB is 1 KiB-granular with 512 entries per privilege, so a miss costs a
  full `arm_mmu_translate` through `bus_read`.
- **No block linking** beyond the in-engine dispatch loop and the VA-keyed
  block map. Planned in `IMPLEMENTATION_PLAN.md` §5; not built because the
  map already removed most lookup cost and linking needs its own
  invalidation proof.
- `snapboot` accepts `--cpu-backend`/`--cached-cpu` but its loop calls
  `arm_step` directly, so the option only allocates the engine and never runs
  it. Engine runs use `bootkernel --run-api` (or the app).
- `arm_ci_stats_t.mem_fast` is never incremented (the fast path is kept free
  of counters); it always reads 0.

## 3. Guest app (IPA) install: icon appears, launch fails (being fixed)

First real test (device report on `6cf0ed2`, 2026-09-24): a user-supplied
IPA that passed the importer's checks (ARMv6 slice, unencrypted, minimum OS
<= 3.1.3) **appears on the guest home screen** (so SpringBoard does scan
`/Applications`; the MobileInstallation cache concern did not bite), but:

- **Opening it shows black, then returns to the home screen.** Most likely
  cause, from `activation.md` A.3.1: the kernel's page-fault signature kill
  (`_cs_enforcement_disable`, which the boot-arg does not reach) killed it at
  its first code page. Fixed for machines with an installed app by an
  optional kernel patch in the same verified transaction as the others.
  Unconfirmed until the next device run. Other causes that would look the
  same: an OpenGL ES game (MBX graphics are hidden from the guest by default,
  so context creation fails), a missing framework or symbol, or the launch
  watchdog on a slow launch.
- **The icon is square.** Probably because apps in `/Applications` are
  treated as system apps, whose icons SpringBoard shows as shipped (stock
  icons are pre-rendered with rounded corners); a user app installed through
  MobileInstallation would be masked. Cosmetic; not changed.

Diagnostics added for exactly this: Performance & Sound Details now ends with
the last 40 lines of the guest console (where the kernel prints a
code-signing kill), and Machines -> swipe a machine -> **Crash Logs** lists
and shows the guest's own crash reports (Apple's ReportCrash writes them to
`/private/var/mobile/Library/Logs/CrashReporter`), read from the stopped
disk with the new read-only HFS API (`rootfs_work_list_directory`,
`rootfs_work_read_file`).

## 4. Pre-existing reference-interpreter gap found during this work

**A host that clears `SCTLR.M` directly, without calling
`arm_mmu_tlb_flush()`, keeps getting the old translation from the reference
interpreter's data-read/data-write block caches.** `dread_hit()` checks the
tag and `tlb_gen` but not the MMU enable, and `arm_mmu_translate()`'s MMU-off
early return is not stamped. The guest path is correct (an `MCR` to SCTLR
flushes). Reproduce: build `core/tests/test_ci_translation.c` with
`S5L8900_CPU_BACKEND_INTERPRETER` — case 3 ("after host clears SCTLR.M")
fails, and the others pass. The cached interpreter handles it (it purges on a
change of the M bit), so the two backends can differ **only** in this
host-side misuse. Nothing in the repository clears M directly on a running
machine. Contract until fixed: after changing `SCTLR.M` from the host, call
`arm_mmu_tlb_flush(&m->cpu)`.

## 5. Bugs found and fixed by this work (for the record)

| Where | Bug | Found by | Fixed in |
|---|---|---|---|
| `75ad89f`/`c05df84`/`4a8480f` tiers | 8 of 11 kernel-critical cases wrong; inexact timing; no invalidation; fabricated profiler; red strict CI | differential probe, audit | `7143034` (removed) |
| Engine (pre-commit) | `MSR CPSR_c` setting T without a branch left the block running ARM ops in Thumb state | fuzzer, seed 3 case 36010 | `962aa8b` |
| Engine | host TLBs and (later) block map outlived `arm_reset`, snapshot restore and a host SCTLR.M clear | code review while adding the map; `test_ci_translation` | `e06c8e7` |
| Engine | `base` local in LDM/STM handlers shadowed the block base used by `PC_OF` (latent) | `-Wpointer-to-int-cast` when dispatch was inlined | `a50b124` |
| Reference | `SMMLA`/`SMMLS`/`SMMLAR` shifted a negative Ra left and could overflow a signed 64-bit sum; `SMLALD`/`SMLSLD` likewise (undefined behaviour in C; results were right on the compilers tried) | `asan + ubsan` CI job running the differential fuzzer (run 35935011470) | this commit, with `test_arm` cases that trip UBSan on the old code |

## 6. Repository hygiene

- The baseline tag `baseline-pre-cached-interpreter` exists only in the
  session clone: the git proxy refused the tag push. The baseline is commit
  `4a8480f` (`origin/main` at the start of the work). Create the tag locally
  with `git tag baseline-pre-cached-interpreter 4a8480f` if wanted.
- `docs/ARCHITECTURE_CURRENT.md` and `docs/ICUBE_TRANSLATION_RESEARCH.md` are
  superseded by `CURRENT_ARCHITECTURE.md` and `ICUBE_DOLPHIN_RESEARCH.md`
  (each carries a note); they are kept for history.

## 7. Existing emulator limitations (not caused by the CPU work)

Carried from `CURRENT_ARCHITECTURE.md` §8, sources README, `QUALITY.md`,
`ROADMAP.md`, `audio.md`:

- On-device foreground cadence around 0–4 fps; no measured configuration
  reaches 30 fps. Dominant costs in the steady state include RSA/crypto in
  `Security.framework` and QuartzCore's software rasteriser, not decode alone
  (`hotpath.md`). A 2× faster CPU core does not translate to 2× frames.
- Audio modelled, never heard; an AMC/BSU lock freeze reproduces on device.
- PPP link comes up; no guest IP packet carried; the app has no PPP endpoint.
- MBX graphics experimental and hidden from the guest by default.
- RTC placeholder; baseband, Wi-Fi, Bluetooth, camera, accelerometer not
  modelled.
- Two-finger gestures reach userspace but have not visibly moved anything.
- iOS 6 is a different machine (ARMv7, Thumb-2, NEON, a different SoC and a
  GPU-composited UI): see `IOS6_READINESS.md`.
