# STATUS

**Last updated:** 2026-07-28
**Branch:** `claude/mesa-nvk-horizon-vszk01`

---

## Current phase

**Phase 4 — `nvkmd_horizon`. The backend exists and NVK builds.
All ten milestone items are implemented and the mandatory
Vulkan sequence links as a .nro. What is owed is a hardware run.**

**Step 1** is the interface reading: what `nvkmd` requires, operation by
operation, with its semantics, against what `horizon_gpu` already
provides. It exists to answer one question before any code: **is Phase 4
implementation, or also an extension of `horizon/`?** The answer is
*both*, and the extensions are six, small, and enumerated — four of them
conditional on Vulkan-side decisions that are named (D9–D12) and
deliberately not taken yet.

**Step 2** closed the six unresolved libc symbols, and the result is an
artefact rather than an argument: **an executable that pulls NIR,
SPIR-V, `nir_print`, `mesa_log` and `util_sparse_array` out of Mesa's
built core links, with zero undefined symbols.** Two new patches (0013,
0014 — the series is now **fourteen**), nothing added to `compat/`, and
the two symbols that remain unresolved in the archives are in members
nothing references, which the link demonstrates rather than assumes.

**Step 3** reproduced the Rust failure first
(`error[E0463]: can't find crate for 'std'`, at `mesa/meson.build:841`)
and then got past it: **Rust has been compiled and linked for Horizon
for the first time in this project** — a `no_std` + `alloc` staticlib,
linked by devkitA64 into a Horizon ELF with zero undefined symbols and
**zero TLS relocations**, which also measures the `-mtp=soft` sub-risk
`docs/rust-toolchain.md` § 4 left open. `scripts/fetch-rust-crates.sh`
is new: the container has no network and `-Zbuild-std` needs crates.io,
so the 30 packages are fetched on the host against Rust's own lockfile
checksums, and the check was broken four ways before being trusted.

**Phase 3 is closed except for one thing, unchanged:** `t_threads` and
`t_ostime` have never run on a console (they pass on Eden — 67/67 and
43/43). That does not block Phase 4 and is recorded as owed. The full
Phase 3 state is kept verbatim below under "Phase 3 — the state it
closed in".

All three known blockers carried into this phase are now addressed: the
libc symbols (step 2), Rust (step 3), and `-Db_staticpic=false`, which
remains mandatory with its gate running after every Mesa build.

**What is not written yet is `nvkmd_horizon` itself.** The nearest
obstacle is named and measured: NAK and NIL are still `std` crates, so
Mesa's Rust half does not build until the seven substitutions
`docs/rust-toolchain.md` § 2 lists become patches.

**Step 4** is the build machine. `meson setup -Dvulkan-drivers=nouveau`
now **configures and builds for Horizon**: the C half of the nouveau
Vulkan driver compiles, and bindgen generates NAK's, NIL's and
`compiler`'s bindings. Getting there needed four things the toolchain
did not have — libclang, bindgen, cbindgen and a Rust sysroot for a
tier-3 target — and then a fifth, `mesa_clc`, because NVK compiles two
OpenCL C files into SPIR-V at build time. On the owner's suggestion
these stopped being mounted directories and became a **derived Docker
image** (`toolchain/Dockerfile`), which is both tidier and the reason
`build/` is disposable again. Patch 0015 makes the driver a static
library where there is no dynamic loader; the series is fifteen.

**Where it stops, and why that is progress:** `rustc` fails with five
hundred errors that are one fact — with no `std` in the sysroot there
is no prelude. That is the `no_std` conversion arriving as a compiler
error instead of as a plan, and **D13 is now decided by measurement**:
two `no_std` Rust staticlibs cannot be linked into one binary
(`multiple definition of __rust_alloc`, five symbols), so NAK and NIL
become rlibs behind a single staticlib that carries the one
`#[global_allocator]` and `#[panic_handler]`.

**Step 5** built it. **`libnouveau_rust_runtime.a`, 15 292 744 bytes,
is Mesa's entire Rust half compiled `#![no_std]` for the Switch
target** — NAK, NIL, `compiler`, `nvidia_headers`, `bitview`,
`nak_latencies` and both bindgen crates — with **0 TLS relocations** and
**exactly one** definition of each allocator and panic symbol.
Reproduced byte-identically from a reset `mesa/` and a re-applied
series. Patches 0016 and 0017; the series is seventeen.

**What the build stops on now is the right thing:**
`src/nouveau/winsys` — libdrm talking to the nouveau kernel driver —
does not compile against newlib, and it is not supposed to. Replacing
it *is* `nvkmd_horizon`.

**`nvkmd_horizon` itself now exists.** `libnvk.a`, 94 162 bytes, is the
nouveau Vulkan driver compiled for Horizon against the `nv` system
services — `vk_icdGetInstanceProcAddr` and `nvk_CreateInstance` defined,
0 TLS relocations, built end to end from a clean tree by
`scripts/build-mesa-nvk.sh` in 4 m 17 s. **D9 and D10 are closed**, by
the hardware and by where the knowledge belongs respectively, and the
seven rejected designs are checked one by one in the section below.
**Items 3, 4 and 5 followed** — memory objects, the VA heap and
bind/unbind — so the driver can now allocate memory, reserve GPU address
space and map one into the other. Items 6 to 10 return
`VK_ERROR_FEATURE_NOT_PRESENT` with their item number, so every gap is
named. Patches 0018 and 0019; the series is nineteen.

**All ten items are implemented and `t_vulkan.nro` links** — 14 000 184
bytes, zero undefined symbols, zero multiple definitions. D11 is
resolved native over syncpoints and D8 where it actually bites: the
absolute deadline becomes a relative duration exactly once. The
`compiler_builtins`-versus-newlib question is answered by that link:
**no collision**. The series is twenty-three.

---

## Phase 4 — step 1: what `nvkmd` requires, against what `horizon_gpu` has (2026-07-28)

**Class: reading of the pinned tree (S).** No build was run in this
step; nothing here is a cross build, an emulator result or a hardware
result. Every claim cites `file:line` in `mesa-26.1.5` @
`6a02618ccf6c5651ecb9cccbde571eb61fd73592`, in the Vulkan runtime of the
same tree, or in the libnx headers extracted from
`ghcr.io/d3fau4/nx-dev@sha256:61a38fe4…` — the same digest Phase 2
recorded.

### Method

Read in full, not skimmed: `nvkmd/nvkmd.h` (627 lines),
`nvkmd/nvkmd.c` (489), and all five files of the `nouveau/` backend
(`nvkmd_nouveau.h` 138, `_pdev.c` 190, `_dev.c` 99, `_mem.c` 312,
`_va.c` 255, `_ctx.c` 470).

An ops table does not state its own semantics, so the callers were read
too: `nvk_physical_device.c`, `nvk_device.c`, `nvk_queue.c`,
`nvk_device_memory.c`, and — because the entry point is the first thing
that does not fit — the Vulkan runtime's `vk_instance.[ch]` and
`vk_sync.h`.

libnx's headers were extracted from the image (`docker cp`, into
`build/probe/`, never `/tmp`) because "does `horizon_gpu` already answer
this" sometimes resolves to **libnx has it and `horizon_gpu` does not
expose it**, which is a different disposition from "Horizon cannot do
this".

### The entry point is the first thing that does not fit — and the runtime already solved it

`nvkmd_try_create_pdev_for_drm()` takes a `struct _drmDevice *` and
forwards unconditionally to the one backend (`nvkmd.c:86-93`). NVK
registers it as `vk_instance::physical_devices.try_create_for_drm`
(`nvk_instance.c:167-168`), and the runtime's DRM path calls
`drmGetDevices2()` (`vk_instance.c:418`).

None of that has to be faked. The runtime has a second hook:

```
/** Enumerate physical devices for this instance
 *  … If this callback is not set, try_create_for_drm will be used for
 *  enumeration. */
VkResult (*enumerate)(struct vk_instance *instance);      vk_instance.h:158
```

tried **first**, before any DRM enumeration (`vk_instance.c:445-449`).
So Horizon enumeration is a driver-side callback that never mentions
DRM: rejected designs 1 and 3 stay closed without a render node, a
sentinel fd, or a `drmDevice` shim. What the patch series has to add is
a second creation entry beside `nvkmd_try_create_pdev_for_drm` — the
existing one is hardcoded to nouveau *and* its signature carries a type
this platform does not have.

### Table 1 — `nvkmd_pdev_ops` (`nvkmd.h:126-136`)

| Op | Required? | Semantics | `horizon_gpu` today |
|---|---|---|---|
| `destroy` | yes | frees the pdev | `horizon_gpu_device_destroy` — but see the pdev/dev split below |
| `get_vram_used` | called unguarded by the inline (`nvkmd.h:396`), but its only caller is behind `kmd_info.has_get_vram_used` (`nvk_physical_device.c:1538`) | bytes of VRAM in use | GM20B has no VRAM. Report the flag false and answer 0 |
| `get_drm_primary_fd` | **optional** — NULL-checked (`nvkmd.h:402`) | DRM primary fd for display | leave NULL |
| `create_dev` | yes | opens the per-device state | `horizon_gpu_device_create` |

**The pdev/dev split is ours to make.** nouveau opens the render node
twice — once for the pdev, once for the dev (`_pdev.c:70`, `_dev.c:40`).
On Horizon the `nv` session is per process and `horizon_gpu_device` owns
it (device.h:96-104), so one `horizon_gpu_device` has to serve both, or
the pdev has to hold GM20B facts only and the dev create the device.
The second reading is the one that matches `nvkmd`'s own lifetime rules
(a pdev exists before any `VkDevice`), and it needs the characteristics
query to be possible without a full device — which today it is not.
**Decision for item 1, not taken here.**

### Table 2 — `nvkmd_dev_ops` (`nvkmd.h:161-195`)

| Op | Required? | Semantics | `horizon_gpu` today |
|---|---|---|---|
| `destroy` | yes | | `horizon_gpu_device_destroy` |
| `get_gpu_timestamp` | **yes, and reached** — `nvk_device.c:172`, `vkGetCalibratedTimestampsKHR` | GPU clock in ns; nouveau reads PTIMER (`nouveau_device.c:606-613`) | **not exposed.** libnx has the same clock: `nvGpuGetTimestamp` (`nvidia/gpu.h:16`) over `nvioctlNvhostCtrlGpu_GetGpuTime` (`nvidia/ioctl.h:262`) → **extension 1** |
| `get_drm_fd` | **optional** — NULL-checked (`nvkmd.h:431`) | | leave NULL |
| `alloc_mem` | yes | allocate + **give it a VA and bind it** — nouveau does both at creation (`_mem.c:56-67`) | `horizon_gpu_mem_create` + `vm_reserve` + `vm_map` |
| `alloc_tiled_mem` | yes (called at `nvk_cmd_draw.c:1055`, `nvk_device_memory.c:229`) | same plus `pte_kind` + `tile_mode`; the kind is carried on the **VA** (`_mem.c:35,56-60`) | `horizon_gpu_vm_map` takes the PTE kind. `tile_mode` has no NvMap equivalent — to be measured |
| `import_dma_buf` | in the table; callers are external-memory extensions (`nvk_device_memory.c:81,219`) | | no dma-buf on Horizon. NvMap has id-based sharing (`nvMapLoadRemote`, `nvidia/map.h:19`), which is **not** the same thing. Disable the extensions; the op returns an error |
| `alloc_va` | yes | reserve VA, optionally sparse/fixed | `horizon_gpu_vm_reserve` |
| `create_ctx` | yes | exec ctx or bind ctx | `horizon_gpu_channel_create` |

### Table 3 — `nvkmd_mem_ops` (`nvkmd.h:210-240`)

| Op | Required? | Semantics | `horizon_gpu` today |
|---|---|---|---|
| `free` | yes | | `horizon_gpu_mem_destroy` |
| `map` / `unmap` | yes (`nvkmd.c:397,416,440,446`) | CPU mapping, refcounted for internal maps, single for the client map | **simpler here, not missing**: the allocation *is* host memory registered with NvMap (memory-model § 1), so `horizon_gpu_mem_cpu_ptr` is valid for the object's whole life. map/unmap become bookkeeping |
| `overmap` | only `VK_EXT_map_memory_placed` (`nvk_device_memory.c:416`) | replace a client map with `PROT_NONE` so the address stays reserved | impossible without `mmap`. **The extension must be turned off** — see the defect below |
| `sync_to_gpu` / `sync_from_gpu` | called **only** when `!util_has_cache_ops()` (`nvkmd.c:464-468,483-487`) | CPU cache maintenance over a range | `horizon_gpu_mem_flush` / `_invalidate`. See the aarch64 note below |
| `export_dma_buf` | asserts `NVKMD_MEM_SHARED` (`nvkmd.h:555`) | | unreachable with external memory off |
| `log_handle` | `NVK_DEBUG_VM` only (`nvkmd.c:210`) | a handle for logging | `horizon_gpu_mem_get_handle` |

**A defect found by reading, worth a patch.** `nvkmd_info` declares
`has_map_fixed` and `has_overmap` (`nvkmd.h:121-122`), nouveau sets both
true (`_pdev.c:102-103`) — and **nothing in Mesa 26.1.5 reads either
flag.** Measured over the whole tree:

```
$ grep -rn "has_map_fixed\|has_overmap" src/ | grep -v "nvkmd.h:"
src/nouveau/vulkan/nvkmd/nouveau/nvkmd_nouveau_pdev.c:102:      .has_map_fixed = true,
src/nouveau/vulkan/nvkmd/nouveau/nvkmd_nouveau_pdev.c:103:      .has_overmap = true,
```

so `EXT_map_memory_placed` is advertised unconditionally
(`nvk_physical_device.c:256`). A capability flag with no consumer is
invisible until a second backend exists, which is exactly the situation.
Gating that extension on the flags the interface already carries is a
one-line general fix with a stated defect behind it — the shape
`mesa-patches/README.md` requires. `has_dma_buf` is the same case.

**The cache question, and why it is not settled by reading.**
`util_has_cache_ops()` returns true unconditionally on aarch64
(`cache_ops.h:50`), so on Horizon the two `sync_*` ops would never be
called and Mesa would do the maintenance itself with `dc cvac` /
`dc civac` and a granule read from `CTR_EL0` (`cache_ops_aarch64.c:44-107`).
Whether those EL0 instructions and that system register are permitted
under Horizon is a **hardware** question. There is a precedent — libnx's
own `armDCacheFlush` uses the same instruction — but a precedent is not
a measurement, and `nc_atom_size_B` (`nvkmd.h`/`nv_device_info.h`) is
`util_cache_granularity()` in nouveau (`_pdev.c:109`), i.e. that same
`CTR_EL0` read, reported to applications as `nonCoherentAtomSize`.
Recorded as an open question; the ops are implementable from
`horizon_gpu` either way.

### Table 4 — `nvkmd_va_ops` (`nvkmd.h:271-285`)

| Op | Required? | Semantics | `horizon_gpu` today |
|---|---|---|---|
| `free` | yes | release the reservation | `horizon_gpu_vm_release` |
| `bind_mem` | yes | map `mem[mem_offset, +range)` at `va->addr + va_offset`, with `va->pte_kind`; every bound is asserted in core (`nvkmd.c:232-240`) | `horizon_gpu_vm_map` — the same shape, because **R8 already forced FIXED-inside-a-reservation**, which is what `nvkmd` assumes anyway |
| `unbind` | yes | unmap a sub-range **addressed by (offset, range)** | `horizon_gpu_vm_unmap` takes the mapping object. `nvkmd_horizon` keeps its own per-VA mapping list — bookkeeping in the new backend, not a `horizon_gpu` gap |

`NVKMD_VA_SPARSE` binds the whole reservation sparse at alloc time
(`_va.c:144-154`). libnx has it — `nvAddressSpaceAlloc(…, bool sparse, …)`
(`nvidia/address_space.h:14`) — and `horizon_gpu_vm_reserve` does not
expose the flag → **extension 4**, needed only if sparse binding stays
exposed.

`NVKMD_VA_ALLOC_FIXED | NVKMD_VA_REPLAY` is capture/replay; nouveau
splits its heap in two (`nvkmd_nouveau.h:40-44`). libnx has
`nvAddressSpaceAllocFixed` (`address_space.h:15`), unexposed →
**extension 5**, needed only if capture/replay stays exposed.

### Table 5 — `nvkmd_ctx_ops` (`nvkmd.h:321-351`) — where the semantics live

Every one of these is **batched**, and that is the interface's most
important property for this project:

| Op | Blocks? | Semantics |
|---|---|---|
| `wait(waits)` | **no** | records `vk_sync` waits for the *next* flush (`_ctx.c:114-133`). It does not wait |
| `exec(execs)` | **no** | appends pushes; flushes early only when the batch is full (`_ctx.c:191-195`) |
| `bind(binds)` | **no** | appends map/unmap ops, coalescing adjacent ones (`_ctx.c:399-414`) |
| `signal(signals)` | no CPU wait | adds signal syncs and **flushes** (`_ctx.c:235`) |
| `flush()` | no | the actual ioctl |
| `sync()` | **yes — the only one** | flush, then wait on the ctx's own syncobj (`_ctx.c:238-279`) |

And `sync()` is reached from `nvkmd_ctx_exec` **only** under
`NVK_DEBUG_PUSH_SYNC` (`nvkmd.c:313-317`). That is CLAUDE.md's rejected
design 6 already written into upstream NVK: submission is asynchronous,
and the synchronous mode is a documented debug flag. `horizon_gpu` has
the same distinction (`device.h:88-90`, `HORIZON_GPU_SYNC=1`). **The two
debug-synchronous modes should be wired to each other, not left as two
unrelated switches.**

`struct nvkmd_ctx_exec` (`nvkmd.h:297-306`) is `{addr, size_B, incomplete,
no_prefetch}` — a GPU VA and a byte length, i.e. `horizon_gpu_cmd_span`
`{gpu_va, num_dwords}` — except that `no_prefetch` is **per entry**,
while `horizon_gpu_submit`'s flags are per submit (`submit.h:38-44`).
→ **extension 2**. `incomplete` means "this push and the next must be in
the same submit ioctl", which a span array satisfies by construction.

**Two context flavours** (`_ctx.c:458-470`): exec (engines) and bind
(`NVKMD_ENGINE_BIND`). The bind ctx exists because nouveau's VM_BIND is
asynchronous and ordered against syncobjs; Horizon's map/unmap is a
synchronous ioctl, so a Horizon bind ctx is "wait the waits, do the maps,
signal" — legal, but it contains a CPU wait, and it is created **only**
when the queue family advertises `VK_QUEUE_SPARSE_BINDING_BIT`
(`nvk_queue.c:437`, `nvk_physical_device.c:1603`).

**Sparse is not hypothetical on this chip.** `sparseBinding` is
`info->cls_eng3d >= MAXWELL_B` (`nvk_physical_device.c:371`), `MAXWELL_B`
is `0xB197` (`clb197.h:32`), and GM20B's queried 3D class **is** `0xb197`
— measured on console, `t_init`, both process modes. So NVK will
advertise sparse binding on this GPU unless that condition gains a kmd
capability. Item 6 has to decide: implement the bind ctx, or add the
capability and turn the feature off.

### Table 6 — the data the backend must produce

| Datum | Where | Source on Horizon |
|---|---|---|
| `nv_device_info` | `nvkmd_pdev::dev_info` | milestone item 2; field by field below |
| `nvkmd_info` (6 flags) | `nvkmd_pdev::kmd_info` | ours to answer; 3 of the 6 have no consumer (above) |
| `bind_align_B` | `nvkmd_pdev:151` | nouveau uses `os_get_page_size()` (`_pdev.c:114-117`) = **0x1000 here**, already bounded from both sides on console (`t_sysinfo`) |
| `sync_types` | `nvkmd_pdev:158` → `pdev->vk.supported_sync_types` (`nvk_physical_device.c:1615`) | nouveau uses DRM syncobj. **The largest single piece of Phase 4** — see below |
| `va_start` / `va_end` | `nvkmd_dev:204` | read only by `nvk_edb_bview_cache`, itself behind `NVK_DEBUG_FORCE_EDB_BVIEW` (`nvk_physical_device.h:71-75`) — but still must be sane. `horizon_gpu_device_info::va_regions[2]` is the queried answer |

**`nv_device_info` field by field** (`nv_device_info.h`), against
`horizon_gpu_device_info` (`device.h:38-79`):

| Field | Comes from |
|---|---|
| `chipset` | `arch \| impl` = `0x120 \| 0xb` = **0x12b** — queried |
| `type` | `NV_DEVICE_TYPE_SOC` |
| `device_name`, `chipset_name` | `chipname` = `"gm20b"` — queried |
| `gpc_count`, `tpc_count` | `num_gpc`, `num_tpc_per_gpc` — queried (1 × 2 on console) |
| `cls_copy/eng2d/eng3d/m2mf/compute/gpfifo` | queried; all six already in `horizon_gpu_device_info` |
| `vram_size_B`, `bar_size_B` | 0 — SoC |
| `nc_atom_size_B` | `util_cache_granularity()` = `CTR_EL0.CWG` — **unmeasured** |
| `sm` | `sm_for_chipset(0x12b)` = **53** (`nouveau_device.c:84-86`) |
| `mp_per_tpc` | 1 (`nouveau_device.c:163-169`) |
| `max_warps_per_mp` | table on `sm` (`nouveau_device.c:118-160`) |
| `max_smem_per_wg_kB`, `sm_smem_sizes_kB[]` | `init_shared_mem_sizes()` (`nouveau_device.c:172-315`); for sm 53: 16/32/48 kB, cap 48 |
| `zcull_info`, `has_zcull_info` | libnx `nvioctlNvhostCtrlGpu_ZCullGetInfo` (`nvidia/ioctl.h:256`); unexposed by `horizon_gpu` → **extension 6**. nouveau allows this query to fail (`nouveau_device.c:484`) |

**The four derived fields are the problem, and it is a layering one.**
`sm_for_chipset`, `mp_per_tpc_for_chipset`, `max_warps_per_mp_for_sm` and
`init_shared_mem_sizes` are pure functions of the chipset number living
in `src/nouveau/winsys/nouveau_device.c` — a file that belongs to the
nouveau winsys and that a Horizon build does not compile. Either they
are duplicated into `nvkmd_horizon` (a copy, which this tree's rules
discourage and which would drift) or they move to a shared file next to
`nv_device_info.h`. They are properties of the **chip**, not of the
kernel driver, so moving them is a small mechanical patch with a real
argument behind it. **Decision for item 2, not taken here.**

### The sync problem, stated precisely

`nvkmd_ctx_wait` / `_signal` take `struct vk_sync_wait` / `vk_sync_signal`
— Vulkan runtime objects, not fences — and NVK takes its sync type from
the kmd (`nvk_physical_device.c:1615`). nouveau's is DRM syncobj
(`_pdev.c:129`), which is rejected design 3 *and* does not exist here.

What Horizon has, measured or declared:

| Primitive | Where | Exposed by `horizon_gpu`? |
|---|---|---|
| syncpoint read | `nvioctlNvhostCtrl_SyncptRead` (`ioctl.h:246`) | yes — `horizon_gpu_syncpt_read` |
| GPU-side increment on submit | in-stream `SYNCPOINTA/B` | yes — every `horizon_gpu_submit` |
| GPU-side wait | validated on console (R10) | yes — `horizon_cmds_syncpt_wait` |
| CPU wait with timeout | `nvFenceWait` / `EventWait` (`ioctl.h:250`) | yes — `horizon_gpu_fence_wait` |
| **CPU-side increment** | `nvioctlNvhostCtrl_SyncptIncr` (`ioctl.h:247`) | **no** → **extension 3** |
| channel-independent syncpoint allocation | `nvioctlChannel_GetSyncpt` (`ioctl.h:293`) | **no** — every syncpoint here belongs to a channel |

A `vk_sync_type` (`vk_sync.h:156-260`) needs `init`, `finish`, `signal`
(**from the CPU**), `reset`, `move`, `wait` with an absolute timeout,
and for a timeline `get_value`. CPU signal is precisely the primitive
`horizon_gpu` does not expose, and a semaphore that no channel owns is
precisely the object Horizon syncpoints are not.

Two routes exist and **this step does not choose between them**:

1. a Horizon-native `vk_sync` over syncpoints (needs extension 3, and an
   answer for who owns a syncpoint that no channel created);
2. the runtime's own emulation — `vk_sync_timeline.c` builds a timeline
   out of binary syncs — which is where the `cnd_timedwait` measurement
   (honours its 200 ms deadline; **emulator only**) and **D8**
   (`CLOCK_MONOTONIC` here is the real-time clock) already point.

Whichever is chosen, D8 has to be answered first: `vk_sync` waits take
**absolute** timeouts derived from `os_time_get_absolute_timeout`, and a
clock that steps when the date changes is not the clock a Vulkan timeout
wants. That makes D8 a Phase 4 blocker rather than a curiosity.

### Verdict: implementation *and* a bounded extension of `horizon/`

Six candidate extensions, none of them a redesign, each with the
interface site that demands it:

| # | Extension | Demanded by | Conditional? |
|---|---|---|---|
| 1 | GPU timestamp query | `nvkmd_dev_ops::get_gpu_timestamp` → `vkGetCalibratedTimestampsKHR` | **no** |
| 2 | per-span submit flags (`no_prefetch` per GPFIFO entry) | `struct nvkmd_ctx_exec::no_prefetch` | **no** |
| 3 | CPU-side syncpoint increment | any Horizon-native `vk_sync` with CPU signal | on the sync decision |
| 4 | sparse VA reservation | `NVKMD_VA_SPARSE` | on exposing sparse binding |
| 5 | fixed-address VA reservation | `NVKMD_VA_ALLOC_FIXED` | on exposing capture/replay |
| 6 | Zcull info query | `nv_device_info::zcull_info` | on `has_zcull_info` |

Everything else the interface asks for, `horizon_gpu` already answers —
memory objects, VA reservations, FIXED maps with an explicit PTE kind,
channels, asynchronous submission, fences, bounded waits, teardown with
leak accounting — and in two places the fit is better than expected:
`nvkmd`'s reservation-then-bind model is the one R8 forced on us, and
`nvkmd`'s only CPU stall is behind the same debug flag CLAUDE.md
requires.

### Decisions this step names and does not take

| # | Decision | Belongs to |
|---|---|---|
| D9 | pdev/dev split: one `horizon_gpu_device` for both, or GM20B facts queryable without a device | item 1 |
| D10 | the four chipset-derived `nv_device_info` fields: duplicate or move upstream | item 2 |
| D11 | `vk_sync`: Horizon-native over syncpoints, or the runtime's timeline emulation | item 8 |
| D12 | sparse binding: implement the bind ctx, or add a kmd capability and turn it off | item 6 |

### What step 1 did NOT do, said plainly

- **No code, no patch, no build.** `mesa/` was fetched and the series is
  not even applied in this container yet.
- The six unresolved libc symbols are untouched — that is step 2 — and
  Rust is untouched, which is step 3.
- Nothing about images, tiling or NIL was read beyond the ops table.
  That is Phase 5's surface, and reading it now would be guessing.
- `nc_atom_size_B`, and whether `dc cvac`/`CTR_EL0` are permitted at EL0
  under Horizon, are **unmeasured**. They are named above rather than
  assumed.

---

## Phase 4 — step 2: the six libc symbols, closed (2026-07-28)

**Class: cross build (X) and host (H).** Nothing here is a hardware or
emulator result. The headline is an artefact, not an argument: **an
executable that pulls NIR, SPIR-V, `nir_print`, `mesa_log`,
`util_sparse_array`, `ralloc` and `util_get_cpu_caps` out of Mesa's
built core now links, with zero undefined symbols.** Before this session
the same link was impossible.

Two new patches — **0013** and **0014**, the series is now fourteen —
and **nothing went into `compat/`**. The reason for that is given below;
it is not an oversight.

### The disposition, symbol by symbol

| Symbol | Referenced by | Disposition |
|---|---|---|
| `posix_memalign` | `sparse_array.c.o` | **patch 0013** — the configure check answers YES and is wrong; asked properly it answers NO and Mesa's own fallback takes over |
| `geteuid`, `getgid`, `getegid` | `log.c.o`, `perf_u_trace.c.o` | **patch 0014** — `__normal_user()` gated on the C library trait instead of on `_WIN32` |
| `getuid` | `log.c.o`, `perf_u_trace.c.o` (patch 0014) — and `anon_file.c.o` | patch 0014 for the first two; the third is **unreachable**, measured below |
| `flock` | `mesa_cache_db.c.o` | **unreachable**, measured below — no patch, no `compat/` |

### Why none of them went to `compat/`

`CLAUDE.md` opens that door for a function newlib genuinely lacks **and
Mesa genuinely needs**. Neither half holds here:

- `posix_memalign` — `util/os_memory_aligned.h:55-96` already carries a
  complete `os_malloc_aligned()` for platforms without it. Mesa does not
  need the function; it needed a truthful answer about it.
- the uid/gid family — the question `__normal_user()` asks has no
  meaning on a system with no users and no set-user-ID bit. A
  `compat/getuid` returning 0 would be **inventing an answer to a
  question the platform does not have**, which is the opposite of what
  `compat/sysconf.c` does: there the answer existed (`InfoType_CoreMask`,
  `InfoType_TotalMemorySize`) and was measurable.
- `flock` — dead code, and the link proves it.

### `posix_memalign`: the check answers YES, and here is the mechanism

`mesa/meson.build:1643-1651` asks `cc.has_function('posix_memalign')`
with **no prefix**, and carries a comment naming exactly this failure for
MinGW while working around it with `host_machine.system() != 'windows'`.

