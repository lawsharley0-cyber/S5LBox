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
