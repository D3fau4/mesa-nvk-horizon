# Reference analysis — the four `switch-nvk` snapshots

**Phase 0 deliverable.** Audit of four ZIP snapshots of the `switch-nvk` project, treated
as technical references only. No source from them is incorporated into this repository.

Line references are to the `master` snapshot unless stated otherwise. Every claim in this
document was verified by reading the cited file; claims that could not be verified from the
source are marked **[claim]**.

Snapshots analysed (extracted read-only to a scratch directory):

| Alias | Archive | Root directory |
|---|---|---|
| `master` | `switch-nvk-master.zip` | `switch-nvk-master/` |
| `nvk-wsi` | `switch-nvk-switch-port-nvk-wsi.zip` | `switch-nvk-switch-port-nvk-wsi/` |
| `triple-buffer` | `switch-nvk-switch-port-triple-buffer.zip` | `switch-nvk-switch-port-triple-buffer/` |
| `wsi-zero-copy` | `switch-nvk-switch-port-wsi-zero-copy.zip` | `switch-nvk-switch-port-wsi-zero-copy/` |

---

## 1. Differences between the four variants

Method: per-file MD5 across all four trees, then `diff -rq`.

**Of 88 files, 71 are byte-identical across all four snapshots.** Only 17 differ.

### 1.1 Difference matrix

Letters denote distinct content; `–` means the file is absent from that snapshot.

| File | master | nvk-wsi | triple-buffer | wsi-zero-copy |
|---|:--:|:--:|:--:|:--:|
| `LICENSE` | **A** (GPL-2.0) | B (AGPL-3.0) | B | B |
| `README.md` | **A** | B | B | B |
| `.gitignore` | A | **B** | A | A |
| `BUILD_AND_RUN.md` | A | **B** | A | A |
| `RESUME_NVK.md` | A | **B** | A | A |
| `docs/knowledge/SKILL-switch-port.md` | A | **B** | A | A |
| `REPRODUCE.md` | A | – | A | – |
| `crossfiles/native.txt` | A | – | A | – |
| `crossfiles/rust.cross` | A | – | A | – |
| `crossfiles/switch.cross` | A | – | A | – |
| `patches/switch-nvk-mesa-25.0.7.patch` | **A** (63 748 B, 24 files) | B (8 304 B, 10 files) | **A** | B |
| `winsys/drm_shim.c` | A (66 119 B) | **B** (64 826 B) | A | A |
| `winsys/wsi/wsi_common_switch.c` | A (616 lines) | **B** (436 lines) | A | A |
| `winsys/wsi/apply-wsi-switch.sh` | A | **B** | A | A |
| `winsys/mesa-edits/.../nvk_image.c` | A | – | A | A |
| `winsys/mesa-edits/.../nvk_queue.c` | A | – | A | A |
| `winsys/mesa-edits/.../wsi_common.c` | A | – | A | A |

### 1.2 What this actually means

- **`master` and `triple-buffer` are identical except `LICENSE` and `README.md`.** Nothing
  else differs — not one line of code.
- **`wsi-zero-copy` is `master` minus `REPRODUCE.md` and `crossfiles/`, with the older
  small patch.** Its `winsys/` code is `master`'s, but its patch does not contain the WSI
  backend at all. That combination is internally inconsistent: the tree ships zero-copy
  winsys helpers and a patch that never wires a swapchain into Mesa.
- **`nvk-wsi` is the only genuinely earlier snapshot**: an older `drm_shim.c`, a 436-line
  WSI backend, no `mesa-edits/`, and the small patch.

**The branch names do not describe the trees.** There is no tree in which "triple buffer"
is a distinguishing feature, and `wsi-zero-copy` is not the branch where zero-copy lives.
See § 1.3 for where the differentiating code actually is.

### 1.3 The one real functional difference is inside the patch, not the tree

`patches/switch-nvk-mesa-25.0.7.patch` embeds a **complete 640-line
`src/vulkan/wsi/wsi_common_switch.c`** as a new file (`@@ -0,0 +1,640 @@`). The tracked
standalone `winsys/wsi/wsi_common_switch.c` is a **616-line older copy** of the same file.
Diffing them (extraction verified) yields exactly three functional deltas, all present only
in the patch:

```diff
-   caps->minImageCount = 2;
+   caps->minImageCount = 3;  /* Triple-buffer: with 2 buffers (1 in-flight) the acquire
+                                (nwindowDequeueBuffer) stalls on the 60Hz compositor and snaps
+                                every frame to 30fps; 3 lets the framerate track the real load. */
```
```diff
+static struct wsi_switch_swapchain *g_zc_owner = NULL;
```
plus the ownership transfer at swapchain create and the guarded release at destroy.

So: triple buffering **is** implemented, in the big patch, which `master` and
`triple-buffer` both carry. And `winsys/wsi/apply-wsi-switch.sh:15` copies the stale
616-line file over it — see § 10.1.

---

## 2. Relevant file tree

```
switch-nvk-master/
├── Dockerfile                     build image: devkitA64 + rust nightly + LLVM-15 + bindgen
├── apply-patches.sh               patch -p1 the Mesa tree
├── configure-mesa.sh              generates crossfiles/, runs meson setup
├── build-native-tools.sh          Mesa host generators
├── build-std-sysroot.sh           Rust std sysroot via -Zbuild-std
├── rustc-switch.sh                rustc wrapper (strips Meson's -Clinker)
├── package-nvk.sh                 MRI-merge 19 archives into one libvulkan.a
├── aarch64-switch-horizon.json    custom Rust target (embeds the NRO linker script)
├── crossfiles/
│   ├── switch.cross               Meson cross file — system = 'horizon'
│   ├── rust.cross                 rust binary = /work/rustc-switch.sh + bindgen args
│   └── native.txt                 build-machine tools
├── patches/
│   └── switch-nvk-mesa-25.0.7.patch          24 files; contains the WSI backend
├── pristine-25.0.7/               9 unmodified upstream files (of 24 patched)
├── compat/                        newlib gap fillers + forced-include header
│   ├── compat.c                   secure_getenv, mprotect/msync/madvise no-ops
│   ├── switch_compat.h            force-included into every TU
│   ├── dlfcn.h  syslog.h  sys/mman.h  sys/sysmacros.h
├── winsys/
│   ├── drm_shim.c   (1634 lines)  ★ the entire nouveau-uAPI emulation
│   ├── drm_shim.h                 exported open/mmap hooks + render-node path
│   ├── switch_libc_shim.c (267)   --wrap=open/close/stat/lstat + libc gaps
│   ├── nvk_loaderless_shim.c      --wrap=vk_icdGetInstanceProcAddr
│   ├── mesa-edits/                whole edited Mesa files (nvk_image.c, nvk_queue.c,
│   │                              wsi_common.c) — a second source of truth
│   ├── wsi/
│   │   ├── wsi_common_switch.c (616)  ★ STALE copy of the patch's 640-line backend
│   │   └── apply-wsi-switch.sh        copies the stale copy over the patched one
│   ├── smoke/                     11 standalone Vulkan test programs + GLSL shaders
│   └── build-nro.sh, build-exe-linktest.sh, compile-test.sh, rebuild-diag.sh, verify-fix.sh
├── docs/knowledge/                4 long engineering notes (the most valuable prose)
├── RESUME_NVK.md                  running log / source of truth
├── PLAN_WSI_NWINDOW.md            WSI design + an unchecked task list
└── UNDEFINED_SYMBOLS.txt          54 symbols from a stale link
```