Meson 1.11.2 tries a real link first, and it fails
(`build/mesa-probe/meson-logs/meson-log.txt`):

```
testfile.c:17: undefined reference to `posix_memalign'
-> 1
```

then falls back to a builtin test whose guard is **switched off when the
call passes no prefix** — `no_includes = '#include' not in prefix`
(`mesonbuild/compilers/mixins/clike.py:806`), which makes the
`#error` line read `#if !1 && …`:

```
#if !1 && !defined(posix_memalign) && !0
    #error "No definition for __builtin_posix_memalign found in the prefix"
#endif
-> 0
Checking for function "posix_memalign" : YES
```

Reproduced by hand from that template, same flags, both ways:

```
no prefix       (what Mesa asks today)   -> YES
prefix <stdlib.h> (what it should ask)   -> NO
```

and newlib is honest about it: `stdlib.h:290` declares
`posix_memalign` and nothing defines it. A direct link probe:

```
posix_memalign   FAILS: undefined reference to `posix_memalign'
aligned_alloc    LINKS
memalign         LINKS
```

so the C library is not short of aligned allocation, only of that
spelling. **Patch 0013 passes the declaring header**, which turns
Meson's own guard back on. The OS exclusion then has nothing left to do
and goes away — the patch removes a platform test rather than adding
one, and fixes MinGW by the mechanism instead of by the name.

After it: `Checking for function "posix_memalign" : NO`, and
`HAVE_POSIX_MEMALIGN` occurrences in `build/mesa-probe/build.ninja`
drop from 352 to **0**.

### The uid/gid family: a question this platform does not have

All four are reached through one inline function
(`util/u_debug.h:401-410`):

```c
#if defined(_WIN32)
   return true;
#else
   return geteuid() == getuid() && getegid() == getgid();
#endif
```

`__normal_user()` asks whether the process gained privileges its invoker
did not have, and thirteen call sites across Mesa consult it before
trusting a filename from the environment. A C library with no
set-user-ID concept cannot have raised anything, so `true` is correct
for that whole class — and it is already the answer Windows gets.

**Patch 0014** tests `HAVE_GETEUID` instead of the OS. Windows keeps
`true` because it has no `geteuid`; every POSIX platform keeps the
comparison because it has one. Measured: all four fail to link, and none
of the four is a compiler builtin, so the check answers NO honestly with
or without a prefix (all four `-> NO` from Meson's own template).

`log.c.o` is not optional — `nir_print.c.o` references it, and every
Vulkan driver logs — so this one really would have stopped the link.

### The two that are unreachable, measured rather than hoped

An archive member only joins a link when something references one of its
symbols. Both remaining symbols live in members nothing references:

```
=== anon_file.c.o defines:
    os_create_anonymous_file
    referenced from elsewhere in the core by:
        (nothing)

=== mesa_cache_db.c.o
    referenced from elsewhere in the core by:
        mesa_cache_db_* <- mesa_cache_db_multipart.c.o   (only)

mesa_cache_db_multipart.c.o is referenced by:
        (nothing)

disk_cache.c.o:    0 defined symbols, 1424 bytes
disk_cache_os.c.o: 0 defined symbols, 2936 bytes
```

The last two lines are why: `-Dshader-cache=disabled` empties those
translation units, so the chain that would reach `flock` is cut at its
root. And `os_create_anonymous_file` is called nowhere in
`src/nouveau/`, `src/vulkan/runtime/` or `src/compiler/` — checked by
grep, because those are the trees a Horizon link pulls from.

### The link that proves it

Reference-graph analysis is not a link, so the claim was tested by
linking. `build/probe/libc-symbols/link_probe.c` calls into the parts of
the core NVK reaches — `mesa_logi`, `util_sparse_array_*`, `ralloc`,
`nir_shader_create`, `nir_print_shader`, `_mesa_blake3_init`,
`util_get_cpu_caps()`, and takes the address of `spirv_to_nir` — and is
linked against all ten archives with `--start-group`, `libhorizon_compat`
and `-lnx`:

```
=== pulled into the link:
    mesa_log                     yes
    mesa_log_v                   yes
    util_sparse_array_get        yes
    _mesa_hash_table_create      yes
    nir_print_shader             yes
    spirv_to_nir                 yes
    _util_cpu_caps_state         yes   (and _util_cpu_detect_once)
=== NOT pulled in:
    os_create_anonymous_file     absent
    mesa_cache_db_open           absent
    mesa_db_wipe_path            absent
=== unresolved in the executable:
    (0 undefined symbols)
=== the six from the audit, by name:
    posix_memalign   not referenced anywhere in the executable
    flock            not referenced anywhere in the executable
    getuid           not referenced anywhere in the executable
    geteuid          not referenced anywhere in the executable
    getgid           not referenced anywhere in the executable
    getegid          not referenced anywhere in the executable
```

15 703 488 bytes of ELF, zero undefined symbols. That is the first
executable this project has produced from Mesa's core, and it is the
answer to "these will stop the first executable link".

### The audit, before and after

Same script both times: every undefined reference in the ten archives,
resolved first against the core itself and then against all 158
toolchain archives (libc, libm, libsysbase, libpthread, libstdc++,
libnx, portlibs, libgcc, `libhorizon_compat`).

| | Before | After |
|---|---|---|
| core undefined refs | 1462 | 1458 |
| resolved inside the core | 1287 | 1287 |
| resolved by the toolchain | 169 | 169 |
| **UNRESOLVED** | **6** | **2** |

and the two that remain are the two the link above does not touch.

### Commands run and results

| Command | Class | Result |
|---|---|---|
| `scripts/fetch-mesa.sh` | H | `mesa-26.1.5`, HEAD verified `6a02618ccf6c…`, 503 MB |
| link probes, 8 functions | X | the six fail; `aligned_alloc`, `memalign` link |
| Meson `has_function` template by hand, `posix_memalign` | X | no prefix → YES; `<stdlib.h>` prefix → NO |
| the same for the uid/gid family | X | all four → NO both ways, and none links |
| unresolved-symbol audit | X | **6 → 2** |
| reachability of `anon_file.c.o` / `mesa_cache_db.c.o` | X | nothing in the core references either |
| **executable link probe** | X | **links, 0 undefined symbols** |
| `scripts/configure-mesa.sh` after the patches | X | `posix_memalign : NO`, `geteuid : NO`; `HAVE_POSIX_MEMALIGN` in `build.ninja`: **0** |
| `scripts/build-mesa.sh` | X | **359/359 edges**, 0 failures, 10/10 libraries |
| `git -C mesa reset --hard $MESA_COMMIT && scripts/apply-mesa-patches.sh` ×2 | H | applies **14**; second run `all 14 patches already applied` |
| `scripts/build-switch.sh all -j4` | X | **13 `.nro`** |
| `scripts/run-host-tests.sh` | H | **103/103** (6 suites) |
| Five gates | H | layering, abs-paths, rust-target, mesa-test-parity, tls-relocs — all OK |

### What step 2 did NOT do

- **`getuid` and `flock` are still unresolved in the archives.** They are
  unreachable *today*, with `-Dshader-cache=disabled` and with nothing
  calling `os_create_anonymous_file`. If a later phase enables the
  shader cache — which `scripts/configure-mesa.sh` records as a decision,
  not a workaround — `flock` comes back and needs an answer then.
- The `rand_xor` weak-seed finding from Phase 3 is untouched: still
  Mesa's own documented fallback, still not a `compat/getrandom`.
- No Rust was compiled. That is step 3, and it is next.
- Regenerating the series rewrote the twelve existing patch files. The
  diff was read before committing: only the `From <sha>` line and the
  `[PATCH n/12]` → `[PATCH n/14]` counter changed, in all twelve.

---

## Phase 4 — step 3: Rust, from the real failure to a linked artefact (2026-07-28)

**Class: cross build (X).** Nothing here ran on a console or an
emulator. The headline: **Rust has now been compiled and linked for
Horizon for the first time in this project** — a `no_std` + `alloc`
staticlib, linked by devkitA64 into a Horizon ELF with **zero undefined
symbols and zero TLS relocations**. Phase 2 could only say "no Rust has
been successfully compiled for Horizon"; that sentence is now out of
date.

### The real failure, reproduced first

As instructed, the failure was reproduced before anything was designed.
Configuring the pinned tree with the nouveau Vulkan driver, into a
separate build directory so the working one is untouched:

```
$ MESA_BUILD_DIR=build/probe/mesa-nouveau scripts/configure-mesa.sh \
      -Dvulkan-drivers=nouveau
…
mesa/meson.build:841:2: ERROR: Compiler rustc --target
  aarch64-nintendo-switch-freestanding -C linker=aarch64-none-elf-gcc
  cannot compile programs.
```

and the log says exactly why (`meson-logs/meson-log.txt:340-352`):

```
Sanity check compiler command line: rustc --target aarch64-nintendo-switch-freestanding \
  -C linker=aarch64-none-elf-gcc --emit link=sanity_check_for_rust_cross.exe sanity_check_for_rust.rs
error[E0463]: can't find crate for `std`
  = note: the `aarch64-nintendo-switch-freestanding` target may not be installed
  = help: consider building the standard library from source with `cargo build -Zbuild-std`
```

It is `add_languages('rust')` at `mesa/meson.build:841`, it is
unconditional once the driver is on, and the missing thing is a
prebuilt `std` for a tier-3 target. Same shape Phase 2 predicted.

### The measurement that decides the route: this target is not `unix`

`docs/rust-toolchain.md` answered R13 with "as Mesa links them today,
`std` is required" **and** with the conclusion that the dependency is
seven substitutions deep, so the supported route is `no_std` + `alloc`
and Phase 2 should build no sysroot. This step adds the measurement that
makes that conclusion load-bearing rather than merely preferable:

```
$ rustc --print cfg --target aarch64-nintendo-switch-freestanding
target_os="horizon"      target_env=""      target_vendor="nintendo"
  (no target_family, no unix)

$ rustc --print cfg --target armv6k-nintendo-3ds
target_os="horizon"      target_env="newlib"      target_family="unix"      unix
```

Rust's standard library **does** carry `target_os = "horizon"` support —
and every one of those sites is under `sys/pal/unix/`, `sys/fs/unix.rs`,
`sys/alloc/unix.rs`: it is the **3DS**, which reaches newlib through the
unix PAL. The Switch target has no `target_family`, so a `std` built for
it would select `sys/pal/unsupported`, where the operations that make
`std` worth having return errors. Building it would be expensive and
would buy a `std` that cannot spawn a thread or open a file.

So: **`no_std` + `alloc` is the route**, which is what
`docs/rust-toolchain.md` § 2 concluded from the source. The target
metadata agrees with itself here — `"std": false` in the spec.

### The container has no network, and `-Zbuild-std` wants some

```
$ cargo build --offline -Z build-std=core,alloc \
      --target aarch64-nintendo-switch-freestanding
error: no matching package named `cfg-if` found
location searched: crates.io index
required by package `std v0.0.0 (…/library/std)`
```

`-Zbuild-std` **resolves** the standard library's whole workspace even
when it only builds `core` and `alloc`, and that workspace depends on
crates.io packages. Containers here have no network, which is the
condition `CLAUDE.md` states, so the fetch belongs on the host —
the same rule `fetch-mesa.sh` and `horizon_ensure_python_deps` follow.

**`scripts/fetch-rust-crates.sh` is new** and does exactly that: reads
`library/Cargo.lock` out of the pinned image's `rust-src`, downloads the
**30** registry packages it names, verifies each against the checksum
Rust itself recorded, extracts them into a cargo `directory` source with
a `.cargo-checksum.json` per crate, and writes the source-replacement
config under `build/` (never tracked — it names an absolute path, which
`check-no-abs-paths.sh` forbids in a tracked file).

Nothing in it chooses a version. The pin is Rust's own, read per run, so
it cannot drift from the toolchain — the policy `versions.env` states
for libnx and rustc.

**It was broken on purpose four ways before being trusted**, because a
gate that has never failed has not been tested:

| Breakage | Result |
|---|---|
| append a line to a vendored source file | `cfg-if-1.0.4 does not match its checksums; re-extracting` |
| delete a vendored file | `libc-0.2.189 does not match its checksums; re-extracting` |
| add a file the `.crate` never had | `memchr-2.7.6 does not match its checksums; re-extracting` |
| corrupt a downloaded `.crate` | `shlex-1.3.0.crate does not match the lockfile checksum; re-downloading` |
| clean re-run | `vendored 0 crate(s) (0 re-extracted…)` — a no-op |

The first of those **found a real defect in the first version of the
script**: it compared only the recorded package hash, so a tampered
source file left the stamp matching and the run reported the tree
current. Cargo verifies per-file hashes for crates it actually compiles,
but `-Zbuild-std` pulls most of this vendor tree in for *resolution*
only, so a tampered file in an unused crate would never have been looked
at. The check now hashes every file and also rejects extra ones.

### Rust compiled for Horizon

With the vendor tree in place, offline, inside the container:

```
$ cargo build --offline -Z build-std=core,alloc \
      --target aarch64-nintendo-switch-freestanding
   Compiling compiler_builtins v0.1.160
   Compiling core v0.0.0
   Compiling alloc v0.0.0
   Compiling hello v0.0.0
    Finished `dev` profile in 21.06s
```

The probe crate is shaped like Mesa's: `#![no_std]`, `extern crate
alloc`, `crate-type = ["staticlib"]`, a `#[global_allocator]` over
newlib's `memalign`/`free` — the "smaller open sub-risk"
`docs/rust-toolchain.md` § 4 names — and a `#[panic_handler]` that
aborts, which `panic = "abort"` makes the only sensible one.

Then linked by devkitA64, with the same flags the tests use:

```
linked: 5478760 bytes
    (0 undefined symbols)
```

### The `-mtp=soft` sub-risk, measured for the first time

`docs/rust-toolchain.md` § 4 recorded it as "should not arise — but
'should not' is not 'measured'", to be checked when Rust is first built
and linked. It is now checked, on the artefact:

```
R_AARCH64_TLS* relocations in libhello.a: 0
__aarch64_read_tp references:             0
mrs tpidr_el0 in the linked executable:   0
```

No thread-local storage is generated, so rustc never had the chance to
emit the hardware thread-pointer read that `-mtp=soft` exists to avoid,
and the miscompile that cost this project two review rounds has no
foothold in the Rust half. **Caveat, stated plainly:** this is the probe
crate, not NAK and NIL. The check is the one to re-run on their archives,
and `scripts/check-tls-relocs.sh` already tests the property rather than
the flag — it will need to be pointed at the Rust output too.

### And the rlibs work as a sysroot, which is what Meson needs

Meson drives `rustc` directly and never calls cargo, so the question is
whether what cargo produced can be handed to a bare `rustc`:

```
sysroot holds: liballoc-….rlib libcompiler_builtins-….rlib libcore-….rlib
$ rustc --target … --sysroot <sysroot> --crate-type staticlib …
rustc with --sysroot: OK (12255694 bytes)
```

Three rlibs, installed at `lib/rustlib/<target>/lib/`, and a plain
`rustc --sysroot` finds them. That is the shape the cross file will have
to point at.

### What step 3 established, and what is still open

**Established:** the route is `no_std` + `alloc`; `std` for this target
would be the `unsupported` PAL; the crates.io barrier is solved and
gated; `core`, `alloc` and `compiler_builtins` build for the target
offline; a Rust staticlib links into a Horizon ELF with nothing
undefined; no TLS is generated; and the rlibs work as a sysroot for a
bare `rustc`.

**Still open, and each is a concrete next task:**

1. **NAK and NIL are still `std` crates.** The seven substitutions are
   listed in `docs/rust-toolchain.md` § 2 with their replacements; they
   become `mesa-patches/` entries. Until then Mesa's Rust does not build,
   whatever the sysroot holds.
2. **A script that installs the sysroot**, so the three rlibs are
   produced and placed reproducibly rather than by the commands above.
3. **The cross file has to pass `--sysroot`** to `rust`, and
   `scripts/check-rust-target.sh`'s drift snapshot should grow the
   sysroot's identity — a rebuilt nightly changes the rlib hashes.
4. **Exactly one `#[global_allocator]` and one `#[panic_handler]` may
   exist** across the whole crate graph. In Mesa that is two crates
   (NAK and NIL) linked into one binary, so where those items live is a
   design decision, not a detail. Not taken here.
5. `-Zbuild-std` needed `compiler_builtins` too, which cargo added on
   its own. Whether the C library's `memcpy` and friends collide with
   it at link time was **not** measured — the probe linked, but the
   probe is small.

### Commands run and results

| Command | Class | Result |
|---|---|---|
| `MESA_BUILD_DIR=build/probe/mesa-nouveau scripts/configure-mesa.sh -Dvulkan-drivers=nouveau` | X | **fails at `meson.build:841`**, `error[E0463]: can't find crate for 'std'` |
| `rustc --print cfg` for both Horizon targets | X | Switch has no `target_family`; 3DS is `unix`/`newlib` |
| `cargo build -Z build-std --offline`, before vendoring | X | `no matching package named 'cfg-if'` |
| `scripts/fetch-rust-crates.sh` | H | 30 packages, all checksums match the lockfile |
| the same, four deliberate breakages | H | 4/4 detected and repaired; clean re-run is a no-op |
| `cargo build --offline -Z build-std=core,alloc` | X | **core, alloc, compiler_builtins and the crate build** |
| devkitA64 link of the staticlib | X | **5 478 760 bytes, 0 undefined symbols** |
| TLS check on the archive and the executable | X | **0 TLS relocations, 0 `mrs tpidr_el0`** |
| bare `rustc --sysroot` against the three rlibs | X | **OK** |
| `scripts/check-no-abs-paths.sh`, `check-layering.sh` | H | OK with the new script in `scripts/` |

### A correction to the brief this session started from

The session brief stated R13 as "**`std` hace falta**, no basta
`no_std` + `alloc`". `docs/rust-toolchain.md` says the first half and
not the second: `std` is required **as Mesa links NAK/NIL today**, and
the document's own conclusion is that the dependency is shallow — seven
sites, each with a listed replacement — so the sysroot is not needed and
the work is a small patch set. This step's measurement settles it in the
same direction for a different reason: a `std` for this target would be
the `unsupported` PAL, so building one is not the cheaper option, it is
the worse one.

---

## Phase 4 — step 4: the build machine Mesa's nouveau driver needs (2026-07-28)

Step 3 ended with Rust compiling and linking for Horizon. This step is
what stood between that and `meson setup -Dvulkan-drivers=nouveau`
answering anything at all. **It is now configured and building**, and
the Rust half is where it stops — for one measured reason, recorded at
the end.

Nothing here is a workaround. Every item is something the toolchain
genuinely does not have, obtained the same way every other input to
this project is obtained: **fetched on the host, where the network is;
built in the container, where the result has to run.**

### The shape of the problem, and the owner's suggestion

Each missing piece was first solved by mounting it into the tree and
pointing an environment variable at it. Asked mid-way why not derive a
Docker image from the base one instead, the honest answer was: because
`docker build` runs its `RUN` steps in a container, and dockerd here
runs `--bridge=none`, so a build that installs anything cannot reach
the network. Measured:

```
$ docker build -t probe build/probe/dockerbuild
ERROR: process "/bin/sh -c echo offline-build-ok" did not complete
       successfully: network bridge not found

$ docker build --network=none -t probe build/probe/dockerbuild
sha256:715d551c12912dafea51f5cd7da0c4ebd5bb5efd3017887943449a6b9ac10eec
```

`--network=none` works. So the suggestion was right and the work was
redone as `toolchain/Dockerfile` +
`scripts/build-toolchain-image.sh`: the material is still fetched on
the host, but it is *installed into an image* rather than mounted and
pointed at. That removed three environment variables from
`horizon_run`, put libclang on the system library path where clang-sys
finds it with no configuration, and made `build/` disposable again.

The line the image draws is stated in the Dockerfile: **it carries what
the container cannot otherwise obtain or run; everything that works
identically in both toolchain modes stays in the tree.** Meson and
Mesa's Python generators therefore stay where they were.

### What the image adds, and why each one could not just be installed

| Addition | Why not `apt`/`cargo install` |
|---|---|
| **libclang 16** (Debian bookworm) | bindgen dlopens it. It has to be a bookworm build: the image is glibc 2.36 and this host is Ubuntu 24.04 / glibc 2.39, so the host's own libclang-18 cannot load there. Two packages, because `libclang1-16` needs `libLLVM-16.so.1` — `readelf -d` confirms the NEEDED entry |
| **bindgen 0.72.1** | Mesa requires `>= 0.71.1` and refuses 0.72.0 as known-buggy (`mesa/meson.build:850-865`). Not in the image; a host `cargo install` yields an Ubuntu binary the container cannot execute; a container `cargo install` needs the network |
| **cbindgen 0.29.4** | Same, for NIL's generated `nil.h`. Floor is `>= 0.28` (`mesa/meson.build:909`) |
| **core, alloc, compiler_builtins** for `aarch64-nintendo-switch-freestanding` | Tier 3, so rustup ships no prebuilt `core`, and **Meson drives `rustc` directly and never calls cargo** — there is no `-Zbuild-std` in a Meson build. Built once with cargo and installed at `/opt/rust-sysroot` in the layout `rustc --sysroot` looks for |
| **LLVM 15, clang 15, libclc 15, SPIRV-LLVM-Translator 15** (Debian bookworm) | NVK compiles two OpenCL C files into SPIR-V at build time, so `mesa_clc` has to exist. **LLVM 15, not the 16 already there for libclang**: Mesa demands an SPIRV-LLVM-Translator matching the chosen LLVM's major.minor (`mesa/meson.build:2030-2042`) and bookworm packages `libllvmspirvlib` for 14 and 15 only |
| **SPIRV-Tools v2024.4**, built from source | The one piece the distribution cannot supply: Mesa requires `>= 2024.1` (`mesa/meson.build:2054`) and bookworm ships 2023.1 |

The .deb dependency closure is **resolved**, not listed:
`scripts/fetch-clc-deps.sh` reads Debian's own `Packages` index, asks
the base image what it already has, and takes the difference — 42
packages, each verified against the SHA256 the index records, written
to `build/toolchain/clc-deps/closure.txt` so a change shows up as a
diff.

That resolver had two defects, both found by the install failing rather
than by reading it:

- **Versioned dependencies were ignored.** `libc6-i386 depends on
  libc6 (= 2.36-9+deb12u14)` is not satisfied by the `2.36-9+deb12u10`
  the image carries, and treating "libc6 is installed" as an answer
  produced a closure `dpkg` then refused, leaving five packages
  unconfigured. Fixed by recording each installed package's version and
  checking every constraint with **`dpkg --compare-versions`** rather
  than a reimplementation of Debian version ordering.
- **Architecture qualifiers were not stripped.** `python3:any` looked
  unsatisfiable for a package the image has always had.

### The cross file gained three things, all toolchain description

1. **`kernel = 'none'`** in `[host_machine]`. Meson reads `kernel` in
   exactly one place — its Rust compiler. With anything else, the
   sanity check compiles `fn main() {}`, which needs libstd, and
   configure stops at `E0463`. With `'none'` it compiles a
   `#![no_std] #![no_main]` program instead, which is the shape this
   target supports. It says "freestanding Rust runtime", not "Horizon
   has no kernel" — that is the only question Meson asks it, and it is
   the same word rustc's own target name uses. Mesa never reads
   `host_machine.kernel()`; verified by grep.
2. **`--sysroot`** on the `rust` binary line, from a generated
   constant, for the reason in the table above.
3. **`bindgen_clang_arguments`**. bindgen parses Mesa's headers with
   libclang, not with the cross gcc, and given only
   `--target=aarch64-nintendo-switch-freestanding` it went looking for
   the C library in the *build machine's* `/usr/include`:

   ```
   /usr/include/stdio.h:27:10: fatal error: 'bits/libc-header-start.h'
                              file not found
   ```

   `--sysroot` at devkitA64's newlib fixes it. Meson passes these to
   every `rust.bindgen()` call, so they belong in the cross file rather
   than in a Mesa patch: it is a description of the toolchain.

### mesa_clc: a second, native Mesa build

`scripts/build-mesa-clc.sh` configures the same pinned Mesa checkout
*natively* — no cross file, no devkitA64 — with
`-Dinstall-mesa-clc=true`, and builds exactly two targets. The cross
build then uses `-Dmesa-clc=system`, which is the documented cross
path and also the one that keeps `dep_llvm` from being resolved as a
*host machine* dependency for aarch64-horizon, where it does not exist
and is not wanted.

Two packages were added to the closure only because this build asked
for them, each found by a compile failure rather than by prediction:
`zlib1g-dev`/`libzstd-dev`/`libexpat1-dev` (Meson fell through to
downloading the zlib wrap, which cannot work offline) and
`libclang-15-dev` (`clc_helpers.cpp:45: fatal error:
clang/Config/config.h: No such file or directory`).

### Patch 0015 — a static driver where there is no dynamic loader

The series is now **fifteen**. An ICD is a shared object the Vulkan
loader `dlopen()`s plus a JSON manifest naming it; neither means
anything with no loader, and the shared library will not even link
against non-PIC static archives:

```
src/nouveau/vulkan/meson.build:160:20: ERROR: Can't link non-PIC static
library 'nvk' into shared library 'vulkan_nouveau'.
```

Where there is no `dlopen` — the same `with_dlopen` condition patch
0001 introduced — `libnvk` is declared as a dependency and the shared
library and both manifests are skipped. The application links the
driver and enters it through `vk_icdGetInstanceProcAddr`.

### Where it stands, measured

```
$ meson setup ... -Dvulkan-drivers=nouveau -Dmesa-clc=system   -> configures
$ meson compile -C build/probe/mesa-nouveau
   ... the C half builds; bindgen generates NAK/NIL/compiler bindings
   ... rustc then fails, 500+ errors, of which:
   141 error: cannot find attribute `derive` in this scope
   102 error[E0405]: cannot find trait `Default` in this scope
    35 error[E0425]: cannot find type `Option` in this scope
     2 error[E0463]: can't find crate for `std`
```

That shape is one fact, not five hundred: **with no `std` in the
sysroot there is no prelude**, so every prelude name is unresolved.
It is the `no_std` conversion, arriving as a compiler error instead of
as a plan.

### D13, decided by measurement: exactly one Rust staticlib

`docs/rust-toolchain.md` § 6 left open "where the single
`#[global_allocator]` and `#[panic_handler]` live". The question is now
answered, and it is not a preference. Two `no_std` + `alloc` Rust
staticlibs, each with its own allocator and panic handler, **cannot be
linked into one binary**:

```
ld: libb.a(...): multiple definition of `__rustc::__rust_alloc';
    liba.a(...): first defined here
    ... and __rust_dealloc, __rust_realloc, __rust_alloc_zeroed,
        rust_begin_unwind — five symbols
LINK FAILED
```

Measured with and without `-O`, so it is not a codegen-unit accident.
Upstream Mesa gets away with two Rust staticlibs because `std` supplies
the shim and the archive member holding it is simply not pulled the
second time; under `no_std` the definition is explicit and lands beside
the code that needs it.

So Mesa's arrangement — NAK and NIL both `rust_abi : 'c'` — cannot
survive here. **NAK and NIL become rlibs, and one new staticlib links
both and carries the single allocator and panic handler.** That is a
build-system patch, not source churn, and it is the shape the rest of
the `no_std` work is written against.

### The size of what is left, counted rather than estimated

| | files | needing `use alloc::…` |
|---|---|---|
| `src/compiler/rust` | 13 | 9 |
| `src/nouveau/compiler/nak` | 42 | 27 |
| `src/nouveau/nil` | 10 | 0 |
| `src/nouveau/rust` (bitview) | 1 | 0 |
| `src/nouveau/headers` | 1 | 1 |

and ~210 `std::` paths, of which the overwhelming majority are
`core::` under another name — `std::mem` (50), `std::cmp` (32),
`std::slice` (30), `std::ops` (22), `std::marker` (20). The genuinely
operating-system ones are the seven `docs/rust-toolchain.md` § 2 lists.

### Known rough edge, not a blocker

`rustfmt` is not installed in the base image and is a rustup component,
so it cannot be added offline. bindgen reports
`Failed to run rustfmt: Internal rustfmt error (non-fatal, continuing)`
and emits each generated file as one very long line. The bindings
compile; the only cost is that a rustc diagnostic in a generated file
quotes a hundred kilobytes of it.


---

## Phase 4 — step 5: Mesa's Rust half, built without `std` (2026-07-28)

**`libnouveau_rust_runtime.a` — 15 292 744 bytes — is NAK, NIL, the
shared `compiler` crate, `nvidia_headers`, `bitview`, `nak_latencies`
and both bindgen crates, compiled `#![no_std]` for
`aarch64-nintendo-switch-freestanding`.** Cross build (X). Reproduced
from a reset `mesa/` and a re-applied series, byte-identical.

### What it actually took, against what was predicted

`docs/rust-toolchain.md` § 2 listed seven `std` sites and called the
work "a small, mechanical, plausibly upstreamable patch set". The seven
were right and all seven are gone. What the document did not count was
the **prelude**: without `std` there is no `Vec`, `Box`, `String`,
`vec!` or `format!` in scope anywhere, which is why the first build
produced five hundred errors that were one fact.

Measured before starting, so the size was known rather than discovered:
37 files needing `use alloc::…`, ~210 `std::` paths of which the
overwhelming majority are `core::` under another name.

