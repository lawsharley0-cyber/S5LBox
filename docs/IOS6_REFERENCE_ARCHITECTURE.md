# iOS 6 reference architecture

Four open-source projects compared with NEON (formerly S5LBox), to decide
what NEON should take from each on the way to iOS 6.1.6 (10B500) on the
iPhone 3GS (iPhone2,1, S5L8920, Cortex-A8). Inspected 2026-10-02 at:

| Project | Commit | License | What it is |
|---|---|---|---|
| [nokernelspace/qemu-ios](https://github.com/nokernelspace/qemu-ios) | `3288156` | GPL-2.0 (QEMU) | Full-system emulator: iPod touch 2G (S5L8720) as QEMU board + 30 device models |
| [touchHLE/touchHLE](https://github.com/touchHLE/touchHLE) | `8eb3418` | MPL-2.0 | App-level HLE: runs 32-bit iOS 2–4 apps against its own frameworks |
| [HyperHLE/HyperHLE](https://github.com/HyperHLE/HyperHLE) | `2281c14` | MPL-2.0 | touchHLE fork with many more frameworks |
| [0xjohnnydev/Applesauce](https://github.com/0xjohnnydev/Applesauce) | `e8e9d67` | MPL-2.0 | iOS app packaging touchHLE and HyperHLE as switchable cores |

**Licensing, decided up front.** NEON is MIT. QEMU-iOS is GPL-2.0: nothing of
it can be copied into NEON without relicensing NEON, so it is a reference for
*what the hardware does* only. The touchHLE family is MPL-2.0, which is
file-level copyleft: a file taken from it stays MPL-2.0 with its notice,
inside an otherwise-MIT project. That is permitted, but this document
recommends no such copy, because none of their files fits NEON's
architecture as is (Rust, a different machine model). HyperHLE's README
also describes a provenance dispute with upstream touchHLE, which is one
more reason to use it as a list of *what newer apps need*, not as a code
source. Everything below is concepts.

---

## A. Current NEON architecture

NEON already is a full-system (LLE) emulator: it boots Apple's own unmodified
kernel and userspace on modelled hardware. Two machines exist.

| | `s5l8900` (iPhone 3G) | `n88` (iPhone 3GS) |
|---|---|---|
| Code | `core/src/soc/machine.c` + device files | `core/src/soc/n88.c` + `spi.c`, `spi_nor.c`, `cdma.c`, `vic.c` |
| CPU | ARM1176 (ARMv6, Thumb-1, VFPv2) | Cortex-A8 (ARMv7-A, Thumb-2, VFPv3, NEON) |
| DRAM | 128 MiB at 0x08000000 | 256 MiB at 0x40000000 |
| Firmware | iPhone OS 3.1.3 (7E18) | iOS 6.1.6 (10B500); iPhone OS 3.1.3 (7E18) |
| Furthest point | SpringBoard, touch, apps installed from IPAs | 10B500: launchd and daemons, then reboots at the system keybag; 7E18: IOKit up, waits for root device |

Shared layers:

- **CPU** (`core/src/arm/`): `arm_interp.c` reference interpreter (the
  specification, differential-tested against Unicorn), `arm_ci.c` cached
  interpreter (pre-decoded blocks, the default in the app), `vfp.c`,
  `neon.c`; an opt-in build-time-generated AArch64 engine
  (`a64_static_engine.c`). The runtime JIT in `core/src/jit/` is
  development-only and excluded from the app. **No JIT in the product.**
- **MMU** (`core/src/mmu.c`): ARMv6 and ARMv7 short-descriptor
  translation, software TLB, fetch and data host-pointer caches.
- **Boot** (`core/src/boot/bringup.c`, `n88_boot`): iBoot's hand-off done
  directly: Mach-O segments placed, device tree patched (memory, clocks,
  framebuffer, un-matched nodes), `boot_args` built, CPU entered. No boot
  ROM, LLB or iBoot runs, so no GID-key decryption is needed at runtime.
- **Firmware tools** (`core/src/firmware/`, `app/Sources/VMFirmware*.c`):
  IPSW unzip, IMG3, AES, LZSS, DMG/HFS, device tree, kernel symbols.
  Keys are supplied by the user, never stored in the repository.
- **Storage**: privileged-SVC memory-disk bridges (`md_bridge.c`) serve an
  HFS+ image as `/dev/md0`; the kernel is patched at hash-pinned sites to
  call them.
- **Devices**: PL192 VICs (now daisy-chained), timers, UARTs, I²C, PMU,
  SPI (v0 and v1), NOR + effaceable storage, GPIO, multi-touch Z2 (3G),
  WM8991 + I²S + PL080 audio (3G), CLCD scanout (3G), CDMA + AES (3GS),
  experimental MBX (3G).
- **HLE that already exists**: guest-function interception with
  byte-verified prologues (`tools/ios3_hle.c`, `app/Sources/VMFirmwareHLE.c`)
  for iPhone OS 3's software rasteriser; SVC bridges for the disk.
- **Host side** (`app/Sources/`): UIKit shell, frame publication to a
  UIKit view (no Metal yet), AudioQueue output from a lock-free ring,
  touch and button queues, instances, snapshots, IPA install into the guest.

## B. QEMU-iOS: reusable concepts

QEMU-iOS emulates the iPod touch 1G/2G by running the **real boot ROM**,
then LLB, iBoot and the kernel from NOR and NAND images, on QEMU's TCG
CPU. Its board (`hw/arm/ipod_touch_2g.c`) maps ~30 device models, one per
file under `hw/arm/ipod_touch_*.c`.

What is worth taking, as ideas:

1. **One file per peripheral, one board file that wires them.** Each
   device owns its registers and raises a named IRQ; the board file is the
   only place that knows addresses and lines. NEON's `s5l8900` follows this;
   `n88.c` has started to (SPI, NOR, CDMA are separate files) and should
   keep going rather than grow `n88.c`.
2. **The device list itself** is a map of what an Apple SoC of that era
   needs: chip ID, clock and power managers, system IC, timers, VICs + edge
   IC, GPIO, SPI x5, I²C x2, PL080 DMA, SHA1, AES, PKE, FMSS (NAND), SDIO
   (Wi-Fi), USB OTG + PHY, LCD + MIPI-DSI, scaler/CSC, TV-out, MBX, PMU,
   audio codec, accelerometer (LIS302DL), light sensor, multitouch. The 3GS
   has successors of most of these; the CS42L61 codec, the Z2 multi-touch on
   spi1, the accelerometer and the MIPI-DSI/CLCD are the ones iOS 6 will ask
   for next.
3. **NAND as a directory of page files** (`ipod_touch_fmss.c`): sparse,
   simple, debuggable. Useful if NEON ever needs a real FTL path; for now the
   memory-disk bridge avoids NAND entirely, which is cheaper.
4. **MBX is a register stub.** Even a project that boots to the home screen
   leaves the GPU unmodelled and lets iOS composite on the CPU. That is the
   same position as NEON's 3GS (SGX535 unmodelled) and supports deferring a
   GPU model.

What not to take:

- **Booting the real boot ROM chain.** It needs the GID key, which no
  emulator has; QEMU-iOS answers GID requests with a hard-coded sequence of
  firmware keys in the AES model and patches iBoot in memory at fixed
  addresses from inside the NAND model. That ties the emulator to one
  firmware and puts Apple's keys in source. NEON's direct kernel hand-off
  avoids both, and is the better design for iOS 6.
- QEMU itself (GPL, TCG needs a JIT on iOS or the slow TCI interpreter).

## C. touchHLE: reusable concepts

touchHLE does not emulate hardware. It loads an app's Mach-O, runs only the
app (plus a few dylibs) on dynarmic, and replaces iOS with host Rust code:
its own dyld, Objective-C runtime, Foundation, UIKit, Core Animation,
OpenGL ES (on host GL), OpenAL and AudioToolbox.

Mechanisms worth understanding:

1. **Host calls through SVC stubs** (`src/dyld.rs`). Each imported symbol is
   linked to a guest stub `SVC #n` whose number indexes a table of host
   functions; ABI translation (`src/abi.rs`) moves AAPCS arguments in and the
   result out. NEON's disk bridge (SVC 0xe1/0xe2) is the same pattern; it
   generalises to any **host service a patched or interposed guest function
   can call**.
2. **Lazy linking**: stubs bind on first call (SVC 0 is the lazy linker),
   so unused imports cost nothing.
3. **Flat guest address space**: one 4 GiB host reservation, guest address
   + base = host pointer, null page excluded. Cheap, but it needs the
   `extended-virtual-addressing` entitlement on older devices (see E), and it
   is only possible because touchHLE has no MMU. NEON runs a real kernel
   with paging, so it keeps a TLB; the lesson is to make the TLB hit path as
   close to "base + offset" as possible (NEON's host-pointer caches do this).
4. **Cooperative guest threads** on one host thread, with coroutines for
   host functions that block.
5. **GLES translation** (`src/gles/`): GLES 1.1 passed through or rebuilt
   on GL 2.1. This is the reference for a future *guest GLES → Metal* path
   if NEON ever intercepts GL calls rather than modelling the SGX.
6. **What apps actually call.** touchHLE's framework coverage, and the
   app compatibility database behind it, is a measured list of the APIs early
   games depend on.

What NEON should not adopt as its main path: the whole-framework HLE. It
scales with the size of the API surface, and iOS 6's surface (UIKit,
CoreText, AVFoundation, Core Data, MapKit, Social, PassKit…) is many times
iOS 3's. NEON runs Apple's real frameworks, so every one of those works as
soon as the hardware and kernel underneath do.

## D. HyperHLE: what newer software needs

HyperHLE is touchHLE (≈76 k lines of Rust) grown to ≈205 k lines. The
additions are exactly the frameworks that iOS 4–6-era software starts using:

- Audio: `AudioConverter`, `ExtAudioFile`, `AUGraph`, Core Audio, a CAF
  decoder; media: `AVPlayer`, `AVCapture`, Core Media, Core Video.
- Text and graphics: Core Text, Core Image, CG paths/gradients/layers/patterns,
  `CATransform3D`, keyframe animations.
- System: Accounts, AddressBook (+UI), AssetsLibrary, CoreBluetooth,
  CommonCrypto, Accelerate, CFNetwork streams and HTTP messages, captive
  network, more Foundation (`NSCalendar`, `NSCondition`, …).
- Roughly 100 lines naming third-party bundle identifiers (a pattern
  count, not reviewed one by one), and 33 source files that mention hacks or
  workarounds (touchHLE: 23).

The lesson for NEON is a prediction, not code: **as software moves from
iPhone OS 3 to iOS 6, it leans on audio conversion, media playback, text
layout, keychain/crypto and networking.** In NEON these run as Apple's own
code, so what they need from the emulator is hardware and kernel support:
the AES engine and keybag (crypto, keychain, data protection), the audio
codec and DMA (AudioConverter output still ends at I²S), a working network
interface, and a fast CPU path for software text and image rendering.
Per-app hacks are a failure mode NEON avoids by design.

## E. Applesauce: lessons for running on modern iOS

Applesauce is the iOS host app for touchHLE and HyperHLE. Relevant facts:

1. **It requires JIT.** dynarmic needs executable memory, so the app only
   works after StikDebug (iOS 17.4+), TrollStore's Enable JIT, or AltJIT;
   its README lists iOS versions where none of those works. It detects JIT
   by checking `CS_DEBUGGED` through `csops`, because probing `mmap` misleads
   when `MAP_JIT` is absent. This is the strongest confirmation of NEON's
   no-JIT design: a JIT core is only as available as the current JIT
   workaround.
2. **Its flat 4 GiB guest mapping needs entitlements** a free Apple account
   cannot sign (`extended-virtual-addressing`, `increased-memory-limit`), so
   those builds are TrollStore-only. NEON maps only the guest's DRAM (128 or
   256 MiB), which fits any signing.
3. **Two cores, chosen per game.** NEON's equivalent is per-instance machine
   profiles: an instance is a device + firmware + engine choice.
4. **Touch in rotated orientations** needed repeated fixes (its recent
   commits). NEON's touch map should be tested in every interface
   orientation the guest supports, not only portrait.
5. Practical packaging: unsigned IPA for sideloading, TrollStore variants,
   `touchHLE_apps` folder exposed through Files. NEON already sideloads and
   imports IPSWs and IPAs through Files.

## F. Missing iOS 6 components

Status on the 3GS (`n88`) unless noted. "Needed" is what iOS 6.1.6 must
have from the emulator to reach SpringBoard and run apps.

| Component | NEON | QEMU-iOS | touchHLE | Needed for iOS 6 |
|---|---|---|---|---|
| ARM CPU | ARMv7-A + Thumb-2 + VFPv3 + NEON; interpreter + cached interpreter, no JIT | QEMU TCG (ARM1176) | dynarmic JIT (ARMv6/v7 user) | have; speed is the open problem |
| MMU | ARMv7 short-descriptor, software TLB | QEMU softmmu | none (flat 4 GiB) | have |
| Mach-O | kernelcache loader; userspace by Apple's dyld | n/a (iBoot loads kernel) | own loader | have |
| dyld | Apple's, with the shared cache | Apple's | own HLE dyld | have |
| Objective-C | Apple's runtime | Apple's | own runtime | have |
| UIKit / Foundation | Apple's | Apple's | partial reimplementation | have (as guest code) |
| Framebuffer | iBoot framebuffer + boot log; AppleM2CLCD scanout not modelled | LCD + MIPI-DSI | host window | **CLCD scanout** |
| GPU | none (SGX535); software compositing | MBX stub | GLES → host GL | defer; use `CA_NO_ACCEL`-style software path |
| Audio | 3G only (WM8991/I²S/PL080) | CS42L58 | OpenAL/AudioToolbox HLE | **CS42L61 + 3GS I²S/DMA** |
| Networking | PPP + NAT/TCP in core (3G); none on 3GS | none | host sockets (CFNetwork HLE) | later |
| NAND | none; root via memory-disk bridge | FMSS page files | n/a | not needed while the bridge works |
| Filesystem | HFS+ image via bridge; work-image provisioning | NAND image | host directory | **writable data volume** for /private/var |
| Kernel boot | direct hand-off; 10B500 and 7E18 | real boot ROM → iBoot | none | have |
| Device tree | parse + patch (memory, clocks, framebuffer, un-match) | iBoot's | n/a | have |
| Interrupts | PL192 x3, chained | PL192 x2 + edge IC | n/a | have |
| Timers | timebase + decrementer | S5L8720 timers | host time | have |
| AES / keys | CDMA + AES, stand-in hardware key | AES with hard-coded GID results | n/a | have (new) |
| Keybag / data protection | effaceable storage on NOR; **no system keybag** | n/a (pre-iOS 4) | n/a | **create keybag on first boot** |
| Sandbox | Apple's (Seatbelt) | Apple's | none | have (as guest code) |
| Touch | Z2 on 3G only; 3GS spi1 multi-touch not wired | multitouch over SPI | host touch → UIEvent | **3GS Z2 on spi1** |
| Buttons | 3G only | GPIO keys | n/a | **3GS GPIO buttons** |
| Sensors | none | LIS302DL, ISL29003 | host accelerometer | accelerometer later |
| Baseband / Wi-Fi / BT | none | SDIO stub | n/a | stubs only |

## G. Proposed architecture

The four projects sit at different points between LLE and HLE. NEON's
place is already chosen and is the right one for iOS 6: **real Apple
software on modelled hardware**, with HLE only at narrow, verified
boundaries. The proposal keeps every working component and adds structure
in small steps.

```
            Apple's iOS 6 kernel, dyld, frameworks, apps (unchanged)
                                   │
      ┌────────────────────────────┼─────────────────────────────┐
      │  LLE: modelled hardware    │   HLE: verified boundaries   │
      │  CPU, MMU, VICs, timers,   │   - disk: SVC memory-disk     │
      │  SPI/NOR, CDMA/AES, CLCD,  │     bridge (have)             │
      │  touch, audio codec + DMA  │   - hot guest functions run   │
      │                            │     natively, prologue-checked │
      │                            │     (have for 3.1.3 raster)   │
      │                            │   - later: GL calls → Metal   │
      └────────────────────────────┴─────────────────────────────┘
                                   │
          Host: frame publication, audio ring, touch/button queues
```

1. **Device profiles as data, not a rewrite.** `n88.h` already holds the
   3GS facts as constants (memory map, IRQ lines, clocks, framebuffer) and
   `n88_boot_t` now carries per-firmware choices (virtual base derived from
   the kernel, `boot_args_version`, root bridge sites). The next step is a
   small `n88_profile_t` naming one exact combination (iPhone2,1 + 10B500,
   iPhone2,1 + 7E18) that bundles the epoch, kernel patch table and
   cmdline, so the app and `boot3gs` stop passing those separately. The
   3G machine stays as it is; a shared profile type is worth it only once a
   third machine exists.
2. **One file per new device**, as QEMU-iOS does and as `spi_nor.c` and
   `cdma.c` now do: CLCD/MIPI-DSI, Z2 on spi1, CS42L61, GPIO buttons. Each
   with a test that replays the exact register sequence the 10B500 driver
   was seen to make.
3. **Generalise the SVC bridge into a host-service table** (touchHLE's
   pattern) so new boundaries (a data-volume block device, later a GL
   transport) do not each invent their own SVC numbers.
4. **Generalise prologue-verified interception** from the 3.1.3 raster
   hooks to the 3GS, for the measured hot spots of iOS 6's software
   compositing. This is the no-JIT speed lever with the best evidence
   (Podium measured 94% of backboardd's time in four span functions).
5. **Keep CPU backends interchangeable** behind the existing
   `arm_ci`/reference split; a future threaded-dispatch backend slots in
   the same way, with the reference interpreter as the oracle.

Order of work toward SpringBoard on the 3GS, each step independently
testable: system keybag on first boot (the next blocker) → writable data
volume → CLCD scanout → Z2 touch on spi1 and buttons → CS42L61 audio →
profile the first SpringBoard frames and add native hot paths.
