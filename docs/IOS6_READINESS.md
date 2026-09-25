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
| ARMv7 profile switch | `arm_arch_t` in `core/include/arm.h`: `ARM_ARCH_V6_ARM1176` (default), `ARM_ARCH_V7_SWIFT`, and `ARM_ARCH_V7_A8` (the 3GS). Code asks `arm_arch_is_v7()` / `arm_arch_has_divide()`, never the enum's order: the A8 is ARMv7 without the divider | Today the engine hands an ARMv7 core to `arm_step` whole (`ARM_CI_STEP_PROFILE`): correct, and as slow as the reference. Step 3 below removes that. |
| Thumb-2 (32-bit Thumb) | **Yes, in the reference interpreter** (2026-09-25; see step 2). On the ARM1176 `0xE800..0xFFFF` still decode as the two BL/BLX halves | Largest engine change: variable-length Thumb records; a 32-bit Thumb instruction may straddle a 1 KiB fetch block, so the block builder must stop before it (the reference already fetches the second half separately); `IT` blocks carry state in CPSR (ITSTATE), so `IT` and the instructions it covers must either be REF with a block end or handled with explicit ITSTATE advance. |
| NEON / VFPv3-D32 (VFPv4 on A6) | **No** (VFPv2 only) | REF first; later specialise the few ops the shared cache's `memcpy`/string routines use (`vld1`/`vst1`, `vmov`), measured. `ROADMAP.md`'s census puts NEON at ~0.37 % of the iOS 8 kernel. |
| ARMv7 system: DMB/DSB/ISB, VMSAv7 (TEX remap, PXN, ASIDs), CP15 layout | Barriers, CLREX and the hints (WFI waits) in both states; SCTLR.U/XP read as one, SCTLR.TE, ITSTATE across exceptions, MSR/MRS execution-state rules. **No** VMSAv7 or v7 CP15 identification | Barriers are no-ops single-core but ISB/`MCR` cache maintenance must keep ending blocks (they already STOP). ASID-tagged translation would let the engine stop purging on every TTBR write: `tlb_gen` flushes today. |
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

   **First slice done (2026-09-25).** The Cortex-A8 profile runs the whole
   integer Thumb-2 set, IT blocks, CBZ/CBNZ, TBB/TBH, the Thumb exclusives
   and LDRD/STRD, VFPv2 in Thumb state (the lazy-enable trap included), and
   in ARM state the ARMv6T2 additions (MLS, SBFX/UBFX, BFI/BFC, RBIT) and
   interworking ALU writes to PC. UMAAL, which the ARM1176 also has, was
   missing in ARM state and is now there for both. How it was checked:

   - `tools/unicorn_thumb2_diff.py --count 1500 --seed 11` against
     Unicorn's Cortex-A8: 10,117 cases where both executed, **0**
     differences in r0-r15, CPSR or a hash of RAM, and **0** encodings
     accepted that Unicorn refused. The encodings S5LBox refuses and Unicorn
     runs are the UNPREDICTABLE forms (PC as an operand, PC in an STM list,
     writeback into a transfer register, SBO/SBZ violations, MSR of an
     invalid mode), and its extra faults are misaligned LDM/STM/LDRD, which
     ARMv7 faults and Unicorn 2 (QEMU 5) does not check.
   - A third `bench/guest` image, `thumb2` (clang `armv7a`, `cortex-a8`,
     `-mthumb`, VFPv2 hard-float: 42 IT blocks, TBB, TBH, CBZ, LDRD/STRD,
     BFI, MLS, UMLAL, VFP in Thumb), runs every workload in
     `test_guest_workloads` against the host checksum, with the timer-FIQ
     variant: of 17,888 FIQs taken, 452 interrupted an IT block.
   - `core/tests/test_armv7.c`, 91 checks for what neither of those reaches:
     ITSTATE stacked by SVC (next state) and by an abort or undefined
     instruction (own state), the Thumb undefined link (first halfword + 2),
     a 32-bit instruction across a page (prefetch abort with IFAR = pc + 2)
     and across a fetch block, VLDR's Thumb literal base, and every place the
     A8 and the ARM1176 must differ. Five planted bugs were each caught.
   - Cost to the iPhone OS 3 machine, in host instructions under
     cachegrind (7 workloads, `--div 40`): Thumb on the reference
     interpreter +0.54 %, ARM on it -0.36 %, the cached engine (the app's
     default) unchanged in both.

   Not yet: SRS/RFE in Thumb state, VMSAv7 (TEX remap, access flag, ASIDs),
   the A8's CP15 identification and cache registers, VFPv3 (d16-d31,
   VMOV immediate) and NEON, ThumbEE, and saving `arch` in snapshots (no
   ARMv7 machine exists yet to save).
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
