# iOS 6 readiness

Status on 2026-09-25: **NEON cannot run iOS 6 yet, and no amount of CPU-engine
speed changes that.** Its kernel does now start: on a bare research machine
(`tools/boot3gs.c`, step 4) it boots to IOKit and prints over the serial
console, and that machine is now in the core (`core/src/soc/n88.c`), but it
has no storage, display or input yet. This document records why, what the cached-interpreter
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
| Advanced SIMD (NEON) / VFPv3-D32 (VFPv4 on A6) | **Yes** on the A8 profile (2026-09-25): VFPv3-D32 (step 2, second slice) and all of ARMv7 Advanced SIMD, data processing and element/structure loads and stores (third slice) | REF first; later specialise the few ops the shared cache's `memcpy`/string routines use (`vld1`/`vst1`, `vmov`), measured. `ROADMAP.md`'s census puts NEON at ~0.37 % of the iOS 8 kernel. |
| ARMv7 system: DMB/DSB/ISB, VMSAv7 (TEX remap, PXN, ASIDs), CP15 layout | Barriers, CLREX and the hints (WFI waits) in both states; SCTLR.TE, ITSTATE across exceptions, MSR/MRS execution-state rules. **The CP15 set iOS 6's kernel uses** (2026-09-25, step 2 fourth slice): the A8's identification and cache registers, PAR and ATS, the L2 auxiliary control, ARMv7's User-mode rules and SCTLR/TTBCR/CPACR masks; the ARMv7 short-descriptor walk, checked against Unicorn. The A8 has no PXN | Barriers are no-ops in every mode; ARMv7 cache maintenance is a no-op record that hands User mode to the reference, and PAR/ATS stay on `arm_step`. ASID-tagged translation would let the engine stop purging on every TTBR write: `tlb_gen` flushes today. |
| Unaligned access always permitted (SCTLR.U fixed) | Handled by the reference through SCTLR | Engine fast paths already fall back on any misalignment. |
| SMP (A6 only) | **No** | Per-core CPU state and engine instance; the code bitmap and region generations must be shared so a store on one core invalidates blocks the other runs; exclusive monitor becomes global; deterministic interleaving quanta. XNU can boot single-core by boot-arg, which defers it. |
| SoC: memory map, interrupt controller, timers, clocks, NAND/eMMC, I²C/SPI devices, display | S5L8900 models only. The 3GS inventory is read from its device tree (step 4) | None. New device models, some adapted from the S5L8900 ones. |
| GPU: SGX535 / SGX543MP3 | **No** (MBX partial) | None. **Answered for the 3GS (2026-09-25):** iOS 6.1.6's window server falls back to QuartzCore's software renderer when OpenGL is off, selected by `CA_ENABLE_OGL=0` in `backboardd`'s environment (`IOS6_GRAPHICS.md` §6). So the home screen and UIKit apps need no GPU model; games that draw with OpenGL ES still do. |
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
   ARM and Thumb state, every NEON group, `--count 3000 --seed 21
   --coverage`: 43,338 cases where both executed, **0** differences and
   **0** encodings accepted that Unicorn refused. Every NEON mnemonic
   agreed at least 50 times except VMOVL (2: it is VSHLL with a zero
   shift, which random encodings rarely hit), VSHRN (21), VQSHRUN (28) and
   VREV16 (37); those four were also run as directed cases against Unicorn
   in both states, and `test_armv7` pins a VMOVL answer. S5LBox refuses what the ARMv7 ARM calls UNDEFINED even
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

   **Fourth slice, CP15 and the page tables (2026-09-25).** Scoped by the
   firmware rather than the ARM ARM: `tools/kcp15.py` lists every MCR/MRC to
   CP15 in the 6.1.6 kernelcache (the user's, decrypted and kept out of the
   repository), and each site below was read in a disassembly. What the
   kernel proper uses, beyond what the ARM1176 path already had:

   | Registers | Where, and what for |
   |---|---|
   | MIDR | read once (Thumb, 0x80088278); 0x8007dc8c overwrites its architecture field with 8, and 0x8008db94 maps implementer 0x41, part 0xc08 to its Cortex-A8 CPU family. The revision is not consulted |
   | CLIDR, CSSELR, CCSIDR | read once at boot (0x8007dccc) to size the L1 data and L2 caches for sysctl: sets x ways x line |
   | PAR, ATS1CPR/CPW/CUR | its virtual-to-physical lookups (0x80088e80, 0x80088ed0, 0x80088f20), which test PAR.F and PAR.SS and clear [11:0] |
   | L2 auxiliary control (p15,1,c9,c0,2), ACTLR | read-modify-written at entry (0x80086348, 0x80086364) |
   | c7 set/way and by-MVA maintenance, TLBIALL/MVA/ASID/MVAA, ITLBIALL | its cache and TLB routines. The set/way loops hard-code the geometry: 128 sets x 4 ways at level 1, 512 x 8 at level 2, 64-byte lines |
   | c15 (the A8's cache and TLB debug arrays) | only routines that dump them (0x8007c65c-0x8007c830, 0x80093644) |

   Its SCTLR ORs at entry (0x800863a4) are U, XP, V, I, Z, W, C and M, so
   iOS 6 runs with neither TEX remap nor the access flag, and it only ever
   writes TTBCR.N (1 or 2). It never touches PRRR, NMRR, VBAR or the
   performance monitors.

   `exec_cp15_v7()` now decodes the A8's CP15 exactly (opc1, CRn, CRm,
   opc2, where the ARM1176 path keys most registers on CRn alone):
   identification values from Unicorn's Cortex-A8, except ID_PFR0/PFR1/DFR0,
   which report what is implemented here (no ThumbEE, no Security
   Extensions, no debug); a cache hierarchy encoding the kernel's own loop
   geometry (32 KB L1, 256 KB L2); PAR and the four ATS1C* operations,
   through a new `arm_mmu_ats()` that walks without the TLB or an abort;
   SCTLR's read-as-one and read-as-zero bits; CPACR's CP10/CP11 fields
   only; and ARMv7's User-mode rule that c7 allows only the three barriers
   (Unicorn agrees on each). MRC to r15 sets NZCV, and MCR from r15, being
   UNPREDICTABLE, is refused. The walk itself needed no change: ARMv7's
   short-descriptor format is the ARMv6 extended one that `mmu.c` already
   walks. The engine keeps barriers as in-block no-ops, runs other ARMv7
   cache maintenance as a no-op only in privileged modes, and leaves PAR and
   ATS to `arm_step`.

   Checked with a new driver, `tools/unicorn_vmsa_diff.py`, which builds a
   random short-descriptor translation for one address (fault, reserved,
   section, supersection, or a page table with a fault, large or small page;
   random AP, APX, domain, XN and attribute bits; random TTBCR.N, PD1, DACR,
   AFE and TRE), runs all four ATS operations and then a real LDR, STR, LDRT
   or STRT on both sides, and compares the registers, the PARs and whether
   the access aborted; when both abort, our DFSR and DFAR must match the
   fault Unicorn's own ATS reported. `--count 30000 --seed 3`: 21,982 agree
   (19,116 of them with both sides aborting), 5,177 reach physical memory
   outside Unicorn's 1 MB (everything before the access is still compared),
   **0** differences, and 2,841 in a `qemu-bug` bucket with three rules:
   QEMU 5.0 checks the domain before it reads a page's second-level
   descriptor and before the access flag, and checks the access flag only in
   Client domains (1,201 + 995 + 645). The ARM ARM orders these faults
   Translation, Access flag, Domain, Permission, as `mmu.c` already does,
   and QEMU's maintainers set out the same order in a 2026 patch that moves
   QEMU's check ([qemu-arm](https://ratatoskr.run/qemu-arm/2026/08/17460220/t)).
   A case is accepted into that bucket only if all four of our PARs are the
   architecture's answer. Three more things were Unicorn's and are kept out
   of the comparison instead: it stores SCTLR as written, so writing XP = 0
   switches it to the ARMv5 table format (no APX) and B = 1 to big-endian
   fetches, both impossible on ARMv7; and its Cortex-A8 runs Non-secure,
   which sets PAR.NS.

   `test_armv7` gained 55 checks (197 in all): the identification values,
   CCSIDR's sizes through the kernel's own arithmetic, every mask, ATS over
   a small table (page, read-only page, hole, supersection, No access
   domain, PD1, MMU off) with no abort taken, a real STRT agreeing with ATS,
   and User mode against both the reference and the engine. `test_ci_diff`
   now reaches PAR, ATS, CSSELR, CCSIDR and the L2 register in its ARMv7
   ARM pass and compares them: 0 mismatches over 4 seeds. Cost to the
   iPhone OS 3 machine (cachegrind, `cpubench --mode user --workload
   mmu,sort,calls,crc32 --div 40`, against 875a3f7): interpreter -0.04 % in
   ARM and Thumb, engine -0.08 % in both. A first version cost +0.56 % on
   the `mmu` workload alone, because a second caller made the compiler stop
   inlining the table walk; it is now always inlined.

   Not yet: SRS/RFE in Thumb state, ThumbEE, the Security Extensions (SMC,
   Monitor mode, VBAR), PRRR/NMRR (TEX remap changes only memory types,
   which nothing models), ASID-tagged TLB entries (a speed item: every
   CONTEXTIDR and TTBR write still flushes), and saving `arch` in snapshots
   (no ARMv7 machine exists yet to save).
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

   **The 3GS inventory (2026-09-25),** from the user's 10B500 device tree
   (`DeviceTree.n88ap.img3`, 89 nodes; decrypted with the user's key, kept
   out of the repository). One Cortex-A8 (`cpu0`, `ARM,v7`). Everything
   else hangs off `/arm-io` (`s5l8920x`). What the compatible strings
   suggest, before any register is read:

   | Likely adaptable from the S5L8900 models | New to this machine |
   |---|---|
   | `vic` (`pl192`, `0x3f200000`) | `pmgr` (power, clocks; `0x3f100000`, `0x3fc00000`) |
   | `uart0-4` (`uart-1,samsung`) | `cdma` DMA (replaces the PL080) |
   | `spi0-2` (`spi-1,samsung`; NOR, multi-touch, baseband) | `dart0/1` IOMMUs |
   | `i2c0/2`, `gpio`, `pwm` (Samsung-style, to be checked) | `flash-controller0` (`fmi,s5l8920x`) NAND |
   | `usb-complex` (lists `s5l8900x` as compatible) | PMU `d1755` (Dialog), codec `cs42l61`/`cs42l58` |
   | `clcd`, `tv-out` (list `s5l8720x` as compatible) | `mipi-dsim` display link, `sgx`, `vxd`, `venc`, `isp`, `jpeg`, `scaler` |

   The nearest milestone, the kernel's first console line, needs the CPU
   pieces above plus the VIC, a timer source, a UART and `pmgr`; the rest
   waits for the boot to ask for it, as it did on the current machine.

   **First console line reached (2026-09-25).** `tools/boot3gs.c` is a
   research harness, not a machine model: it loads the user's decrypted
   6.1.6 kernelcache and device tree into 256 MB of DRAM at `0x40000000`
   (the kernel is linked at `0x80000000`; `/arm-io` puts devices at
   physical `0x80000000` and up), fills in what iBoot would (`/memory`, the
   clock frequencies, `/pram`, the memory-map entries, `boot_args`), starts
   the Cortex-A8 profile at the entry point with the MMU off, and logs every
   device access, exception and UART byte. Two things stood between the
   kernel and its console, each found by running it:

   - `boot_args.Version` must be **5**. `pe_identify_machine` (0x8027ace0)
     loads it and panics "Epoch Mismatch" otherwise. iPhone OS 3's kernel
     wants 6.
   - `/pram:reg` must name real memory of at least 16 KB. The platform
     expert maps it for its panic log (0x8027ba54) and faulted on the
     template's {0, 0}. The harness reserves the top 1 MB of DRAM for it,
     with `boot_args.memSize` stopping below.

   With those, the kernel (`xnu-2107.7.55.2.2~1/RELEASE_ARM_S5L8920X`) turns
   its MMU on, maps UART0 and configures it exactly as the S5L8900's
   Samsung UART is configured (ULCON 3, UCON 0x405, UBRDIV 0x80019, FIFOs
   on), starts IOKit and loads corecrypto, whose eight FIPS self-tests
   (integrity, AES-CBC, TDES-CBC, SHA, HMAC, ECDSA, DRBG) all pass on the
   emulated Cortex-A8 with NEON. The devices it has touched, in order, and
   what the kernel's own code says about them:

   | Device | Where | What the kernel does |
   |---|---|---|
   | UART0 | `0x82500000` | polled console output (`_serial_init`) |
   | PMGR timer | `0xbf100200` | +0x200/+0x204 a 64-bit count read high/low/high (0x800895a4); +0x208 a decrementer, written with an interval and read back as what remains (0x800895d0, 0x800895dc); +0x220 control, bit 0 enable, bit 1 written to acknowledge |
   | VIC0 | `0xbf200000` | line 6 (the timer) selected as FIQ and enabled (0x8027b4f8) |

   The timer's interrupt reaches the kernel through the FIQ vector's
   `mov pc, r9` into a fast path (0x8008958c) that acknowledges it and
   re-arms; its first interval is `0x3a975` = 240,000 ticks, 10 ms at the
   24 MHz timebase the harness advertises. Modelled that way (count =
   retired instructions / 25, i.e. a 600 MHz core; the existing PL192
   model for the VICs), the kernel takes its timer interrupts and keeps
   going: 3 billion instructions, 5.0 s of guest time, 33 interrupts, no
   panic, still inside IOKit. The harness runs the reference interpreter
   one step at a time with its own checks, 3 billion instructions in 131.7 s
   on this host (about 23 M instructions/s); that is the harness's speed,
   not the product's.

   A third requirement surfaced next: `/chosen/nvram-proxy-data`, the
   NVRAM image iBoot passes, is 8 KB of zeros in the template, and
   IODTNVRAM's partition walk (0x80267298) advances by each header's length
   (a little-endian u16 at +2, in 16-byte blocks), so a zero header never
   advances and the walk never ends. The harness now writes the smallest
   image that parses: an empty "common" partition (0x70) and the rest as
   the free-space partition (0x7f, "wwwwwwwwwwww"), with CHRP header
   checksums. With it the kernel goes straight on into IOKit matching and
   starts, printing as it goes, the S5L8920X I/O and GPIO controllers, the
   PL192 VICs, the performance controller (one domain, three voltage and
   four performance states), baseband, SDIO, the camera, H.264 encoder and
   video decoder, both Samsung serial ports, I2C, PWM, MIPI-DSI, SWI and
   the audio complex with its three I2S controllers, until
   `com.apple.driver.AppleARM7M` panics "ARM7M not stopped for some
   reason". ARM7M is the IOP, an ARM7 I/O coprocessor at `0x86300000` and
   `0xbf300000` that the kernel loads with firmware ("EmbeddedIOP firmware
   s5l8920x-RELEASE iBoot-1537.9.55") and must first see stopped. Emulating
   it means a second CPU running Apple's IOP firmware, so for now the
   harness takes `-u arm-io/iop`, the same un-matching the iPhone OS 3
   bring-up uses, to find out what depends on it.

   **Waiting for the root device (2026-09-25).** Without the IOP the kernel
   finishes IOKit matching (the CS42L61 codec, AppleMobileFileIntegrity,
   the zlib decompressor, IOSurface, the M2 scaler, TV-out, CLCD, the
   camera, H.264 and JPEG blocks) and reaches the root mount:
   `Waiting on <dict>…IOMedia…Apple_HFS</dict>`, then "Still waiting for
   root device" every minute of guest time. It is idle there, in WFI
   between timer interrupts; the harness jumps time to each deadline, so a
   10-billion-instruction run covers hours of guest time. The only client
   of the IOP seen so far is AppleIOPSDIOEndpoint ("Failed to get IOP after
   10 sec", every 120 s). This is the stage at which iPhone OS 3 needed the
   memory-disk bridge, and the iOS 6 kernel still carries the same md
   driver (`mdevadd`, `mdevstrategy`, `rd=`, a `RAMDisk` memory-map entry),
   so the same approach applies: find its copy sites in this kernel and
   serve the root filesystem from the host.

   Not yet: the kernel also copies a vector-like block to physical address
   0, which this machine has no memory at (probably the reset trampoline
   for waking the core; only sleep would use it); and what the kernel asks
   for next is what the next runs find.

   **The machine moves into the core (2026-09-25).** What the harness
   discovered is now `core/src/soc/n88.c` (`core/include/n88.h`): DRAM,
   UART0 as a polled console, the PMGR timer, the three VICs, and bring-up
   as iBoot does it, with `core/tests/test_n88.c` covering each on
   synthetic inputs (no Apple bytes). `tools/boot3gs.c` is now a thin
   reporter on top of it, so the app and the harness run the same code.
   The machine can run on the cached interpreter (`boot3gs -e`); on this
   kernel both engines give byte-identical console output, identical
   registers, device-access counts and guest time at 1,000,000,000
   instructions, and the cached interpreter reaches "Still waiting for root
   device" within 3,000,000,000 instructions in 48.3 s of host CPU time
   (62.1 M instructions/s, Linux x86-64 container, one run; single-stepping
   ran about 24 M/s on the same host).

   **In the app, as a preview (2026-09-25).** New Machine offers "iPhone
   3GS · iOS 6 (preview)", a machine with an iPhone 3GS device record
   (`.device-v1`) that runs `n88` through `VMN88Engine` instead of
   `VMEngine`, and shows the kernel's console in the phone's screen area.
   The importer accepts an iPhone2,1 IPSW (`accept_iphone_3gs`) and writes its
   kernel and device tree to `Documents/firmware-iphone3gs`, never over the
   S5L8900 files; it leaves the root filesystem in the archive, because the
   preview mounts none and the unpacker does not yet read iOS 6's disk image.
   On this host's copy of 10B500 it produces a kernel and device tree
   byte-identical to the ones the harness boots. The guest clock is paced to
   the wall clock, so the kernel's once-a-minute messages arrive once a minute.

   **The root device, first slice (2026-09-25).** iOS 6 creates the memory
   disk differently from iPhone OS 3: `IOFindBSDRoot` (0x80270684) passes the
   RAMDisk entry through `ml_static_ptovirt` and calls
   `mdevadd(-1, va >> 12, size >> 12, phys = 0)`, a virtual disk that
   `mdevstrategy` (0x8009765c) reads with plain `bcopy`. The strategy routine
   also has the physical path the bridge already services,
   `bcopy_phys(src64, dst64, len)` one page at a time, so four patches, gated
   on the kernel's LC_UUID and each site's bytes
   (`tools/ios6_kernel_patch.c`), make md0 a physical disk at the token
   address and trap its two copies. `n88` publishes the RAMDisk entry at
   `N88_MD_TOKEN_PA` and installs the bridge; `boot3gs -r` serves an image.
   Result on 10B500: the kernel prints `BSD root: md0, major 3, minor 0` and
   the bridge serves 18 reads with no failures (the HFSX volume header,
   journal info block and header, and B-tree headers), then the system goes
   idle with `rootvnode` still 0 -- the root mount has not completed. Why is
   the next question; the raw-device path (`mdevrw` calling `uiomove64`,
   which iPhone OS 3 needed a second bridge for) is not yet patched.

   **launchd, then a keybag reboot (2026-09-26).** Two more accommodations
   get the root mounted and userland running. The root node's
   `secure-root-prefix` ("md") makes AppleARMPlatform's SecureRootName
   handler wait for a `SecureRoot` platform call from a storage stack this
   machine does not have (10B500: 0x804b0596); `n88` strikes the property
   out, as it un-matches the IOP, so the platform treats the root as
   unchecked and answers at once. And the stock `/etc/fstab` names the NAND's
   disk0s1/disk0s2, so the work image needs it rewritten to `/dev/md0`;
   `rootfs_work_create` already does that (it now accepts the 3GS image's
   4 KiB partition tail past the last allocation block and its empty
   in-volume journal), and it grows the volume, because Apple ships the root
   with zero free blocks (on the phone /private/var is a separate partition)
   and the first file the system writes -- corecrypto's FIPS control file --
   otherwise fails. With all that, the kernel mounts md0, FIPS passes, and
   launchd runs, then hits `FATAL KEYBAG ERROR: kb_load` and reboots into
   recovery: the data partition's keybag (Effaceable storage /
   IOAESAccelerator) is the next thing to model. `boot3gs -P pristine.img
   -r work.img` provisions and boots in one step; `-w` records where each
   kernel thread last blocked.

   **The keybag, mapped (2026-09-26).** Getting here first needed one
   interpreter fix: the Thumb-2 exclusives refused LR as the data register
   (they checked `>= 13` where the architecture forbids only SP and PC), so
   the 10B500 atomics' `LDREX lr, [Rn]`, on the reboot path, stopped the
   harness dead; fixed and Unicorn-verified (commit b07df0a). The keybag
   itself is a missing DATA PARTITION plus two crypto devices, all confirmed:

   - `AppleKeyStore` **is** registered (an IOResources software service, seen
     with boot-arg `io=0xffffff`), so it is not the blocker.
   - `keybagd` (extracted with tools/hfsx_extract.py) reads and writes
     `/private/var/keybags/systembag.kb`; its kb_load worker (Thumb at
     0x3a48, fatal path at 0x249e) fails and the process calls its fatality
     thunk, which the kernel turns into "REBOOTING INTO RECOVERY MODE".
   - `/private/var` is the DATA partition (disk0s2 on the phone), a separate
     volume that is NOT in the system-only root image: the image has no
     `keybags` directory and no `systembag.kb`. So there is no keybag to load
     and nowhere writable and durable to create one. A writable data volume
     at /private/var (a second md device, or a data partition) is the first
     requirement.
   - Creating a keybag then needs the hardware AES engine (`IOAESAccelerator`,
     a prelinked kext present in the cache; on the S5L8920 it is a fixed MMIO
     block, not a device-tree node) for the device UID key, and the effaceable
     lockbox for BAG1. That lockbox is the SPI0 NOR (`effaceable,nor` at NOR
     offsets 0xfa000/0xfb000): spi0's nub registers but its MMIO (0x82000000)
     is never driven, so the NOR and effaceable children never enumerate and
     AppleEffaceableStorage never matches.

   The system keybag a real device stores is wrapped with that device's UID
   key, unknowable here, so loading it could never work regardless; the
   "give device keybag access to everyone" secure-root path is the one that
   must create a fresh keybag under an emulator UID key. So this stage is its
   own multi-device arc -- a data volume, the AES engine, and the
   SPI/NOR/effaceable chain -- the same shape as the iPhone OS 3 audio and
   graphics bring-up, and not verifiable without on-device testing.

   The effaceable lockbox chain is now traced to a structural blocker. The
   IOKit provider ladder is spi0 (`spi-1,samsung`) -> `AppleSamsungSPI` ->
   `AppleARMNORFlashDevice` -> `AppleEffaceableNOR` (`IONameMatch
   effaceable,nor`) -> `AppleEffaceableStorage`. A SPI NOR flash is a command
   state machine (RDID, READ, RDSR, WREN, PP, SE...) that is framed by the
   chip-select edge: the machine of one command ends when CS deasserts. On
   this board the SPI chip selects are GPIO platform functions
   (`function-spi_cs0`), not the controller's internal CS, and the core's SPI
   model says so outright -- its `s5l_spi_slave_t` has no chip-select
   callback because the controller cannot observe a GPIO select edge. So a
   correct SPI NOR cannot even be framed until the GPIO block is modelled;
   the order is GPIO chip-selects, then the SPI NOR + effaceable lockbox,
   then the AES engine, then the keybag-creation semantics, and only the
   last of those is checkable without hardware.

   **Step 1 done: the GPIO pad controller (2026-09-26).** `/arm-io/gpio`
   (`gpio,s5l8920x`, 0x83000000) is now a faithful register file in `n88`
   (core/include/n88.h), the single busiest unmodelled block in a boot (743
   config accesses): the driver reads a pin's register, ORs in its config
   and writes it back, and the old always-zero stub made every read-back
   lose the pin's prior bits. A write is stored and read back verbatim,
   reset zero; nothing is fabricated (an input pin with nothing wired reads
   its own last value, the honest "undriven" answer). The boot still reaches
   launchd and the keybag reboot -- no regression -- and GPIO no longer
   appears in the unmodelled census. Still to wire: the specific chip-select
   pins to the SPI NOR's command framing, which is the next step of the
   chain. kb_load's worker (keybagd
   0x3a48) is a 30 s event-wait for the kernel to publish a keybag, so the
   gate is entirely kernel-side in that ladder, not in keybagd's own file I/O.

   **Step 2: the clock tree, and spi0 comes alive (2026-09-26).** spi0 was
   idle for a reason other than GPIO or the PMGR registers. Traced
   instruction by instruction, `AppleSamsungSPIController::start` runs and
   returns false at its first real step: it asks its provider for the
   frequency of its `pclk` clock and gets 0. `AppleS5L8920XIO` answers that
   from `/arm-io:clock-frequencies`, one word per PMGR clock (a device's
   `clock-ids` entry 0x100 + n selects word n; spi0-2 and uart0-4 take word
   4), and the image carries it as zeros because iBoot fills it. iBoot
   computes it from the PLLs and 25 clock dividers that LLB programs (read
   from this firmware's own LLB and iBoot; the derivation is in
   `core/include/n88.h` and `n88_clock_frequencies`), so `n88` now writes
   those 28 words, plus `/arm-io:usbphy-frequency` and
   `/arm-io/audio-complex:ncoref-frequency` from the same iBoot pass. The
   derivation reproduces every cpu0 frequency that was already known or
   guessed (CPU 600 MHz, bus 100, memory 200, fixed and timebase 24) except
   one: the provisional 50 MHz peripheral clock is really 100 MHz, and is
   corrected. The display's pixel clock is LLB's default; iBoot's display
   driver retunes it for the panel, which only matters once there is a
   display. Result on 10B500: both SPI controllers start
   (`_spiVersion = 1`) and program spi0's registers, the audio complex
   reports `ncoRef: 162000000` instead of 0, `AppleSamsungLP65USBPhy::start
   : failed` is gone, and the boot still reaches launchd and the keybag
   reboot at the same speed. The PMGR suspicion was wrong and is dropped:
   a caller census shows every PMGR register the kernel touches is either
   written once at init or rewritten by the CPU performance-state routine,
   and none is polled, so no status wait was failing. Next in the chain:
   spi0 so far sees only its configuration writes, no transfers, so the NOR
   under it has not probed yet.

   **Step 3: spi0, the NOR flash, and effaceable storage starts
   (2026-09-26).** The flash driver (`AppleARMSPIFlashController`) queued a
   read-status command and slept waiting for spi0's interrupt. Three pieces,
   each read from the 10B500 drivers' own code:
   - The core's Samsung SPI model gained `spi-version 1` (commit 0fefcc7):
     five-bit FIFO levels at STATUS [10:6]/[15:11], depth 16, event mask
     0x0040000F, SETUP base 0x4000, the 0x4c count register. The interrupt
     rule is unchanged, and the S5L8900 machine and its snapshots are
     untouched.
   - `core/src/soc/spi_nor.c`: a serial NOR flash implementing exactly the
     commands the driver sends (ID, status, write enable/disable, write
     status, read, page program, 4 KiB erase), framed by chip select, with
     the datasheet rules for when each takes effect. It presents ST's M25PE80
     (JEDEC 20 80 14), a 1 MiB part from the driver's own table; which
     vendor's part a given phone has is not known here, so this is a choice
     among supported parts. Time is not modelled (busy never reads 1).
   - `n88` wires spi0 at 0x82000000 on VIC0 line 29, with the flash at its
     only select. The select is GPIO pin 0x1204, register 0x250 in the pad
     block: the driver writes 0x12 before a transfer and 0x13 after, so bit 0
     is the level and the select is active low. The flash starts erased and
     keeps its contents across reboots of the machine.
   Result on 10B500: the driver identifies the part,
   `AppleDiagnosticDataAccess started with AppleARMNORFlashDevice`, and
   **AppleEffaceableStorage starts**: it finds the blank lockbox ("unable to
   find content"), formats it (two 4 KiB erases and 32 page programs, the
   two lockbox copies at 0xfa000/0xfb000), and reports `[effaceable:INIT]
   started`. 191 flash commands, none refused or unknown. Both CPU engines
   give identical device counters and console text at 300M instructions.
   The keybag still fails, as expected: what remains is the AES engine
   (`IOAESAccelerator`) and a writable data volume at `/private/var`.

   **Why kb_load fails, exactly (2026-09-26).** Traced at the AppleKeyStore
   user client (10B500; its __TEXT is 0x80afa000-0x80b04000, dispatch at
   0x80afce4c): keybagd calls selector 0 (OK), 17 (blocks about 4.7 s, OK),
   0 (OK), then 14, which looks up the keybag with handle -1 (the system
   bag, 0x80afd67e) and returns kIOReturnNotFound; keybagd then prints
   `FATAL KEYBAG ERROR: kb_load`. It never calls a create selector. Its own
   code confirms it: it reads `systembag` from `/private/var//keybags`
   (0x3102-0x3106) and goes straight to "Can't load the keybag. tears in
   rain... Time to die" when that fails (0x310e); creating a system keybag
   is MobileKeyBag's `MKBKeyBagCreateSystem`, which keybagd does not call.
   On a phone the system keybag is written when the data partition is set
   up -- a restore, or Erase All Content and Settings, whose on-device tool
   (`mobile_obliterator`, launchd job `com.apple.mobile.obliteration`) is on
   this root filesystem. (The restore ramdisk's daemon is stored compressed
   and was not read.) So modelling the AES engine is necessary -- loading a
   keybag unwraps its class keys with the device's UID key -- but not
   sufficient: this machine also needs a data volume and a system keybag
   provisioned the way a restore or erase would, created by this machine's
   own kernel against its own (stand-in) UID and its effaceable lockbox.
   The only AES operation the boot makes today is one early CDMA request
   (0x87800000 and channels 1-2 at 0x87001000/0x87002000) that nothing
   completes.

   **The display, first slice: iBoot's framebuffer (2026-09-26).** The top of
   DRAM is now laid out as this firmware's iBoot leaves it: `/pram` is the
   last 16 KiB (0x4fffc000), and `/vram` is the three page-rounded 320x480
   buffers below it (0x4fe3a000-0x4fffbfff) that iBoot reserves for its
   panel entry "n88" (320x480, 163 ppi, 10.8 MHz pixel clock -- the same
   pixel clock n88's clock table already carries), in its default RGB888
   colour space laid out as 32 bits per pixel. Boot_Video describes the
   first buffer and `memSize` stops below the pool. `v_display` is 0 (text
   mode), so the kernel's own console paints the boot log onto it; with
   `serial=3` on the command line the log goes to the UART instead and the
   screen shows only the cursor, which is why the harness keeps it and the
   app can choose. `boot3gs -F screen.ppm` writes the framebuffer at the end
   of a run; with `-c "rd=md0 debug=0x8 -v"` on 10B500 it shows the verbose
   log through the root mount, launchd and the FIPS self-test. The display
   controller itself (`clcd,s5l8920x` at 0x85400000, whose window 0 iBoot
   programs at +0x20..+0x34) is not modelled yet: that is what AppleM2CLCD
   and, later, SpringBoard's surfaces will need. In the app, the 3GS preview
   now boots with `debug=0x8 -v` and presents this framebuffer on the phone's
   screen (VMN88Engine publishes a copy about 30 times a second from the
   machine's own thread), instead of overlaying console text.

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

### Speed of the 3GS path vs the real device (#37)

Measured, not estimated. All host figures are on the Linux x86-64 dev
container; the on-phone ceiling is separate and cited below.

- **The iOS 6 boot, host.** `boot3gs -e` retires the first 300M
  instructions of the 10B500 boot at ~122 M guest instr/s, 99.9% through
  the cached interpreter. `boot3gs` now prints the engine's own accounting
  (`arm_ci_describe_stats`), so the levers are visible: 8.6% of the boot
  drops to the slower reference path, dominated by the catch-all "other"
  class (14.1M ops), memory corners (6.8M), PC-writing forms (2.0M), media
  (1.5M) and block transfers (1.2M). Bringing those classes onto fast paths
  is the concrete lever for this workload.
- **CPU-bound host throughput** (`cpubench --backend cached`) ranges by
  workload: Thumb user 85–305 M instr/s, ARM user 57–223, with MMU-heavy
  (57–85) and syscall-heavy (81) code the floors and straight-line ALU the
  ceiling.
- **The real device, for reference.** The guest is an in-order dual-issue
  600 MHz Cortex-A8; sustained real throughput is on the order of a few
  hundred million instr/s and varies with IPC, so on this dev host the
  emulator runs CPU-bound guest code within a small factor of the real
  chip's rate. That is NOT the on-phone story: the product target is a
  modern iPhone running the same no-JIT core, where a firmware-backed replay
  measured ~7 M instr/s on an A9 (docs/device-benchmark.md). The iOS 6 path
  has not been separately timed on a phone, but it shares that core, so its
  on-phone rate is the same order — well below the guest's real-time rate,
  which is why closing the gap needs the reference-path work above and,
  ultimately, the techniques iCube uses (docs/ICUBE_DOLPHIN_RESEARCH.md).
  This project keeps the no-JIT constraint, so the cached interpreter is the
  ceiling here.

### Why display and touch stop at the framebuffer (#44)

The scanout controller (AppleM2CLCD) and the touch controller
(`multi-touch,n88` on spi1, the Z2 the iPhone OS 3 machine already models)
are driven by SpringBoard and the window server, which never start: the
system reboots at the keybag (kb_load) first. During the boot the only
display-side hardware touched is the M2 scaler at 0x85500000, and it
no-ops harmlessly -- the driver reads HW version 0, warns, and continues.
So a CLCD or Z2 model added now could not be exercised or validated on a
booting system. The verifiable slice of #44 -- iBoot's framebuffer with
the kernel's own boot log on it, and the spi-version 1 controller the
touch device would sit on -- is done; the rest is gated behind the keybag,
which this session stopped short of (see above).

### iPhone OS 3.1.3 on the same machine

iPhone OS 3.1.3 (7E18) for the 3GS predates data protection (iOS 4), so it
has no keybag to stop at; it is the way past the point where iOS 6
reboots, on the same n88 hardware model. Four differences from iOS 6 had
to be handled, each measured on the 7E18 kernelcache and device tree:

- **Virtual base.** The 3.1.3 kernel is linked at 0xC0000000, iOS 6's at
  0x80000000; both load at physical 0x40000000. n88 now takes the base
  from the kernel's lowest segment, rounded down to 256 MiB (the DRAM
  size), so a kernel linked high inside that window is still refused.
- **memSize must be whole MiB.** 3.1.3's start code (0xc00670c8) maps
  memory a MiB at a time and stops only when memSize reaches exactly zero;
  the old memSize (DRAM less the 0x1C6000 framebuffer + PRAM top) never
  did, and the loop walked off the end of DRAM forever. memSize is now
  rounded down to 0x0FE00000; iOS 6 follows the same path with the same
  timings (root at 60.27 s, keybagd stopped at 68.77 s).
- **No `/chosen/nvram-proxy-data`.** 3.1.3's tree has no such property
  (its kernel reads NVRAM from the NOR), so a tree without it is passed
  over and only a short one refused.
- **Epoch 4.** pe_identify_machine (0xc01a292e) panics unless the
  boot_args version is 4 (iOS 6: 5; the S5L8900 iPhone OS 3: 6). It is a
  request field (`n88_boot_t.boot_args_version`), `boot3gs -B 4`.

Also fixed on the way: the IMG3 decrypt left the last partial AES block of
a payload as ciphertext. The 3.1.3 device tree is 42840 bytes in a tag
padded to 42848, so its last 8 bytes (the final `AAPL,phandle`) came out
as garbage and the tree did not parse; a tail whose block the tag's
padding completes is now decrypted, as iBoot does.

Result (`boot3gs kernelcache.macho devicetree.bin -e -B 4`): the kernel
reaches IOKit, starts the platform drivers (VIC, GPIO, performance
controller, SPI v1 x2, I2C, PWM, USB PHY, MIPI DSI, I2S x3, CS42L61 audio,
UART, camera, NOR image access), and waits for its root filesystem
("Still waiting for root device") at 60 s guest time, about 13 s of host
time. Next is a root filesystem for it: the iOS 6 memory-disk bridge
patches the 10B500 kernel at fixed sites, and 7E18 needs its own.

#### Root filesystem to SpringBoard (#48)

`boot3gs kernelcache.macho devicetree.bin -e -P rootfs.hfs -r work.hfs` now
takes 7E18 from the root mount to a running user space. Four things were
needed, in the order the boot met them:

- **The memory-disk bridge for 7E18** (`tools/ios3_n88_kernel_patch.c`):
  the same SVC #0xe1/#0xe2 bridge as 10B500, at mdevstrategy's two
  bcopy_phys calls (0xc007238e read, 0xc0072442 write), plus the
  "physical" flag (0xc019c6d0). The sites are gated on the kernel's
  LC_UUID and the expected bytes; boot3gs picks the 7E18 patch and epoch
  4 from the kernel itself. Result: `BSD root: md0` at 60.15 s.
- **The SHA-1 engine** (`core/src/soc/s5l_sha1.c`, /arm-io/sha1 at
  0x80100000). Every executable page is hashed by cs_validate_page, and
  with IOCryptoAcceleratorFamily present SHA1Init/SHA1Update run on this
  engine, fed only by CDMA channel 4 as a peripheral request (CSR 0x18,
  FIFO +0xA0). Before it, launchd's first page never validated and its
  exec slept forever. The engine's protocol (start bit 1, continue bit 3,
  state byte-reversed at +0x20, the driver's own padding) is read from
  AppleS5L8920XSHA1 and checked against the FIPS 180 vectors.
- **CDMA peripheral requests and CAR progress** (cdma.h). A channel
  without the memory-to-memory bit now sends its chain to the device at
  +0x8, and when a request ends CAR is left past the descriptors consumed.
  3.1.3's AppleCDMA keeps a 128-descriptor ring and completes requests
  from CAR, so with CAR left at the start the first hash "finished" and
  was never delivered. iOS 6 is unaffected (same device-access counts,
  same milestones: root 60.27 s, reboot 92.78 s).
- **The fstab and the Thumb-2 `STRB.W r0, [r4, sp]`.** `-P` provisions
  the work image as the S5L8900 machine does (fstab to /dev/md0, the
  volume grown), so launchctl's fsck passes and / is remounted
  read-write. Then a library (lowercasing into a stack buffer) runs
  0xf804 0x000d: a register offset of SP, UNPREDICTABLE in the ARM ARM
  but executed as written by the Cortex-A8 and emitted by Apple's
  compiler. The interpreter now uses SP's value there (PC is still
  refused).

