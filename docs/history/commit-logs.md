## Commit log for Phase 4

| Commit | Scope |
|---|---|
| `mesa-patches: nonCoherentAtomSize is a CPU cache property` | patch 0033, STATUS — the pdev half of the audit |
| `mesa-patches: close the push-truncation class in the submit path` | patch 0032, STATUS — the submit half of the audit |
| `mesa-patches: every nvkmd_mem needs its own VA` | patch 0031, STATUS — the audit of the never-executed path |
| `tests: t_uncached, and t_sysinfo's hardware failure was a stale binary` | `tests/t_uncached.c`, `meson.build`, `Makefile`, STATUS — D14 and the t_sysinfo diagnosis |
| `horizon,tests: an opt-in untrusted syncpoint baseline, and a run that admits it` | `horizon/device`, `horizon/channel`, `tests/t_vulkan.c`, synchronization.md § 9 |
| `docs: record what nvkmd requires, against what horizon_gpu has` | STATUS — step 1, the interface tables and D9–D12 |
| `mesa-patches: close the libc gaps the first executable link meets` | patches 0013–0014, STATUS — step 2 |
| `scripts: vendor the crates -Zbuild-std needs, and compile Rust for Horizon` | `fetch-rust-crates.sh`, STATUS — step 3 |
| `mesa-patches,tests: the mandatory Vulkan sequence links as a .nro` | patches 0020-0021, `tests/t_vulkan.c`, STATUS — items 6-10, D8 and D11 closed |
| `mesa-patches: nvkmd_horizon memory, VA heap and binding` | patch 0019, STATUS — items 3-5 |
| `mesa-patches: add the Horizon kernel-mode-driver backend` | patch 0018, `configure-mesa-nvk.sh`, `build-mesa-nvk.sh`, STATUS — items 1-2, D9 and D10 closed |
| `mesa-patches: build Mesa's Rust half without a standard library` | patches 0016–0017, STATUS — step 5 |
| `toolchain: build the machine Mesa's nouveau driver needs` | `toolchain/Dockerfile`, `build-toolchain-image.sh`, `fetch-rust-tools.sh`, `fetch-clc-deps.sh`, `fetch-mesa-subprojects.sh`, `build-mesa-clc.sh`, `build-rust-sysroot.sh`, cross file, patch 0015, STATUS — step 4 |

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

## Commit log for the third Codex review round on PR #4

| Commit | Scope |
|---|---|
| `build: keep the Mesa build whatever it is nested under, and package one build's artefacts` | `Makefile` (`clean_keeps`), `package-horizon.sh` |
| `scripts: record which Mesa directory a build was configured for, and wire the TLS gate` | `toolchain-env.sh`, `configure-horizon.sh`, `build-horizon.sh`, `build-mesa.sh`, `configure-mesa.sh` |
| `tests: synchronise the condvar waiters, and count call_once atomically` | `t_threads.c` |
| `tests: run every blocking os_time call on a watched worker` | `t_ostime.c` |
| `docs: record the third PR #4 review round` | this update |

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