★ = the three files that carry essentially all the Horizon-specific engineering.

---

## 3. Principal functions

### `winsys/drm_shim.c` — the nouveau-uAPI emulation

| Function | Line | Role |
|---|---|---|
| `shim_nv_up` / `shim_nv_down` | 159 / 212 | `nv` service bring-up and teardown |
| `drm_shim_open` / `_close` | 265 / 286 | refcounted sentinel fd |
| `drm_shim_mmap` / `_munmap` | 310 / 321 | returns the raw backing pointer / no-op |
| `nouveau_getparam` | 331 | synthetic GM20B parameters |
| `nouveau_nvif` | 364 | synthetic NVIF object model, device info, `SCLASS` |
| `shim_bo_alloc_locked` | 442 | `memalign` + `nvMapCreate` |
| `shim_internal_bo_locked` | 476 | as above + auto GPU VA |
| `nouveau_gem_new` / `_info` | 501 / 523 | GEM handle = array index + 1 |
| `drm_shim_bo_nvmap_by_va` | 541 | linear scan, VA → `NvMap` id (WSI zero-copy) |
| `drmCloseBufferHandle` | 569 | unmap + `nvMapClose` + `free` |
| `gen_fence_cmdlist` | 616 | Maxwell syncpoint-increment + L2 flush |
| `gen_flush_cmdlist` | 625 | cache-flush list — **built but never submitted** |
| `gen_setobj_cmdlist` | 647 | in-stream `SET_OBJECT` engine binds |
| `kickoff_retry` | 666 | 400 × 250 µs sleep loop on `0xd5c` |
| `nouveau_channel_alloc` / `_free` | 678 / 739 | `nvGpuChannelCreate`, Zcull, cmdbuf |
| `vm_bind_op` / `nouveau_vm_bind` | 772 / 825 | `MapBufferEx` FIXED / `UnmapBuffer` |
| `nouveau_exec` | 846 | **the whole submit path** |
| `nouveau_dispatch` | 1030 | ioctl dispatch under the global mutex |
| `drmSyncobj*` (12 functions) | 1285–1467 | userspace syncobj table |
| `drm_shim_selftest` | 1551 | in-tree GPU smoke test |

### `winsys/wsi/wsi_common_switch.c` — the swapchain

| Function | Line | Role |
|---|---|---|
| `wsi_switch_build_graphic_buffer` | 59 | NIL layout + `NvMap` id → `NvGraphicBuffer` |
| `..._surface_get_capabilities` | 156 | `minImageCount` (2 tracked / 3 patched) |
| `..._acquire_next_image` | 303 | `nwindowDequeueBuffer` + CPU fence wait |
| `..._queue_present` | 349 | zero-copy queue or CPU memcpy fallback |
| `..._release_images` | 441 | clears `busy` only — does not cancel buffers |
| `..._swapchain_destroy` | 451 | `nwindowReleaseBuffers` / `framebufferClose` |
| `..._create_swapchain` | 469 | `nwindowConfigureBuffer` registration loop |

### `winsys/switch_libc_shim.c` — libc interposition

`__wrap_open` (48), `__wrap_close` (64), `__wrap_stat` (87), `__wrap_lstat` (94),
`mmap` (104), `munmap` (115), plus ~20 defined-from-scratch POSIX functions.

---

## 4. Initialisation flow

```
app                                       (a homebrew .nro)
 └─ vkCreateInstance
     └─ Mesa vk_instance / NVK nvk_instance
         └─ drmGetDevices2()                       drm_shim.c:1185
             └─ shim_build_device()                drm_shim.c:1133
                 → synthetic DRM_BUS_PLATFORM device, "nvidia,gm20b"
         └─ open("/dev/dri/renderD128")
             └─ __wrap_open → drm_shim_is_render_node → drm_shim_open
                 └─ refcnt 0→1 ⇒ shim_nv_up()      drm_shim.c:159
                     ├─ nvInitialize()                      → nvdrv session
                     ├─ nvFenceInit()                       → /dev/nvhost-ctrl
                     ├─ nvMapInit()                         → /dev/nvmap
                     ├─ nvGpuInit()                         → /dev/nvhost-gpu, -ctrl-gpu
                     ├─ nvGpuGetCharacteristics()->big_page_size   (fallback 0x10000)
                     ├─ nvAddressSpaceCreate(big_page_size) → /dev/nvhost-as-gpu
                     └─ nvioctlNvhostAsGpu_AllocSpace(
                            pages = 8 GiB / 4 KiB, page = 0x1000,
                            flags = 0 (non-fixed), align = 0x10000)  → va_base
                        └─ the one arena all NVK VA must live inside
             └─ stat("/dev/dri/renderD128")
                 └─ __wrap_stat → fabricated char device, rdev = (226<<8)|128
 └─ vkEnumeratePhysicalDevices
     └─ drmGetVersion → "nouveau 1.4.0"            drm_shim.c:1094
     └─ DRM_NOUVEAU_GETPARAM  → chipset 0x12b, GRAPH_UNITS = 1 GPC / 2 TPC
     └─ DRM_NOUVEAU_NVIF SCLASS → {0xb0b5, 0x902d, 0xb197, 0xb1c0, 0xa140}
     └─ DRM_NOUVEAU_NVIF MTHD/DEVICE_INFO → SOC, MAXWELL, rev 0xa1, "GM20B"
     └─ DRM_NOUVEAU_VM_INIT → 0 (accepted ⇒ NVK sets has_vm_bind)
 └─ vkCreateDevice
     └─ nvkmd_nouveau_create_dev  [patched] → drm_shim_va_base() gives the arena
         └─ util_vma_heap_init(main = 7/8, replay = 1/8 of the arena)
     └─ DRM_NOUVEAU_CHANNEL_ALLOC              drm_shim.c:678
         ├─ nvGpuChannelCreate(addr_space, Priority_Medium)
         │     (libnx internally allocates the GPFIFO and AllocObjCtx(0xB197))
         ├─ Zcull BO (nvGpuGetZcullCtxSize(), align 0x20000) + nvGpuChannelZcullBind
         └─ builtin cmdbuf BO (0x1000, align 0x20000) holding three command lists:
              fence-increment | cache-flush (unused) | SET_OBJECT engine binds
```

Ordering constraints discovered by the reference and worth carrying forward:

1. `nvFenceInit()` must precede any `nwindowDequeueBuffer` / `nvMultiFenceWait`.
2. A `FIXED` VA map is only accepted **inside** a previously reserved non-fixed range;
   a bare fixed reservation returns `-EINVAL` (`drm_shim.c:176-180`).
