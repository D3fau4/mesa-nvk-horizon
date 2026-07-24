# STATUS

**Last updated:** 2026-07-24
**Branch:** `claude/mesa-nvk-horizon-audit-q4ysep`

---

## Current phase

**Phase 0 — Audit of the reference ports. COMPLETE.**

No GPU code has been written. No Mesa tree has been fetched. Nothing in this repository
builds yet, by design — Phase 0's deliverables are documents.

---

## Completed work

### Repository skeleton
Created the directory structure from the project brief, plus `LICENSES/` content and
`.gitignore`. Structural clarifications (all documented in `docs/architecture.md` § 4):
`mesa/` is a pinned checkout rather than a vendored copy; `nvkmd_horizon` and the WSI
backend live in `mesa-patches/`; `toolchain/` holds declarative inputs and `scripts/`
holds executables.

### Audit documents
| File | Contents |
|---|---|
| `docs/reference-analysis.md` | Variant diff matrix, file tree, principal functions, init/alloc/submit/present flows, simulated functionality, synchronous operations, disabled NVK code, inconsistencies, reusable material with attribution |
| `docs/architecture.md` | Five layers, per-layer responsibilities, forbidden-dependency table, object model, error contract |
| `docs/memory-model.md` | Ten distinct memory concepts, the binding chain, VA layout, alignment/overflow, cache coherency, tiling, ownership, leak accounting |
| `docs/synchronization.md` | Syncpoints, wraparound, async submission, retirement, cross-channel dependencies, Vulkan mapping, timeouts, debug-synchronous mode |
| `docs/wsi.md` | The two divergent reference backends, no-global design, slot states, surface-owned registration, zero-copy requirements, acquire/present contracts |
| `docs/milestones.md` | Phases 0–6 with exit criteria, classified host / cross / hardware |
| `docs/known-risks.md` | 18 risks, 6 pending decisions |
| `CLAUDE.md` | Working rules, rejected designs, layer rules, coding and process rules |
| `LICENSES/README.md` | Licence policy and the copyleft hazard |

### Analysis method
Five read-only subagents analysed the DRM shim, memory/VM, submit/sync, WSI, and
toolchain in parallel. **Every finding cited below was re-verified by the main agent
against the source files**, which mattered: two subagents reached opposite conclusions
about triple buffering, and resolving it against the code produced the audit's most
important structural finding (see *Findings* below).

---

## Tests executed

| Command | Purpose | Result |
|---|---|---|
| `git status`, `git branch -a`, `git log` | inspect repository state | Empty repo, no commits, already on `claude/mesa-nvk-horizon-audit-q4ysep` |
| `unzip -q -o <archive> -d <alias>` × 4 | extract references read-only | OK — 88, 81, 88, 84 files |
| `find … \| xargs md5sum` + Python matrix | cross-variant comparison | 71 of 88 files identical across all four; 17 differ |
| `diff -rq` between all tree pairs | confirm the matrix | Confirmed |
| `grep -E '^diff --git' patches/*.patch` | patch scope | 24 files (master/triple-buffer) vs 10 (nvk-wsi/wsi-zero-copy) |
| Python extraction of the patch-embedded `wsi_common_switch.c` + `diff -u` | resolve the subagent conflict | 640 lines vs 616 tracked; 3 functional deltas |
| Targeted `sed`/`grep` reads of `drm_shim.c`, `switch_libc_shim.c`, `wsi_common_switch.c`, `wsi_common.c`, build scripts, `LICENSE`, `README.md` | verify subagent claims | All spot-checked claims confirmed |

**Build/compile status:** nothing was built. No compiler was invoked. No Mesa source was
fetched. There is nothing in this repository to build yet.

**Hardware status:** no Nintendo Switch is connected to this environment. **No claim in
any of these documents has been verified on hardware.**

---

## Findings summary

1. **The four "variants" are three near-identical snapshots.** 71 of 88 files are
   byte-identical across all four. `master` and `triple-buffer` differ only in `LICENSE`
   and `README.md` — not one line of code. Only `nvk-wsi` is a genuinely earlier snapshot.
2. **The differentiating code is inside the patch, not the tree.** The big patch embeds a
   640-line WSI backend with `minImageCount = 3` (triple buffering) and a `g_zc_owner`
   swapchain-recreate fix. The tracked standalone file is a stale 616-line copy with
   neither — and `apply-wsi-switch.sh:15` copies the stale one over the patched one,
   silently reverting both. The two build documents give contradictory instructions about
   whether to run it.
3. **Submission is synchronous by construction** — a full GPU drain after every submit, a
   CPU wait on every dependency before submitting, and a global mutex spanning both.
   Consequence: the reference's own tests must call `vkQueueWaitIdle` twice per frame.
4. **"Zero-copy" still costs a full-resolution GPU copy per frame**, because the swapchain
   requests `WSI_IMAGE_TYPE_CPU` and Mesa's common WSI then attaches a
   `CmdCopyImageToBuffer` into a buffer the zero-copy path never reads.