Result at 74 s of guest time (6000M instructions, 42 s of host time):
21 processes: launchd, launchctl, syslogd, ptpd, lockdownd, mediaserverd,
mDNSResponder, itunesstored, IQAgent, fairplayd, configd, accessoryd,
**SpringBoard**, misd, CommCenter, notifyd, ReportCrash, installd,
mDNSResponderHelper and securityd. 4,404 code pages are SHA-1 validated
on the engine. At 90 s the same SpringBoard (pid 18) is still running.

At this point (fixed in #55, below) the screen stayed black because no
framebuffer was ever published.
AppleM2DisplayDrivers' start has slept since 0.4 s in waitForService: the
tree's /arm-io/clcd takes `function-lcd_enable` from
/arm-io/mipi-dsim/lcd (the "pinot" panel), which takes `function-lcd_ldo`
from /arm-io/i2c0/pmu (the Dialog D1755), and neither the S5L8920 I2C
controllers, the D1755 nor the MIPI-DSI controller is modelled. The one
crash report shows it from the other side: DataMigrator (run once after a
restore) draws its progress bar through CoreSurface, gets a NULL base
address, and faults at 0xffffff70. Next, in order: I2C0 and enough of the
D1755 for its driver to start and provide the LDO, the DSIM controller
for the panel driver, then the display driver publishes the framebuffer
that m2clcd.c already scans out. A camera client also retries the ISP
(no firmware, no mailbox) every 2 s, and the clock reads 1969 (no RTC,
which is also the PMU).

#### The display chain: I2C, the PMU, the panel id and the DART (#55)

The chain turned out to need four pieces, and no DSIM model yet:

- **The I2C controllers** (`core/src/soc/s5l8920_i2c.c`, i2c0 at
  0x83200000 line 0x13, i2c2 at 0x83400000 line 0x11). Not the S5L8900's
  Samsung controller: AppleS5L8920XI2CController runs a whole transfer
  from a few registers (address +0x0, first byte +0x10, count +0x18, FIFO
  +0x20, start +0x24 = 4 | write; status +0xC, bit 4 done, bit 5 no
  acknowledge). The boot's first transfer was the accelerometer's
  WHO_AM_I (slave 0x1D, register 0x0F), and every later one, the PMU's
  included, queued behind it.
- **The devices**: the LIS331DL answers WHO_AM_I = 0x3B; the D1755 is a
  register file that acknowledges and keeps what is written. That is
  enough for AppleD1755PMU to start (it reads its bucks as 725 mV and
  programs its LDOs), and with it the backlight, the multitouch loader,
  USB and, most of all, the RTC users: **the root mount moves from 60 s
  to 0.42 s** (the 60 s was "RTC did not show up"), and iOS 6's from
  60.27 s to 0.52 s. iOS 6 then takes its usual path (the keybag fails at
  3.9 s, launchd stops every job), and its restart now arrives through
  the PMU ("pmu restart", 10.3 s) where it came at 92.8 s before; the PMU
  model does not yet reset the machine.
- **The panel id.** ApplePinotLCD refuses to start on the tree's
  placeholder `lcd-panel-id` 0, which iBoot fills after reading the panel
  over MIPI-DSI. n88_boot now writes a made-up one (`N88_LCD_PANEL_ID`,
  "NEON"); nothing but Pinot reads it, and Pinot only logs it. With the
  panel service up, the display driver's lcd_enable wait ends.
- **The DART** (`core/src/soc/s5l8920_dart.c`). The display driver then
  takes the controller over and points window A at 0x3C0D8000, an I/O
  address behind dart0 (the clcd's `iommu-parent`). AppleH2PDART loads 16
  first-level slots through +0x8 (bits 11:8 the slot, bits 27:12 a
  second-level table's DRAM offset) and 4 KiB tables of
  (DRAM offset | 1) entries. `n88_framebuffer()` now gathers the scanout
  through dart0 a page at a time.

With those four the screen shows what the guest draws: the Apple logo on
black (boot3gs -F, 3,322 lit pixels) from about 0.5 s of guest time, and
DataMigrator no longer crashes (it gets a real framebuffer). SpringBoard,
running, still drew nothing; the rest was finding what it waited for.

**Devices worse declared and silent than absent.** As on the S5L8900
machine (app/Sources/VMBootOptions.c), a node whose hardware is not
modelled can hang or panic the boot, where an absent one just takes its
driver's no-hardware path. Each of these was found as the cause of a
stall, in this order, by walking the threads of the stalled process in a
DRAM dump (the task and thread layouts are the kernel's own panic
printer's: a task's threads at +0x28, a thread's user state at +0x324,
task+0x14 -> map, +0x24 -> pmap, +0x4 -> its table):

- `/baseband` and `/arm-io/spi2`: with a modem declared and silent,
  lockdownd spins in pthread_once behind CoreTelephony's first call (the
  3G machine's CommCenter wall, docs/ROADMAP.md).
- `/arm-io/usb-otg`: AppleSynopsysOTGDevice::findMaxEndpoints panics
  on unmodelled configuration registers once lockdownd brings USB up.
- `/arm-io/isp`: the camera client polls the ISP's mailbox forever.
- `/arm-io/spi1/multi-touch`: the touch controller is not modelled.
- `/arm-io/tv-out`: SpringBoard's main thread, in UIApplicationMain ->
  CAWindowServer _detectDisplays -> M2TVOutDisplay::open, releases the TV
  out framebuffer, and IOServiceClose sleeps in IOMobileGraphicsFamily
  waiting for a swap the TV out never completes (iOS 6 logs the same:
  "AppleM2TVOut, client ... going to wait on swap").
- `/arm-io/sgx` and `/arm-io/amc`, as on the 3G: QuartzCore draws in
  software, and audio decodes in software.

Un-matching had to strike every string of a node's compatible: S5L8920
nodes also list their older relatives ("usb-otg,s5l8920x",
"usb-otg,s5l8720x", ...), and a driver matching a later one still started.
boot3gs applies this list by default to the 7E18 kernel (`ios3_unmatch`).

Two more device details then mattered: the LIS331DL's CTRL_REG2 BOOT bit
clears itself (AppleLIS302DL panics if it reads it set 500 ms later), and
**the MIPI-DSI master** (`core/src/soc/s5l8920_dsim.c`), which
AppleS5L8900XMIPIDSIController polls without timeouts: STATUS bit 10
follows CLKCTRL bit 31 (the HS clock), bit 20 reads the software reset
released, and the ULPS bits (7:4, 9) follow ESCMODE 0x8A in and 0x8F/0x80
out. Before it, one of those loops took 730 million reads.

**Result: iPhone OS 3.1.3's own activation screen**, drawn by SpringBoard
on the emulated 3GS: "Searching...", the iTunes and cable art, and "slide
for emergency" (boot3gs -F, 33,317 lit pixels, from about 11 s of guest
time), with SpringBoard's background apps (MobilePhone, MobileMail,
MobileMusicPlayer, voiced) running behind it. It is the screen of a phone
that has not been activated and has no SIM.

**Activation** is the S5L8900 machine's: boot3gs's -P now also writes the
lockdown activation record into the work copy for the 7E18 kernel
(`rootfs_work_activation_entries`, the plan bootkernel's --activate
writes, derived from lockdownd in docs/derivations.md 23.3; -a leaves it
out). With it SpringBoard shows the **lock screen**: the clock, the Earth
wallpaper and "slide to unlock" (92,133 lit pixels). The clock reads
4:00, Wednesday December 31 -- 1969, the D1755's RTC registers being
zero.

#### Touch, and the home screen (#56)

The 3GS's touch controller (an N1, `multi-touch,n88` on spi1) is driven by
the same kext as the 3G's Z2, AppleMultitouchSPI, through the same HBPP
bootloader and report protocol; only its calibration registers
(0x1000300C, 0x1000305C = 0x20, 0x10003058 = 6, 0x10003000 = 3) and
version register (0x10003800) differ, and the 3G's `mtz2.c` serves it
unchanged. What was new was the board around it:

- **spi1** at 0x82100000 (line 0x1C), version 1, with the device at its
  only select.
- **The pins**, decoded like spi0's select: select 0x1300 (pad 0x260),
  reset 0x1401 (pad 0x284). The pad mode matters: bits 3:1 = 001 drive
  bit 0 out, 000 make the pin an input the board pulls high, and the
  driver releases the reset that way (0x12, then 0x10). Read as "bit 0 is
  the level" it stayed in reset and the driver reported "Could not detect
  HBPP". (The full mode table is in the next section.)
- **The firmware arrives by CDMA** (channel 18 into spi1's TXDATA), and the
  driver starts the channel *before* it sets the port's DMA bit (SETUP
  0x4018 -> 0x4058); on hardware the channel waits on the port's request.
  With the transfer done at once, 53,892 octets were dropped at the FIFO.
  A peripheral request now waits until its device takes it, and n88
  offers it again when spi1's SETUP changes (`cdma_retry`).
- **The GPIO interrupt controller**: status at pad block +0x800 + 4g,
  write one to clear (AppleS5L8920XGPIOIC's handler, 0xc0673b94); a pad's
  bit 4 masks its pin (the driver enables the attention line, interrupt
  0xB4, with pad 0x2D0 = 0x20A); one VIC line, 0x5E.

With them the driver detects HBPP, downloads 128 bytes of prox
calibration, 256 of panel calibration and the 53,924-byte firmware
`0x0066.bin` "in 106ms", runs the N1 calibration and execute, and
interrogates the running part (0xEE wake, 0xE2, 0xE3, 0xE6, 0xE7).

`boot3gs -D x0,y0,x1,y1,t` drags one finger (24 reports, 16 ms apart in
guest time), and `-S t` takes a screenshot mid-run. A drag at 40 s found
the touch already off ("disabled power" at 19.7 s: the idle lock screen's
display timed out, and the touch with it). At 13 s, from (50,431) to
(310,431), **the slider unlocks the phone**: by 15 s the screen is iPhone
OS 3.1.3's **home screen** (Messages, Calendar, Photos, Camera, ...,
Settings, iTunes, App Store, Compass; the dock's Phone, Mail, Safari and
iPod) under the first-run "Edit Home Screen" tip, with a "ringer" HUD
(the volume buttons, it turned out; see below) and "No Service" (no
modem). 111,269 lit pixels against the lock screen's 92,133.

#### Buttons, the ringer switch, the clock, sleep and wake (#57)

**The pins.** /buttons gives each button a GPIO function, <gpio 'GPIO'
pin flags>, and an interrupt on its pad index: hold 0x1607 (pad 0x2DC,
0xB7), menu 0x1606 (0x2D8, 0xB6), volume up 0x1600 (0x2C0, 0xB0), volume
down 0x1601 (0x2C4, 0xB1), the ringer switch 0x1403 (0x28C, 0xA3).
AppleS5L8920XGPIO's pin read (0xc06736f4) configures the pin as an input
and returns bit 0 of its pad; the function inverts it unless flags bit 8 is
set (0xc06737a8), so hold and menu are high when pressed and the volume
buttons low, and the switch is low at silent (the driver inverts the
ringer once more before reporting it). A pad's bits 3:1 are its mode, read
from the configure (0xc0674164) and interrupt setup (0xc0673ff0): 000
input, 001 and 111 output, 010/011 interrupt while high/low, 100/101 on a
rising/falling edge, 110 on both; AppleM68Buttons asks for both edges
(0x20C), and on any of its interrupts reads every button (0xc068a1a4) and
reports the changes as HID consumer usages 0x30 (power), 0x40 (menu),
0xE9/0xEA (volume) and telephony usage 0x2E (the switch).

**The "ringer" HUD was the volume buttons.** With every pad reading back
its own register, bit 0 clear, the first poll found both volume buttons
held, and SpringBoard showed its ringer-volume display over the home
screen. With the pins wired (`n88_set_input`), it is gone, and
`boot3gs -K name,t[,s]` presses a button or moves the switch: volume up
shows the "ringer" volume HUD one step up, "silent" the crossed bell,
"ring" the bell, each as on a phone.

**The clock.** AppleD1755PMU reads the time as a 32-bit seconds count at
PMU registers 0x4C..0x4F, twice until both agree (0xc038aacc); setting the
time stores the difference from the count at 0x64..0x67 (0xc038ab00). The
count read 0, the Unix epoch, which the lock screen showed as 4:00 PM,
Wednesday 31 December 1969 (Pacific time). It is now a count running with
guest time from what the host sets (`n88_set_rtc`; boot3gs uses the host's
clock, or `-R seconds`): the status bar, the lock screen and Calendar's
icon and day view show the day and time.

**Tapping into an app.** `boot3gs -T x,y,t` taps (four reports at one
point). A tap on the tip's Dismiss button (160,330) clears it, a tap on
Calendar (117,70) opens **Calendar** on its day view, "Friday, Oct 2 2026",
and one on Clock (197,246) opens its **World Clock** (Cupertino, the time,
"Today").

**Sleep and wake.** A minute after the last touch the display goes off,
and 15 seconds later IOPMrootDomain sleeps the system: cpu_sleep
(0xc00606a4) writes start_cpu's physical address into the exception-vector
page at the bottom of DRAM (+0x24; the reset vector there jumps through it
with r0 = +0x28) and the octets "XSOMPSUS" at +0x80, cleans the caches and
parks the CPU on a branch to itself with interrupts masked (ml_arm_sleep,
0xc00603cc) for the PMU to cut the power. n88 now treats a CPU parked that
way as asleep (no instructions, time passing), and hold or menu does what
the PMU and the boot loader do: latch the reason in PMU register 0x01 (bit
0 menu, bit 1 hold, which AppleD1755PMU reads into its cache on wake and
AppleM68Buttons reports through function-wake_button_*), and, with the mark
present, reset the CPU alone into the vector page (time kept, devices
retained). The kernel logs "pmu wake events: menu", "System Wake", the lock
screen comes back with the right time, and a drag unlocks it again, back
into the app that was open (Clock, its time moved on). The
touch controller's power is PMU register 0x11 bit 6 (the driver turns it
off with the display and at sleep, on at wake); wired to the touch model,
the flashless part loses its firmware at sleep and the driver bootloads it
again after wake, as on hardware.

