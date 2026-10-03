# Building, testing and benchmarking on Windows 11

Everything in `core/` and `tools/` (the emulator core, the desktop harness
`bootkernel`, benchmarks and every test) builds natively on Windows 11 x64
with MSVC. No WSL, no Xcode. The iOS app (`app/*.m`) is built on a Mac; see
`MAC_VALIDATION.md`.

CI proves this on every push: `core-tests` → `build + test (windows-latest)`
(MSVC, Release) runs the same `ctest` suite as Linux and macOS.

## 1. Prerequisites

| Tool | Notes |
|---|---|
| Visual Studio 2022 (Community is fine) | workload **Desktop development with C++**; it includes MSVC x64, CMake and Ninja. Optional component *C++ Clang tools for Windows* for `clang-cl` |
| Git for Windows | the repository forces LF line endings through `.gitattributes`; nothing to configure |
| Python 3 | optional: research scripts and regenerating guest benchmark workloads |

Use an **"x64 Native Tools Command Prompt for VS 2022"** (or *Developer
PowerShell for VS 2022*) for the Ninja presets, so `cl.exe` is on `PATH`. The
`vs2022` preset works from any shell.

## 2. Clone

```bat
git clone https://github.com/lawsharley0-cyber/S5LBox.git
cd S5LBox
```

## 3. Configure and build

Presets are defined in `CMakePresets.json` (`cmake --list-presets`).

Visual Studio generator (multi-config):
```bat
cmake --preset vs2022
cmake --build --preset vs2022-release
```

Ninja + MSVC (fastest incremental loop; Developer prompt):
```bat
cmake --preset msvc-ninja-release
cmake --build --preset msvc-ninja-release
```

Ninja + clang-cl (Developer prompt with the LLVM component):
```bat
cmake --preset clang-cl-release
cmake --build --preset clang-cl-release
```

Without presets (equivalent to what CI runs):
```bat
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Optional features, in a separate build directory:
`-DS5LBOX_JIT=ON` (compiles the inactive translator and its tests; emitted code
only executes on arm64 hosts).

## 4. Test

```bat
ctest --preset vs2022-release
ctest --preset msvc-ninja-release -j 8
ctest --test-dir build -C Release --output-on-failure
```

A single test with output, e.g. the CPU interpreter suite:
```bat
ctest --preset vs2022-release -R arm_interp -V
```

## 5. Benchmark (Release builds only)

Synthetic interpreter loops (reference numbers in
`docs/PERFORMANCE_BASELINE_PLAN.md`):
```bat
build\vs2022\core\Release\insnbench.exe --insns 20000000 --reps 5
build\msvc-ninja-release\core\insnbench.exe --filter tick=run
```

Benchmarks are measurements, not tests: they print median/best/worst M
instructions per second and exit non-zero only when a run fails its end-state
check. Close other heavy programs, plug in the laptop, and compare only runs
from the same binary and session.

## 6. Clean and rebuild

```bat
cmake --build --preset msvc-ninja-release --target clean
cmake --build --preset msvc-ninja-release
```
Full reset (delete one build tree, then configure again):
```bat
rmdir /s /q build\msvc-ninja-release
cmake --preset msvc-ninja-release
cmake --build --preset msvc-ninja-release
```
PowerShell equivalent of the `rmdir`: `Remove-Item -Recurse -Force build\msvc-ninja-release`.

## 7. Running real firmware on Windows

`bootkernel.exe` boots the user's own iPhone OS 3.1.3 files exactly as on
Linux/macOS (see the README for the accepted SHA-256 digests and flags). Keep
firmware under the git-ignored `firmware\` directory; never commit it.

```bat
mkdir work
build\msvc-ninja-release\core\bootkernel.exe firmware\kernel.macho ^
  -d firmware\devicetree.bin -c "debug=0x8 serial=1 nand-enable-adm=0" ^
  --external-md firmware\rootfs.img work\rootfs-run01.img -R 128 -n 420000000
```

Windows will not relink an executable that a running boot holds open; the
`.gitignore` already reserves `build-verify/` and similar second trees for
that.

## 8. Profiling on Windows

Profile **Release** builds only (optionally add `/Zi` debug info:
`-DCMAKE_C_FLAGS_RELEASE="/O2 /Ob2 /DNDEBUG /Zi"` and link `/DEBUG`).

- Visual Studio: *Debug → Performance Profiler → CPU Usage*, target
  `insnbench.exe` or `bootkernel.exe ... --run-api --fast`.
- Windows Performance Recorder / Analyzer:
  `wpr -start CPU` … run … `wpr -stop trace.etl`, open in WPA, *CPU Usage
  (Sampled)* grouped by module/function.

Inspect generated assembly only for the hottest functions
(`/FAs` for a listing, or the Disassembly window).