5. **Cache coherency is unresolved**: cached heap memory registered as `cacheable = false`,
   zero CPU cache maintenance in the winsys, and a GPU L2 flush standing in for an ARM
   D-cache invalidate it cannot perform.
6. **A concrete memory bug**: VM_BIND unmap does not clear `bo->gpu_va`, so
   `drmCloseBufferHandle` unmaps the same address a second time — potentially tearing down
   a live mapping if the VA has been recycled.
7. **Errors are logged but success is returned** — including after an MMU fault or a drain
   timeout in the submit path.
8. **Nothing in the toolchain is pinned** (Mesa by filename, untagged base image, undated
   Rust nightly), `/work` is hardcoded and non-overridable in 25+ executed sites, and
   personal Windows paths — two different usernames — leak into the documents and into the
   Mesa patch itself.
9. **Licence conflict**: `master` is GPL-2.0, the other three are AGPL-3.0, every README
   claims GPL-2.0-or-later, and the upstream being patched is MIT.

---

## Known failures / limitations of this audit

- **No hardware verification.** Everything is source analysis. The reference's claims of a
  working triangle, textures, depth and TV present are recorded as claims, not facts
  (`docs/reference-analysis.md` § 13).
- **Numeric `DRM_NOUVEAU_*` / NVIF constants unverified.** The `drm-uapi` headers are not
  vendored in the snapshots; they come from the Mesa tree fetched at build time. Only the
  symbolic names could be checked. This does not affect our design, which does not
  implement that uAPI.
- **libnx internals unverified.** Which device node each `nv*Init` opens is an inference
  from the libnx API, not stated in the snapshots. To be confirmed against libnx sources in
  Phase 1.
- **`UNDEFINED_SYMBOLS.txt` is of unknown provenance** — byte-identical across all four
  snapshots and listing symbols the shim now defines, so it reflects a stale link.

---

## Pending decisions

Full detail in `docs/known-risks.md`. Blocking ones first:

| # | Decision | Needed by | Recommendation |
|---|---|---|---|
| D1 | Any literal code reuse from the GPL/AGPL reference? | now | **No.** Re-derive facts. Provenance of the WSI backend cannot even be audited. |
| D4 | Is a Nintendo Switch available to run Phase 1 tests? | Phase 1 exit | Needed — otherwise Phase 1 stops at cross-compilation and every exit criterion stays unverified |
| D2 | Mesa version to pin: 25.0.7 or current stable? | Phase 2 start | Current stable — better `nvkmd` surface, credible upstreaming; audit line references become approximate |
| D3 | Mesa as git submodule or script-fetched checkout? | Phase 2 start | Submodule, with `scripts/fetch-mesa.sh` as the fallback for environments without submodule support |
| D5 | Cache policy per Vulkan memory type | Phase 4 | Blocked on measuring R6 in Phase 1 |
| D6 | Advertise timeline semaphores, or fix the upload queue? | Phase 4 | Defer |

---

## Next concrete task

**Phase 1, item 1 — `horizon/device/`: `nv` bring-up, GM20B query, teardown.**

Specifically:

1. Define the public header `horizon/include/horizon_gpu/device.h`:
   - `horizon_gpu_result` (carrying the libnx `Result` where one exists)
   - `horizon_gpu_device_info` — chipset, GPC/TPC counts, big-page size, VA region
     descriptors, syncpoint availability, arch — all **queried**, none hardcoded
   - `horizon_gpu_device_create(const horizon_gpu_device_create_info*, horizon_gpu_device**)`
   - `horizon_gpu_device_get_info`, `horizon_gpu_device_destroy`
2. Implement `horizon/device/device.c`:
   - ordered bring-up: `nvInitialize` → `nvFenceInit` → `nvMapInit` → `nvGpuInit` →
     `nvAddressSpaceCreate`, each result checked, each failure unwinding in reverse
   - `nvGpuGetCharacteristics()` is **required**; failure is an error, not a fallback to a
     hardcoded big-page size
   - live-object counters for the leak accounting in `docs/memory-model.md` § 8
   - no file-scope mutable state whatsoever
3. Write `tests/t_init.c` (Phase 1 test 1): create → assert the GM20B fields are plausible
   → destroy → repeat twice in one process → assert all counters are zero.
4. Add `scripts/check-layering.sh`: fail if anything under `horizon/` includes a Vulkan,
   Mesa or `nwindow` header.

This requires **no Mesa and no Rust** — it depends only on devkitA64 and libnx, which is
what makes Phase 1 independently testable.

**Blocked on:** D4 for hardware validation. The code, the test and the layering gate can be
written and cross-compiled without it; only the *verification* is blocked.

---

## Commit log for this phase

Prepared locally, not pushed (no authorisation to push).

| Commit | Scope |
|---|---|
| `docs: analyse reference NVK Horizon ports` | Skeleton, licences, all Phase 0 documents |
