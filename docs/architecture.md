# Architecture

## 1. Layers

```
┌───────────────────────────────────────────────────────────────────────────┐
│  Vulkan application (homebrew .nro / .elf)                                 │
├───────────────────────────────────────────────────────────────────────────┤
│  Mesa / NVK                                                               │
│    Vulkan runtime, NIR, NAK (Rust), NIL (Rust), nvk_* objects             │
│    Statically linked ICD — no loader, no dlopen                          │
├───────────────────────────────────────────────────────────────────────────┤
│  nvkmd_horizon                          [lives in mesa-patches/]          │
│    Implements NVK's nvkmd interface (nvkmd_dev / nvkmd_mem / nvkmd_va /   │
│    nvkmd_ctx) on top of horizon_gpu. Knows Vulkan; knows nothing of libnx.│
├───────────────────────────────────────────────────────────────────────────┤
│  disk_cache_horizon                     [lives in mesa-patches/]          │
│    Replaces Mesa's util/disk_cache_os.c. Knows Mesa's util; knows nothing │
│    of Vulkan, NVK or libnx. Beside nvkmd_horizon, not below it.           │
├───────────────────────────────────────────────────────────────────────────┤
│  horizon_gpu                            [this repo, horizon/]             │
│    Device, memory, VA, channel, submit, sync, surface info, debug,        │
│    and the shader cache's blob store.                                     │
│    Pure C. No Vulkan. No Mesa. No WSI. Explicit contexts.                 │
├───────────────────────────────────────────────────────────────────────────┤
│  libnx                                                                    │
│    nvMap*, nvAddressSpace*, nvGpuChannel*, nvFence*, nvinfo, nwindow, vi  │
├───────────────────────────────────────────────────────────────────────────┤
│  Horizon OS — nv services (nvdrv), host1x, nvgpu                          │
└───────────────────────────────────────────────────────────────────────────┘
```

WSI Horizon sits *beside* `nvkmd_horizon`, not below it: it is a Vulkan-level component
(`src/vulkan/wsi/`) that consumes `horizon_gpu`'s **surface-info** primitives and libnx
`nwindow`, and consumes NVK images through the normal WSI image interface.

```
NVK ──┬── nvkmd_horizon ──┐
      │                   ├── horizon_gpu ── libnx ── Horizon
      └── wsi_horizon ────┘        ▲
                    │              │
                    └── nwindow ───┘   (nwindow used directly by WSI only)
```

## 2. Responsibilities

### `horizon/` — horizon_gpu

Owns every interaction with the `nv` services. Provides:

| Area | Responsibility |
|---|---|
| `device/` | Bring up / tear down `nv`; open the GPU; query GM20B characteristics (chipset, GPC/TPC counts, big-page size, VA regions, syncpoint availability). |
| `memory/` | Allocate page-aligned CPU-backed storage; create/destroy `NvMap` objects; expose the `NvMap` id and handle; cache maintenance (flush / invalidate). |
| `vm/` | Create the GPU address space; reserve and release VA ranges; map/unmap `NvMap` regions at a given VA with a given PTE kind and page size. |
| `channel/` | Create/destroy GPFIFO channels; bind Zcull; bind engine objects to subchannels; query the channel error notifier. |
| `submit/` | Build GPFIFO entries from caller-provided command lists; append and kick off; never block. |
| `sync/` | Syncpoint identity and monotonic values; fence creation, query, wait with an explicit timeout; fence comparison. |
| `surface/` | Describe a presentable allocation: `NvMap` id, offset, pitch, block-height, PTE kind, colour format, GPU VA. **No `nwindow` calls, no queueing.** |
| `debug/` | Structured logging, optional synchronous mode, channel error decoding, GPFIFO dumping. |
| `cache/` | A keyed blob store on a Horizon filesystem: one append-only, CRC-validated file, an in-memory index, in-place compaction. **Not a GPU concern**, and it is here for a reason given below. |

**Prohibited in `horizon/`:** any `#include <vulkan/*>`, any Mesa header, any
`nwindowQueueBuffer`/`nwindowDequeueBuffer` call, any global mutable device state.

`cache/` is the one entry in that table that is not about the GPU, and it is a
deliberate exception rather than drift. The shader disk cache's whole value is what it
does with a file that is *wrong* — truncated by a flat battery, bit-flipped by a
failing card, written by a different driver build. A format tested only where it runs
is a format nobody has tested against that damage, because producing a damaged file on
a console means an operator with a card reader. Being libnx-free and Mesa-free is what
lets `scripts/run-host-tests.sh` compile the same translation unit the console does
and hand it those files under ASan and UBSan.

It also carries an invariant the rest of the layer does not need: **no path operation
after `open()`**. libnx routes `open`, `stat`, `unlink`, `mkdir` and `rename` through
one unlocked global buffer, and the cache is reached from Mesa's put queue — i.e. from
a thread the caller never named. See [`shader-cache.md`](shader-cache.md).

### `nvkmd_horizon`

Translates NVK's kernel-mode-driver abstraction into `horizon_gpu` calls. It owns the
Vulkan-side lifetime rules (memory object refcounts, VA heap policy, queue/context
mapping) and nothing else. It must not call libnx.

