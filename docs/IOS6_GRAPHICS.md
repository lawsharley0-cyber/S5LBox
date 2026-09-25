# iOS 6 graphics: the deciding question (iPhone 3GS, iOS 6.1.6)

Status 2026-09-24: a design note, written before any iOS 6 code exists.
Nothing below is measured on iOS 6 yet. Every claim is marked **Known**
(established in this project), **Expected** (from general knowledge of the
platform, to be confirmed from the firmware), or **Unknown**.

**Update 2026-09-25: Q1 is answered, yes.** iOS 6.1.6's QuartzCore still
composites the display in software when OpenGL is switched off, and the
switch is `CA_ENABLE_OGL=0` in `backboardd`'s environment. Section 6 has the
evidence; by the decision rule in section 4, route A comes first.

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

## 6. Answers from the firmware (2026-09-25)

Read statically from the user's `iPhone2,1_6.1.6_10B500_Restore.ipsw`
(MD5 `1b6a0a0c9701d3762be7c4e90799c0b5`, equal to Apple's download ETag), with
the user's own keys, kept out of the repository like the firmware itself.
The root filesystem was decrypted with `tools/vfdecrypt.py`, unpacked with
`tools/udif.py`, and `dyld_shared_cache_armv7` extracted with
`tools/hfsx_extract.py`. The cache was read with `tools/dsc6.py`, new for
this: the iOS 3 tools do not read iOS 6's separate local-symbol region or
Thumb-2 address building. Addresses below are cache virtual addresses in
that build; nothing here has run yet.

**Q1, the CPU renderer: Known, present and wired to the display.**

1. QuartzCore contains the software renderer the current machine uses,
   `CA::OGL::SWContext` (vtable `0x396c2110`), with its samplers and blend
   routines (`CA::OGL::SW::*`).
2. `CA::WindowServer::Server::sw_renderer()` (`0x32daf078`) builds an
   `SWContext` and a renderer over it on first use. Its callers are
   `Server::render_update` (`0x32daf124`), which renders every update with
   it unconditionally, and `Server::render_surface`.
3. iOS 6 has one concrete window server, `EAGLServer`; `Server` and
   `IOMFBServer` are its bases. `EAGLServer::render_update` (`0x32da6a78`)
   asks `EAGLServer::renderer()` (`0x32da6964`) for the OpenGL renderer and,
   **when that returns none, tail-calls `Server::render_update`**: the
   software path.
4. `EAGLServer::renderer()` returns none when a flag is clear, or when the
   OpenGL context cannot be created. The flag is read once, in the
   `EAGLServer` constructor (`0x32da67ec`): `getenv("CA_ENABLE_OGL")`,
   default 1, `atoi` of the value when set. So `CA_ENABLE_OGL=0` selects
   software compositing.
5. The window server runs in `backboardd`, not SpringBoard as on iPhone
   OS 3: `/System/Library/LaunchDaemons/com.apple.backboardd.plist` owns
   the `com.apple.CARenderServer` Mach service, and `backboardd` drives
   `CAWindowServer`. The switch therefore belongs in that plist's
   `EnvironmentVariables`, the iOS 6 counterpart of the `CA_ENABLE_MBX2D=0`
   the work image puts into SpringBoard's today.
6. Not the switch: `CA_NO_ACCEL` is read only by `CA::CG::IOSurfaceQueue`,
   Core Graphics drawing into IOSurfaces.

What this does not settle (**Unknown** until it runs): whether anything
else in `backboardd` or UIKit insists on OpenGL ES before the first frame;
how fast `SWContext` is at 320×480 on this emulator; and what
`IOMFBDisplay` needs from the display driver (Q4). The display path is
`IOMobileFramebuffer`, as on the current machine; on the 3GS it sits on the
`clcd` controller and a MIPI DSI link (`mipi-dsim`) in the device tree.
Games that draw with OpenGL ES themselves still need route B or C.

**Q2 and Q3 (the SGX driver and shader path): not yet read.** The device
tree has the GPU at `/arm-io/sgx` (`sgx,s5l8920x`, registers
`0x05300000`, interrupt 41). Route A no longer waits on them.

**Q4, what QuartzCore needs at minimum:** partly known. The display class
is `CA::WindowServer::IOMFBDisplay` over `IOMobileFramebuffer`, with
`IOSurface`-backed pages (`CA::WindowServer::IOSurface`). The exact calls
are next to read, from the same cache, before the display model is built.
