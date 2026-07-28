# STATUS

**Last updated:** 2026-07-27
**Branch:** `claude/mesa-horizon-phase3-closeout-2ahzsq`

---

## Current phase

**Phase 3 — minimal Horizon support in Mesa. Every milestone item now
has a disposition with evidence behind it; the phase's build criterion
is met. Two hardware measurements are owed and named below.**

**Mesa configures for `horizon` (`meson setup` exits 0) and its
non-driver core builds: 379 of 379 edges, zero failures, all ten static
libraries archived.** That is `docs/milestones.md`'s Phase 3 exit
criterion "Mesa configures for `horizon` and builds the non-driver
core", as a cross build (X).

Item by item: **1** OS detection (patch 0007 + 0012), **2** Meson
`host_machine.system()` — **no patch, and the reason is measured**,
**3** newlib/libnx gaps (patches 0001–0006), **4** threads (patch 0003 +
0011, decision recorded), **5** timers/clocks (patch 0008), **6**
physical memory / page size (`compat/sysconf.c` + patches 0009–0010),
**7** endianness (patch 0004), **8** build ID (answered in Phase 2, no
patch). The series stands at **twelve patches**, every one formulated as
a property of the C library or the compiler rather than as an OS name.

**What is owed:** `t_threads` and `t_ostime` have now been run **on an
emulator, not on a console** (2026-07-28). `t_ostime` passed 27/27;
`t_threads` passed 70 checks and then stopped inside
`util_get_cpu_caps()` without writing a verdict. Items 4 and 5 stay
cross-build results as far as hardware is concerned — an emulator answers
for its own implementation of libnx, not for the console's — but the
emulator run is real evidence and is written up in "First run of
`t_threads` and `t_ostime` — emulator" below, together with the open
failure and the staged probe added to locate it.

**Codex reviewed PR #4 twice.** The first round left 8 findings — 7 real
and fixed, 1 refuted with the generated `build.ninja` in hand — mostly
about the two new tests being able to *report* a failure rather than
hang on it. The second round reviewed that output too and left **18
findings, 17 of them real**: a `make clean` that still lost the Mesa
build for any non-canonical spelling of the path, a Meson build that
could never notice Mesa appearing after it was configured, bounds loose
enough to pass for the failure they name, unchecked mutex returns in the
test's own workers, and six places where the tree described itself more
strongly than the code supports. Both rounds are written up below; the
second one also corrects the first one's write-up of the refuted P1.

Two things happened before any Mesa work was possible, both recorded
below: a defect in **our own** cross file was making six of Mesa's
configure checks return false answers, and `mesa-patches/` had no
mechanics at all (it held a `.gitkeep`).

`compat/` has its **first content**: `sysconf`, which devkitA64's newlib
declares and does not define. That is the one door `CLAUDE.md` leaves
open, and `scripts/check-layering.sh` now polices it.

**Closed 2026-07-27:** all eleven `.nro` were run on a real Switch, in
**both applet and full/game mode**. Ten of eleven PASS in both; Phase
1's two hardware exit criteria go back to ✅ on the current code. See
"Hardware run of all eleven `.nro`" below.

`compat/sysconf.c`'s mode-aware argument is confirmed by a wide margin —
394 MiB of process memory in applet mode against 3189 MiB in full/game,
an 8.1× difference on the same console — so a hardcoded constant would
have been badly wrong, not approximately right.

`t_sysinfo` failed one check on the first run — its granularity probe
asked for the wrong memory region, which the test correctly reported as
"not a statement about the page size". Fixed and re-run the same day:
**PASS 21/21 in both modes**, so the page size is now bounded from both
sides by measurement. **Nothing is owed on hardware.**

---

## Codex PR review, PR #4 (2026-07-27) — 8 findings, 7 real

`chatgpt-codex-connector` reviewed PR #4 at `1b5e0bf` and left **2 × P1
and 6 × P2**. Each was checked against the code and against a command
before anything was changed. **Seven held up. One did not**, and it was a
P1 — the first finding in these rounds that did not survive being looked
at, so it is written up in as much detail as the ones that did.

Nothing in `horizon/` and nothing in `mesa-patches/` was touched: every
fix is in this repository's build files or in the two new tests.

| # | Finding | Disposition |
|---|---|---|
| P1 | The Mesa archives reach the link only through `link_args`, so Ninja does not treat them as inputs and a rebuilt Mesa would not relink the tests | **Refuted, with the generated file** — and the write-up of that refutation was itself defective; see the correction below. Meson already records them: `guess_external_link_dependencies()` (`ninjabackend.py:3636` in meson 1.11.2) appends any link argument that `os.path.isabs() and is_library() and os.path.isfile()`, and `build/meson/build.ninja:287` reads `build t_threads.elf: c_LINKER … \| …/libmesa_util_c11.a …/libmesa_util.a …`. Measured: `touch build/mesa-probe/src/util/libmesa_util.a` then `scripts/build-horizon.sh` → `[1/4] Linking target t_ostime.elf … [3/4] Linking target t_threads.elf`. The Makefile path already had the archives as explicit prerequisites (`$(MESA_TESTS:%=$(BUILD)/%.elf): $(MESA_LIBS)`). `link_depends` was added anyway, one line |
| P1 | A build that skips tests 12 and 13 leaves the previous build's `.nro` for `scripts/package-horizon.sh` to ship | **Real on the Makefile path.** Measured: build 13 `.nro`, move `build/mesa-probe` aside, `make` → the two stale `.nro` were still there, and `package-horizon.sh` copies `"$SRC"/*.nro` unconditionally into a manifest whose entire job is attributing an artefact to one build. Fixed: the skip branch now collects `$(STALE_MESA)` and a `prune-stale` prerequisite of `all` removes the `.nro`, `.elf`, `.nacp` and `.t.o`. **Not real on the Meson path**, and the reason is measured: Meson runs `ninja -t restat && ninja -t cleandead` itself after regenerating `build.ninja` (`ninjabackend.py:705`, for ninja ≥ 1.12 or ≥ 1.10 without dyndeps — this build has ninja 1.11.1 and no dyndeps), so after the same experiment there were 11 `.nro` on disk *before* the build step ran. A `cleandead` added to `build-horizon.sh` cleaned 0 files and was removed again; the condition is recorded in that script so a future ninja or dyndeps change has something to check against |
| P1 | `thrd_join` on the `mtx_timedlock` worker, and the main thread's own `cnd_timedwait`, are unbounded waits — the exact failure the section exists to catch would hang the test instead of failing it | **Real, and the sharpest finding of the round.** A `mtx_timedlock` that never returns is *the* thing test 12 was written for, and the test would have hung on it with the verdict unwritten. Fixed with two different mechanisms because the two cases are not the same: the worker is watched by the main thread through an `atomic_int done` polled against `armGetSystemTick` for `WATCHDOG_MS` (2000 ms, above the 800 ms upper bound so a merely slow implementation is still reported as slow), and the mutex is released **before** the join, which is what frees a worker stuck in a lock that ignored its deadline; the main thread's `cnd_timedwait` gets a watchdog thread that sets the predicate and broadcasts. The failing `t_check` is written *before* either call that could still block, and `testfw` fflushes each line to sdmc, so even a run that hangs anyway leaves a log naming the section |
| P2 | `make clean` deletes `build/mesa-probe`, so `make clean && make` silently drops the two Mesa tests | **Real.** `clean` was `rm -rf build` and `$(MESA_BUILD)` lives there. Fixed by filtering. **Wider than reported, and found by running it:** the same `rm -rf` also removed `build/toolchain`, i.e. the pinned Meson install and Mesa's Python generator deps, both installed *from the network* — which `CLAUDE.md` names as the thing that may not be reachable. `make clean` uninstalling the build system is not what anyone types it for. Now `clean` keeps `$(MESA_BUILD)`, its `.crossid` stamp and `$(BUILD)/toolchain`, and removes the parts of `toolchain/` this Makefile does produce (`lib/`, `compat-obj/`) explicitly |
| P2 | `deadline_in_ms()` ignores `timespec_get`'s return, and C11 leaves `*ts` untouched on failure | **Real, and it is the same defect this session documented in `os_time_get_nano()`** — writing the test that reports Mesa's bug with Mesa's bug in it. Now returns `bool`; every caller reports "no deadline could be built" and does **not** call the timed function, so a clock failure cannot be misread as a timeout failure |
| P2 | `mtx_init`/`cnd_init`/`tss_create` failures were recorded and then the section carried on with an invalid object | **Real.** Fixed by giving each section its own function that returns as soon as its own setup fails — the object is also created and destroyed inside it, so ownership is one function's rather than `run_test`'s. `section_cpu_count` depends on none of them and runs regardless, which matters: it is the hardware evidence for patch 0012 |
| P2 | `t_ostime`'s 2000 reads can finish inside one tick of the 1 ms resolution the test itself accepts, so `distinct == 1` would report a working clock as stopped | **Real.** The loop now also runs until the ARM system counter — not the clock under test — says `SAMPLE_MIN_MS` (5 ms, four accepted ticks) has passed, with a `SAMPLES_MAX` ceiling so a stopped clock ends the loop rather than the console. "Did the clock advance" is asserted only once that interval is established; if the ceiling is hit first, that is the *reference* counter failing and the note says so |
| P2 | `meson.build` and the `Makefile` hardcode `build/mesa-probe` while `scripts/{configure,build}-mesa.sh` honour `$MESA_BUILD_DIR` | **Real.** Fixed in one place: `scripts/toolchain-env.sh` now defines the default, both Mesa scripts inherit it, the Makefile reads `$(or $(MESA_BUILD_DIR),build/mesa-probe)`, `scripts/build-switch.sh` forwards it into the container (which sees only the variables named on `docker run`), and `configure-horizon.sh` passes `-Dmesa_build_dir` — an option and not an environment read, because Meson runs *inside* that container |

### What the round says about the verification, not the code

- **Three of the seven are the same mistake:** a test that measures a
  timeout was written as if the implementation would always return. The
  lower and upper bounds were carefully both-sided and then reached
  through an unbounded wait. Bounding the *value* and not the *wait* is
  half a measurement.
- **Two findings were only partly right, and finding the rest needed
  running the command, not reading the diff.** `make clean` losing the
  pinned Meson install, and the Meson half of the stale-`.nro` finding
  being already handled, both came out of executing the scenario.
- **The refuted P1 was refuted with a file, not an argument.** The rule
  this project applies to Mesa patches — no change without a measured
  defect — applies to review findings too. The one-line `link_depends`
  that went in anyway is documented as hardening of a Meson heuristic,
  not as a fix.

**Correction, made in the second round (2026-07-27).** That row
originally justified the one-line change by saying the inference "is
conditional on the file existing at configure time" — which asserts a
residual defect inside a row labelled *Refuted*. Codex called out the
contradiction and was right about it. The disposition stands, and the
reasoning does not: the conditionality creates no hole, because
`meson.build` builds these two tests **only** when the archives exist,
so any configuration in which the edge could be missing is one in which
there is no edge. `link_depends` is defence against a future Meson
changing an undocumented inference, and nothing more. The genuine defect
in that neighbourhood is a different one — `fs.exists()` never being
asked again — and it is finding 8 of the second round.

### A defect this round introduced and closed inside itself

Adding `meson.options` in the same commit that passes `-Dmesa_build_dir`
broke `scripts/configure-horizon.sh` on any existing build directory:

```
$ scripts/configure-horizon.sh
reconfiguring build/meson
ERROR: Unknown option: "mesa_build_dir".
```

`setup --reconfigure` validates every `-D` against the options recorded
at the first configure, *before* re-reading the file that declares them.
This is the same class as the P1 from the previous round (machine files
are only read on a first configure) and it is fixed the same way:
`horizon_setup_mode` / `horizon_cross_id` / `horizon_record_cross_id`
now take extra identity files, `configure-horizon.sh` passes
`meson.options`, and a change to it wipes that build directory.
`configure-mesa.sh` deliberately does **not** pass it — our options do
not affect Mesa's build directory, and wiping that one costs minutes.

### Verification after the fixes

Every command below was run from the repository root; the toolchain is
`ghcr.io/d3fau4/nx-dev:latest` (no local devkitA64). All of it is **cross
build (X)**. Nothing here is a hardware result.

| Check | Command | Result |
|---|---|---|
| Rebuilt Mesa relinks the tests | `touch build/mesa-probe/src/util/libmesa_util.a && scripts/build-horizon.sh` | `[1/4] Linking target t_ostime.elf … [4/4]` — 2 tests relinked |
| `make clean` keeps what it does not own | `scripts/build-switch.sh clean && ls build/` | `mesa-probe  mesa-probe.crossid  toolchain` |
| …and a clean build still makes 13 | `scripts/build-switch.sh` | 13 `elf2nro` invocations, 13 `.nro` |
| Stale pruning, Makefile path | move `build/mesa-probe` aside, `scripts/build-switch.sh` | `removing stale Mesa test artefacts: …t_threads.nro …t_ostime.nro …` then **11** `.nro` |
| Stale pruning, Meson path | same, then `configure-horizon.sh` | **11** `.nro` on disk before `build-horizon.sh` ran; Meson had already cleaned |
| `MESA_BUILD_DIR`, Makefile path | `MESA_BUILD_DIR=build/mesa-alt scripts/build-switch.sh` | links `build/mesa-alt/src/…/libmesa_util*.a`, 13 `.nro` |
| `MESA_BUILD_DIR`, Meson path | `MESA_BUILD_DIR=build/mesa-alt scripts/configure-horizon.sh` | `tests : 13`, `mesa_build_dir: build/mesa-alt` |
| Both tests still compile `-Wall -Wextra -Werror` | `scripts/build-switch.sh` | clean, no diagnostics |
| Patch series on a reset `mesa/`, ×2 | `scripts/apply-mesa-patches.sh` | applies 12 (`mesa at bf8dbcd`); second run `all 12 patches already applied; nothing to do` |
| `.nro` parity, Makefile vs Meson | `stat -c%s` over both directories | **13/13 identical sizes** |
| Host unit tests | `scripts/run-host-tests.sh` | **103/103** |
| Gates | `check-layering.sh`, `check-no-abs-paths.sh`, `check-rust-target.sh` | all OK |

`scripts/check-no-abs-paths.sh` gained `meson.options` as a target in the
same commit: it is a build input, and it now carries a *default
directory*, which is the shape a machine-specific path takes when it
arrives by accident.

### Still owed after this round — unchanged

`t_threads` and `t_ostime` have still never run on a console. Everything
above makes them better at reporting what they find; none of it is a
measurement of Horizon. The watchdogs in particular have never fired,
because the code containing them has never executed.

---

## First run of `t_threads` and `t_ostime` — emulator (2026-07-28)

**Class: E (emulator).** The owner ran both `.nro` (`3f97f5d2…`,
`ff999f01…`) on a Nintendo Switch **emulator**, not on a console. That is
a fourth class alongside host (H), cross (X) and hardware (HW), and it is
kept separate on purpose: an emulator answers for its own implementation
of libnx's syscalls, not for the console's. Nothing below is a hardware
result and none of it closes items 4 or 5.

### `t_ostime` — PASS 27/27

Every check passed. The measurements, which are the point of the test:

| Quantity | Measured |
|---|---|
| `timespec_get(TIME_MONOTONIC)` | returns `TIME_MONOTONIC`; the clock answers |
| `os_time_get_nano` monotonicity | 22915 samples over 5 ms, **22915 distinct values, 0 backwards** |
| Resolution | **52 ns** — far finer than the 1 ms the test would have accepted |
| Rate against the ARM system counter | 100164688 ns measured against 100242500 ns reference over one 100 ms sleep — **0.08 % apart**, against a 10 % tolerance |
| `os_time_sleep(50000 us)` | 50509 us |
| `os_time_sleep(0)` | 60 us |
| `os_time_nanosleep_until(+50 ms)` | 50335 us |
| Past deadline | 185 us |

So `os_time.c` behaves on this emulator, and the unchecked
`timespec_get` inside `os_time_get_nano()` is not returning stack
garbage here. The tightened bounds from the second review round all held
with room to spare — the sleep landed 1 % long against a −25 % floor.

**One thing the run recorded that no check asserts.** The first note
reads `ts = 1785229380 s + 81102164 ns`, which is **2026-07-28 09:03:00
UTC** — wall-clock time, not time since boot. `CLOCK_MONOTONIC` here is
the real-time clock, so `os_time_get_nano()` would step if the system
clock were adjusted. Monotonicity held across the 5 ms sample window and
across the 100 ms reference sleep, which is all this test claims; a clock
that is monotonic *only while nobody sets the date* is a different
property from the one Vulkan timeouts want, and Phase 4 needs to know
which one it has. Recorded as an open question, not a failure.

### `t_threads` — did not finish

70 checks, **all `ok`**, and then the log stops. There is no `RESULT:`
line, so the process did not reach the end of `run_test`. The last line
written is

```
  ok   sysconf(_SC_NPROCESSORS_CONF) = 4 answers as _ONLN (4), the case label it shares
```

and the next statement in the file is `caps = util_get_cpu_caps();`.
`testfw` fflushes every line to sdmc, and `t_ostime`'s log from the same
run is complete, so the missing tail was never written rather than lost
in writeback. **The process stopped inside `util_get_cpu_caps()`.**

Everything the test set out to measure about Mesa's C11 shim passed
first:

| | |
|---|---|
| `thrd_create` / `thrd_join` / `u_thread_create` | ok, return value carried |
| `call_once` across 4 threads | body ran exactly once |
| Shared counter, 4 × 20000 under a mutex | **80000, no update lost** |
| `mtx_timedlock` on a held mutex | `thrd_timedout` after **200 ms** for a 200 ms deadline |
| `mtx_timedlock` on a free mutex | `thrd_success`, 0 ms |
| `cnd_signal` / `cnd_broadcast` | all 4 waiters woke, 0–1 ms |
| `cnd_timedwait` with nobody signalling | `thrd_timedout` after **200 ms**, watchdog never fired |
| TSS | per-thread values isolated, destructor ran for all 4 |
| `InfoType_CoreMask` | **0xf — four cores**, `sysconf` agrees on both names |

That is milestone item 4's whole question answered on this emulator: the
polling `mtx_timedlock` neither returns at once nor hangs, and it lands
on its deadline rather than near it.

### What is known about the failure, and what is not

