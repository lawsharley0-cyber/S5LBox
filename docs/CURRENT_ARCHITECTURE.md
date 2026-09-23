# Current architecture (inspection record, 2026-09-23)

This is the inspection record written **before** any cached-interpreter work,
at tag `baseline-pre-cached-interpreter` (`4a8480f`). It names the source files
responsible for every stage and subsystem, records how the three most recent
commits changed the CPU path, and lists known defects separately so that they
are never attributed to later work.

It does not replace the detailed documents it links to. Where this file and a
primary source disagree, the primary source wins:
[`ARCHITECTURE.md`](ARCHITECTURE.md) (design),
[`BOOT_CHAIN.md`](BOOT_CHAIN.md) (firmware),
[`hotpath.md`](hotpath.md) (every performance experiment, ~500 numbered runs),
[`QUALITY.md`](QUALITY.md) (evidence), [`ROADMAP.md`](ROADMAP.md) (status).

`docs/ARCHITECTURE_CURRENT.md` (added by `75ad89f`) is superseded by this file;
several of its statements are contradicted by measurements in `hotpath.md`
(see §7).

---

## 1. What the project is

S5LBox emulates the Samsung **S5L8900** SoC (iPhone 3G, `iPhone1,2`) and its
**ARM1176JZF-S** core (ARMv6, Thumb-1, VFPv2) well enough to boot the user's own
unmodified **iPhone OS 3.1.3 (7E18)** kernel, `launchd` and SpringBoard. The
emulator core is portable C11; the product is an iOS app (no JIT, no private
entitlements); the desktop harness `bootkernel` is the diagnostic instrument.

## 2. Repository map

| Path | Contents | Language | Built by |
|---|---|---|---|
| `core/src/arm/` | CPU: `arm_interp.c` (ARM+Thumb interpreter, CP15, exceptions), `vfp.c` (VFPv2), `a64_static_engine.c` (opt-in build-time-signed AArch64 engine), and the `75ad89f` tiers (`arm_block_*`, `arm_ir_*`, `arm_profile.c`) | C11 | CMake + Xcode |
| `core/src/mmu.c` | ARMv6 short-descriptor MMU, software TLB, fetch-block cache refill | C11 | both |
| `core/src/soc/` | Machine (`machine.c`: memory map, bus, run loop, tick) and every device model | C11 | both |
| `core/src/firmware/` | IMG3, AES, LZSS, Mach-O, device tree, kernel symbols, loader | C11 | both |
| `core/src/boot/bringup.c` | Kernel hand-off: segments, device-tree patches, `boot_args`, CPU entry | C11 | both |
| `core/src/md_*bridge.c`, `vm_block.c`, `vm_source.c` | Guest disk: privileged-SVC memory-disk bridges over a host block device | C11 | both |
| `core/src/net/` | Host PPP peer, IPv4 NAT, TCP (no sockets inside) | C11 | both |
| `core/src/snapshot.c` | Deterministic machine checkpoints | C11 | both |
| `core/src/jit/` | Runtime ARMv6→arm64 translator (tested, **never called by the run loop**, excluded from the app) | C11 | CMake `-DS5LBOX_JIT=ON` only |
| `core/include/` | Public headers (`arm.h`, `soc.h` 4.6 k lines, …) | C11 | both |
| `core/tests/` | 77 ctest executables/scripts | C11/CMake | CMake |
| `tools/` | `bootkernel.c` (39.8 k lines), `insnbench.c`, `jitbench.c`, `snapboot.c`, importers, HLE, research Python | C11/Python | CMake (C) |
| `app/Sources/*.c` | App logic kept in C so host CI can test it (firmware import, boot, touch map, queues, audio ring, instances…) | C11 | CMake tests + Xcode |
| `app/Sources/*.m` | UIKit/Foundation/AudioToolbox shell | Objective-C | Xcode only |
| `app/project.yml` | XcodeGen spec (no `.xcodeproj` is committed) | YAML | `xcodegen` |
| `.github/workflows/` | `core-tests` (Linux/macOS/Windows + asan/ubsan + strict warnings + JIT matrix), `ios-build`, device benchmark/replay, fetch-refill perf | YAML | GitHub Actions |

## 3. Build system, hosts, dependencies

