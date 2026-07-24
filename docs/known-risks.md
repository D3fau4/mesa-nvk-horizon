# Known risks and pending decisions

Every entry has an owner phase, an impact, and either a mitigation or an explicit
"unresolved". Entries are removed only when the risk is retired, never when it becomes
inconvenient.

---

## R1 — Licence incompatibility with the reference ports

**Phase:** 0 (live) · **Impact:** project-defining

The reference snapshots are copyleft, and they contradict themselves:

| Snapshot | `LICENSE` file | `README.md` claims |
|---|---|---|
| `master` | GNU **GPL-2.0** | "GPL-2.0-or-later" |
| `switch-port/nvk-wsi` | GNU **AGPL-3.0** | "AGPL-3.0" |
| `switch-port/triple-buffer` | GNU **AGPL-3.0** | "AGPL-3.0" |
| `switch-port/wsi-zero-copy` | GNU **AGPL-3.0** | "AGPL-3.0" |

Mesa/NVK is MIT. Copying reference source into this tree would make the driver
GPL/AGPL-derived, block upstreaming, and — for AGPL — attach network-service obligations
that make no sense for a GPU driver.

**Mitigation:** re-derive from facts, do not copy text. Hardware constants, service call
ordering and documented failure modes are facts. Implementation expression is not.
`LICENSES/README.md` records the policy.

**Unresolved:** whether the project owner wants *any* literal reuse. Until answered, the
answer is no.

---

## R2 — No hardware in this environment

**Phase:** all · **Impact:** high

Nothing in this repository can be validated on a Switch from here. Every Phase 1–6 exit
criterion marked **HW** is unverifiable until the owner runs it on a console.

**Mitigation:** each test is a self-contained `.nro` that prints a machine-checkable
PASS/FAIL summary to `stdout` and to a file on the SD card, so a run can be reported back
as text. `STATUS.md` distinguishes host build / cross build / hardware-verified for every
claim, and never collapses them.

---

## R3 — GPFIFO entry flags: `NOT_MAIN | NO_PREFETCH`

**Phase:** 1, 4 · **Impact:** high

The reference found that every submit which worked on hardware used
`GPFIFO_ENTRY_NOT_MAIN | GPFIFO_ENTRY_NO_PREFETCH`, and that NVK's default `flags = 0`
(MAIN, prefetch enabled) caused kickoff failures on Horizon
(`drm_shim.c:900-906`). This is an undocumented behavioural constraint of the Horizon
`nv` path, not a Vulkan or nouveau requirement.

**Risk:** we do not know *why*. If the real cause is something else (a size threshold, an
alignment, a missing prior bind), copying the flags forward hides it.

**Mitigation:** Phase 1 test 7 submits with both flag combinations and records which
work, so the constraint is measured on our own code rather than inherited as folklore.

---

## R4 — Syncpoint increment must be both requested and emitted

**Phase:** 1 · **Impact:** high

`nvGpuChannelIncrFence` only advances libnx's expected value; without an actual
syncpoint-increment command in the stream, the GPU never bumps the counter and every
fence wait times out. Conversely, doing both twice double-increments. The reference cycled
through several wrong combinations before settling (`drm_shim.c:941-953`).

**Mitigation:** the two are one indivisible operation in `horizon/submit/`
(`docs/synchronization.md` § 1.2). Phase 1 test 8 asserts exactly one increment per submit.

---

## R5 — Syncpoint wraparound is unhandled everywhere in the reference

**Phase:** 1 · **Impact:** medium, latent

32-bit counters wrap. The reference compares with `>=` and has no 64-bit shadow. A
long-running application will eventually hit this.

**Mitigation:** wrap-safe comparison plus a per-channel 64-bit shadow, specified in
`docs/synchronization.md` § 1.1. **Open question:** whether Horizon resets a channel's
syncpoint on channel creation, and what the counter's value is at that moment — this
determines how the shadow is initialised. Must be measured in Phase 1.

---

## R6 — Cache coherency is unresolved in the reference

**Phase:** 1, 4 · **Impact:** high

