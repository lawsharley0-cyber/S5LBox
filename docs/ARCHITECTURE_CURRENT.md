# Current S5LBox Architecture Audit

> **Superseded** by [`CURRENT_ARCHITECTURE.md`](CURRENT_ARCHITECTURE.md)
> (2026-09-23). Section 4 below lists bottlenecks without the measurements in
> `hotpath.md` that refute several of them; do not use it to plan work.

## 1. System Overview

S5LBox is a full-system emulator modeling the Samsung **S5L8900** System-on-Chip (SoC) and its core ARM processor, the **ARM1176JZF-S** (ARMv6 architecture with VFPv2 vector floating point). The emulator's primary objective is booting genuine Apple iPhone OS (versions 1.x through 3.x, targeting iPhone1,1 and iPhone1,2 / iPhone 3G) without reimplementing the guest operating system's higher-level APIs.

The core is implemented in portable C11 with zero external runtime dependencies, compiling and executing across Windows, Linux, macOS, and iOS.

---

## 2. Execution Path: From Mach-O Guest to Hardware Simulation

The complete execution path is traced below:

```
+-------------------------------------------------------------------+
|               Guest Mach-O Application / Kernel                    |
|   (e.g., iPhone OS 3.1.3 Mach-O kernelcache / Userland binaries)  |
+-------------------------------------------------------------------+
                                 |
                                 v
+-------------------------------------------------------------------+
|                    Guest Virtual Memory (VA)                      |
|  - Kernel space: 0xc0000000 - 0xffffffff (TTBR1, N=2)             |
|  - User space:   0x00000000 - 0xbfffffff (TTBR0)                  |
|  - Vectors:      0xffff0000 (SCTLR.V = 1)                         |
+-------------------------------------------------------------------+
                                 |
                                 v
+-------------------------------------------------------------------+
|            Instruction Fetch & Translation Fast-Path              |
|  1. Check 1 KB Fetch Block Cache (c->fetch_host, c->fetch_blk)    |
|  2. If Miss: Consult 4096-entry Software TLB (arm_mmu_translate)  |
|  3. If TLB Miss: Execute ARMv6 Page Table Walk (mmu_walk)          |
|     - L1 Section (1MB) / Supersection (16MB) / Coarse Table       |
|     - L2 Small Page (4KB) / Large Page (64KB)                     |
|     - AP / APX permissions + XN (Execute-Never) enforcement       |
|  4. Resolve Host Memory Pointer via bus->host_ram                 |
+-------------------------------------------------------------------+
                                 |
                                 v
+-------------------------------------------------------------------+
|                     ARM / Thumb Instruction Decode                |
|  - Thumb State (CPSR.T = 1): 16-bit fetch -> thumb_step()         |
|  - ARM State   (CPSR.T = 0): 32-bit fetch                         |
|  - Condition Code Check: arm_cond_passed(c, cond)                 |
|  - Bitfield pattern matching: ALU, Shift, Load/Store, Branch,     |
|    Multiply, Coprocessor CP15, VFP11 CP10/CP11, System/Exceptions |
+-------------------------------------------------------------------+
                                 |
                                 v
+-------------------------------------------------------------------+
|                 Instruction Execution (arm_step)                  |
|  - Register File Updates: r0-r15, CPSR, banked r13/r14/spsr       |
|  - Arithmetic / Logical operations with flag updates (N, Z, C, V) |
|  - Barrel Shifter: LSL, LSR, ASR, ROR, RRX                        |
|  - Exclusive Monitor: LDREX / STREX tracking                      |
|  - VFP11 Unit: Single & Double precision operations               |
+-------------------------------------------------------------------+
                                 |
                                 v
+-------------------------------------------------------------------+
|                     Memory Subsystem Dispatch                     |
|  - Data Reads: dread cache (64-entry) -> TLB -> host_ram / bus    |
|  - Data Writes: dwrite cache (64-entry) -> TLB -> bus->write*     |
|  - Address Routing:                                               |
|      * 0x08000000 - 0x10000000 : Main DRAM (128 MB)               |
|      * 0x20000000 - 0x20001000 : SecureROM                        |
|      * 0x22000000 - 0x22010000 : SRAM                             |
|      * 0x38000000 - 0x3ff00000 : S5L8900 MMIO Peripheral Bus       |
+-------------------------------------------------------------------+
                                 |
                                 v
+-------------------------------------------------------------------+
|                    Simulated Hardware / Peripherals               |
|  - VIC (Vectored Interrupt Controllers 0 & 1)                     |
|  - Timers: S5L8900 64-bit microsecond counter & interval timers   |
|  - CLCD: Color LCD Controller -> Host Framebuffer publication    |
|  - PowerVR MBX 2D/3D Graphic Core Registers                       |
|  - PCF50635 PMU via I2C Bus                                       |
|  - WM8991 Audio Codec & I2S interface                             |
|  - MultiTouch Z2 Controller (MTZ2) via SPI/I2C                    |
|  - UART0 (Console) & UART4 (PPP networking)                       |
|  - Storage: NOR Flash, NAND Controller, Host Memory-Disk Bridges  |
+-------------------------------------------------------------------+
```

