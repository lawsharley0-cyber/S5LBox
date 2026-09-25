# Mac and device validation

What has been validated on Apple platforms without a Mac in hand, and the
exact steps that remain for the owner's Mac, firmware and iPhone. Nothing in
§3–§5 has been run by the CPU-engine work; the steps are written so that the
results can be pasted into `BENCHMARK_RESULTS.md` and `KNOWN_ISSUES.md`
unchanged.

The Windows side is in `WINDOWS_VALIDATION.md`; the two are independent: all
engine development and testing happens on Windows or Linux, and the Mac is
needed only for the Xcode/iOS build, signing, the device and Metal-era
profiling.

## 1. Already proven by CI on every push

| Workflow / job | Host | What it proves |
|---|---|---|
| `core-tests` → `build + test (macos-latest)` | Apple Silicon (arm64), Apple clang | the whole core, tools and `ctest` suite, including `cached_interpreter_differential`, `cached_interpreter_translation` and `guest_workloads`, on an ARM64 host |
| `core-tests` → `CPU engine A/B` step (same job) | arm64 | `cpubench` reference vs cached on arm64, with the same correctness exit code; ratios recorded in `BENCHMARK_RESULTS.md` §5 |
| `core-tests` → `jit (macos-14/15)` | arm64 | the JIT and signed-static configurations still build and test with the engine present |
| `ios-build` | Xcode, iphoneos arm64, unsigned | the app (`app/project.yml` via XcodeGen) compiles and links the engine: `core/src` is compiled whole into the app, so `arm_ci.c`/`arm_ci_decode.c` are in every app build |

The engine uses no executable memory, no `MAP_JIT`, no `mprotect(PROT_EXEC)`
and no private API; it is plain C11 with a `switch` fallback, so it is allowed
on a stock, non-jailbroken iPhone exactly as the reference interpreter is.
Apple clang builds it with computed-goto dispatch (the GCC/Clang path).

## 2. Build on the Mac

Core, tools and tests (Xcode command-line tools or full Xcode, CMake, Ninja):
```sh
cmake --preset ninja-release
cmake --build --preset ninja-release
ctest --preset ninja-release
```

The app (needs full Xcode and XcodeGen):
```sh
cd app
mkdir -p Generated
python3 ../tools/gen_a64_static.py Generated/a64_static_handlers.S
xcodegen generate
open NEON.xcodeproj
```
Select your team for signing in Xcode, then build and run on the device. The
`Info.plist`, entitlements and asset catalog in `app/` are unchanged by the
engine work.

## 3. Real-firmware A/B of the cached interpreter (desktop, Mac or Windows)

This is the one validation that can find an engine bug the fuzzer and the
compiled workloads cannot. It needs the owner's iPhone OS 3.1.3 firmware,
prepared as `README.md` → *Build & run* → *Boot the kernel* describes.

The engine runs only on `bootkernel --run-api` (the app's `s5l8900_run`
path). `snapboot` accepts `--cpu-backend` but steps `arm_step` directly, so
it is used below only to *print* saved states. Pick an instruction count `N`
that reaches SpringBoard on your setup.

1. Reference run with a snapshot at `N` (your usual boot arguments from the
   README, plus):
   ```sh
   ./build/core/bootkernel <kernelcache> <your usual options> --run-api \
       --cpu-backend interp --snapshot-at N ref.snap > ref.log 2>&1
   ```
2. Engine run, verify mode (slower; checks every cached block against RAM):
   ```sh
   ./build/core/bootkernel <same arguments> --run-api \
       --cpu-backend cached --ci-verify --snapshot-at N ci.snap > ci.log 2>&1
   ```
3. Print both machine states and dump both RAM images with `snapboot`, which
   takes the same setup arguments as `bootkernel` (see its header comment)
   and prints only machine-derived state:
   ```sh
   ./build/core/snapboot <same setup arguments> --restore ref.snap -n 0 \
       --dump-ram ref.ram > ref.txt
   ./build/core/snapboot <same setup arguments> --restore ci.snap -n 0 \
       --dump-ram ci.ram > ci.txt
   diff ref.txt ci.txt && cmp ref.ram ci.ram && echo IDENTICAL
   ```
   Device time is exact on both backends, so every register, device state
   and RAM byte must match. (The snapshot files themselves also carry host
   counters such as `tlb_hits`, which legitimately differ between backends;
   compare the printouts and RAM, not the raw files.)
4. In `ci.log`, the `cached interpreter:` lines must end with
   `verify: 0 stale block(s) caught`. Any difference in step 3 or a non-zero
   verify count is an engine bug: bisect by repeating with smaller `N`, then
   restore the last identical snapshot (`--restore`) on both backends to
   narrow the window.
5. The same lines show how much ran through the reference (`via reference`)
   and how often the engine stopped to single-step, which is where the next
   specialisation should go. A run without `--ci-verify` and with
   `--ci-stats` gives the speed: compare the `run api    :` rate lines.

## 4. On the device

1. Build and install as in §2.
2. Settings → Diagnostics → **CPU Execution Backend**: run the same boot once
   with *Reference Interpreter* and once with *Cached Interpreter*. The
   setting applies at the next start; the console prints
   `[vm] CPU execution backend: ...`.
3. Record for each: time to the lock screen, the app's instruction-rate and
   frame telemetry over the same interval, and whether the behaviour is
   identical (lock screen, slide to unlock, first-run dialog).
4. Instruments → Time Profiler on the device run with the cached interpreter
   shows whether `exec_block` / `arm_ci_run` or the device refresh dominate on
   the phone's CPU; the desktop split is in `BENCHMARK_RESULTS.md` §4.
5. Thermals: note whether the rate falls over a 10-minute run on both
   backends.

A faster CPU core is not a frame-rate claim: `hotpath.md` shows the steady
state is dominated by guest crypto and software rasterisation. Report what
you measure.

## 5. Apple-silicon-specific optimisation hooks (optional, not required)

Nothing Apple-specific is needed for correctness or for the current speed.
If device profiling (§4.4) shows dispatch dominating on Apple cores, the
candidate experiments are, in order:

- **Clang `musttail` tail-call threading** (iCube's I3 technique): each handler
  becomes a function tail-calling the next, which keeps guest registers in
  host registers across handlers. Clang-only; it would sit beside the current
  computed-goto path behind a build option, with the switch as the portable
  fallback, and be kept only if the device measures faster.
- **Larger host TLB / 16 KiB-aware sizing** for Apple cores' larger caches.
- **`__builtin_expect` tuning and PGO** (`-fprofile-use`) with a profile from
  the compiled workloads.

None of these is implemented. None needs a JIT.
