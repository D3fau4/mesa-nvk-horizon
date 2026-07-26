# STATUS

**Last updated:** 2026-07-26
**Branch:** `claude/mesa-nvk-horizon-phase1-rp61t8`

---

## Current phase

**Phase 1 — `horizon/` standalone GPU layer. Hardware-verified through
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
| D2/D3 | Mesa pin / checkout mechanism | due at Phase 2 start |
| D5 | Cache policy per memory type | blocked on R6 (first GPU write) |
| D6 | Timeline semaphores vs upload queue | Phase 4 |

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

## Next concrete task

**Re-run the ten `.nro`s on real hardware** to close out the second
review round above — the changed paths (channel create/destroy, vm_map,
device big-page handling, the GPFIFO command emitters) are exercised by
every test, not just one. Once confirmed, Phase 1 returns to fully
verified and:

**Phase 2 — toolchain** (`docs/milestones.md`) is next, pending the
owner's go-ahead:

1. Pin devkitA64 / libnx / nx-dev image versions in
   `toolchain/versions.env` (today they are pinned only implicitly by the
   image digest).
2. Meson cross file `toolchain/horizon-aarch64.cross`; decide D2 (Mesa
   version) and D3 (submodule vs fetched checkout) at phase start.
3. `scripts/check-no-abs-paths.sh` gate and idempotent fetch/configure/
   build/package scripts.

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