3. `AllocObjCtx` must **not** be called again — libnx already did it inside
   `nvGpuChannelCreate`; a second call returns `-EINVAL` (`drm_shim.c:716-718`).

---

## 5. Memory allocation flow

```
NVK: nvkmd_mem_alloc
 └─ DRM_NOUVEAU_GEM_NEW                         drm_shim.c:501
     ├─ align = req->align ? req->align : 0x1000
     ├─ kind  = req->info.tile_flags >> 8        (0 ⇒ NvKind_Pitch)
     └─ shim_bo_alloc_locked                     drm_shim.c:442
         ├─ align = max(align, 0x1000); size = align_up(size, align)
         ├─ linear scan of bos[4096] for a free slot        ← O(n)
         ├─ mem = memalign(0x1000, size); memset(mem, 0, size)
         └─ nvMapCreate(&map, mem, size, align, kind, cacheable = false)
                                                   ↑ cached heap declared uncached
     └─ handle = slot + 1;  map_handle = handle

NVK: mmap(fd, map_handle)
 └─ drm_shim_mmap → returns bo->cpu verbatim      drm_shim.c:310
     (munmap is an unconditional no-op — drm_shim.c:321)

NVK: nvkmd_va_alloc → its VA heap (constrained to the shim arena by patch)
NVK: nvkmd_mem_bind
 └─ DRM_NOUVEAU_VM_BIND → vm_bind_op              drm_shim.c:772
     ├─ MAP:
     │   map_kind = op->flags & 0xff;  if (0) map_kind = bo->kind
     │       ↑ block-linear kinds (e.g. ZF32 = 0x7b) arrive HERE, not at GEM_NEW
     │   nvioctlNvhostAsGpu_MapBufferEx(
     │       FixedOffset | IsCacheable, map_kind, nvMapGetHandle(bo),
     │       page = 0x1000, bo_offset, range, addr)
     │   on success: bo->gpu_va = addr; bo->gpu_bo_offset = bo_offset
     └─ UNMAP:
         nvioctlNvhostAsGpu_UnmapBuffer(fd, op->addr)   ← result ignored,
                                                          bo->gpu_va NOT cleared
```

The un-cleared `bo->gpu_va` is the reference's most concrete memory bug: `drmCloseBufferHandle`
later unmaps that same address again (`drm_shim.c:580-581`), and if NVK's VA allocator has
re-bound it to a different buffer in the meantime, the live mapping is torn down.
`drm_shim_bo_nvmap_by_va` (used by the WSI) has the same blind spot — it matches on
`bo->gpu_va` with no "still mapped" check.

Full concept separation and our rules: `docs/memory-model.md`.

---

## 6. Submit flow

```
NVK: nvk_queue_submit → nvkmd_ctx_wait / _exec / _signal
 └─ DRM_NOUVEAU_EXEC → nouveau_exec               drm_shim.c:846
     │  (entire body runs under the global g_dev.lock — drm_shim.c:1036)
     │
     ├─ for each wait syncobj:  nvFenceWait(fence, 2 000 000 µs)   ← CPU BLOCKS
     │                                                    result ignored
     ├─ if push_count == 0: attach the channel's current fence, return 0
     │
     ├─ first submit only: append SET_OBJECT list (subch 0..4 → 0xb197, 0xb1c0,
     │                     0xa140, 0x902d, 0xb0b5)
     ├─ for each push: nvGpuChannelAppendEntry(va, va_len/4,
     │                     NOT_MAIN | NO_PREFETCH, 0)
     │                 ↑ NVK passes flags = 0; forcing these two is load-bearing
     │                 (also: an O(4096) BO scan per push, purely for logging)
     ├─ nvGpuChannelIncrFence(chan)              ← bumps libnx's expected value
     ├─ append the fence cmdlist                  ← the GPU-side actual increment
     │      dword[2] = syncpt_id | (1<<20) | (1<<16)   incr | GPU L2 flush
     ├─ kickoff_retry: up to 400 × svcSleepThread(250 µs) on 0xd5c
     │                 ↑ sleeping while holding the global lock
     ├─ nvGpuChannelGetFence(chan, &fence)
     ├─ signal syncobjs with that fence
     ├─ nvFenceWait(&fence, 2 000 000 µs)         ← FULL DRAIN, EVERY SUBMIT
     ├─ nvioctlNvhostCtrl_SyncptRead(...)         ← extra ioctl, every submit
     └─ nvGpuChannelGetErrorNotification(...)     ← extra ioctl, every submit
     return 0                                      ← even on fault or drain timeout
```

`nvk_queue.c` itself was **not** functionally rewired. The `mesa-edits` copy adds only a
`qe_log` helper and ten `NVK_TRACE`-gated trace calls; the exec loop is upstream.

---

## 7. Acquire and present flow

```
vkAcquireNextImageKHR
 └─ wsi_switch_acquire_next_image                wsi_common_switch.c:303
     ├─ nwindowDequeueBuffer(window, &slot, &mf)          ← Result checked
     ├─ slot out of range → nwindowCancelBuffer (result ignored) + OUT_OF_DATE
     ├─ nvMultiFenceWait(&mf, 1 000 000 µs)               ← CPU BLOCKS, result ignored
     └─ images[slot].acquire_fence = mf   ← stored and NEVER READ
        images[slot].busy = true;  *image_index = slot

vkQueuePresentKHR
 └─ Mesa common WSI (wsi_common.c)
     ├─ throttle: WaitForFences(fence[image_index], true, ~0ull)   ← CPU BLOCKS
     └─ submits image->blit.cmd_buffers[qf]
            = CmdCopyImageToBuffer(image → host-visible buffer)
              ← a FULL-RESOLUTION GPU COPY, every frame, even in "zero-copy" mode,
                into a buffer the zero-copy path never reads
 └─ wsi_switch_queue_present                     wsi_common_switch.c:349
     ├─ zero-copy:  WaitForFences(..., ~0ull)              ← INFINITE CPU WAIT
     │              nwindowQueueBuffer(window, slot, NULL) ← NULL fence,
     │                                                       result ignored
     └─ fallback:   WaitForFences(..., ~0ull)
                    armDCacheFlush(cpu_map, stride*height) ← ~3.5 MB per frame
                    framebufferBegin → row memcpy → framebufferEnd
     └─ if zero_copy && !configured && !fb_created:
            falls through both branches, returns VK_SUCCESS having presented nothing
```

The reference's own smoke test calls `vkQueueWaitIdle` **twice per frame**
(`winsys/smoke/nvk_vi_swapchain.c:187, :194`), and presents with
`waitSemaphoreCount = 0`, because the shim's CPU-side semaphore wait would otherwise
deadlock the WSI's internal present submit — the reason is written down at
`winsys/smoke/nvk_swapchain.c:395-401`.

