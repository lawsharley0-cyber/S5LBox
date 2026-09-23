# CPU benchmark results

Measured results only. Method: `docs/PERFORMANCE_BASELINE_PLAN.md`. Every row
below was produced by `cpubench` (compiled ARMv6 guest workloads through
`s5l8900_run()`, MMU on with 4 KiB pages and VA ≠ PA, device ticks live,
direct RAM writes on, 100 000-instruction chunks) and verified: the guest's
checksum equals the host-computed one, and all backends in a row end in the
same architectural state (CPU + 16 MiB of guest DRAM digest).

Host for every table in this file unless stated: Linux x86-64 cloud container,
Intel Xeon @ 2.80 GHz, 4 vCPU; GCC 13.3.0, CMake Release (`-O3`), no LTO.
These are **desktop numbers**; they are not phone numbers and not FPS.

## 1. Baseline: reference interpreter (`arm_step`), commit `7143034`

`cpubench --reps 3 --mode user,svc` — median M guest instructions/s.

| Workload | ARM User | ARM SVC | Thumb User | Thumb SVC |
|---|---:|---:|---:|---:|
| bignum | 52.55 | 40.52 | 54.39 | 41.53 |
| crc32 | 56.25 | 46.80 | 66.38 | 51.61 |
| sha1 | 56.15 | 46.20 | 57.70 | 42.95 |
| memops | 56.58 | 45.29 | 62.44 | 45.38 |
| sort | 53.44 | 44.51 | 58.46 | 44.14 |
| raster | 56.33 | 45.43 | 53.91 | 44.44 |
| calls | 45.90 | 36.77 | 53.71 | 44.23 |
| mmu | 24.40 | 21.13 | 30.58 | 26.15 |
| interp | 48.88 | 40.73 | 59.56 | 45.77 |
| vfp | 36.82 | 31.62 | — | — |
| svc | 35.12 | — | 37.57 | — |

SVC-mode rows are slower because the reference run loop batches device ticks
only in User mode (`interpreter_tick_batch_limit`): privileged code pays one
`s5l8900_tick()` per instruction.

## 2. Where the reference interpreter's host time goes

`gprof` flat profile (`-pg`, Release) over one full `cpubench --reps 1 --mode
user,svc` run (1.42 G `arm_step` calls). Self time by function, grouped.
`-pg` instrumentation inflates small, frequently called functions, so treat
the split as indicative, not exact.

| Group | Functions (self %) | Share |
|---|---|---:|
| Instruction interpretation | `arm_step` 29.8, `thumb_step` 13.4, `exec_data_processing` 5.3, `barrel_shift` 1.3, `arm_cond_passed` 1.2, `exec_block_transfer` 0.8, VFP (`f32_do`, `vfp_execute_inner`) 1.4, other 0.2 | ≈ 53 % |
| Device time (`s5l8900_tick` and the refresh graph) | `s5l8900_tick` 9.9, `s5l_vic_set_line` 2.7, GPIO-IC 2.6, buttons 0.7, GPIO 0.6, DMA/SPI/PMU/TV-out ≈ 1.8 | ≈ 18 % |
| Run loop | `s5l8900_run` 9.4 | ≈ 9 % |
| Memory and MMU | `bus_read` 6.3, `mem_r32_as` 3.4, `mem_w32_as` 1.8, `arm_mmu_translate` 1.5, `mem_r8_as` 0.5, other 0.9 | ≈ 14 % |
| Harness (host checksum, digest; outside timed region) | `wl_run`, `gb_run` | ≈ 3 % |

`s5l8900_tick` was called 713 M times for 1.42 G instructions. Consequences
for the engine design: removing per-instruction decode/dispatch addresses
roughly half of the time; the per-instruction tick in privileged mode and the
run-loop overhead are a further quarter, which an engine that honours the
exact batch limit in *all* modes (not only User) can also remove.