- **Core, tools, tests: CMake ≥ 3.16**, C11, default `Release` (`-O3` on
  GCC/Clang). `enable_testing()`; one `ctest` runs everything. Options:
  `S5LBOX_JIT` (OFF), `S5LBOX_STATIC_A64_ENGINE` (OFF; generator needs Python 3
  and only produces native code on an arm64 host).
- **App: XcodeGen → Xcode**, iOS 13+, arm64, `-O3` + LTO. The app compiles
  `core/src/**` directly (minus `jit/**`), so **any new file under `core/src`
  is compiled into the app automatically**. The app defines
  `S5LBOX_STATIC_A64_ENGINE=1` and `S5LBOX_STATIC_A64_DEFAULT_COMPACT_RAW=1`.
- **Hosts verified by CI**: `ubuntu-latest` (GCC), `macos-latest` (Apple
  Clang, arm64), `windows-latest` (**MSVC**, x64) all build and pass the full
  suite at `4a8480f`; `macos-14/15` additionally execute emitted arm64 code.
- **Third-party dependencies of the core: none.** `libm` on Linux only.
  Python 3 is needed only for the optional static-engine generator and the
  research scripts in `tools/*.py`. The app links Foundation, UIKit,
  CoreGraphics, QuartzCore, AudioToolbox, AVFoundation.

## 4. Boot pipeline, stage by stage