Not yet: the PMU's own interrupt and events, and its alarm (nothing but a
button wakes the system); the ambient light sensor (i2c2 0x49) still does
not answer.

#### In the app (#58)

The app's iPhone 3GS machine (VMN88Engine) now boots iPhone OS 3.1.3 the
way boot3gs does, from one shared recipe (`tools/n88_ios3.h`: the kernel
check, the working root filesystem, the boot request with its un-match
list, the memory-disk patch), so the phone runs what was measured here.

- **Import.** An iPhone2,1 3.1.3 (7E18) IPSW, with the user's kernelcache,
  device tree and root filesystem keys, now gives `rootfs.img` as well
  (the 3G's kind of disk image; the importer used to leave every 3GS root
  filesystem in the archive, because iOS 6's is a format it does not
  read). The files go to the firmware-iphone3gs folder, unverified (there
  are no reference hashes for this product).
- **First start** makes the machine's own `rootfs-work.img` in its folder
  (activated, fstab to md0, grown by 256 MiB: about 814 MB), on the
  engine's thread, with the progress in the status line; later starts boot
  straight from it, and the guest's writes persist there.
- **Input.** Touches on the screen go to the touch controller through the
  same queue the 3G machines use; Home, Power, the volume buttons and the
  ring/silent switch (the phone shell, the key bar and the Controls menu)
  go to `n88_set_input`, a release held back until its press is at least
  0.1 s of guest time old. While the phone sleeps the screen is black, the
  status line says "asleep", and Home or Power wakes it.