`util_get_cpu_caps()` is an inline function in `u_cpu_detect.h`; it calls
`call_once(&_util_cpu_caps_state.once_flag, _util_cpu_detect_once)`. On
aarch64/Horizon that function does, in order: two `sysconf` calls, an
assignment for NEON, `check_cpu_caps_override()`, `check_max_vector_bits()`
(an assignment), `get_cpu_topology()` (a `memset` on this arch), and
`debug_get_option_dump_cpu()`.

Most of that is already known to work in this very run. The two `sysconf`
calls are the ones logged two lines above. `call_once` passed its own
section. What has **never** run before this point is the option lookup:
`debug_get_option_cached` → `os_get_option_cached`, which on first use
takes a statically initialised `simple_mtx_t` — whose lock goes through a
`thread_local` in `u_call_once.c` on this platform, because
`UTIL_FUTEX_SUPPORTED` is 0 here — then builds a hash table with
`ralloc` and registers an `atexit` handler. `nm` on the linked ELF
confirms all of it is in the binary (`util_call_once_data_slow`,
`_simple_mtx_plain_init_once`, `os_get_option_cached`,
`_mesa_hash_table_create`).

Two hypotheses were checked and **eliminated** rather than left as
suspicion:

- **`getenv` with a null `environ`.** newlib's `_findenv_r` loads
  `environ` and branches out on zero before dereferencing it
  (`ldr x20, [x22]` / `cbz x20, …`, disassembled from
  `libc_a-getenv_r.o` in the pinned image). It returns NULL; it does not
  fault.
- **A `once_flag` ABI mismatch between the test and `libmesa_util.a`.**
  `u_cpu_detect.h` includes `util/u_thread.h`, which includes
  `c11/threads.h`, and the test includes the same header. One type.

Not established: whether the process crashed or hung, which the log
cannot say and the owner can. A hang points at the mutex or `call_once`;
a fault points at an access. Also not established: whether this is
emulator-specific. The whole path is libc and libnx — no GPU, no `nv`
services — which makes an emulator artefact less likely than for the
Phase 1 tests, and does not exclude one.

### What changed in response

`section_cpu_count` now reaches that call in four named stages, each
announced before it is entered, so the next log names the step instead of
the section: plain `getenv`, then `os_get_option`, then
`os_get_option_cached` (the hash table, `ralloc`, `simple_mtx` and
`atexit` path), then `util_get_cpu_caps()`. A provisional tally is
printed before the first of them, because a call that does not return
takes the `RESULT:` line with it and a run that produced 70 results
should not be unreadable on account of the 71st.

No check was removed or weakened, and nothing was worked around. New
`t_threads.nro`: `00d15baa9beca514c39cc566ce60c4b4bc438ef54b8b0507f2b68296704289ff`.
`t_ostime` is unchanged.

---

## Codex PR review, PR #4, second round (2026-07-27) — 18 findings, 17 real

`chatgpt-codex-connector` reviewed `e0980e2..b328813` — that is, the
first review round's own output plus the Phase 3 closeout — and left
**18 findings**. Each was checked against the code and against a command
before anything was changed. **Seventeen held up.** The one that did not
is the one Codex itself marked as needing confirmation against the
pinned checkout.

Nothing in `horizon/` and nothing in `mesa-patches/` was touched. Six of
the eighteen are corrections to what the tree *says* about itself rather
than to what it does, and they are treated as defects on the same
footing: a comment that overstates what a check proves is how a false
claim gets into a hardware report.

| # | Finding | Disposition |
|---|---|---|
| 1 | `make clean` still deletes the Mesa build for any non-canonical `MESA_BUILD_DIR`, because `$(filter-out)` is a literal comparison | **Real, reproduced.** `make -n clean` with `build/mesa-probe/`, `./build/mesa-probe` and `build//mesa-probe` all put `build/mesa-probe` and its `.crossid` into the `rm -rf`. The verification behind the original fix only used `build/mesa-alt`, which happens to be canonical — the check passed because the input was well-behaved. Both sides now go through `$(abspath)`, which normalises without requiring the path to exist; `scripts/toolchain-env.sh` normalises for the script path too. After: all four spellings keep it, and `build/mesa-alt` still correctly does not keep `build/mesa-probe` |
| 2 | `clean`'s stated contract ("removes what this Makefile produces, and only that") contradicts what it does — it deletes `build/meson`, which Meson and `configure-horizon.sh` produce | **Real.** The behaviour is right and the sentence was wrong. The rule is now stated as it actually is: anything cheap to regenerate from this tree is removed, anything costing minutes of compilation or a network fetch is kept. `build/meson` is removed because it reconfigures in seconds and a stale cross build directory is a hazard; `$(MESA_BUILD)` and `$(BUILD)/toolchain` are kept because they are not |
| 3 | The processor-count section is circular and is described as a cross-check: `sysconf(_SC_NPROCESSORS_ONLN)` **is** the popcount of the `svcGetInfo` it is compared against, and `conf == onln` shares a `case` label | **Real, and the wording was the defect.** The checks stay — they catch the wiring breaking (query failure inside `sysconf`, a `_SC_` name reaching the `EINVAL` default, a later edit reporting something else) — but they are now labelled wiring checks in the test, in `compat/sysconf.c` and here, not evidence of a core count. The independent measurements in that section are the raw mask, its bounds, and `caps->nr_cpus`, which reached 1 regardless of the mask before patch 0012 |
| 4 | `os_time_get_absolute_timeout((uint64_t)INT64_MAX)` asserts a defined result for a signed-overflow path that `-O2` may fold away | **Not real against the pinned Mesa**, and Codex flagged it as needing confirmation there. Mesa 26.1.5's `os_time.c` does not add first: it uses `util_add_overflow(int64_t, time, timeout, &abs_timeout)`, which is `__builtin_add_overflow` here — `HAVE___BUILTIN_ADD_OVERFLOW` is in this build's compile args (`build/mesa-probe/build.ninja`) — and computes in infinite precision before deciding. The shape the finding describes (`abs_timeout = time + timeout; if (abs_timeout < time)`) is older upstream. Recorded in the test, with the guard's own case added as a second check, so a Mesa bump that restores it fails here rather than passing by accident |
| 5 | The resolution check's message says the opposite of the check: `min_step <= 1000000` printed as "clock resolution is at least 1 ms" | **Real.** That line is read by someone looking at a FAIL in a console log with no source to hand. Now "1 ms or finer" |
| 6 | "Every timed check is bounded from both sides" is not true of the code — the `PROMPT_MAX_MS` checks, `os_time_sleep(0)`, a past deadline and the `drift` check are one-sided | **Real.** Each is defensible on its own; the blanket claim was not. The header now separates the two: a call that must *expire* is bounded from both sides, a call that must *not wait* has no lower bound to assert and is bounded above only |
| 7 | Several bounds are loose enough to pass for the failure they name: `SLEEP_MIN_US = SLEEP_US / 2` accepts a 2× unit error, `os_time_sleep(0)` accepts 49 ms against a 50 ms sleep, `nanosleep_until(+50 ms)` accepts 25–200 ms — and `t_threads` uses −25 %/×4 for the same class of measurement with no explanation of the disagreement | **Real, and the sharpest of the documentation-shaped findings.** One convention now, stated once: −25 % below, ×4 above, in both files. The "must not wait" bound is 10 ms — a fifth of `SLEEP_US`, so it can no longer be satisfied by a call that slept the full requested amount, which is the one thing it existed to distinguish |
| 8 | Meson and Make detect Mesa's archives at different times and only one recovers: `fs.exists()` runs once at configure and is baked into `build.ninja`, so building Mesa afterwards leaves that path producing 11 `.nro` indefinitely | **Real, and measured before and after.** With the archives hidden, `configure-horizon.sh`, then the archives restored: the `build-horizon.sh` on the branch printed `11 .nro` on two consecutive runs. `configure-horizon.sh` now records what `fs.exists()` decided beside the build directory and `build-horizon.sh` compares it against the state on disk — same scenario: `Mesa archives are present, build/meson was configured with them absent; reconfiguring`, then `13 .nro`. Both directions verified |
| 9 | Four facts are restated in two build systems with a comment as the only enforcement, and the `.nro`-size parity check would not notice a define present on one path and missing on the other | **Real.** The duplication stays — each build system has to be readable on its own, and the Makefile is the hardware-verified path — but divergence now fails: `scripts/check-mesa-test-parity.sh` compares the test names, the two archive paths, the defines, the Mesa include directories and the default build directory across `Makefile`, `meson.build`, `meson.options` and `scripts/toolchain-env.sh`. Verified by breaking each of the five in turn plus the gate's own extraction — six deliberate breakages, six exit 1 |
| 10 | Link order is stated on one path and left to inference on the other | **Real, and the answer needed measuring rather than asserting.** The Makefile needs the archives *before* `-lhorizon_compat -lnx` because it is a plain left-to-right link. The Meson path does the opposite — a dependency's `link_args` go last — and links anyway because Meson wraps the whole set in `-Wl,--start-group … --end-group`, so the linker rescans. Read out of the generated `build.ninja` and recorded beside `idep_mesa_core`; the two paths are not expected to emit the same link line |
| 11 | `horizon_cross_id` hashes with no separator and discards `cat`'s exit status, so a renamed identity file degrades the hash back to the cross files alone — silently | **Real, and it fails open, which is the worst direction for a staleness check.** Every input is now verified to exist *before* the pipeline (a pipeline's status is `sha256sum`'s), and each file's path is hashed with its contents, so a line moved between two hashed files is no longer invisible |
| 12 | The extra-identity-file mechanism reproduces the coupling it was added to fix: two call sites must be handed the same list, with a comment as the only enforcement | **Real.** The list is given once, to `horizon_setup_mode`, and `horizon_record_cross_id` takes no arguments |
| 13 | `compat/sysconf.c` merges `_SC_NPROCESSORS_CONF` into `_ONLN` on an argued, unmeasured premise; `svcGetSystemInfo` is dismissed in prose | **Real, and the measurement was available.** libnx's `SystemInfoType` accepts exactly `TotalPhysicalMemorySize`, `UsedPhysicalMemorySize` and `InitialProcessIdRange` (`switch/kernel/svc.h:222-225`, read out of the pinned image). None is a processor count. The cost of sharing the case — a caller cannot distinguish "the SoC has four, you may use three" — is now stated rather than implied, as is why the SoC constant lives in the test and not in the C library function |
| 14 | Mutex operations go unchecked in the worker threads, and `section_mutex_timed`'s "the mutex is not left held either way" rests on one of them | **Real, and `CLAUDE.md` requires the check.** Every `mtx_*` return is checked now, in the workers too. A `cnd_worker` whose lock failed returns instead of proceeding — it would otherwise read the predicate unguarded, hand `cnd_wait` a mutex it does not own and race the counters. The main thread's locks go through one helper that still stores the predicate when the mutex call fails, because that store is what releases the waiters and lets the joins terminate; the lock before `cnd_timedwait` is the exception and ends the check, since calling it unlocked is undefined |
| 15 | `counter_worker` produces two failures for one defect — the same pattern `section_mutex_timed` explicitly apologises for | **Real.** The counter is now measured against what the workers actually performed. With no lock failure that is the same number as `created * INCREMENTS`, so nothing is weakened |
| 16 | The rate check consumes a possibly-backwards clock without consulting the monotonicity result it just computed; and `ref_ns / 100 * PCT` divides before multiplying | **Real.** One backwards pair makes the subtraction about 1.8e19, which the note printed as a measurement in nanoseconds while the check failed for an unstated reason. The interval is now tested first and the rate check is skipped with a note. The division order is fixed — harmless at 1e8 ns and still the wrong order in a file whose stated rule is to check every arithmetic step |
| 17 | The "refuted P1" is recorded as both refuted and fixed, and the round's headline count depends on the disposition the same row contradicts | **Real as a write-up defect; the disposition stands.** Correction written into the first round's section above: the configure-time conditionality creates no hole, because these two tests are built only when the archives exist, so any configuration where the edge could be missing is one where there is no edge. Citing it as a reason to change was the contradiction. The real defect in that neighbourhood is finding 8, and it is not about the link edge |
| 18 | Third-party line-number citations name no version, in a repository whose own rule is that every constant cites its source | **Real.** Both `ninjabackend.py` citations now name meson 1.11.2, the version `toolchain/versions.env` pins, and both were re-read at that version before the change |

### What this round says about the verification, not the code

- **A gate that has never failed has not been tested.** Finding 1 is the
  clearest case: the fix was correct for the input it was tried with. It
  is why every claim in this round's verification table below was
  produced by breaking the thing first — the parity gate was checked by
  six deliberate divergences, `make clean` by four spellings of the same
  directory, finding 8 by running the previous commit's script in the
  scenario that defeats it.
- **Six of eighteen are the tree describing itself wrongly**, and none
  of them would have failed a build: a message that says the opposite of
  its check, a claim of two-sided bounds that were one-sided, two checks
  offered as evidence they cannot be. These reach the owner's hands in a
  console log, which is exactly where there is no source to check them
  against.
- **The one finding that did not hold up was the one Codex hedged.**
  It asked for confirmation against the pinned checkout; the pinned
  checkout answered. That is the same discipline this project applies to
  its own patches, arriving from the other direction.

### A defect this round introduced and closed inside itself

Removing the two-call-site coupling of finding 12 broke the first run
outright:

```
$ scripts/configure-horizon.sh
Found ninja-1.11.1 at /usr/bin/ninja
error: horizon_record_cross_id before horizon_setup_mode
```

`horizon_setup_mode` remembered the identity file list in a shell
variable while its callers still ran it as `mode=$(horizon_setup_mode
…)` — a command substitution is a subshell, so every assignment was
discarded before the caller reached the next line. The mode is returned
in `$HORIZON_SETUP_MODE` now and there is no subshell. It failed loudly
rather than recording a wrong stamp, which is the behaviour finding 11
was about.

### Verification after the fixes

Every command was run from the repository root against
`ghcr.io/d3fau4/nx-dev:latest` (no local devkitA64). All of it is **cross
build (X)**. Nothing here is a hardware result.

| Check | Command | Result |
|---|---|---|
| `clean` keeps Mesa whatever the spelling | `MESA_BUILD_DIR=<4 spellings> make -n clean` | before: 3 of 4 deleted it; after: 4 of 4 keep it, and `build/mesa-alt` still does not keep `build/mesa-probe` |
| `make clean` keeps the toolchain | `scripts/build-switch.sh clean && ls build/` | `mesa-probe  mesa-probe.crossid  toolchain`, and `toolchain/` still holds `meson-1.11.2` and `python` |
| …and rebuilds to thirteen | `scripts/build-switch.sh -j4` | 13 `.nro` |
| Meson path after `clean` removed its directory | `scripts/build-horizon.sh` | configures and builds, `13 .nro` |
| Mesa built *after* configure, old script | `git show HEAD:scripts/build-horizon.sh`, twice | `11 .nro`, `11 .nro` — the defect |
| Mesa built *after* configure, new script | `scripts/build-horizon.sh` | `Mesa archives are present, build/meson was configured with them absent; reconfiguring` → `13 .nro` |
| Mesa hidden, both paths | `scripts/build-switch.sh`, `scripts/build-horizon.sh` | `removing stale Mesa test artefacts: …` → **11**; `Mesa archives are absent…` → **11** |
| Mesa restored, both paths | same | **13** and **13** |
| Parity gate detects divergence | 6 deliberate breakages | 6 × exit 1 with the right label, including "extracted nothing" when the gate's own extraction is broken |
| Parity gate on the tree | `scripts/check-mesa-test-parity.sh` | OK |
| Both tests compile `-Wall -Wextra -Werror` | `scripts/build-switch.sh` | clean, no diagnostics |
| Patch series on a reset `mesa/`, ×2 | `git -C mesa reset --hard $MESA_COMMIT && scripts/apply-mesa-patches.sh` | applies 12; second run `all 12 patches already applied` |
| `.nro` parity, Makefile vs Meson | `stat -c%s` over both directories | **13/13 identical sizes** |
| Host unit tests | `scripts/run-host-tests.sh` | **103/103** |
| Gates | `check-layering.sh`, `check-no-abs-paths.sh`, `check-rust-target.sh`, `check-mesa-test-parity.sh` | all OK |

One-time cost, recorded because someone will hit it: the identity format
changed, so the first `scripts/configure-mesa.sh` on an existing tree
sees a stamp it cannot match, treats the directory as changed and wipes
it — which rebuilds Mesa. That is the conservative direction and it
happens once.

### Still owed after this round — unchanged

`t_threads` and `t_ostime` have still never run on a console. This round
made three of their bounds able to fail for the defect they name and
made the rest of the file report one defect once; none of it is a
measurement of Horizon. **Both `.nro` changed again** — the hashes at the
end of the closeout section below are the ones to run.

---

## Phase 3 — closing items 1, 2, 4, 5 and 7 (2026-07-27)

No code in `horizon/` was touched. Everything in this section is **cross
build (X)** or **host (H)**. The two new `.nro` are **not** hardware
results and are not written up as if they were.

The session's method was the one the earlier rounds settled on: measure
first, and let the measurement decide whether there is a patch at all.
It produced **two** new patches out of five items, and the two it
produced were not the ones the milestone list would have suggested.

### Item 2 — Meson `host_machine.system() == 'horizon'`: no patch

The item asks for "handling". The measurement says there is nothing to
handle, and that is recorded here with the numbers rather than asserted.

`mesa/meson.build` mentions `host_machine.system()` **54 times**,
classified by parsing every occurrence rather than by eye:

| Form | Count | Operands |
|---|---|---|
| `== '<os>'` | 36 | windows 21, darwin 7, freebsd 2, gnu / cygwin / haiku / linux / sunos / openbsd 1 each |
| `!= '<os>'` | 7 | windows 6, netbsd 1 |
| list membership (`.contains(…)`, `in`) | 8 | lines 159, 195, 197, 281, 414, 1208, 1549, 2374 |
| inside an error message's `.format()` | 3 | lines 201, 286, 464 |

**Not one of them names an OS this port is.** Every comparison is
against somebody else's platform, so `horizon` falls to the `else` in
all 54 — and in all 54 the `else` is the answer a platform with no
KMS/DRM, no dynamic loader, no X11 and no Win32 should get:
`system_has_kms_drm` false, `with_dri_platform = 'none'`,
`HAVE_RENDERDOC_INTEGRATION=0`, `sys/sysctl.h` probed rather than
assumed absent, the `-Werror=format` and `-Werror=thread-safety` trials
enabled, `dependency('threads')` used, `nm` chosen for the symbols
check. The `_GNU_SOURCE` OS list at line 1208 is the one place the
`else` was wrong, and patch **0005** already replaced it with a probe.