| Stage | Desktop harness | iOS app | What happens |
|---|---|---|---|
| **Startup** | `tools/bootkernel.c` `main()` parses ~80 options | `app/Sources/main.m` → `AppDelegate.m` → `VMInstanceListViewController.m` → `EmulatorViewController.m` → `VMEngine.m -start` (owns the one emulator thread) | |
| **Firmware / IPSW** | User supplies decrypted `kernel.macho`, `devicetree.bin`, `rootfs.img`; `tools/fwimport.c`, `img3dump.c`, `unlzss.c`, `vfdecrypt.py` | `VMFirmwareImporter.m` (shell) → `VMFirmwareImport.c`, `VMFirmwareZip.c`, `VMFirmwareInflate.c`, `VMFirmwareDMG.c`, `VMFirmwarePlist.c`, `VMFirmwareDigest.c` | IPSW is unzipped, IMG3 unwrapped (`core/src/firmware/img3.c`), decrypted (`aes.c`), decompressed (`lzss.c`); artefacts are pinned by SHA-256 |
| **Work image** | `tools/rootfs_work.c` (+ `hfsx_extract.py` for research) | `VMFirmwareBoot.c` `vm_firmware_boot_provision` | Writable copy of the root FS; fstab repointed; activation state provisioned; volume grown |
| **Machine init** | `s5l8900_init()` | same | `core/src/soc/machine.c` allocates DRAM (128 MB at `0x08000000`), EDRAM, NOR; resets every device; wires the bus callbacks and IRQ graph |
| **CPU reset** | `arm_reset()` | same | `core/src/arm/arm_interp.c`: SVC mode, I/F masked, CP15 reset values, TLB/fetch/dread caches cleared |
| **Boot hand-off** | `s5l_bringup()` via `tools/ios3_bringup_gate.c` | `VMFirmwareBoot.c` → `s5l_bringup()` | `core/src/boot/bringup.c`: map Mach-O segments (`macho.c`), patch the device tree in memory (`devicetree.c`), reserve boot memory, build `boot_args`, install the privileged-SVC disk bridges, set `r0`/PC. Kernel byte patches are host policy in `tools/ios3_kernel_patch.c`, gated by hash |
| **Kernel** | `s5l8900_run()` / literal `arm_step()` diagnostic loop | `VMEngine.m -threadMain` calls `s5l8900_run()` in 100 000-instruction chunks | XNU 1357.5.30 runs on the interpreter; disk I/O through `md_bridge.c` / `md_raw_bridge.c` → `vm_block.c` → host file (`tools/file_block.c`) |
| **Userspace** | same | same | `launchd`, daemons, `dyld_shared_cache_armv6` — ordinary guest code |
| **SpringBoard** | framebuffer dumped by bootkernel (`-F`, PPM) | `VMFramePublication.c` → `VMFramebufferView.m` | CLCD scanout (`soc/clcd.c`) of guest-composited pixels (Apple's CPU renderer by default; experimental MBX model in `soc/mbx.c`); touch via `soc/mtz2.c` + `soc/gpioic.c` ← `VMTouchQueue.c`/`VMTouchMap.c`; buttons via `soc/buttons.c` ← `VMButtonQueue.c` |

The step-by-step diagnosis history of each stage is in `BOOTLOG.md`
(5.9 k lines) and `debugging.md`.

## 5. Subsystems

### 5.1 CPU — `core/src/arm/arm_interp.c`, `vfp.c`, `core/include/arm.h`

- **Guest CPU:** ARM1176JZF-S, ARMv6 (with v6K exclusives/`CLREX`), ARM and
  Thumb-1 states, VFPv2 (`FPSID 0x410120b4`), CP15 with ARMv6 extended page
  tables (`SCTLR.XP`), no Thumb-2, no NEON. `arm_arch_t` already names a
  future `ARM_ARCH_V7_SWIFT` profile gating `SDIV/UDIV` and ARM-state
  `MOVW/MOVT`.
- **Entry point:** `arm_step()` executes exactly one instruction: pending-abort
  and mode check → FIQ/IRQ sampling → fetch (1 KB fetch-block host-pointer
  cache, else `arm_mmu_translate(FETCH)`) → Thumb dispatch (`thumb_step`,
  switch on `insn>>12`) or ARM: `cond==0xF` space, `AL` fast path, then an
  ordered comparison chain (data processing hoisted first) into `exec_*`
  helpers.
- **Exceptions:** `take_exception()` (banking, SPSR, A/I/F masks, `CPSR.E ←
  SCTLR.EE`, clears exclusive monitor, high vectors); data aborts are latched
  by the memory helpers (`note_abort`) and taken after the instruction
  (base-restored model); prefetch aborts at fetch; lazy-VFP undefined traps are
  vectored to the guest; any other unimplemented encoding returns
  `ARM_UNDEFINED` and stops the machine loudly.
- **Memory helpers:** `mem_rN_as`/`mem_wN_as` implement alignment faults,
  `SCTLR.U` unaligned support, legacy rotation, page-crossing splits,
  translation-mode (`LDRT`) privilege; 64-entry 1 KB `dread`/`dwrite`
  host-pointer caches (now in `core/include/arm_mem.h`) validated by
  `tlb_gen`.
- **Cycle model:** `cycles++` per retired instruction (1 instruction = 1 tick).

### 5.2 MMU — `core/src/mmu.c`

ARMv6 short descriptors (sections, supersections, coarse tables, 4 KB/64 KB
pages, legacy 1 KB subpage AP, APX/XN), `TTBCR.N` split, domains, FCSE.
Software TLB: 4096 direct-mapped entries at 1 KB granularity, tagged with
`(va>>10, access, priv)`, invalidated wholesale by bumping `tlb_gen` on any
TLB/cache-relevant CP15 write. Faults return an FSR which callers convert to
aborts. Counters: `tlb_hits/misses/flushes`, `dread_*`, `dwrite_*`.

### 5.3 Memory map and bus — `core/src/soc/machine.c`, `core/include/soc.h`

`bus_read`/`bus_write` test `in_ram()` first (plain `memcpy`), otherwise set
`m->level_dirty = true` and dispatch by window to a device. **Every guest
device access therefore marks the device graph dirty**, which is what makes
the run loop's batching exact. `machine_host_ram()` hands out host pointers
for plain DRAM only; `host_ram_write` (direct stores that bypass bus
observers) is a separate consent (`s5l8900_set_direct_ram_writes`).

### 5.4 Timing and the run loop — `machine.c`

- `s5l8900_tick(m, n)` converts retired CPU ticks to timebase ticks at the
  guest's `cpu_hz:tb_hz` ratio and refreshes the device graph (timers, VIC,
  GPIO cascade, SPI/DMA, PMU…) → `cpu.irq_line/fiq_line`.
- `s5l8900_run(m, max, &st)` is the product entry point. Per iteration: exact-PC
  pre-step hook (HLE) → optional static A64 engine → (since `75ad89f`) optional
  tier → **User-mode tick batching** up to the next exact timebase edge
  (`retirement_batch_limit`), breaking on MMIO (`level_dirty`), host input or
  leaving User mode → else one `arm_step()` + `tick(1)`.
