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
| SPI0..4 | 0x82000000 + n·0x100000; IRQ 0x1D, 0x1C, 0x1B, 0x1A, 0x19 | spi1 hosts `multi-touch,n88`; spi1's dma-channels: CDMA 18 into TXDATA (+0x10), 19 out of RXDATA (+0x20) | spi0 (v1) + NOR and spi1 (v1) + the touch controller modelled; CDMA channel 18 feeds spi1 once its SETUP DMA bit is set |
| GPIO | 0x83000000; int level/stat/en/type at +0x80/+0xA0/+0xC0/+0xE0, 7 groups | 0x83000000, size 0x1000, 7 interrupt groups, 0x2E ports, IRQ 0x5E | pads a register file whose bits 3:1 are the pin's mode (000 input, 001/111 output driving bit 0, 010/011 interrupt while high/low, 100/101 on a rising/falling edge, 110 on both; AppleS5L8920XGPIO 0xc0674164, 0xc0673ff0); bit 0 of an input reads the pin's level; bit 4 masks the pin's interrupt; interrupt status at +0x800 + 4g, write one to clear (AppleS5L8920XGPIOIC, 0xc0673b94); the touch attention (0xB4, active low) and the five button pins are wired |
| I²C0/1/2 | 0x83200000, 0x83300000, 0x83400000 | i2c0 (IRQ 0x13): accelerometer 0x1D, compass 0x1E/0x1F, audio0 0x4A, pmu 0x74, mikey 0x39, tethered 0x29; i2c2 (IRQ 0x11): als 0x49 | i2c0 and i2c2 modelled (`s5l8920_i2c.c`): not the S5L8900's Samsung controller but a whole-transfer one (address +0x0, first byte +0x10, count +0x18, FIFO +0x20, start +0x24, status +0xC); devices: LIS331DL (WHO_AM_I 0x3B), and the D1755, the CS42L61 codec (0x4A) and the CD3272 Mikey (0x39) as register files (the codec's driver waits for Mikey's) |
| CDMA | 0x87000000; channel n IRQ = 0x2A + n | reg 0x07000000+0x1C000, IRQs 0x2B.. | modelled (cdma.c) — **agrees**; a channel into an I2S port is paced at the sample rate (bit 20 past a descriptor marked 0x300, pause bit 5 with the position at +0x10, stop bit 2 acknowledged in bit 21) |
| Audio complex | — | `audio-complex,s5l8920x`: reg 0x04300000+0x5000 and 0x04400000+0x1000, dma-parent cdma, dma-channels 0x15..0x18, `ncoref-frequency` (iBoot fills it; 162 MHz) | stored and read back; the NCO (+0x18 = 2f, +0x1C = 2f − ref, AppleS5L8920X 0xc067832c) is the I2S master clock, and f / 64 the sample rate the ports play at |
| I2S0/1/2 | — | 0x04500000, 0x04501000, 0x04502000 (+0x80000000), IRQ 0x49/0x48/0x47, children `audio-data,cs42l58` (the codec), `voice`, `baseband`; dma-parent the audio complex | stored and read back (the driver never reads them); i2s0's frames, from CDMA channel 21, go to the host's speaker; the interrupts are not driven |
| SHA-1 | (none) | reg 0x00100000+0x1000, IRQ 0x21, `sha1,s5l8920x`, `slave-dma-only`, dma-channels {4, 0x801000A0} | modelled (`s5l_sha1.c`): fed by CDMA channel 4 as a peripheral request; 3.1.3's code-page validation runs on it |
| AES | +0x800000; ctx control: bit16 encrypt, bit17 CBC, bits19:18 key size, bit20 register key, bits21+ hardware key (0 UID, 1 GID); IV ctx1 at 0x87801010; global +0x0 bits 0/1 = UID/GID disabled | reg 0x07800000+0x9000 | modelled with a stand-in hardware key — **agrees**; global register reads 0 (both keys available) |
| CLCD | 0x85400000; control +0x0 (bit0 enable, bit1 idle), window +0x20..+0x3C (framebuffer address +0x24, stride +0x28, size +0x30), timing +0x1B10..+0x1B24, +0x300, +0x400..+0x40C | reg 0x05400000+0x300000, IRQ 0x25, `clcd,s5l8920x` | modelled (`m2clcd.c`): iBoot's hand-off, the driver's start/interrupt protocol, 60 Hz frames, scanout from the window registers |
| MIPI-DSIM | 0x89000000; Samsung DSIM register map (STATUS, SWRST, CLKCTRL, … PLLCTRL) | reg 0x09000000+0x100000, IRQ 0x24, `mipi-dsim-1,samsung`, child `lcd` (`lcd,pinot`, `lcd-panel-id` 0 for iBoot to fill) | modelled as far as the driver waits (`s5l8920_dsim.c`): STATUS follows CLKCTRL's HS request, the software reset, and ESCMODE's ULPS; packets counted, no panel answers; n88_boot fills `lcd-panel-id` as iBoot does (ApplePinotLCD refuses 0) |
| DART0/1 | — | 0x3FE00000 / 0x3FF00000 (+0x80000000), `dart,s5l8920x`; dart0 is the clcd's `iommu-parent` | modelled (`s5l8920_dart.c`): 16 slots loaded through +0x8, 4 KiB second-level tables, translation on at +0xC bit 31; the scanout follows the clcd window through dart0 |
| PMU | (none for 3GS) | `pmu,d1755` (Dialog) at i2c0 address 0x74, IRQ 0x9D via GPIO | a register file that acknowledges: AppleD1755PMU starts (bucks read as 725 mV), so the LDOs, backlight and RTC users stop waiting; the RTC is a seconds count at 0x4C..0x4F (the OS keeps its offset at 0x64..0x67), run from the host clock; 0x11 bit 6 powers the touch controller; 0x01 bit 0/1 latch a menu/hold wake; no charger or other events yet |
| Buttons | GPIO-based; pins from board config | volup GPIO 0x1600, voldown 0x1601, menu 0x1606, hold 0x1607, ringer 0x1403; wake via PMU | modelled (`n88_set_input`): hold and menu are high when pressed (flags bit 8), volume low, the switch low at silent; interrupts on both edges; hold or menu wakes a sleeping system through the kernel's vector page |
| Touch | (none) | `multi-touch,n88` on spi1; enable_cs GPIO 0x1300, reset GPIO 0x1401, IRQ 0xB4 | modelled with the 3G's `mtz2.c` unchanged: the N1 bootloads over HBPP (53,924-byte firmware by DMA, prox and panel calibration, N1 calibration registers 0x1000300C/305C/3058/3000, version 0x10003800) and reports contacts; a drag unlocks the phone |
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
3. ~~GPIO interrupts and buttons~~: done (the interrupt status at +0x800,
   not openiBoot's +0x80..+0xE0, and the five button pins above).
4. ~~Touch on spi1~~: done, the 3G's Z2 model unchanged.
5. **PMU D1755** on i2c0 (0x74): traced as far as boot, the RTC, the touch
   LDO and the wake reason need; still to come: its interrupt (GPIO 0x9D)
   and events, the charger, the alarm that wakes a sleeping system.
