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
| ARMv7 profile switch | `arm_arch_t` in `core/include/arm.h`: `ARM_ARCH_V6_ARM1176` (default), `ARM_ARCH_V7_SWIFT`, and `ARM_ARCH_V7_A8` (the 3GS). Code asks `arm_arch_is_v7()` / `arm_arch_has_divide()`, never the enum's order: the A8 is ARMv7 without the divider | The engine decodes for the core it runs: ARMv7 ARM state differs in an interworking `MOV pc` and the WFI hint, both handled (step 3). |
| Thumb-2 (32-bit Thumb) | **Yes, in the reference interpreter** (2026-09-25; see step 2). On the ARM1176 `0xE800..0xFFFF` still decode as the two BL/BLX halves | **Done (step 3, first slice).** ARMv7 Thumb blocks carry a halfword-offset table for their mixed 16/32-bit records; a 32-bit instruction that straddles a 1 KiB block ends the block before it; `IT` and the instructions it covers are REF records, and a block is never entered with ITSTATE live. |
| NEON / VFPv3-D32 (VFPv4 on A6) | **Yes** on the A8 profile (2026-09-25): VFPv3-D32 (step 2, second slice) and all of ARMv7 Advanced SIMD, data processing and element/structure loads and stores (third slice) | REF first; later specialise the few ops the shared cache's `memcpy`/string routines use (`vld1`/`vst1`, `vmov`), measured. `ROADMAP.md`'s census puts NEON at ~0.37 % of the iOS 8 kernel. |
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

   **Second slice, VFPv3 (2026-09-25).** The register file is d0-d31 (the
   ARM1176 still refuses every encoding that names d16-d31), and the A8
   profile adds VMOV (immediate), VCVT between floating and fixed point,
   FPSID/MVFR0/MVFR1 with Unicorn's Cortex-A8 values, the A8's FPSCR and
   FPEXC write masks, and the Advanced SIMD 8/16-bit scalar transfers and
   VDUP from a core register. Snapshots are unchanged on disk: d16-d31 are
   not stored, and a machine whose CPU is not the ARM1176 is refused.

   Checked with a new driver, `tools/unicorn_neon_diff.py`, which compares
   r0-r15, CPSR, RAM, FPSCR and d0-d31 against Unicorn's Cortex-A8 in ARM
   and Thumb state: `--count 2000 --seed 7` gave 23,021 cases where both
   executed, **0** differences and **0** encodings accepted that Unicorn
   refused. Getting there found five bugs that the ARM1176 shares, all fixed:
   NaN propagation took the host's operand order (ARM's signalling-first
   rule is now explicit); an Invalid Operation made the host's negative NaN
   instead of ARM's positive default NaN; VSQRT of a quiet NaN raised IOC (a
   signalling `<` compare); flush-to-zero missed a tiny result the host had
   already rounded to zero (UFC alone, not IXC); and VMOV between two core
   registers and a double accepted bit 4 clear, which is UNDEFINED. Six cases
   sit in a `qemu-bug` bucket: QEMU 5.0's double-precision VMOV/VABS/VNEG/
   VSQRT short vectors write element 1 into the source register (isolated
   with directed cases; its single-precision and three-operand forms agree).
   Short vectors with stride 2 are not generated, because QEMU 5.0 steps
   FPSCR.Stride + 1 registers. `test_vfp` (620 checks) and `test_armv7`
   (131) pin one answer for each fix and each new instruction. Cost in host
   instructions (cachegrind, `cpubench --workload vfp --isa arm --div 8`):
   interpreter +0.56 %, engine +0.84 %, the engine's all in `f32_do`'s test
   for a NaN result.

   **Third slice, Advanced SIMD (2026-09-25).** `core/src/arm/neon.c` runs
   every ARMv7 NEON instruction on the A8 profile, in ARM (0xF2/0xF3/0xF4)
   and Thumb (0xEF/0xFF/0xF9) state: the three-register, by-scalar, shift,
   modified-immediate and miscellaneous groups, VEXT, VTBL/VTBX, VDUP, and
   VLD1-4/VST1-4 in their multiple, single-lane and all-lanes forms with
   alignment checks and writeback. Floating point runs under ARM's standard
   FPSCR (flush-to-zero, default NaN) through the VFP unit's own rounding
   step; FPSCR.QC records saturation. The ARM1176 still refuses all of it.

   Checked with `tools/unicorn_neon_diff.py` against Unicorn's Cortex-A8 in
   ARM and Thumb state: data processing, `--count 2500 --seed 3`, 24,849
   cases where both executed, every NEON mnemonic among them (`--coverage`
   counts agreeing cases per mnemonic; the thinnest in a targeted run of
   the 3-same, miscellaneous and shift groups, `--count 5000 --seed 21`,
   were VMOVL 3, VRECPS 27 and VRSQRTS 29); loads and stores,
   `--count 2000`, 6,607 cases. All with **0** differences and **0** encodings accepted that
   Unicorn refused. S5LBox refuses what the ARMv7 ARM calls UNDEFINED even
   where QEMU 5 runs it (VMUL.F32 with bit 21 set, VQDMULL/VQDMLAL with
   U=1), and the UNPREDICTABLE forms (a register list past d31, VZIP/VUZP/
   VTRN of one register with itself, a zero modified immediate). Its extra
   faults are alignment faults QEMU 5 does not raise: with every base
   register 32-byte aligned that bucket is empty. Two bugs were found on
   the way, both fixed: VQDMULH/VQRDMULH treated U as unsigned (it selects
   the rounding form), and in ARM state the ARMv6 PLD test claimed any
   0xF4 load or store with Vd = 15; the engine had the same test, and
   `test_ci_diff`, which now mixes NEON into both ARMv7 passes and compares
   d16-d31, catches it when it is put back. Cost to the iPhone OS 3 machine
   (cachegrind, all workloads, `--div 40`, against 80931e1): ARM +0.045 %
   on the interpreter and +0.043 % on the engine, Thumb under 0.001 %.

   Not yet: SRS/RFE in Thumb state, VMSAv7 (TEX remap, access flag, ASIDs),
   the A8's CP15 identification and cache registers, CPACR.ASEDIS/D32DIS,
   ThumbEE, and saving `arch` in snapshots (no ARMv7 machine exists yet to
   save).