Why zero-copy needs a residual GPU blit at all: the swapchain requests
`WSI_IMAGE_TYPE_CPU` (`wsi_common_switch.c:494-496`), and
`wsi_cpu_image_needs_buffer_blit` returns `true` unless `wants_linear` is set — which it is
not (`wsi_common.c:2215-2225`, `:88`). `WSI_SWAPCHAIN_NO_BLIT` was planned
(`PLAN_WSI_NWINDOW.md:359`) and never implemented.

---

## 8. Simulated functionality

Everything below has **no kernel object behind it**; it exists only to satisfy Mesa.

| Simulated thing | Where | Nature |
|---|---|---|
| `/dev/dri/renderD128` | `drm_shim.h:35`, `switch_libc_shim.c:78-85` | path match; fabricated `stat` with Linux DRM major/minor 226/128 |
| DRM file descriptor | `drm_shim.c:80` | the constant `0x6E760000`; no fd table |
| GEM handle namespace | `drm_shim.c:512` | array index + 1 into `bos[4096]` |
| `mmap` cookie | `drm_shim.c:515` | `map_handle` = the GEM handle |
| NVIF object model | `drm_shim.c:364-431` | `NEW`/`DEL`/`MTHD`/`SCLASS` fully synthesised |
| Device info / chipset | `drm_shim.c:384-397` | constants: rev `0xa1`, SOC, MAXWELL, "GM20B" |
| `drmVersion` | `drm_shim.c:1094-1118` | "nouveau" 1.4.0, date "20240101" |
| DRM device enumeration | `drm_shim.c:1133-1161` | one synthetic `DRM_BUS_PLATFORM` device |
| DRM syncobjs (all 12 calls) | `drm_shim.c:1285-1467` | userspace struct table; only the attached `NvFence` is real |
| Timeline semaphores | `drm_shim.c:1428-1435` | `drmSyncobjTimelineWait` **discards the points** |
| PRIME / dma-buf | `drm_shim.c:1243-1257` | `ENOSYS` |
| syncobj FD export/import | `drm_shim.c:1438-1467` | `ENOSYS` |
| Kernel-side engine binding | `drm_shim.c:647-658` | replaced by in-stream `SET_OBJECT` |
| Total physical memory | patch, `os_misc.c` | fixed 3 GiB |
| `sysconf` values | `switch_libc_shim.c:184-203` | 3 online CPUs, 4 configured, 4 GiB/4 KiB pages |
| POSIX regex | `switch_libc_shim.c:210-223` | `regcomp` succeeds, `regexec` always `REG_NOMATCH` |
| uid/gid/chown family | `switch_libc_shim.c:249-265` | constant returns |
| `mprotect`/`msync`/`madvise` | `compat/compat.c:20-22` | no-op success |
| `dlopen`/`dlsym`/`syslog` | `compat/dlfcn.h`, `compat/syslog.h` | inline stubs |

### 8.1 Silent stubs — success returned without doing the work

The ones that can hide a real failure, as opposed to the merely-absent:

| Location | Behaviour |
|---|---|
| `drm_shim.c:1022` | **`nouveau_exec` returns 0 even when the drain timed out or the error notifier reports an MMU fault** — both are only logged |
| `drm_shim.c:561-567` | `GEM_CPU_PREP` is a documented no-op — no fence tracking, no cache invalidate |
| `drm_shim.c:321-326` | `drm_shim_munmap` unconditionally returns 0 |
| `drm_shim.c:783-786` | VM_BIND unmap discards the `UnmapBuffer` result and leaves `bo->gpu_va` stale |
| `drm_shim.c:834-842` | VM_BIND **drops wait-syncs entirely**; signals sig-syncs immediately with no fence |
| `drm_shim.c:428-430` | an **unknown NVIF type returns success** |
| `drm_shim.c:1044-1046` | `VM_INIT` returns 0 unconditionally |
| `drm_shim.c:1389-1393` | every syncobj wait is silently capped at 3 s and returns as if it waited properly |
| `drm_shim.c:587`, `:741` | closing an invalid BO handle / freeing an unknown channel returns success |
| `wsi_common_switch.c:437-438` | present falls through both branches and returns `VK_SUCCESS` having presented nothing |
| `wsi_common_switch.c:142-150` | `surface_get_support` returns `true` for every queue family |
| `wsi_common_switch.c:441-449` | `release_images` never calls `nwindowCancelBuffer` — dequeued slots leak |

---

## 9. Synchronous operations

The reference is synchronous by construction. Every CPU stall found:

| # | Location | Stall |
|---|---|---|
| 1 | `drm_shim.c:983` | **`nvFenceWait` drain after every submit** — the defining one |
| 2 | `drm_shim.c:867` | CPU wait on every dependency before submitting |
| 3 | `drm_shim.c:669-674` | `400 × svcSleepThread(250 µs)` kickoff retry, **holding the global lock** |
| 4 | `drm_shim.c:1036-1073` | one global mutex spans every ioctl, including the drain in (1) |
| 5 | `drm_shim.c:992-1020` | two extra ioctls per submit even on success |
| 6 | `drm_shim.c:1408` | blocking syncobj wait, capped at 3 s |
| 7 | `drm_shim.c:1541` | `svcSleepThread(80 ms)` in the selftest |
| 8 | `wsi_common_switch.c:328` | `nvMultiFenceWait(1 s)` at acquire, result ignored |
| 9 | `wsi_common_switch.c:370, :390` | `WaitForFences(..., ~0ull)` — infinite — before present |
| 10 | `wsi_common.c:1453-1456` | common-WSI throttle fence wait |
| 11 | all 11 smoke tests | `vkQueueWaitIdle` per submit; `nvk_vi_swapchain.c` twice per frame |
| 12 | `nvk_queue.c:440-442` | upstream `nvkmd_ctx_sync` after `nvk_queue_submit_simple` |

Consequence: no CPU/GPU overlap is possible anywhere, and a channel reset can make a drain
"succeed" spuriously because recovery force-advances the syncpoint — the reference's own
notes flag this (`docs/knowledge/nvk_winsys_debugging_patterns.md:16`).

Our replacement model: `docs/synchronization.md`.

### 9.1 Ignored `Result` values

Checked: `nv` bring-up, `nvMapCreate`, `nvAddressSpaceMap`, `nvGpuChannelCreate`,
`nvGpuChannelZcullBind`, `MapBufferEx`, kickoff, both error-info getters,
`nwindowDequeueBuffer`.

Ignored: **every one of the nine `nvGpuChannelAppendEntry` calls**,
`nvGpuChannelIncrFence`, `nvGpuChannelGetFence`, the dependency `nvFenceWait`,
`nvioctlNvhostAsGpu_UnmapBuffer` (both sites), every teardown call
(`nvAddressSpaceUnmap`, `nvMapClose`, `nvGpuChannelClose`, `nvAddressSpaceFree`),
`nvFenceInit`, `nvMultiFenceWait`, `nwindowCancelBuffer`, `nwindowQueueBuffer`.

---

## 10. Disabled or diverted code inside NVK

Changes the big patch makes to NVK/Mesa behaviour (as opposed to diagnostics):

