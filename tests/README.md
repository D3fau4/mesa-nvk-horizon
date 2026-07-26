# Phase 1 tests — running them on a Nintendo Switch

Ten standalone `.nro` homebrew apps, one per Phase 1 milestone item
(`docs/milestones.md`). Each prints one line per check and a final
machine-checkable verdict — `RESULT: PASS (n/n)` or `RESULT: FAIL (k/n)` —
to the console **and** to `sdmc:/horizon_gpu_tests/<name>.log`, so results
can be reported back as plain text (known-risks R2).

## Building

With devkitA64 installed (`$DEVKITPRO` set):

```sh
make            # or: scripts/build-switch.sh
```

Without a local toolchain, with Docker available:

```sh
scripts/build-switch.sh     # uses ghcr.io/d3fau4/nx-dev:latest
```

Outputs land in `build/*.nro`.

## Running on the console

1. Copy the ten `.nro` files to `sdmc:/switch/horizon_gpu_tests/`.
2. Launch them from hbmenu **in this order** (later tests assume the
   areas earlier tests validate):

   | # | Test | Verifies |
   |---|------|----------|
   | 1 | `t_init` | nv bring-up/teardown, GM20B fields, repeatability |
   | 2 | `t_alloc` | aligned allocation, overflow rejection |
   | 3 | `t_nvmap` | NvMap create/close, id/handle validity |
   | 4 | `t_va_reserve` | VA reserve/release, alignment, exhaustion |
   | 5 | `t_map` | fixed-VA map/unmap, kinds, cleared-VA invariant, big pages |
   | 6 | `t_channel` | channel create/destroy, syncpoint ids, Zcull, N channels |
   | 7 | `t_submit` | minimal submit executes; 2 submits in flight; R3/R10 probes |
   | 8 | `t_syncpt` | exactly one increment per submit |
   | 9 | `t_fence_wait` | wait-after-completion; timeout path timing |
   | 10 | `t_teardown` | full reverse teardown, zero leaks, twice per process |

3. Each test ends with "Press + to exit". The verdict is on screen and in
   `sdmc:/horizon_gpu_tests/<name>.log`.
4. Report back the ten log files (or just their `RESULT:` lines plus any
   `FAIL`/`note` lines). The `note` lines carry the measurements the
   design needs: the syncpoint value at channel creation (R5), the R3
   entry-flags outcome and the R10 syncpoint-wait outcome in `t_submit`,
   and the exhaustion behaviour in `t_va_reserve`.

Nothing here requires Mesa, a specific firmware beyond homebrew-capable
CFW, or network access. If a test hangs longer than ~30 s, hold the power
button, note which test it was, and report that too — every wait in the
suite is bounded, so a hang is itself a finding.

## Host-side unit tests (no console needed)

```sh
scripts/run-host-tests.sh
```

builds the pure-logic modules (alignment/overflow, VA interval set,
wrap-safe syncpoint math, command emitters) with the host compiler and
sanitizers. These run anywhere and are also what CI should run.