Three sites can print the string `horizon`, all three inside
`error('Unknown OS @0@…')`, all three reached only when an option is
left at `auto`. Measured as a chain, each with a fresh build directory:

```
$ meson setup … build/probe/mesa-default mesa
mesa/meson.build:200:4: ERROR: Problem encountered: Unknown OS horizon.
  Please pass -Dgallium-drivers to set driver options.

$ meson setup … -Dgallium-drivers= …
mesa/meson.build:285:4: ERROR: … Please pass -Dvulkan-drivers …

$ meson setup … -Dgallium-drivers= -Dvulkan-drivers= …
mesa/meson.build:463:4: ERROR: … Please pass -Dplatforms …

$ meson setup … -Dgallium-drivers= -Dvulkan-drivers= -Dplatforms=
(no error)
```

That is Mesa saying "I have no default driver list for your OS, name
one", which is **correct** rather than merely different — and
`scripts/configure-mesa.sh` has passed all three explicitly since item 3,
for reasons already recorded. Phase 4 will pass
`-Dvulkan-drivers=nouveau`, equally explicitly. Adding an
`elif host_machine.system() == 'horizon'` there would encode a driver
and platform policy, not fix a defect.

The Phase 4 route has no branches at all:

```
$ grep -rn "host_machine.system()" mesa/src/nouveau/ mesa/src/vulkan/ \
      --include=meson.build
(no output, exit 1)
```

Across all of `mesa/src`, 22 `meson.build` files mention it; **five**
sites are inside the non-driver core this project builds, and each was
read:

| Site | Horizon takes | Correct? |
|---|---|---|
| `util/blake3/meson.build:8` | `is_windows = false` | yes — portable C, no MASM |
| `util/meson.build:417, 471, 502` | all three inside `if with_tests` | not reached; tests are off |
| `util/rust/meson.build:31` | not in `['linux','windows','darwin','macos']` → `rustix` **not** required | yes — a POSIX-syscall crate that does not support this target |
| `c11/impl/meson.build:12` | `threads_posix.c` | yes, and it is item 4's whole subject |

**One honest caveat.** `meson.build:22-31` gives Horizon
`libname_prefix = 'lib'`, `libname_suffix = 'so'`, which feeds
`icd_file_name = 'libvulkan_nouveau.so'` in
`src/nouveau/vulkan/meson.build:168`. On a platform with no dynamic
loader (`dlopen : NO`, `HAVE_DLOPEN` unset) that name describes a file
nothing will ever open — a Vulkan ICD manifest is a loader concept, and
NVK here will be linked into the application. It is *meaningless*, not
*wrong*, it costs nothing today, and deciding what it should say is a
Phase 4/6 question about how the driver is delivered. Recorded rather
than patched.

**Disposition: item 2 needs no patch (case (a)).** A patch here would be
decoration, and `mesa-patches/README.md` requires a measurement behind
every one.

### Item 4 — threads: keep `threads_posix.c`, and the defect that decided it

**Which implementation Mesa uses, re-measured.**

```
$ grep -c "HAVE_THRD_CREATE" build/mesa-probe/build.ninja
0
```

`mesa/meson.build:1619-1626` sets `with_c11_threads` only when
`with_platform_android`, so `src/c11/impl/meson.build:10-17` compiles
`threads_posix.c`. newlib's own `thrd_create` links
(`Checking for function "thrd_create" : YES`) and is not used.

**Should it be? No, and the reason is not a preference.** newlib defines
the whole C11 threads API in `libc.a(libc_a-threads.o)` — 25 symbols,
`thrd_*`, `mtx_*`, `cnd_*`, `tss_*`, `call_once`. Two of them are not
implementations:

```
$ aarch64-none-elf-objdump -d --disassemble=mtx_timedlock libc_a-threads.o
0000000000000000 <mtx_timedlock>:
   0:   52800040        mov     w0, #0x2      // thrd_error
   4:   d65f03c0        ret
```

An unconditional failure that never attempts the lock. And `mtx_init`
tests bit 2 of the type — `mtx_timed = 0x4` in newlib's `threads.h` —
and branches straight to the same `#2`, so a *timed mutex cannot even be
created*. Consistent with the object's undefined symbols: it references
`pthread_mutex_lock`, `_trylock` and `_unlock`, `nanosleep`,
`sched_yield` and the cond/key family, and **no timedlock of any kind**.

Mesa's `threads_posix.c` really implements the timeout, by polling
`mtx_trylock` — the path `mesa-patches/0003` turns on where
`pthread_mutex_timedlock` is absent. Both implementations sit on the
same `libsysbase` pthreads underneath, so switching would buy nothing
and would replace a working timed lock with `return thrd_error`.

The two are also not mixable: newlib's enumeration is
`thrd_busy = 1, thrd_error = 2, thrd_nomem = 3, thrd_success = 4,
thrd_timedout = 5`, Mesa's is `thrd_success = 0, thrd_timedout = 1, …`.
A translation unit that included the wrong header would compare against
the wrong constants and compile silently.

**Decision: keep `threads_posix.c`. No patch for the selection itself.**

**The defect the item did produce → patch 0011.** `util/u_thread.c`'s
`u_thread_create()` blocks every signal around `thrd_create()` under a
`defined(HAVE_PTHREAD)` guard. POSIX puts `pthread_sigmask` in the
signal option group, not in threads, and devkitA64 has all of pthreads
without it — so the file does not link, on the function `util/u_queue.c`
uses to create every worker thread. Link probes, run directly against
`switch.specs`:

| Probe | Result |
|---|---|
| `pthread_sigmask` | **undefined reference** |
| `sched_yield` | links |
| `clock_gettime` | links |
| `pthread_barrier_init` | links |
| `pthread_getcpuclockid` | links |

So it is one absent function, not a missing threading layer — which is
why the patch is a configure check (`HAVE_PTHREAD_SIGMASK`) and not an
OS branch. After it:

```
Checking for function "pthread_sigmask" with dependency threads: NO
$ aarch64-none-elf-nm -u …/u_thread.c.o | grep pthread_sigmask
(no output)
```

`tests/t_threads.c` is the console half, described below.

### Item 1 — OS detection: what `DETECT_OS_HORIZON` is for, and the audit

`DETECT_OS_HORIZON` is defined by patch 0007 and **referenced nowhere
else in Mesa**:

```
$ grep -rn "DETECT_OS_HORIZON" mesa/src/
src/util/detect_os.h:103:#define DETECT_OS_HORIZON 1
src/util/detect_os.h:135:#ifndef DETECT_OS_HORIZON
src/util/detect_os.h:136:#define DETECT_OS_HORIZON 0
```

That is deliberate and is the answer to "is it used everywhere it is
needed": an OS needs an *identity*, but every behavioural question is
answered by `DETECT_OS_POSIX_LITE`, which is a statement about the C
library and therefore upstreamable. `DETECT_OS_POSIX_LITE` defaults to
`DETECT_OS_POSIX`, so each such patch is additive for every existing
platform. It gates `os_time.c` (patch 0008) and now `u_cpu_detect.c`
(patch 0012).

**Finding the `#else` branches that assume Linux, by measurement rather
than by reading.** 21 of the 323 sources compiled into the core mention
`DETECT_OS_*`. Rather than judge each by eye, the whole core was audited
at the link level — a branch that assumes more C library than exists
shows up as a symbol nothing defines:

```
core undefined refs   : 1816
resolved inside core  : 1637
resolved by toolchain :  156   (libc, libm, libsysbase, libpthread,
                                libstdc++, libnx, portlibs, libgcc,
                                libhorizon_compat)
UNRESOLVED            :    7
```

| Symbol | Referenced by | Category |
|---|---|---|
| `pthread_sigmask` | `u_thread.c.o` | **fixed here — patch 0011** |
| `posix_memalign` | `sparse_array.c.o` | item 3 residue, and a **lying configure check** |
| `flock` | `mesa_cache_db.c.o` | item 3 residue |
| `getuid` | `anon_file.c.o`, `log.c.o`, `perf_u_trace.c.o` | item 3 residue |
| `geteuid`, `getgid`, `getegid` | `log.c.o`, `perf_u_trace.c.o` | item 3 residue |

**`posix_memalign` is the interesting one.** Configure says

```
Checking for function "posix_memalign" : YES
```

and `-DHAVE_POSIX_MEMALIGN` reaches 352 compile lines — while a direct
link probe answers `undefined reference to 'posix_memalign'`. GCC has a
`__builtin_posix_memalign`, so Meson's check compiles and links its own
snippet successfully. This is *exactly* the hazard `mesa/meson.build`
documents three lines above the check, for MinGW, and it applies here
too. It is a false configure answer of the same class as the `-Werror`
defect this project found in its own cross file in item 3.

**These six are recorded, not fixed.** They are item 3's category
(newlib/libnx gaps) and item 3 is closed; none of them blocks Phase 3's
criterion, because the criterion is that the core *builds* and a static
archive never resolves anything. They are what Phase 4 will meet at its
**first link**, and this is the list. Fixing them is a scoped piece of
work, not a drive-by.

**Three more things the audit surfaced, none of them a link failure:**

- `util/u_thread.c:92` emits `#warning Not sure how to call
  pthread_setname_np` — measured, it fires. Thread names are cosmetic
  and the branch is a no-op; recorded so it is not mistaken for a defect
  later.
- `util/u_process.c:186` emits `#pragma message ( "Warning: Per
  application configuration won't work with your OS version." )`. driconf
  per-application matching will not work; Mesa says so itself and
  degrades rather than failing.
- `util/rand_xor.c` takes the `!DETECT_OS_WINDOWS` path and calls
  `open("/dev/urandom")`, which cannot succeed here. The file's own
  fallback then seeds from a constant plus `time(NULL)`. Nothing is
  simulated and no rejected design is involved — the open simply fails —
  but the seed is weak. Horizon does have an entropy source
  (`InfoType_RandomEntropy`, libnx `randomGet`), so a `compat/getrandom`
  would make `HAVE_GETRANDOM` true and route Mesa to it. Deliberately
  **not** done in this session: it is item 3's category and out of scope.

**Item 1's own patch → 0012**, below.

### The known defect, fixed: `util_cpu_detect` reported 1 CPU

Recorded at the end of item 6 and carried since. `_util_cpu_detect_once`
counts processors under `#elif DETECT_OS_POSIX`, patch 0007 chose
POSIX-**lite**, so nothing set `available_cpus`, `nr_cpus` fell to
`MAX2(1, 0)` and `util/u_queue.c` would have sized its thread pool for a
single-core machine.

Two changes, and the split between them is a layering decision:

- **`mesa-patches/0012`** turns the outer guard and the `<unistd.h>`
  include into `DETECT_OS_POSIX_LITE`. Nothing inside the block needs
  the whole of POSIX — every path is already guarded by the macro naming
  what it uses (`HAS_SCHED_GETAFFINITY`, `_SC_NPROCESSORS_ONLN`,
  `_SC_NPROCESSORS_CONF`, `HW_NCPUONLINE`) — so this is the same shape
  as patch 0008 and changes no existing platform.
- **`compat/sysconf.c`** answers `_SC_NPROCESSORS_ONLN` and
  `_SC_NPROCESSORS_CONF` from `svcGetInfo(InfoType_CoreMask)`,
  "Bitmask of allowed Core IDs" (libnx `switch/kernel/svc.h:185`), whose
  population count is the set of cores the process may run on.

**Why the number comes from `compat/` and not from a Mesa patch:** the
only source for it is libnx, and `<switch.h>` inside Mesa's generic
`src/util` would break this project's layer rules and could never go
upstream. `sysconf` is already the door `CLAUDE.md` leaves open and the
cross file already links `libhorizon_compat` before `-lnx`. newlib
defines both names (`sys/unistd.h:370-371`, values 9 and 10) and defines
neither symbol, which is the same case as the three names `compat/`
already answered.

`_ONLN` and `_CONF` deliberately return the same value: Horizon exposes
no second, wider count to an ordinary process, and reporting the SoC's
four Cortex-A57s for `_CONF` would be a constant nobody measured. A zero
mask is treated as a failed query, since a running process must be
allowed at least one core.

Measured effect (cross build):

```
before:  $ aarch64-none-elf-nm u_cpu_detect.c.o | grep sysconf   →  (nothing)
after :  $ aarch64-none-elf-nm -u u_cpu_detect.c.o | grep sysconf →  U sysconf
```

`HAS_SCHED_GETAFFINITY` is absent (`sched_getaffinity : NO`), which is
why the `_SC_NPROCESSORS_ONLN` path is the one that had to be reachable.
**The value itself is not yet measured** — that needs `t_threads` on a
console.

### Item 5 — timers / clocks

`os_time.c` has compiled since patch 0008 and no line of it had run. Two
of its functions cannot be taken on trust from a compile:

- **`os_time_get_nano()` does not check its clock.** It calls
  `timespec_get(&ts, TIME_MONOTONIC)` and returns from `ts` regardless.
  On this platform `timespec_get` is Mesa's own `c23_timespec_get`
  (`src/c11/impl/time.c`), which forwards to
  `clock_gettime(CLOCK_MONOTONIC)` and, on failure, returns 0 **without
  touching `ts`** — so the caller gets uninitialised stack. newlib
  defines `CLOCK_MONOTONIC` as 4 (`time.h:278`) whether or not
  `libsysbase` honours it, and `clock_gettime` links, so nothing about
  this is visible before the code runs.
- **`os_time_sleep()` is `usleep()`** through the POSIX-lite branch
  patch 0008 added. A `usleep` that returns at once and one that sleeps
  ten times too long both "work".

`tests/t_ostime.c` measures both, plus resolution, rate,
`os_time_nanosleep_until` and `os_time_get_absolute_timeout`. **Item 5
stays a cross-build result until that runs.**

### Item 7 — endianness: closed on the cross build

This one needs no console: it is a compile-time property.

```
$ aarch64-none-elf-gcc -D__SWITCH__ -Imesa/src … -E -dM -x c mesa/src/util/u_endian.h
#define UTIL_ARCH_BIG_ENDIAN 0
#define UTIL_ARCH_LITTLE_ENDIAN 1
#define __BYTE_ORDER__ __ORDER_LITTLE_ENDIAN__

$ # and which branch answered:
$ echo '#include <endian.h>' | aarch64-none-elf-gcc -c -x c -
fatal error: endian.h: No such file or directory
```

A translation unit with
`_Static_assert(UTIL_ARCH_LITTLE_ENDIAN == 1)` and
`_Static_assert(UTIL_ARCH_BIG_ENDIAN == 0)` compiles clean under
`-Wall -Wextra -Werror`. `endian.h` is absent and no libc branch in
`u_endian.h` matches, so the answer came from patch 0004's
`__BYTE_ORDER__` fallback — the branch under test, not one of the
pre-existing ones. **Item 7 is closed (X).**

### The two new `.nro`, and why they link Mesa's archives

`tests/t_threads.c` (12) and `tests/t_ostime.c` (13) link
`build/mesa-probe/src/c11/impl/libmesa_util_c11.a` and
`.../src/util/libmesa_util.a` — **the archives Mesa's own build
produced**, not those sources recompiled with flags of ours. The object
under test has to be the object Mesa builds, or the measurement is about
a different build. The only two defines the test sources need are
`-DHAVE_PTHREAD` and `-DHAVE_STRUCT_TIMESPEC`, both Mesa's own configure
results here, both visible in `build/mesa-probe/build.ninja`.

Consequences accepted and handled:

- They need `scripts/configure-mesa.sh && scripts/build-mesa.sh` first.
  Both build paths **skip them with a message** when the archives are
  absent, so a bare clone still produces the eleven tests that need only
  the toolchain. `make` prints
  `skipping t_threads t_ostime — no Mesa archives in build/mesa-probe`.
- Tests 1–11 still build with no Mesa in sight; the Mesa include path
  and archives are target-specific in the `Makefile` and a separate
  dependency in `meson.build`.
- `libmesa_util.a` links cleanly into both, which also demonstrates the
  audit's finding from the other side: the six unresolved libc symbols
  live in objects (`log.c.o`, `anon_file.c.o`, `sparse_array.c.o`,
  `mesa_cache_db.c.o`) that these two tests do not pull in.

What `t_threads` checks: `thrd_create`/`thrd_join` including the
returned value, `u_thread_create` (the function patch 0011 changed),
`call_once` across four threads, a shared counter of 4 × 20 000
increments under a mutex, `mtx_trylock` on a held mutex,
**`mtx_timedlock` expiring**, `mtx_timedlock` on a free mutex returning
promptly, `cnd_wait`/`cnd_signal`, `cnd_broadcast` waking all four,
**`cnd_timedwait` expiring** (which `src/vulkan/runtime/vk_sync_timeline.c`
depends on in Phase 4), `tss_create`/`tss_set`/`tss_get` with a
cross-thread leak check and the destructor count, and the processor
count against the raw `InfoType_CoreMask`.

Every timed check is bounded **from both sides** — 150 ms ≤ elapsed ≤
800 ms for a 200 ms timeout — and timed with `armGetSystemTick()`, the
ARM system counter read directly. That is not the clock the code under
test uses; measuring a clock with itself proves nothing, and a polling
`mtx_timedlock` whose comparison is inverted returns `thrd_timedout`
immediately, which a one-sided check cannot tell from correct.

`t_check`/`t_note` write shared state, so they are called from the main
thread only; workers set plain per-thread fields read after the join
that orders them. Everything read *without* a join in between is
`atomic_int`: the `tss` destructor count several exiting threads
increment at once, the `done` flag the main thread polls instead of
joining the `mtx_timedlock` worker, the `outcome` the `cnd_timedwait`
waiter and its watchdog settle with one compare-exchange so that exactly
one of them ends the wait, and — added by the second review round — the
count of failed `mtx_lock`/`mtx_unlock` calls inside the condvar
workers, which is the one field a worker writes while *not* holding the
mutex, because not holding it is what it records.

The artefacts handed over, so a console log can be attributed to exactly
these builds (Makefile path, which is the reference path):

```
00d15baa9beca514c39cc566ce60c4b4bc438ef54b8b0507f2b68296704289ff  t_threads.nro
ff999f013acd300f029f13605bad20e70992afa191033d405cfed475b8f2d6c8  t_ostime.nro
```