---

## 3. Subsystem Breakdown

### 3.1 CPU Core & Decoder (`core/src/arm/arm_interp.c`)

- **CPU State (`arm_cpu_t`)**:
  - `r[16]`: 16 general-purpose registers (r15 is PC).
  - `cpsr`: Current Program Status Register (N, Z, C, V, Q, E, A, I, F, T, Mode).
  - `bank_r13[ARM_BANK_COUNT]`, `bank_r14[ARM_BANK_COUNT]`, `spsr[ARM_BANK_COUNT]`: Banked registers for USR/SYS, FIQ, IRQ, SVC, ABT, UND modes.
  - `fiq_r8_12[5]`, `usr_r8_12[5]`: FIQ-specific banked high registers.
  - `cycles`: Instruction retirement tick counter.
  - `excl_valid`, `excl_addr`: Single-core exclusive monitor for `LDREX`/`STREX`.
- **Interpreter Loop (`arm_step`)**:
  - Step 1: Validates CPSR mode (`arm_mode_is_valid`).
  - Step 2: Checks interrupt assertions (`fiq_line`, `irq_line` against CPSR F/I masks). If asserted, branches to high vector `take_exception` and sets PC to vector base (`0xffff001c` / `0xffff0018`).
  - Step 3: Instruction fetch via `c->fetch_host` or `arm_mmu_translate(ARM_ACCESS_FETCH)`.
  - Step 4: If `CPSR.T` is set, calls `thumb_step` for 16-bit instruction handling.
  - Step 5: If ARM mode, checks condition field `insn >> 28`. If `0xF`, decodes unconditional extensions (`PLD`, `CLREX`, `CPS`, `BLX`, `SRS`, `RFE`). Otherwise, calls `arm_cond_passed()`; if false, skips instruction.
  - Step 6: Cascading pattern checks for Data Processing, Single Data Transfer, Block Transfer, Multiplies, Coprocessor instructions, and Software Interrupts (`SWI`/`SVC`).
  - Step 7: Commits next PC and advances `cycles`.

### 3.2 Thumb Interpreter (`thumb_step` in `arm_interp.c`)

- Covers standard ARMv6 16-bit Thumb-1 encodings:
  - Shift by immediate (LSL, LSR, ASR).
  - Add/Subtract (register and immediate).
  - Move/Compare/Add/Subtract immediate.
  - ALU operations (AND, EOR, LSL, LSR, ASR, ADC, SBC, ROR, TST, NEG, CMP, CMN, ORR, MUL, BIC, MVN).
  - Hi-register operations and branch exchange (BX, BLX reg).
  - PC-relative load (`LDR Rd, [PC, #imm]`).
  - Load/Store with register/immediate offset (Word, Halfword, Byte).
  - SP-relative load/store.
  - Load address (`ADD Rd, PC/SP, #imm`).
  - Stack operations (`PUSH`, `POP` with PC/LR).
  - Multiple load/store (`LDMIA`, `STMIA`).
  - Conditional branch (`B<cond>`), Software Interrupt (`SWI`).
  - 32-bit `BL`/`BLX` prefix and suffix instructions.

### 3.3 Vector Floating Point: VFP11 (`core/src/arm/vfp.c`)