- Optional *active host clock* mode (app): guest time follows wall time,
  batches ≤ 256 instructions, clock sampled every 4096.
- WFI calls `bus->wait_for_interrupt` → the machine fast-forwards (and on iOS
  paces) idle time.

### 5.5 Devices — `core/src/soc/`

UART0/UART4 (`uart.c`), two PL192 VICs (`vic.c`), timers (`timer.c`), power
controller (`power.c`), CLCD (`clcd.c`), TV-out (`tvout.c`), I²C + PCF50635
PMU (`i2c.c`, `pcf50635.c`), WM8991 codec + I²S (`wm8991.c`, `i2s.c`), SPI
(`spi.c`), GPIO interrupt controller (`gpioic.c`), buttons (`buttons.c`), Z2
multitouch with HBPP bootloader (`mtz2.c`), USB OTG config regs (`usbotg.c`),
PL080 DMA (`pl080.c`), NOR (`nor.c`), NAND (`nand.c`), host storage glue
(`storage.c`), PowerVR MBX 2D/3D model (`mbx.c`, 5.2 k lines, experimental),
generic stubs.

### 5.6 Graphics

Default: Apple's software compositor draws into guest RAM; the CLCD model
scans out; `VMFramePublication.c` detects changed frames and
`VMFramebufferView.m` (UIKit/CoreGraphics — **no Metal anywhere in the
repository**) presents them. `mbx.c` models enough of the MBX command stream
to complete measured SpringBoard workloads but is opt-in.

### 5.7 Audio

Guest side: WM8991 over I²C, I²S0/1, PL080 DMA into the I²S FIFO
(`i2s.c` exposes a host sink callback). Host side: `VMAudioBuffer.c`
(lock-free SPSC ring, tested on the host) → `VMAudioOutput.m` (AudioQueue).
Known problems are recorded in `AUDIO_DIAGNOSTICS.md` and `docs/audio.md`;
audio is **out of scope for CPU work** and is not touched by it.

### 5.8 Filesystem / storage

The guest's root device is served by two privileged-SVC "memory-disk bridges"
(`md_bridge.c`, `md_raw_bridge.c`) that `memcpy` directly between guest RAM and
a host `vm_block_t` (`vm_block.c`, `tools/file_block.c`). This is one of
several host-side writers of guest RAM that bypass the bus (others: loaders,
`s5l8900_load`, snapshot restore, bring-up) — relevant to any code cache.

### 5.9 Logging and diagnostics