`t_ostime` is the build the emulator run of 2026-07-28 used and is
unchanged since. `t_threads` was rebuilt after that run to add the staged
probe around `util_get_cpu_caps()`; the build that produced the emulator
log is `3f97f5d2…`, and the two before it — `45e49f1e…` (before any
review) and `0ca4b59f…` (after the first review round) — are older still.
Only the hashes in the block above should be run. What changed across
those builds is in the two review sections and the emulator section
above: no check has ever been removed, several were added, every wait on
a timed call is bounded, and the second review round tightened three
bounds that were loose enough to pass for the failure they name.

The Meson path produces the same **sizes** for all thirteen and
different sha256 for these two — the inter-object padding difference
recorded at the end of Phase 2 (≤32 bytes of `.bss`, ≤16 of `.text`,
from archive ordering), not a behavioural difference.

### Commands run and results

| Command | Class | Result |
|---|---|---|
| `scripts/fetch-mesa.sh` | H | `mesa-26.1.5`, HEAD verified `6a02618ccf6c…`, 503 MB |
| `scripts/apply-mesa-patches.sh` on a reset `mesa/`, twice | H | applies **12**; second run `all 12 patches already applied; nothing to do`, exit 0 |
| `scripts/apply-mesa-patches.sh --list` | H | 12 applied, 0 pending |
| `scripts/build-compat.sh` | X | rebuilds after the `sysconf` change |
| `scripts/configure-mesa.sh` | X | exit 0; `sysconf : YES`, `thrd_create : YES`, `sched_getaffinity : NO`, **`pthread_sigmask : NO`** |
| `scripts/build-mesa.sh` from a deleted `build/mesa-probe` | X | **379/379 edges, 0 FAILED, 10/10 libraries** |
| the same after `fetch-mesa.sh --force` + re-apply (end-to-end rerun) | X | 321/321 edges rebuilt, 0 FAILED, 10/10 |
| `meson setup` with default options, then adding one option at a time | X | the three `Unknown OS horizon` errors, in order, then success |
| `nm -u` audit of the ten core archives vs the whole toolchain | X | 1816 → 7 unresolved (table above) |
| Link probes for 9 libc/pthread functions | X | 4 absent, 5 present (table above) |
| `-E -dM` on `u_endian.h`; `_Static_assert` TU | X | `UTIL_ARCH_LITTLE_ENDIAN 1` / `BIG 0`; compiles under `-Werror` |
| `objdump -d` of newlib's `libc_a-threads.o` | X | `mtx_timedlock` is `mov w0,#2; ret` |
| `scripts/build-switch.sh all -j4` | X | **13 `.nro`** |
| `scripts/configure-horizon.sh && scripts/build-horizon.sh` | X | **13 `.nro`**, `tests : 13` |
| Makefile vs Meson, all thirteen | X | **identical sizes 13/13** |
| `scripts/run-host-tests.sh` | H | **103/103 PASS** (6 suites, unchanged) |
| `scripts/check-layering.sh` | H | OK |
| `scripts/check-no-abs-paths.sh` | H | OK |
| `scripts/check-rust-target.sh` | H | OK |

### Phase 3 exit criteria — state

| Criterion (`docs/milestones.md`) | State |
|---|---|
| Each item is a separate patch file with a header explaining it (X) | ✅ 12 patches, four-field header each. Items 2 and 8 carry **no** patch, each with the measurement that says none is warranted |
| Mesa configures for `horizon` and builds the non-driver core (X) | ✅ configure exit 0; **379/379 edges, 0 FAILED, 10/10 libraries** |
| No patch mixes functional change with formatting | ✅ |

### Milestone items — final disposition

| # | Item | Disposition |
|---|---|---|
| 1 | OS detection | patch 0007 (identity) + patch 0012 (CPU count). Audit above says where the `#else` still assumes more libc than exists |
| 2 | Meson `host_machine.system()` | **no patch** — 54 sites classified, all 54 correct in the `else`; the 3 that name the OS are `auto`-default errors we already pass options for |
| 3 | newlib/libnx gaps | patches 0001–0006. **Six link-time gaps remain**, listed above, deliberately not reopened |
| 4 | threads | patch 0003 + patch 0011; decision to keep `threads_posix.c` recorded with the disassembly behind it. `t_threads` **not yet run** |
| 5 | timers / clocks | patch 0008; `t_ostime` **not yet run** |
| 6 | physical memory / page size | `compat/sysconf.c` + patches 0009–0010, hardware-verified 2026-07-27 |
| 7 | endianness | patch 0004, **closed on the cross build** — it is a compile-time property |
| 8 | build ID | closed in Phase 2 without a patch: `-Wl,--build-id=sha1` is supported |

### What this session did NOT do, said plainly

- **`t_threads` and `t_ostime` have never run.** Items 4 and 5 are cross
  builds. Nothing about Mesa's threading or clocks on a console is
  claimed here, including the CPU count patch 0012 produces.
- **Six unresolved libc symbols are left in place** (`posix_memalign`,
  `flock`, `getuid`, `geteuid`, `getgid`, `getegid`). They will stop the
  first executable link in Phase 4 and they are listed with their
  objects so that is a task, not a surprise.
- **`rand_xor` seeds weakly on Horizon**, by Mesa's own documented
  fallback. A `compat/getrandom` is the fix and was not written.
- The `posix_memalign` configure check answers `YES` and is wrong. Not
  fixed here.

---

## Hardware run of all eleven `.nro`, both process modes (2026-07-27)

Owner-executed on a real Switch, **applet mode and full/game mode**,
logs received as `horizongputests.rar` (22 files, `sdmc:/horizon_gpu_tests/`).
This closes the re-run owed since Phase 1's second review round and is
the first console evidence for anything in Phase 3.

| # | Test | Applet | Full/game |
|---|---|---|---|
| 1 | `t_init` | **PASS 22/22** | **PASS 22/22** |
| 2 | `t_alloc` | **PASS 21/21** | **PASS 21/21** |
| 3 | `t_nvmap` | **PASS 16/16** | **PASS 16/16** |
| 4 | `t_va_reserve` | **PASS 18/18** | **PASS 18/18** |
| 5 | `t_map` | **PASS 28/28** | **PASS 28/28** |
| 6 | `t_channel` | **PASS 17/17** | **PASS 17/17** |
| 7 | `t_submit` | **PASS 30/30** | **PASS 30/30** |
| 8 | `t_syncpt` | **PASS 48/48** | **PASS 48/48** |
| 9 | `t_fence_wait` | **PASS 14/14** | **PASS 14/14** |
| 10 | `t_teardown` | **PASS 34/34** | **PASS 32/32** |
| 11 | `t_sysinfo` | FAIL 18/19 → **PASS 21/21** | FAIL 18/19 → **PASS 21/21** |

**Eleven of eleven pass in both modes, on the current code**, after one
fix to the test itself (below). The first run had ten. The counts are
higher than the Phase 1 run (`t_map` 26→28, `t_submit` 23→30,
`t_va_reserve` 17→18, `t_teardown` 28→34/32) because the second review
round added assertions; those are the very tests whose paths it changed.
`t_teardown`'s different totals between modes are the documented
in-flight-destroy race taking its two legal branches — both PASS.

`t_init` and `t_map` are **byte-identical between modes**, which is what
you want from a device-query test.

### Second run: `t_sysinfo` PASS 21/21 in both modes — everything closed

The fixed probe was re-run the same day. Both modes:

```
ok   unmapped the 0x1000 probe (rc=0x00000000)
note svcMapMemory(0x1000): OK
ok   the kernel mapped at least one size up to 0x10000
ok   smallest granularity the kernel accepts is 0x1000, and sysconf reports 0x1000
RESULT: PASS (21/21)
```

The **first** rung mapped. So the page size is now bounded from *both*
sides by measurement: no region boundary falls off `0x1000` (upper), and
the kernel accepts a map of exactly `0x1000` (lower). `compat/sysconf.c`'s
cited constant is confirmed, not merely consistent. 21 checks rather
than 19 because a successful ladder adds the unmap and the equality.

**Eleven of eleven now PASS on hardware, in both process modes.**

The memory figures **reproduce exactly** across the two independent
runs — `total`, `used` and `_SC_AVPHYS_PAGES` byte-identical in each
mode (394 MiB / 35214 pages applet; 3189 MiB / 995 pages full-game), so
they are deterministic per mode rather than sampling noise. Only the
region base addresses move, which is ASLR doing its job; the `aslr`
region base itself is `0x8000000` in every run.

### The first run's one failure, and why it was not what it looked like

`t_sysinfo` fails exactly one check, identically in both modes: the
`svcMapMemory` granularity ladder never mapped anything. Every rung
returned `0x0000dc01`.

Decoded against libnx's `result.h`: module 1 (kernel), description 110 =
**`KernelError_InvalidMemoryRange`** — *not* `KernelError_InvalidSize`
(101), which is what a granularity refusal would be. So the kernel was
objecting to *where* the probe tried to map, never to the size.

The cause is a bug in the probe: it took its destination from
`virtmemFindAslr`, and `svcMapMemory` only accepts the **stack** region
— it is documented as "mainly used for adding guard pages around stack",
and libnx uses `virtmemFindStack` for exactly this call. Fixed; the
probe's diagnostics now name the kernel description instead of printing
only hex.

**The ladder design did its job.** The test reported
`granularity NOT measured: no rung mapped, so the cause is something
other than the page size` rather than failing the page-size check — which
is precisely the self-diagnosis it was built for after the Codex review.
A single-size probe would have looked like "the page size is wrong".

### What was measured, as opposed to reasoned

**The page size is 0x1000, upper bound confirmed.** `sysconf(_SC_PAGESIZE)`
returns 4096, and every region the kernel reports is page-aligned and a
whole number of pages of it, in both modes:

| Region | Applet | Full/game |
|---|---|---|
| heap | `0x11ec600000` + `0x200000000` | `0x695de00000` + `0x200000000` |
| alias | `0x4d4a200000` + `0x1000000000` | `0x42bce00000` + `0x1000000000` |
| aslr | `0x8000000` + `0x7ff8000000` | `0x8000000` + `0x7ff8000000` |
| stack | `0x3469000000` + `0x80000000` | `0x5a68e00000` + `0x80000000` |

**And confirmed from below**, by the second run: `svcMapMemory` accepts
a map of exactly `0x1000`, the ladder's first rung. Both bounds are now
measurements.

**The mode-aware argument is confirmed, and by a wide margin.** This is
the reasoning `compat/sysconf.c` was written on, now measured:

| | Applet | Full/game | Ratio |
|---|---|---|---|
| `InfoType_TotalMemorySize` | `0x18aab000` = **394 MiB** | `0xc7500000` = **3189 MiB** | **8.1×** |
| `_SC_PHYS_PAGES` | 101 035 | 816 384 | |
| `_SC_AVPHYS_PAGES` | 35 214 (137 MiB free) | 995 (3 MiB free) | |

A hardcoded "4 GiB" would have been wrong by more than 8× in applet
mode, and `_SC_PHYS_PAGES × page size` equals the raw
`InfoType_TotalMemorySize` exactly in both. This is the decision from
"Phase 3 — item 6" holding up against hardware rather than against an
argument.

It also shows why the **zero-available fix from the Codex review
matters**: in full/game mode the console reported 995 free pages, 3 MiB.
A process starting slightly later reports zero, and the pre-fix code
would have called that a failed query and made NVK log that it could not
read the budget.

### Other measurements worth keeping

- GM20B identical in both modes: `chip='gm20b' arch=0x120 impl=0xb
  rev=0xa1`, 1 GPC × 2 TPC, L2 `0x40000`, `va_bits=40`,
  `big_page=0x20000`, `compression_page=0x20000`, classes 3d `0xb197` /
  compute `0xb1c0` / 2d `0x902d` / gpfifo `0xb06f` / i2m `0xa140` /
  copy `0xb0b5`.
- VA regions: small `base=0x8000000 pages=0x3f7fff page=0x1000`, big
  `base=0x400000000 pages=0xdffff page=0x20000`.
- **R5 again**: syncpoint id 26, value 68680 at channel create, 71046 at
  `t_syncpt` start — counters still not reset per channel, third
  independent confirmation that the shadow-from-read design is required.
- **Async submission still holds**: both submits issued in **149 µs**
  with no intervening CPU wait (Phase 1 measured 148 µs).

### Consequences

- **Phase 1's two hardware exit criteria go back to ✅** — measured on
  the current code, not on `732b58c`. See that table below.
- `compat/sysconf.c` is hardware-verified in full, after the probe fix.
- **Nothing is owed on hardware.** `compat/sysconf.c` is verified end to
  end: the page size it reports is bounded from above by every region
  the kernel exposes and from below by the smallest map the kernel
  accepts, and both memory figures match the raw syscall exactly in two
  process modes that differ by 8×.

---

## Codex PR review, Phase 3 (2026-07-26) — 9 findings, all real

`chatgpt-codex-connector[bot]` reviewed PR #3 at `bd74d45` and left
**3 × P1 and 6 × P2**. Each was checked against the code before anything
was changed, as in the two earlier rounds. **This time all nine held
up** — none was investigated and dismissed.

Two of them are failures of *my verification*, not just of the code, and
are recorded as such.

| # | Finding | Disposition |
|---|---|---|
| P1 | `meson setup --reconfigure` keeps the machine-file args recorded at the first configure, so `-lhorizon_compat` never reaches an existing build directory | **Real, and it breaks the ordinary upgrade path.** Reproduced end to end: a directory configured with the old cross file then `--reconfigure`d has **zero** occurrences of the flag, and `t_sysinfo` fails with `undefined reference to 'sysconf'` ×5. `--wipe` does re-read them (11). Fixed: `horizon_setup_mode` hashes both cross files beside the build directory and picks `""`/`--reconfigure`/`--wipe`; a configured directory with no stamp counts as changed, which is every directory predating the fix |
| P1 | `GIT_WORK_TREE` / `core.worktree` defeat the applier's git-dir assertion | **Real, measured.** With `GIT_WORK_TREE` set, `--absolute-git-dir` still answers `mesa/.git` — the guard passes — while `--show-toplevel` answers the foreign tree, which is where `status` and `am` would act. Fixed on both sides: the redirecting Git environment variables are cleared, *and* `--show-toplevel` is asserted, since `core.worktree` needs no variable. `fetch-mesa.sh` had the same guard and got the same fix |
| P1 | "Already applied" compared only commit subjects, so a regenerated patch body was invisible | **Real.** Fixed with `git patch-id --stable` on both sides, subject kept for diagnostics. Verified by editing a line inside 0010's diff and leaving its `Subject:` alone: now reported as divergence, where before it said "all 10 applied". Residual stated in `mesa-patches/README.md`: a change confined to the commit message below the subject, with an identical diff, still reads as applied |
| P2 | Patch 0010 treated `avail_pages == 0` as a query failure | **Real, and self-inconsistent:** `compat/sysconf.c` deliberately returns 0 when `used >= total`, so the patch turned its own documented case into a spurious failure. NVK would log "failed to query the budget" instead of publishing a valid zero. Now only `-1` is an error |
| P2 | `t_sysinfo`'s available-memory bound underflows on u64 | **Real**, same root cause: `total - used` wraps in exactly the case `compat/` documents, making the bound trivially true. Clamped before subtracting |
| P2 | The page-size check is one-directional | **Real, and the claim in the code and in this file was wrong.** A 64 KiB-aligned region is also divisible by 4 KiB, so divisibility catches an *overstated* page size and says nothing about an understated one — the opposite of what was written. Corrected, and the missing bound added: `probe_map_granularity` walks a ladder of `svcMapMemory` sizes and checks the smallest accepted one equals what `sysconf` reports. A ladder rather than one size so a failure from any other cause fails at every rung and is reported as "not measured" instead of blaming the page size |
| P2 | `sed -i` in GNU form fails on BSD/macOS | **Real.** It would fail on the *first* configure of a local devkitPro install on macOS. Rewritten through a temp file |
| P2 | An apostrophe in the checkout path breaks the generated cross file | **Real.** Escaped for Meson's string syntax, backslashes first |
| P2 | `build-compat.sh` cannot see the toolchain move | **Real, and wider than reported.** mtimes cannot notice a different `$DEVKITPRO` or a newer image, and the Switch toolchain is deliberately unpinned, so Mesa could link an object built against the previous environment right after an update. Replaced with a content identity (flags, toolchain description, image digest, compiler banner, sources). Codex named only the script; **the `Makefile` had the matching gap** — it generated `build/toolchain/lib/*.d` and never listed them in its `-include` line — and that is fixed too |

### What the round says about the verification, not the code

- **The upgrade path was never tested.** Every check in the item 3 and
  item 6 sessions began with `rm -rf build/meson`. The clean path was
  verified repeatedly; the path an actual developer takes — an existing
  build directory — not once. That is why a P1 got through.
- **The page-size claim was the one I flagged as least certain** in the
  review request, and it was indeed wrong. Asking about it was right;
  writing it as settled in `STATUS.md` was not.
- The gates caught two of the fixes' own comments — the `--wrap` spelling
  in the compat round, and a `/home/` example path in this one. Both
  reworded rather than exempted.

### Re-verification after the fixes

| Check | Result |
|---|---|
| Old-cross-file build directory + `configure-horizon.sh` | detected, wiped, flag lands (11 occurrences); second run reports `--reconfigure` |
| `GIT_WORK_TREE` set / `core.worktree` set | ignored correctly / refused with the tree it would have written to |
| 0010's diff edited, subject untouched | divergence reported and refused |
| `scripts/apply-mesa-patches.sh` on a reset `mesa/`, ×2 | applies 10; second run `nothing to do` |
| `scripts/configure-mesa.sh` + `ninja -k 0` | exit 0, `sysconf : YES`, **379/379, 0 FAILED, 10/10 libraries** |
| Ten Phase 1 `.nro` vs the Phase 2 baseline | **identical 10/10** |
| `make clean && make all -j4` | exit 0, 11 `.nro` |
| `build-compat.sh` on a changed identity / unchanged | rebuilds / no-op |
| Host tests, three gates | 103/103, all OK |

---

## Phase 3 — item 6, physical memory and page size (2026-07-26)

Closes the one object item 3 could not: `src/util/os_misc.c`. No code in
`horizon/` was touched. Everything here is **cross build (X)** or
**host (H)**; `t_sysinfo` is explicitly **not** hardware-verified.

