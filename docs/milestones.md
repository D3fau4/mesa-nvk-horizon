# Milestones

Phases are executed in order. Skipping or reordering requires a documented blocker
recorded in `STATUS.md` under *pending decisions*.

Each phase lists **exit criteria** that must be *verified*, and states what class of
verification is possible: **H** = host build/run, **X** = cross-compile only,
**HW** = real Nintendo Switch hardware.

---

## Phase 0 — Audit ✅ complete

Analyse the four reference ports; produce the audit documents; make no GPU code.

**Exit criteria**

- [x] `docs/reference-analysis.md` with variant diff table, file tree, call flows,
      simulated functionality, synchronous operations, NVK modifications, inconsistencies,
      reusable code with attribution.
- [x] `docs/architecture.md` defining the five layers and forbidden dependencies.
- [x] `docs/memory-model.md` distinguishing every address/handle/layout concept.
- [x] `docs/synchronization.md` describing the async submission model.
- [x] `docs/wsi.md`, `docs/known-risks.md`, `docs/milestones.md`, `STATUS.md`.
- [x] Repository skeleton created; no Mesa tree vendored.

---

## Phase 1 — `horizon/` standalone GPU layer

Build `horizon_gpu` with **no Mesa dependency at all**. This is the phase that proves the
architecture: if it needs Vulkan headers, the design is wrong.

**Order of implementation**

1. `device/` — `nv` bring-up, GM20B query, teardown
2. `memory/` — aligned CPU allocation, `NvMap` create/close, id/handle accessors
3. `vm/` — address space create, VA reserve/release
4. `vm/` — map/unmap with explicit PTE kind and page size
5. `channel/` — GPFIFO channel create/destroy, Zcull, engine binds
6. `submit/` — minimal GPFIFO entry + kickoff, **asynchronous**
7. `sync/` — syncpoint read, fence get, fence wait with timeout
8. teardown of everything in reverse order with leak accounting

**Tests** (`tests/`) — one per numbered item, runnable as a standalone `.nro`:

| # | Test | Verifies |
|---|---|---|
| 1 | `t_init` | `nv` up/down, GM20B fields sane, repeatable |
| 2 | `t_alloc` | aligned allocation, size/alignment overflow rejection |
| 3 | `t_nvmap` | `NvMap` create/close, id ≠ 0, handle valid, double-close safe |
| 4 | `t_va_reserve` | VA reserve/release, exhaustion behaviour, alignment |
| 5 | `t_map` | map/unmap at a fixed VA, correct kind, unmap invalidates |
| 6 | `t_channel` | channel create/destroy, syncpoint id assigned, N channels |
| 7 | `t_submit` | minimal command list executes (no-op + fence increment) |
| 8 | `t_syncpt` | syncpoint value increases monotonically per submit |
| 9 | `t_fence_wait` | fence wait returns after completion; timeout path returns timeout |
| 10 | `t_teardown` | full destroy with no leaked `NvMap`/VA/channel; run twice in a process |

**Exit criteria**

- All ten tests build for the host where meaningful (**H**) and cross-compile (**X**).
- Tests 1–10 pass on **HW**, with the exact console output recorded in `STATUS.md`.
- `horizon/` compiles with zero references to Vulkan, Mesa or `nwindow`, verified by a
  grep gate in `scripts/check-layering.sh`.
- Multiple submits are in flight simultaneously in test 7 without a CPU wait between them.

**Explicit non-goal:** no Mesa, no NVK, no shaders, no images.

---

## Phase 2 — Toolchain

Reproducible cross-compilation, pinned, with no machine-specific absolute paths.

1. devkitA64 / devkitPro pinned by package version, resolved from `$DEVKITPRO`
2. Meson cross file for `horizon` + Mesa (`toolchain/horizon-aarch64.cross`)
3. Rust target JSON for Horizon (`toolchain/aarch64-horizon.json`) for NAK/NIL
4. `rustc` wrapper if still required, with its reason documented
5. Rust `std`/`core` sysroot build, pinned nightly
6. Mesa host tools (native build for generators)
7. `scripts/` for fetch, configure, build, package — each idempotent
8. Every version pinned in `toolchain/versions.env`

**Exit criteria**

- A clean container reproduces the toolchain from `scripts/` alone (**H**).
- `grep` for `/home/`, `/work`, `D:\\`, `/mnt/` in `toolchain/` and `scripts/` returns
  nothing (gate in `scripts/check-no-abs-paths.sh`).
- Phase 1 tests cross-compile with the new cross file (**X**).

---

## Phase 3 — Minimal Horizon support in Mesa

Independent, individually reviewable patches in `mesa-patches/`:

1. OS detection (`detect_os.h` → `DETECT_OS_HORIZON`)
2. Meson `host_machine.system() == 'horizon'` handling
3. newlib/libnx gaps
4. threads (C11 threads over pthread/newlib)
5. timers / clocks
6. physical memory / page size queries
7. endianness
8. build ID

**Exit criteria**

- Each item is a separate patch file with a header explaining it (**X**).
- Mesa configures for `horizon` and builds the non-driver core (**X**).
- No patch mixes functional change with formatting.

---

## Phase 4 — `nvkmd_horizon`

1. device
2. GM20B info → `nvkmd_pdev_info`
3. memory objects
4. VA heap
5. bind / unbind
6. queue and channel
7. submit
8. fences
9. on-demand waits only
10. destruction

**First mandatory Vulkan result:**

```
vkCreateInstance
vkEnumeratePhysicalDevices
vkCreateDevice
vkCreateBuffer
vkAllocateMemory
vkBindBufferMemory
vkCmdFillBuffer
vkQueueSubmit
vkWaitForFences
→ CPU reads back the filled pattern and validates it
```

**Exit criteria**

- The sequence above runs on **HW** and the CPU-side validation passes.
- No `vkQueueWaitIdle`/`vkDeviceWaitIdle` inserted internally to make it pass.
- Recorded console log in `STATUS.md`.

---

## Phase 5 — Off-screen rendering

1. transfers (copy buffer↔buffer, buffer↔image)
2. compute dispatch
3. off-screen images
4. clears
5. triangle to an off-screen image
6. textures
7. depth
8. format coverage
9. several submits in flight simultaneously

**Exit criteria**

- Each item verified on **HW** by CPU readback of the rendered result, not by absence of
  errors.
- Item 9 demonstrates ≥ 2 submits in flight with no intervening CPU wait.

---

## Phase 6 — Horizon WSI

- Swapchain with **no global state**
- 2 to 4 images, real double and triple buffering
- Explicit slot ownership (free / acquired / rendering / queued)
- Safe recreation and destruction
- Zero-copy when the format/tiling combination allows it
- Documented fallback when it does not
- `nwindowDequeueBuffer` / `nwindowQueueBuffer`
- Correct fence-based synchronisation at acquire and present
- Resize handling
- Error recovery (`VK_ERROR_OUT_OF_DATE_KHR`, `VK_SUBOPTIMAL_KHR`)

**Exit criteria**

- A rotating-colour swapchain test presents on **HW** at the display refresh rate.
- Triple buffering measurably differs from double buffering (frame pacing recorded).
- Two swapchains can exist and be destroyed independently in one process.
- Zero-copy on/off is a runtime-observable decision with a logged reason.
