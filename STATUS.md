# STATUS

**Last updated:** 2026-07-26
**Branch:** `claude/mesa-nvk-horizon-phase3-item3-9g57iu`

---

## Current phase

**Phase 3 — minimal Horizon support in Mesa. Items 3 and 6 are done;
the build half of the phase's exit criterion is met.**

**Mesa configures for `horizon` (`meson setup` exits 0) and its
non-driver core builds: 379 of 379 edges, zero failures, all ten static
libraries archived.** That is `docs/milestones.md`'s Phase 3 exit
criterion "Mesa configures for `horizon` and builds the non-driver
core", as a cross build (X).

Two things happened before any Mesa work was possible, both recorded
below: a defect in **our own** cross file was making six of Mesa's
configure checks return false answers, and `mesa-patches/` had no
mechanics at all (it held a `.gitkeep`). The series now stands at **ten
patches**, every one formulated as a property of the C library or the
compiler rather than as an OS name.

`compat/` has its **first content**: `sysconf`, which devkitA64's newlib
declares and does not define. That is the one door `CLAUDE.md` leaves
open, and `scripts/check-layering.sh` now polices it.

**Carried over and still open:** the ten `.nro` have not been re-run on
real hardware since Phase 1's second review round. Phase 3 changes no
code in `horizon/` — the ten `.nro` are byte-identical to the Phase 2
baseline, verified again after every change here — so that item is
unchanged, not resolved. **`t_sysinfo` (the eleventh `.nro`) has never
been run at all**: it exists precisely to measure what `compat/sysconf.c`
returns on a console, and until it does, those numbers are cited and
reasoned, not measured.

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

- **`t_sysinfo` has not run on hardware.** Its whole purpose is to turn
  `compat/sysconf.c`'s numbers into measurements; it ships as a cross
  build. It now bounds the page size from both sides (see the Codex
  review round above); the earlier claim that divisibility alone caught
  an understated page size was wrong.
- **`util_cpu_detect` reports 1 CPU on Horizon.** Its whole CPU-counting
  block is under `#elif DETECT_OS_POSIX`, and patch 0007 chose
  POSIX-**lite** deliberately (no `<syslog.h>`, no `<sys/shm.h>`, no
  loader), so nothing sets `available_cpus`. Confirmed by `nm`:
  `u_cpu_detect.c.o` has no `sysconf` reference. A quality problem for
  Phase 4 — the Switch has more than one usable core — not a blocker,
  and recorded rather than papered over.
- Milestone items 2 and 4 are untouched; items 1, 5 and 7 still carry
  only the minimum each needed.

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
| Tests 1–10 pass on hardware (HW) | ⚠️ **all ten PASSED, on code that has since changed** — 8/10 on the first run, plus `t_channel` 17/17 and `t_teardown` 28/28 on the confirmation re-run. Measured at `732b58c`; the second review round (`747b915`) then changed `horizon/`. **Not re-run since.** |
| ≥2 submits in flight without CPU wait (test 7) | ⚠️ **measured on hardware** (148 µs for both submits, single wait at the end) — same caveat: `submit.c` changed in `747b915` |

**Every Phase 1 exit criterion was met at `732b58c`.** Two of them are
hardware criteria and `horizon/` changed afterwards, so they are
*stale*, not *failed*: nothing suggests a regression, and the changed
code builds clean and passes 103/103 host tests. But a ✅ here would
claim console evidence for code no console has run, which is exactly the
host / cross / hardware distinction this project refuses to blur
(CLAUDE.md). They go back to ✅ after the re-run in "Next concrete task".

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

**Re-run the ten `.nro`s on real hardware** to close out the second
review round — the changed paths (channel create/destroy, `vm_map`,
device big-page handling, the GPFIFO command emitters) are exercised by
every test, not just one. `build/pkg/` now holds the ten `.nro` plus a
`MANIFEST.txt` recording each artefact's sha256 against the toolchain
pins, so the result can be attributed to an exact build. Phase 2 changed
no `horizon/` code, so either build path's artefacts are valid for this.

That step now covers **eleven** `.nro`, not ten. `t_sysinfo` is new and
has never run; it is what turns `compat/sysconf.c`'s page size and
memory figures from cited-and-reasoned into measured, and it needs no nv
services so it is the cheapest one to start with.

**Phase 3's build criterion is met**, so the remaining Phase 3 work is
the items that were never blocking: **2** (Meson
`host_machine.system() == 'horizon'` handling) and **4** (threads beyond
the one `pthread_mutex_timedlock` patch), plus the rest of items 1, 5
and 7 beyond the minimum each has. Item 8 was closed in Phase 2 without
a patch.

Also worth doing before Phase 4 leans on it: `util_cpu_detect` reports
one CPU on Horizon (see "Known gaps" above).

Already in place: `mesa/` at `MESA_COMMIT` with the ten-patch series
applying cleanly and idempotently, `scripts/configure-mesa.sh` and
`scripts/build-mesa.sh` as the reproducible loop, `compat/` linked into
both build paths and policed by the layering gate, the cross file no
longer corrupting Mesa's configure answers, and pkg-config resolving the
Switch portlibs.

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

## Commit log for the Codex review round

| Commit | Scope |
|---|---|
| `scripts: wipe a build directory when the cross files change (P1)` | `horizon_setup_mode`, both configure scripts |
| `scripts: harden the mesa/ git guards against redirection (P1)` | `--show-toplevel` assertion, Git env cleared, patch-id matching, `fetch-mesa.sh`, README |
| `mesa-patches: treat zero available memory as an answer, not a failure (P2)` | patch 0010 |
| `tests: measure the page granularity instead of asserting it (P2)` | `t_sysinfo.c` — the map ladder and the clamp |
| `scripts,build: fix portability and staleness in the compat plumbing (P2)` | `sed -i`, Meson quoting, compat identity, Makefile depfiles |
| `docs: record the Codex review round` | this update |