### The decision: `compat/`, not a Mesa patch

Item 6 needed two facts with no libc route — `sysconf` and
`getpagesize` are both genuinely absent, each verified by link probe
(`undefined reference`). Three things settled where the answer comes
from:

1. **Returning `false` is not benign.** In NVK,
   `os_get_total_physical_memory` failing is a hard
   `VK_ERROR_INITIALIZATION_FAILED`
   (`mesa/src/nouveau/vulkan/nvk_physical_device.c:1513`) — the device
   is not enumerated at all. `os_get_page_size` failing is worse:
   `nvkmd/nouveau/nvkmd_nouveau_pdev.c:114` reads an **uninitialised**
   `uint64_t` into `bind_align_B`, which reaches
   `VkMemoryRequirements::alignment`. Both must return true.
2. **newlib *declares* `sysconf`** (`sys/unistd.h:236`) and defines
   `_SC_PAGESIZE`, `_SC_PHYS_PAGES` and `_SC_AVPHYS_PAGES` (8, 11, 12).
   It simply never defines the symbol — the probe failed at *link*, not
   at compile. That is the textbook `compat/` case `CLAUDE.md` allows,
   and the layer table already permits `compat/ → libnx`.
3. **`HAVE_SYSCONF` makes all three functions take their *first*
   branch**, so most of item 6 needed no Mesa change at all.

A Mesa patch could not have answered this. Total memory has no source
that is not libnx, and putting `<switch.h>` into Mesa's generic
`src/util` would be both un-upstreamable and a layering breach. A
hardcoded constant would be *wrong*, not merely approximate: the Switch
gives an application and an applet very different limits.

### What `compat/sysconf.c` reports, and on what authority

| Name | Value | Source |
|---|---|---|
| `_SC_PAGESIZE` / `_SC_PAGE_SIZE` | `0x1000` | switchbrew's SVC docs — every memory svc takes sizes "aligned to 0x1000 bytes", repeated in libnx's `switch/kernel/svc.h`; ARMv8-A's smallest granule is 4 KiB. Corroborated by this project's own console runs: `horizon_gpu_mem_create` uses `aligned_alloc(0x1000, …)` and `nvMapCreate`, which requires page-aligned CPU memory, accepted it across `t_alloc` 21/21, `t_nvmap` 16/16, `t_map` 26/26 |
| `_SC_PHYS_PAGES` | `svcGetInfo(InfoType_TotalMemorySize) / 0x1000` | `svc.h:191` |
| `_SC_AVPHYS_PAGES` | `(Total − `InfoType_UsedMemorySize`) / 0x1000`, clamped | `svc.h:192` |
| anything else | `-1`, `errno = EINVAL` | POSIX |

**The semantics are declared, not implied.** `InfoType_TotalMemorySize`
is *"Total amount of memory available for process"* — the process's
limit, not the console's DRAM. That is deliberately what gets reported:
it is what a caller sizing a heap needs, since memory the process cannot
allocate is not usable however much the machine has, and it is
mode-aware. `SystemInfoType_TotalPhysicalMemorySize` would give the DRAM
but is privileged and not reliably available to homebrew.

`svcGetInfo` is a raw syscall needing no service session and no
`__appInit`, so `sysconf` is safe to call before libnx's service
initialisation — which matters for a C library function.

### Two Mesa patches, both general

- **0009 `util/os_misc: include <unistd.h> where sysconf is the answer`**
  — the include chain at the top of the file is a list of OS names
  ending in `#error`, and it fires before any implementation is looked
  at. An `#elif HAVE_SYSCONF` at the end of the chain is all a
  sysconf platform needs. Additive; unreachable for anything that
  already matched.
- **0010 `util/os_misc: query available memory through sysconf`** —
  `os_get_available_system_memory` fell to `#else return false`, which
  compiles but in Phase 4 gives `heapBudget = 0` for the **only** heap
  on a Tegra SoC, violating the `VK_EXT_memory_budget` clause the code
  quotes at `nvk_physical_device.c:1725` and logging a `vk_loge` per
  query. Adds an `#elif HAVE_SYSCONF && defined(_SC_AVPHYS_PAGES)`
  branch after every existing one.

### Linking `compat/` — why it is in the cross file

`cc.has_function('sysconf')` is a **link** test, which is how Mesa
decides `HAVE_SYSCONF`, and Meson links a test program during its own
sanity check at setup. So the archive has to exist **before any
`meson setup`** — including the one that would build it, which is why it
is not a target of our `meson.build`.

- `scripts/build-compat.sh` — provisioning, in the same sense as
  `horizon_ensure_meson`. Idempotent; archives from scratch so a deleted
  source cannot leave a stale member.
- `horizon_compat_libdir`, a new constant from `gen-cross-file.sh`
  alongside `devkitpro` — same pattern, so no tracked file gains a path.
- `toolchain/horizon-aarch64.cross` links `-lhorizon_compat` before
  `-lnx`. From Meson's point of view compat/ *completes the C library*,
  which is what a cross file describes.
- The **`Makefile` builds the archive natively**, and that was not
  optional: `make clean` is `rm -rf build` and the archive lives inside
  it, so a Makefile that only consumed it broke on `make clean && make`.
  Found by doing exactly that. Verified after the fix: a full clean
  followed by `make all -j4`, with nothing provisioned, builds all
  eleven `.nro`.

### The gate gained `compat/` — and caught two of my own comments

`scripts/check-layering.sh` did not look at `compat/` at all. It now
checks that `compat/` includes no Vulkan/Mesa/NIR/DRM header **and no
`horizon/` header** (compat/ is below it; reaching up would invert the
stack), the reciprocal that `horizon/` includes no `compat/` header, and
`compat/` joins the rejected-design greps.

On its first run it failed — on comments in `compat/sysconf.c` and
`build-compat.sh` that spelled out the banned linker flag while
explaining that they do not use it. **Reworded rather than filtered:** a
comment-line exemption would have weakened a check whose entire value is
being blunt. Verified afterwards that the gate *detects* rather than
merely passes: a probe file including `horizon_gpu/device.h` from
`compat/` is reported and exits 1.

### `t_sysinfo` — the eleventh `.nro`

`compat/sysconf.c` is the only code here that answers with a number
nobody can check by compiling. `tests/t_sysinfo.c` measures it, and is
deliberately **not** an assertion of the constant against itself:

- **upper bound** — every region `svcGetInfo` reports (heap, alias,
  aslr, stack) must be page-aligned and a whole number of pages of
  whatever `sysconf` returned. A page size *bigger* than the real one
  puts a boundary off it;
- **lower bound** — the smallest size `svcMapMemory` accepts must equal
  what `sysconf` reports. Divisibility cannot see an understated page
  size (a 64 KiB-aligned region divides by 4 KiB too), so this half is
  what covers it. **Added after the Codex review**: the first version of
  this section claimed divisibility caught both, and that was wrong;
- `_SC_PHYS_PAGES × page size` must equal the raw
  `InfoType_TotalMemorySize`, which is the claim `compat/` makes about
  *what* it reports;
- available never exceeds total, and agrees with total − used, clamped
  the way `compat/` clamps it;
- an unknown name gives `-1`/`EINVAL`, not a plausible number.

Raw values are printed as well as checked, so a console log records the
figures as data. It uses no `horizon_gpu` and needs no nv services,
which also makes it the cheapest test to run first when triaging.

**It has never been run.** Until it is, the page size is cited and the
memory figures are reasoned.

### Commands run and results

| Command | Class | Result |
|---|---|---|
| `scripts/build-compat.sh`, twice | X | builds 1 object; second run `up to date` |
| `scripts/configure-mesa.sh` | X | exit 0, **`Checking for function "sysconf" : YES`** |
| `ninja -k 0` over the ten core libraries | X | **379/379 edges, 0 FAILED, 10/10 libraries archived** |
| `scripts/apply-mesa-patches.sh` on a reset `mesa/`, twice | H | applies 10; second run `all 10 patches already applied` |
| `scripts/configure-horizon.sh && scripts/build-horizon.sh` | X | **11 `.nro`**; the original ten keep the Phase 2 baseline's sha256 |
| `scripts/build-switch.sh clean` then `all -j4`, nothing provisioned | X | exit 0, 11 `.nro`, archive built by the Makefile itself |
| Makefile vs Meson, all eleven | X | identical sizes 11/11 |
| `nm build/meson/t_sysinfo.elf` | X | `T sysconf` — the archive member is pulled in |
| `scripts/run-host-tests.sh` | H | **103/103 PASS** |
| `scripts/check-layering.sh` | H | OK, with the `compat/` checks; and rejects a planted violation |
| `scripts/check-no-abs-paths.sh` | H | OK |
| `scripts/check-rust-target.sh` | H | OK |

### Phase 3 exit criteria — state

| Criterion (`docs/milestones.md`) | State |
|---|---|
| Each item is a separate patch file with a header explaining it (X) | ✅ 10 patches, four-field header each |
| Mesa configures for `horizon` and builds the non-driver core (X) | ✅ **configure exit 0; 379/379 edges, 10/10 libraries** |
| No patch mixes functional change with formatting | ✅ |

### Known gaps at the end of item 6

- ~~**`t_sysinfo` has not run on hardware.**~~ **Closed 2026-07-27:**
  PASS 21/21 in both process modes. It bounds the page size from both
  sides now — the earlier claim that divisibility alone caught an
  understated page size was wrong, and the lower bound it was missing is
  measured (`svcMapMemory` accepts exactly `0x1000`).
- ~~**`util_cpu_detect` reports 1 CPU on Horizon.**~~ **Fixed
  2026-07-27** by `mesa-patches/0012` plus `_SC_NPROCESSORS_ONLN` /
  `_SC_NPROCESSORS_CONF` in `compat/sysconf.c`; `u_cpu_detect.c.o` now
  references `sysconf`. The value it produces is **not yet measured on a
  console** — that is what `t_threads` is for. See "Phase 3 — closing
  items 1, 2, 4, 5 and 7".
- ~~Milestone items 2 and 4 are untouched; items 1, 5 and 7 still carry
  only the minimum each needed.~~ **Closed 2026-07-27**, in the same
  section: item 2 with a measurement saying no patch is warranted, item
  4 with a decision and patch 0011, items 1, 5 and 7 with evidence and
  (for 1) patch 0012.

---

## Phase 3 — item 3, newlib/libnx gaps (2026-07-26)

No code in `horizon/` was touched. Everything here is **cross build
(X)** or **host (H)**. Nothing in this phase says anything about
behaviour on a console.

### First: a defect in our own cross file

`toolchain/horizon-aarch64.cross` carried `-Wall -Wextra -Werror` in
`c_args` and `cpp_args`. Meson hands `[built-in options]` compiler args
to its **detection snippets** as well as to the build, and those
snippets are not written to survive `-Wextra -Werror`. Measured by
configuring the pinned Mesa tree with and without them, command lines
otherwise identical:

| Check | with `-Werror` | without | Cause inside Meson's snippet |
|---|---|---|---|
| `sizeof(void*)` | **-1** | 8 | `-Werror=unused-variable` |
| GCC atomic builtins | **NO** | YES | `-Werror=uninitialized` |
| `struct timespec` | **NO** | YES | `has_header_symbol` emits `#ifndef struct timespec` → *extra tokens* |
| strtod locale support | **NO** | YES | `-Werror=unused-variable` |
| gc-sections links | **NO** | YES | `-Werror=unused-function` |
| **GNU `qsort_r` links** | **NO** | YES | *not previously known* — `-Werror` in the `cpp.links` probe |
| `-ffunction-sections` / `-fdata-sections` supported (C and C++) | **NO** ×4 | YES | `get_supported_arguments` probes |

Six false answers, not five, plus four support probes silently reporting
"unsupported". Left alone, Mesa would have configured *successfully*
with `USE_GCC_ATOMIC_BUILTINS`, `HAVE_STRUCT_TIMESPEC` and
`HAVE_GNU_QSORT_R` off and without `--gc-sections` — configured cleanly
and wrong.

**Fixed by moving the warning policy to `meson.build`**
(`add_project_arguments(..., language : 'c')`), which Meson does *not*
pass to detection snippets. `language : 'c'` only: `project()` declares
just C, and `add_project_arguments` rejects a language the project does
not have. `warning_level = '0'` and `werror = false` stay in the cross
file so Meson still adds no `-Wall` of its own. The `Makefile` was not
touched and already carries the same three flags.

**Verified the hard way, the Phase 2 method.** Both sides rebuilt from a
deleted build directory — an incremental rebuild need not notice a
cross-file edit. All ten `.nro` **byte-identical**: same sha256, size,
`nm` symbol set, `.bss` symbols and sizes, section sizes; the `.elf`
files are identical too. The generated compile line still carries
`-Wall -Wextra -Werror` on every object. Re-checked again at the end of
the session, after the `PATH` change below: still identical.

Also corrected in this file: the Phase 2 note blaming
`needs_exe_wrapper = true` for `void* : -1`. Meson's size check is a
compile-time binary search; the exe wrapper was never involved.
`needs_exe_wrapper` is untouched and remains correct.

### `mesa-patches/` now has mechanics

`mesa-patches/README.md` defines the convention: a numbered
`git format-patch` series applied on top of `MESA_COMMIT`, file order is
apply order, the commit subject is the patch's identity, and every patch
carries a four-field header — milestone item, why, the **measurement**
that justifies it, and whether it is upstreamable and on what grounds.

`scripts/apply-mesa-patches.sh` applies what is missing and nothing
else. "Already applied" is decided by matching the subjects in
`git -C mesa log MESA_COMMIT..HEAD` against the subjects `git mailinfo`
extracts from the patch files (which unfolds wrapped `Subject:` lines
and strips `[PATCH n/m]`). The applied commits must be a *prefix* of the
series; divergence is reported, never repaired by guessing. Every write
path is guarded first — `mesa/.git` tested as a directory rather than by
asking git (the Phase 2 incident at the end of this file),
`--absolute-git-dir` asserted to be literally `$PWD/mesa/.git`,
`MESA_COMMIT` an ancestor of `HEAD`, clean tree, and a clear error for
the archive path that has no `.git`.

`scripts/fetch-mesa.sh` gained the matching guard, because the
interaction bites: after a `git am` the tree is **clean** and `HEAD` is
`MESA_COMMIT + N`, so neither `at_pinned_commit` nor `mesa_dirty` fired
and the next fetch would have checked the tag back out, silently
un-applying the series. It now recognises that state and requires
`--force` to reset.

`scripts/check-no-abs-paths.sh` also scans `mesa-patches/` now — a patch
is a build input, and diff output is a place an absolute path arrives
without anyone typing it. `check-layering.sh` deliberately does **not**:
it greps for `drm_nouveau_*`/`drmSyncobj*`, which Phase 4's
`nvkmd_horizon` patches will legitimately mention.

`scripts/configure-mesa.sh` and `scripts/build-mesa.sh` make the loop
reproducible in one line each.

### The patch series (8 patches)

Every one is formulated as a property of the **C library or the
compiler**, not as an OS name. That is what makes them upstreamable and
what stops the series growing a Horizon special case per file.

| # | Patch | Item | What it replaces |
|---|---|---|---|
| 0001 | `meson: do not require libdl where there is no dynamic loader` | 3 | `find_library('dl', required : true)` → optional, plus `-DHAVE_DLOPEN` |
| 0002 | `util/u_dl: gate the dlfcn path on HAVE_DLOPEN` | 3 | `DETECT_OS_POSIX_LITE` → `HAVE_DLOPEN` |
| 0003 | `c11/threads_posix: detect pthread_mutex_timedlock` | 3 | `!__CYGWIN__ && !__APPLE__ && !__NetBSD__` → a configure check |
| 0004 | `util/u_endian: fall back to __BYTE_ORDER__` | **7** | adds a last resort after the libc branches |
| 0005 | `meson: detect whether the C library needs _GNU_SOURCE` | 3 | an OS list carrying Mesa's own `TODO: this is very incomplete` |
| 0006 | `util/log: include u_process.h for the use that is not POSIX-guarded` | 3 | a guard mismatch (`!DETECT_OS_WINDOWS` use, `DETECT_OS_POSIX` include) |
| 0007 | `util/detect_os: add DETECT_OS_HORIZON` | **1** (minimum only) | nothing — fills a case that fell through |
| 0008 | `util/os_time: sleep with usleep on any POSIX-lite platform` | **5** | `DETECT_OS_POSIX` → `DETECT_OS_POSIX_LITE` |

None adds a fallback that did not already exist. `u_dl.c`'s
`NULL`/`"unknown error"` branches and `threads_posix.c`'s trylock loop
were both already in their files and simply unreachable.

### Every gap, with the destination chosen and why

| Gap | Measured | Destination | Why that one |
|---|---|---|---|
| `libdl` / `dlopen` | `dlopen : NO`, `dlfcn.h : NO`, configure **error** | patch 0001+0002 | Mesa assumed dlopen is in libc or libdl. Having a loader is a libc trait; the degraded path existed already |
| `pthread_mutex_timedlock` | **undefined reference** even with a hand-written prototype; absent from `libc.a`, `libpthread.a`, `libnx.a`; `pthread.h` declares it only `#if defined(_POSIX_TIMEOUTS)`, undefined here even after `<unistd.h>` | patch 0003 | Genuinely missing, so `compat/` was open — but Mesa's own C11-threads shim already has the fallback, and a global libc symbol would silently give every future consumer our polling implementation. Narrower and reviewable inside Mesa |
| `mkostemp`, `asprintf` not declared | `mkostemp : YES` by link test, yet `implicit declaration` at compile | patch 0005 | Not a missing function — a **visibility** gap (`__GNU_VISIBLE`). Exactly why an OS list gets it wrong |
| `endian.h` absent, no branch matched | `endian.h : NO`; `#error "UTIL_ARCH_… were unset."` | patch 0004 | The compiler answers the question directly on every target |
| `DETECT_OS_*` all zero | `#error Unsupported OS` ×2 in `os_time.c` | patch 0007 + 0008 | An OS genuinely needs an identity; kept to POSIX-lite and to what item 5 needed |
| `util_get_process_name` undeclared | `-Werror=format=` on the adjacent `%s` | patch 0006 | Pre-existing Mesa guard mismatch, not a newlib gap at all |
| `sys/mman.h` absent (`disk_cache`) | `flock`, `posix_fallocate`, `memfd_create` all `NO` | **`-Dshader-cache=disabled`** | Optional on-disk cache, not part of the non-driver core, and unwanted on Switch as it stands. Both files are wholly inside `#ifdef ENABLE_SHADER_CACHE`. Recorded as a decision, in `configure-mesa.sh` with its reason |
| bundled googletest vs newlib | `fileno`, `strdup`, `fdopen`, `::mkstemp` not declared under `-std=c++17` | **excluded, with the failure recorded** | `build-tests` defaults to false, `libgtest` is `build_by_default : false`, and every `idep_gtest` user is inside `if with_tests`. It is the unit-test framework, and its tests cannot run here anyway (`needs_exe_wrapper`, no emulator) |
| `sysconf`, `getpagesize`, total RAM | both **undefined reference** by link probe | **not done — item 6** | See below |