Guest UART console (tee'd to `uart-console.log` by `-F` runs; shown in the
app's Console), bootkernel's run header/config lines, progress checkpoints,
device-access census, fault sites, call probes, framebuffer dumps; the app's
status publication and on-device self-test. There is **no circular recent-PC
trace buffer in the core**; bootkernel keeps its own diagnostic windows.

### 5.10 Tests

`ctest` runs 77 tests at `4a8480f` (all pass locally in 3.5 s): CPU
(`test_arm` ~810 assertions, `test_vfp`, `test_soc` ~5.5 k assertions incl.
run/tick differential), MMU/TLB inside those, every device, firmware parsers,
snapshot, bridges, networking, app C logic, policy checks on `Info.plist` and
entitlements. `tools/arm_diff_probe.c` + `unicorn_diff.py` is an out-of-tree
differential oracle against Unicorn. The `75ad89f` tests
(`test_block_cache`, `test_cached_interp`, `test_ir`, `test_fastmem`,
`test_profiler`) check hand-picked single cases and do not compare against the
reference in general.

### 5.11 Existing profiling facilities

- `bootkernel -W` windowed sampling profile with guest symbolisation
  (`machoinfo`, `dscmap.py`), `--sequence-profile` (exact instruction-class /
  site / pair census), `--run-api` (app-shaped timed chunks), `--frame-meter`
  (changed-frame cadence), call probes, device census.
- `insnbench` (synthetic loops, interleaved reps, end-state checked),
  `jitbench` (static/JIT ceilings), CI throughput steps on three hosts, the
  `ios-device-benchmark` and `fetch-refill-perf` workflows, gprof builds.
- **Not trustworthy:** `insnbench --profile` and `arm_profile.c` (see §7).

## 6. Execution engines present at the baseline

| Engine | Default | Status |
|---|---|---|
| Reference interpreter (`arm_step`) + User-mode tick batching | **yes, everywhere** | The specification. Exact, deterministic, heavily differential-tested |
| Static AArch64 "compact raw" engine (`a64_static_engine.c`, `tools/a64_static.c`) | on in the iOS app (arm64 only) | User-mode only, falls back before mutation; measured +6.06 % on A9 |
| Runtime JIT (`core/src/jit`) | off | Never called by the run loop; excluded from the app |
| `CACHED_BLOCK` / `IR_OPTIMIZED` / `JIT` backends (`75ad89f`) | off since `c05df84` | **Unsound** — see §7 |

## 7. Audit of the `75ad89f` / `c05df84` / `4a8480f` tiers

These three commits (2026-09-23) added a pre-decoded block cache with linking,
a micro-op IR with an optimiser, a "fastmem" layer, a profiler and an app
backend picker in one 4.7 k-line commit, then defaulted the app back to the
interpreter after a black screen. Findings, each reproduced rather than
inferred:

1. **Wrong results on kernel-critical instructions.** A scratch differential
   probe running identical code through `s5l8900_run()` with each backend
   found 8 mismatches in 11 cases: `SUBS pc, lr, #4` does not restore CPSR
   from SPSR (every IRQ return), `MRS` writes nothing, `MSR CPSR_f` is dropped,
   Thumb `BLX` loses the Thumb bit in LR, `LDR pc` does not interwork (IR).
   Code review additionally shows shift-by-32/`RRX` forms and rotated-immediate
   carry-out are wrong in `eval_shift`.
2. **Timing is not exact.** Blocks of up to 64 instructions run past the next
   timebase edge and in privileged mode; the reference and the static engine
   both stop at the edge. Runs are no longer reproducible against the
   interpreter.
3. **Self-modifying code / context switches.** Blocks are keyed by virtual
   address + privilege only; invalidation happens only on stores executed by
   the tier itself. Stores by the interpreter, DMA, the disk bridges or a new
   address space leave stale blocks.
4. **Memory semantics bypassed.** `arm_fastmem_*` ignore `SCTLR.A/U`, legacy
   rotation and page-crossing splits.
5. **Cycle accounting** drops retired instructions on early exits.
6. **Fabricated profiling.** `insnbench --profile` prints a time breakdown
   computed as fixed fractions (65 % exec, 20 % decode, 10 % MMU, 5 % memory)
   and `branches = insns/5`. None of it is measured.
7. **CI red since `75ad89f`.** The `warnings as errors` job fails on a
   use-after-free of `sorted` in that `--profile` block. All other jobs,
   including Windows/MSVC, pass.
8. The tier's headline speedups (e.g. `alu/branch CACHED 213 M/s` vs 85 M/s
   literal) are measured with the MMU off, no device tick and outside
   `s5l8900_run`, on code the tier executes incorrectly in general.

The reference-interpreter throughput is unaffected by these commits (e.g.
`tick=run` rows at `326ae10` vs `4a8480f`: 51.1–52.2 vs 48.7–51.6 M/s
alu/branch, 40.1–41.6 vs 41.2–42.0 M/s load/store, same machine, interleaved).

## 8. Known existing defects and limitations (not caused by CPU work)

Recorded so that later work is not blamed for them. Sources: README,
`QUALITY.md`, `ROADMAP.md`, `audio.md`, `AUDIO_DIAGNOSTICS.md`.

- On-device foreground cadence can fall to ~0–4 fps; no measured arm reaches
  30 fps. Dominant measured costs in the steady state are RSA/crypto in
  `Security.framework` (`_mulg_common`) and QuartzCore's software rasteriser,
  not decode alone (`hotpath.md`).
- Audio: codec/I²S/DMA are modelled; an AMC/BSU "could not lock BSU" freeze
  reproduces on device; audible output is not established.
- Networking: PPP link comes up; no guest IP packet has been carried; the app
  has no PPP endpoint.
- MBX graphics is experimental (hidden from the guest by default).
- RTC returns a placeholder; baseband, Wi-Fi, Bluetooth, camera,
  accelerometer are not modelled.
- Installed third-party apps: an install path exists; compatibility is not
  characterised, and there is no crash classification pipeline.
- Two-finger gestures reach userspace but none has visibly moved anything.
- The `75ad89f` tiers (§7) and the red strict-warnings CI job.