GM20B is not IO-coherent. The reference allocates cached heap memory, registers it as
`cacheable = false`, performs no CPU cache maintenance in the winsys, and relies on a GPU
L2 flush that cannot invalidate the ARM D-cache. Its own smoke tests do cache maintenance
by hand. See `docs/memory-model.md` § 5.

**Risk:** the correct policy is not yet known. Options: (a) uncached backing for
`HOST_COHERENT`, (b) cached backing plus explicit flush/invalidate, (c) both, chosen per
Vulkan memory type.

**Unresolved.** Phase 1 test 5 must measure whether a GPU write is visible to a CPU read
without an invalidate, and whether a CPU write is visible to the GPU without a flush.
The answer decides which memory types Phase 4 advertises.

---

## R7 — Engine binding must be emitted in-stream

**Phase:** 4 · **Impact:** high

On Linux, nouveau binds engine classes to subchannels kernel-side. On Horizon, libnx's
`nvGpuChannelCreate` allocates only the 3D object context; the remaining classes must be
bound with in-stream `SET_OBJECT` methods, or an unbound subchannel MMU-faults and resets
the channel (`drm_shim.c:640-658`).

Additionally, calling `AllocObjCtx` a second time is rejected — libnx already did it.

**Mitigation:** `horizon/channel/` exposes an explicit "bind engines" operation the caller
must invoke before first use, rather than hiding it inside the first submit.

---

## R8 — Fixed GPU VA maps require a prior non-fixed reservation

**Phase:** 1, 4 · **Impact:** high

Tegra `nvgpu` rejects a bare fixed-VA reservation; a `FIXED` map is only valid inside a
previously reserved non-fixed range. NVK's VA allocator assumes it owns the address space,
so its heap must be constrained to a reservation we made.

**Mitigation:** `horizon/vm/` models reservations as first-class objects
(`docs/memory-model.md` § 3). Phase 4 initialises NVK's heap from a reservation rather
than patching in a hardcoded arena.

**Open question:** whether multiple reservations are cheaper or more robust than one large
one, and what the practical maximum reservation size is. Measure in Phase 1 test 4.

---

## R9 — Bind page size is hardcoded to 4 KiB in the reference

**Phase:** 1, 4 · **Impact:** medium

Every `MapBufferEx` uses `0x1000` regardless of the reservation's half. The reference's own
notes list this as an unresolved suspect for a GR fault.

**Mitigation:** page size derives from the containing reservation. Big-page reservations
are exercised in Phase 1 test 5.

---

## R10 — Cross-channel GPU-side dependencies are unimplemented anywhere

**Phase:** 4, 6 · **Impact:** medium

The reference resolves Vulkan wait-semaphores by blocking the CPU before submit, which is
why its own tests must `vkQueueWaitIdle` before every present to avoid a deadlock with the
WSI's internal present submit.

**Mitigation:** emit syncpoint-wait commands in the dependent channel
(`docs/synchronization.md` § 4). Until that exists, expose one channel per queue and
document any CPU-side cross-queue wait explicitly.

**Open question:** the exact Maxwell method encoding for a host1x syncpoint wait in the
GPFIFO stream is not present in the reference. Must be derived from envytools/deko3d and
validated in Phase 1.

---

## R11 — Timeline semaphores must be tracked or not advertised

**Phase:** 4 · **Impact:** medium

The reference advertises `DRM_CAP_SYNCOBJ_TIMELINE` because NVK's upload queue crashes
without it, but its `drmSyncobjTimelineWait` discards the requested points and its main
submit path never records them. That is a silent correctness failure.

**Mitigation:** implement timeline points properly, or do not advertise the feature and fix
the upload-queue path instead. Decision deferred to Phase 4, recorded in `STATUS.md`.

---

## R12 — "Zero-copy" still costs a full GPU copy per frame

**Phase:** 6 · **Impact:** medium (performance)

Because the reference's swapchain uses `WSI_IMAGE_TYPE_CPU`, Mesa's common WSI attaches a
`CmdCopyImageToBuffer` blit to every present, into a buffer the zero-copy path never reads.
`WSI_SWAPCHAIN_NO_BLIT` was planned and never implemented.

**Mitigation:** Phase 6 targets `NO_BLIT` from the start, and measures present cost with
and without to prove the difference.

---