| File | Change | Reason given |
|---|---|---|
| `nvk_cmd_draw.c` | **`nvk_mme_set_priv_reg` neutered** | privileged GR register writes are blocked for homebrew channels and reset the channel |
| `nvk_wsi.c` | `supports_modifiers = false` | no dma-buf / PRIME on Horizon |
| `nvk_wsi.c` | `khr_present_wait = false` | the timeline-semaphore present path cannot be backed |
| `nvkmd_nouveau.h` | VA heap moved to `[1<<34, 1<<37]` | nvgpu only accepts fixed VA in roughly that window |
| `nvkmd_nouveau_dev.c` | heap re-pointed into `drm_shim_va_base()` | fixed maps must be inside a real reservation |
| `nvk_physical_device.h` | `NVK_USE_WSI_PLATFORM` forced on | enable WSI with no X11/Wayland/KMS |
| `nvk_instance.c` | `.NN_vi_surface = true` | advertise `VK_NN_vi_surface` |
| `vk_image.c/.h` | `DETECT_OS_HORIZON` added to the `drm_format_mod` guard | otherwise `drm_format_mod` stays 0 = LINEAR and **NIL makes every image linear** |
| `vulkan/runtime/meson.build` | compile `vk_drm_syncobj.c` on `horizon` | the shim supplies the `drmSyncobj*` family |
| `wsi/meson.build` | compile `wsi_common_switch.c` on `horizon` | the new backend |
| `util/libdrm.h` | include real `<xf86drm.h>` on `__SWITCH__` | bind Mesa's calls to the shim's definitions |
| `os_misc.c` | fixed 3 GiB physical memory | no query available |
| `threads_posix.c` | `mtx_timedlock` busy-loop | libnx pthread lacks `pthread_mutex_timedlock` |
| `u_endian.h` | `__BYTE_ORDER__` fallback | no platform branch matched |

Diagnostics shipped in the "authoritative" patch, some **not** runtime-gated:

- `vk_instance.c` — two blocks marked `/* TEMP diagnostic (switch-nvk). */`, ungated
- `wsi_common_headless.c` — `hl_log`, comment says `REMOVE before a clean re-extract`, ungated
- `wsi_common.c` — `sc_log`, comment says `REMOVE`; gated on `NVK_TRACE`
- `nvk_queue.c` `qe_log`, `nvk_image.c` image trace — gated on `NVK_TRACE`

---

## 11. Inconsistencies found

### 11.1 `apply-wsi-switch.sh` reverts two shipped fixes — the most serious

`winsys/wsi/apply-wsi-switch.sh:15` copies the tracked **616-line**
`wsi_common_switch.c` over the **640-line** version the patch just installed, reverting:

- `minImageCount = 3` → `2` (the triple-buffering fix; its own comment says 2 buffers snap
  the frame rate to 30 fps)
- the `g_zc_owner` ownership transfer (fixes a hardware-confirmed `0xf59` crash on
  swapchain recreate)

The two documents contradict each other:
- `BUILD_AND_RUN.md:65-67` — running the script is "**REQUIRED**"
- `REPRODUCE.md:52-54` — the tracked file "is **NOT** the build source — the patch is the
  source of truth"

Following the build document produces a measurably worse driver.

### 11.2 Two sources of truth for the same three Mesa files

`winsys/mesa-edits/` holds whole edited copies of `nvk_image.c`, `nvk_queue.c` and
`wsi_common.c`, all three of which the patch also modifies. `apply-wsi-switch.sh:23-26`
copies them over the patched tree. They are currently supersets of the patch — but nothing
checks that, and § 11.1 proves the failure mode is real, not theoretical.

### 11.3 Nothing is pinned

- **Mesa 25.0.7 by filename only.** No `git clone`, no commit, no checksum. `apply-patches.sh`
  requires the tree to already exist at `/work/mesa-25`; the docs say to download the
  tarball manually. `patch -p1` is used rather than `git apply`, so even the `index` blob
  hashes in the patch are never checked.
- `FROM devkitpro/devkita64` — **no tag, no digest**.
- `--default-toolchain nightly` — **no date**, while the custom Rust target JSON uses a
  schema that changes between nightlies.
- `cargo install --locked bindgen-cli cbindgen` — no version constraint.
- `libc-switch` and `libdrm_nouveau` cloned with no branch, tag or SHA.

The image is not rebuildable to the same toolchain, and will bit-rot silently.

### 11.4 Machine-specific and personal absolute paths

- `/work` is hardcoded and **non-overridable** in 25+ executed sites, including both Meson
  cross files (`rust = ['/work/rustc-switch.sh']`, `-I/work/compat`). The mount point is
  effectively part of the build ABI.
- `/opt/devkitpro` is hardcoded everywhere; `$DEVKITPRO` / `$DEVKITA64` are never used.
- `D:\switch-nvk` appears in the *executed* scripts' `docker run` lines and, worse, **inside
  the Mesa patch itself** — so a Windows path gets compiled into the driver's comments.
- Personal paths leak: `C:\Users\<name>\AppData\Roaming\...` appears at
  `docs/knowledge/SKILL-switch-port.md:114` in all four snapshots, with **two different
  human usernames** across them. (The names are deliberately not reproduced here; they are
  in the snapshots at that line if the owner needs to scrub them.)

### 11.5 Documentation out of date with its own artefacts

| Claim | Reality |
|---|---|
| `BUILD_AND_RUN.md:70` "9 files" | the master patch touches **24** |
| `BUILD_AND_RUN.md:158` `pristine-25.0.7/` is "unmodified copies of the patched files" | 9 of 24 present; re-diffing the master patch is impossible |
| `.gitignore:14` ignores `crossfiles/` | `REPRODUCE.md:23` says it is "tracked … do NOT delete"; two snapshots have it, two do not |
| `PLAN_WSI_NWINDOW.md:359-367` — 8 of 8 items unchecked | several (zero-copy, `nwindowConfigureBuffer`) are in fact implemented; `NO_BLIT` and native-fence present are not |
| `package-nvk.sh:93` recommends `_linktest_merged.sh` | the file does not exist in any snapshot, and `/_*.sh` is gitignored |
| `UNDEFINED_SYMBOLS.txt` | byte-identical in all four snapshots; lists `strrchr` and the `drm*` symbols the shim now defines — captured from a link that had no libc, never regenerated |

### 11.6 Build failures masked

`winsys/rebuild-diag.sh:9` is `ninja -C /work/mb || echo "(… failed as expected …)"`. The
`||` swallows **every** ninja failure, not only the known-broken `.so` link, then proceeds
to build an NRO from stale objects. `verify-fix.sh` has the same shape with the exit status
lost to a pipe.

### 11.7 Toolchain internal contradictions

- `LIBCLANG_PATH=/usr/lib/llvm-14/lib` while `LLVM_CONFIG=llvm-config-15` and all CLC
  tooling is LLVM-15 — bindgen runs libclang-14 against LLVM-15-era headers.