| The seven | Replaced by |
|---|---|
| `std::env::var("NAK_DEBUG")` | `os_get_option()`, Mesa's own accessor for exactly this. It was **not** already reachable through the bindings, as § 2 assumed — it had to be allowlisted |
| `std::sync::OnceLock<Debug>` | an `AtomicU32` with `u32::MAX` reserved for "not read yet". `Debug` is seven flag bits, so a race recomputes the same answer and `Relaxed` is enough |
| `std::panic::catch_unwind` ×2 | called directly. It cannot catch anything under `panic = "abort"`, which this target's spec sets |
| `std::collections::HashMap` | hashbrown — the implementation `std` itself uses — with rustc-hash's `FxBuildHasher` |
| `save_graphviz`'s `std::fs`/`std::io` | `#[cfg(feature = "std")]`. Dead code, three commented-out call sites, the only filesystem use in NAK |
| `std::io::Result` over `u_memstream` | a local error type, `src/compiler/rust/io.rs`. No Rust code there reads or writes anything |

### Four things the list did not have

1. **`eprintln!`, 21 sites.** All behind a NAK_DEBUG flag. What `std`'s
   macro ends up doing is `write(2)` to fd 2, which newlib has, so the
   macros are reproduced rather than the call sites rewritten.
2. **`f32::round`, `f32::powf`, `f32::log2`.** These live on `std`'s
   float types, not in `core` — core has no libm. newlib supplies
   `roundf`, `powf` and `log2f` and the driver already links `-lm`, so
   they are called directly rather than adding a Rust libm for three
   calls.
3. **rustc-hash defines `FxHashMap`/`FxHashSet` only under its `std`
   feature**, because there they are `std::collections::HashMap` with a
   different hasher. Mesa's wrap turned that feature on. It is now off,
   and the aliases are rebuilt over hashbrown — whose packagefile
   gained a host-machine build, since Mesa only had a `native : true`
   one for indexmap.
4. **bindgen emits `::std::os::raw::c_int`** unless asked for
   `--use-core`. Measured on the first successful bindgen run, not
   predicted.

### NAK's bindings no longer drag in the kernel winsys

`nak_bindings.h` included `nouveau_bo.h`, `nouveau_context.h`,
`nouveau_device.h`, `xf86drm.h` and `drm-uapi/nouveau_drm.h` — for one
file, `nak/hw_runner.rs`, which is behind `#[cfg(test)]`. That made NAK
unbuildable wherever the winsys is not, over symbols nothing that would
be built uses:

```
nouveau_bo.h:15:10: fatal error: 'sys/mman.h' file not found
```

The include and the allowlist entries it feeds are now asked for only
`with_tests`. This is the first piece of the nouveau winsys to come
out, and it came out for its own reason rather than as part of
`nvkmd_horizon`.

### D13 in effect: `src/nouveau/rust_runtime`