- **Clock.** The guest's RTC starts at the phone's time.

An iOS 6 kernel in the folder still boots the preview, as before.

#### Sound (#60)

The first app build on a phone booted and ran but was silent. Three things
stood between iPhone OS 3.1.3 and the speaker, found in that order with
boot3gs's `-X` (who called this) and `-C` (which blocks ran) and the kernel's
own code:

1. **The codec's driver never finished starting.** AppleCS42L61Audio
   waits for the "mikey" platform function, which AppleCD3272Mikey
   publishes only once the CD3272 headset controller answers on i2c0
   (0x39). Mikey and the CS42L61 (0x4A) are now register files on i2c0.
2. **The sample rate came out as 0 Hz.** A clock-change message
   (0xE3FF8001, from AppleS5L8920X) makes the audio driver ask the codec's
   "mclk_frequency" function for the master clock, which the audio complex
   reports by reading its NCO back (+0x18, +0x1C at 0x84300000; see
   n88.h, "Sound"), and divide it by the 64 bits of a frame. Unmodelled, the
   NCO read 0, the rate was 0, and the codec refused it (0xE00002C2,
   AppleEmbeddedAudio+0x3cc0). The audio complex and the I2S ports are now
   stored and read back, and the NCO also sets the rate the model plays at.
3. **CDMA could not play a sound.** iPhone OS feeds i2s0 through CDMA
   channel 21, a peripheral request with the port's address as its FIFO.
   The engine completed every peripheral request at once, so a sound's
   whole chain vanished in an instant; worse, AppleCDMA stops a channel
   with CSR bit 2 and waits for bit 21, which nothing set, so the first
   sound's stop hung its thread in a loop. CDMA now has *paced* channels
   (cdma.h): a channel whose FIFO is an I2S port runs through its chain
   only as the port plays it, one 4-octet frame (16-bit left and right;
   the channel's transfer size is 2 octets) per period of the sample rate,
   raising bit 20 past each descriptor marked 0x300 (the end of a queued
   command) and stopping, done, at the terminator; it answers the pause
   (bit 5) the driver uses to read the play position (+0x10) and the stop
   (bit 2, acknowledged in bit 21). The next frame boundary that raises a
   line is a time event like the decrementer, so a waiting core wakes for
   it and the interrupt arrives when the sound has played, not before.

Measured with boot3gs `-A` (i2s0 to a WAV file): after a slide to unlock
the guest plays 20,939 frames that correlate 0.94 with the root
filesystem's own `unlock.caf` (20,800 frames, 44.1 kHz) at zero lag and a
gain of 0.92 (the system volume), and the lock button gives 17,556 frames
correlating 0.95 with `lock.caf` (17,600 frames). The difference is the
system's output processing, not the path: the format, the rate and the
order of the samples are the file's. Unlike the 3G, no move of the ring
switch is needed first; with the switch at silent the unlock is silent, as
on a phone.

In the app, VMN88Engine hands i2s0's frames to the same VMAudioOutput the
3G machines use (44.1 kHz 16-bit stereo; another rate is matched by
repeating or dropping frames), paused with the machine. When the host is
slower than the guest needs, the speaker runs dry and fills with silence;
nothing waits for it.

Not yet: the microphone (a peripheral-to-memory request on these
channels), the headset (Mikey reads as nothing plugged in), the voice and
baseband ports (i2s1 and i2s2 play their frames nowhere), and the codec's
own registers have no effect (the volume the guest sets is already in its
samples).

### The CDMA engine and AES with a stand-in hardware key (#46)

This was the step the keybag work stopped short of. It turns out not to
involve any real device's secret: the emulated phone gets its own fixed
"fused" key (`CDMA_STANDIN_KEY`), the way every real phone has its own.
Whatever the guest wraps with it is unwrapped only by this same engine, and
nothing made on a real phone unwraps here. (Podium, an iPod touch 4 / iOS
6.1.6 emulator, takes the same approach on the A4's identical block.)

- `core/src/soc/cdma.c` models /arm-io/cdma's memory-to-memory path
  (0x87000000, channels 1..27, line 0x2a + n) and the eight AES contexts
  (0x87800000): UID/GID requests use the stand-in key, register keys are
  honoured, 128/192/256-bit, CBC or ECB. The register protocol and
  descriptor format are AppleCDMA's (cdma.h cites the addresses); the
  boot's own request (context 0x30100, descriptors 0x30103/0x103) is a test.
- The VICs are daisy-chained: AppleARMPL192VIC reads VIC0's VECTADDR for
  every interrupt, so a VIC with nothing pending now passes the next one's
  vector through. Before this, the first VIC1 interrupt this machine ever
  raised (CDMA channel 1, line 43) dispatched as source 0, was never
  acknowledged, and stormed.
- Result: by 63 s of guest time, 6 transfers / 6 AES operations (5 under
  the hardware key), 0 errors, and the boot otherwise follows the same path
  as before. kb_load still fails, because no system keybag file exists:
  on a real phone a restore creates it (#47).

### The display controller (AppleM2CLCD)

`core/src/soc/m2clcd.c` models /arm-io/clcd at 0x85400000 from the iOS 6
driver's own code (com.apple.driver.AppleM2DisplayDrivers) and openiBoot's
independent register map:

- At hand-off it shows what iBoot left: +0x4 bit 4 (window A on), window A
  = the boot framebuffer (0x4fe3a000, 320x480, stride 320 pixels, format 7 =
  32-bit ARGB). `create_default_fb_surface` (0x809961f8) adopts the boot
  framebuffer from exactly these registers; with zeros it would find none.
- `start_hardware` (0x80995652) and the interrupt (0x809947f4): +0x0 bit 0
  enable / bit 1 idle / bit 8 soft reset, +0x8 interrupt enable, +0xC status
  (write one to clear; bit 0 = frame, 0x1700 = underruns), 0xF to +0x1B2C.
- A frame starts every 1/60 s of guest time; with frame interrupts enabled it
  raises line 0x25 (VIC1 bit 5, through the daisy chain) and the run loop
  and WFI stop exactly there.
- `n88_framebuffer()` follows the window registers (a swap moves what the app
  and `boot3gs -F` show) while the geometry stays 320x480x32.

In the 10B500 boot the driver is constructed (60.3 s) but never touches the
controller before the keybag reboot, so the boot is unchanged; the swap path
is exercised only once a UI process runs.
