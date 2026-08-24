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

**FIRED IN PHASE 4 (2026-07-28), not Phase 5.** The two writes are issued while the
*queue's context* is initialised, long before any draw, so the third hardware run met them
inside `vkCreateDevice`. The symptom is R18's, exactly: submission is asynchronous, so
`vkCreateDevice` returns 0 and the *next* kickoff on that channel fails with
`LibnxNvidiaError_Timeout` (0x00000d5c) and an error notifier set.

The registers, read off the decoded push dump: `gr_gpcs_tpcs_sm_disp_ctrl` (0x419f78,
clearing bit 3) and `sms_hww_warp_esp_report_mask` (0x419e44). What NVK loses, from its own
comments: FP helper invocation memory loads, without which one dEQP subgroups test fails
occasionally; and Out Of Range Address exceptions stay enabled for a case involving an
empty fragment shader. Neither is reachable by anything this port runs today.

Gated on `nvkmd_info::has_priv_reg_writes` (patch 0023) rather than neutered, so nouveau
keeps NVK's behaviour and the cost is stated at the declaration instead of vanishing into
a no-op. **Not yet confirmed on hardware** — the console that confirms it is the one that
closes Phase 4.

---

## R13 — Rust/NAK toolchain for a non-Linux target

**Phase:** 2 · **Impact:** high

NVK requires NAK and NIL, which are Rust. Building Rust `std` for a custom Horizon target
is the single most fragile part of the toolchain, and the reference describes it as having
taken substantial effort.

**Open question:** whether `std` is actually required, or whether `no_std` + `alloc`
suffices for NAK/NIL as Mesa links them. Answering this could remove the sysroot build
entirely. Investigate at the start of Phase 2, before building anything.

**ANSWERED at Phase 2 start (2026-07-26). Impact: high → low.**
See `docs/rust-toolchain.md` for the evidence, taken from the pinned tree
(`mesa-26.1.5`, `6a02618ccf6c`).

`std` *is* required as Mesa links NAK/NIL today — there is no `#![no_std]` anywhere in
`src/`, and both are built with `rust_abi : 'c'` (`--crate-type staticlib`), which bundles
the target's `libstd`. But the dependency is shallow: seven sites, all with direct
replacements, and the files using `std::process`/`fs`/`env` are `#[cfg(test)]`-gated and
never reach the driver. **No Rust sysroot is built in Phase 2**; closing the gap is a small
patch set for `mesa-patches/` in Phase 3/4.

Also resolved by the same investigation: rustc already ships
`aarch64-nintendo-switch-freestanding` (tier 3, `os = "horizon"`, `std = false`,
`panic = abort`), so milestone items 3 and 4 — a custom target JSON and a `rustc` wrapper —
are not needed. `toolchain/aarch64-horizon.json` is committed only as a drift snapshot of
that built-in spec, checked by `scripts/check-rust-target.sh`.

**Sub-risk still open:** devkitA64 compiles with `-mtp=soft`; the Rust target cannot express
that. Under `no_std + alloc` there should be no TLS at all, but that is unmeasured until
Rust is first built for Horizon in Phase 3/4. `alloc` will also need a global-allocator shim
over newlib's.

---

## R14 — Mesa version choice

**Phase:** 2 · **Impact:** medium

The reference pins Mesa 25.0.7. NVK moves quickly; a newer Mesa has a better `nvkmd`
abstraction (which is exactly the interface we implement) but may have drifted from the
reference's findings.

~~**Unresolved.**~~ Decide at the start of Phase 2: pin the same 25.0.7 for maximum
applicability of the audit, or pin a current release for a better `nvkmd` surface and a
credible upstreaming path. Recommendation: a current stable release, accepting that some
reference line references become approximate. Record the chosen commit hash in
`toolchain/versions.env`.

**RESOLVED at Phase 2 start (2026-07-26) — decision D2: `mesa-26.1.5`**, commit
`6a02618ccf6c5651ecb9cccbde571eb61fd73592` (tag object `3c008c397d06`, released
2026-07-15), i.e. the recommendation above. Pinned as `MESA_TAG`/`MESA_COMMIT` in
`toolchain/versions.env`; `scripts/fetch-mesa.sh` verifies the SHA rather than trusting the
tag.

Consequence accepted: line references in `docs/reference-analysis.md`, which describe a
25.0.7 tree, are now approximate. Treat them as pointers to the right file and concept, not
to the right line.