### WSI Horizon

Owns swapchain state, image slots, `nwindow` dequeue/queue, buffer-count policy,
resize and error recovery. It obtains tiling/kind/`NvMap`-id facts from
`horizon_gpu`'s surface layer rather than reaching into `nvkmd_horizon`.

### `disk_cache_horizon`

Replaces `src/util/disk_cache_os.c` on Horizon and nothing else: Mesa's generic
`disk_cache.c` — key derivation, the put queue, compression, the statistics — is used
unchanged. It owns the entry layout (an uncompressed length, then the deflated blob),
the lock that serialises the store against Mesa's put queue, and the cache path. It
sits beside `nvkmd_horizon` rather than under it: it is a `src/util` component and
knows nothing of Vulkan, NVK or the WSI, and nothing of libnx either — the platform is
entirely behind `horizon_gpu`'s blob store.

### `compat/`

Only functions that newlib/libnx genuinely lack and that Mesa genuinely needs. Each entry
must be justified in a comment naming the Mesa call site. `mmap`, `open`, `close`, `stat`
interposition is **not** a valid use of this directory.

## 3. Forbidden dependencies

| From | To | Reason |
|---|---|---|
| `horizon/` | Vulkan / Mesa / NVK | The layer must be testable and shippable without Mesa (Phase 1 gate). |
| `horizon/` | `nwindow` / `vi` queueing | Presentation policy is a WSI concern; `horizon/` only describes surfaces. |
| `nvkmd_horizon` | libnx | Keeps the OS surface in one auditable place. |
| WSI Horizon | `nvkmd_horizon` internals | WSI must work through public NVK/Mesa interfaces. |
| `disk_cache_horizon` | Vulkan / NVK / WSI / libnx | It is a `src/util` component; the platform is behind `horizon_gpu`'s blob store. |
| `horizon/cache/` | a path operation after `open()` | libnx's devoptab shares one unlocked path buffer, and the store is reached from Mesa's put queue. |
| anything | a DRM/nouveau uAPI shim | The whole point of the rewrite. |
| anything | global device/window/swapchain state | Prevents multi-object correctness and clean teardown. |

## 4. Structural deviations from the requested skeleton

The skeleton in the task description is followed exactly, with these clarifications
(no directories removed, only their meaning fixed):

1. **`mesa/` is a pinned checkout, not a vendored copy.** It is populated by
   `scripts/fetch-mesa.sh` at a recorded commit hash, or added as a git submodule. It is
   listed in `.gitignore` if fetched rather than submoduled. Rationale: the reference
   ports carry whole edited Mesa files (`winsys/mesa-edits/`) *and* a patch that overlaps
   them, which makes it impossible to tell what the real delta is — see
   `docs/reference-analysis.md` § Inconsistencies.

2. **`nvkmd_horizon` and the WSI backend live in `mesa-patches/`, not in `horizon/`.**
   They are Mesa source files and belong to Mesa's build; keeping them as patches (or as
   files that a script copies into the pinned tree with a manifest) preserves a reviewable
   delta and a credible upstreaming path. `horizon/` stays Vulkan-free, which is what makes
   Phase 1 independently testable.
   The series format, the header every patch carries and the idempotent applier are
   defined in `mesa-patches/README.md`.

3. **`toolchain/` holds declarative inputs; `scripts/` holds executables.** Cross files,
   the Rust target JSON and version pins live in `toolchain/`; anything that runs lives in
   `scripts/`. The reference ports mix both at the repository root, which is why several of
   their scripts reference files that no longer exist in the branch they ship in.

4. **`horizon/include/` is the only public surface of the layer.** Everything under
   `horizon/<area>/` is implementation. Tests link against the public headers only.

## 5. Object model

Every object is explicit and owned:

```c
horizon_gpu_device   *dev;   /* owns nv session, address space, GM20B info    */
horizon_gpu_mem      *mem;   /* owns CPU backing + NvMap                       */
horizon_gpu_va_range *va;    /* owns a reserved GPU VA interval                */
horizon_gpu_channel  *chan;  /* owns an NvGpuChannel + its syncpoint           */
horizon_gpu_fence     fence; /* value type: {syncpoint id, threshold}          */
```

No function reads ambient state. `horizon_gpu_device` is created by the caller, passed
explicitly, and destroyed by the caller. Two devices in one process must not interfere —
even though the Switch has one GPU, this is the constraint that keeps teardown honest.

Bindings between the three memory concepts are always explicit and never implied:

```
CPU pointer  ──(register)──▶  NvMap  ──(map at VA, kind, page size)──▶  GPU VA
```

See `docs/memory-model.md`.

## 6. Error handling contract

- Every `horizon_gpu_*` function returns a `horizon_gpu_result` carrying the libnx
  `Result` when one exists. No function returns `void` if it can fail.
- No function retries internally on a timeout; timeouts are surfaced.
- A channel that has faulted is marked lost; every subsequent submit on it fails fast
  with the decoded error-notifier reason rather than timing out.