NAK and NIL are rlibs where there is no `std`, and one new staticlib
links both and carries the single `#[global_allocator]` (over the C
library's `memalign`/`free`, so there is one heap rather than two) and
`#[panic_handler]` (abort, which is what the target's panic strategy
already means). The condition is `host_machine.kernel()`, which is the
same property Meson's own Rust compiler reads to decide whether to
sanity-check a `fn main()` or a `#![no_std] #![no_main]` program — so
Mesa and Meson agree about the machine instead of each deciding
separately.

### Verified on the artefact, not argued

```
R_AARCH64_TLS* relocations in libnouveau_rust_runtime.a : 0
__aarch64_read_tp references                            : 0
__rust_alloc                                            : 1 definition
__rust_alloc_zeroed                                     : 1
__rust_alloc_error_handler                              : 1
rust_begin_unwind                                       : 1
archive members                                         : 57
```

The first two close `docs/rust-toolchain.md` § 4's open sub-risk **on
NAK and NIL**, which step 3 could only close on a probe crate. The
next four are D13 holding.

### Patches 0016 and 0017; the series is seventeen

0016 gates `vk_instance.c`'s `dlfcn.h` include on the RenderDoc
integration that is its only user — a header a loaderless C library
does not have, included for code that was already compiled out.

0017 is the `no_std` conversion, and it is large: 74 files. It is one
patch because the pieces do not stand up separately — the crates cannot
compile until the prelude, the collections, the floats and the crate
layout are all answered at once.

### Still open, and now the next thing

The **nouveau winsys** is what the build stops on now:

```
FAILED: src/nouveau/winsys/libnouveau_ws.a.p/nouveau_bo.c.o
nouveau_bo.c: fatal error: sys/mman.h: No such file or directory
```

That is not a gap to patch. `nouveau_ws` is libdrm talking to the
nouveau kernel driver, and replacing it is the whole point of
`nvkmd_horizon`. NVK still lists `dep_libdrm` and `idep_nouveau_ws` in
`nvk_deps`, and both come out when the Horizon backend goes in.

`compiler_builtins` versus newlib's `memcpy` family is **measured and
closed**: the linked `t_vulkan.elf` has exactly one global definition of
each of `memcpy`, `memmove`, `memset` and `memcmp`, and zero undefined
symbols. They do not collide.


---

## Phase 4 — items 1 and 2: `nvkmd_horizon` exists and NVK builds (2026-07-28)

**`libnvk.a` — 94 162 bytes — is the nouveau Vulkan driver compiled for
Horizon with a kernel-mode-driver backend that talks to the `nv` system
services.** `vk_icdGetInstanceProcAddr` and `nvk_CreateInstance` are
defined in it; the three backend objects are in the archive; 0 TLS
relocations in it and in the Rust staticlib beside it. Cross build (X).
`scripts/build-mesa-nvk.sh` does the whole thing from a clean tree in
4 m 17 s.

### The seven rejected designs, checked one by one

| # | | In this backend |
|---|---|---|
| 1 | no simulated `/dev/dri`, render node or sentinel fd | `get_drm_fd` and `get_drm_primary_fd` return −1, which is what nvkmd defines as "none". Nothing is opened |
| 2 | no fake libc wrappers, no `--wrap` | none; the backend calls `horizon_gpu` and nothing else |
| 3 | no re-implemented nouveau uAPI | no `drm_nouveau_*`, no `drmSyncobj*`. The winsys is not built at all on Horizon |
| 4 | no synthetic GEM handles | memory is `NvMap`-backed through `horizon_gpu_mem`; nothing invents a handle |
| 5 | no globals | every entry point takes its `nvkmd_pdev`/`nvkmd_dev`; the shared `horizon_gpu_device` hangs off the pdev |
| 6 | no CPU wait after submit | nothing submits yet (item 7); the only `wait` in the ops table is the one `nvkmd` defines |
| 7 | no Mesa files copied here | the backend is `mesa-patches/0018`, applied to the pinned tree |

### D9, decided by the hardware

Horizon's `nv` session is per process, and `horizon_gpu_device_create()`
is what makes the GM20B characteristics queryable at all — **there is no
way to describe the device without opening it**, which is exactly why
the nouveau backend opens the render node a second time
(`nvkmd_nouveau_dev.c:40`).

So **the pdev owns the `horizon_gpu_device` and every `nvkmd_dev`
created from it shares that one, under a reference count.** Vulkan
permits several `VkDevice`s from one `VkPhysicalDevice`; they share the
GPU address space, which is harmless because every VA in it is handed
out by `horizon_gpu_vm_reserve` and no two reservations overlap.

### D10, decided by where the knowledge belongs

Shader model, warps per MP, MPs per TPC and the legal shared-memory
splits are **pure functions of the chipset** and they lived inside
`src/nouveau/winsys/nouveau_device.c` — libdrm talking to the nouveau
kernel driver. A second backend could not reach them and would have had
to carry a copy that drifts.

They moved to `src/nouveau/headers/nv_device_info_chipset.c`, next to
the struct they fill. **Nothing about them changed**; `nouveau_device.c`
calls them instead of defining them. That is the "move upstream" arm of
D10, not the "duplicate" one.

### Enumeration without DRM, using a hook that already exists

`vk_instance::physical_devices` offers `try_create_for_drm`, which walks
`drmGetDevices2`, and `enumerate`, which hands the whole job to the
driver. Horizon uses the second. **Nothing fabricates a `drmDevice`, a
render node or a file descriptor** — rejected designs 1 and 3 stay
closed without inventing anything, which is what step 1 predicted.

`nvk_create_physical_device` is split out of
`nvk_create_drm_physical_device` so both paths share everything that is
not about *how* the device was found.

### Four things gave way, none of them Horizon-specific

- **`vk_image::drm_format_mod`** was declared under
  `#if DETECT_OS_LINUX || DETECT_OS_BSD`, so every driver reading it had
  to know which operating system it was on. The field is now
  unconditional and stays `DRM_FORMAT_MOD_INVALID` where the extension
  is not advertised; the entry point implementing
  `VK_EXT_drm_format_modifier` stays guarded, because *that* is a real
  platform dependency.
- **`<sys/mman.h>`** in `nvk_descriptor_table.c` and
  `nvk_device_memory.c`, with nothing used from it.
- **`<sys/sysmacros.h>`** in `nvk_physical_device.c`, for `major()`/
  `minor()` on a DRM device number.
- **The ELF build-id**, which `util/build_id.h` only declares where
  `dl_iterate_phdr` exists. It becomes `driverUUID` and the
  pipeline-cache UUID, so it cannot simply be dropped; it falls back to
  hashing `PACKAGE_VERSION` and `MESA_GIT_SHA1`. **This is coarser** —
  two builds of the same source with different local patches hash the
  same — and is recorded as a limitation rather than papered over with
  `__DATE__`, which would change the UUID on every rebuild of unchanged
  source.

### What the pdev reports, and why each answer is a fact

`nvkmd_info` is all false, and each false is a statement about Horizon
rather than a gap left for later: no dma-buf (there is no DRM to carry
one), no VRAM (GM20B is an SoC part), no tiled allocation (the PTE kind
is a property of the *mapping* here, applied at `horizon_gpu_vm_map`
time, not of the NvMap), no fixed or over-mapping (NvMap returns a CPU
pointer of its own choosing), no compression (the services report a
compression page size, but nothing has exercised compressed kinds on
this chip and claiming it before measuring would corrupt images rather
than fail).

`bind_align_B` is the address space's **queried** big-page size.
`sync_types` is NULL: that is D11 and milestone item 8, and a
build-visible gap is better than a silently wrong answer.

### Items 3 to 10 are named gaps, not silence

Every unimplemented op returns `VK_ERROR_FEATURE_NOT_PRESENT` with the
milestone item number in the message. `alloc_tiled_mem` and
`import_dma_buf` are left NULL on purpose: `nvkmd.h`'s inline wrappers
assert on the matching `nvkmd_info` flag before dispatching, so a caller
that ignored the capability faults at the assert rather than getting a
wrong allocation.

### Two new scripts

`scripts/configure-mesa-nvk.sh` and `scripts/build-mesa-nvk.sh`. They
use their own build directory: `scripts/configure-mesa.sh` is still the
Phase 3 build — Mesa's non-driver core, no drivers, no Rust — and the
two answer different questions, so they must not share configured state.


---

## Phase 4 — items 3, 4 and 5: memory, VA and binding (2026-07-28)

`libnvk.a` is 94 482 bytes with all five backend objects and 0 TLS
relocations. The driver can hold a buffer.

**Memory is NvMap-backed, and `map()` has nothing to do.** The storage
behind an NvMap object is ordinary process memory registered with the
service, so it has a CPU address for its whole lifetime and every map
hands back the same pointer; nvkmd core does the reference counting.
`sync_to_gpu`/`sync_from_gpu` are the cache maintenance homebrew heap
memory needs, and horizon_gpu decides whether it is required **from the
policy recorded at creation, not from the call site**. `log_handle` is
the in-process NvMap handle — the closest thing this platform has to the
GEM handle the field was named after, and a real identifier rather than
a number invented to fill it (rejected design 4).

**Six requests fail by name rather than being quietly downgraded:**

| Request | Why not |
|---|---|
| `NVKMD_MEM_VRAM` | GM20B has none, and nvk already knows from `type == NV_DEVICE_TYPE_SOC` |
| `NVKMD_MEM_SHARED` | no dma-buf; `nvkmd_info` says so |
| `NVKMD_MEM_MAP_FIXED` | NvMap chooses the CPU address. Answering at a different one is worse than failing |
| `NVKMD_VA_ALLOC_FIXED` | `NVGPU_AS_IOCTL_ALLOC_SPACE` lets the kernel choose the base; there is no "place it here" form. Extension 5 of the six step 1 enumerated |
| `NVKMD_VA_SPARSE` | decision **D12** — NVK derives `sparseBinding` from `cls_eng3d >= MAXWELL_B` and GM20B's queried class is exactly that |
| `NVKMD_VA_REPLAY` | capture/replay needs fixed addresses, same as the fourth |

**The address-space half is chosen, not assumed.** Horizon's GPU address
space has a small-page region and a big-page one, and a reservation
lives entirely in one — `horizon_gpu_vm_map` uses the reservation's page
size for every mapping inside it. The big-page half is taken when both
the size and the alignment are multiples of the big page, which is
exactly when every mapping inside can satisfy the alignment the map call
will demand.

**Unbinding needs bookkeeping the interface does not carry.** `nvkmd`
unbinds by `(offset, range)` and never hands back what `bind_mem`
returned, while `horizon_gpu_vm_unmap` takes the mapping. So the VA
keeps a list. An **exact** match is required: horizon_gpu maps and
unmaps whole mappings, and unbinding half of one would leave the address
space and that list disagreeing — guessing which half was meant is worse
than saying the request cannot be met.

**D5 is still open and is now visibly load-bearing.**
`HORIZON_GPU_MEM_CACHED` is the only policy horizon_gpu offers, which is
a statement about the platform: homebrew heap memory on Horizon is
CPU-cached. `NVKMD_MEM_COHERENT` therefore cannot be satisfied by the
allocation itself — it is satisfied by the flush/invalidate nvk already
makes around every access. Whether that is enough is exactly what D5
asks, and it is answered by the first GPU write, not by reading.


---

## Phase 4 — items 6 to 10, and the mandatory sequence (2026-07-28)

**`t_vulkan.nro` — 14 000 184 bytes — links with zero undefined symbols
and zero multiple definitions.** It is the fourteenth `.nro`, shaped
like the other thirteen, and it contains the whole driver: NVK, the Rust
half, NIR, SPIR-V, the Vulkan runtime, NIL, NAK and `horizon_gpu`.

### D11, resolved: native over syncpoints

A **binary** `vk_sync` backed by a channel fence — a syncpoint id and a
threshold — with the runtime's `vk_sync_timeline` emulation on top for
timeline semaphores. The emulation needs a binary type underneath
regardless; the only question was whether *that* should be a syncpoint
fence or a CPU-only event, and a syncpoint fence is what a submit
actually produces.

### D8, resolved where it actually bites

`vk_sync` waits take an **absolute** deadline built from
`os_time_get_nano()`, and `t_ostime` measured that clock to be the
real-time clock. The deadline is therefore converted to a **relative
duration exactly once**, at the moment the wait starts, and horizon_gpu
waits with libnx's tick-based sleep, which is monotonic in hardware. A
date change during the wait cannot move the deadline **because by then
there is no deadline left to move** — only a remaining duration. One
place, not everywhere a wait happens.

### Where a CPU stall happens, and where it does not

| | |
|---|---|
| `exec()` | never. The submit is asynchronous and stays that way |
| `sync()` | always, and that is what it is for: `vkQueueWaitIdle` and `vkDeviceWaitIdle` are made of it |
| `wait()` | on the CPU **for now**, and stated rather than hidden. A GPU-side wait needs a command-stream builder this backend does not have; it runs only when the application asked one submission to wait for another, never after every submit |
| `flush()` | nothing to flush — `horizon_gpu_submit` appends and kicks off in one call |

### The last unmeasured thing, measured

`docs/rust-toolchain.md` has asked since step 3 whether
`compiler_builtins` collides with newlib's `memcpy` family. **It does
not.** The full driver link reports **zero** multiple definitions. That
question needed a linked executable, and there now is one.

### Three more general fixes the first full link found

- `meson.build` asked whether POSIX regex exists **only on Windows**.
  Whether `regcomp` exists is a property of the C library, and newlib is
  another that lacks it. The question is now asked with
  `cc.has_function()` everywhere, and the `NO_REGEX` fallback
  `xmlconfig.c` already carries works for the same reason it works on
  Windows.
- `nvk_device.c` set `vk.copy_sync_payloads` unconditionally; copying a
  payload between syncobjs is a DRM operation on a DRM fd.
- `-Dxmlconfig=disabled`: driconf parses XML with expat and matches
  executable names with regex, and there is no per-application
  configuration file on a console anyway.

### The test itself

It enters through `vk_icdGetInstanceProcAddr` — the symbol a loader
would have called — and fetches every other entry point by name from
there, so it exercises the same dispatch the real thing would. It
**poisons the buffer with the complement of the pattern before
submitting**, so the check cannot pass on memory that already held the
right value. The only CPU stall is `vkWaitForFences`, with a one-second
bound: **no `vkQueueWaitIdle`, no `vkDeviceWaitIdle`**, which is the
phase's exit criterion.

### What Phase 4 still owes, and it is one thing

**A hardware run.** Everything above is a cross build (X). The exit
criterion is that the sequence runs on a real Switch and the CPU-side
validation passes, with the console log recorded here. Nothing in this
environment can produce that.


---

## Phase 4 — the hardware run, and what is ready for it (2026-07-28)

**This is the only thing Phase 4 still owes, and it cannot be done in
this environment: there is no console attached to it.** Everything
recorded above is a cross build (X). What follows is what has been done
instead — packaging it, and removing the first-run failures that could
be found by reading rather than by running.

### The package

`scripts/package-horizon.sh` wrote **fourteen `.nro`** to `build/pkg/`
with `MANIFEST.txt` beside them: each artefact's sha256, the resolved
toolchain image digest and the live package versions, so a result
measured on console stays attributable to a specific build.

`t_vulkan.nro` is 14 008 376 bytes.

### Two avoidable failures found by reading, and fixed

NVK advertises `KHR_timeline_semaphore` and `timelineSemaphore`
**unconditionally** (`nvk_physical_device.c:212,439`) and passes
`nvkmd->sync_types` straight through. The backend registered only the
binary syncpoint type, so `vkCreateSemaphore` would have failed to find
a type for every timeline semaphore an application asked for — a
failure that would have looked like a driver bug and was a missing list
entry. The runtime's `vk_sync_timeline` emulation over the binary type
is now registered beside it, which is what the nouveau backend does when
the kernel has no timeline either.

**A block-linear mapping in the small-page half.** A non-pitch PTE kind
is block-linear, and the GPU MMU only fills block-linear kinds in big
pages on Maxwell — such a mapping **faults on first access rather than
failing to map**. NVK asks for one whenever an image's layout is tiled
(`nvk_image.c:1175` passes `plane->nil.pte_kind` straight through) and
does not necessarily round the size to a big page, so choosing the half
by size and alignment alone put some tiled images in the wrong one. The
kind now decides.

That second one is the shape of bug that does not fail at the call that
causes it: the map succeeds and the fault arrives later, somewhere
else. It is exactly what a hardware run would have reported as an
unexplained channel fault.

Patches 0022 and 0023; the series is twenty-three.

### What the hardware run has to show

The exit criterion is not "it does not crash":

1. the sequence runs — `vkCreateInstance` through `vkWaitForFences`;
2. the **CPU-side validation of the written pattern passes** — the test
   poisons the buffer with `~0xa5c3f00d` before submitting, so it cannot
   pass on memory that already held the right value;
3. **no wait-idle was inserted to make it pass** — there is none in the
   test and none in `exec()`;
4. the console log is pasted into this file.

`t_vulkan` prints one line per check and a machine-checkable
`RESULT: PASS (n/n)` / `RESULT: FAIL (k/n)`, to stdout and to
`sdmc:/horizon_gpu_tests/t_vulkan.log`, like the other thirteen.

### Who runs it — decided with the owner (2026-07-28)

Asked directly how to tackle the one thing this environment cannot do,
the owner chose: **the owner runs it and returns the log.** That is the
same arrangement every hardware measurement in this project has used —
Phase 1's ten tests and Phase 3's two were all owner-executed — so it is
continuity rather than a new exception.

What that makes the handover:

```
build/pkg/            14 .nro + MANIFEST.txt (sha256 each, toolchain
                      image digest, live package versions)
t_vulkan.nro          14 008 376 bytes — the Phase 4 exit criterion
t_threads.nro         owed by Phase 3 on hardware (emulator only so far)
t_ostime.nro          same
```

Each prints one line per check and a machine-checkable
`RESULT: PASS (n/n)` / `RESULT: FAIL (k/n)`, to stdout and to
`sdmc:/horizon_gpu_tests/<name>.log`.

### Three failures found by reading, because running was not available

The only progress available without a console is to walk the paths the
first run will take. It found three, and none of them would have been
obvious from the crash:

1. **The timeline sync type was missing.** `vkCreateSemaphore` would
   have failed for every timeline semaphore.
2. **Block-linear mappings landed in the small-page half.** Maxwell's
   MMU only fills those kinds in big pages, so the map succeeds and the
   fault arrives later, somewhere else.
3. **A push whose size is not a multiple of four truncated the command
   stream mid-method.** The submit succeeds; the GPU reports it much
   later as a channel fault with no obvious cause.

All three are the same shape: they do not fail where they are caused.
Patches 0022, 0023 and 0024; the series is twenty-four.

### Said plainly

**The test has never been executed.** That it compiles and links says
only that nothing is missing, not that anything works. A first run that
fails is the expected outcome, not a surprise, and the log is what turns
either result into knowledge. Two Phase 3 measurements travel in the
same package and are owed alongside it: `t_threads` and `t_ostime` on
hardware rather than on the emulator.


---

## First hardware run of the Phase 4 sequence (2026-07-28)

Owner-executed on a real Switch. Three `.nro`, three logs, pasted in
full below the analysis.

### Phase 3's two remaining hardware measurements: both PASS

| Test | Result | What it closes |
|---|---|---|
| `t_threads` | **PASS (67/67)** | The last open Phase 3 item. It had only ever been run under an emulator, where it did not finish |
| `t_ostime` | **PASS (43/43)** | Same |

**Phase 3 owes nothing further.** Two facts worth carrying forward:

- `t_threads` ran the four-stage probe to the end — `getenv`,
  `os_get_option`, `os_get_option_cached` (hash table, ralloc,
  `simple_mtx`, `atexit`) and `util_get_cpu_caps` — all four returning
  cleanly, with `util_cpu_caps: nr_cpus=4 max_cpus=4`. That is the
  path the `-mtp=soft -fPIC` TLS miscompile used to hang on, exercised
  on console and not hanging.
- `t_ostime` measured the clock at **52 ns** resolution, agreeing with
  the ARM system counter to 79 981 ns over a 100 ms sleep — 0.08 %.
  `timespec_get(TIME_MONOTONIC)` still returns wall-clock seconds
  (1785247609 s), which is **D8**, unchanged and still open.

### `t_vulkan`: FAIL (29/30), and the reason is not the backend

```
  ok   vkCreateInstance -> 0
  ...  27 entry points resolved through vk_icdGetInstanceProcAddr
  ok   vkEnumeratePhysicalDevices -> 0
  FAIL one physical device, got 0
RESULT: FAIL (29/30) [aborted early]
```

Everything up to enumeration worked on real hardware, first time:
`vkCreateInstance` succeeded, and all 27 entry points resolved through
`vk_icdGetInstanceProcAddr` — which is the no-loader arrangement
(patch 0015) working exactly as designed.

`vkEnumeratePhysicalDevices` then returned **VK_SUCCESS with a count of
zero**. Not an error — an empty list is a legal answer — and that is
the problem: nothing anywhere said why.

**The cause, found by reading rather than by guessing.**
`nvk_physical_device.c:91-106`:

```c
static bool
nvk_is_conformant(const struct nv_device_info *info)
{
   /* Tegra is not currently supported */
   if (info->type != NV_DEVICE_TYPE_DIS)
      return false;
   ...
}
```

and `nvk_physical_device.c:1442-1453`:

```c
if (!nvk_is_conformant(&nvkmd->dev_info) &&
    !debug_get_bool_option("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", false)) {
#ifdef NDEBUG
   result = VK_ERROR_INCOMPATIBLE_DRIVER;      /* silently */
#else
   result = vk_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER, ...);
#endif
```

`nvkmd_horizon_pdev.c:129` sets `info->type = NV_DEVICE_TYPE_SOC`,
which is what GM20B is. So NVK declines the device, and because the
build is `--buildtype=plain` (`-DNDEBUG`) it declines it with no
message. `nvk_horizon_enumerate_physical_devices` turned that into an
empty list, also silently.

**So the driver was never entered.** Nothing below `nvk_create_physical_device`
ran — not the VA heap, not a channel, not a submit. This run says
nothing at all about whether `nvkmd_horizon` works; it says the
application never got as far as asking.

### Two changes, and why each is where it is

1. **`t_vulkan` sets `NVK_I_WANT_A_BROKEN_VULKAN_DRIVER=1`** before
   `vkCreateInstance`. NVK supplies this flag for exactly this case and
   this is what it is for.

   **Not** a patch to `nvk_is_conformant()`, deliberately. That check
   is telling the truth: NVK is not conformant on this chip, nobody has
   run the CTS on it, and a patch saying otherwise would be a claim
   this project cannot support. The application is the right place to
   say "I know, proceed" — and
   `vk_warn_non_conformant_implementation("NVK")` still fires.

2. **Patch 0025 makes the enumeration say why it found nothing.**
   Both paths that produce an empty list now log which one it was: the
   nv services could not be opened, or the GPU was found and NVK
   declined it. Swallowing that silently is what turned a one-line
   answer into a console round trip, and it was a defect in code
   written in this phase.

The series is **twenty-five**.

### Console logs, verbatim

```
== t_ostime ==
  note timespec_get(TIME_MONOTONIC) returned 2 (TIME_MONOTONIC=2), ts = 1785247609 s + 64292332 ns
  ok   the monotonic clock is available to timespec_get
  ok   the realtime clock is available to timespec_get
  note os_time_get_nano: first=1785247609067607332 last=1785247609072664956 over 41426 samples spanning 5 ms by the ARM counter, 41003 distinct values
  ok   os_time_get_nano never went backwards in 41426 samples (0 did)
  note smallest observed step: 52 ns
  ok   clock resolution is 1 ms or finer (52 ns)
  note over one 100 ms sleep: system counter 100233593 ns, os_time_get_nano 100153612 ns, difference 79981 ns
  ok   os_time_get_nano agrees with the ARM counter within 10% (79981 ns of 10023359 ns allowed)
  note os_time_sleep(50000 us) took 50341 us
  note os_time_sleep(0) took 1 us
  note os_time_nanosleep_until(+50000 us) took 50217 us
  ok   a deadline in the past returns at once (103 us, bound 10000 us)
  ok   an overflowing timeout saturates to OS_TIMEOUT_INFINITE
RESULT: PASS (43/43)
```

(elided: the 30 further `ok` lines; the full log is the artefact.)

```
== t_threads ==
  ok   call_once ran the body exactly once across 4 threads (1)
  note shared counter: 4 threads x 20000 increments, 80000 performed
  ok   counter is exactly 80000, no update lost (got 80000)
  note mtx_timedlock(200 ms) returned 1 after 200 ms
  ok   cnd_broadcast woke every waiter within 2000 ms (4 of 4)
  note cnd_timedwait(200 ms) returned 1 after 200 ms
  ok   cnd_timedwait ended on its own, without the 2000 ms watchdog having to signal it
  note tss destructor calls after 4 workers: 4
  note InfoType_CoreMask = 0xf, 4 core(s) allowed
  ok   sysconf(_SC_NPROCESSORS_ONLN) = 4 is the popcount of the mask it is computed from (4)
  note stage 1/4: getenv("GALLIUM_OVERRIDE_CPU_CAPS") — plain newlib -> (null)
  note stage 2/4: os_get_option -> (null)
  note stage 3/4: os_get_option_cached — hash table, ralloc, simple_mtx, atexit -> (null)
  note stage 4/4: util_get_cpu_caps
  note util_cpu_caps: nr_cpus=4 max_cpus=4 num_cpu_mask_bits=32
  ok   util_cpu_detect reports 4 CPUs, matching the core mask (4)
RESULT: PASS (67/67)
```

```
== t_vulkan ==
  ok   GetInstanceProcAddr(vkCreateInstance)
  ok   vkCreateInstance -> 0
  ... 26 further GetInstanceProcAddr lines, all ok ...
  ok   vkEnumeratePhysicalDevices -> 0
  FAIL one physical device, got 0
RESULT: FAIL (29/30) [aborted early]
```

### What is owed now

One more console run of `t_vulkan`. Everything else in Phase 4 is
built, linked and gated; the exit criterion is a hardware measurement
and it is the owner's to take.


---

## Audit before the second hardware run (2026-07-28)

The console can only answer one question per round trip, so the hour
after the first run went on reading the path the test takes rather than
on waiting. Three findings, one of them a defect in the test itself,
plus one open question that needs a decision.

### 1. The test never flushed its own poison — fixed

`t_vulkan` maps the buffer, writes `~FILL_PATTERN` over it, and
submits. The memory type it picks is index 0 on this device:
`DEVICE_LOCAL | HOST_VISIBLE | HOST_CACHED`, and **not**
`HOST_COHERENT`. So the poison sat in the CPU's cache as dirty lines
with no `vkFlushMappedMemoryRanges` behind it.

Vulkan requires the flush for visibility, but the reason it matters
here is worse: **a dirty line can be evicted after the GPU's fill has
landed, overwriting the GPU's result with the poison.** The test would
have reported that the GPU had not written, and the test would have
been the thing that was wrong — the most expensive kind of failure,
because the next hours go into the driver.

Fixed: the poison is flushed before the submit. Without it, it is not a
poison, it is a landmine.

### 2. A stack overflow in `nvkmd_horizon_ctx_exec` — fixed

The exec loop split its spans into runs whenever nvkmd's per-entry
`no_prefetch` flag changed, building them in a fixed 16-element array:

```c
if (n > 0 && !must_continue && (flag_changed || n == 16)) { flush; n = 0; }
spans[n] = ...;   /* n may be 16 */
n++;
```

`must_continue` — set when an entry ends mid-method and its
continuation must land in the same submit — suppresses the flush.
Seventeen consecutive `incomplete` entries therefore write past the end
of the array, and `n` is then 17, so the `n == 16` test never matches
again and it keeps going. Found by reading; no run has produced it, and
NVK is unlikely to emit that many in one exec — which is exactly what
would have made it a bad bug rather than an obvious one.

**And the splitting bought nothing.** `HORIZON_GPU_SUBMIT_DEFAULT` is
already `NOT_MAIN | NO_PREFETCH` (`submit.h:39`); the only other value
is the R3 experiment that failed on hardware. So there is no
prefetching mode to select, every submit already had the conservative
flags, and both code paths passed `SUBMIT_DEFAULT` regardless of the
run they had just computed.

Now one submit, one span array sized to `exec_count`.
`horizon_gpu_submit` takes any number of spans and reports `BUSY` on a
full ring rather than truncating, so there is nothing to cap.

**This retires extension 2 of the six that step 1 enumerated.**
Per-span submit flags are not needed, because only one flag combination
works. Four of the six remain conditional on D9–D12; the timestamp one
stands.

### 3. Our cache-maintenance ops are dead code here — not a defect

`nvkmd_mem_sync_to_gpu`/`sync_from_gpu` in this backend are never
called. `nvkmd.c:464` prefers Mesa's own implementation when
`util_has_cache_ops()` is true, and that returns **true** on aarch64,
where `cache_ops_aarch64.c` issues `dc cvac`/`dc civac` directly.

That works here: libnx's own `armDCacheFlush` documents that it reads
the cache line size **from `CTR_EL0`**, so both that register and EL0
cache maintenance are available on Horizon — `SCTLR_EL1.UCT` and `.UCI`
are set. The ops stay, because they are what a platform without Mesa's
architecture support would use, but they are not on this path and
nothing should be concluded about them from a passing run.

### 4. The `HOST_COHERENT` memory type is a promise this platform cannot keep

**This one is open and needs a decision.**

On an SoC, `nvk_physical_device.c:1571-1592` advertises two memory
types and says why:

```c
/* On Tegra, we only have sysmem so we claim it's DEVICE_LOCAL. The
 * only difference in memory types is between cached and uncached (but
 * coherent) maps. */
```

Type 0 is `HOST_CACHED`, type 1 is `HOST_COHERENT`. The coherent one is
meant to be an **uncached** mapping. `horizon_gpu` has exactly one
memory policy — `HORIZON_GPU_MEM_CACHED` (`memory.h:46`) — so this
backend cannot provide it.

The consequence is silent and total: `NVKMD_MEM_COHERENT` makes
`nvkmd_mem_sync_to_gpu` and `sync_from_gpu` **return before doing
anything** (`nvkmd.c:457`, `:476`). An application allocating from that
type gets CPU-cached memory with no cache maintenance anywhere, and
neither side sees the other's writes. `t_vulkan` avoids it only because
it takes the first host-visible type it finds, which is type 0.

**Not advertising it is not an option.** Vulkan requires at least one
memory type with both `HOST_VISIBLE` and `HOST_COHERENT`.

So the fix is an uncached memory policy in `horizon/` — the mechanism
exists (`svcSetMemoryAttribute` with `MemoryAttribute_Uncached`, which
is how deko3d gets its uncached blocks) — and that means touching a
layer this project deliberately freezes. **Raised with the owner rather
than done.**

Until it is decided, the state is: the driver is correct for the
`HOST_CACHED` type, which is what the Phase 4 test uses, and wrong for
the `HOST_COHERENT` one, which nothing here uses yet. That is written
down rather than left to be discovered.


### 5. Does D14 block the Phase 4 exit criterion? No — traced

The worry raised by finding 4 is that `NVKMD_MEM_COHERENT` disables all
cache maintenance, and the driver allocates some of its *own* memory
with that flag. If any of it were on the fill path, the test could not
pass no matter what. Traced, allocation by allocation:

| Memory the fill touches | Flags | Maintained? |
|---|---|---|
| Command-buffer push memory (`nvk_cmd_pool.c:24`) | `NVKMD_MEM_LOCAL` or `GART`, **not COHERENT** | yes — `nvk_cmd_buffer.c:381` flushes the whole block |
| The zero page (`nvk_device.c:284`) | — | yes, flushed explicitly |
| Heap-backed allocations (`nvk_mem_arena.c:173`) | shader/qmd heaps are `NVKMD_MEM_LOCAL`, not COHERENT | yes, flushed |
| The destination buffer | `HOST_CACHED` type — the app's choice, type 0 | yes: the test flushes the poison and invalidates before reading |
| The event heap (`nvk_device.c:351`) | `NVKMD_MEM_LOCAL \| **COHERENT**` | no — and it is not on this path |
| The printf buffer (`nvk_device.c:137`) | `GART \| **COHERENT**` | no — and it is not on this path |

And `vkCmdFillBuffer` is not a shader at all: `nvk_cmd_copy.c:897`
pushes NV90B5 methods — the DMA copy engine — with a constant remap. No
QMD, no shader heap, no event heap.

**So D14 is real but it does not block Phase 4's exit criterion.** The
two COHERENT allocations the driver makes are the printf buffer and the
event heap, and the mandatory sequence uses neither. It has to be fixed
before anything uses events, `debugPrintfEXT`, or a `HOST_COHERENT`
memory type — which is to say before Phase 5 — but the fill test should
be able to pass without it.


---

## Second hardware run: the driver entered, and died on a NULL dispatch entry (2026-07-28)

```
  ok   the non-conformance opt-in is set in the environment
  ok   vkCreateInstance -> 0
  ...  27 entry points resolved through vk_icdGetInstanceProcAddr
  ok   vkEnumeratePhysicalDevices -> 0
  ok   one physical device, got 1
  ok   vkEnumeratePhysicalDevices(list) -> 0
<nothing further — no RESULT line>
```

**One physical device.** The opt-in worked, NVK accepted GM20B, and
`nvkmd_horizon_create_pdev` and `nvk_create_physical_device` both
succeeded on hardware. That is the backend's first real measurement and
it passed.

Then the log stops with no `RESULT:` line. `t_vemit` flushes after
every line, so line 34 is genuinely the last thing that completed: the
process died in **`vkGetPhysicalDeviceProperties`**, the next call.

### The cause: a NULL function pointer that no tool reported

`vk_common_GetPhysicalDeviceProperties` does one thing
(`vk_physical_device.c:136`):

```c
pdevice->dispatch_table.GetPhysicalDeviceProperties2(physicalDevice, &props2);
```

and the linked binary contained **no `GetPhysicalDeviceProperties2` at
all** — neither `nvk_` nor `vk_common_`. Measured on the artefact:

```
$ aarch64-none-elf-nm t_vulkan.elf | grep -c GetPhysicalDeviceProperties2
0
$ aarch64-none-elf-nm -u t_vulkan.elf | wc -l
0
```

Zero undefined symbols, and a hole. Mesa generates its dispatch tables
with `--weak`, so an entry point nothing defines resolves to zero
rather than becoming an undefined symbol; the link succeeds and the
call jumps to address 0.

Upstream states the rule and the remedy in one comment:

```
# Instruct users of this library to link with --whole-archive.
# Otherwise, our weak function overloads may not resolve properly.
    -- mesa/src/vulkan/runtime/meson.build:250
```

Upstream never meets it, because it links the driver into one shared
object where every archive is pulled whole by construction. Here the
archives are named by hand.

### Two wrong fixes before the right one, both worth recording

1. **Adding `libvulkan_lite_runtime.a` to the list.** No effect — the
   member is reached only by weak references, so nothing pulls it.
   That is the whole trap restated.
2. **`--whole-archive` around every archive.** Hundreds of duplicate
   definitions, and the reason is the interesting part:

   ```
   ld: .../libvulkan_instance.a.p/vk_instance.c.o: multiple definition
       of `vk_instance_end_renderdoc_capture';
       .../src/nouveau/vulkan/../../vulkan/runtime/libvulkan_instance.a.p/
       vk_instance.c.o: first defined here
   ```

   The same object by two paths. **`libnvk.a` already contains the
   Vulkan runtime** — upstream builds it with dependencies that
   `link_whole` the runtime, and Meson propagates that into the static
   archive: 178 members, including `vk_physical_device_properties.c.o`
   and two copies of `vk_instance.c.o`.

**The fix is one archive.** `libnvk.a` is pulled whole; the separate
`libvulkan_runtime.a`, `libvulkan_lite_runtime.a` and
`libvulkan_instance.a` are not in the list at all, because they are
already inside it. `t_vulkan.elf` goes from 59 449 576 to 63 358 064
bytes — the members that used to be dropped.

### A gate, and the first version of it was worthless

`scripts/check-dispatch-complete.sh`.

The obvious check — "no weakly undefined symbols in the executable" —
**cannot fail**. devkitA64 links with `-z nodynamic-undefined-weak`
(libnx's `switch.specs`), so such a symbol is resolved to zero and does
not survive into the symbol table. Written, then tested against a file
built to have exactly that hole, and it reported OK. A gate that cannot
fail is not a gate; it was deleted rather than kept as decoration.

What the gate checks instead: every entry point the generated common
dispatch table names must be implemented by *something* —
`nvk_<Entry>`, `vk_common_<Entry>`, or the `KHR`/`EXT` alias a promoted
core entry point is often implemented under. Extension entry points are
exempt, because an unadvertised extension's NULL is unreachable. One
core entry point is exempt by name with its reason:
`EnumerateInstanceLayerProperties` is the loader's job and this
platform has no loader.

Current state: **825 entry points named, 234 core, 1 allowed absence,
OK.** And broken on purpose — reverting the `--whole-archive` and
rebuilding makes it report **19** missing core entry points, including
the one that killed the console run.



### One more, found in the same reading: `GPU_MULTI_WAIT`

`vk_sync_timeline` validates the type it is built on
(`vk_sync_timeline.c:51-60`) and requires
`VK_SYNC_FEATURE_GPU_MULTI_WAIT`. `nvk_horizon_sync_type` did not
declare it. Both that check and `vk_sync.c:74` are **asserts**, so it
was silent in this `NDEBUG` build and would abort inside
`vkCreateDevice` in a debug one — the sync type registered for timeline
emulation did not meet the contract of the thing emulating over it.

The flag is also simply true: `nvkmd_horizon_ctx_wait` takes a
`wait_count` and waits on every entry, so the declaration was
describing less than the code does. Declared. No runtime behaviour
changes in this build; every use of the flag is an assert, which is
exactly why it needed reading rather than running to find.

Series: **twenty-seven**.


---

## Third hardware run, and how far reading gets on `VK_ERROR_UNKNOWN` (2026-07-28)

```
  ok   one physical device, got 1
  note device: NVIDIA gm20b (NVK gm20b) (api 1.3.354)
  ok   at least one queue family, got 1
  FAIL vkCreateDevice -> -13
RESULT: FAIL (34/35) [aborted early]
```

`GetPhysicalDeviceProperties2` works: the driver names the chip and its
API version, and finds its queue family. The `--whole-archive` fix was
the right one. **Four of the mandatory sequence's steps are now
hardware-verified.**

`-13` is `VK_ERROR_UNKNOWN`, which this backend returns from
`nvkmd_horizon_result()` for a `horizon_gpu` status with no Vulkan
equivalent — `INVALID_ARG`, `NV`, `BUSY`, `STATE`, `OVERFLOW`, `LEAK`.

### What reading ruled out, and what it did not

| Candidate | Verdict |
|---|---|
| `nvkmd_horizon_create_dev` | **ruled out.** It returns only `OUT_OF_HOST_MEMORY` and `INITIALIZATION_FAILED`; there is no path to `UNKNOWN` |
| `nvkmd_horizon_create_ctx` | **very unlikely.** `horizon_gpu_channel_create` + `bind_engines` is what `t_channel` does, and `t_channel` passes on this console |
| the device init that follows — heaps, arena, upload queue | **the remaining ground.** Three sites there produce `UNKNOWN`: a failed `vm_map` (`va.c`), a failed unbind (`va.c:135`), and a push that is not a whole number of dwords (`ctx.c:132`) |

The leading hypothesis was a **page-size alignment mismatch** on bind:
`horizon_gpu_vm_map` requires both offsets to be aligned to the
*reservation's* page size (`vm.h:59-63`), and `bind_align_B` is the
big-page size.

**Checked, and it does not hold.** `nvkmd_horizon_alloc_va` picks the
big-page half only when the size *and* the alignment are both multiples
of the big page — which is exactly the condition under which every
mapping inside the reservation can meet the alignment `vm_map` will
demand. An offset aligned to `bind_align_B` is aligned to either page
size. Patching that alignment would have changed something already
correct.

So reading has gone as far as it goes. Three candidates survive, and
they are distinguished by one line of output rather than by a guess.

### What was done instead of guessing

Two diagnostic changes, because the reason had already been lost twice:

1. **`stderr` now lands in the test's log file** (`dup2` onto its
   descriptor, unbuffered). Mesa reports through `mesa_logw`/`vk_errorf`,
   which reach stderr, and on a console stderr is the screen — which is
   not what comes back. The driver had printed the failing call and the
   libnx `Result` both times and neither survived.
2. **The bind failure names the number that decides it.** It printed
   the VA offset and the range; it now prints the memory offset, the
   PTE kind and the reservation's page size as well.

Neither changes behaviour. The next console run distinguishes the three
candidates instead of narrowing them.

Series: **twenty-eight**.


---

## RESOLVED: the syncpoint failure is the emulator, not the driver (2026-07-28)

**The owner has confirmed it: the four `t_vulkan` runs of 2026-07-28
were on an emulator, and the same binaries behave differently there
than on the console.** The comparison is now on the record from both
sides:

| | real Switch (2026-07-27) | emulator (2026-07-28) |
|---|---|---|
| `t_channel` | **PASS 17/17**, `syncpt id=26, value at create=104880`, and a *second* channel got a distinct id (27) | first channel gets `syncpt id=1` |
| `SyncptRead` on that id | works | `0x55c` = `LibnxNvidiaError_NotImplemented` |

The console logs of eleven tests in both process modes — `normal` and
`applet` — are all PASS except `t_sysinfo` 18/19, which is a separate
known item.

**So `horizon_gpu` is not at fault and nothing found inside
`vkCreateDevice` on those runs is a driver defect.** The error code said
so literally and it was read too late: `NotImplemented` is what an
emulator answers for an `nvhost-ctrl` ioctl it does not implement, and
a syncpoint id of 1 is the degenerate value that goes with it.

**What this does not change:** Phase 4's exit criterion still needs the
real console, because "the CPU reads the pattern the GPU wrote" cannot
be certified by an environment that does not implement the syncpoint
the fence is built on.

**What it does change:** the emulator cannot take `t_vulkan` past
`vkCreateDevice` as the code stands, because
`horizon_gpu_channel_create` treats the initial syncpoint read as
fatal. That read initialises `shadow_target`, which fence arithmetic
depends on, so degrading it silently would make fences lie. The right
shape is the one already used for NVK's non-conformance check: the
*application* opts in to a degraded mode by name, and nothing degrades
by default. Next task.

### The labelling error this section began as

`CLAUDE.md` requires distinguishing **host build**, **cross build** and
**verified on real hardware**, and says never to claim Switch behaviour
from anything weaker. The four runs were recorded as "hardware run"
**without asking where they were executed**. That was mine, and it is
corrected here rather than quietly left.

`CLAUDE.md` requires distinguishing **host build**, **cross build** and
**verified on real hardware**, and says never to claim Switch behaviour
from anything weaker. The runs recorded above as "hardware run" —
the four `t_vulkan` executions of 2026-07-28 — were labelled that way
**without asking where they were executed**. The owner has since said
they may have been on an emulator.

What is *not* in doubt, because it is recorded with its own date and
attribution: the runs of **2026-07-26 and 2026-07-27** are marked
"real Switch, owner-executed", and that is where `t_channel` reported
`syncpt id=26` and passed 17/17, and where `t_threads` and `t_ostime`
passed 67/67 and 43/43.

### Why this matters to the diagnosis, and not only to the bookkeeping

The failure being chased is:

```
[horizon_gpu:E] initial SyncptRead(1) failed: 0x0000055c
```

`0x55c` decodes as `Module_LibnxNvidia` description 2 —
**`LibnxNvidiaError_NotImplemented`**. That is the literal answer an
emulator gives for an `nvhost-ctrl` ioctl it does not implement, and a
degenerate syncpoint id of 1 fits the same shape.

So the two readings are very different work:

| If the last runs were… | The syncpoint result means | What follows |
|---|---|---|
| **an emulator** | the environment does not implement `SyncptRead`; `horizon_gpu` is not at fault and nothing found in `vkCreateDevice` is a driver bug | record it as an environment limitation, and Phase 4's exit criterion still needs the real console |
| **a real Switch** | a regression in `horizon/` since 2026-07-27, or something about the 63 MB `t_vulkan` process | four commits to bisect, all of them here |

`t_channel.nro` from the current build separates them: it passed 17/17
on real hardware, it is 250 KB with no Mesa in it, and it makes the
same two calls. Sent to the owner for exactly that purpose.

**Until the platform is confirmed, the four `t_vulkan` runs of
2026-07-28 should be read as "execution platform unconfirmed", and no
Switch behaviour should be concluded from them.** The findings they
produced that are *not* platform-dependent stand on their own evidence:
the NULL dispatch entry was proved by `nm` on the binary, the unflushed
poison and the stack overflow by reading, and D12 and D14 by the
interface contracts.


---

## Phase 3 — the state it closed in (previously "Current phase")

**Phase 3 — minimal Horizon support in Mesa. Every milestone item now
has a disposition with evidence behind it; the phase's build criterion
is met. Two hardware measurements are owed and named below.**

**Mesa configures for `horizon` (`meson setup` exits 0) and its
non-driver core builds: 379 of 379 edges, zero failures, all ten static
libraries archived.** That is `docs/milestones.md`'s Phase 3 exit
criterion "Mesa configures for `horizon` and builds the non-driver
core", as a cross build (X).

Item by item: **1** OS detection (patch 0007 + 0012), **2** Meson
`host_machine.system()` — **no patch, and the reason is measured**,
**3** newlib/libnx gaps (patches 0001–0006), **4** threads (patch 0003 +
0011, decision recorded), **5** timers/clocks (patch 0008), **6**
physical memory / page size (`compat/sysconf.c` + patches 0009–0010),
**7** endianness (patch 0004), **8** build ID (answered in Phase 2, no
patch). The series stands at **twelve patches**, every one formulated as
a property of the C library or the compiler rather than as an OS name.

**Both new tests now pass — on an emulator (Eden), not on a console:**
`t_ostime` **43/43**, `t_threads` **67/67** (2026-07-28, after three
review rounds; 27/27 and 65/65 before the third round added checks). Getting there
found a **miscompile**: devkitA64 gcc 15.2.0 generates wrong code for
every `_Thread_local` access under `-mtp=soft -fPIC`, doubling the
thread pointer instead of adding the variable's offset and emitting no
relocation at all. Meson was appending that `-fPIC` to every
static-library object in the Mesa build, so all three objects there that
use TLS were affected, silently, and one of them hung `t_threads`. Fixed
with `-Db_staticpic=false` — already used in this project's own
`meson.build` for a smaller reason — and gated by
`scripts/check-tls-relocs.sh`, which fails on the property rather than on
the flag.

`mesa-patches/0012` is a measurement now: `util_cpu_detect` reports **4
CPUs** from a `0xf` core mask, where without the patch it reports 1.

**Codex reviewed a third time after that fix and left 8 findings, all
8 real.** Three are earlier fixes that stopped one level short — the
`clean` rule fixed for spelling but not for nesting, stale-`.nro`
pruning fixed in `build/` but not in `build/pkg`, the unbounded-wait rule
applied to `t_threads` but not to `t_ostime` — and one is a comment
claiming a gate was wired when nothing invoked it. See "Codex PR review,
PR #4, third round" below.

**What is still owed:** a **console** run. Eden answers for its own
libnx and SVCs, so items 4 and 5 remain cross-build-plus-emulator until
a Switch log exists. The two sections below have every measurement.

**Codex reviewed PR #4 twice.** The first round left 8 findings — 7 real
and fixed, 1 refuted with the generated `build.ninja` in hand — mostly
about the two new tests being able to *report* a failure rather than
hang on it. The second round reviewed that output too and left **18
findings, 17 of them real**: a `make clean` that still lost the Mesa
build for any non-canonical spelling of the path, a Meson build that
could never notice Mesa appearing after it was configured, bounds loose
enough to pass for the failure they name, unchecked mutex returns in the
test's own workers, and six places where the tree described itself more
strongly than the code supports. Both rounds are written up below; the
second one also corrects the first one's write-up of the refuted P1.

Two things happened before any Mesa work was possible, both recorded
below: a defect in **our own** cross file was making six of Mesa's
configure checks return false answers, and `mesa-patches/` had no
mechanics at all (it held a `.gitkeep`).

`compat/` has its **first content**: `sysconf`, which devkitA64's newlib
declares and does not define. That is the one door `CLAUDE.md` leaves
open, and `scripts/check-layering.sh` now polices it.

**Closed 2026-07-27:** all eleven `.nro` were run on a real Switch, in
**both applet and full/game mode**. Ten of eleven PASS in both; Phase
1's two hardware exit criteria go back to ✅ on the current code. See
"Hardware run of all eleven `.nro`" below.

`compat/sysconf.c`'s mode-aware argument is confirmed by a wide margin —
394 MiB of process memory in applet mode against 3189 MiB in full/game,
an 8.1× difference on the same console — so a hardcoded constant would
have been badly wrong, not approximately right.

`t_sysinfo` failed one check on the first run — its granularity probe
asked for the wrong memory region, which the test correctly reported as
"not a statement about the page size". Fixed and re-run the same day:
**PASS 21/21 in both modes**, so the page size is now bounded from both
sides by measurement. **Nothing is owed on hardware.**

---

## Codex PR review, PR #4 (2026-07-27) — 8 findings, 7 real

`chatgpt-codex-connector` reviewed PR #4 at `1b5e0bf` and left **2 × P1
and 6 × P2**. Each was checked against the code and against a command
before anything was changed. **Seven held up. One did not**, and it was a
P1 — the first finding in these rounds that did not survive being looked
at, so it is written up in as much detail as the ones that did.

Nothing in `horizon/` and nothing in `mesa-patches/` was touched: every
fix is in this repository's build files or in the two new tests.

| # | Finding | Disposition |
|---|---|---|
| P1 | The Mesa archives reach the link only through `link_args`, so Ninja does not treat them as inputs and a rebuilt Mesa would not relink the tests | **Refuted, with the generated file** — and the write-up of that refutation was itself defective; see the correction below. Meson already records them: `guess_external_link_dependencies()` (`ninjabackend.py:3636` in meson 1.11.2) appends any link argument that `os.path.isabs() and is_library() and os.path.isfile()`, and `build/meson/build.ninja:287` reads `build t_threads.elf: c_LINKER … \| …/libmesa_util_c11.a …/libmesa_util.a …`. Measured: `touch build/mesa-probe/src/util/libmesa_util.a` then `scripts/build-horizon.sh` → `[1/4] Linking target t_ostime.elf … [3/4] Linking target t_threads.elf`. The Makefile path already had the archives as explicit prerequisites (`$(MESA_TESTS:%=$(BUILD)/%.elf): $(MESA_LIBS)`). `link_depends` was added anyway, one line |
| P1 | A build that skips tests 12 and 13 leaves the previous build's `.nro` for `scripts/package-horizon.sh` to ship | **Real on the Makefile path.** Measured: build 13 `.nro`, move `build/mesa-probe` aside, `make` → the two stale `.nro` were still there, and `package-horizon.sh` copies `"$SRC"/*.nro` unconditionally into a manifest whose entire job is attributing an artefact to one build. Fixed: the skip branch now collects `$(STALE_MESA)` and a `prune-stale` prerequisite of `all` removes the `.nro`, `.elf`, `.nacp` and `.t.o`. **Not real on the Meson path**, and the reason is measured: Meson runs `ninja -t restat && ninja -t cleandead` itself after regenerating `build.ninja` (`ninjabackend.py:705`, for ninja ≥ 1.12 or ≥ 1.10 without dyndeps — this build has ninja 1.11.1 and no dyndeps), so after the same experiment there were 11 `.nro` on disk *before* the build step ran. A `cleandead` added to `build-horizon.sh` cleaned 0 files and was removed again; the condition is recorded in that script so a future ninja or dyndeps change has something to check against |
| P1 | `thrd_join` on the `mtx_timedlock` worker, and the main thread's own `cnd_timedwait`, are unbounded waits — the exact failure the section exists to catch would hang the test instead of failing it | **Real, and the sharpest finding of the round.** A `mtx_timedlock` that never returns is *the* thing test 12 was written for, and the test would have hung on it with the verdict unwritten. Fixed with two different mechanisms because the two cases are not the same: the worker is watched by the main thread through an `atomic_int done` polled against `armGetSystemTick` for `WATCHDOG_MS` (2000 ms, above the 800 ms upper bound so a merely slow implementation is still reported as slow), and the mutex is released **before** the join, which is what frees a worker stuck in a lock that ignored its deadline; the main thread's `cnd_timedwait` gets a watchdog thread that sets the predicate and broadcasts. The failing `t_check` is written *before* either call that could still block, and `testfw` fflushes each line to sdmc, so even a run that hangs anyway leaves a log naming the section |
| P2 | `make clean` deletes `build/mesa-probe`, so `make clean && make` silently drops the two Mesa tests | **Real.** `clean` was `rm -rf build` and `$(MESA_BUILD)` lives there. Fixed by filtering. **Wider than reported, and found by running it:** the same `rm -rf` also removed `build/toolchain`, i.e. the pinned Meson install and Mesa's Python generator deps, both installed *from the network* — which `CLAUDE.md` names as the thing that may not be reachable. `make clean` uninstalling the build system is not what anyone types it for. Now `clean` keeps `$(MESA_BUILD)`, its `.crossid` stamp and `$(BUILD)/toolchain`, and removes the parts of `toolchain/` this Makefile does produce (`lib/`, `compat-obj/`) explicitly |
| P2 | `deadline_in_ms()` ignores `timespec_get`'s return, and C11 leaves `*ts` untouched on failure | **Real, and it is the same defect this session documented in `os_time_get_nano()`** — writing the test that reports Mesa's bug with Mesa's bug in it. Now returns `bool`; every caller reports "no deadline could be built" and does **not** call the timed function, so a clock failure cannot be misread as a timeout failure |
| P2 | `mtx_init`/`cnd_init`/`tss_create` failures were recorded and then the section carried on with an invalid object | **Real.** Fixed by giving each section its own function that returns as soon as its own setup fails — the object is also created and destroyed inside it, so ownership is one function's rather than `run_test`'s. `section_cpu_count` depends on none of them and runs regardless, which matters: it is the hardware evidence for patch 0012 |
| P2 | `t_ostime`'s 2000 reads can finish inside one tick of the 1 ms resolution the test itself accepts, so `distinct == 1` would report a working clock as stopped | **Real.** The loop now also runs until the ARM system counter — not the clock under test — says `SAMPLE_MIN_MS` (5 ms, four accepted ticks) has passed, with a `SAMPLES_MAX` ceiling so a stopped clock ends the loop rather than the console. "Did the clock advance" is asserted only once that interval is established; if the ceiling is hit first, that is the *reference* counter failing and the note says so |
| P2 | `meson.build` and the `Makefile` hardcode `build/mesa-probe` while `scripts/{configure,build}-mesa.sh` honour `$MESA_BUILD_DIR` | **Real.** Fixed in one place: `scripts/toolchain-env.sh` now defines the default, both Mesa scripts inherit it, the Makefile reads `$(or $(MESA_BUILD_DIR),build/mesa-probe)`, `scripts/build-switch.sh` forwards it into the container (which sees only the variables named on `docker run`), and `configure-horizon.sh` passes `-Dmesa_build_dir` — an option and not an environment read, because Meson runs *inside* that container |

### What the round says about the verification, not the code

- **Three of the seven are the same mistake:** a test that measures a
  timeout was written as if the implementation would always return. The
  lower and upper bounds were carefully both-sided and then reached
  through an unbounded wait. Bounding the *value* and not the *wait* is
  half a measurement.
- **Two findings were only partly right, and finding the rest needed
  running the command, not reading the diff.** `make clean` losing the
  pinned Meson install, and the Meson half of the stale-`.nro` finding
  being already handled, both came out of executing the scenario.
- **The refuted P1 was refuted with a file, not an argument.** The rule
  this project applies to Mesa patches — no change without a measured
  defect — applies to review findings too. The one-line `link_depends`
  that went in anyway is documented as hardening of a Meson heuristic,
  not as a fix.

**Correction, made in the second round (2026-07-27).** That row
originally justified the one-line change by saying the inference "is
conditional on the file existing at configure time" — which asserts a
residual defect inside a row labelled *Refuted*. Codex called out the
contradiction and was right about it. The disposition stands, and the
reasoning does not: the conditionality creates no hole, because
`meson.build` builds these two tests **only** when the archives exist,
so any configuration in which the edge could be missing is one in which
there is no edge. `link_depends` is defence against a future Meson
changing an undocumented inference, and nothing more. The genuine defect
in that neighbourhood is a different one — `fs.exists()` never being
asked again — and it is finding 8 of the second round.

### A defect this round introduced and closed inside itself

Adding `meson.options` in the same commit that passes `-Dmesa_build_dir`
broke `scripts/configure-horizon.sh` on any existing build directory:

```
$ scripts/configure-horizon.sh
reconfiguring build/meson
ERROR: Unknown option: "mesa_build_dir".
```

`setup --reconfigure` validates every `-D` against the options recorded
at the first configure, *before* re-reading the file that declares them.
This is the same class as the P1 from the previous round (machine files
are only read on a first configure) and it is fixed the same way:
`horizon_setup_mode` / `horizon_cross_id` / `horizon_record_cross_id`
now take extra identity files, `configure-horizon.sh` passes
`meson.options`, and a change to it wipes that build directory.
`configure-mesa.sh` deliberately does **not** pass it — our options do
not affect Mesa's build directory, and wiping that one costs minutes.

### Verification after the fixes

Every command below was run from the repository root; the toolchain is
`ghcr.io/d3fau4/nx-dev:latest` (no local devkitA64). All of it is **cross
build (X)**. Nothing here is a hardware result.

| Check | Command | Result |
|---|---|---|
| Rebuilt Mesa relinks the tests | `touch build/mesa-probe/src/util/libmesa_util.a && scripts/build-horizon.sh` | `[1/4] Linking target t_ostime.elf … [4/4]` — 2 tests relinked |
| `make clean` keeps what it does not own | `scripts/build-switch.sh clean && ls build/` | `mesa-probe  mesa-probe.crossid  toolchain` |
| …and a clean build still makes 13 | `scripts/build-switch.sh` | 13 `elf2nro` invocations, 13 `.nro` |
| Stale pruning, Makefile path | move `build/mesa-probe` aside, `scripts/build-switch.sh` | `removing stale Mesa test artefacts: …t_threads.nro …t_ostime.nro …` then **11** `.nro` |
| Stale pruning, Meson path | same, then `configure-horizon.sh` | **11** `.nro` on disk before `build-horizon.sh` ran; Meson had already cleaned |
| `MESA_BUILD_DIR`, Makefile path | `MESA_BUILD_DIR=build/mesa-alt scripts/build-switch.sh` | links `build/mesa-alt/src/…/libmesa_util*.a`, 13 `.nro` |
| `MESA_BUILD_DIR`, Meson path | `MESA_BUILD_DIR=build/mesa-alt scripts/configure-horizon.sh` | `tests : 13`, `mesa_build_dir: build/mesa-alt` |
| Both tests still compile `-Wall -Wextra -Werror` | `scripts/build-switch.sh` | clean, no diagnostics |
| Patch series on a reset `mesa/`, ×2 | `scripts/apply-mesa-patches.sh` | applies 12 (`mesa at bf8dbcd`); second run `all 12 patches already applied; nothing to do` |
| `.nro` parity, Makefile vs Meson | `stat -c%s` over both directories | **13/13 identical sizes** |
| Host unit tests | `scripts/run-host-tests.sh` | **103/103** |
| Gates | `check-layering.sh`, `check-no-abs-paths.sh`, `check-rust-target.sh` | all OK |

`scripts/check-no-abs-paths.sh` gained `meson.options` as a target in the
same commit: it is a build input, and it now carries a *default
directory*, which is the shape a machine-specific path takes when it
arrives by accident.

### Still owed after this round — unchanged

`t_threads` and `t_ostime` have still never run on a console. Everything
above makes them better at reporting what they find; none of it is a
measurement of Horizon. The watchdogs in particular have never fired,
because the code containing them has never executed.

---

## First run of `t_threads` and `t_ostime` — emulator (2026-07-28)

**Class: E (emulator).** The owner ran both `.nro` (`3f97f5d2…`,
`ff999f01…`) on a Nintendo Switch **emulator**, not on a console. That is
a fourth class alongside host (H), cross (X) and hardware (HW), and it is
kept separate on purpose: an emulator answers for its own implementation
of libnx's syscalls, not for the console's. Nothing below is a hardware
result and none of it closes items 4 or 5.

### `t_ostime` — PASS 27/27

Every check passed. The measurements, which are the point of the test:

| Quantity | Measured |
|---|---|
| `timespec_get(TIME_MONOTONIC)` | returns `TIME_MONOTONIC`; the clock answers |
| `os_time_get_nano` monotonicity | 22915 samples over 5 ms, **22915 distinct values, 0 backwards** |
| Resolution | **52 ns** — far finer than the 1 ms the test would have accepted |
| Rate against the ARM system counter | 100164688 ns measured against 100242500 ns reference over one 100 ms sleep — **0.08 % apart**, against a 10 % tolerance |
| `os_time_sleep(50000 us)` | 50509 us |
| `os_time_sleep(0)` | 60 us |
| `os_time_nanosleep_until(+50 ms)` | 50335 us |
| Past deadline | 185 us |

So `os_time.c` behaves on this emulator, and the unchecked
`timespec_get` inside `os_time_get_nano()` is not returning stack
garbage here. The tightened bounds from the second review round all held
with room to spare — the sleep landed 1 % long against a −25 % floor.

**One thing the run recorded that no check asserts.** The first note
reads `ts = 1785229380 s + 81102164 ns`, which is **2026-07-28 09:03:00
UTC** — wall-clock time, not time since boot. `CLOCK_MONOTONIC` here is
the real-time clock, so `os_time_get_nano()` would step if the system
clock were adjusted. Monotonicity held across the 5 ms sample window and
across the 100 ms reference sleep, which is all this test claims; a clock
that is monotonic *only while nobody sets the date* is a different
property from the one Vulkan timeouts want, and Phase 4 needs to know
which one it has. Recorded as an open question, not a failure.

### `t_threads` — did not finish

70 checks, **all `ok`**, and then the log stops. There is no `RESULT:`
line, so the process did not reach the end of `run_test`. The last line
written is

```
  ok   sysconf(_SC_NPROCESSORS_CONF) = 4 answers as _ONLN (4), the case label it shares
```

and the next statement in the file is `caps = util_get_cpu_caps();`.
`testfw` fflushes every line to sdmc, and `t_ostime`'s log from the same
run is complete, so the missing tail was never written rather than lost
in writeback. **The process stopped inside `util_get_cpu_caps()`.**

Everything the test set out to measure about Mesa's C11 shim passed
first:

| | |
|---|---|
| `thrd_create` / `thrd_join` / `u_thread_create` | ok, return value carried |
| `call_once` across 4 threads | body ran exactly once |
| Shared counter, 4 × 20000 under a mutex | **80000, no update lost** |
| `mtx_timedlock` on a held mutex | `thrd_timedout` after **200 ms** for a 200 ms deadline |
| `mtx_timedlock` on a free mutex | `thrd_success`, 0 ms |
| `cnd_signal` / `cnd_broadcast` | all 4 waiters woke, 0–1 ms |
| `cnd_timedwait` with nobody signalling | `thrd_timedout` after **200 ms**, watchdog never fired |
| TSS | per-thread values isolated, destructor ran for all 4 |
| `InfoType_CoreMask` | **0xf — four cores**, `sysconf` agrees on both names |

That is milestone item 4's whole question answered on this emulator: the
polling `mtx_timedlock` neither returns at once nor hangs, and it lands
on its deadline rather than near it.

### What is known about the failure, and what is not

`util_get_cpu_caps()` is an inline function in `u_cpu_detect.h`; it calls
`call_once(&_util_cpu_caps_state.once_flag, _util_cpu_detect_once)`. On
aarch64/Horizon that function does, in order: two `sysconf` calls, an
assignment for NEON, `check_cpu_caps_override()`, `check_max_vector_bits()`
(an assignment), `get_cpu_topology()` (a `memset` on this arch), and
`debug_get_option_dump_cpu()`.

Most of that is already known to work in this very run. The two `sysconf`
calls are the ones logged two lines above. `call_once` passed its own
section. What has **never** run before this point is the option lookup:
`debug_get_option_cached` → `os_get_option_cached`, which on first use
takes a statically initialised `simple_mtx_t` — whose lock goes through a
`thread_local` in `u_call_once.c` on this platform, because
`UTIL_FUTEX_SUPPORTED` is 0 here — then builds a hash table with
`ralloc` and registers an `atexit` handler. `nm` on the linked ELF
confirms all of it is in the binary (`util_call_once_data_slow`,
`_simple_mtx_plain_init_once`, `os_get_option_cached`,
`_mesa_hash_table_create`).

Two hypotheses were checked and **eliminated** rather than left as
suspicion:

- **`getenv` with a null `environ`.** newlib's `_findenv_r` loads
  `environ` and branches out on zero before dereferencing it
  (`ldr x20, [x22]` / `cbz x20, …`, disassembled from
  `libc_a-getenv_r.o` in the pinned image). It returns NULL; it does not
  fault.
- **A `once_flag` ABI mismatch between the test and `libmesa_util.a`.**
  `u_cpu_detect.h` includes `util/u_thread.h`, which includes
  `c11/threads.h`, and the test includes the same header. One type.

Not established: whether the process crashed or hung, which the log
cannot say and the owner can. A hang points at the mutex or `call_once`;
a fault points at an access. Also not established: whether this is
emulator-specific. The whole path is libc and libnx — no GPU, no `nv`
services — which makes an emulator artefact less likely than for the
Phase 1 tests, and does not exclude one.

### What changed in response

`section_cpu_count` now reaches that call in four named stages, each
announced before it is entered, so the next log names the step instead of
the section: plain `getenv`, then `os_get_option`, then
`os_get_option_cached` (the hash table, `ralloc`, `simple_mtx` and
`atexit` path), then `util_get_cpu_caps()`. A provisional tally is
printed before the first of them, because a call that does not return
takes the `RESULT:` line with it and a run that produced 70 results
should not be unreadable on account of the 71st.

No check was removed or weakened, and nothing was worked around.

---

## Codex PR review, PR #4, third round (2026-07-28) — 8 findings, 8 real

Reviewed after the TLS fix landed. **All eight held up**, six of them
reproduced with a command before anything was changed and two confirmed
by reading the code they describe. Three are the same defect class as
something fixed in an earlier round, arriving one level further out —
which is the useful thing about this round.

| # | Finding | Disposition |
|---|---|---|
| 1 (P1) | The reconfigure stamp records only *whether* Mesa's archives are present, so pointing `$MESA_BUILD_DIR` at a different directory that also has them reconfigures nothing and the tests keep linking the old build | **Real, reproduced.** `MESA_BUILD_DIR=build/mesa-alt scripts/build-horizon.sh` left `build.ninja` naming `mesa-probe`, silently. The stamp now holds `<present\|absent> <directory>` and is produced by one function, `horizon_mesa_state`, called from both the writer and the reader. Verified in both directions and idempotent on a third run |
| 2 (P1) | The condvar sections signal before the worker has reached `cnd_wait`, so the already-true predicate lets it finish even against a broken `cnd_signal` — and if it *is* waiting, that same defect blocks `thrd_join` forever | **Real, and both halves matter.** The check could pass for a `cnd_signal` that does nothing, and could hang for one that wakes nobody. Workers now publish `ready` under the mutex immediately before waiting — `cnd_wait` releases the mutex atomically, so a main thread that sees the count and then takes the mutex knows the worker is *inside* the wait — and the main thread awaits `woken` on an atomic with a 2000 ms bound instead of joining, records the failure before the join, and broadcasts as recovery. The post-join wakeup check was dropped: keeping it would report one defect twice, which round 2 fixed elsewhere |
| 3 (P2) | `make clean`'s keep-set is an exact match, so `MESA_BUILD_DIR=build/cache/mesa-probe` puts `build/cache` in `$(wildcard)` and `rm -rf build/cache` takes the Mesa build with it | **Real, reproduced** with `make -n clean` after creating the nested directory. This is the *same finding as round 2's #1* one level out: that round fixed how the path is spelled, not how deep it is. An entry now survives when it is in the keep-set **or contains** the Mesa build. Keeping a whole intermediate directory is the conservative direction and is stated in the Makefile rather than left to be discovered |
| 4 (P2) | An absolute `MESA_BUILD_DIR` outside `$PWD` is accepted, but `horizon_run` mounts only `$PWD`, so Meson configures into the container's own filesystem and the directory is gone when it exits | **Real, measured.** A file written to `/var/tmp/...` inside the container is readable there and absent on the host a moment later; the same write under `$PWD` is on the host. Now rejected with the reason, and only in container mode — with a local devkitA64 there is no container and the path works. An absolute path *inside* the tree still works, which is what `meson.build` advertises |
| 5 (P2) | `configure-mesa.sh`'s comment says `check-tls-relocs.sh` fails the build, and nothing invokes it — so `-Db_staticpic=true`, which the trailing `"$@"` allows on purpose, still ships the miscompile | **Real, and the sharpest of the round.** A comment claiming enforcement that does not exist is exactly what round 2 was about, written into the fix for round 2's own lesson. `build-mesa.sh` now runs the gate over the objects after every build — after, because a gate that runs first inspects the previous build's output |
| 6 (P1) | `package-horizon.sh` copies the current `.nro` into `build/pkg` but never removes ones the source no longer has, then hashes everything in the destination into a fresh manifest | **Real, reproduced.** *Same defect as round 2's stale-`.nro` finding, one level further out*: that round stopped `build/` from mixing two builds, and the packaging step then did it anyway. Packaged 13, removed the two Mesa tests from the source, packaged again — `build/pkg` kept them and the new manifest claimed them. Now the destination is synchronised first: `dropping t_ostime.nro — not in build/meson`, manifest 11, and back to 13 when they return |
| 7 (P2) | `once_calls++` is a plain `int`, so if the broken `call_once` being hunted runs the body concurrently the increments race and can lose one, leaving 1 and passing | **Real.** The counter written to detect concurrent execution has to be defined under concurrent execution. Now `atomic_int` |
| 8 (P2) | `t_ostime` calls `os_time_sleep` and `os_time_nanosleep_until` synchronously, so a sleep that never returns takes the upper-bound check and the verdict with it | **Real, and it is round 1's P1 applied to the file it was not applied to.** `t_threads` was corrected for exactly this; `t_ostime` was not. Each blocking call now runs on a worker awaited against `armGetSystemTick()` with a 2000 ms bound, and the failing check is written before anything that could block. **libnx threads, not Mesa's C11 shim**: that shim is what `t_threads` measures, and building this file's watchdog out of it would make `t_ostime` fail for reasons that are not about `os_time.c`. On the timeout path the worker's thread and context are deliberately leaked — it may still be inside the call and will write to that memory when it returns |

### What this round says about the previous ones

- **Three findings are earlier fixes that stopped one level short.** The
  clean rule was fixed for spelling and not for nesting; the stale-`.nro`
  pruning was fixed in `build/` and not in `build/pkg`; the unbounded-wait
  rule was applied to `t_threads` and not to `t_ostime`. In each case the
  original finding was fully addressed *as reported*, and the class was
  not. Reading the next call site outward is cheaper than a review round.
- **A gate was claimed and not wired.** Finding 5 is a comment asserting
  enforcement that did not exist — written in the same commit that
  introduced the gate, and in a round whose own lesson was that a comment
  is not enforcement.

### A defect this round introduced and closed inside itself

The nesting fix for finding 3 made `clean` delete **nothing at all**:

```
$ make -n clean
rm -rf
rm -rf build/toolchain/lib build/toolchain/compat-obj
```

The two `$(filter)` calls are joined by a line continuation, so with both
empty the expression is `" "` — and `$(if)` reads a lone space as true,
so every entry was kept. `$(strip)` around it, and the reason recorded in
the Makefile. Caught by running `make -n clean` on all six spellings
rather than on the one the fix was written for, which is the lesson
round 2 wrote down.

### Verification

| Check | Command | Result |
|---|---|---|
| Stamp records the directory | `MESA_BUILD_DIR=build/mesa-alt scripts/build-horizon.sh`, then back | before: `build.ninja` kept naming `mesa-probe`; after: reconfigures both ways, and a third run does not |
| `clean`, six path shapes | `make -n clean` × 6 | each keeps exactly the selected Mesa build (or the directory containing it) and `build/toolchain`, and deletes the rest |
| Absolute path outside the tree | `MESA_BUILD_DIR=/var/tmp/… scripts/build-mesa.sh` | rejected with the reason, exit 1; `configure-horizon.sh` likewise |
| Absolute path inside the tree | `MESA_BUILD_DIR=$PWD/build/mesa-probe` | accepted |
| The gate is wired | `scripts/build-mesa.sh` | ends with `check-tls-relocs: OK (3 object(s) use TLS…)` |
| Packaging drops stale artefacts | package 13, hide the two, package again | `dropping t_ostime.nro`, `dropping t_threads.nro`, 11 in the manifest; 13 again when restored |
| Both tests compile `-Wall -Wextra -Werror` | `scripts/build-switch.sh` | clean |
| `make clean && make` | `scripts/build-switch.sh clean && scripts/build-switch.sh` | Mesa and toolchain kept, **13 `.nro`** |
| Patch series on a reset `mesa/`, ×2 | `git -C mesa reset --hard $MESA_COMMIT && scripts/apply-mesa-patches.sh` | applies 12; second run `all 12 patches already applied` |
| `.nro` parity | `stat -c%s` over both directories | **13/13 identical sizes** |
| Host unit tests | `scripts/run-host-tests.sh` | **103/103** |
| Gates | tls-relocs, mesa-test-parity, layering, abs-paths, rust-target | all OK |

Both `.nro` changed again: `833d14ef…` (`t_threads`), `8c33bccf…`
(`t_ostime`).

### Re-run on the emulator after this round (2026-07-28, Eden)

```
RESULT: PASS (43/43)   t_ostime    (was 27/27)
RESULT: PASS (67/67)   t_threads   (was 65/65)
```

Every check the round added passes, and two of them make the verdict
mean more than it did:

```
ok   the cnd_wait worker reached the wait within 2000 ms (1 of 1)
ok   cnd_signal woke the waiter within 2000 ms (1 of 1)
ok   every broadcast worker reached the wait within 2000 ms (4 of 4)
ok   cnd_broadcast woke every waiter within 2000 ms (4 of 4)
```

The waiters were confirmed **inside** `cnd_wait` before the signal went
out, so "the waiter woke" is now a statement about `cnd_signal` rather
than one the predicate could have satisfied on its own. The previous
PASS could not distinguish the two.

The four bounded `os_time` calls all returned by themselves — no
watchdog fired and no worker was abandoned — and moving the measurement
inside the worker sharpened one figure: `os_time_sleep(0)` took **6 µs**,
against 50–60 µs when it was timed around a call on the main thread.

Two measurements are worth recording as *ranges* rather than constants,
because three runs now exist: `mtx_timedlock(200 ms)` returned after
**199–200 ms**, and the smallest observed clock step was **52 ns** in
two runs and **156 ns** in this one, on 13029 samples instead of 25310 —
the emulator was busier. Both are far inside their bounds; neither is
the rounded constant a single run made them look like.

Still class E. A console run is the only thing left.

---

## The cause: `-mtp=soft -fPIC` miscompiles thread-local storage (2026-07-28)

**Class: X (cross build), reproduced from a four-line file.** The staged
probe above located the failure in one run, and the owner supplied the
one fact the log could not: the application **hung**, it did not crash.
The emulator is **Eden**.

### What the second log says

```
note provisional tally before the staged probe below: 64 passed, 0 failed
note stage 1/4: getenv("GALLIUM_OVERRIDE_CPU_CAPS") — plain newlib
note stage 1/4 returned (null)
note stage 2/4: os_get_option — Mesa's wrapper over getenv
note stage 2/4 returned (null)
note stage 3/4: os_get_option_cached — hash table, ralloc, simple_mtx, atexit
```

and stops. So `os_get_option_cached()` does not return — on the main
thread, in a fresh process, outside any `call_once`. An isolated repro,
not a symptom of the CPU-detection path at all.

### The defect

`os_get_option_cached()` opens with `simple_mtx_lock(&options_tbl_mtx)`.
`UTIL_FUTEX_SUPPORTED` is 0 here, so that expands to
`util_call_once_data()` on a statically initialised flag, whose slow path
is in `u_call_once.c`:

```c
static thread_local struct util_call_once_context_t call_once_context;

void util_call_once_data_slow(once_flag *once, ...)
{
   struct util_call_once_context_t *once_context = &call_once_context;
   once_context->data = data;
   once_context->func = func;
   call_once(once, util_call_once_data_slow_once);
}
```

What devkitA64 gcc 15.2.0 compiled that to, **in the object file**:

```
   8:  bl   __aarch64_read_tp
   c:  lsl  x0, x0, #1          <- no relocation
  14:  stp  x2, x1, [x0]
```

It **doubles the thread pointer** instead of adding the variable's offset
to it, and emits no relocation for the linker to fix up. `readelf -r` on
that object finds no `R_AARCH64_TLS*` at all. Every read and write of a
`_Thread_local` lands at a wild address; here that is a 16-byte store at
`2 × tp`, which is what corrupted enough state to hang the process.

Isolated to one flag, from a four-line file compiled with the same
`$CFLAGS`:

| Flags | Generated |
|---|---|
| `-mtp=soft -fPIE` | `bl __aarch64_read_tp` + `add`/`add` with `R_AARCH64_TLSLE_ADD_TPREL_HI12` / `_LO12_NC` — correct |
| `-mtp=soft -fPIC` | `bl __aarch64_read_tp` + `lsl x0, x0, #1`, **no relocation** — the bug |
| `-fPIE` (hardware TP) | `mrs x0, tpidr_el0` + the same two `add`s — correct |
| `-mtp=soft -fPIE -ftls-model=local-exec` | correct |

It is `-fPIC` alone. The generated code is byte-identical to what is in
`u_call_once.c.o`.

### How it got in, and how far it reached

Meson appends `-fPIC` to every static-library object unless
`b_staticpic=false`. This project's **own** `meson.build` already sets
that in `default_options`, for a smaller reason recorded there — `-fPIC`
overriding the cross file's `-fPIE` made the Meson output diverge from
the hardware-verified Makefile output by 48 bytes of `.text` in
`t_alloc`. `scripts/configure-mesa.sh` did not set it, so Mesa's build
got `-fPIC` on every object.

Every object in the Mesa build that uses TLS was affected — **three of
three**:

```
src/util/libmesa_util.a.p/u_call_once.c.o
src/util/libmesa_util.a.p/u_debug.c.o
src/util/libmesa_util.a.p/u_qsort.cpp.o
```

Nothing warns. The compile succeeds, the link succeeds, and the program
misbehaves the first time a thread-local is touched. `t_threads` reached
`u_call_once.c` through a mutex; NVK in Phase 4 would have reached
`u_debug.c` and `u_qsort.cpp` through ordinary logging and sorting.

### The fix, and the gate

`scripts/configure-mesa.sh` now passes `-Db_staticpic=false`, with the
measurement above in its comment. Nothing built here is a shared library
— Horizon has no dynamic loader, which patch 0007 records — so `-fPIC`
buys nothing on this platform and costs this.

`scripts/check-tls-relocs.sh` is new and fails the build if it returns.
It tests the property rather than the flag: an object that calls
`__aarch64_read_tp` is accessing a thread-local, and correct code for
that access carries at least one `R_AARCH64_TLS*` relocation. An object
with the call and no relocation is the broken form, whatever produced it.

This gate is one of the few in the tree that has already failed on real
code rather than only on a deliberate breakage: it reports 3 bad before
the fix and 0 after.

### Verification

| Check | Command | Result |
|---|---|---|
| The miscompile, isolated | four-line file, four flag combinations | only `-mtp=soft -fPIC` is wrong; output byte-identical to `u_call_once.c.o` |
| Blast radius before the fix | scan every `.o` in the Mesa build | **3 of 3** TLS objects with no relocation |
| Mesa rebuilds with the option | `configure-mesa.sh && build-mesa.sh` | **379/379 edges**, 0 failures; `-fPIC` occurrences in `build.ninja`: **0** |
| The object after | `objdump -dr u_call_once.c.o` | `add`/`add` with `R_AARCH64_TLSLE_ADD_TPREL_HI12` / `_LO12_NC` |
| The linked binary after | `objdump -d t_threads.elf` | `add x0, x0, #0x0, lsl #12` / `add x0, x0, #0x10` — thread pointer plus offset |
| Gate | `scripts/check-tls-relocs.sh` | 3 TLS objects, all with relocations, 350 scanned |
| Both build paths | `build-switch.sh`, `build-horizon.sh` | 13 `.nro` each, **13/13 identical sizes** |
| Host unit tests | `scripts/run-host-tests.sh` | **103/103** |
| Other gates | layering, abs-paths, rust-target, mesa-test-parity | all OK |

Both `.nro` changed: `t_ostime` links the same rebuilt archives, so it is
a new binary too even though its source did not change.

### Confirmed by re-running: both tests pass (2026-07-28, emulator)

The same two `.nro` rebuilt against the repaired archives
(`a58e2af8…`, `92899b59…`), run on Eden:

```
RESULT: PASS (27/27)   t_ostime
RESULT: PASS (65/65)   t_threads
```

The test source did not change between the hanging run and this one —
the staged probe is the same code — so the only difference is the Mesa
rebuild with `b_staticpic=false`. Stage 3/4 now returns:

```
note stage 3/4: os_get_option_cached — hash table, ralloc, simple_mtx, atexit
note stage 3/4 returned (null)
note stage 4/4: util_get_cpu_caps
note util_cpu_caps: nr_cpus=4 max_cpus=4 num_cpu_mask_bits=32
ok   util_cpu_detect reports 4 CPUs, matching the core mask (4)
```

That closes the causal question: the TLS miscompile was what stopped it,
and nothing else was.

**`mesa-patches/0012` is now a measurement rather than a compile
result.** `util_cpu_detect` reports **4 CPUs** from a `0xf` core mask.
Without the patch its counting block is under `DETECT_OS_POSIX`, which
Horizon is not, so `available_cpus` stays 0 and `nr_cpus` falls to
`MAX2(1, 0)` — `util/u_queue.c` would have sized its thread pool for a
single-core machine on a four-core one. That is the number the patch
exists to fix, read off a running system.

Milestone items 4 and 5 now have a **behavioural** result behind them
and not only a link: Mesa's polling `mtx_timedlock` returns
`thrd_timedout` after 200 ms for a 200 ms deadline, `cnd_timedwait` the
same with its watchdog never firing, and `os_time.c` tracks the ARM
system counter to 0.08 % with 52 ns resolution.

**Still class E.** Eden is not a Switch: it answers for its own
implementation of libnx and of the SVCs beneath it. Items 4 and 5 stay
cross-build-plus-emulator until a console log exists. What an emulator
run *does* settle is everything that is a property of the compiled code
rather than of the machine — which is exactly what the TLS miscompile
was, and why finding it here was worth the two rounds.

---

## Codex PR review, PR #4, second round (2026-07-27) — 18 findings, 17 real

`chatgpt-codex-connector` reviewed `e0980e2..b328813` — that is, the
first review round's own output plus the Phase 3 closeout — and left
**18 findings**. Each was checked against the code and against a command
before anything was changed. **Seventeen held up.** The one that did not
is the one Codex itself marked as needing confirmation against the
pinned checkout.

Nothing in `horizon/` and nothing in `mesa-patches/` was touched. Six of
the eighteen are corrections to what the tree *says* about itself rather
than to what it does, and they are treated as defects on the same
footing: a comment that overstates what a check proves is how a false
claim gets into a hardware report.

| # | Finding | Disposition |
|---|---|---|
| 1 | `make clean` still deletes the Mesa build for any non-canonical `MESA_BUILD_DIR`, because `$(filter-out)` is a literal comparison | **Real, reproduced.** `make -n clean` with `build/mesa-probe/`, `./build/mesa-probe` and `build//mesa-probe` all put `build/mesa-probe` and its `.crossid` into the `rm -rf`. The verification behind the original fix only used `build/mesa-alt`, which happens to be canonical — the check passed because the input was well-behaved. Both sides now go through `$(abspath)`, which normalises without requiring the path to exist; `scripts/toolchain-env.sh` normalises for the script path too. After: all four spellings keep it, and `build/mesa-alt` still correctly does not keep `build/mesa-probe` |
| 2 | `clean`'s stated contract ("removes what this Makefile produces, and only that") contradicts what it does — it deletes `build/meson`, which Meson and `configure-horizon.sh` produce | **Real.** The behaviour is right and the sentence was wrong. The rule is now stated as it actually is: anything cheap to regenerate from this tree is removed, anything costing minutes of compilation or a network fetch is kept. `build/meson` is removed because it reconfigures in seconds and a stale cross build directory is a hazard; `$(MESA_BUILD)` and `$(BUILD)/toolchain` are kept because they are not |
| 3 | The processor-count section is circular and is described as a cross-check: `sysconf(_SC_NPROCESSORS_ONLN)` **is** the popcount of the `svcGetInfo` it is compared against, and `conf == onln` shares a `case` label | **Real, and the wording was the defect.** The checks stay — they catch the wiring breaking (query failure inside `sysconf`, a `_SC_` name reaching the `EINVAL` default, a later edit reporting something else) — but they are now labelled wiring checks in the test, in `compat/sysconf.c` and here, not evidence of a core count. The independent measurements in that section are the raw mask, its bounds, and `caps->nr_cpus`, which reached 1 regardless of the mask before patch 0012 |
| 4 | `os_time_get_absolute_timeout((uint64_t)INT64_MAX)` asserts a defined result for a signed-overflow path that `-O2` may fold away | **Not real against the pinned Mesa**, and Codex flagged it as needing confirmation there. Mesa 26.1.5's `os_time.c` does not add first: it uses `util_add_overflow(int64_t, time, timeout, &abs_timeout)`, which is `__builtin_add_overflow` here — `HAVE___BUILTIN_ADD_OVERFLOW` is in this build's compile args (`build/mesa-probe/build.ninja`) — and computes in infinite precision before deciding. The shape the finding describes (`abs_timeout = time + timeout; if (abs_timeout < time)`) is older upstream. Recorded in the test, with the guard's own case added as a second check, so a Mesa bump that restores it fails here rather than passing by accident |
| 5 | The resolution check's message says the opposite of the check: `min_step <= 1000000` printed as "clock resolution is at least 1 ms" | **Real.** That line is read by someone looking at a FAIL in a console log with no source to hand. Now "1 ms or finer" |
| 6 | "Every timed check is bounded from both sides" is not true of the code — the `PROMPT_MAX_MS` checks, `os_time_sleep(0)`, a past deadline and the `drift` check are one-sided | **Real.** Each is defensible on its own; the blanket claim was not. The header now separates the two: a call that must *expire* is bounded from both sides, a call that must *not wait* has no lower bound to assert and is bounded above only |
| 7 | Several bounds are loose enough to pass for the failure they name: `SLEEP_MIN_US = SLEEP_US / 2` accepts a 2× unit error, `os_time_sleep(0)` accepts 49 ms against a 50 ms sleep, `nanosleep_until(+50 ms)` accepts 25–200 ms — and `t_threads` uses −25 %/×4 for the same class of measurement with no explanation of the disagreement | **Real, and the sharpest of the documentation-shaped findings.** One convention now, stated once: −25 % below, ×4 above, in both files. The "must not wait" bound is 10 ms — a fifth of `SLEEP_US`, so it can no longer be satisfied by a call that slept the full requested amount, which is the one thing it existed to distinguish |
| 8 | Meson and Make detect Mesa's archives at different times and only one recovers: `fs.exists()` runs once at configure and is baked into `build.ninja`, so building Mesa afterwards leaves that path producing 11 `.nro` indefinitely | **Real, and measured before and after.** With the archives hidden, `configure-horizon.sh`, then the archives restored: the `build-horizon.sh` on the branch printed `11 .nro` on two consecutive runs. `configure-horizon.sh` now records what `fs.exists()` decided beside the build directory and `build-horizon.sh` compares it against the state on disk — same scenario: `Mesa archives are present, build/meson was configured with them absent; reconfiguring`, then `13 .nro`. Both directions verified |
| 9 | Four facts are restated in two build systems with a comment as the only enforcement, and the `.nro`-size parity check would not notice a define present on one path and missing on the other | **Real.** The duplication stays — each build system has to be readable on its own, and the Makefile is the hardware-verified path — but divergence now fails: `scripts/check-mesa-test-parity.sh` compares the test names, the two archive paths, the defines, the Mesa include directories and the default build directory across `Makefile`, `meson.build`, `meson.options` and `scripts/toolchain-env.sh`. Verified by breaking each of the five in turn plus the gate's own extraction — six deliberate breakages, six exit 1 |
| 10 | Link order is stated on one path and left to inference on the other | **Real, and the answer needed measuring rather than asserting.** The Makefile needs the archives *before* `-lhorizon_compat -lnx` because it is a plain left-to-right link. The Meson path does the opposite — a dependency's `link_args` go last — and links anyway because Meson wraps the whole set in `-Wl,--start-group … --end-group`, so the linker rescans. Read out of the generated `build.ninja` and recorded beside `idep_mesa_core`; the two paths are not expected to emit the same link line |
| 11 | `horizon_cross_id` hashes with no separator and discards `cat`'s exit status, so a renamed identity file degrades the hash back to the cross files alone — silently | **Real, and it fails open, which is the worst direction for a staleness check.** Every input is now verified to exist *before* the pipeline (a pipeline's status is `sha256sum`'s), and each file's path is hashed with its contents, so a line moved between two hashed files is no longer invisible |
| 12 | The extra-identity-file mechanism reproduces the coupling it was added to fix: two call sites must be handed the same list, with a comment as the only enforcement | **Real.** The list is given once, to `horizon_setup_mode`, and `horizon_record_cross_id` takes no arguments |
| 13 | `compat/sysconf.c` merges `_SC_NPROCESSORS_CONF` into `_ONLN` on an argued, unmeasured premise; `svcGetSystemInfo` is dismissed in prose | **Real, and the measurement was available.** libnx's `SystemInfoType` accepts exactly `TotalPhysicalMemorySize`, `UsedPhysicalMemorySize` and `InitialProcessIdRange` (`switch/kernel/svc.h:222-225`, read out of the pinned image). None is a processor count. The cost of sharing the case — a caller cannot distinguish "the SoC has four, you may use three" — is now stated rather than implied, as is why the SoC constant lives in the test and not in the C library function |
| 14 | Mutex operations go unchecked in the worker threads, and `section_mutex_timed`'s "the mutex is not left held either way" rests on one of them | **Real, and `CLAUDE.md` requires the check.** Every `mtx_*` return is checked now, in the workers too. A `cnd_worker` whose lock failed returns instead of proceeding — it would otherwise read the predicate unguarded, hand `cnd_wait` a mutex it does not own and race the counters. The main thread's locks go through one helper that still stores the predicate when the mutex call fails, because that store is what releases the waiters and lets the joins terminate; the lock before `cnd_timedwait` is the exception and ends the check, since calling it unlocked is undefined |
| 15 | `counter_worker` produces two failures for one defect — the same pattern `section_mutex_timed` explicitly apologises for | **Real.** The counter is now measured against what the workers actually performed. With no lock failure that is the same number as `created * INCREMENTS`, so nothing is weakened |
| 16 | The rate check consumes a possibly-backwards clock without consulting the monotonicity result it just computed; and `ref_ns / 100 * PCT` divides before multiplying | **Real.** One backwards pair makes the subtraction about 1.8e19, which the note printed as a measurement in nanoseconds while the check failed for an unstated reason. The interval is now tested first and the rate check is skipped with a note. The division order is fixed — harmless at 1e8 ns and still the wrong order in a file whose stated rule is to check every arithmetic step |
| 17 | The "refuted P1" is recorded as both refuted and fixed, and the round's headline count depends on the disposition the same row contradicts | **Real as a write-up defect; the disposition stands.** Correction written into the first round's section above: the configure-time conditionality creates no hole, because these two tests are built only when the archives exist, so any configuration where the edge could be missing is one where there is no edge. Citing it as a reason to change was the contradiction. The real defect in that neighbourhood is finding 8, and it is not about the link edge |
| 18 | Third-party line-number citations name no version, in a repository whose own rule is that every constant cites its source | **Real.** Both `ninjabackend.py` citations now name meson 1.11.2, the version `toolchain/versions.env` pins, and both were re-read at that version before the change |

### What this round says about the verification, not the code

- **A gate that has never failed has not been tested.** Finding 1 is the
  clearest case: the fix was correct for the input it was tried with. It
  is why every claim in this round's verification table below was
  produced by breaking the thing first — the parity gate was checked by
  six deliberate divergences, `make clean` by four spellings of the same
  directory, finding 8 by running the previous commit's script in the
  scenario that defeats it.
- **Six of eighteen are the tree describing itself wrongly**, and none
  of them would have failed a build: a message that says the opposite of
  its check, a claim of two-sided bounds that were one-sided, two checks
  offered as evidence they cannot be. These reach the owner's hands in a
  console log, which is exactly where there is no source to check them
  against.
- **The one finding that did not hold up was the one Codex hedged.**
  It asked for confirmation against the pinned checkout; the pinned
  checkout answered. That is the same discipline this project applies to
  its own patches, arriving from the other direction.

### A defect this round introduced and closed inside itself

Removing the two-call-site coupling of finding 12 broke the first run
outright:

```
$ scripts/configure-horizon.sh
Found ninja-1.11.1 at /usr/bin/ninja
error: horizon_record_cross_id before horizon_setup_mode
```

`horizon_setup_mode` remembered the identity file list in a shell
variable while its callers still ran it as `mode=$(horizon_setup_mode
…)` — a command substitution is a subshell, so every assignment was
discarded before the caller reached the next line. The mode is returned
in `$HORIZON_SETUP_MODE` now and there is no subshell. It failed loudly
rather than recording a wrong stamp, which is the behaviour finding 11
was about.

### Verification after the fixes

Every command was run from the repository root against
`ghcr.io/d3fau4/nx-dev:latest` (no local devkitA64). All of it is **cross
build (X)**. Nothing here is a hardware result.

| Check | Command | Result |
|---|---|---|
| `clean` keeps Mesa whatever the spelling | `MESA_BUILD_DIR=<4 spellings> make -n clean` | before: 3 of 4 deleted it; after: 4 of 4 keep it, and `build/mesa-alt` still does not keep `build/mesa-probe` |
| `make clean` keeps the toolchain | `scripts/build-switch.sh clean && ls build/` | `mesa-probe  mesa-probe.crossid  toolchain`, and `toolchain/` still holds `meson-1.11.2` and `python` |
| …and rebuilds to thirteen | `scripts/build-switch.sh -j4` | 13 `.nro` |
| Meson path after `clean` removed its directory | `scripts/build-horizon.sh` | configures and builds, `13 .nro` |
| Mesa built *after* configure, old script | `git show HEAD:scripts/build-horizon.sh`, twice | `11 .nro`, `11 .nro` — the defect |
| Mesa built *after* configure, new script | `scripts/build-horizon.sh` | `Mesa archives are present, build/meson was configured with them absent; reconfiguring` → `13 .nro` |
| Mesa hidden, both paths | `scripts/build-switch.sh`, `scripts/build-horizon.sh` | `removing stale Mesa test artefacts: …` → **11**; `Mesa archives are absent…` → **11** |
| Mesa restored, both paths | same | **13** and **13** |
| Parity gate detects divergence | 6 deliberate breakages | 6 × exit 1 with the right label, including "extracted nothing" when the gate's own extraction is broken |
| Parity gate on the tree | `scripts/check-mesa-test-parity.sh` | OK |
| Both tests compile `-Wall -Wextra -Werror` | `scripts/build-switch.sh` | clean, no diagnostics |
| Patch series on a reset `mesa/`, ×2 | `git -C mesa reset --hard $MESA_COMMIT && scripts/apply-mesa-patches.sh` | applies 12; second run `all 12 patches already applied` |
| `.nro` parity, Makefile vs Meson | `stat -c%s` over both directories | **13/13 identical sizes** |
| Host unit tests | `scripts/run-host-tests.sh` | **103/103** |
| Gates | `check-layering.sh`, `check-no-abs-paths.sh`, `check-rust-target.sh`, `check-mesa-test-parity.sh` | all OK |

One-time cost, recorded because someone will hit it: the identity format
changed, so the first `scripts/configure-mesa.sh` on an existing tree
sees a stamp it cannot match, treats the directory as changed and wipes
it — which rebuilds Mesa. That is the conservative direction and it
happens once.

### Still owed after this round — unchanged

`t_threads` and `t_ostime` have still never run on a console. This round
made three of their bounds able to fail for the defect they name and
made the rest of the file report one defect once; none of it is a
measurement of Horizon. **Both `.nro` changed again** — the hashes at the
end of the closeout section below are the ones to run.

---

## Phase 3 — closing items 1, 2, 4, 5 and 7 (2026-07-27)

No code in `horizon/` was touched. Everything in this section is **cross
build (X)** or **host (H)**. The two new `.nro` are **not** hardware
results and are not written up as if they were.

The session's method was the one the earlier rounds settled on: measure
first, and let the measurement decide whether there is a patch at all.
It produced **two** new patches out of five items, and the two it
produced were not the ones the milestone list would have suggested.

### Item 2 — Meson `host_machine.system() == 'horizon'`: no patch

The item asks for "handling". The measurement says there is nothing to
handle, and that is recorded here with the numbers rather than asserted.

`mesa/meson.build` mentions `host_machine.system()` **54 times**,
classified by parsing every occurrence rather than by eye:

| Form | Count | Operands |
|---|---|---|
| `== '<os>'` | 36 | windows 21, darwin 7, freebsd 2, gnu / cygwin / haiku / linux / sunos / openbsd 1 each |
| `!= '<os>'` | 7 | windows 6, netbsd 1 |
| list membership (`.contains(…)`, `in`) | 8 | lines 159, 195, 197, 281, 414, 1208, 1549, 2374 |
| inside an error message's `.format()` | 3 | lines 201, 286, 464 |

**Not one of them names an OS this port is.** Every comparison is
against somebody else's platform, so `horizon` falls to the `else` in
all 54 — and in all 54 the `else` is the answer a platform with no
KMS/DRM, no dynamic loader, no X11 and no Win32 should get:
`system_has_kms_drm` false, `with_dri_platform = 'none'`,
`HAVE_RENDERDOC_INTEGRATION=0`, `sys/sysctl.h` probed rather than
assumed absent, the `-Werror=format` and `-Werror=thread-safety` trials
enabled, `dependency('threads')` used, `nm` chosen for the symbols
check. The `_GNU_SOURCE` OS list at line 1208 is the one place the
`else` was wrong, and patch **0005** already replaced it with a probe.

Three sites can print the string `horizon`, all three inside
`error('Unknown OS @0@…')`, all three reached only when an option is
left at `auto`. Measured as a chain, each with a fresh build directory:

```
$ meson setup … build/probe/mesa-default mesa
mesa/meson.build:200:4: ERROR: Problem encountered: Unknown OS horizon.
  Please pass -Dgallium-drivers to set driver options.

$ meson setup … -Dgallium-drivers= …
mesa/meson.build:285:4: ERROR: … Please pass -Dvulkan-drivers …

$ meson setup … -Dgallium-drivers= -Dvulkan-drivers= …
mesa/meson.build:463:4: ERROR: … Please pass -Dplatforms …

$ meson setup … -Dgallium-drivers= -Dvulkan-drivers= -Dplatforms=
(no error)
```

That is Mesa saying "I have no default driver list for your OS, name
one", which is **correct** rather than merely different — and
`scripts/configure-mesa.sh` has passed all three explicitly since item 3,
for reasons already recorded. Phase 4 will pass
`-Dvulkan-drivers=nouveau`, equally explicitly. Adding an
`elif host_machine.system() == 'horizon'` there would encode a driver
and platform policy, not fix a defect.

The Phase 4 route has no branches at all:

```
$ grep -rn "host_machine.system()" mesa/src/nouveau/ mesa/src/vulkan/ \
      --include=meson.build
(no output, exit 1)
```

Across all of `mesa/src`, 22 `meson.build` files mention it; **five**
sites are inside the non-driver core this project builds, and each was
read:

| Site | Horizon takes | Correct? |
|---|---|---|
| `util/blake3/meson.build:8` | `is_windows = false` | yes — portable C, no MASM |
| `util/meson.build:417, 471, 502` | all three inside `if with_tests` | not reached; tests are off |
| `util/rust/meson.build:31` | not in `['linux','windows','darwin','macos']` → `rustix` **not** required | yes — a POSIX-syscall crate that does not support this target |
| `c11/impl/meson.build:12` | `threads_posix.c` | yes, and it is item 4's whole subject |

**One honest caveat.** `meson.build:22-31` gives Horizon
`libname_prefix = 'lib'`, `libname_suffix = 'so'`, which feeds
`icd_file_name = 'libvulkan_nouveau.so'` in
`src/nouveau/vulkan/meson.build:168`. On a platform with no dynamic
loader (`dlopen : NO`, `HAVE_DLOPEN` unset) that name describes a file
nothing will ever open — a Vulkan ICD manifest is a loader concept, and
NVK here will be linked into the application. It is *meaningless*, not
*wrong*, it costs nothing today, and deciding what it should say is a
Phase 4/6 question about how the driver is delivered. Recorded rather
than patched.

**Disposition: item 2 needs no patch (case (a)).** A patch here would be
decoration, and `mesa-patches/README.md` requires a measurement behind
every one.

### Item 4 — threads: keep `threads_posix.c`, and the defect that decided it

**Which implementation Mesa uses, re-measured.**

```
$ grep -c "HAVE_THRD_CREATE" build/mesa-probe/build.ninja
0
```

`mesa/meson.build:1619-1626` sets `with_c11_threads` only when
`with_platform_android`, so `src/c11/impl/meson.build:10-17` compiles
`threads_posix.c`. newlib's own `thrd_create` links
(`Checking for function "thrd_create" : YES`) and is not used.

**Should it be? No, and the reason is not a preference.** newlib defines
the whole C11 threads API in `libc.a(libc_a-threads.o)` — 25 symbols,
`thrd_*`, `mtx_*`, `cnd_*`, `tss_*`, `call_once`. Two of them are not
implementations:

```
$ aarch64-none-elf-objdump -d --disassemble=mtx_timedlock libc_a-threads.o
0000000000000000 <mtx_timedlock>:
   0:   52800040        mov     w0, #0x2      // thrd_error
   4:   d65f03c0        ret
```

An unconditional failure that never attempts the lock. And `mtx_init`
tests bit 2 of the type — `mtx_timed = 0x4` in newlib's `threads.h` —
and branches straight to the same `#2`, so a *timed mutex cannot even be
created*. Consistent with the object's undefined symbols: it references
`pthread_mutex_lock`, `_trylock` and `_unlock`, `nanosleep`,
`sched_yield` and the cond/key family, and **no timedlock of any kind**.

Mesa's `threads_posix.c` really implements the timeout, by polling
`mtx_trylock` — the path `mesa-patches/0003` turns on where
`pthread_mutex_timedlock` is absent. Both implementations sit on the
same `libsysbase` pthreads underneath, so switching would buy nothing
and would replace a working timed lock with `return thrd_error`.

The two are also not mixable: newlib's enumeration is
`thrd_busy = 1, thrd_error = 2, thrd_nomem = 3, thrd_success = 4,
thrd_timedout = 5`, Mesa's is `thrd_success = 0, thrd_timedout = 1, …`.
A translation unit that included the wrong header would compare against
the wrong constants and compile silently.

**Decision: keep `threads_posix.c`. No patch for the selection itself.**

**The defect the item did produce → patch 0011.** `util/u_thread.c`'s
`u_thread_create()` blocks every signal around `thrd_create()` under a
`defined(HAVE_PTHREAD)` guard. POSIX puts `pthread_sigmask` in the
signal option group, not in threads, and devkitA64 has all of pthreads
without it — so the file does not link, on the function `util/u_queue.c`
uses to create every worker thread. Link probes, run directly against
`switch.specs`:

| Probe | Result |
|---|---|
| `pthread_sigmask` | **undefined reference** |
| `sched_yield` | links |
| `clock_gettime` | links |
| `pthread_barrier_init` | links |
| `pthread_getcpuclockid` | links |

So it is one absent function, not a missing threading layer — which is
why the patch is a configure check (`HAVE_PTHREAD_SIGMASK`) and not an
OS branch. After it:

```
Checking for function "pthread_sigmask" with dependency threads: NO
$ aarch64-none-elf-nm -u …/u_thread.c.o | grep pthread_sigmask
(no output)
```

`tests/t_threads.c` is the console half, described below.

### Item 1 — OS detection: what `DETECT_OS_HORIZON` is for, and the audit

`DETECT_OS_HORIZON` is defined by patch 0007 and **referenced nowhere
else in Mesa**:

```
$ grep -rn "DETECT_OS_HORIZON" mesa/src/
src/util/detect_os.h:103:#define DETECT_OS_HORIZON 1
src/util/detect_os.h:135:#ifndef DETECT_OS_HORIZON
src/util/detect_os.h:136:#define DETECT_OS_HORIZON 0
```

That is deliberate and is the answer to "is it used everywhere it is
needed": an OS needs an *identity*, but every behavioural question is
answered by `DETECT_OS_POSIX_LITE`, which is a statement about the C
library and therefore upstreamable. `DETECT_OS_POSIX_LITE` defaults to
`DETECT_OS_POSIX`, so each such patch is additive for every existing
platform. It gates `os_time.c` (patch 0008) and now `u_cpu_detect.c`
(patch 0012).

**Finding the `#else` branches that assume Linux, by measurement rather
than by reading.** 21 of the 323 sources compiled into the core mention
`DETECT_OS_*`. Rather than judge each by eye, the whole core was audited
at the link level — a branch that assumes more C library than exists
shows up as a symbol nothing defines:

```
core undefined refs   : 1816
resolved inside core  : 1637
resolved by toolchain :  156   (libc, libm, libsysbase, libpthread,
                                libstdc++, libnx, portlibs, libgcc,
                                libhorizon_compat)
UNRESOLVED            :    7
```

| Symbol | Referenced by | Category |
|---|---|---|
| `pthread_sigmask` | `u_thread.c.o` | **fixed here — patch 0011** |
| `posix_memalign` | `sparse_array.c.o` | item 3 residue, and a **lying configure check** |
| `flock` | `mesa_cache_db.c.o` | item 3 residue |
| `getuid` | `anon_file.c.o`, `log.c.o`, `perf_u_trace.c.o` | item 3 residue |
| `geteuid`, `getgid`, `getegid` | `log.c.o`, `perf_u_trace.c.o` | item 3 residue |

**`posix_memalign` is the interesting one.** Configure says

```
Checking for function "posix_memalign" : YES
```

and `-DHAVE_POSIX_MEMALIGN` reaches 352 compile lines — while a direct
link probe answers `undefined reference to 'posix_memalign'`. GCC has a
`__builtin_posix_memalign`, so Meson's check compiles and links its own
snippet successfully. This is *exactly* the hazard `mesa/meson.build`
documents three lines above the check, for MinGW, and it applies here
too. It is a false configure answer of the same class as the `-Werror`
defect this project found in its own cross file in item 3.

**These six are recorded, not fixed.** They are item 3's category
(newlib/libnx gaps) and item 3 is closed; none of them blocks Phase 3's
criterion, because the criterion is that the core *builds* and a static
archive never resolves anything. They are what Phase 4 will meet at its
**first link**, and this is the list. Fixing them is a scoped piece of
work, not a drive-by.

**Three more things the audit surfaced, none of them a link failure:**

- `util/u_thread.c:92` emits `#warning Not sure how to call
  pthread_setname_np` — measured, it fires. Thread names are cosmetic
  and the branch is a no-op; recorded so it is not mistaken for a defect
  later.
- `util/u_process.c:186` emits `#pragma message ( "Warning: Per
  application configuration won't work with your OS version." )`. driconf
  per-application matching will not work; Mesa says so itself and
  degrades rather than failing.
- `util/rand_xor.c` takes the `!DETECT_OS_WINDOWS` path and calls
  `open("/dev/urandom")`, which cannot succeed here. The file's own
  fallback then seeds from a constant plus `time(NULL)`. Nothing is
  simulated and no rejected design is involved — the open simply fails —
  but the seed is weak. Horizon does have an entropy source
  (`InfoType_RandomEntropy`, libnx `randomGet`), so a `compat/getrandom`
  would make `HAVE_GETRANDOM` true and route Mesa to it. Deliberately
  **not** done in this session: it is item 3's category and out of scope.

**Item 1's own patch → 0012**, below.

### The known defect, fixed: `util_cpu_detect` reported 1 CPU

Recorded at the end of item 6 and carried since. `_util_cpu_detect_once`
counts processors under `#elif DETECT_OS_POSIX`, patch 0007 chose
POSIX-**lite**, so nothing set `available_cpus`, `nr_cpus` fell to
`MAX2(1, 0)` and `util/u_queue.c` would have sized its thread pool for a
single-core machine.

Two changes, and the split between them is a layering decision:

- **`mesa-patches/0012`** turns the outer guard and the `<unistd.h>`
  include into `DETECT_OS_POSIX_LITE`. Nothing inside the block needs
  the whole of POSIX — every path is already guarded by the macro naming
  what it uses (`HAS_SCHED_GETAFFINITY`, `_SC_NPROCESSORS_ONLN`,
  `_SC_NPROCESSORS_CONF`, `HW_NCPUONLINE`) — so this is the same shape
  as patch 0008 and changes no existing platform.
- **`compat/sysconf.c`** answers `_SC_NPROCESSORS_ONLN` and
  `_SC_NPROCESSORS_CONF` from `svcGetInfo(InfoType_CoreMask)`,
  "Bitmask of allowed Core IDs" (libnx `switch/kernel/svc.h:185`), whose
  population count is the set of cores the process may run on.

**Why the number comes from `compat/` and not from a Mesa patch:** the
only source for it is libnx, and `<switch.h>` inside Mesa's generic
`src/util` would break this project's layer rules and could never go
upstream. `sysconf` is already the door `CLAUDE.md` leaves open and the
cross file already links `libhorizon_compat` before `-lnx`. newlib
defines both names (`sys/unistd.h:370-371`, values 9 and 10) and defines
neither symbol, which is the same case as the three names `compat/`
already answered.

`_ONLN` and `_CONF` deliberately return the same value: Horizon exposes
no second, wider count to an ordinary process, and reporting the SoC's
four Cortex-A57s for `_CONF` would be a constant nobody measured. A zero
mask is treated as a failed query, since a running process must be
allowed at least one core.

Measured effect (cross build):

```
before:  $ aarch64-none-elf-nm u_cpu_detect.c.o | grep sysconf   →  (nothing)
after :  $ aarch64-none-elf-nm -u u_cpu_detect.c.o | grep sysconf →  U sysconf
```

`HAS_SCHED_GETAFFINITY` is absent (`sched_getaffinity : NO`), which is
why the `_SC_NPROCESSORS_ONLN` path is the one that had to be reachable.
**The value itself is not yet measured** — that needs `t_threads` on a
console.

### Item 5 — timers / clocks

`os_time.c` has compiled since patch 0008 and no line of it had run. Two
of its functions cannot be taken on trust from a compile:

- **`os_time_get_nano()` does not check its clock.** It calls
  `timespec_get(&ts, TIME_MONOTONIC)` and returns from `ts` regardless.
  On this platform `timespec_get` is Mesa's own `c23_timespec_get`
  (`src/c11/impl/time.c`), which forwards to
  `clock_gettime(CLOCK_MONOTONIC)` and, on failure, returns 0 **without
  touching `ts`** — so the caller gets uninitialised stack. newlib
  defines `CLOCK_MONOTONIC` as 4 (`time.h:278`) whether or not
  `libsysbase` honours it, and `clock_gettime` links, so nothing about
  this is visible before the code runs.
- **`os_time_sleep()` is `usleep()`** through the POSIX-lite branch
  patch 0008 added. A `usleep` that returns at once and one that sleeps
  ten times too long both "work".

`tests/t_ostime.c` measures both, plus resolution, rate,
`os_time_nanosleep_until` and `os_time_get_absolute_timeout`. **Item 5
stays a cross-build result until that runs.**

### Item 7 — endianness: closed on the cross build

This one needs no console: it is a compile-time property.

```
$ aarch64-none-elf-gcc -D__SWITCH__ -Imesa/src … -E -dM -x c mesa/src/util/u_endian.h
#define UTIL_ARCH_BIG_ENDIAN 0
#define UTIL_ARCH_LITTLE_ENDIAN 1
#define __BYTE_ORDER__ __ORDER_LITTLE_ENDIAN__

$ # and which branch answered:
$ echo '#include <endian.h>' | aarch64-none-elf-gcc -c -x c -
fatal error: endian.h: No such file or directory
```

A translation unit with
`_Static_assert(UTIL_ARCH_LITTLE_ENDIAN == 1)` and
`_Static_assert(UTIL_ARCH_BIG_ENDIAN == 0)` compiles clean under
`-Wall -Wextra -Werror`. `endian.h` is absent and no libc branch in
`u_endian.h` matches, so the answer came from patch 0004's
`__BYTE_ORDER__` fallback — the branch under test, not one of the
pre-existing ones. **Item 7 is closed (X).**

### The two new `.nro`, and why they link Mesa's archives

`tests/t_threads.c` (12) and `tests/t_ostime.c` (13) link
`build/mesa-probe/src/c11/impl/libmesa_util_c11.a` and
`.../src/util/libmesa_util.a` — **the archives Mesa's own build
produced**, not those sources recompiled with flags of ours. The object
under test has to be the object Mesa builds, or the measurement is about
a different build. The only two defines the test sources need are
`-DHAVE_PTHREAD` and `-DHAVE_STRUCT_TIMESPEC`, both Mesa's own configure
results here, both visible in `build/mesa-probe/build.ninja`.

Consequences accepted and handled:

- They need `scripts/configure-mesa.sh && scripts/build-mesa.sh` first.
  Both build paths **skip them with a message** when the archives are
  absent, so a bare clone still produces the eleven tests that need only
  the toolchain. `make` prints
  `skipping t_threads t_ostime — no Mesa archives in build/mesa-probe`.
- Tests 1–11 still build with no Mesa in sight; the Mesa include path
  and archives are target-specific in the `Makefile` and a separate
  dependency in `meson.build`.
- `libmesa_util.a` links cleanly into both, which also demonstrates the
  audit's finding from the other side: the six unresolved libc symbols
  live in objects (`log.c.o`, `anon_file.c.o`, `sparse_array.c.o`,
  `mesa_cache_db.c.o`) that these two tests do not pull in.

What `t_threads` checks: `thrd_create`/`thrd_join` including the
returned value, `u_thread_create` (the function patch 0011 changed),
`call_once` across four threads, a shared counter of 4 × 20 000
increments under a mutex, `mtx_trylock` on a held mutex,
**`mtx_timedlock` expiring**, `mtx_timedlock` on a free mutex returning
promptly, `cnd_wait`/`cnd_signal`, `cnd_broadcast` waking all four,
**`cnd_timedwait` expiring** (which `src/vulkan/runtime/vk_sync_timeline.c`
depends on in Phase 4), `tss_create`/`tss_set`/`tss_get` with a
cross-thread leak check and the destructor count, and the processor
count against the raw `InfoType_CoreMask`.

Every timed check is bounded **from both sides** — 150 ms ≤ elapsed ≤
800 ms for a 200 ms timeout — and timed with `armGetSystemTick()`, the
ARM system counter read directly. That is not the clock the code under
test uses; measuring a clock with itself proves nothing, and a polling
`mtx_timedlock` whose comparison is inverted returns `thrd_timedout`
immediately, which a one-sided check cannot tell from correct.

`t_check`/`t_note` write shared state, so they are called from the main
thread only; workers set plain per-thread fields read after the join
that orders them. Everything read *without* a join in between is
`atomic_int`: the `tss` destructor count several exiting threads
increment at once, the `done` flag the main thread polls instead of
joining the `mtx_timedlock` worker, the `outcome` the `cnd_timedwait`
waiter and its watchdog settle with one compare-exchange so that exactly
one of them ends the wait, and — added by the second review round — the
count of failed `mtx_lock`/`mtx_unlock` calls inside the condvar
workers, which is the one field a worker writes while *not* holding the
mutex, because not holding it is what it records.

The artefacts handed over, so a console log can be attributed to exactly
these builds (Makefile path, which is the reference path):

```
833d14ef5ac2d44c7ed981b44fc45f4b0b8412afb9765935103db1a1de1284d1  t_threads.nro
8c33bccfff69aff4f2768cdb0fc1f3c8babbd093a47f3d25c80768cd76e4810a  t_ostime.nro
```

These are the current builds, after the third review round. The
**passing** emulator runs of 2026-07-28 were `a58e2af8…` / `92899b59…`,
and those results stand as measurements of those binaries: the third
round changed how both tests behave when a call does *not* answer, which
is the case those runs did not reach. Older still, and not to be run:
`3f97f5d2…` / `ff999f01…` (the first emulator pair), `00d15baa…` (the
staged-probe `t_threads`), `0ca4b59f…` and `45e49f1e…`. What changed across
those builds is in the two review sections and the emulator section
above: no check has ever been removed, several were added, every wait on
a timed call is bounded, and the second review round tightened three
bounds that were loose enough to pass for the failure they name.

The Meson path produces the same **sizes** for all thirteen and
different sha256 for these two — the inter-object padding difference
recorded at the end of Phase 2 (≤32 bytes of `.bss`, ≤16 of `.text`,
from archive ordering), not a behavioural difference.

### Commands run and results

| Command | Class | Result |
|---|---|---|
| `scripts/fetch-mesa.sh` | H | `mesa-26.1.5`, HEAD verified `6a02618ccf6c…`, 503 MB |
| `scripts/apply-mesa-patches.sh` on a reset `mesa/`, twice | H | applies **12**; second run `all 12 patches already applied; nothing to do`, exit 0 |
| `scripts/apply-mesa-patches.sh --list` | H | 12 applied, 0 pending |
| `scripts/build-compat.sh` | X | rebuilds after the `sysconf` change |
| `scripts/configure-mesa.sh` | X | exit 0; `sysconf : YES`, `thrd_create : YES`, `sched_getaffinity : NO`, **`pthread_sigmask : NO`** |
| `scripts/build-mesa.sh` from a deleted `build/mesa-probe` | X | **379/379 edges, 0 FAILED, 10/10 libraries** |
| the same after `fetch-mesa.sh --force` + re-apply (end-to-end rerun) | X | 321/321 edges rebuilt, 0 FAILED, 10/10 |
| `meson setup` with default options, then adding one option at a time | X | the three `Unknown OS horizon` errors, in order, then success |
| `nm -u` audit of the ten core archives vs the whole toolchain | X | 1816 → 7 unresolved (table above) |
| Link probes for 9 libc/pthread functions | X | 4 absent, 5 present (table above) |
| `-E -dM` on `u_endian.h`; `_Static_assert` TU | X | `UTIL_ARCH_LITTLE_ENDIAN 1` / `BIG 0`; compiles under `-Werror` |
| `objdump -d` of newlib's `libc_a-threads.o` | X | `mtx_timedlock` is `mov w0,#2; ret` |
| `scripts/build-switch.sh all -j4` | X | **13 `.nro`** |
| `scripts/configure-horizon.sh && scripts/build-horizon.sh` | X | **13 `.nro`**, `tests : 13` |
| Makefile vs Meson, all thirteen | X | **identical sizes 13/13** |
| `scripts/run-host-tests.sh` | H | **103/103 PASS** (6 suites, unchanged) |
| `scripts/check-layering.sh` | H | OK |
| `scripts/check-no-abs-paths.sh` | H | OK |
| `scripts/check-rust-target.sh` | H | OK |

### Phase 3 exit criteria — state

| Criterion (`docs/milestones.md`) | State |
|---|---|
| Each item is a separate patch file with a header explaining it (X) | ✅ 12 patches, four-field header each. Items 2 and 8 carry **no** patch, each with the measurement that says none is warranted |
| Mesa configures for `horizon` and builds the non-driver core (X) | ✅ configure exit 0; **379/379 edges, 0 FAILED, 10/10 libraries** |
| No patch mixes functional change with formatting | ✅ |

### Milestone items — final disposition

| # | Item | Disposition |
|---|---|---|
| 1 | OS detection | patch 0007 (identity) + patch 0012 (CPU count). Audit above says where the `#else` still assumes more libc than exists |
| 2 | Meson `host_machine.system()` | **no patch** — 54 sites classified, all 54 correct in the `else`; the 3 that name the OS are `auto`-default errors we already pass options for |
| 3 | newlib/libnx gaps | patches 0001–0006. **Six link-time gaps remain**, listed above, deliberately not reopened |
| 4 | threads | patch 0003 + patch 0011; decision to keep `threads_posix.c` recorded with the disassembly behind it. `t_threads` **PASS 67/67 on an emulator** (Eden, 2026-07-28) — the polling `mtx_timedlock` and `cnd_timedwait` both land on their deadline, and the condvar wakeups are measured with the waiters confirmed inside the wait. Not yet run on a console |
| 5 | timers / clocks | patch 0008; `t_ostime` **PASS 43/43 on an emulator** (Eden, 2026-07-28) — 52–156 ns resolution, 0.08–0.09 % rate agreement with the ARM counter, every blocking call bounded. Not yet run on a console |
| 6 | physical memory / page size | `compat/sysconf.c` + patches 0009–0010, hardware-verified 2026-07-27 |
| 7 | endianness | patch 0004, **closed on the cross build** — it is a compile-time property |
| 8 | build ID | closed in Phase 2 without a patch: `-Wl,--build-id=sha1` is supported |

### What this session did NOT do, said plainly

- **`t_threads` and `t_ostime` have never run.** Items 4 and 5 are cross
  builds. Nothing about Mesa's threading or clocks on a console is
  claimed here, including the CPU count patch 0012 produces.
- **Six unresolved libc symbols are left in place** (`posix_memalign`,
  `flock`, `getuid`, `geteuid`, `getgid`, `getegid`). They will stop the
  first executable link in Phase 4 and they are listed with their
  objects so that is a task, not a surprise.
- **`rand_xor` seeds weakly on Horizon**, by Mesa's own documented
  fallback. A `compat/getrandom` is the fix and was not written.
- The `posix_memalign` configure check answers `YES` and is wrong. Not
  fixed here.

---

## Hardware run of all eleven `.nro`, both process modes (2026-07-27)

Owner-executed on a real Switch, **applet mode and full/game mode**,
logs received as `horizongputests.rar` (22 files, `sdmc:/horizon_gpu_tests/`).
This closes the re-run owed since Phase 1's second review round and is
the first console evidence for anything in Phase 3.

| # | Test | Applet | Full/game |
|---|---|---|---|
| 1 | `t_init` | **PASS 22/22** | **PASS 22/22** |
| 2 | `t_alloc` | **PASS 21/21** | **PASS 21/21** |
| 3 | `t_nvmap` | **PASS 16/16** | **PASS 16/16** |
| 4 | `t_va_reserve` | **PASS 18/18** | **PASS 18/18** |
| 5 | `t_map` | **PASS 28/28** | **PASS 28/28** |
| 6 | `t_channel` | **PASS 17/17** | **PASS 17/17** |
| 7 | `t_submit` | **PASS 30/30** | **PASS 30/30** |
| 8 | `t_syncpt` | **PASS 48/48** | **PASS 48/48** |
| 9 | `t_fence_wait` | **PASS 14/14** | **PASS 14/14** |
| 10 | `t_teardown` | **PASS 34/34** | **PASS 32/32** |
| 11 | `t_sysinfo` | FAIL 18/19 → **PASS 21/21** | FAIL 18/19 → **PASS 21/21** |

**Eleven of eleven pass in both modes, on the current code**, after one
fix to the test itself (below). The first run had ten. The counts are
higher than the Phase 1 run (`t_map` 26→28, `t_submit` 23→30,
`t_va_reserve` 17→18, `t_teardown` 28→34/32) because the second review
round added assertions; those are the very tests whose paths it changed.
`t_teardown`'s different totals between modes are the documented
in-flight-destroy race taking its two legal branches — both PASS.

`t_init` and `t_map` are **byte-identical between modes**, which is what
you want from a device-query test.

### Second run: `t_sysinfo` PASS 21/21 in both modes — everything closed

The fixed probe was re-run the same day. Both modes:

```
ok   unmapped the 0x1000 probe (rc=0x00000000)
note svcMapMemory(0x1000): OK
ok   the kernel mapped at least one size up to 0x10000
ok   smallest granularity the kernel accepts is 0x1000, and sysconf reports 0x1000
RESULT: PASS (21/21)
```

The **first** rung mapped. So the page size is now bounded from *both*
sides by measurement: no region boundary falls off `0x1000` (upper), and
the kernel accepts a map of exactly `0x1000` (lower). `compat/sysconf.c`'s
cited constant is confirmed, not merely consistent. 21 checks rather
than 19 because a successful ladder adds the unmap and the equality.

**Eleven of eleven now PASS on hardware, in both process modes.**

The memory figures **reproduce exactly** across the two independent
runs — `total`, `used` and `_SC_AVPHYS_PAGES` byte-identical in each
mode (394 MiB / 35214 pages applet; 3189 MiB / 995 pages full-game), so
they are deterministic per mode rather than sampling noise. Only the
region base addresses move, which is ASLR doing its job; the `aslr`
region base itself is `0x8000000` in every run.

### The first run's one failure, and why it was not what it looked like

`t_sysinfo` fails exactly one check, identically in both modes: the
`svcMapMemory` granularity ladder never mapped anything. Every rung
returned `0x0000dc01`.

Decoded against libnx's `result.h`: module 1 (kernel), description 110 =
**`KernelError_InvalidMemoryRange`** — *not* `KernelError_InvalidSize`
(101), which is what a granularity refusal would be. So the kernel was
objecting to *where* the probe tried to map, never to the size.

The cause is a bug in the probe: it took its destination from
`virtmemFindAslr`, and `svcMapMemory` only accepts the **stack** region
— it is documented as "mainly used for adding guard pages around stack",
and libnx uses `virtmemFindStack` for exactly this call. Fixed; the
probe's diagnostics now name the kernel description instead of printing
only hex.

**The ladder design did its job.** The test reported
`granularity NOT measured: no rung mapped, so the cause is something
other than the page size` rather than failing the page-size check — which
is precisely the self-diagnosis it was built for after the Codex review.
A single-size probe would have looked like "the page size is wrong".

### What was measured, as opposed to reasoned

**The page size is 0x1000, upper bound confirmed.** `sysconf(_SC_PAGESIZE)`
returns 4096, and every region the kernel reports is page-aligned and a
whole number of pages of it, in both modes:

| Region | Applet | Full/game |
|---|---|---|
| heap | `0x11ec600000` + `0x200000000` | `0x695de00000` + `0x200000000` |
| alias | `0x4d4a200000` + `0x1000000000` | `0x42bce00000` + `0x1000000000` |
| aslr | `0x8000000` + `0x7ff8000000` | `0x8000000` + `0x7ff8000000` |
| stack | `0x3469000000` + `0x80000000` | `0x5a68e00000` + `0x80000000` |

**And confirmed from below**, by the second run: `svcMapMemory` accepts
a map of exactly `0x1000`, the ladder's first rung. Both bounds are now
measurements.

**The mode-aware argument is confirmed, and by a wide margin.** This is
the reasoning `compat/sysconf.c` was written on, now measured:

| | Applet | Full/game | Ratio |
|---|---|---|---|
| `InfoType_TotalMemorySize` | `0x18aab000` = **394 MiB** | `0xc7500000` = **3189 MiB** | **8.1×** |
| `_SC_PHYS_PAGES` | 101 035 | 816 384 | |
| `_SC_AVPHYS_PAGES` | 35 214 (137 MiB free) | 995 (3 MiB free) | |

A hardcoded "4 GiB" would have been wrong by more than 8× in applet
mode, and `_SC_PHYS_PAGES × page size` equals the raw
`InfoType_TotalMemorySize` exactly in both. This is the decision from
"Phase 3 — item 6" holding up against hardware rather than against an
argument.

It also shows why the **zero-available fix from the Codex review
matters**: in full/game mode the console reported 995 free pages, 3 MiB.
A process starting slightly later reports zero, and the pre-fix code
would have called that a failed query and made NVK log that it could not
read the budget.

### Other measurements worth keeping

- GM20B identical in both modes: `chip='gm20b' arch=0x120 impl=0xb
  rev=0xa1`, 1 GPC × 2 TPC, L2 `0x40000`, `va_bits=40`,
  `big_page=0x20000`, `compression_page=0x20000`, classes 3d `0xb197` /
  compute `0xb1c0` / 2d `0x902d` / gpfifo `0xb06f` / i2m `0xa140` /
  copy `0xb0b5`.
- VA regions: small `base=0x8000000 pages=0x3f7fff page=0x1000`, big
  `base=0x400000000 pages=0xdffff page=0x20000`.
- **R5 again**: syncpoint id 26, value 68680 at channel create, 71046 at
  `t_syncpt` start — counters still not reset per channel, third
  independent confirmation that the shadow-from-read design is required.
- **Async submission still holds**: both submits issued in **149 µs**
  with no intervening CPU wait (Phase 1 measured 148 µs).

### Consequences

- **Phase 1's two hardware exit criteria go back to ✅** — measured on
  the current code, not on `732b58c`. See that table below.
- `compat/sysconf.c` is hardware-verified in full, after the probe fix.
- **Nothing is owed on hardware.** `compat/sysconf.c` is verified end to
  end: the page size it reports is bounded from above by every region
  the kernel exposes and from below by the smallest map the kernel
  accepts, and both memory figures match the raw syscall exactly in two
  process modes that differ by 8×.

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

- ~~**`t_sysinfo` has not run on hardware.**~~ **Closed 2026-07-27:**
  PASS 21/21 in both process modes. It bounds the page size from both
  sides now — the earlier claim that divisibility alone caught an
  understated page size was wrong, and the lower bound it was missing is
  measured (`svcMapMemory` accepts exactly `0x1000`).
- ~~**`util_cpu_detect` reports 1 CPU on Horizon.**~~ **Fixed
  2026-07-27** by `mesa-patches/0012` plus `_SC_NPROCESSORS_ONLN` /
  `_SC_NPROCESSORS_CONF` in `compat/sysconf.c`; `u_cpu_detect.c.o` now
  references `sysconf`. The value it produces is **not yet measured on a
  console** — that is what `t_threads` is for. See "Phase 3 — closing
  items 1, 2, 4, 5 and 7".
- ~~Milestone items 2 and 4 are untouched; items 1, 5 and 7 still carry
  only the minimum each needed.~~ **Closed 2026-07-27**, in the same
  section: item 2 with a measurement saying no patch is warranted, item
  4 with a decision and patch 0011, items 1, 5 and 7 with evidence and
  (for 1) patch 0012.

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
| Tests 1–10 pass on hardware (HW) | ✅ **all ten PASS, on the current code, in both process modes** (2026-07-27 — see "Hardware run of all eleven `.nro`" above). Applet and full/game agree test for test; the counts are higher than the Phase 1 run because the second review round added assertions to exactly these paths |
| ≥2 submits in flight without CPU wait (test 7) | ✅ **re-measured on hardware**: both submits issued in **149 µs** with no intervening CPU wait, and the bound is now `t_check`ed rather than noted (148 µs at `732b58c`) |

**Every Phase 1 exit criterion is met, and the two hardware ones are no
longer stale.** They were measured at `732b58c` and `horizon/` changed
afterwards in `747b915`, which is why they sat at ⚠️ through Phases 2
and 3 — a ✅ would have claimed console evidence for code no console had
run. The 2026-07-27 run closes that: it exercised the current code,
including every path the second review round touched (channel
create/destroy, `vm_map`, device big-page handling, the GPFIFO
emitters), and found no regression in either mode.

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
| D4 | Switch available | **yes — closed.** Full run, confirmation re-run, and the Phase 4 hardware run all done |
| D2 | Mesa version to pin | **closed at Phase 2 start: `mesa-26.1.5`** @ `6a02618ccf6c5651ecb9cccbde571eb61fd73592` |
| D3 | Mesa checkout mechanism | **closed at Phase 2 start: script-fetched**, not a submodule |
| D5 | Cache policy per memory type | blocked on R6 (first GPU write) |
| D6 | Timeline semaphores vs upload queue | Phase 4 |
| D7 | Report the devkitA64 TLS miscompile upstream | **open** — `-mtp=soft -fPIC` generates a TLS access with no relocation on gcc 15.2.0 (see the section on it). This tree no longer triggers it, so nothing here is blocked; a four-line reproducer exists and devkitPro should have it |
| D8 | Whether `CLOCK_MONOTONIC` here may be relied on as monotonic | **closed by design (items 6-10)** — it may not, so the absolute deadline is converted to a relative duration exactly once, at the start of the wait, and horizon_gpu waits on libnx's monotonic ticks. A date change cannot move a deadline that no longer exists. Was: — `t_ostime` measured `TIME_MONOTONIC` returning wall-clock time (epoch seconds), so it is the real-time clock. Monotonic across both measured intervals; a date change would step it. `vk_sync` waits take *absolute* timeouts built from `os_time_get_absolute_timeout`, so this has to be answered before the sync type is designed |
| D9 | `nvkmd` pdev/dev split: one `horizon_gpu_device` serving both, or GM20B facts queryable without a device | **closed by the hardware (items 1-2)** — the `nv` session is per process and the GM20B characteristics are only queryable once it is up, so there is no describing the device without opening it. The pdev owns the `horizon_gpu_device`; every `nvkmd_dev` shares it under a reference count. Was: — Phase 4 item 1. nouveau opens the render node twice (`_pdev.c:70`, `_dev.c:40`); Horizon's `nv` session is per process |
| D10 | The four chipset-derived `nv_device_info` fields (`sm`, `mp_per_tpc`, `max_warps_per_mp`, shared-memory sizes) | **closed: moved upstream (items 1-2)** — they are now in `src/nouveau/headers/nv_device_info_chipset.c`, next to the struct they fill, unchanged, and `nouveau_device.c` calls them. Was: — Phase 4 item 2. They are pure functions of the chipset living in `src/nouveau/winsys/nouveau_device.c`, which Horizon does not build: duplicate them into `nvkmd_horizon`, or move them upstream next to `nv_device_info.h`. They describe the chip, not the kernel driver |
| D11 | `vk_sync` type: Horizon-native over syncpoints, or the runtime's `vk_sync_timeline` emulation | **closed: native (items 6-10)** — a binary vk_sync over a channel fence, with the runtime's timeline emulation on top. The emulation needs a binary type underneath regardless, and a syncpoint fence is what a submit produces. Was: — Phase 4 item 8, and the largest single piece of the phase. The native route needs a CPU-side syncpoint increment (`nvioctlNvhostCtrl_SyncptIncr`) that `horizon_gpu` does not expose, and an owner for a syncpoint no channel created. Depends on D8 |
| D12 | Sparse binding: implement the bind context, or add a kmd capability and turn the feature off | **open** — Phase 4 item 6. `sparseBinding` is `cls_eng3d >= MAXWELL_B` and GM20B's queried 3D class is `0xb197` = MAXWELL_B, so NVK advertises it on this chip unless the condition changes |
| D14 | An uncached memory policy in `horizon/` | **open, raised with the owner; does NOT block Phase 4** (traced: the fill path touches no COHERENT allocation, and vkCmdFillBuffer is DMA-engine methods, not a shader) — Vulkan requires a `HOST_VISIBLE + HOST_COHERENT` memory type; NVK advertises one on SoC and means an *uncached* map by it; `horizon_gpu` offers only `HORIZON_GPU_MEM_CACHED`, and `NVKMD_MEM_COHERENT` makes nvkmd skip cache maintenance entirely. The mechanism exists (`svcSetMemoryAttribute` + `MemoryAttribute_Uncached`, as deko3d does) but it means touching `horizon/` |
| D13 | Where the single `#[global_allocator]` and `#[panic_handler]` live | **closed by measurement (step 4)** — they cannot live in both NAK and NIL: two `no_std` Rust staticlibs fail to link with `multiple definition of `__rust_alloc`` and four more. NAK and NIL become rlibs; one new staticlib links both and carries the pair |

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

## The degraded-baseline opt-in (2026-07-28)

The emulator stops at `horizon_gpu_channel_create`, so steps 5–9 of the
mandatory sequence — buffer, allocation, bind, fill, submit, wait,
readback — have never been executed anywhere, and the next console run
would be the first time that code runs at all. This adds the opt-in
recorded as the previous next task, so that code can be shaken out
before the console round trip rather than during it.

Shape, following the rule that nothing degrades by default:

- `horizon_gpu_device_create_info::allow_untrusted_syncpt_baseline`, or
  `HORIZON_GPU_UNTRUSTED_SYNCPT_BASELINE=1`. Off unless asked for by
  name, exactly like `NVK_I_WANT_A_BROKEN_VULKAN_DRIVER`.
- With it, a failed initial `SyncptRead` no longer fails channel
  creation; the shadow baseline becomes 0 and is marked untrusted.
  `horizon_gpu_channel_syncpt_baseline_trusted()` and
  `horizon_gpu_device_untrusted_syncpt_seen()` report it, and both the
  opt-in and each degraded channel are logged at `ERROR` level.
- `t_vulkan` decides at run time: its bare-channel probe now doubles as
  the platform test. The read works → nothing is enabled, the run is a
  normal one. The read fails → the test enables the mode, says so, and
  **fails the run at the end whatever the readback said**, because a
  fence built on a baseline nobody measured can report "reached" without
  the GPU having done anything. One `.nro`, and the real-hardware path
  is bit-for-bit the previous behaviour.

Documented as `docs/synchronization.md` § 9. No Mesa patch was needed:
NVK's channels inherit it from the device.

What this does **not** do: it does not let the emulator satisfy
anything. `horizon_gpu_fence_wait` reads the same syncpoint, so a
degraded run is expected to fail at `vkWaitForFences` instead of at
`vkCreateDevice` — four more steps of coverage, and an honest failure.

Tested: host tests 103/103, cross build, six gates, series re-applied
twice from the pinned base. **Not run on hardware or emulator yet** —
the artefacts are in `build/pkg`.

## `t_sysinfo` 18/19 on hardware — the binary was stale (2026-07-28)

The 2026-07-27 console logs fail exactly one check in both process
modes: every rung of the `svcMapMemory` ladder is rejected with
`0x0000dc01`. That is not a new failure and it needs no further console
time to explain — it is the **already-fixed** one, run again from an
older `.nro`.

The proof is in the log's own text:

```
note svcMapMemory(0x1000): rejected 0x0000dc01
```

Commit `1857448` (2026-07-27, *"tests: map the granularity probe into
the stack region"*) changed that probe to use `virtmemFindStack` instead
of `virtmemFindAslr`, and added a decoder that appends the kernel
description to exactly that line — `0xdc01` is kernel description 110,
`KernelError_InvalidMemoryRange`, i.e. wrong region, not a granularity
refusal. The logged line carries no decoded suffix, and the current
source cannot print it without one. So the `.nro` that produced this log
predates the fix.

Status: the fix exists in the tree and has **never been run on
hardware**. `t_sysinfo.nro` travels in the next package; if the region
was the whole story it goes to 19/19, and if it does not, the new line
names the kernel description and that is a different investigation with
a real starting point.

## D14 closed on the build side — `t_uncached` (2026-07-28)

D14's remaining debt was a hardware test for the UNCACHED policy.
`tests/t_uncached.c` is that test, registered in both build paths. It
asks the three questions the kernel alone can answer, separately, so a
failure names which:

1. **Does `svcSetMemoryAttribute` accept our allocations?** A CACHED
   allocation of identical size and alignment runs first as the control,
   so an allocator problem cannot be read as a D14 problem.
2. **Is the result usable by ordinary C?** Uncached on AArch64 is
   Normal-NC if the kernel is generous and Device-nGnRE if it is not,
   and multi-register accesses to Device memory fault. A dword loop and
   a `memcpy` are the check, preceded by a note — if the log stops
   there, that *is* the answer.
3. **Do CPU writes reach the GPU with no cache maintenance?** The test
   builds a NOP command list in uncached memory, **never calls
   `horizon_gpu_mem_flush`**, submits and waits. Completion means the
   GPU read what the CPU wrote. The missing flush is the test, not an
   oversight, and it needs no new engine emitters — the list the GPU
   fetches is the CPU write being measured.

Teardown is checked too: a second uncached allocation after the first is
freed would notice a `destroy` that forgot to clear the attribute.

Tested: cross build clean under `-Werror`, six gates, host tests
103/103. **Never executed** — it is a hardware measurement.

## Audit of the never-executed path — one crash found (2026-07-28)

Steps 5–9 of the mandatory sequence have never run anywhere, so the
console run would have been their first execution. Reading them first
was worth one round trip, and it found a null dereference that would
have ended the run inside `vkCreateDevice` with no log line at all.

**The defect.** `nvkmd_horizon_alloc_mem` never filled
`nvkmd_mem::va`. NVK reads that field directly and not as a special
case: `nvk_CreateBuffer` allocates a VA for a buffer only when it is
sparse or capture/replay (`nvk_buffer.c:119-121`), so every ordinary
buffer takes the memory's —

```
buffer->vk.device_address = mem->mem->va->addr + memoryOffset;
                                 -- nvk_buffer.c:271
```

— and it is not confined to `vkBindBufferMemory` either.
`nvk_device.c:295` reads `dev->zero_page->va->addr` while
`vkCreateDevice` is still running, and `nvk_cmd_buffer.c:195` and
`:261` read it for every push and every upload. `nvkmd_mem_init`
(`nvkmd.c:66-83`) zeroes the field and nothing in nvkmd core fills it;
the nouveau backend allocates the VA and binds the memory inside its
own `alloc_mem` (`nvkmd_nouveau_mem.c:56-66`). Patch 0031 does the
same here, and frees the VA before the memory because releasing a
reservation unmaps what is bound in it while `horizon_gpu_mem_destroy`
refuses with `ERR_BUSY` for exactly that reason.

**What else the audit checked, and cleared:**

- `vkCmdFillBuffer` needs no shader: it drives the NV90B5 copy engine
  (`nvk_cmd_copy.c:897-957`), so NAK is not on this path.
- NVK's subchannel table (`nv_push.h:68-96`) is 0=3D, 1=compute,
  2=NV9039, 3=NV902D, 4=NV90B5 — identical to
  `horizon_cmds_set_objects`. And NVK does not emit the binds itself;
  `nvk_queue_subchannels_from_engines` is annotated *"these line up with
  nouveau_ws_context_create"*, i.e. on nouveau the kernel does it. Ours
  is the in-stream replacement (R7), and `t_submit` has it passing on
  console: *"SET_OBJECT list completed without fault"*, *"no error
  notification after engine binds"*.
- `nvkmd_horizon_ctx_wait` passes a NULL `vk_device` into the sync
  type's `wait()`. Safe: `__vk_log_impl` returns early on a NULL
  instance when `MESA_DEBUG=0`, which this build is
  (`-DMESA_DEBUG=0`, confirmed in `build.ninja`). It does mean the
  error text is dropped — a diagnostics gap, not a fault.
- `horizon_gpu_channel_wait_fence` reads the syncpoint before testing
  the deadline, so `vkWaitForFences(timeout=0)` polls rather than
  reporting a false timeout.

**The submit path, checked against nouveau's.** `nvkmd_ctx_exec`
carries a per-entry `incomplete` flag that this backend ignores. That
is correct, and now verified rather than assumed: nouveau uses it only
to decide when to flush its batched push array, so that an incomplete
run is never split across two ioctls
(`nvkmd_nouveau_ctx.c:179-195`) — it never reaches a GPFIFO entry, and
only `no_prefetch` becomes a flag. This backend submits every entry of
a call in one `horizon_gpu_submit`, so the run cannot be split and
there is nothing to honour.

The same reading did find a half-done guard. `nvkmd_horizon_ctx_exec`
refuses a push whose byte count is not a multiple of four, with a
comment about silent truncation — but an entry also carries its address
with the low two bits unavailable and its length as a 21-bit dword
count, so a misaligned address or a push of 8 MiB or more truncates the
same way through two other doors. nouveau asserts both
(`nvkmd_nouveau_ctx.c:198-199`); patch 0032 refuses both, because an
assert is enough where the values go on to a validating kernel and this
is not that. Neither is reachable from NVK today — push memory is
dword-aligned by construction and command-buffer chunks are far below
8 MiB — so this closes the class, not an instance.

**A second live defect: `nonCoherentAtomSize`.** `nvkmd_pdev_info::
nc_atom_size_B` was filled from the services' compression page size. It
is a CPU cache-maintenance granularity, not a GPU one:
`vkFlushMappedMemoryRanges` and `vkInvalidateMappedMemoryRanges` run
`dc cvac` / `dc civac`, so it has to be the cache write-back granule.
`cache_ops.h:65` says so outright — *"Vulkan drivers should return this
as nonCoherentAtomSize"* — and nouveau uses `util_cache_granularity()`
(`nvkmd_nouveau_pdev.c:113`).

Not merely imprecise: GM20B reports `compression_page=0x20000`
(measured, `t_init` on console, 2026-07-27), and
`nvk_device_memory.c:461` rounds a flush range **up** to this number
before handing it to the cache ops. A 4 KiB allocation flushed with
`VK_WHOLE_SIZE` therefore ran `dc civac` across 128 KiB — 124 KiB past
the end of the object, a data abort if the pages beyond it are not
mapped and silent maintenance of unrelated memory if they are. And it is
on the mandatory sequence's own path: `t_vulkan` flushes its poison and
invalidates before the readback, both with `VK_WHOLE_SIZE`. Patch 0033.

Also settled while reading it: `t_vulkan` picks the first HOST_VISIBLE
memory type, and NVK lists cached first on an SoC
(`nvk_physical_device.c:1581-1591`), so the mandatory sequence exercises
the **cached** path with explicit maintenance — D5's mechanism. D14's
uncached type is type 1 and `t_vulkan` never touches it; `t_uncached` is
its only coverage. Worth knowing before reading either log.

**One latent hazard found and deliberately not fixed.** Of the fifteen
ops `nvkmd.c` dispatches, this backend leaves two NULL:
`alloc_tiled_mem` and `import_dma_buf`. Neither is on the Phase 4 path
— `vkAllocateMemory` takes the tiled branch only when
`pte_kind != 0 || tile_mode != 0` (`nvk_device_memory.c:227`), and
`t_vulkan` allocates plain buffer memory — but they differ in how safe
that is:

- `import_dma_buf` is genuinely unreachable: it needs an import-fd
  struct, which needs an external-memory extension this driver does not
  advertise.
- `alloc_tiled_mem` is **not** guarded. `nvkmd_dev_alloc_tiled_mem`
  (`nvkmd.c:118`) calls it through the pointer with no check, and any
  tiled image reaches it through ordinary Vulkan with no extension
  involved (`nvk_cmd_draw.c:1055`, `nvk_device_memory.c:228`). It is a
  null call waiting for Phase 5.

Left as it is on purpose: a stub returning an error would be a Phase 5
design decision taken early, and Phase 5 is where tiled memory gets its
real answer. Recorded here so it is found by reading rather than by
crashing.

Tested: cross build, six gates, host tests 103/103, series re-applied
twice from the pinned base (33 patches). Not executed.

## Hardware run, 2026-07-28 — four tests, and the first real Vulkan fault

Four logs from the owner's console, this build.

| Test | Result |
|---|---|
| `t_uncached` | **PASS 19/19** — D14 holds on hardware |
| `t_threads` | **PASS 67/67** |
| `t_ostime` | **PASS 43/43** |
| `t_vulkan` | FAIL 36/37, aborted at `vkCreateDevice -> -13` |

**D14 is closed on hardware.** The uncached policy answered all three
questions it was built to ask: `svcSetMemoryAttribute` accepts our
reservations; the resulting memory is usable with ordinary C — `memcpy`
round-trips, so Horizon gives Normal-NC and not Device, and the crash
this test warned about did not happen; and the GPU read a 64-pair NOP
list written by the CPU **with no cache maintenance at all** and
completed it. That is the whole property `NVKMD_MEM_COHERENT` needs.

`t_threads` and `t_ostime` were owed from Phase 3 on hardware rather
than on the emulator. Both paid.

### The syncpoint question is settled

```
[horizon_gpu:I] channel 0x3df8f20050: created, syncpt id=26 (live channels now 1)
[horizon_gpu:I] channel 0x3df8f20050: up, syncpt=26 initial=77282 zcull=off
  ok   probe: the syncpoint baseline is readable (id=26, value=77282)
```

The degraded mode did not engage, as designed. NVK's own channel came up
on the same syncpoint. Everything the emulator blamed on the driver was
the emulator.

### `vkCreateDevice -> -13`, and why the log did not say why

The only Mesa output was two lines that turned out to be noise:

```
MESA: error: nvkmd_horizon: VA 0x80fa000 freed with 0x10000 still bound at +0x0
MESA: error: nvkmd_horizon: VA 0x80f9000 freed with 0x1000 still bound at +0x0
```

Freeing a bound VA is nvkmd's ordinary path — `nvkmd_horizon_mem_free`
releases the VA its object owns without unbinding, and
`nvk_mem_arena_finish` states it outright ("Freeing the VA will unbind
all the memory"). `nvkmd_nouveau_mem_free` does the same. Logging it as
a driver bug was wrong, and it was actively harmful: it was the only
thing printed, so it read as the cause.

The real failure was silent because **every `vk_errorf` in this build
is**. `vk_log.c` returns before printing unless the instance has debug
logging on or a messenger is registered, and the `#if !MESA_DEBUG` guard
is compiled in at `--buildtype=plain`. `enable_debug_logging` is set by
the Intel drivers and nobody else.

### Diagnosis, from the numbers alone

`nvkmd_horizon_alloc_va` chose the big-page half whenever the
reservation's own size and alignment were multiples of the big page.
That reasons about the wrong object. `horizon_gpu_vm_map` applies the
reservation's page size to **each mapping inside it** — both offsets
aligned to it, size rounded up to it and still having to fit the memory
object — and a reservation is not always bound as one piece.
`nvk_mem_arena` binds `NVK_MEM_ARENA_MIN_SIZE` (64 KiB) chunks at 64 KiB
offsets into a contiguous VA that is megabytes wide.

So `dev->images` asked for a contiguous VA of `1024 * 1024 *
sizeof(nil_descriptor)`, which met the old condition and landed in the
big-page half (`as_big_page = 0x20000`, from the log). Its first chunk,
`nvk_mem_arena_mem_size_B(0) = 0x10000`, was bound at offset 0;
`horizon_gpu_vm_map` rounded `0x10000` up to `0x20000`;
`horizon_range_fits_u64(0, 0x20000, 0x10000)` is false →
`HORIZON_GPU_ERR_OVERFLOW` → `nvkmd_horizon_result` default →
`VK_ERROR_UNKNOWN` = **-13**.

Every number in the log is accounted for: exactly two memory objects
alive at the failure (zero_page `0x1000` at 0x80f9000, arena chunk
`0x10000` at 0x80fa000), freed in reverse order, and 0x80fa000 is not
`0x20000`-aligned because it is the chunk's own GART VA, not the
arena's.

### Fixed (patch 0034)

- The small-page half is now the default; the reservation's size and
  alignment no longer decide. The block-linear exception stays, because
  the Maxwell MMU only fills those kinds in big pages.
- Freeing a bound VA is silent again.
- `t_vulkan` registers a `VK_EXT_debug_utils` messenger, chained into
  `VkInstanceCreateInfo::pNext` **and** created as an object, so the two
  windows are both covered. This is the API's own answer to "tell me
  why", it costs the test one struct, and Mesa stays untouched. Finding
  this defect cost a full console round trip that a messenger would have
  answered in one line.

**Known risk, recorded not guessed at.** The block-linear path inherits
the same rounding hazard: a tiled image whose memory object is not a
multiple of the big page will have its bind size rounded past the end of
the object and fail. The fix is to round the *allocation* up where the
kind is block-linear, which is `nvkmd_horizon_mem`'s decision. Untested
and untouched because Phase 4's sequence binds no images.

Tested: host 103/103, cross build, six gates, series re-applied twice
from the pinned base. **Not yet run on hardware.**

## Hardware run, 2026-07-28 (second) — `vkCreateDevice` passes, the GPU faults

The page-half fix worked. `vkCreateDevice -> 0`, and the sequence gets
eleven steps further before stopping:

```
  ok   probe: the syncpoint baseline is readable (id=26, value=92772)
  ok   vkCreateDebugUtilsMessengerEXT -> 0 (driver errors will be reported below)
[horizon_gpu:I] channel 0x274d9296e0: up, syncpt=26 initial=92772 zcull=off
[horizon_gpu:I] channel 0x274d969020: up, syncpt=27 initial=2 zcull=off
  ok   vkCreateDevice -> 0
  ok   vkGetDeviceQueue
  ok   vkCreateBuffer -> 0
  ok   a host-visible memory type the buffer accepts
  ok   vkAllocateMemory -> 0
  ok   vkBindBufferMemory -> 0
  ok   vkMapMemory -> 0
  ok   vkFlushMappedMemoryRanges (poison) -> 0
  ok   vkCreateCommandPool -> 0
  ok   vkAllocateCommandBuffers -> 0
  ok   vkBeginCommandBuffer -> 0
  ok   vkEndCommandBuffer -> 0
  ok   vkCreateFence -> 0
[horizon_gpu:E] Kickoff failed: 0x00000d5c (notifier: 31 'MMU fault')
  note vk warning [nvkmd_horizon_ctx.c:180]: horizon_gpu_submit(1 spans) failed: status 4, nv 0x00000d5c (VK_ERROR_UNKNOWN)
  FAIL vkQueueSubmit -> -4
RESULT: FAIL (51/52) [aborted early]
```

Two channels came up on this device — the upload queue's (syncpt 26,
sharing the probe's) and the queue's own (syncpt 27, `initial=2`, a
fresh counter). Everything through `vkCreateDevice` therefore includes
successful submits: `horizon_gpu_channel_bind_engines` runs at context
creation on both, and the descriptor-table uploads went through the
upload queue. The first submit that fails is NVK's own work.

**The debug-utils messenger paid for itself immediately.** That `note vk
warning` line is the first driver message this project has ever seen on
hardware; every previous failure was silent.

`0x00000d5c` decodes as module 348 (`Module_LibnxNvidia`), description 6
= `LibnxNvidiaError_Timeout`. So the kickoff timed out *and* the error
notifier had an MMU fault recorded. The fault is the event; the timeout
is the channel already wedged behind it.

### Why this could not be diagnosed from the log, and what was changed

nvgpu's error notifier carries the fault **type** and nothing else, and
libnx exposes no ioctl for the faulting address. "Notifier 31" is all
the kernel will ever say. The message also named neither channel, with
two live.

The addresses are ours, though, and so is the map they were supposed to
be in. Patch 0035: `nvkmd_horizon_dev` keeps every live VA on a list,
and a failed submit walks it for each push it submitted, answering one
of three things — the VA and binding that contains the address, that it
is inside a reservation with **nothing bound there**, or that no
reservation covers it at all. The failure names its channel too.

The middle answer is the diagnosis for a fault of this shape, printed on
the spot instead of costing a console round trip per hypothesis.

### Hypotheses **not** acted on

Recorded because they were considered and rejected on evidence, not
shipped as guesses:

- *Engines never bound.* `nvkmd_horizon_create_ctx` calls
  `horizon_gpu_channel_bind_engines` at creation. Also an unbound
  subchannel gives notifier 25 (illegal method), not 31.
- *The small-page half is exhausted by patch 0034.* Measured: the region
  is `pages=0x3f7fff` × 4 KiB = **15.87 GiB** (`t_init`, run 1), and NVK
  reserves ≈ 4.03 GiB in it — 4 GiB for the contiguous `shader_heap`
  (`cls_eng3d` 0xb197 = MAXWELL_B < VOLTA_A), ≈32 MiB for `images`,
  128 KiB for `samplers`; `event_heap` and `qmd_heap` are not contiguous
  and reserve nothing. Margin 4:1.
- *`sync_types[0]` is binary while `nvk_mem_stream_init` asks it for a
  timeline.* This is real — nouveau's `sync_types[0]` is a DRM syncobj,
  which is binary *and* timeline, and the `assert` in `vk_sync_init`
  that would catch the mismatch is compiled out (`-DNDEBUG` confirmed in
  the build). But the obvious fix is wrong: `nvkmd_horizon_ctx_signal`
  calls `nvk_horizon_sync_set_fence`, which `container_of`s to
  `nvk_horizon_sync`. Putting the emulated timeline first would hand it
  a `vk_sync_timeline` and corrupt memory. As it stands the mismatch
  degenerates to "wait for the latest fence", which over-waits and is
  safe. **Pending decision, not a fix to guess at.**

### Audit of the fault path while waiting for the console

Five more candidates read through and **rejected on evidence**. Recorded
because a rejected hypothesis is worth as much as a fix when the next
measurement costs a console round trip:

- *`no_prefetch` is dropped by the backend.* True — `struct
  nvkmd_ctx_exec` carries it and `nvkmd_horizon_ctx_exec` reads only
  `addr` and `size_B`, while `nvkmd_nouveau_ctx` turns it into
  `DRM_NOUVEAU_EXEC_PUSH_NO_PREFETCH`. Harmless here:
  `HORIZON_GPU_SUBMIT_DEFAULT` is already `NOT_MAIN | NO_PREFETCH`, so
  horizon never prefetches any span. The flag is redundantly always on,
  which is the safe direction.
- *`incomplete` is dropped too.* It means "the next push must be in the
  same submit ioctl", and `nvkmd_horizon_ctx_exec` puts every span in one
  `horizon_gpu_submit` call. Satisfied by construction.
- *Subchannel assignment differs from NVK's.* It does not. NVK
  (`nv_push.h`): 0 = NV9097 3D, 1 = NV90C0 compute, 2 = NV9039 M2MF,
  3 = NV902D 2D, 4 = NV90B5 copy. horizon (`channel.c:408`):
  `threed_class, compute_class, inline_to_memory_class, twod_class,
  dma_copy_class`. Identical, and `vkCmdFillBuffer` emits NV90B5 —
  subchannel 4, `dma_copy_class` 0xb0b5 as queried on this console.
- *Engines never bound on the queue's channel.* `nvkmd_horizon_create_ctx`
  calls `horizon_gpu_channel_bind_engines` at creation, and an unbound
  subchannel reports notifier 25, not 31.
- *The push address comes from somewhere exotic.* It does not:
  `nvk_cmd_buffer.c:195` uses `cmd->push_mem->mem->va->addr + offset`,
  and `nvk_cmd_mem_create` is a plain `nvkmd_dev_alloc_mapped_mem`. Both
  candidate addresses — the push and `vkCmdFillBuffer`'s destination
  (`mem->mem->va->addr + memoryOffset`) — come from patch 0031's per-mem
  VA.

Both candidates therefore live in the same place, and reading cannot
separate them. So patch 0035 also dumps the **whole VA map** on a submit
failure — every reservation, its page size, and what is bound in it —
not just the pushes. `vkCmdFillBuffer`'s destination is baked into the
push by NVK and this layer never sees it, so naming only the pushes
would leave half the candidates unaccounted for. The dump is capped at
64 reservations and says how many it dropped.

Tested: host 103/103, cross build, six gates, series re-applied twice
from the pinned base. **Not yet run on hardware.**

## BLOCKER: no console (2026-07-28)

The owner no longer has access to a Nintendo Switch; only an emulator is
available. Phase 4's exit criterion is **blocked**, not abandoned, by the
owner's decision recorded here.

The criterion cannot be met on the emulator, and this is not a
formality. The emulator answers `NVHOST_IOCTL_CTRL_SYNCPT_READ` with
`0x55c` (`LibnxNvidiaError_NotImplemented`), and the fence is built on
that counter. "The CPU reads the pattern the GPU wrote" is a statement
about *ordering*; without the counter there is no ordering, so a
matching pattern would prove nothing — it could be right by accident
with the GPU never having run. `t_vulkan` already refuses to print PASS
in that state, and that refusal stays.

What the emulator is still good for, and what work continues on it:

- It executes the code. The `get_value` defect fixed in patch 0036 is a
  jump through a NULL pointer; it would crash there exactly as it would
  on console.
- The degraded baseline (`HORIZON_GPU_UNTRUSTED_SYNCPT_BASELINE`,
  synchronization.md § 9) is enabled by `t_vulkan`'s own probe when the
  platform cannot read the syncpoint, so the run reaches the steps after
  `vkCreateDevice` instead of stopping there.
- If the MMU fault of run 3 reproduces, it can be fixed without a
  console in the loop.

What it cannot establish, and must never be recorded as: fence
behaviour, submit completion, or anything the syncpoint orders.

`T_VULKAN_DEBUG_SYNC` is **off** for emulator builds. The mode waits on
each submit's fence, and that wait needs the same missing read, so it
would replace every notifier reading with the same failure.

Every result obtained from here until a console returns is labelled
**emulator** in this file, never "hardware".

### Patch 0036 — a NULL call, found by reading

`nvk_mem_stream_init` creates its stream sync on `sync_types[0]` with
`VK_SYNC_IS_TIMELINE`. On nouveau that is a DRM syncobj, binary *and*
timeline; here it is `nvk_horizon_sync_type`, which is binary. The
runtime's guard is an `assert` in `vk_sync_init` and this is a `-DNDEBUG`
build, so the mismatch never surfaced as an error.

It surfaces as a crash instead: `vk_sync_get_value` calls
`sync->type->get_value` with **no NULL check**, and this type had no
`get_value`. Reached from mem-stream chunk recycling as soon as a
chunk's idle time point is ahead of what the stream has seen pass.

Fixed by giving the type the timeline bookkeeping that use needs: the
object carries the value its pending fence will complete and the largest
value known to have completed; `get_value` polls the fence with a
zero-timeout `channel_wait_fence` (which also notices a faulted channel
and needs no device pointer); `wait` returns at once for a value already
passed. `nvkmd_horizon_ctx_signal` now passes `signal_value` through
rather than dropping it, which is what left that counter at zero.

Binary use is unchanged: `wait_value` is 0 there and `passed_value`
starts at 0, so the new early-out is never taken.

**Still a pending decision**: whether this type should advertise
`VK_SYNC_FEATURE_TIMELINE` and replace the runtime's emulation the way
nouveau's syncobj does. That would change the device's timeline mode
from EMULATED to ASSISTED across the whole driver, and reordering
`sync_types` alone would hand `nvkmd_horizon_ctx_signal` a
`vk_sync_timeline` to `container_of` — worse than the mismatch it fixes.
Not done mid-bring-up.

## The push dump, and what is actually in the stream (emulator, 2026-07-28)

`NVK_DEBUG=push_dump,vm` on the emulator produced 4596 lines: every VA
operation in order, and every push decoded. The emulator reaches
`vkQueueSubmit -> 0`, so this is the same stream real nvgpu faults on.

Four submits on the queue's channel, matching the fence `(2, 4)` the
run waited on: `bind_engines`, the queue context init, the queue state
update, the fill.

**Every address in the stream, checked against the map:**

| Method | Value | Mapped? |
|---|---|---|
| `SET_TEX_HEADER_POOL_B`, max index 0x3ff | `0x8002000` | yes — 32 KiB used of 64 KiB bound |
| `SET_TEX_SAMPLER_POOL_B` | `0xa012000` | yes — two chunks, 128 KiB |
| `OFFSET_OUT` (the fill) | `0x1_0a064000` | yes — mem\<0x28\>, 128 KiB bound |
| `SET_SHADER_LOCAL_MEMORY_WINDOW` | `0xff000000` | aperture, see below |
| `SET_SHADER_SHARED_MEMORY_WINDOW` | `0xfe000000` | aperture, see below |
| **`SET_PROGRAM_REGION`** (3D and compute) | **`0xa052000`** | **no — `alloc va [0xa052000, 0x10a052000)`, never bound** |

So the fill's destination and the pushes themselves were never the
problem, and neither were the descriptor pools. Two things came out of
this that were not visible any other way.

### Finding A — the shader memory windows collide with our address space

`nvk_push_dispatch_state_init` programs the local and shared memory
windows at fixed addresses, `0xff000000` and `0xfe000000`. They are
apertures, not allocations: a shader's LDL/STL/LDS/STS go through them.
Upstream knows nothing protects them and says so:

> "Reduce likelihood of collision with real buffers by placing the hole
> at the top of the 4G area. This will have to be dealt with for real
> eventually by blocking off that area from the VM." Really?!? TODO: Fix
> this for realz.

On this platform it is not a likelihood. The small-page region begins at
`0x8000000` and NVK's reservations march up through 4 GiB, so the shader
heap's own reservation `[0xa052000, 0x10a052000)` **covers both
windows**. The day a shader uses local or shared memory it will read
whatever the heap has bound there — its own code, most likely.

First detected (patch 0037), then **fixed** (patch 0039) — because the
premise of the detection was wrong. This file said
`NVGPU_AS_IOCTL_ALLOC_SPACE` has no "place it here" form. It has one:
`NvAllocSpaceFlags_FixedOffset`, sitting in libnx's `ioctl.h` next to the
call we were already making. The correction is worth stating plainly:
the aperture was always reservable, and "open decision" was a conclusion
drawn from a fact nobody had checked.

`horizon_gpu_vm_reserve_fixed` now exists, and reports a range that came
back at a different address as a failure rather than accepting it. With
it:

- the window aperture `[0xfe000000, 0x100000000)` is reserved at device
  creation and never bound, which is exactly what "blocking off that
  area from the VM" means. An access through the window still faults;
  the point is that it can no longer hit somebody's buffer instead.
  Not fatal if the reservation fails — the collision needs a shader to
  matter, so refusing to create the device would trade a future wrong
  answer for an immediate one;
- **`NVKMD_VA_ALLOC_FIXED` is implemented**, closing extension 5 of the
  six Phase 4 step 1 enumerated.

### Finding B — the leading hypothesis for the hardware MMU fault

The queue context init writes **two GPU privileged registers from the
pushbuffer**, via `CALL_MME_MACRO(23)` = `NVK_MME_SET_PRIV_REG`:

```
mthd 38b8 NV9097_CALL_MME_MACRO(23) .V = 0x0
mthd 38bc NV9097_CALL_MME_DATA(23)  .V = 0x8          <- mask, bit 3
mthd 38bc NV9097_CALL_MME_DATA(23)  .V = 0x419f78     <- gr_gpcs_tpcs_sm_disp_ctrl

mthd 38b8 NV9097_CALL_MME_MACRO(23) .V = 0x0
mthd 38bc NV9097_CALL_MME_DATA(23)  .V = 0x4000
mthd 38bc NV9097_CALL_MME_DATA(23)  .V = 0x419e44     <- sms_hww_warp_esp_report_mask
```

This is the only thing in the whole stream that is not an ordinary
memory operation, and it is the only one whose success depends on what
the *kernel* permits a user channel to do. Desktop nouveau allows it;
Horizon's nvgpu is not going to be as permissive, and an emulator would
neither implement nor enforce it — which is exactly why the fault does
not reproduce there.

NVK's own comment says what the writes are for: enabling FP helper
invocation memory loads, so that one dEQP subgroup test stops failing
occasionally. Not required for anything Phase 4 does.

### Finding B is R18, and it was already in this repository

Written up as a hypothesis above, then found already recorded — Phase 0
put it in `docs/known-risks.md` as **R18**, derived from the reference
ports' hardware experience:

> **R18 — Privileged GR register writes reset a homebrew channel.** The
> reference neuters `nvk_mme_set_priv_reg` in `nvk_cmd_draw.c` because
> privileged graphics-register writes are rejected for homebrew channels
> and reset the channel, **after which every submit times out**.

That is the third hardware run, symptom for symptom: `vkCreateDevice`
returns 0 because submission is asynchronous, and the next kickoff on
that channel fails with `LibnxNvidiaError_Timeout` and an error notifier
set. R18 expected it in Phase 5, at the first draw. It arrives in Phase 4
because these two writes happen while the *queue's context* is being
initialised, long before anything is drawn.

So this is not a guess to defer. It is a documented, hardware-derived
fact that we knowingly parked, and it has come due.

**Fixed as patch 0038, the way R18 asked for.** R18's own mitigation was
"do not carry this patch forward blindly … determine what NVK loses by
skipping them. Record the answer rather than inheriting the no-op." So:

- The gate is a kmd capability, `nvkmd_info::has_priv_reg_writes` — the
  same shape as D12's `has_sparse`, and for the same reason. The class
  check says the *chip* supports SET_PRIV_REG; whether the driver
  underneath lets an unprivileged channel use it is a separate question.
  True for nouveau, where NVK's behaviour is untouched; false for
  horizon.
- What is lost is recorded at the declaration, from NVK's own comments:
  bit 3 of `gr_gpcs_tpcs_sm_disp_ctrl` enables FP helper invocation
  memory loads, without which one dEQP subgroups test fails
  occasionally; `sms_hww_warp_esp_report_mask` disables Out Of Range
  Address exceptions for a case involving an empty fragment shader.
  Neither is reachable by anything this port runs today.

**Unverified on hardware.** The reasoning is: R18's recorded fact, the
timeout symptom matching it exactly, and the push dump showing these are
the only two operations in the whole stream that are not ordinary memory
access. That is strong, and it is still not a measurement. The console
run that confirms it is the same one that closes Phase 4.

## The last unmeasured link: what the readback rests on

Audited by reading, because nothing can execute it. The test picks the
first HOST_VISIBLE memory type the buffer accepts, and NVK's list for
this backend puts a `HOST_VISIBLE | HOST_CACHED` type before the
coherent ones. So the buffer is very likely **cached**, which means:

- `NVKMD_MEM_COHERENT` is not set, so D14's uncached mapping does not
  apply and the flush/invalidate are **not** no-ops;
- the poison flush before the submit, and the invalidate before the
  readback, both have to do real work;
- and that work is Mesa's `cache_ops_aarch64.c` (`dc cvac` / `dc civac`),
  because `util_has_cache_ops()` is true on aarch64 and `nvkmd_mem_sync_*`
  defers to it.

**Nothing has measured those on Horizon, in either direction.**
`t_uncached` passed on hardware, but it measured the *uncached* path —
and CPU→GPU at that. The readback needs GPU→CPU through a *cached*
mapping. The two share no mechanism.

What makes it plausible: the fill's `LAUNCH_DMA` carries
`FLUSH_ENABLE = TRUE` (read off the push dump), which flushes the GPU's
side, and `dc civac` cleans to the point of coherency, which on a UMA
part is DRAM. Plausible is not measured.

So `t_vulkan` now reports the memory type it chose and which mechanism
the result rests on, in one line, because that is the first question
anyone will ask if the readback comes back wrong.

**Recorded gap, not built on a guess:** a horizon-level test of GPU→CPU
visibility would need a GPU command that writes to memory, and
`horizon_gpu` has only fence-increment, syncpoint-wait, `SET_OBJECT` and
NOP. Adding a semaphore-release builder to `horizon/` to measure
something `t_vulkan` already measures end to end is not worth it unless
`t_vulkan`'s readback actually fails; then it is the right next step.

## Next concrete task

**The emulator is exhausted.** It answered everything it could:

- `vkCreateDevice -> 0`, then every step through `vkQueueSubmit -> 0`;
- it stops at `vkWaitForFences`, which is where a platform with no
  syncpoint read *must* stop — waits are deliberately not degraded;
- the MMU fault does **not** reproduce there, which is itself the
  finding: it is specific to real nvgpu;
- and with `NVK_DEBUG=push_dump,vm` it handed over the whole push stream,
  which is how R18 and the shader-window collision were found without a
  console in the loop.

There is no further question the emulator can answer about this
sequence. Five defects were found and fixed since the last console run —
patches 0034 (page half), 0036 (`get_value`, a call through NULL), 0037
(window collision, detected), 0038 (R18), plus the degraded reap in
`horizon/` — and **none of the five has been executed on hardware.**

So the next step is one console run, and it is the same run either way:
it closes Phase 4 or it produces a complete diagnosis, because the build
now carries the debug-utils messenger, the VA-map dump on failure, the
decoded push dump and the memory-type line.

Held for the console, unchanged:

**Run `t_vulkan.nro` on a real Switch.** That is Phase 4's exit
criterion and the only thing left in it:

- the sequence runs,
- the CPU-side validation of the written pattern passes,
- no wait-idle was inserted to make it pass — there is none in the test
  and none in `exec()`,
- the console log is pasted into this file.

`scripts/package-horizon.sh` produces the artefact set and the manifest.
Nothing in this environment can produce the log; it is a hardware
measurement and it belongs to whoever holds the console.

Four things are owed alongside it and travel in the same package, none
of which blocks the criterion above:

- `t_threads` and `t_ostime` on hardware rather than on the emulator
  (Phase 3).
- `t_uncached`, D14's measurement, never executed anywhere.
- `t_sysinfo` re-run from a current binary; the 18/19 above was a stale
  `.nro`.

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
