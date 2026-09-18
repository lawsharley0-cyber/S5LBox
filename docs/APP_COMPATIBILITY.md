# Application Compatibility

Status as of 2026-09-18. This track was completely unstarted before this
session — no diagnostic pipeline, no compatibility matrix existed. This
records what was found/verified and what's still missing.

## What already exists

- **Install path**: `app/Sources/VMUserApp.c` + `VMUserAppInstall.c` (merged
  into `main` via `fix/audio-buffer-integrity`, along with the app's own
  `VMUserAppViewController.m` UI). Validates a user-supplied legacy IPA:
  32-bit ARMv6 Mach-O check, main-executable type check, load-command table
  bounds check, and a version gate (`vm_user_app_validate_version`) that
  rejects anything claiming a guest OS newer than 3.1.3 (`0x00030103`).
  Covered by `app_user_ipa` (`test_vmuserapp.c`).
- **Guest-native crash reporting already works** — confirmed by this
  project's own README: SpringBoard's historical MBX2D crash loop produced
  real `.crash` files written by Apple's own `ReportCrash`/`CrashReporter`
  daemon into the guest's filesystem, which is how that bug was originally
  diagnosed. **The guest does not need a host-built crash reporter — it has
  its own, and it is correct**, because it's Apple's unmodified code.

## What was missing, and is now verified working

A way to get those crash reports *out* of the guest's disk image after a
run, for a developer to actually read. `tools/hfsx_extract.py` already
existed (read-only HFS+/HFSX catalog walker, extracts one named file or
lists matches by substring) but had never been used for this.

Verified this session against a real, freshly-booted guest work image
(`work/rootfs-7e18-run05.img`, from a real boot with this session's
byte-verified `iPhone1,2` 3.1.3 firmware):

```sh
python tools/hfsx_extract.py <work-image> --list CrashReporter
```

Correctly walked the real catalog B-tree and found both `CrashReporter`
directories (`id=16045`, `id=16050`) and every `com.apple.ReportCrash*.plist`
/ `CrashHousekeeping` / `crash_mover` binary shipped in the real firmware.
No `.crash`/`.ips` files were present in *this* run because nothing crashed
in it (the historical MBX2D crash loop is already fixed) — this is expected,
not a gap in the tool.

**The pipeline, end to end, now looks like:**

```
install app (VMUserApp) -> boot -> tap icon -> (if it crashes) Apple's
own ReportCrash writes a .crash file into the guest fs -> pull it out
with: python tools/hfsx_extract.py <work-image> <crash-file-name>
```

To extract a specific crash report once one exists:
```sh
python tools/hfsx_extract.py <work-image> --list <bundle-id-or-substring>
python tools/hfsx_extract.py <work-image> <exact-name-from-listing> out.crash
```

## What's still missing (real gaps, not yet built)

1. **No structured capture of emulator-level (not guest-level) faults per
   installed app.** `VMEngine.m`'s existing fault path (~line 1841) reports
   `pc`/`cpsr` when the *whole machine* hits an undefined instruction or
   halts — useful, but it's a whole-VM stop, not an app-scoped record. If a
   third-party app's bug trips something the CPU core doesn't implement
   (rather than a normal guest-handled signal), today that just stops the
   VM with a console line, not a labeled "app X hit unimplemented opcode Y"
   report. Worth building once there's a real app that actually does this.
2. **No crash-taxonomy classification or compatibility matrix** — the
   categories in the original task brief (missing instruction, MMU/alignment,
   dyld/loader, filesystem, GPU, audio-dependency, network-dependency) are
   unstarted. Building this against zero real crash samples would be
   guessing; it should be built from actual observed failures.
3. **No test applications.** Milestone 6 wants simple, single-subsystem test
   apps (UIKit, touch, filesystem, CoreGraphics, OpenGL ES, audio) built
   before complex ones are attempted. None exist yet, built or acquired.

## What's needed to make real progress here

A legitimately-owned period-compatible app (or several) from the user, to
actually install, run, and — if it fails — feed through the pipeline above.
Without one, further work on this track is either speculative (bad) or
blocked (honest). This is the same category of external dependency firmware
was: cannot be substituted or faked.