## R17 — Scanout buffers are registered on the window, not the swapchain

**Phase:** 6 · **Impact:** high

`nwindowConfigureBuffer` registers buffers on the `NWindow`. Vulkan's recreation contract
creates the new swapchain *before* destroying the old one, so two swapchains legitimately
coexist over one window and the second registration collides — the reference hit a
hardware-confirmed `0xf59` crash on resolution change and fixed it with a file-scope
`g_zc_owner` pointer (present in the patch's 640-line WSI backend, absent from the stale
616-line tracked copy).

**Mitigation:** the *surface* owns the registration and hands it over explicitly
(`docs/wsi.md` § 2.5). Same algorithm, no global.

**Open question:** whether `nwindowReleaseBuffers` on a window with images still dequeued by
the old swapchain is safe. Must be tested in Phase 6 before relying on the handover.

---

## R18 — Privileged GR register writes reset a homebrew channel

**Phase:** 5 · **Impact:** high, deferred

The reference neuters `nvk_mme_set_priv_reg` in `nvk_cmd_draw.c` because privileged
graphics-register writes are rejected for homebrew channels and reset the channel, after
which every submit times out.

**Risk:** neutering it is a workaround whose functional cost is unknown — those writes
configure real GPU state, and something will eventually depend on them.

**Mitigation:** do not carry this patch forward blindly. When Phase 5 reaches the first
draw, determine which specific registers are rejected and what NVK loses by skipping them.
Record the answer rather than inheriting the no-op.

---

## R13 — Rust/NAK toolchain for a non-Linux target

**Phase:** 2 · **Impact:** high

NVK requires NAK and NIL, which are Rust. Building Rust `std` for a custom Horizon target
is the single most fragile part of the toolchain, and the reference describes it as having
taken substantial effort.

**Open question:** whether `std` is actually required, or whether `no_std` + `alloc`
suffices for NAK/NIL as Mesa links them. Answering this could remove the sysroot build
entirely. Investigate at the start of Phase 2, before building anything.

---

## R14 — Mesa version choice

**Phase:** 2 · **Impact:** medium

The reference pins Mesa 25.0.7. NVK moves quickly; a newer Mesa has a better `nvkmd`
abstraction (which is exactly the interface we implement) but may have drifted from the
reference's findings.

**Unresolved.** Decide at the start of Phase 2: pin the same 25.0.7 for maximum
applicability of the audit, or pin a current release for a better `nvkmd` surface and a
credible upstreaming path. Recommendation: a current stable release, accepting that some
reference line references become approximate. Record the chosen commit hash in
`toolchain/versions.env`.

---

## R15 — devkitPro is not version-pinnable in the usual sense

**Phase:** 2 · **Impact:** medium

devkitPro distributes rolling packages. Reproducibility depends on pinning package versions
in the container image and recording them.

**Mitigation:** `toolchain/versions.env` records exact package versions;
`scripts/` resolve everything through `$DEVKITPRO` / `$DEVKITA64` and never hardcode a
path. A check script enforces the absence of machine-specific absolute paths.

---

## R16 — Reference status claims are not independently verified

**Phase:** 0 · **Impact:** low, but affects planning

The reference documents claim a triangle, textures, indexed draws, depth, and a working
`nwindow` swapchain on real hardware. These claims are plausible and internally detailed,
but this audit verified only the **source**, not the hardware behaviour.

**Mitigation:** treat every reference claim as a hypothesis to re-test, never as a
guarantee. `docs/reference-analysis.md` marks which statements are source-verified and
which are claims.

---

## Pending decisions (require the project owner)

| # | Decision | Blocking |
|---|---|---|
| D1 | Any literal code reuse from the GPL/AGPL reference? (default: no) | nothing yet |
| D2 | Mesa version to pin (25.0.7 vs current stable) | Phase 2 start |
| D3 | Mesa as git submodule vs script-fetched checkout | Phase 2 start |
| D4 | Does the owner have a Switch available to run Phase 1 tests? | Phase 1 exit |
| D5 | Cache policy per Vulkan memory type — after R6 is measured | Phase 4 |
| D6 | Advertise timeline semaphores, or fix the upload queue instead | Phase 4 |