**Nothing went to `compat/`.** It is still empty, so
`scripts/check-layering.sh` still does not scan it. If it ever gains
content the gate must gain `compat/` in checks 6–8 — the `--wrap` one
above all, since `compat/` is exactly where interposition would be
reintroduced — plus a check that `compat/` includes no Mesa/NVK/Vulkan
**or `horizon/`** headers.

### Deviations from the item 3 scope

Three patches are not item 3, and are not filed as if they were. Each
was the only thing left stopping the core, and each is a one-line
general fix:

- **0004 — item 7 (endianness).** Additive, unreachable on every
  platform already handled.
- **0007 — item 1 (OS detection), minimum only.** `DETECT_OS_HORIZON`
  from `__SWITCH__`, as POSIX-lite. Nothing else from item 1.
- **0008 — item 5 (timers/clocks).** `DETECT_OS_POSIX` →
  `DETECT_OS_POSIX_LITE`; POSIX implies POSIX-lite, so nothing existing
  changes.

`-Dshader-cache=disabled` is a fourth deviation of a different kind: a
configure decision, not a patch, recorded rather than hidden.

### Where item 3 stops, and why

`src/util/os_misc.c` is the last failing object. Three `#error`s, all
milestone **item 6** (physical memory / page size queries):

```
os_misc.c:81:2:  #error unexpected platform in os_sysinfo.c
os_misc.c:407:2: #error unexpected platform in os_misc.c   (os_get_total_physical_memory)
os_misc.c:507:2: #error unexpected platform in os_sysinfo.c (os_get_page_size)
```

There is no libc route to either answer here — **`sysconf` and
`getpagesize` are both genuinely absent**, each verified by a link probe
(`undefined reference`), so `os_get_page_size`'s `HAVE_SYSCONF` path is
unavailable. What is left needs two facts about Horizon this session
cannot measure: the CPU page size, and total physical memory. Neither
has a source that does not either require hardware or drag libnx into
Mesa's generic `src/util`, which would be un-upstreamable and would blur
the layering. `horizon/`'s `HORIZON_GPU_SMALL_PAGE_SIZE` (0x1000) is the
**GPU MMU** small page, cited from nvgpu/nvmap — a different quantity,
and conflating the two would be a guess wearing a citation.

Writing an unmeasured constant here is the same mistake this session
opened by fixing. Item 3 stops here with item 6 scoped instead.

### Commands run and results

| Command | Class | Result |
|---|---|---|
| `scripts/configure-horizon.sh && scripts/build-horizon.sh` (before and after the warning move, `rm -rf build/meson` both times) | X | 10/10 `.nro`, **byte-identical**; `diff -r` of sha256 + size + `nm` + `.bss` + section sizes empty |
| `scripts/fetch-mesa.sh` | H | `mesa-26.1.5`, HEAD verified `6a02618ccf6c…`, 503 MB |
| `scripts/apply-mesa-patches.sh` on a reset `mesa/` | H | applied 8/8 |
| `scripts/apply-mesa-patches.sh` again | H | `all 8 patches already applied; nothing to do`, exit 0, nothing written |
| `scripts/apply-mesa-patches.sh --list` | H | 8 applied, 0 pending |
| `scripts/fetch-mesa.sh` with the series applied | H | recognises `MESA_TAG plus 8 local commit(s)`, exits 0 without resetting |
| `scripts/configure-mesa.sh` | X | **exit 0** — `void* : 8`, `GCC atomic builtins : YES`, `struct timespec : YES`, `gc-sections : YES`, `GNU qsort_r : YES`, `zlib : YES 1.3.1`, `dlopen : NO` / `dladdr : NO` / `dl_iterate_phdr : NO` with no `-ldl` and no `-DHAVE_DLOPEN` in the build |
| `ninja -k 0` over the ten core libraries | X | **325/326 objects**; 9/10 libraries archived; `libmesa_util` 89/90; the one failure is `os_misc.c` |
| `scripts/run-host-tests.sh` | H | **103/103 PASS** (6 suites) |
| `scripts/check-layering.sh` | H | OK |
| `scripts/check-no-abs-paths.sh` | H | OK (now over `mesa-patches` too) |
| `scripts/check-rust-target.sh` | H | OK |

Failures reproduced deliberately and left recorded rather than worked
around: the libdl stop, every compile error in the table above, the
gtest failure, and `os_misc.c`.

### Two toolchain gaps found by building Mesa, not by reading

Neither is a Mesa fault. Both are the same shape as the Phase 2 finding
that the image's `PATH` omits `devkitA64/bin`.

1. **`portlibs/switch/bin` was not on `PATH`.** Mesa tried to *download*
   zlib (`zlib.net`, then wrapdb) and failed — containers here have no
   network. zlib was installed the whole time: `zlib.h`, `libz.a` and a
   `zlib.pc` reporting 1.3.1 are in `portlibs/switch`. What was missing
   was `aarch64-none-elf-pkg-config`, devkitPro's wrapper that points
   pkg-config at the Switch portlibs, which lives in that directory. The
   cross file names its `[binaries]` unqualified on purpose, so
   `pkg-config` did not resolve and every `dependency()` found nothing.
   Recorded as `HORIZON_PORTLIBS_BINDIR_REL`; `horizon_run` prepends it
   last so it cannot shadow a cross tool. The ten `.nro` were rebuilt
   after this change and are still byte-identical.
2. **The pinned Meson launcher had the host's interpreter baked into its
   shebang.** pip writes the installing machine's path
   (`/usr/local/bin/python3`), which does not exist in the image.
   `horizon_meson` sidesteps it by running the launcher through
   `python3`, but Meson's own `--internal exe` wrapper — which every
   `custom_target` that captures output goes through — re-invokes it *by
   path* from `/bin/sh`. Every generated Mesa source failed with
   `/bin/sh: 1: .../bin/meson: not found`. `horizon_ensure_meson` now
   rewrites it to `/usr/bin/env python3`, idempotently. This is the
   same rule `check-no-abs-paths.sh` enforces on tracked files, applied
   to a generated one the gate cannot see.

### Phase 3 exit criteria — state

| Criterion (`docs/milestones.md`) | State |
|---|---|
| Each item is a separate patch file with a header explaining it (X) | ✅ 8 patches, four-field header each, convention in `mesa-patches/README.md` |
| Mesa configures for `horizon` and builds the non-driver core (X) | ⚠️ **configures: yes** (exit 0). **Builds: 325/326 objects, 9/10 libraries.** `os_misc.c` remains, blocked on item 6 |
| No patch mixes functional change with formatting | ✅ |

Items 2, 4 and 6 are untouched, and items 1, 5 and 7 have only the
minimum each needed above. Item 8 was already closed in Phase 2 without
a patch.

---

## Phase 1 (previous phase)

**`horizon/` standalone GPU layer. Hardware-verified through
the Codex review round (see below); a second, owner-authored review
round (2026-07-26, same day) found 20 further issues in `horizon/` and
`tests/`, now fixed (host + cross build green, host tests 81 -> 103) but
NOT yet re-run on hardware — see "Second review round" below. Treat
Phase 1 as verified-pending-reconfirmation again until that re-run
happens; the changed paths include channel creation/destroy, vm_map,
device big-page-size handling and the GPFIFO command emitters, several
of which are exercised by every other test.**

Following the 9 Codex review fixes in `2dc8513` (see "Codex PR review"
below), the owner rebuilt and re-ran all ten `.nro`s on a real Switch and
confirmed all ten pass. This round was reported as a verbal confirmation
("los 10 dan positivo") without captured logs or per-test PASS/FAIL
counts, unlike the first hardware round below — recorded here as such,
not padded with numbers that were not actually reported. This closes the
"verified-pending-reconfirmation" state the Codex fixes had left Phase 1
in, in particular for the two paths that changed behaviour materially:
`t_channel`'s notifier handling (now matching `KERNELRESULT(TimedOut)`
specifically) and `t_submit`'s R10 measurement (rebuilt as a real
producer/consumer cross-channel wait).

The owner ran all ten `.nro`s on a real Switch (logs received
2026-07-26); 8/10 passed outright and the two failures were fixed
(a wrong assumption about `GetErrorNotification` semantics in
`channel.c`, and a benign race in the t_teardown test). The
**confirmation re-run passed both**: `t_channel` PASS 17/17 (notifier
now reports `status=ok type=0 'none'`; syncpt value at create 58686 —
further R5 evidence the counter persists across runs) and `t_teardown`
PASS 28/28 (both cycles landed on the work-already-retired side of the
race; retirement callbacks ran exactly once; leak refusal and zero
counters held in both cycles). Evidence: console screenshots from the
owner (the second run's verdicts were captured on screen; the sdmc log
files were reported missing — note the tests write them to
`sdmc:/horizon_gpu_tests/` at the SD root, not next to the `.nro`s).

---

## Hardware run results (real Switch, owner-executed, 2026-07-26)

| # | Test | Result | Key output |
|---|------|--------|-----------|
| 1 | t_init | **PASS 22/22** | gm20b arch=0x120 impl=0xb rev=0xa1, 1 GPC × 2 TPC, L2 0x40000, va_bits=40, big_page=0x20000 (avail 0x30000), classes 3d=0xb197 compute=0xb1c0 2d=0x902d gpfifo=0xb06f i2m=0xa140 copy=0xb0b5; small region base=0x8000000 pages=0x3f7fff; big region base=0x400000000 pages=0xdffff |
| 2 | t_alloc | **PASS 21/21** | rounding, alignment, every overflow rejection |
| 3 | t_nvmap | **PASS 16/16** | ids/handles valid and distinct, close path healthy |
| 4 | t_va_reserve | **PASS 17/17** | small base=0x8000000; big base=0x400000000 inside queried region; oversized reservation → clean nv error **0x275c** (R8 exhaustion behaviour) |
| 5 | t_map | **PASS 26/26** | fixed VA honoured, cleared-VA invariant holds, remap after unmap works, **Generic_16BX2 kind OK**, **big-page (0x20000) map OK** (R9) |
| 6 | t_channel | FAIL 16/17 → **fixed** | syncpt id=26, **value at create=14064** (R5: counters NOT reset at channel creation — shadow-from-read design is required and correct). The one FAIL: `GetErrorNotification` returns a failed Result on a healthy channel — Horizon semantics are "error when nothing pending"; get_error now treats that as "none" |
| 7 | t_submit | **PASS 23/23** | fence-only submit executes (validates the WFI+SYNCPOINTA/B increment encoding, R4); NOP list executes; **2 submits in flight, both issued in 148 µs, no CPU wait**; SET_OBJECT binds complete without fault (R7); **R3: entry flags 0 accepted and completed**; **R10: GPU-side syncpoint wait completed — encoding VALIDATED** |
| 8 | t_syncpt | **PASS 48/48** | exactly +1 per submit across 10 submits (initial value 26545); shadow agrees with hardware |
| 9 | t_fence_wait | **PASS 14/14** | wait-after-completion, prompt signalled return, 200 ms timeout honoured without unit inflation, zero-timeout poll |
| 10 | t_teardown | FAIL 31/32 → **test fixed** | cycle 1: in-flight destroy refused (BUSY) as required; cycle 2: the NOP work had already retired so the probe destroy legitimately succeeded and the test then double-destroyed. Both outcomes are legal; the test now handles the race. All leak accounting held in both cycles |

### Measurements recorded (design consequences)

- **R5 answered:** syncpoints are NOT reset at channel creation (14064 /
  26545 observed). Initialising the 64-bit shadow from a hardware read is
  mandatory, and is what the code does.
- **R10 answered:** the SYNCPOINTA/B GPU-side wait encoding works on the
  Horizon nv path. Cross-channel GPU dependencies (Phase 4) are unblocked.
- **R3 revised:** GPFIFO entry flags 0 **work** through our submit path —
  the reference's NOT_MAIN|NO_PREFETCH folklore is not reproducible on
  this code. Default stays NOT_MAIN|NO_PREFETCH (matches the only
  hardware-proven full stack) until Phase 4 retests with real engine
  workloads; recorded as measured, not inherited.
- **R4 validated:** one increment requested = one increment observed,
  48/48.
- **R8 data:** oversized ALLOC_SPACE fails cleanly with nv Result 0x275c.
- **Horizon semantics finding:** `NVGPU_IOCTL_CHANNEL_GET_ERROR_NOTIFICATION`
  fails when no notification is pending (fresh-channel measurement);
  `horizon_gpu_channel_get_error` treats that failure as "none".

---

## Fixes after the first run — CONFIRMED on console

1. `horizon/channel/channel.c` — `get_error` treats a failed
   GetErrorNotification as "no notification pending" (cites the
   measurement). Confirmed: `t_channel` PASS 17/17 on the re-run.
2. `tests/t_teardown.c` — the in-flight-destroy probe accepts both legal
   outcomes (BUSY while in flight / success when already retired) without
   double-destroying. Confirmed: `t_teardown` PASS 28/28 on the re-run
   (both cycles took the already-retired branch).

---

## Tests executed in this environment (unchanged classes)

| Command | Class | Result |
|---|---|---|
| `scripts/run-host-tests.sh` (gcc 13.3, x86_64, ASan+UBSan) | host build+run | 78/78 PASS (h_align 20, h_va_space 21, h_syncpt_math 19, h_cmds 18) |
| `scripts/check-layering.sh` | host run | OK |
| `docker run … nx-dev make all -j4` (post-fix) | cross build | exit 0, 10/10 `.nro`, `-Wall -Wextra -Werror` clean |

---

## Phase 1 exit criteria — verification state

| Criterion | State |
|---|---|
| Ten tests cross-compile (X) | ✅ |
| Pure logic builds/runs on host (H) | ✅ 103/103 |
| Layering gate clean | ✅ |
| Tests 1–10 pass on hardware (HW) | ✅ **all ten PASS, on the current code, in both process modes** (2026-07-27 — see "Hardware run of all eleven `.nro`" above). Applet and full/game agree test for test; the counts are higher than the Phase 1 run because the second review round added assertions to exactly these paths |
| ≥2 submits in flight without CPU wait (test 7) | ✅ **re-measured on hardware**: both submits issued in **149 µs** with no intervening CPU wait, and the bound is now `t_check`ed rather than noted (148 µs at `732b58c`) |

**Every Phase 1 exit criterion is met, and the two hardware ones are no
longer stale.** They were measured at `732b58c` and `horizon/` changed
afterwards in `747b915`, which is why they sat at ⚠️ through Phases 2
and 3 — a ✅ would have claimed console evidence for code no console had
run. The 2026-07-27 run closes that: it exercised the current code,
including every path the second review round touched (channel
create/destroy, `vm_map`, device big-page handling, the GPFIFO
emitters), and found no regression in either mode.

---

## Known failures / limitations

- **R6 (cache coherency) still unmeasured** — needs a GPU write primitive
  (inline-to-memory/DMA), deferred to the first GPU write (early
  Phase 4/5). Deliberate, recorded deviation.
- **Kickoff rejection code map** still unpopulated: no kickoff was ever
  rejected during the run (good), so BUSY-vs-other classification remains
  based on the libnx-side queue precheck only.
- The submit rollback paths touch libnx's public `NvGpuChannel` fields;
  correct against libnx as pinned in the nx-dev image.

## Pending decisions

| # | Decision | State |
|---|---|---|
| D1 | Literal reuse from GPL/AGPL reference | **no**; nothing copied |
| D4 | Switch available | **yes — closed.** Full run + confirmation re-run done |
| D2 | Mesa version to pin | **closed at Phase 2 start: `mesa-26.1.5`** @ `6a02618ccf6c5651ecb9cccbde571eb61fd73592` |
| D3 | Mesa checkout mechanism | **closed at Phase 2 start: script-fetched**, not a submodule |
| D5 | Cache policy per memory type | blocked on R6 (first GPU write) |
| D6 | Timeline semaphores vs upload queue | Phase 4 |

### D2 — Mesa version: `mesa-26.1.5`

Current stable series at Phase 2 start (released 2026-07-15; 26.2 had
branched but was only at rc2). Chosen over the reference port's 25.0.7
for a modern `nvkmd` surface — which is exactly the interface Phase 4
implements — and a credible upstreaming path. This is the
recommendation `docs/known-risks.md` R14 already carried.

Accepted cost: line references in `docs/reference-analysis.md` describe
a 25.0.7 tree and are now approximate. They still point at the right
file and concept.

Pinned as `MESA_TAG` / `MESA_COMMIT` in `toolchain/versions.env`. The
commit, not just the tag, because tags can be moved;
`scripts/fetch-mesa.sh` verifies the SHA it actually got.

### D3 — Mesa checkout: script-fetched, not a submodule

`mesa/` stays gitignored and is populated by `scripts/fetch-mesa.sh`.
Reasons, in order of weight:

1. `docs/milestones.md` Phase 2 item 8 says *every* version is pinned in
   `toolchain/versions.env`. A submodule splits the Mesa pin between a
   gitlink and that file — two sources of truth that can disagree.
2. Phase 3 applies `mesa-patches/` into `mesa/`. A patched submodule is
   permanently dirty, and its recorded SHA can drift by accident.
3. A clone with `--recursive` would pull the full Mesa history for
   everyone, including CI, whether or not they build Mesa.

The fallback the submodule option was meant to provide is kept anyway:
if git-over-https is blocked, the script downloads GitLab's archive
addressed by *commit*, so the content is the pinned commit by
construction.

Measured here: `git ls-remote` and `git fetch` against
`gitlab.freedesktop.org` both work from this environment, so the git
path is the one actually exercised (503 MB checked out, SHA verified).

---

## Codex PR review (2026-07-26) — findings addressed

