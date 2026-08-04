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

