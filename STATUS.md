# STATUS

**Last updated:** 2026-07-24
**Branch:** `claude/mesa-nvk-horizon-phase1-rp61t8`

---

## Current phase

**Phase 1 — `horizon/` standalone GPU layer. CODE COMPLETE; hardware
verification pending.**

All eight implementation items and the ten tests are written and
cross-compile cleanly. The pure-logic modules pass 78 host-side unit
checks under ASan/UBSan. **Nothing has run on a Switch yet** — every
hardware exit criterion below is explicitly unverified.

---

## Completed work

### `horizon_gpu` (all of it Vulkan-, Mesa- and nwindow-free; gate enforced)

| Area | Files | Notes |
|---|---|---|
| results/logging | `horizon/include/horizon_gpu/result.h`, `horizon/debug/` | `horizon_gpu_result` carries the libnx `Result`; log config lives in the device — zero global mutable state |
| device | `horizon/device/device.c` | ordered bring-up `nvInitialize→nvFenceInit→nvMapInit→nvGpuInit→GetCharacteristics→nvAddressSpaceCreate→GetVARegions`, reverse unwind on every failure; characteristics query is **required** (no big-page fallback); live-object counters |
| memory | `horizon/memory/` | NvMap-backed, truthful cacheability (cached heap registered cacheable), id/handle exposed as distinct concepts, range-checked flush (clean) / invalidate (civac), all rounding overflow-checked, 32-bit nvmap limits reject |
| vm | `horizon/vm/` | reservations first-class (ALLOC_SPACE non-fixed, R8); FIXED maps inside a reservation with the **reservation's** page size (R9); pure interval set rejects overlap pre-kernel; unmap checks its Result and clears the mem's recorded VA (memory-model §2 invariant) |
| channel | `horizon/channel/` | NvGpuChannel + syncpoint identity, initial value recorded (R5), one-page internal cmdbuf, optional Zcull (queried size, 0x20000 align), explicit SET_OBJECT engine binds with queried classes (R7), retirement list, lost-channel fail-fast with decoded notifier |
| submit | `horizon/submit/` | request+emit increment indivisible (R4); kickoff surfaced, never retried/slept; **no `nvFenceWait` anywhere in the submit path**; entry flags caller-selectable for the R3 measurement; debug-synchronous mode compiled-in, off by default |
| sync | `horizon/sync/` | wrap-safe predicate `(int32_t)(cur-thr) >= 0`, 64-bit shadow extension, ns→µs single conversion (round-up, saturating), bounded waits, timeout returned never masked |

### Command encodings (facts re-derived, nothing copied)

Fence increment = WFI + SYNCPOINTA/B (the per-job sequence the Linux
`nvgpu` kernel driver emits on gk20a/gm20b); method addresses/fields from
NVIDIA open-gpu-doc `cla06f.h`; pushbuffer header format per envytools.
GPU-side syncpoint wait (R10) emitted with the same class methods —
validated only on host so far; `t_submit` proves or disproves it on
hardware. Every constant carries its citation in
`horizon/include/horizon_gpu/cmds.h`.

### Tests

Ten standalone `.nro`s (`tests/`, framework in `tests/common/`, console
instructions in `tests/README.md`) printing PASS/FAIL summaries to stdout
and `sdmc:/horizon_gpu_tests/*.log`. Four host-side unit test binaries
(`tests/host/`) for the pure-logic modules.

### Build & gates

`Makefile` (devkitA64 via `$DEVKITPRO` only), `scripts/build-switch.sh`
(container fallback `ghcr.io/d3fau4/nx-dev:latest`, override with
`HORIZON_NX_IMAGE`), `scripts/check-layering.sh` (forbidden includes,
nwindow/vi symbols, `g_dev` pattern, CPU waits outside sync/channel),
`scripts/run-host-tests.sh` (host cc + ASan/UBSan).

---

## Tests executed (in this environment)

| Command | Class | Result |
|---|---|---|
| `scripts/run-host-tests.sh` (gcc 13.3, x86_64, ASan+UBSan) | **host build + run** | `h_align` 20/20, `h_va_space` 21/21, `h_syncpt_math` 19/19, `h_cmds` 18/18 — all PASS |
| `scripts/check-layering.sh` | **host run** | OK — horizon/ Vulkan-, Mesa- and nwindow-free |
| `docker run … ghcr.io/d3fau4/nx-dev:latest make clean && make all -j4` | **cross build** | exit 0; 10/10 `.nro` produced with `-Wall -Wextra -Werror`; toolchain: devkitA64 gcc 15.2.0, libnx from the image |

