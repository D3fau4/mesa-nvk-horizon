# Tests — running them on a Nintendo Switch

Thirteen standalone `.nro` homebrew apps: ten for the Phase 1 milestone
items (`docs/milestones.md`), plus three that measure Phase 3 work —
`t_sysinfo` for `compat/sysconf.c`, and `t_threads` / `t_ostime` for the
Mesa code this port depends on. Each prints one line per check and a final
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

`t_threads` and `t_ostime` link the archives Mesa's own build produced,
so they need Mesa built first:

```sh
scripts/configure-mesa.sh && scripts/build-mesa.sh
```

Without that, both build paths skip those two with a message and produce
the other eleven — and the Makefile path also deletes any `.nro` a
previous build with Mesa present had left, so `build/` never mixes
artefacts from two builds. Nothing else here needs Mesa.

`$MESA_BUILD_DIR` selects where Mesa was built (default `build/mesa-probe`)
and is honoured by all four of `scripts/{configure,build}-mesa.sh`, the
Makefile and the Meson build; set it for the Mesa build and for the test
build alike. How the path is spelled does not matter —
`build/mesa-probe`, `build/mesa-probe/` and `./build/mesa-probe` are the
same directory to every consumer, including `make clean`.

`make clean` leaves that directory alone, so `make clean && make` still
produces all thirteen. It also leaves `build/toolchain` alone, since the
pinned Meson and Mesa's Python dependencies are installed there over the
network. Everything else under `build/`, including the Meson build
directory, is removed.

Building Mesa *after* configuring the Meson build directory is fine:
`scripts/build-horizon.sh` notices that the archives have appeared and
reconfigures, so it does not keep producing eleven.

`scripts/check-mesa-test-parity.sh` checks that the Makefile and the
Meson build still agree on which tests these two are, which archives
they link, and which defines and include paths they compile with. Run it
after touching either build system.

## Running on the console

1. Copy the `.nro` files to `sdmc:/switch/horizon_gpu_tests/`.
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
   | 11 | `t_sysinfo` | `compat/sysconf.c`: page size bounded from both sides, process memory, `_SC_NPROCESSORS_*` |
   | 12 | `t_threads` | Mesa's C11 threads shim: `mtx_timedlock` and `cnd_timedwait` expiry (both-sided timing on the calls that must expire; an upper bound only on the ones that must not wait), mutual exclusion, condvars, TSS, and the CPU count |
   | 13 | `t_ostime` | Mesa's `os_time.c`: monotonicity, resolution, rate against the ARM counter, `os_time_sleep` accuracy |

   Tests 11-13 use no `horizon_gpu` and need no nv services, so they are
   also the cheapest to run first when triaging a console.

3. Each test ends with "Press + to exit". The verdict is on screen and in
   `sdmc:/horizon_gpu_tests/<name>.log`.
4. Report back the log files (or just their `RESULT:` lines plus any
   `FAIL`/`note` lines). The `note` lines carry the measurements the
   design needs: the syncpoint value at channel creation (R5), the R3
   entry-flags outcome and the R10 syncpoint-wait outcome in `t_submit`,
   and the exhaustion behaviour in `t_va_reserve`. For `t_threads` and
   `t_ostime` the `note` lines are the measurement: the elapsed time each
   timed call actually took, the raw `InfoType_CoreMask`, and the clock
   resolution.

Beyond the build step above for tests 12 and 13, nothing here requires a
specific firmware beyond homebrew-capable CFW, or network access. If a test hangs longer than ~30 s, hold the power
button, note which test it was, and report that too — every wait in the
suite is bounded, so a hang is itself a finding.

## Host-side unit tests (no console needed)

```sh
scripts/run-host-tests.sh
```

builds the pure-logic modules (alignment/overflow, VA interval set,
wrap-safe syncpoint math, command emitters) with the host compiler and
sanitizers. These run anywhere and are also what CI should run.