**Point release taken, 2026-08-10: `mesa-26.1.5` → `mesa-26.1.6`**, commit
`ffa422e53d59a4938b38abd5c3fc319555da31dd` (tag object `87f2e1c684613954`, released
2026-07-29, [release notes](https://docs.mesa3d.org/relnotes/26.1.6.html)). This is
*inside* the series D2 chose, not a new choice: the notes say "New features: None", and
of the 114 files it changes, **none is one of the 121 `mesa-patches/` writes to**. The
series applies unmodified, 75 of 75. Two NVK commits are in it, both worth having and
neither large — `nvk: free copy_memory_indirect_temps on command buffer destroy` (a leak
on the command-buffer destroy path) and `nvk: report fills from memory correctly`
(`VK_KHR_pipeline_executable_properties` reported spills where it meant fills).

**Point release taken, 2026-08-12: `mesa-26.1.6` → `mesa-26.1.7`**, commit
`e8617e4ca95fc655b0f13fd115c224d27eba2441` (tag object `09741f24d171`, released
2026-08-12, [release notes](https://docs.mesa3d.org/relnotes/26.1.7.html)). Again inside
the series D2 chose, again "New features: None" — but unlike 26.1.6 it is **not** a clean
miss: of the 121 files it changes, **six are among the 121 `mesa-patches/` writes to**.
The series still applies unmodified, 77 of 77, no fuzz and no rejects, and the six were
read rather than assumed:

| file | what 26.1.7 does to it | reaches a Switch? |
|---|---|---|
| `meson.build` | `dep_clc` is now looked up only for rusticl / microsoft_clc, and prefers a `mesa-libclc` fork over `libclc` | no — our native `mesa_clc` build enables neither, so it stops needing libclc at all |
| `src/util/os_misc.c` | a macOS `MAP_JIT` probe, all of it inside `DETECT_OS_APPLE` | no |
| `src/nouveau/compiler/nak/ir.rs` | adds `Dst::is_carry()` | only through the fix below |
| `src/nouveau/compiler/nak/opt_instr_sched_prepass.rs` | serialises carry-register access in the scheduler's dependency graph | **yes** — shader-model-independent, so it is live on GM20B's SM50 |
| `src/nouveau/compiler/nak/sm70_encode.rs` | `OpFMnMx` stops encoding `RZ` as src2 | no — SM70+ encoder, GM20B is SM50 |
| `src/nouveau/vulkan/nvkmd/nouveau/nvkmd_nouveau_pdev.c` | Turing compression re-enabled on nouveau 1.4.3 | no — the nouveau KMD backend, not `nvkmd_horizon` |

So exactly one upstream change in this release can alter code generated for a Switch, and
it is a correctness fix in the instruction scheduler. **Verified as a cross build, not on
hardware** — no console has run 26.1.7.

**Point release taken, 2026-08-24: `mesa-26.1.7` → `mesa-26.1.8`**, commit
`0fadfea4f394211946f308458f614839ef253ee8` (tag object `d6393a37abc5`, released
2026-08-19, [release notes](https://docs.mesa3d.org/relnotes/26.1.8.html)). Inside the
series D2 chose, "New features: None", and back to a clean miss: of the 98 files it
changes, **none is among the 131 `mesa-patches/` writes to**, and **nothing under
`src/nouveau/` changed at all** — no NVK, NAK, NIL or `nvkmd` file moved, so nothing in
this release can alter code generated for a Switch by way of the compiler. The series
applies unmodified, 49 of 49.

The one part of the delta that touches ground this repo stands on is
`subprojects/*-rs.wrap`: 28 wraps had their `source_url` host rewritten from
`crates.io/api/v1/crates/…` to `static.crates.io/crates/…`. Versions, filenames and
`source_hash` lines are byte-identical — the whole `subprojects/` delta is `source_url`
lines and nothing else — so the sha256 pinning `docs/rust-toolchain.md` relies on is
unchanged, and `scripts/fetch-mesa-subprojects.sh` reads the URL out of the wrap rather
than carrying its own. **Verified as a cross build, not on hardware** — no console has
run 26.1.8.

**The 26.2 series is a decision, and it is D21, not this.** `mesa-26.2.0` (2026-08-05)
is a development release whose own notes tell stability-minded users to wait for 26.2.1
— which now exists (`mesa-26.2.1`, 2026-08-20,
[notes](https://docs.mesa3d.org/relnotes/26.2.1.html)), so the condition D21 was waiting
on is met and the decision is live rather than deferred. It still is not taken here,
and it moves the ground under this port: 3588 files changed since 26.1.6, **53 of the 121
we patch among them** — 20 NAK files, 8 under `src/nouveau/vulkan/`, 4 in
`src/vulkan/wsi/`, both `src/vulkan/runtime/vk_image.{c,h}`, and the Rust helper crates
NAK is built on (`src/compiler/rust/{as_slice,smallvec,cfg,dataflow,bitset,nir,lib}.rs`,
plus `proc/as_slice.rs`). Rebasing onto it is a real piece of work with a real risk of
silent behaviour change on a driver whose evidence is all hardware runs, so it is put to
the owner rather than taken.

---

## R15 — devkitPro is not version-pinnable in the usual sense

**Phase:** 2 · **Impact:** medium

devkitPro distributes rolling packages. Reproducibility depends on pinning package versions
in the container image and recording them.

**Mitigation:** `toolchain/versions.env` records exact package versions;
`scripts/` resolve everything through `$DEVKITPRO` / `$DEVKITA64` and never hardcode a
path. A check script enforces the absence of machine-specific absolute paths.

**Implemented at Phase 2, and the risk turned out to be worse than described.**
`toolchain/versions.env` exists and `scripts/check-no-abs-paths.sh` is the gate (it found
one real violation on its first run, a hardcoded `/work` in `scripts/build-switch.sh`).
`scripts/print-toolchain-versions.sh --check` re-derives every pin from the live toolchain,
so the file is verifiable rather than declarative.

But *package versions are not the pin*, for two of the most important components:

- **libnx.** The `nx-dev` image installs the `libnx 4.12.0-1` package and then builds
  `switchbrew/libnx` git HEAD over it. Measured: `dkp-pacman -Qkk libnx` reports
  **226 total files, 205 altered**, with `libnx.a` and `libnxd.a` mismatching on size, MD5
  and SHA256. The package version describes 21 of 226 files.
- **Rust.** The image installs rustup's rolling `nightly`; a channel name pins nothing.

**Policy decision (owner, 2026-07-26): the Switch toolchain is NOT this project's to pin or
to update.** libnx, devkitA64 and the portlibs belong to the *environment* — whatever
`$DEVKITPRO` points at, or whatever is inside the container image the developer runs. This
repository neither freezes them nor upgrades them. Updating libnx is `dkp-pacman -Syu`, or a
newer image; it is not an edit here.

The reasoning: libnx gains fixes and new `nv`-service bindings frequently, and this backend
is written against exactly those services. Any version number recorded in this repository
could only be a stale copy of a fact that lives elsewhere — and a misleading one, because
the image builds libnx from git HEAD on top of the package, so `dkp-pacman -Q libnx`
describes only 21 of its 226 installed files (`dkp-pacman -Qkk libnx` → **226 total, 205
altered**, with `libnx.a` and `libnxd.a` mismatching on size, MD5 and SHA256).

So the original framing of this risk — "reproducibility depends on pinning package versions
and recording them" — is **rejected**, not implemented. What the project does instead is
**read and record, never control**:

- `toolchain/versions.env` has an ENVIRONMENT half that says only how to *reach* the
  toolchain (image repo/tag, prefix inside it, the `PATH` quirk, the target triple) and
  records nothing about its contents.
- `scripts/print-toolchain-versions.sh` is a read-only reporter answering "what am I
  building against right now?". It does not compare against a stored value, because there
  is none, and it cannot update anything.
- `scripts/package-horizon.sh` embeds that report plus the resolved image digest in
  `build/pkg/MANIFEST.txt`, next to each artefact's sha256 and the exact
  `HORIZON_NX_IMAGE=…@sha256:…` command that rebuilds against the same toolchain. **This is
  what keeps a hardware result attributable** when the inputs are not frozen.
- `scripts/check-rust-target.sh` reports drift in the Rust *target specification* — which
  decides how NAK/NIL get compiled — but exits 0, since the environment's nightly is
  expected to move. `--strict` where a change is the thing to catch.

Mesa and Meson stay pinned in the PINNED half: `mesa-patches/` applies to a specific tree,
and a Mesa that moved underneath would break Phase 3 silently rather than loudly. That is
the line — inputs this project *chooses* are pinned; the environment it *runs in* is not.

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
| ~~D2~~ | ~~Mesa version to pin (25.0.7 vs current stable)~~ — **closed 2026-07-26: `mesa-26.1.5` @ `6a02618ccf6c`** (R14) | — |
| ~~D3~~ | ~~Mesa as git submodule vs script-fetched checkout~~ — **closed 2026-07-26: script-fetched**, pin in `toolchain/versions.env`, `scripts/fetch-mesa.sh` verifies the SHA | — |
| ~~D4~~ | ~~Does the owner have a Switch available to run Phase 1 tests?~~ — **closed: yes**, Phase 1 ran on hardware | — |
| D5 | Cache policy per Vulkan memory type — after R6 is measured | Phase 4 |
| D6 | Advertise timeline semaphores, or fix the upload queue instead | Phase 4 |
