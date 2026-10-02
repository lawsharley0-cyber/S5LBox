# S5L8920 (iPhone 3GS) hardware references

Independent, public sources for the hardware NEON's `n88` machine models,
checked against the 3GS's own device tree (10B500) and the iOS 6 kernel's
drivers. Inspected 2026-10-02.

| Source | Commit | License | Use here |
|---|---|---|---|
| [iDroid-Project/openiBoot](https://github.com/iDroid-Project/openiBoot) `plat-s5l8920/` (`scons iPhone3GS`) | `866562f` | GPLv3 | register layouts and init sequences, as facts; no code copied (NEON is MIT) |
| [axi0mX/ipwndfu](https://github.com/axi0mX/ipwndfu) `limera1n.py`, `device_platform.py` | `0e28932` | GPLv3 | SecureROM-stage memory layout only. Its exploit code is not relevant, and its `aes-keys/` (Apple firmware keys) was not read and must not be used |
| The 3GS device tree (user's own IPSW) | 10B500 | — | the authority for addresses, interrupts and board wiring |

openiBoot is an independently written bootloader, not Apple's iBoot. Its 3GS
build covers boot-level hardware (clocks, VIC, timers, GPIO, SPI, I²C, UART,
CDMA/AES, CLCD, MIPI-DSI, NAND via H2FMI/VFL/YAFTL, USB) and has **no PMU,
touch or audio driver** for the 3GS (`audiohw-null.c`; no `pcf` module, and
the 3GS PMU is not a PCF part anyway, see below). So it complements, but does
not replace, tracing what iOS 6's own drivers do.

## Cross-check: openiBoot vs device tree vs NEON

| Block | openiBoot | Device tree (10B500) | NEON `n88` |
|---|---|---|---|
| VIC0/1/2 | 0xBF200000, +0x10000 each | — | same; daisy-chained VECTADDR (8e6d34d) |
| Timer | 0xBF100000; 7 timers at 0x20 stride (+0x0..+0xE0), ticks at +0x200/+0x204, IRQ latch +0xF8, status +0x10000 | — | ticks and decrementer at 0xBF100200 only; the per-timer registers are **not modelled** (3.1.3's AppleS5L8920X reads +0x10..+0xFC) |
| UART0 | — | 0x82500000 | modelled (console) |
| SPI0..4 | 0x82000000 + n·0x100000; IRQ 0x1D, 0x1C, 0x1B, 0x1A, 0x19 | spi1 hosts `multi-touch,n88` | spi0 (v1) + NOR modelled; spi1 not |
| GPIO | 0x83000000; int level/stat/en/type at +0x80/+0xA0/+0xC0/+0xE0, 7 groups | 0x83000000, size 0x100000, 7 interrupt groups, 0x2E ports, IRQ 0x5E | register file only; no GPIO interrupts |
| I²C0/1/2 | 0x83200000, 0x83300000, 0x83400000 | i2c0: pmu, audio0, accelerometer, compass, mikey; i2c2: als | not modelled |
| CDMA | 0x87000000; channel n IRQ = 0x2A + n | reg 0x07000000+0x1C000, IRQs 0x2B.. | modelled (cdma.c) — **agrees** |
| SHA-1 | (none) | reg 0x00100000+0x1000, IRQ 0x21, `sha1,s5l8920x`, `slave-dma-only`, dma-channels {4, 0x801000A0} | modelled (`s5l_sha1.c`): fed by CDMA channel 4 as a peripheral request; 3.1.3's code-page validation runs on it |
| AES | +0x800000; ctx control: bit16 encrypt, bit17 CBC, bits19:18 key size, bit20 register key, bits21+ hardware key (0 UID, 1 GID); IV ctx1 at 0x87801010; global +0x0 bits 0/1 = UID/GID disabled | reg 0x07800000+0x9000 | modelled with a stand-in hardware key — **agrees**; global register reads 0 (both keys available) |
| CLCD | 0x85400000; control +0x0 (bit0 enable, bit1 idle), window +0x20..+0x3C (framebuffer address +0x24, stride +0x28, size +0x30), timing +0x1B10..+0x1B24, +0x300, +0x400..+0x40C | reg 0x05400000+0x300000, IRQ 0x25, `clcd,s5l8920x` | modelled (`m2clcd.c`): iBoot's hand-off, the driver's start/interrupt protocol, 60 Hz frames, scanout from the window registers |
| MIPI-DSIM | 0x89000000; Samsung DSIM register map (STATUS, SWRST, CLKCTRL, … PLLCTRL) | reg 0x09000000+0x100000, IRQ 0x24, `mipi-dsim-1,samsung`, child `lcd` | not modelled |
| PMU | (none for 3GS) | `pmu,d1755` (Dialog) at i2c0 address 0x74, IRQ 0x9D via GPIO | not modelled |
| Buttons | GPIO-based; pins from board config | volup GPIO 0x1600, voldown 0x1601, menu 0x1606, hold 0x1607, ringer 0x1403; wake via PMU | not modelled |
| Touch | (none) | `multi-touch,n88` on spi1; enable_cs GPIO 0x1300, reset GPIO 0x1401, IRQ 0xB4 | not modelled (the 3G's Z2 model, `mtz2.c`, is the starting point) |
| Backlight | — | `/backlight`, `skip-clcd-probe`, a 128-entry table | not modelled |

## Boot chain

Public sources describe the S5L8920 chain as BootROM → LLB → iBoot →
kernel. From ipwndfu: the SecureROM runs from 0x0, loads LLB/DFU images into
SRAM at **0x84000000** (at most 0x24000 bytes), with its stack at
0x84034000. openiBoot shows what the stages initialise (clocks, MIU, NOR,
NAND, framebuffer at 0x4FD00000 for its own console).

NEON starts at the end of that chain on purpose: `n88_boot` performs iBoot's
hand-off itself (kernel placed, device tree patched, `boot_args` built). Running
LLB and iBoot instead would need their images decrypted by the user, and
iBoot then asks the AES engine's **GID** key to decrypt the kernelcache and
device tree. No emulator has that key, and answering GID requests with
firmware-specific keys (as QEMU-iOS does) ties the emulator to one firmware
and puts Apple's keys in source. The direct hand-off avoids both, so the boot
ROM, LLB and iBoot stay out of scope. ROM bytes are never used.

## What these references unblock, in order

1. **Display**: the CLCD at 0x85400000 is now modelled (`core/src/soc/m2clcd.c`,
   from AppleM2CLCD's own code plus openiBoot's map). Still to come: the
   MIPI-DSIM at 0x89000000, which the boot only reads once (zeros are
   accepted), and validation of the swap path, which needs a UI process
   (after the keybag on iOS 6, or 3.1.3 with a root filesystem).
2. **The timer block** at 0xBF100000 (per-timer config/state/count), which
   the 3.1.3 kernel probes.
3. **GPIO interrupts and buttons**: the GPIO interrupt registers at
   +0x80..+0xE0 and the five button pins above.
4. **Touch on spi1** (0x82100000, line 0x1C): adapt the 3G's Z2 model to
   `multi-touch,n88` with the tree's enable/reset pins.
5. **PMU D1755** on i2c0 (0x74): not in openiBoot; needs the kernel's
   AppleD1755PMU accesses traced, as was done for the SPI flash.
