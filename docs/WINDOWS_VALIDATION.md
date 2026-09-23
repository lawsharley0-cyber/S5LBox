# Windows validation

Windows 11 x64 is the primary development host for the core. This records
what has been proven to build, pass and perform on Windows, how, and what
still needs a local run. Build instructions: `WINDOWS_BUILD.md`.

## 1. Proven by CI (`core-tests` → `build + test (windows-latest)`)

Runner: Windows Server x64, Visual Studio generator, **MSVC**, Release
(`cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`, `cmake --build build
--config Release`, `ctest --test-dir build -C Release`). Every commit of the
CPU-engine work was built and the full `ctest` suite passed:

| Commit | What it added | core-tests run | Windows job |
|---|---|---|---|
| `554c587` | compiled guest workload suite, `cpubench` | 35920628756 | passed |
| `962aa8b` | cached interpreter, run-loop integration, fuzzer | 35924933491 | passed |
| `373d674` | LDM/STM/PUSH/POP fast path | 35925927756 | passed |
| `e06c8e7` | VA-keyed block map, translation purge, `test_ci_translation` | 35927346797 | passed |
| `a50b124` | threaded dispatch (switch on MSVC), `--ci-verify`, CPU engine A/B step | 35928628781 | passed |

The tests that matter most for the engine all run on this job:
`cached_interpreter_differential` (10,000 random ARM + 10,000 random Thumb
cases against the reference, MMU on, device pages, faults),
`cached_interpreter_translation`, and `guest_workloads` (every compiled
workload × ARM/Thumb × User/SVC × both backends × direct writes off/on/FIQ,
digest-compared). MSVC compiles the engine's portable path: the `switch`
dispatch, `__forceinline`, and the loop fallbacks for `clz`/`ctz`.

### Measured on the Windows runner (commit `a50b124`)

`cpubench --backend interp,cached --reps 1 --div 4 --mode user,svc`, from the
job log: **geomean 2.501× over 40 rows, `failures=0`** (every row reported the
host checksum and identical architectural state on both backends). Selected
rows (M guest instructions/s, reference → cached): bignum ARM SVC 36.7 →
142.1, sha1 ARM SVC 38.2 → 138.1, calls ARM User 40.3 → 89.5, mmu ARM SVC
28.2 → 61.6, vfp ARM User 35.4 → 41.3. One repetition on a shared runner:
indicative, not a benchmark of record (`BENCHMARK_RESULTS.md` §5).

## 2. Still to run on a local Windows 11 machine

None of this has been run outside CI. From a *Developer PowerShell for VS
2022* in a fresh clone:

```bat
cmake --preset vs2022
cmake --build --preset vs2022-release
ctest --preset vs2022-release

cmake --preset msvc-ninja-release
cmake --build --preset msvc-ninja-release
ctest --preset msvc-ninja-release

cmake --preset clang-cl-release
cmake --build --preset clang-cl-release
ctest --preset clang-cl-release
```

Expected: every test passes on all three. `clang-cl` compiles the engine's
computed-goto path (it defines `__clang__`); if it rejects the GNU extensions
used there, configure it with `-DCMAKE_C_FLAGS=-DS5LBOX_CI_SWITCH_DISPATCH`
and record that in `KNOWN_ISSUES.md`. clang-cl has not been tried.

Then the engine checks and a benchmark of record:

```bat
build\msvc-ninja-release\core\test_ci_diff.exe 200000 1
build\msvc-ninja-release\core\test_guest_workloads.exe
build\msvc-ninja-release\core\cpubench.exe --backend interp,cached --reps 5 --mode user,svc > cpubench-windows.txt
```

(Each preset builds under `build\<preset-name>`; the Visual Studio preset puts
executables under `build\vs2022\core\Release`.) Paste the `CPUBENCH-SUMMARY` line and the
per-row ratios into `BENCHMARK_RESULTS.md` with the CPU model, Windows build
and compiler version. `failures=0` is the correctness gate; the rates are
the measurement.

## 3. Firmware runs on Windows

`bootkernel` and `snapboot` build on Windows like every other tool. The
real-firmware A/B of the two backends is the same procedure as
`MAC_VALIDATION.md` §3 with `.exe` paths; firmware stays on your machine and
is never committed.

## 4. What Windows cannot do here

The iOS app (`app/*.m`, Xcode, signing, the device) needs a Mac:
`MAC_VALIDATION.md`. Windows builds and tests everything in `core/` and
`tools/`, which is where all CPU-engine work happens, so the Mac is needed
only at the end of a change, not for every iteration.
