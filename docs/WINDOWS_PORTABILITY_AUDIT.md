# Windows 11 portability audit

Question answered here: can Windows 11 x64 be the primary development machine
for the emulator core, and what must stay on a Mac?

**Short answer: yes, and most of the work was already done before this
effort.** The core, the desktop harness, every tool written in C and every
test are portable C11 built by CMake, and `core-tests` has been building and
passing them with **MSVC on `windows-latest`** for every push (including
`4a8480f`, whose only red job is the Linux strict-warnings job; see
`CURRENT_ARCHITECTURE.md` §7). What was missing is documentation, presets and
rules for new code. Those are added by this effort; nothing Apple-side moves.

## 1. Build system (inspected)

| System | Present | Used for |
|---|---|---|
| CMake ≥ 3.16 | **yes** (`CMakeLists.txt`, `core/CMakeLists.txt`) | core library `emucore`, tools, 77 tests |
| Ninja | usable (CI uses the default generator) | optional, fastest local loop |
| Visual Studio projects / Meson / Make | no | — |
| XcodeGen `app/project.yml` → Xcode | yes | iOS app only |
| Custom scripts | `tools/*.py` (research), `tools/run23-cold-replay.ps1` | not needed to build |

No reorganisation into `core/{cpu,memory,…}` is needed: the repository already
separates `core/` (portable, zero platform dependencies) from `app/` (Apple
shell) and `tools/` (desktop harness). Moving 43 k lines to match a template
would break every path cited by ~30 k lines of documentation and CI for no
technical gain. New CPU-engine files go under `core/src/arm/`, new tests under
`core/tests/`, new benchmarks under `tools/` and `bench/`.

## 2. Platform-specific code, by file

Search terms: Foundation, UIKit, Metal, CoreAudio/AudioToolbox,
CoreFoundation, Objective-C, Mach, Darwin, pthreads, `mmap`, `sys/*`.

| Area | Files | Windows status |
|---|---|---|
| UIKit/Foundation UI | `app/Sources/*.m`, `*.h` that `#import` UIKit/Foundation | Apple-only, **never compiled on Windows** |
| Audio output | `VMAudioOutput.m` (AudioToolbox AudioQueue) | Apple-only; the ring buffer `VMAudioBuffer.c` is portable and tested on Windows |
| Graphics presentation | `VMFramebufferView.m` (CGImage on CALayer — **there is no Metal code in the repository**) | Apple-only; `VMFramePublication.c` (portable) is tested on Windows |
| JIT capability probe | `VMJitProbe.c` (`mmap`, `pthread_jit_write_protect_np`, guarded by `__APPLE__`/POSIX) | builds on Windows as the "no JIT" path; tested |
| Runtime JIT | `core/src/jit/*` | builds with `-DS5LBOX_JIT=ON`; emitted code executes only on arm64 |
| Static AArch64 engine | `core/src/arm/a64_static_engine.c`, `tools/a64_static.c`, generated `.S` | C parts build everywhere; native handlers are generated/linked only on arm64 hosts |
| Sockets | `tools/net_host.c` | Winsock (`ws2_32`) branch exists and is tested |
| Files | `tools/file_block.c`, `VMGuestInstall*.c`, `VMResumeCheckpoint.c` (`sys/stat.h`) | MSVC branches exist; tested with `/W4 /WX` |
| Timing | `insnbench.c` uses `clock_gettime`, falling back to C11 `timespec_get` on MSVC | fine |
| Threads | `test_vmaudiobuffer` links pthreads only on non-Windows | fine |

Nothing in `core/src` includes a platform header.

## 3. Host abstraction that already exists

The task suggests `HostClock/HostAudio/HostGraphics/HostInput/HostFilesystem`
interfaces. They already exist as plain C callbacks and need no C++ layer:

| Suggested interface | Existing seam |
|---|---|
| HostClock | `s5l8900_t::active_host_now` (+ WFI pacing via `bus.wait_for_interrupt`) |
| HostAudio | I²S host sink callback (`s5l_audio_ready_fn`, `audio_ctx`) |
| HostGraphics | frontend reads the scanout; `VMFramePublication.c` decides publication |
| HostInput | `s5l_buttons_set`, the MTZ2 touch queue (`VMTouchQueue.c`) |
| HostFilesystem | `vm_block_t` (`core/include/vm_block.h`), `tools/file_block.c` |
| Guest memory | `arm_bus_t` (read/write/`host_ram`/`host_ram_write`) |

A headless Windows "platform" is therefore just the existing test harnesses and
`bootkernel` (which runs real firmware on Windows — the 2026-09-18 profiling in
`PERFORMANCE_PLAN.md` §8 was done that way).

## 4. What can be developed and validated on Windows

Everything below `s5l8900_run()`: the CPU (interpreter and the new cached
interpreter), MMU/TLB, memory, exceptions, interrupts, timing, device models,
firmware parsing, snapshots, the disk bridges, and **real-firmware boots and
restored-checkpoint A/B runs with `bootkernel`** using the user's own firmware.

What cannot: Objective-C, UIKit, AudioToolbox, Xcode signing, iOS deployment,
arm64 native-code execution (static engine, JIT), Apple-Silicon performance,
thermals. See `MAC_VALIDATION.md`.

## 5. Toolchains

| Toolchain | Status |
|---|---|
| Visual Studio 2022 (MSVC 19.3x+) x64 | verified by CI on every push |
| Clang/LLVM on Windows (`clang-cl` or `clang` with MSVC STL/CRT) | expected to work (same C11); not yet in CI |
| MinGW-w64 GCC | historically used by the maintainers (see `insnbench.c` timer notes); not in CI |
| WSL | **not required** |

Python 3 is optional (research scripts, workload regeneration).

## 6. Rules for new portable code (the cached interpreter must follow them)

1. C11 only. **No VLAs** (MSVC's C compiler has none), no `__int128`, no
   statement expressions, no GNU `case a ... b` ranges.
2. Every compiler extension behind a macro with a portable fallback:
   `likely/unlikely`, `always_inline`/`__forceinline`, `noinline`, computed
   `goto` (GCC/Clang only → `switch` fallback), `[[clang::musttail]]`.
   The fallback must be the *tested default* path on MSVC, and CI must build
   both.
3. No `<stdatomic.h>` in the core (MSVC C support is experimental). The CPU
   core is single-threaded by design (`VMEngine.h`).
4. Warning-clean on GCC/Clang `-Wall -Wextra -Werror` (CI) and on MSVC `/W4`
   for new files.
5. No host-endianness assumptions beyond the existing documented
   little-endian contract; use `memcpy` for unaligned host loads.
6. ARM64-specific code is an optimisation only: it must compile out to the
   portable path, and the differential tests must pass with it disabled.

## 7. Gaps closed by this effort

- `CMakePresets.json` with MSVC, clang-cl and Ninja configurations.
- `docs/WINDOWS_BUILD.md` with exact commands (clone/configure/build/test/
  benchmark/clean/rebuild).
- The red strict-warnings job (use-after-free in `insnbench --profile`) is
  fixed by removing the fabricated profiler output.
- New engine code follows §6 and is exercised by `ctest` on all three CI OSes.
