# iOS 6 graphics: the deciding question (iPhone 3GS, iOS 6.1.6)

Status 2026-09-24: a design note, written before any iOS 6 code exists.
Nothing below is measured on iOS 6 yet. Every claim is marked **Known**
(established in this project), **Expected** (from general knowledge of the
platform, to be confirmed from the firmware), or **Unknown**.

The target is the one recommended in `IOS6_READINESS.md`: iPhone 3GS
(S5L8920, Cortex-A8, PowerVR SGX535, 320×480), iOS 6.1.6.

## 1. Why graphics decides the project

**Known:** on the current machine (iPhone OS 3.1.3), the home screen and
UIKit apps render without any GPU model. The work image's SpringBoard
launchd plist gets `CA_ENABLE_MBX2D=0`, and QuartzCore then uses its own CPU
renderer (the `CA::OGL::sw_*` functions that dominate today's profiles). That
is the "CPU graphics" mode.

**Expected:** iOS 6 composites every screen through CoreAnimation on OpenGL
ES 2.0, on the SGX, and `IOS6_READINESS.md` records "no software fallback".
**Unknown:** whether that is literally true of iOS 6.1.6's QuartzCore, or
whether a CPU renderer and a switch to select it still exist. That one fact
splits the plan in two.

## 2. The three routes

| Route | What the emulator implements | Runs | Cost |
|---|---|---|---|
| **A. CPU renderer** (if §3 Q1 says it exists) | Nothing new for graphics; a launch-time switch, as today | SpringBoard, UIKit apps | Small |
| **B. Emulate the SGX535** | The GPU's register file, command submission, tiler, and its programmable shader cores (USSE) | Everything, unmodified | Very large. The shader instruction set is not publicly documented; the MBX model had no shaders to execute. |
| **C. Emulate at the driver boundary** | The kernel driver's IOKit user-client interface, turning submitted work into host rendering | Everything that goes through that interface | Large. The user-space GL driver still compiles shaders to USSE binaries, so the shader problem of route B remains unless it is solved above that point. |

Route A does not help games that draw with OpenGL ES themselves (Angry Birds
among them). They need B or C whatever A shows.

## 3. The questions, in order, and how each is answered

All of these are answered from the 3GS iOS 6.1.6 IPSW, which the user
supplies, with tools already in `tools/`: `ipsw_explore.py` (members),
`img3dump` (containers), `hfsx_extract.py` (files from the root image), and
`dscsym.py` / `dscmap.py` (the dyld shared cache). No emulator run is needed.

1. **Does iOS 6.1.6's QuartzCore contain the CPU renderer?** Search the
   shared cache for the `CA::OGL::sw_` symbols seen in today's profiles, and
   for the environment variable or default that selects them (the 3.1.3 build
   reads `CA_ENABLE_MBX2D`, falling back to `LK_ENABLE_MBX2D`). If both
   exist, route A is the first milestone.
2. **Which GL driver and kernel driver does the 3GS use?** Expected: a
   user-space GLES driver bundle for the SGX535 and a kernel extension that
   owns the GPU's registers. Their names, the user-client methods, and the
   command buffer layout come from the firmware, not from memory.
3. **How much of the shader path can be avoided?** For route C: whether
   shaders reach the kernel as USSE binaries only, or whether any higher-level
   form is available at a boundary the emulator can see.
4. **What does QuartzCore need at minimum** (surfaces, framebuffer, vsync)
   for route A on iOS 6: `IOSurface` and `IOMobileFramebuffer`, as on the
   current machine?

## 4. Decision rule

- Q1 yes: build route A first. It gets iOS 6 to the home screen and UIKit
  apps with the CPU and SoC work of `IOS6_READINESS.md` §3 and no GPU model.
  Choose between B and C for games afterwards, with the evidence from Q2–Q3.
- Q1 no: no screen at all without B or C, so Q2–Q3 come before any SoC
  modelling beyond the boot to a kernel console.

## 5. What carries over from the current machine

- The framebuffer, display, touch and host presentation path in the app.
- The work-image mechanism that already injects `CA_ENABLE_MBX2D=0`, if Q1
  finds an equivalent switch.
- The method used for MBX (`core/src/soc/mbx.c`): read the driver, model
  what it waits on, and count every rejected command. The MBX model itself
  does not carry over: the SGX is a different GPU.