`chatgpt-codex-connector[bot]` reviewed commit `b1c53f0a5c` (PR #1) and left
9 comments. All are real; fixed here (host + cross build clean, `-Wall
-Wextra -Werror`, host tests 78/78, layering gate OK) — **hardware re-run
still pending**, since several touch code paths already measured on
console (t_channel's notifier fix, t_submit's R10 measurement).

| # | File:line | Finding | Fix |
|---|---|---|---|
| 1 | `tests/t_submit.c:192` (P2) | R10 test waited on the consumer's own already-reached syncpoint — proved nothing about the encoding | Redesigned as a real producer/consumer pair: wait on a *future* threshold on a different channel's syncpoint, assert a short wait times out while unresolved, then assert it unblocks after the producer submits. Re-measures R10; the old "encoding VALIDATED" note undersold what was actually proven |
| 2 | `horizon/channel/channel.c:432` | `wait_fence` read `chan`'s own syncpoint regardless of `fence.syncpt_id`, so a foreign-channel fence could compare the wrong counter | Validate `fence.syncpt_id == chan->syncpt_id`, matching `add_retirement`'s existing check |
| 3 | `horizon/channel/channel.c:513` (P1) | A lost channel with pending retirements could never destroy: `!chan->lost` gated both the busy check *and* the reap, and nothing ever clears `retire_count` | Always reap (a plain syncpt read, harmless on a lost channel); if entries remain on a *lost* channel, force-retire them (documented as "abandoned", not "completed" — no cancellation ioctl exists) instead of refusing destroy forever |
| 4 | `horizon/vm/vm.c:269` | Unmapping the most-recently-created mapping zeroed `mem->mapped_va` even if an older mapping of the same object was still live, contradicting the documented "0 = no live mapping" contract | Added an intrusive most-recent-first list of live mappings per `horizon_gpu_mem` (`mem_priv.h`/`vm_priv.h`); unmap now restores `mapped_va` to another live mapping if one remains |
| 5 | `tests/t_submit.c:116` | If `make_nop_list` failed, `span` was uninitialised but the next section submitted it anyway | Gated the multi-submit section on the same success flag as the NOP-list section |
| 6 | `horizon/submit/submit.c:133` (P1) | Shadow/kernel fence mismatch was logged but still returned success with the (untrustworthy) shadow-derived fence | Marks the channel lost and returns `HORIZON_GPU_ERR_CHANNEL_LOST` instead — the submit already reached hardware and cannot be undone, but no caller gets a fence it can't trust |
| 7 | `horizon/channel/channel.c:81` | `get_error` treated *every* failed `GetErrorNotification` as "no error", which would mask a genuine service/driver failure as a healthy channel | Verified against libnx source (`nx/source/nvidia/gpu_channel.c`, fetched from github.com/switchbrew/libnx): "no notification pending" is the non-blocking `eventWait(..., 0)`'s own `KERNELRESULT(TimedOut)`, not an nv error. Now matches that exact result only; anything else propagates as a real failure |
| 8 | `horizon/device/device.c:140` | When `as_big_page_size` selected a non-default (but valid) size, `dev->info.big_page_size` kept the characteristics default, so `vm_page_size_valid` validated reservations against the wrong value | Store the effective `as_big_page` into `dev->info.big_page_size` before `nvAddressSpaceCreate` |
| 9 | `horizon/vm/vm.c:210` | If `MapBufferEx` returned an unexpected VA *and* the cleanup `UnmapBuffer` also failed, the interval and mapping bookkeeping were freed anyway — an orphaned kernel mapping with the device/mem counters reporting no leak | On that double-failure, keep the VA interval and both live-mapping counters non-reusable/elevated and return `HORIZON_GPU_ERR_LEAK` instead of freeing the bookkeeping |

Also fixed while rebuilding: `Makefile`'s per-recipe `mkdir -p $(dir $@)`
raced under `make -j4` on this toolchain image's overlay filesystem — one
object file's compile silently never ran and `ar` failed. Object
directories are now order-only prerequisites created by a single rule.
Confirmed: `-j4` clean after the fix (previously reproduced the failure
twice, `-j1` always worked).

**Re-run on hardware:** owner-confirmed all ten `.nro`s pass after these
fixes (2026-07-26, verbal confirmation, no logs/per-test counts captured
this round — see "Current phase" above).

---

## Second review round (2026-07-26) — findings addressed

The owner posted a second, more detailed review (PR #1 comment
`5085013365`, 20 findings across `horizon/` and `tests/`) after the Codex
round above and its hardware reconfirmation. Each finding was checked
against the actual code before acting — 18 were real and are fixed; 2
were investigated and found not applicable given evidence already
recorded above.

| Finding | Fix / disposition |
|---|---|
| `mem_destroy`/`vm_release`/`vm_unmap`/`channel_destroy` poisoned a field then freed the struct anyway — a second call reads through freed memory, so "double-destroy fails INVALID_ARG, not UAF" was false | Removed the dead poison writes and the false claim; double-destroy is caller UB per the single-owner contract (memory-model § 7), not defended against |
| `horizon_gpu_submit`'s back-pressure guard could overflow with a large `num_spans`, and the per-span validation loop read `spans[]` out of bounds before any capacity check ran | `num_spans` is now bounded against `GPFIFO_QUEUE_SIZE` before either happens |
| `horizon_gpu_channel_create`'s error-unwind path discarded every teardown `Result` | Each unwind step's failure is now logged (`channel_create_unwind_step`) |
| `horizon_gpu_vm_map` never checked `mem` and `range` belong to the same device | Rejected with `HORIZON_GPU_ERR_INVALID_ARG` |
| `device.c` overwrote the queried, "never defaulted" `info.big_page_size` with the AS-effective size (this was itself finding #8 of the *first* Codex round) | Split into `big_page_size` (hw default, untouched) and a new `as_big_page_size` (what `vm_page_size_valid` validates against) |
| Every channel reserved/aligned 128 KiB of VA (`CHANNEL_ZCULL_ALIGN`) even without Zcull, for a 4 KiB cmdbuf | Reservation only steps up to `CHANNEL_ZCULL_ALIGN` size/alignment when `bind_zcull` is requested |
| `horizon_cmds_nop` had no buffer-capacity bound (public entry point, unbounded write) | Added a `buf_dwords` parameter; rejects rather than overruns |
| `horizon_cmds_set_objects` truncated an out-of-range class number (`& 0xFFFF`) into a different, valid-looking class instead of rejecting it | Rejects (returns 0) instead of truncating |
| `horizon_cmd_hdr_incr` had no field masks — an out-of-range count/subch/method would bleed into an adjacent bit field | Masked each field |
| No compile-time check that the cmdbuf's fence-increment and SET_OBJECT lists cannot overlap | Added `_Static_assert` |
| `device_priv.h`'s atomics comment overstated thread-safety (claimed concurrent create/destroy "keeps counts exact" generally; the structures the counters describe are not synchronized) | Corrected to state the actual guarantee |
| `t_teardown.c` used `dev` unconditionally after a `device_destroy(dev)` call whose success path (if every child creation had failed) would have freed it | Bail out immediately if that call does not return `HORIZON_GPU_ERR_LEAK` |
| `t_submit.c`'s "no CPU wait between submits" milestone criterion was only a `t_note`, never asserted | Now `t_check`ed against a 50 ms bound (hardware measured ~148 us) |
| `t_map.c` never asserted the sibling-mapping VA fallback it exists to prove — only checked the fully-unmapped end state | Added an assertion right after unmapping the newer sibling: `mapped_va` must equal the still-live mapping's VA |
| `t_va_reserve.c`'s two rejection probes reused the live `r1` out-param, relying on the undocumented fact that `vm_reserve` never touches `*out_range` on failure | Uses a scratch out-param instead |
| Several `Result`s discarded in tests (`t_submit.c` syncpt_read/mem_flush/get_error, `t_syncpt.c` syncpt_read, `t_teardown.c` mem_flush/add_retirement) | Checked and asserted |
| `t_init.c`'s comment said GM20B values were "plausibility bounds, not hard requirements" immediately above a hard `strcmp(chipname, "gm20b")` check | Corrected: this project targets GM20B specifically, so chipname/has_syncpoints/page-size-consistency are genuinely hard requirements; GPC/TPC counts and engine class numbers are the actual plausibility bounds |
| `check-layering.sh` only checked one direction (nothing under `horizon/` includes Vulkan/Mesa/DRM/nwindow) | Added: `horizon/include/` headers themselves stay libnx-free (no `switch.h`, no `Nv*`/`Result` types in code lines), plus greps for rejected designs #1–3 (`/dev/dri`, `-Wl,--wrap`, nouveau uAPI symbols) |
| `status.c`/`log.c` are documented "pure C11, libnx-free" but had no host coverage | Added `tests/host/h_status.c`, `h_log.c` |
| `Makefile` used `-ffunction-sections` without the matching `-Wl,--gc-sections` | Added to `LDFLAGS` |
| **Investigated, not applicable:** the R8 VA-exhaustion probe "never reaches the kernel" | Contradicted by evidence already in this file: the measured `0x275c` nv error (Hardware run results table, test 4) proves the request *did* reach `AllocSpace`; recomputing the probe's actual page count against the measured region size (≈16 GiB + 4 GiB ≈ 5.2M pages) confirms it stays far under the `pages > UINT32_MAX` guard |
| **Investigated, not applicable:** `submit.c`'s shadow/kernel `fence.value` mismatch check (added in the *first* Codex round, finding #6) could "kill the first submit on every channel" if libnx's fence base and our shadow disagree | Both bases are hardware syncpoint reads at channel creation; the confirmed hardware re-run (all ten `.nro`, including every `t_submit`/`t_channel`/`t_syncpt` submit) never triggered this path, which is the empirical answer to the exact R5 question this check depends on |

Verified here: host tests 81/81 -> 103/103 (2 new suites), layering gate
clean with the added checks, cross build clean (`-Wall -Wextra -Werror`)
at `-j1`, all ten `.nro` produced (`-j4` intermittently hits the same
overlay-filesystem directory race noted above; pre-existing, not a
regression from this round). **Not yet re-run on real hardware.**

---

## Phase 2 — toolchain (2026-07-26)

No code in `horizon/` was touched. This phase is toolchain only.

### Environment facts (measured, not assumed)

| Resource | State |
|---|---|
| `pkg.devkitpro.org`, `apt.devkitpro.org` | **403** — devkitPro cannot be installed or queried over the network here |
| `gitlab.freedesktop.org` | reachable; `git ls-remote`, `git fetch` and `/-/archive/` tarballs all work |
| `github.com` raw / codeload | **403 / 404** |
| `pypi.org` | reachable |
| Containers (`--bridge=none`) | **no network**; anything fetched must be fetched on the host and mounted in |

### What is pinned, and what deliberately is not

**Owner's decision, taken during this phase: the Switch toolchain
belongs to the environment, not to this repository.** libnx, devkitA64
and the portlibs are neither pinned nor updated from here — they are
whatever `$DEVKITPRO` points at, or whatever is inside the container
image the developer runs. Updating libnx is `dkp-pacman -Syu` or a newer
image, not an edit to this tree. libnx moves fast and this backend is
written against exactly the `nv` services it exposes, so following it is
worth more than freezing it.

`toolchain/versions.env` is split accordingly:

| Half | Contents |
|---|---|
| ENVIRONMENT | How to *reach* the toolchain only: image repo/tag, the devkitPro prefix inside it, the `PATH` quirk, the target triple. **No version of anything.** |
| PINNED | What this project chooses: Mesa (`mesa-26.1.5` @ `6a02618ccf6c`), Meson `1.11.2`, the Rust target name. |

That line is deliberate: inputs this project *chooses* are pinned; the
environment it *runs in* is not. Mesa stays pinned because
`mesa-patches/` applies to a specific tree and a Mesa that moved
underneath would break Phase 3 silently.

**What replaces pinning is recording.** `scripts/package-horizon.sh`
writes into `build/pkg/MANIFEST.txt`, per build: the resolved image
digest, the live `dkp-pacman -Q` output, the rustc banner, each
artefact's sha256, and the exact
`HORIZON_NX_IMAGE=…@sha256:… scripts/build-horizon.sh` command that
rebuilds against the same toolchain. **This is what keeps a hardware
result attributable** when the inputs are not frozen.
`scripts/print-toolchain-versions.sh` is a read-only reporter feeding
it; it compares against nothing and updates nothing.

Observed in this environment at the time of writing (recorded as
evidence, **not** as a pin): devkitA64 `r29.2-1`, gcc `15.2.0-7`
(`aarch64-none-elf-gcc (devkitA64) 15.2.0`), binutils `2.45.1-2`
(`GNU ld 2.45.1`), newlib `4.6.0.20260123-4`, switch-tools `1.13.1-1`,
libnx `4.12.0-1`, deko3d `0.5.0-1`, `rustc 1.99.0-nightly (008fa22ce
2026-07-25)`, image digest `sha256:61a38fe4…`.

Why a libnx version number would have been misleading even if we had
pinned it — R15 in concrete form: the image installs the libnx package
and then builds `switchbrew/libnx` git HEAD over it.
`dkp-pacman -Qkk libnx` → **`226 total files, 205 altered files`**, with
`libnx.a` and `libnxd.a` mismatching on size, MD5 *and* SHA256. The
package version describes 21 of 226 files.
`print-toolchain-versions.sh` prints that measurement every time, so the
limitation is visible rather than assumed.

Also recorded because it bites: the image's own `PATH` does **not**
include `devkitA64/bin`. Anything resolving the cross compiler by bare
name — which the Meson cross file does deliberately — must prepend it.

### R13 answered — no Rust sysroot is built

Full evidence in `docs/rust-toolchain.md`, taken from the checked-out
tree at `MESA_COMMIT`. Summary: `std` **is** required as Mesa links
NAK/NIL today (no `#![no_std]` anywhere in `src/`; both built with
`rust_abi : 'c'` = `--crate-type staticlib`, which bundles libstd), but
the dependency is seven sites deep, all with direct replacements, and
the files using `std::process`/`fs`/`env` are `#[cfg(test)]`-gated and
never reach the driver. Closing the gap is a small `mesa-patches/` job
for Phase 3/4.

Milestone items 3–5 (custom target JSON, `rustc` wrapper, std sysroot)
are therefore **not needed**: rustc already ships
`aarch64-nintendo-switch-freestanding` (tier 3, `os = "horizon"`,
`std = false`, `panic = abort`, `+v8a,+neon,+crypto,+crc`, PIE) matching
devkitA64's flags. `toolchain/aarch64-horizon.json` is committed only as
a drift snapshot, checked by `scripts/check-rust-target.sh`.

This is a **source-level** conclusion. No Rust has been compiled for
Horizon yet.

### Meson cross file and build

`toolchain/horizon-aarch64.cross` is committed with **no absolute
paths**: bare `[binaries]` names resolved through `PATH`, and a
`devkitpro` constant it deliberately never defines.
`scripts/gen-cross-file.sh` writes that constant — and only that — into
a gitignored three-line file. Verified that Meson shares `[constants]`
across every `--cross-file` on one command line, so the two compose
without duplication. devkitPro's own generator
(`$DEVKITPRO/meson-toolchain.sh`) bakes in `which`-resolved absolute
paths, which is exactly what the gate forbids; its `[host_machine]`
block is matched exactly (`horizon`/`aarch64`/`cortex-a57`/`little`).

`meson.build` builds `libhorizon_gpu.a` and the ten `.nro`. The
`Makefile` is untouched and remains the reference path — it produced the
artefacts verified on console.

**Comparing the two paths found one real divergence.** Meson appends
`-fPIC` to static-library objects (`b_staticpic` defaults true),
overriding the cross file's `-fPIE` for those objects but not for the
executables' own. Measured on `t_alloc`: 48 bytes of `.text`, 32 of
`.bss`. Fixed with `b_staticpic=false`, plus a hard error in
`meson.build` if it is ever true — because Meson applies
`default_options` only on a build directory's *first* configure, so
reconfiguring an old directory would silently restore it.

After the fix the paths agree:

| Check | Result |
|---|---|
| `.nro` size, all ten tests | **identical 10/10** |
| Symbol sets (`nm`), `t_init` | identical (empty diff) |
| `.bss` symbols, `t_init` | identical count (149) and summed size (17183 bytes) |
| TLS region (`__tls_end - __tls_start`) | identical (`0x410`) |
| Residual | 32 bytes of `.bss` section padding, ≤16 of `.text` — inter-object padding from Meson's `--start-group`/archive ordering vs the Makefile's explicit order |

Also checked rather than assumed: Meson's automatic
`-D_FILE_OFFSET_BITS=64` is a **no-op** on devkitA64's newlib (`off_t`
already 8 bytes, `struct stat` `0x68` with and without), so it is left
alone.

### The cross file's "+ Mesa" half, validated

`docs/milestones.md` item 2 asks for a cross file for `horizon` **and
Mesa**. The Phase 1 half was exercised by building the ten `.nro`; the
Mesa half was, at first, `sys_root` and `pkg_config_libdir` written from
reasoning and never run. Configuring the pinned Mesa tree with it closed
that gap.

It works further than expected — Mesa accepts the machine description,
detects `aarch64-none-elf-gcc/g++ 15.2.0` for the host machine and gets
840 lines into its own `meson.build` — and it found one real defect **in
the cross file**, not in Mesa:

- Mesa calls `add_languages('rust')` unconditionally for the nouveau
  Vulkan driver and fails with *"'rust' compiler binary not defined in
  cross file [binaries] section"*. Fixed: `rust` and `bindgen` are now
  declared, and `horizon_run` puts the image's rustup on `PATH` (the
  image keeps it outside the default one).

With that, configuration proceeds to the Rust sanity check and stops
where R13 predicted:

```
error[E0463]: can't find crate for `std`
  = note: the `aarch64-nintendo-switch-freestanding` target may not be installed
  = help: consider building the standard library from source with `cargo build -Zbuild-std`
```

**This is R13 confirmed against the toolchain**, not just against the
source. It is a failure reproduced deliberately — no Rust has been
successfully compiled for Horizon.

Two further items handed to Phase 3, found here rather than later:

- `Checking for size of "void*" : -1`. **Corrected in Phase 3 (see
  "Phase 3 — item 3" below): this was recorded here as a consequence of
  `needs_exe_wrapper = true`, and that was wrong.** Meson's size check
  is a *compile*-time binary search, not an execution, so an exe wrapper
  has nothing to do with it. The real cause was this repository's own
  cross file putting `-Werror` in `c_args`, which Meson also passes to
  its detection snippets; the snippet tripped `-Werror=unused-variable`.
  With the warnings moved out, the same configure answers
  `void* : 8`. `needs_exe_wrapper = true` is unaffected and stays — it
  is correct, there is no emulator.