- The Dockerfile bakes host-x86 `.pc` files into the **Switch cross** pkg-config directory;
  `configure-mesa.sh:56-57`, `rebuild-diag.sh` and `verify-fix.sh` then each delete the same
  four files again. Three places fight over one directory.
- `configure-mesa.sh` generates `crossfiles/*` and then `sed -i`s two substitutions into the
  generated file. If devkitPro's generator ever changes its quoting, both `sed`s silently
  no-op and the build proceeds with a broken cross file. Nothing verifies the substitution
  landed.
- `rustc-switch.sh` silently overrides whatever `--target` Meson passed, and sets
  `RUSTC_BOOTSTRAP=1`, which disables the nightly gate — so the build appears to work on a
  stable toolchain until a `-Z` flag changes meaning.
- The small patch is a concatenation of two differently-formatted diffs: its last two
  entries (including the load-bearing FECS no-op) have no `diff --git` and no `index` line.
  `patch` accepts it; `git apply` would reject it.

### 11.8 Licence inconsistency

`master` is **GPL-2.0**; the three `switch-port/*` snapshots are **AGPL-3.0** — different
licence families, not versions. Every `README.md` claims "GPL-2.0-or-later". `master` is the
newest snapshot, so the change is a licence *downgrade* made without a note. There are no
SPDX headers anywhere, and no `COPYING`/`NOTICE`.

---

## 12. Potentially reusable material

**Nothing is being copied.** This section records what is *worth knowing*, and separates
facts (safely re-derivable) from expression (licence-encumbered).

### 12.1 Hardware and API facts — reusable as knowledge

These are observations about the Tegra X1 and the Horizon `nv` services. They are facts,
not authorship, and each is independently verifiable against switchbrew, envytools, deko3d
or by measurement on hardware.

| Fact | Why it matters |
|---|---|
| GM20B is chipset `0x12b`; 1 GPC, 2 TPC; NVK maps it to `sm_53` | physical-device reporting |
| Maxwell classes: `0xb197` 3D, `0xb1c0` compute, `0xb0b5` DMA copy, `0x902d` 2D, `0xa140` inline-to-memory | engine binding |
| Engines must be bound in-stream via `SET_OBJECT` on subchannels 0–4; only the 3D obj-ctx is kernel-allocated, and `AllocObjCtx` must not be repeated | otherwise MMU fault → channel reset |
| Every working GPFIFO entry used `NOT_MAIN | NO_PREFETCH`; NVK's default `flags = 0` failed | to be re-measured, see `docs/known-risks.md` R3 |
| `nvGpuChannelIncrFence` must be paired with an emitted syncpoint-increment command | otherwise the fence is never reached |
| The increment command's third dword carries the GPU L2 flush bit | completion implies memory visibility |
| A `FIXED` VA map is only valid inside a previously reserved non-fixed range | shapes the whole VA design |
| `nvFenceWait`'s timeout is in **microseconds** | a units bug that looks like a hang |
| `NvGraphicBuffer.header.num_ints` must be `(sizeof(NvGraphicBuffer) - sizeof(NativeHandle))/4` with `num_fds = 0` | otherwise `nwindowDequeueBuffer` blocks forever on frame 0 |
| `nvFenceInit()` is required before `nwindowDequeueBuffer` / `nvMultiFenceWait` | black screen otherwise |
| The compositor scanout kind is `NvKind_Generic_16BX2` (`0xfe`) with `NvLayout_BlockLinear` | zero-copy present |
| `NwindowConfigureBuffer` registers on the *window*, so overlapping swapchains collide with `0xf59` | drives our surface-owned registration design |
| Mesa's `vk_image.c` sets `drm_format_mod` on Linux/BSD only, so any other OS gets LINEAR images | must be patched or nothing is block-linear |
| `mtx_timedlock` needs a fallback — libnx pthread has no `pthread_mutex_timedlock` | Phase 3 |
| Privileged GR register writes from `nvk_mme_set_priv_reg` reset a homebrew channel | Phase 5 blocker to expect |

### 12.2 Structure worth imitating, to be written from scratch

- The **separation of a fence-increment command list from the caller's pushbuffer**, held in
  a per-channel command buffer, is the right shape.
- The **channel error-notifier decode** (31 = MMU fault, 25 = illegal method, 32 = PBDMA
  error, 8 = idle timeout) is exactly the diagnostic our debug mode needs.
- The **in-tree GPU selftest** (`drm_shim_selftest`) is a good idea: a driver that can prove
  the channel works before Mesa is involved. Phase 1's ten tests are the same idea, better
  scoped.

### 12.3 Not reusable

The `drm_shim.c` architecture itself — the synthetic fd, the GEM handle table, the nouveau
uAPI dispatch, the syncobj emulation — is precisely what this project exists to avoid. Its
value is entirely as a record of what the hardware accepts.

### 12.4 Attribution and licence status

| Item | Origin | Licence | Status here |
|---|---|---|---|
| `switch-nvk` `master` snapshot | third party | **GPL-2.0** | studied; **no code copied** |
| `switch-nvk` `switch-port/*` snapshots | third party | **AGPL-3.0** | studied; **no code copied** |
| Facts in § 12.1 | hardware/API behaviour | not copyrightable | recorded, to be re-verified |
| `wsi_common_switch.c` lineage | the reference's patch credits reverse-engineering of a third party's binary, citing notes at a path the repository deliberately does not publish | unclear | **not usable** — provenance cannot be audited |

The reference's own `.gitignore` excludes the reverse-engineered binary as "not
redistributable from us" while the shipped patch credits that work. Provenance of the WSI
backend therefore cannot be established from the snapshots alone. That alone rules out
reuse of that file, independently of the licence question.

Policy and consequences: `LICENSES/README.md`, `docs/known-risks.md` R1.

---

## 12.5 A second, independent reference: `nxvk` (2026-07-28)

`nxvk` (`github.com/PalindromicBreadLoaf/nxvk`, default branch `switch`, commit
`02b642f9b996b6149bc68108baed0f064c4291f5`, 2026-07-27) is an unrelated project analysed at
the owner's request, on **read access only** — a shallow clone of the `switch` branch, with
the files below read directly. It is not one of the four Phase 0 snapshots and is not
covered by the rest of this document; it is added here because it is the closest thing to a
second data point on the same hardware problem, built by a different author with a
different strategy.

**What it is.** A full fork of Mesa's own repository (not a patch series) with a
`switch/` directory for build tooling and smoke tests, plus two in-tree Vulkan/NVK
additions:

- `src/nouveau/vulkan/nvkmd/nvgpu/` — a **native `nvkmd` backend** (`nvkmd_nvgpu_dev.c`,
  `_mem.c`, `_va.c`, `_ctx.c`, `_sync.c`, `_pdev.c`, 1191 lines total) that calls libnx
  directly. No DRM/nouveau uAPI emulation anywhere — the same rejection of the `drm_shim.c`
  strategy this project makes in `CLAUDE.md`.