3. **Cached interpreter for v7**: Thumb-2 variable-length records and the
   straddle rule, IT handling, then specialisations in order of a kernel
   census (`tools/kcensus.py`), each kept only with a fuzzer proof and a
   measured gain.

   **First slice done (2026-09-25).** Before it the engine handed an ARMv7
   core to `arm_step` one instruction at a time, and measured *slower* than
   the plain interpreter on the `thumb2` image: 0.61-0.72x per workload
   (`cpubench --isa thumb2 --mode svc --div 4`). Now 32-bit Thumb-2 maps onto
   the existing specialised records wherever the operation is the same
   (data processing with immediates and shifted registers, ADDW/SUBW/ADR,
   MOVW, loads and stores in their immediate, register and literal forms,
   LDM/STM/PUSH/POP, B/BL/BLX/B<c>, the multiplies and long multiplies,
   extends, REV, CLZ, the hints and barriers) plus four new ones (Thumb
   BL and BLX, CBZ/CBNZ, MOVT); the rest runs through the reference. Result
   on the same image: 4.29x the interpreter, geometric mean of 21 rows (SVC
   and User); vfp is the weak row (1.48-1.74x) because Thumb VFP is still a
   reference record.

   How it was checked: `test_ci_diff` gained a Thumb-2 pass and an ARMv7
   ARM-state pass (0 mismatches over 4 seeds, each 20,000 + 10,000 runs, with
   random ITSTATE at entry and in the SPSRs); `test_guest_workloads` compares
   the engine with the interpreter on the `thumb2` image, FIQ variant
   included; `test_armv7` covers the case the fuzzer cannot reach (an
   exception return landing, in the same mode, on the next instruction with
   ITSTATE live). Of ten planted engine bugs, nine were caught; the tenth
   (IT-covered 16-bit instructions decoded as specialised records) changes
   no result, because the ITSTATE guard in the reference path then leaves
   the block, and it is caught together with that guard's removal. Cost to
   the iPhone OS 3 machine in host instructions
   (cachegrind, 7 workloads, `--div 40`): Thumb on the engine +0.40 %, ARM on
   it -0.19 %.

   NEON runs in-block as reference records (2026-09-25), in both states:
   correct, and no longer a block boundary, but not yet specialised.

   Next in this step: Thumb VFP and the NEON forms libSystem's `memcpy` and
   string routines use (VLD1/VST1, VMOV, VDUP) as specialised records,
   chosen by a census of real iOS 6 code and each kept only with a fuzzer
   proof and a measured gain; LDRD/STRD and the IT-covered instructions as
   specialised records with explicit conditions.
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