- `WARNING: cannot auto-detect -mtls-dialect when cross-compiling`.
  Directly adjacent to the open `-mtp=soft` sub-risk in R13.

The probe wrote nothing and patched nothing; the build directory was
deleted afterwards.

### How far Mesa's non-driver core gets — the Phase 3 starting line

Phase 3's exit criterion is "Mesa configures for `horizon` and builds
the non-driver core". Configuring with no drivers at all skips the Rust
check entirely (no nouveau driver, no `add_languages('rust')`), so that
criterion is reachable without touching R13. Probed with
`-Dgallium-drivers= -Dvulkan-drivers= -Dplatforms= -Dopengl=false
-Dllvm=disabled`:

1. **`Python (3.x) mako module >= 0.8.0 required to build mesa`** —
   milestone item 6 in concrete form. Mesa's generators are Python and
   the image ships no mako, no pyyaml, no pip and has no network.
   **Fixed here**, pinned in `versions.env` and installed on the host by
   `horizon_ensure_python_deps`.
2. With that, configure runs deep into real compile-and-link checks
   against the Horizon toolchain — `strtod` locale support, `Bsymbolic`,
   version scripts, `-Wl,--build-id=sha1` (milestone Phase 3 item 8,
   already answered: **supported**) — and stops at:

   ```
   Checking for function "dlopen" : NO
   mesa/meson.build:1684:16: ERROR: C shared or static library 'dl' not found
   ```

**That is where Phase 3 starts**: `libdl` does not exist on
newlib/libnx, which is Phase 3 item 3 ("newlib/libnx gaps"). Not a
toolchain problem — the toolchain is answering correctly.

Worth noting for Phase 3's plan: the milestone lists OS detection first,
but the configure order means the newlib/libnx gaps are what actually
block first. The list is a set of items to complete, not an order to
follow.

### Commands run and results

All on this machine. **Host build/run (H)** and **cross build (X)** only
— nothing in this phase needed a console.

| Command | Class | Result |
|---|---|---|
| `rm -rf build && scripts/configure-horizon.sh` | H | installs pinned meson 1.11.2, writes cross constants, configures; host compiler detected as `aarch64-none-elf-gcc (devkitA64) 15.2.0`; 32 build targets |
| `scripts/build-horizon.sh` | X | **10/10 `.nro`**, `-Wall -Wextra -Werror` clean, zero warnings |
| `scripts/package-horizon.sh` | H | 10 copied + `MANIFEST.txt` with sha256 per artefact and the toolchain pins |
| `scripts/build-switch.sh all -j4` (Makefile path) | X | exit 0, 10 `.nro`, no warnings (`-j4` did **not** hit the overlayfs race this time) |
| `scripts/check-no-abs-paths.sh` | H | **OK** (`toolchain scripts Makefile meson.build`) |
| `scripts/check-layering.sh` | H | OK |
| `scripts/check-rust-target.sh` | H | OK — built-in target matches the snapshot (reports drift, exits 0: the environment's nightly is not ours to pin) |
| `scripts/print-toolchain-versions.sh` | H | reports the live toolchain; feeds the artefact manifest |
| `scripts/run-host-tests.sh` | H | **103/103 PASS** (6 suites), no regression |
| `scripts/fetch-mesa.sh` | H | `mesa-26.1.5` checked out, **HEAD verified = `6a02618ccf6c…`**, 503 MB |
| Every script re-run a second time | H | idempotent: "already installed" / "unchanged" / "0 copied, 10 already current" / "nothing to do" |

### Phase 2 exit criteria

| Criterion (`docs/milestones.md`) | State |
|---|---|
| A clean container reproduces the toolchain from `scripts/` alone (H) | ✅ `build/` deleted entirely, then configure → build → package from scripts only. **Read as "reconstructs against the current environment", not "bit-identical forever"** — with the Switch toolchain unpinned by decision, a rebuild months later uses a newer libnx. `build/pkg/MANIFEST.txt` records which one, and the `HORIZON_NX_IMAGE=…@sha256:…` command to go back to it |
| `grep` for `/home/`, `/work`, `D:\`, `/mnt/` in `toolchain/` and `scripts/` returns nothing | ✅ gate green — and it **caught a real violation on its first run** (`scripts/build-switch.sh` mounted at a hardcoded `/work`; now mounts at `"$PWD"`) |
| Phase 1 tests cross-compile with the new cross file (X) | ✅ 10/10, identical in size to the Makefile's |

**Every Phase 2 exit criterion is met.**

### Deviations from the milestone item list, with reasons

| Item | Disposition |
|---|---|
| 1. devkitA64 / devkitPro pinned by package version | **Deliberately not done** — owner's decision during this phase. The Switch toolchain belongs to the environment; it is read and recorded per build, never pinned. R15's original mitigation is rejected rather than implemented, and R15 now says so. The `resolved from $DEVKITPRO` half of the item *is* done |
| 3. Rust target JSON for Horizon | **Not created as a target.** rustc ships `aarch64-nintendo-switch-freestanding`; the file is committed only as a drift snapshot (R13) |
| 4. `rustc` wrapper | **Not needed** — no custom target, so nothing to wrap |
| 5. Rust `std`/`core` sysroot, pinned nightly | **Not built** — R13's answer removes it. The nightly is the environment's, not pinned here |
| 6. Mesa host tools (native build for generators) | **Done**, once the probe below showed what the item concretely is: Mesa's generators are Python and need `mako` (and `pyyaml`), which the image ships neither of, along with no pip and no network. Pinned in `versions.env` and provisioned by `horizon_ensure_python_deps`. An earlier revision of this file recorded the item as deferred; that was wrong — it was a Phase 2 gap and it is now closed |

### Is Phase 2 finished?

Yes, for everything verifiable without a console. All three exit
criteria are met, and the one item that was genuinely outstanding
(milestone item 6) is closed above.

**One thing is owed and it is not Phase 2's:** the ten `.nro` have not
been re-run on hardware since Phase 1's second review round. Phase 2
changed no `horizon/` code, so nothing here made that worse — but the
longer it waits, the more work sits on an unreconfirmed base. It does
not block Phase 3, which touches Mesa and not `horizon/`; it does block
Phase 4, which builds directly on `horizon/`.

### Known gaps at the end of Phase 2

- Nothing Rust has been **compiled** for Horizon. R13's answer is
  source-level, and the `-mtp=soft` sub-risk stays open until it is.
- `mesa/` is fetched but **never configured or built**. That is Phase 3.
- The `.nro` produced by the Meson path are a **cross build**, not
  hardware-verified. They are identical in size to the Makefile's, whose
  hardware run is itself still pending re-confirmation (see above).

---

## Next concrete task

**Find out what stops `t_threads` inside `util_get_cpu_caps()`, then run
both on a console.**

The emulator run of 2026-07-28 (section above) leaves one open failure
and one open question. The rebuilt `t_threads.nro`
(`00d15baa…`) reaches the failing call in four named stages, so the next
log says which of `getenv`, `os_get_option`, `os_get_option_cached` or
`util_get_cpu_caps` is the one that does not return. Worth knowing at the
same time, and only the person at the machine can say: whether the
application **crashed** or **hung** — a hang points at the statically
initialised `simple_mtx` or at `call_once`, a fault points at an access.

A console run is still owed regardless: an emulator answers for its own
libnx, not for the Switch's. Both `.nro` are in `build/` and
`build/meson/` (identical sizes) and write their logs to
`sdmc:/horizon_gpu_tests/` like the other eleven.

Rebuild them first if the copies on the SD card predate the **second**
PR #4 review round: both were changed by both rounds. `t_threads` can
now report a timed call that never returns instead of hanging on it, and
checks every mutex return in its own workers; `t_ostime`'s sleep bounds
can now fail for a 2× unit error, which they could not before. The
current hashes are in "Phase 3 — closing items 1, 2, 4, 5 and 7" below.
Until their output is in hand:

- item 4 is a cross build, and nothing is claimed about Mesa's C11
  threads shim on hardware — in particular not the polling
  `mtx_timedlock`, which is where the implementation can lie;
- item 5 is a cross build, and `os_time_get_nano`'s unchecked
  `timespec_get` is an open question, not a known-good path;
- `mesa-patches/0012`'s effect is a compile result: `u_cpu_detect.c.o`
  references `sysconf`, and what number it produces on a Switch is
  unmeasured.

Everything else in Phase 3 is closed with evidence — see "Phase 3 —
closing items 1, 2, 4, 5 and 7" for the item-by-item disposition and for
the four things this session deliberately did not do.

Then **Phase 4 — `nvkmd_horizon`**. Two things from this session belong
at the top of its plan, both measured rather than anticipated:

1. **Six libc symbols are unresolved** in Mesa's built core —
   `posix_memalign`, `flock`, `getuid`, `geteuid`, `getgid`, `getegid` —
   and a static archive never resolves anything, so they will stop the
   first executable link. The list, with the object referencing each, is
   in the audit above. `posix_memalign` additionally has a configure
   check that answers `YES` and is wrong.
2. **`vk_sync_timeline.c` uses `cnd_timedwait`**, so Phase 4's timeline
   semaphores depend on the timeout path `t_threads` measures.

Already in place: `mesa/` at `MESA_COMMIT` with the **twelve**-patch
series applying cleanly and idempotently, `scripts/configure-mesa.sh`
and `scripts/build-mesa.sh` as the reproducible loop, `compat/` linked
into both build paths and policed by the layering gate, the cross file
no longer corrupting Mesa's configure answers, and pkg-config resolving
the Switch portlibs.

---

## Commit log for this phase

| Commit | Scope |
|---|---|
| `horizon/debug: add result plumbing and context-owned logging` | result.h, log, status |
| `horizon/device: add nv bring-up and GM20B query` | device + align + testfw + t_init |
| `horizon/memory: add NvMap-backed allocations` | memory + tests 2–3 |
| `horizon/vm: add GPU VA reservations and fixed maps` | vm + tests 4–5 |
| `horizon/submit: add GPFIFO command emitters` | cmds (public, pure) |
| `horizon/sync: add syncpoint fences with wrap-safe waits` | sync |
| `horizon/channel,submit: add GPFIFO channels and async submission` | channel + submit |
| `tests: add channel, submit, syncpoint, fence and teardown tests` | tests 6–10 |
| `build,scripts: add devkitA64 Makefile, layering gate, host tests` | build + gates |
| `docs: record Phase 1 implementation status` | STATUS, tests/README |
| `horizon/device: track the public device header` | fixup |
| `horizon/channel: treat absent error notification as no error` | HW finding fix |
| `tests: make t_teardown's in-flight destroy probe race-tolerant` | test fix |
| `docs: record the first hardware run` | this update |
| `docs: document the devkitA64 Docker fallback in CLAUDE.md` | CLAUDE.md |
| `horizon,tests,build: fix Codex review findings` | channel/vm/submit/device fixes, t_submit R10 redesign, Makefile -j race |

## Commit log for Phase 2

| Commit | Scope |
|---|---|
| `scripts: add the absolute-path gate and fix its one violation` | `check-no-abs-paths.sh`; `build-switch.sh` `/work` → `"$PWD"` |
| `toolchain: pin devkitA64, libnx, Mesa and Rust versions` | `versions.env`, `print-toolchain-versions.sh` |
| `toolchain: add a path-free Meson cross file for Horizon/aarch64` | `horizon-aarch64.cross`, `toolchain-env.sh`, `gen-cross-file.sh` |
| `build: add a Meson build for horizon_gpu and the ten Phase 1 tests` | `meson.build`, `configure-horizon.sh`, `build-horizon.sh` |
| `scripts: add idempotent Mesa fetch and .nro packaging` | `fetch-mesa.sh`, `package-horizon.sh` |
| `docs: answer R13 — no Rust sysroot is needed for Phase 2` | `rust-toolchain.md`, `aarch64-horizon.json`, `check-rust-target.sh`, `known-risks.md` |
| `docs: record Phase 2 toolchain results` | this update |

### Incident during Phase 2, recorded because it was destructive

The first version of `scripts/fetch-mesa.sh` used
`git -C mesa rev-parse --git-dir` to decide whether `mesa/` was already
a repository. `mesa/` sits inside this repository and had no `.git` of
its own, so that query answered for the **parent**: `git init` was
skipped, and the following commands ran against `mesa-nvk-horizon`
itself — `origin` was repointed at Mesa and Mesa's tree was checked out
over the working tree.

Nothing was lost. The branch ref was never touched; recovery was
`git checkout` of the branch, `git remote set-url` back to the GitHub
URL, deleting the fetched Mesa tag and removing `.git/shallow`.
Verified afterwards: `git diff HEAD` empty, all three commits present,
remote restored.

The script now tests for the directory rather than asking git, and then
**asserts** that `mesa/` really is its own repository before running
anything that writes — aborting instead of falling through to the
archive path. Its stderr is also no longer swallowed; suppressing it is
what hid the failure at the time.

`scripts/apply-mesa-patches.sh`, added in Phase 3, carries both guards
for the same reason and cites this incident in its header.

---

## Commit log for Phase 3 (item 3)

| Commit | Scope |
|---|---|
| `toolchain,build: move the warning policy out of the cross file` | `horizon-aarch64.cross`, `meson.build`; ten `.nro` proven identical |
| `docs: correct the cause of the void* size check failure` | STATUS |
| `mesa-patches,scripts: define the patch series and add its applier` | `mesa-patches/README.md`, `apply-mesa-patches.sh`, `configure-mesa.sh`, `fetch-mesa.sh` guard, `check-no-abs-paths.sh` scope, `architecture.md` |
| `mesa-patches: make dlopen availability a libc trait, not an OS trait` | patches 0001–0002 |
| `toolchain: put devkitPro's portlibs pkg-config on PATH` | `versions.env`, `toolchain-env.sh` |
| `mesa-patches,scripts: close the item 3 newlib/libnx gaps that block the core` | patches 0003–0008, `build-mesa.sh`, shader-cache decision, meson shebang fix |
| `docs: record Phase 3 item 3` | STATUS |

## Commit log for Phase 3 (item 6)

| Commit | Scope |
|---|---|
| `compat: implement the sysconf newlib declares but does not define` | `compat/sysconf.c` — first content in `compat/` |
| `toolchain,build: link compat into both build paths` | `build-compat.sh`, `horizon_compat_libdir`, cross file, `Makefile`, both configure scripts |
| `scripts: extend the layering gate to compat/` | `check-layering.sh` |
| `mesa-patches: let sysconf answer the memory and page-size queries` | patches 0009–0010 |
| `tests: add t_sysinfo, the eleventh .nro` | `t_sysinfo.c`, `meson.build`, `Makefile` |
| `docs: record Phase 3 item 6` | STATUS |

## Commit log for Phase 3 (closeout — items 1, 2, 4, 5, 7)

| Commit | Scope |
|---|---|
| `compat: answer the processor-count queries from the kernel's core mask` | `compat/sysconf.c` — `_SC_NPROCESSORS_ONLN` / `_SC_NPROCESSORS_CONF` from `svcGetInfo(InfoType_CoreMask)` |
| `mesa-patches: gate the thread-creation signal mask on pthread_sigmask` | patch 0011 (item 4) |
| `mesa-patches: count the CPUs on POSIX-lite platforms too` | patch 0012 (item 1; the `util_cpu_detect` defect) |
| `tests: add t_threads and t_ostime, the twelfth and thirteenth .nro` | `t_threads.c`, `t_ostime.c`, `Makefile`, `meson.build` |
| `docs: record the Phase 3 closeout` | this update |

## Commit log for the Codex review round on PR #4

| Commit | Scope |
|---|---|
| `tests: bound every wait on a timed call in t_threads` | `t_threads.c` — watchdogs, per-section functions, `deadline_in_ms` checked |
| `tests: measure the clock over an interval, not over a sample count` | `t_ostime.c` — ARM-counter-bounded sample loop |
| `build: honour MESA_BUILD_DIR in both build paths` | `meson.options`, `meson.build`, `Makefile`, `toolchain-env.sh`, `configure-horizon.sh`, `configure-mesa.sh`, `build-mesa.sh`, `build-switch.sh`, `check-no-abs-paths.sh` |
| `build: stop clean and stale artefacts from crossing between builds` | `Makefile` (`clean`, `prune-stale`), `build-horizon.sh` |
| `docs: record the PR #4 review round` | this update, `tests/README.md` |

## Commit log for the second Codex review round on PR #4

| Commit | Scope |
|---|---|
| `tests: check every mutex call, and report one defect once` | `t_threads.c` — worker mutex returns, the counter's expected total, the processor-count section's claims, the both-sided header claim |
| `tests: bounds that can fail for the defect they name` | `t_ostime.c` — one wait convention, the prompt bound, the rate check's backwards guard, the resolution message, the overflow note |
| `scripts: a loud build identity, and one that survives Mesa appearing` | `toolchain-env.sh`, `configure-horizon.sh`, `configure-mesa.sh`, `build-horizon.sh` |
| `build: gate the four facts both build systems restate` | `check-mesa-test-parity.sh` (new), `meson.build` — link-order measurement, versioned citation |
| `build: clean that does not depend on how the path was spelled` | `Makefile` — `$(abspath)` comparison, the rule's actual contract |
| `compat: measure the claim that Horizon has no wider core count` | `compat/sysconf.c` |
| `docs: record the second PR #4 review round` | this update, `tests/README.md` |

## Commit log for the Codex review round

| Commit | Scope |
|---|---|
| `scripts: wipe a build directory when the cross files change (P1)` | `horizon_setup_mode`, both configure scripts |
| `scripts: harden the mesa/ git guards against redirection (P1)` | `--show-toplevel` assertion, Git env cleared, patch-id matching, `fetch-mesa.sh`, README |
| `mesa-patches: treat zero available memory as an answer, not a failure (P2)` | patch 0010 |
| `tests: measure the page granularity instead of asserting it (P2)` | `t_sysinfo.c` — the map ladder and the clamp |
| `scripts,build: fix portability and staleness in the compat plumbing (P2)` | `sed -i`, Meson quoting, compat identity, Makefile depfiles |
| `docs: record the Codex review round` | this update |