- `src/vulkan/wsi/wsi_switch.c` (869 lines) — a `VK_NN_vi_surface` swapchain over
  `nwindow`.

Its own `switch/README.md` states it does **not** copy GPL source from `switch-nvk` and
instead re-derives hardware behaviour from documentation — the same policy as this
project's "Licence hazard" section. File headers throughout (`nvkmd_nvgpu.h:1-4`,
`wsi_switch.c:1-4`, etc.) carry `SPDX-License-Identifier: MIT`, `Copyright © 2026
PalindromicBreadLoaf`; the repository itself reports no root `LICENSE` file
(`license: null` via the GitHub API), so the per-file SPDX header is the only stated
terms. **No text from `nxvk` is copied into this repository.** What follows is facts about
its design, verified by reading the cited files in the clone above.

### 12.5.1 Where it lands relative to our layer rules

| | `mesa-nvk-horizon` (this repo, target) | `nxvk` |
|---|---|---|
| Mesa tree | pinned checkout, untouched; changes are patches in `mesa-patches/` (`CLAUDE.md` rule 7) | Mesa forked directly; the diff against upstream lives only as this fork's commit history |
| GPU abstraction below NVK | `nvkmd_horizon` **must not** depend on libnx directly; it calls `horizon_gpu` (Vulkan-free, independently testable) | `nvkmd_nvgpu.h:14-17` includes `<switch/nvidia/{address_space,fence,gpu_channel,map}.h>` directly — no intermediate layer. `nvkmd_nvgpu_dev.c` calls `nvAddressSpaceCreate`, `nvioctlNvhostAsGpu_AllocSpace` etc. inline |
| Testability without Mesa | `horizon/` is a standalone static library with its own `.nro` tests (Phase 1, done) | no equivalent; `nvgpu/*.c` only builds as part of the full Mesa/NVK build |
| DRM/nouveau uAPI emulation | rejected outright (`CLAUDE.md` § rejected designs) | also absent — `nvkmd_nvgpu_dev_ops.get_drm_fd = NULL` (`nvkmd_nvgpu_dev.c:117`) |

Two layers vs three is a real trade-off, not just a style difference: `nxvk` gets to a
running triangle faster because there is nothing to build below NVK first, at the cost of
not being able to unit-test the GPU bring-up independently of a full Mesa/NIR/NAK build —
exactly the property Phase 1 of this project (`docs/milestones.md`) exists to buy.

### 12.5.2 Submission model — converges with this project's design, not the `switch-nvk` reference's

`nvkmd_nvgpu_exec_ctx_exec` (`nvkmd_nvgpu_ctx.c:249-283`) appends GPFIFO entries and sets
`ctx->has_pending = true`; it does not call any wait. `nvkmd_nvgpu_exec_ctx_flush`
(`:184-217`) kicks the channel off and records the resulting fence — still no wait. A CPU
wait (`nvFenceWait`, `:321`) happens only in `nvkmd_nvgpu_exec_ctx_sync` (explicit,
`:308-330`) and in `_destroy` (teardown drain, `:219-234`). This is the asynchronous-submit
model `docs/synchronization.md` specifies, and the opposite of the `switch-nvk`
reference's per-submit drain (§ 9.1 above) — independent confirmation that the design is
buildable, from an author who apparently never saw this project's docs.

One technique here is worth recording as new, not present in the `switch-nvk` audit:
`nvkmd_nvgpu_warmup_channel` (`nvkmd_nvgpu_ctx.c:49-111`) calibrates a freshly created
channel by kicking off synthetic fence-only pushbuffers of increasing size (32, 128, 512,
… 8192 dwords), CPU-waiting each one before trying the next, so a GPFIFO/ring-size fault is
pinned to the exact size that caused it at channel-creation time rather than surfacing
later during real submission. This is a bring-up diagnostic, not a per-submit cost, and it
does not conflict with `CLAUDE.md`'s "no CPU wait after every submit" — it runs once, at
`create_exec_ctx`.

One gap noted, not yet confirmed as a bug: `nvkmd_nvgpu_exec_ctx_wait`
(`nvkmd_nvgpu_ctx.c:236-246`) unconditionally returns `VK_SUCCESS` regardless of
`wait_count`/`waits`, commented as relying on single-channel in-order execution. Since NVK
on this hardware appears to expose one queue, this may never be exercised with a real
cross-context dependency — but if it ever is, the wait is silently dropped. **[claim]**,
not verified against NVK's actual queue-family count in this fork.

### 12.5.3 WSI — one `switch-nvk` defect fixed, the CPU-synchronous-present defect repeated

- **Fixed**: `wsi_switch_init_zero_copy` sets `chain->base.blit.type = WSI_SWAPCHAIN_NO_BLIT`
  (`wsi_switch.c:575`). The `switch-nvk` reference planned this and never implemented it
  (§ 8, § 1.2 above), so its "zero-copy" still ran a full `CmdCopyImageToBuffer` every
  frame. `nxvk`'s zero-copy path has no residual blit.
- **Repeated**: acquire calls `nwindowDequeueBuffer(chain->window, &slot, NULL)`
  (`wsi_switch.c:421`), and present does
  `chain->base.wsi->WaitForFences(..., true, UINT64_MAX)` (`:446-448`) **before**
  `nwindowQueueBuffer(chain->window, image_index, NULL)` (`:458`) — an unconditional,
  infinite CPU wait ahead of every present, with `NULL` passed as the fence both times
  instead of a real `NvMultiFence` the display block could wait on GPU-side. This is the
  exact pattern `docs/wsi.md` § 4 ("Present") and `docs/synchronization.md` § 7 identify as
  the reference's defect and design around.
- **Fixed count, not a range**: `caps->minImageCount = caps->maxImageCount = 3`
  (`wsi_switch.c:79-80`) — triple buffering only, no double-buffering option and no
  `nwindowSetBufferCount` call observed. `docs/wsi.md` § 2.3 plans `[2, 4]`, clamped, with
  both counts measured.

### 12.5.4 Hardware/API facts independently corroborated

These were reached by a different author working from a different codebase and toolchain,
which makes them stronger evidence than the single `switch-nvk` source:

| Fact | `nxvk` location | Matches § 12.1 entry |
|---|---|---|
| GPFIFO entries need `NOT_MAIN \| NO_PREFETCH`; a prefetching main entry faults NVK's streams | `nvkmd_nvgpu_ctx.c:268-272`, comment on the same lines | "Every working GPFIFO entry used `NOT_MAIN | NO_PREFETCH`" |
| A `FIXED` VA map must land inside a previously reserved non-fixed range | `nvkmd_nvgpu_dev.c:44` comment + `AllocSpace` before any bind | "A `FIXED` VA map is only valid inside a previously reserved non-fixed range" |
| `nvGpuChannelIncrFence` must be paired with an emitted syncpoint-increment command | `gen_fence_cmdlist` (`nvkmd_nvgpu.h`-adjacent, `nvkmd_nvgpu_ctx.c:27-35`) built and appended on every flush | "must be paired with an emitted syncpoint-increment command" |
| Timeline semaphores are not natively available; must be emulated | `nvkmd_nvgpu_pdev.c` comment "Timeline semaphores are emulated on top of the binary NvFence syncobj" (`nvkmd_nvgpu.h:31`) | matches this project's own plan in `docs/synchronization.md` § 5 |
| A large fixed-size VA arena, reserved once at device creation | `NVKMD_NVGPU_VA_ARENA_SIZE_B = 8 GiB` (`nvkmd_nvgpu.h:23`), same order of magnitude as the reference's `8 GiB / 4 KiB` arena (§ 4 above) | independent convergence on arena size |

