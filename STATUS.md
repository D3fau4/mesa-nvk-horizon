# STATUS

**Last updated:** 2026-07-26
**Branch:** `claude/mesa-nvk-horizon-phase2-toolchain-4cbebj`

---

## Current phase

**Phase 2 — toolchain. Complete for everything verifiable without a
console; see "Phase 2" below.** The Meson cross file exists and is
exercised (all ten Phase 1 `.nro` cross-compile through it, identical in
size to the Makefile's), D2 and D3 are decided, R13 is answered — no
Rust sysroot is needed — and the absolute-path gate is in place and
green.

On pinning, the phase ended somewhere different from where
`docs/milestones.md` pointed it: **the Switch toolchain is deliberately
not pinned.** libnx, devkitA64 and the portlibs belong to the
environment (`$DEVKITPRO` or the container image) and are neither frozen
nor updated from this repository. Only the inputs this project chooses —
Mesa, Meson, the Rust target — are pinned. What replaces the pin is
recording, per build, what was actually used. See "What is pinned, and
what deliberately is not" below.

**Carried over from Phase 1 and still open:** the second review round's
fixes have not been re-run on real hardware. Phase 2 changed no code in
`horizon/`, so that item is unchanged, not resolved. See "Second review
round" below.

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
| Pure logic builds/runs on host (H) | ✅ 78/78 |
| Layering gate clean | ✅ |
| Tests 1–10 pass on hardware (HW) | ✅ **all ten PASS** (8/10 first run + t_channel 17/17 and t_teardown 28/28 on the confirmation re-run) |
| ≥2 submits in flight without CPU wait (test 7) | ✅ **verified on hardware** (148 µs for both submits, single wait at the end) |

**Every Phase 1 exit criterion is met.**

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

- `Checking for size of "void*" : -1`. `needs_exe_wrapper = true` means
  Meson cannot run a test program, so the size comes back unknown. Mesa
  will need this answered by a cross property rather than by execution.
- `WARNING: cannot auto-detect -mtls-dialect when cross-compiling`.
  Directly adjacent to the open `-mtp=soft` sub-risk in R13.

The probe wrote nothing and patched nothing; the build directory was
deleted afterwards. Making Mesa configure to completion needs
`DETECT_OS_HORIZON` and the rest of Phase 3.

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
| 6. Mesa host tools (native build for generators) | **NOT DONE — the one genuine gap in this phase.** Mesa's configure does not reach its generators: it stops at the Rust sanity check (above), which Phase 3 has to get past first. Building host tools now would be scaffolding against a configure that has never completed. Deferred to Phase 3, but recorded as owed, not as finished |

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

Then **Phase 3 — minimal Horizon support in Mesa**
(`docs/milestones.md`): `DETECT_OS_HORIZON`, Meson
`host_machine.system() == 'horizon'` handling, newlib/libnx gaps,
threads, timers, page-size queries, endianness, build ID — each as a
separate patch in `mesa-patches/`. The pinned tree is already in place
and the cross file is already written for it.

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