**Hardware status: NOT RUN.** No claim below the host/cross line has been
verified on a console. The Switch behaviour of bring-up, mapping, submit,
fences — everything — is untested. Compilation success is not Switch
behaviour.

---

## Phase 1 exit criteria — verification state

| Criterion | State |
|---|---|
| Ten tests cross-compile (X) | ✅ done (see above) |
| Pure logic builds/runs on host (H) | ✅ done (78 checks) |
| Layering gate clean | ✅ done |
| Tests 1–10 pass on hardware (HW) | ⏳ **pending — owner has a console (D4 answered yes); awaiting the run** |
| ≥2 submits in flight without CPU wait (test 7) | code + test written; ⏳ HW pending |

---

## Known failures / limitations

- **No hardware verification yet** (see above). The `.nro`s and
  `tests/README.md` are ready; the owner needs to run them and report the
  logs.
- **R6 (cache coherency) not yet measurable.** Measuring GPU-write→CPU-read
  visibility needs a GPU write primitive (inline-to-memory or DMA), which
  needs engine methods beyond Phase 1's scope. t_map validates mapping
  mechanics only; the R6 measurement moves to the first GPU write
  (early Phase 4/5). Recorded as a deliberate deviation from the R6 note.
- **Kickoff "ring full" is not yet distinguished from other rejections**
  (synchronization.md §2.1): libnx's queue-full case is prechecked and
  returns BUSY, but a kernel-side rejection code map needs hardware data;
  t_submit records the raw `Result` for that.
- **`nvGpuChannelGetErrorNotification` "no error" detection** assumes a
  zero timestamp means the notifier never fired; t_channel verifies this
  on hardware.
- The two rollback paths in `submit.c` adjust libnx's public
  `NvGpuChannel` fields (`num_entries`, `fence_incr`) directly; correct
  against libnx as pinned in the nx-dev image, revisit if libnx changes.

## Measurements the hardware run must produce

1. **R5**: syncpoint value at channel creation (t_channel/t_syncpt notes).
2. **R3**: entry flags 0 vs `NOT_MAIN|NO_PREFETCH` (t_submit note).
3. **R10**: GPU-side syncpoint-wait encoding outcome (t_submit note).
4. **R8**: oversized-reservation error code (t_va_reserve note).
5. Whether `Generic_16BX2` and big-page maps succeed (t_map notes).

---

## Pending decisions

| # | Decision | State |
|---|---|---|
| D1 | Literal reuse from GPL/AGPL reference | unchanged: **no**; nothing copied in Phase 1 |
| D4 | Switch available for Phase 1 tests | **answered: yes.** Verification now blocked only on the owner running `build/*.nro` (instructions: `tests/README.md`) |
| D2/D3 | Mesa pin / submodule vs fetch | unchanged, due at Phase 2 start |
| D5 | Cache policy per Vulkan memory type | still blocked on R6; R6 itself deferred to first GPU write (see limitations) |
| D6 | Timeline semaphores vs upload queue | unchanged, Phase 4 |

---

## Next concrete task

1. **Owner:** run the ten `.nro`s on the console per `tests/README.md`,
   paste the logs; incorporate results here, fix whatever fails.
2. After a green hardware run: **Phase 2 — toolchain** (`docs/milestones.md`),
   starting with pinning devkitA64/libnx versions (the nx-dev image
   currently pins them implicitly) in `toolchain/versions.env` and the
   Meson cross file.

---

## Commit log for this phase

| Commit | Scope |
|---|---|
| `horizon/debug: add result plumbing and context-owned logging` | result.h, log, status |
| `horizon/device: add nv bring-up and GM20B query` | device + align helpers + testfw + t_init |
| `horizon/memory: add NvMap-backed allocations` | memory + t_alloc + t_nvmap |
| `horizon/vm: add GPU VA reservations and fixed maps` | vm + t_va_reserve + t_map |
| `horizon/submit: add GPFIFO command emitters` | cmds (public, pure) |
| `horizon/sync: add syncpoint fences with wrap-safe waits` | sync |
| `horizon/channel,submit: add GPFIFO channels and async submission` | channel + submit |
| `tests: add channel, submit, syncpoint, fence and teardown tests` | tests 6–10 |
| `build,scripts: add devkitA64 Makefile, layering gate, host tests` | Makefile, scripts, tests/host |
| `docs: record Phase 1 implementation status` | this file, tests/README.md |

Pushed to `origin/claude/mesa-nvk-horizon-phase1-rp61t8` with the owner's
authorisation (given at planning time).