### 12.5.5 Consequences for this project

| Finding | Response |
|---|---|
| Two-layer (`nvkmd_nvgpu` → libnx directly) gets to rendering faster | Accepted trade-off, not adopted: the three-layer split is this project's Phase 1 exit criterion, kept for testability, not abandoned for speed |
| Async submit design is independently proven buildable | No change — corroborates `docs/synchronization.md` as written |
| Real `WSI_SWAPCHAIN_NO_BLIT` zero-copy is achievable | `docs/wsi.md` § 3.1 keeps `WSI_SWAPCHAIN_NO_BLIT` as a Phase 6 requirement, not an optional stretch goal |
| CPU-synchronous present with `NULL` fences, repeating the `switch-nvk` defect | Confirms `docs/wsi.md` § 4's design (pass the real completion fence into `nwindowQueueBuffer`, no CPU wait) is worth the extra work — a second, independent implementation shows the easy path leads back to the same bug |
| Channel warm-up/calibration ramp | Worth considering for `horizon/channel/` bring-up diagnostics; not yet designed, no `STATUS.md` decision made |
| `exec_ctx_wait` ignoring cross-context waits | Recorded as a **[claim]**, open question — not a finding about our own code, noted only because `docs/synchronization.md` § 4 identifies cross-channel waits as the one facility no reference implementation gets right yet |

### 12.5.6 Attribution and licence status

| Item | Origin | Licence | Status here |
|---|---|---|---|
| `nxvk` (`nvkmd_nvgpu/`, `wsi_switch.c`, `switch/`) | third party, MIT per file SPDX header, no repository-level `LICENSE` | MIT (stated) | studied via a local read-only clone; **no code copied** |
| Facts in § 12.5.4 | hardware/API behaviour, independently re-derived by a third party | not copyrightable | recorded, to be re-verified against switchbrew/envytools/measurement as with § 12.1 |

Per `LICENSES/README.md`, any future literal reuse of `nxvk` code — even though it is MIT
and would not carry `switch-nvk`'s GPL/AGPL hazard — still requires a recorded decision in
`STATUS.md`, the upstream header, and an entry in `LICENSES/README.md`. Nothing here
proposes that; this section is knowledge, not an import.

---

## 13. Status claims in the reference — verified vs asserted

**Source-verified by this audit:** the file contents, the call graphs, the constants, the
patch contents, the inconsistencies above. All of § 1 through § 12.

**[claim] — asserted by the reference's documents, not verified here** (no hardware
available; see `docs/known-risks.md` R2):

- NVK cross-compiles and links into a Switch executable with zero unresolved symbols
- A buffer-fill smoke test passes on real Tegra hardware
- Triangle, textures (mipmaps, sRGB, BC1), indexed draws, depth and a 3D scene render on
  real hardware
- The `VK_NN_vi_surface` swapchain presents to the TV
- Zero-copy present reduced frame present cost from ~9.6 ms to ~150 µs

These are detailed and internally consistent, and the engineering notes read like a genuine
bring-up log. They are treated as **hypotheses to re-test**, never as guarantees.

---

## 14. Consequences for this project

| Finding | Design response |
|---|---|
| DRM shim architecture (§ 8) | Native `nvkmd_horizon`; no DRM emulation. `CLAUDE.md` § rejected designs |
| Global `g_dev`, `g_zc_owner`, 9 mutable statics | Explicit contexts everywhere; surface owns registration (`docs/wsi.md` § 2.5) |
| Drain after every submit (§ 9) | Asynchronous submit + fence retirement (`docs/synchronization.md`) |
| CPU-side semaphore waits forcing `vkQueueWaitIdle` | GPU-side syncpoint waits (`docs/synchronization.md` § 4) |
| Stale `bo->gpu_va` double-unmap (§ 5) | Mapping is a first-class owned object (`docs/memory-model.md` § 2) |
| Cached memory registered as uncached, no cache maintenance | Explicit cache policy + flush/invalidate (`docs/memory-model.md` § 5) |
| Syncpoint wraparound unhandled | Wrap-safe comparison + 64-bit shadow (`docs/synchronization.md` § 1.1) |
| Errors logged but success returned | Every function returns a result; nothing is swallowed |
| Two sources of truth for Mesa files (§ 11.2) | Patches only; `mesa/` is a pinned checkout, never our source |
| Nothing pinned (§ 11.3) | `toolchain/versions.env`; reproducibility is a Phase 2 exit criterion |
| Absolute/personal paths (§ 11.4) | `scripts/check-no-abs-paths.sh` gate |
| Build failures masked (§ 11.6) | No `|| echo`; scripts fail loudly |
| Licence conflict (§ 11.8) | MIT project, no reuse (`LICENSES/README.md`) |

---

## Appendix — commands used for this audit

```sh
# extraction (read-only; the archives themselves were not modified)
unzip -q -o <archive>.zip -d <alias>

# variant comparison
find <tree> -type f -print0 | xargs -0 md5sum        # per-file hash matrix
diff -rq <treeA> <treeB>                             # tree-level differences

# patch comparison
grep -E '^diff --git' patches/*.patch                # files touched by each patch

# extraction of the patch-embedded WSI backend for diffing against the tracked copy
#   (python: collect '+' lines following '+++ b/src/vulkan/wsi/wsi_common_switch.c')
diff -u winsys/wsi/wsi_common_switch.c wsi_from_patch.c
```

## Appendix — commands used for § 12.5 (`nxvk`)

```sh
# read-only shallow clone of the branch actually used for builds (not 'main')
git clone --depth 1 --branch switch https://github.com/PalindromicBreadLoaf/nxvk.git

# commit audited
git -C nxvk log -1 --format='%H %ci'
# 02b642f9b996b6149bc68108baed0f064c4291f5 2026-07-27 11:55:09 -0400

# files read directly (Read tool, not a summarising fetch) to produce every
# quoted line number in § 12.5:
#   src/nouveau/vulkan/nvkmd/nvgpu/nvkmd_nvgpu.h
#   src/nouveau/vulkan/nvkmd/nvgpu/nvkmd_nvgpu_ctx.c
#   src/nouveau/vulkan/nvkmd/nvgpu/nvkmd_nvgpu_dev.c
#   src/nouveau/vulkan/nvkmd/nvgpu/nvkmd_nvgpu_sync.c
#   src/vulkan/wsi/wsi_switch.c (surface caps, acquire, present, zero-copy init)
```
