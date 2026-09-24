# iOS 6 readiness

Status on 2026-09-23: **S5LBox cannot run iOS 6, and no amount of CPU-engine
speed changes that.** This document records why, what the cached-interpreter
work does and does not prepare, and the order of work that would get there.
It extends `ROADMAP.md` P2 (second machine profile), which already scopes an
iPhone 5 / iOS 8.4.1 target, rather than competing with it.

## 1. iOS 6 is a different machine

S5LBox emulates the Samsung S5L8900 (iPhone 2G / iPhone 3G / iPod touch 1G):
an ARM1176JZF-S (ARMv6K, Thumb-1, VFPv2), an MBX Lite GPU and a
software-composited iPhone OS 3 SpringBoard. iOS 4.2.1 was the last release
for any S5L8900 device; iOS 6 never shipped for this SoC.

Every iOS 6 device is ARMv7 (Cortex-A8/A9 or Apple Swift), and every one uses
a PowerVR SGX GPU:

| Candidate | SoC / CPU | GPU | Cores | Notes |
|---|---|---|---|---|
| iPhone 3GS | S5L8920, Cortex-A8, ARMv7-A | SGX535 | 1 | oldest iOS 6 device; last release 6.1.6 |
| iPhone 4 / iPod touch 4G | A4 (S5L8930), Cortex-A8 | SGX535 | 1 | |
| iPhone 5 | A6 (S5L8950X), Swift, ARMv7s | SGX543MP3 | 2 | shipped with iOS 6; the `ROADMAP.md` P2 target (for iOS 8.4.1) |

The single-core Cortex-A8 devices avoid SMP and ARMv7s-specific instructions
(VFPv4 FMA). The iPhone 5 is what the project has already scoped, keeps IMG3
and reuses more of P2. **Choosing the target is the owner's decision** and the
first step below.

## 2. What the CPU work so far does and does not prepare

| Needed for iOS 6 | Exists today | Cached interpreter impact |
|---|---|---|
| ARMv7 profile switch | `arm_arch_t` in `core/include/arm.h`: `ARM_ARCH_V6_ARM1176` (default) and `ARM_ARCH_V7_SWIFT`; gates ARM-state MOVW/MOVT and SDIV/UDIV today | The engine decodes independently of `arch` and runs everything it does not specialise through the reference, so a v7 profile stays correct as long as each new v7 encoding is either REF or specialised with the same fuzzer proof. The decoder must learn which v6-UNDEFINED encodings v7 defines, so it does not STOP on them forever (correct but slow). |
| Thumb-2 (32-bit Thumb) | **No.** Thumb-1 only; `0xE800..0xEFFF` decodes as a BLX suffix | Largest engine change: variable-length Thumb records; a 32-bit Thumb instruction may straddle a 1 KiB fetch block, so the block builder must stop before it and the reference fetch must translate both halves; `IT` blocks carry state in CPSR (ITSTATE), so `IT` and the instructions it covers must either be REF with a block end or handled with explicit ITSTATE advance. |
| NEON / VFPv3-D32 (VFPv4 on A6) | **No** (VFPv2 only) | REF first; later specialise the few ops the shared cache's `memcpy`/string routines use (`vld1`/`vst1`, `vmov`), measured. `ROADMAP.md`'s census puts NEON at ~0.37 % of the iOS 8 kernel. |
| ARMv7 system: DMB/DSB/ISB, VMSAv7 (TEX remap, PXN, ASIDs), CP15 layout | Partial/none | Barriers are no-ops single-core but ISB/`MCR` cache maintenance must keep ending blocks (they already STOP). ASID-tagged translation would let the engine stop purging on every TTBR write: `tlb_gen` flushes today. |
| Unaligned access always permitted (SCTLR.U fixed) | Handled by the reference through SCTLR | Engine fast paths already fall back on any misalignment. |
| SMP (A6 only) | **No** | Per-core CPU state and engine instance; the code bitmap and region generations must be shared so a store on one core invalidates blocks the other runs; exclusive monitor becomes global; deterministic interleaving quanta. XNU can boot single-core by boot-arg, which defers it. |
| SoC: memory map, interrupt controller, timers, clocks, NAND/eMMC, I²C/SPI devices, display | S5L8900 models only | None. New device models, from scratch. |
| GPU: SGX535 / SGX543MP3 | **No** (MBX partial) | None. iOS 6 SpringBoard is GPU-composited through CoreAnimation → IOMobileFramebuffer with no software fallback. `ROADMAP.md` P2 names this the decisive risk: emulate an undocumented GPU, or shim IOSurface/IOMobileFramebuffer higher up. **Answer before building anything else.** |
| Boot chain: iBoot/LLB, IMG3 keys, device tree | IMG3 parsing reusable (A4–A6 use IMG3) | None. |

## 3. Order of work (smallest risk first)

1. **Decide the target device** (§1) and **answer the GPU question** with a
   written design, as `ROADMAP.md` P2 already requires. The target is the
   iPhone 3GS on iOS 6.1.6 (chosen 2026-09-24); the design note and the
   firmware questions that decide it are in `IOS6_GRAPHICS.md`.
2. **ARMv7 CPU profile in the reference interpreter**: Thumb-2 decode, IT
   blocks, VMSAv7, barriers, LDREX B/H/D, the v7 CP15 set, VFPv3/NEON as
   needed. Pure CPU work, testable in CI with no firmware: extend
   `test_ci_diff` with a v7 generator and add compiled ARMv7/Thumb-2 guest
   workloads to `bench/guest` (`--target=armv7a-none-eabi`), exactly as the
   ARMv6 suite is built.
3. **Cached interpreter for v7**: Thumb-2 variable-length records and the
   straddle rule, IT handling, then specialisations in order of a kernel
   census (`tools/kcensus.py`), each kept only with a fuzzer proof and a
   measured gain.
4. **SoC model** for the chosen device, then boot to the kernel's first
   console output, then userspace.
5. SMP only if the chosen device needs it and a single-core boot-arg is not
   enough.

## 4. What must not regress along the way

- The ARM1176 profile stays the default (`arch == 0`), and every v7 encoding
  must still be UNDEFINED on it — the reason `arm_arch_t` is a runtime field
  rather than a compile-time switch.
- iPhone OS 3.1.3 boot behaviour on the S5L8900 machine, the compiled ARMv6
  workload suite, and the differential fuzzer stay green at every step.
- No JIT: the engine design (predecoded records, reference fallback) carries
  to ARMv7 without executable memory.