- Implements VFPv2 coprocessor CP10 and CP11:
  - Floating point register file: 32 single-precision registers `vfp_s[32]`, aliased as 16 double-precision registers `vfp_d[16]` (low word first).
  - Status registers: `vfp_fpexc` (with `FPEXC.EN` lazy trap support required by XNU) and `vfp_fpscr` (IEEE 754 condition flags, rounding modes, flush-to-zero).
  - Emulated arithmetic: VADD, VSUB, VMUL, VDIV, VMLA, VMLS, VNMUL, VNMLA, VNMLS, VABS, VNEG, VSQRT, VCMP, VCVT.
  - Load/store: VLDR, VSTR, VLDM, VSTM.

### 3.4 MMU & Translation (`core/src/mmu.c`)

- **Translation Scheme**: ARMv6 short-descriptor format.
  - `TTBCR.N`: Partition between `TTBR0` (user pmap) and `TTBR1` (kernel pmap).
  - First-level table: 4096 entries (16 KB) indexed by VA[31:20].
  - Second-level table: 256 entries (1 KB) indexed by VA[19:12].
- **Fast-Path Caches**:
  - `tlb[4096]`: Direct-mapped cache keyed on 1 KB subpage, tagged with `(va >> 10) << 3 | acc << 1 | priv`.
  - `fetch_host`: Host pointer to currently executing 1 KB block of RAM.
  - `dread[64]`: Data read cache for non-straddling 1 KB RAM blocks.
  - `dwrite[64]`: Data write cache for direct host memory writes (enabled only when frontend consents via `host_ram_write`).

### 3.5 System Bus & Peripheral Simulation (`core/src/soc/`)

- Memory map coordinates:
  - `0x08000000 - 0x10000000`: 128 MB Mobile DDR SDRAM.
  - `0x38000000`: DMA controller (PL080).
  - `0x38400000`: CLCD display controller.
  - `0x38800000`: UART0 (serial console).
  - `0x38804000`: UART4 (PPP network interface).
  - `0x38a00000`: I2C controller (interfaced with PCF50635 PMU).
  - `0x38b00000`: SPI controllers.
  - `0x38c00000`: PowerVR MBX 2D/3D graphics core.
  - `0x38e00000`, `0x38e01000`: Vectored Interrupt Controllers 0 and 1 (VIC).
  - `0x39000000`: Timers and watchdog.
  - `0x39300000`: I2S controller (interfaced with WM8991 audio codec).
  - `0x39400000`: GPIO controller & button inputs (Power, Home, Volume, Ringer).
  - `0x39c00000`: USB OTG controller.
  - `0x3a000000`: NAND controller.

### 3.6 Boot Chain & Kernel Handoff (`core/src/boot/bringup.c`)

- S5LBox implements a synthetic kernel handoff (`bootkernel` / `bringup.c`) that bypasses SecureROM and iBoot:
  1. Parses decrypted/decompressed XNU Mach-O kernelcache.
  2. Allocates and verifies disjoint memory segments in guest RAM.
  3. Prepares DeviceTree binary and patches frequencies, panel ID, and memory maps.
  4. Synthesizes XNU `boot_args` structure:
     - `topOfKernelData`
     - Command line: `"debug=0x8 serial=1 nand-enable-adm=0 rd=md0"`
     - Video memory parameters for CLCD framebuffer.
  5. Hooks privileged SVC call sites for external memory-disk bridges (`md_bridge.c`, `md_raw_bridge.c`).
  6. Initializes CPU state: `r0 = boot_args_pa`, `r15 = entry_point`, CPSR = SVC mode.

---

## 4. Bottlenecks in the Current Architecture

1. **Per-Instruction Decode Overhead**:
   Every retired instruction in `arm_step()` performs full bitfield extraction, switch/if-else ladders, condition testing, and register bank resolution from scratch.
2. **Interpreter Dispatch Overhead**:
   Executing a basic block of $N$ instructions incurs $N$ complete iterations through the outer dispatch loop, repeatedly querying interrupt lines, updating `cycles`, checking fetch block bounds, and manipulating PC.
3. **Redundant Register File Traffic**:
   Every temporary ALU computation immediately updates `c->r[Rd]`, paying memory write latency and preventing host register residency.
4. **Lack of Basic Block Caching**:
   Loop bodies (e.g. tight memory copy, spinlocks, or cryptographic loops) re-decode the same ARM/Thumb opcodes millions of times.
5. **No Direct Block Linking**:
   Even when a branch target is a known constant, execution exits to the outer dispatcher and performs a new address lookup.
