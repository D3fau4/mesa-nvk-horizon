# Memory model

The reference ports conflate several distinct concepts behind one `struct shim_bo`
(`winsys/drm_shim.c:95-104`), which is where most of their memory bugs come from —
notably a stale GPU VA that survives an unmap and is unmapped a second time at close
(`drm_shim.c:783-786` vs `:580-581`). This document names each concept separately and
fixes the rules that bind them.

---

## 1. The distinct concepts

| # | Concept | Type | What it is | What it is **not** |
|---|---|---|---|---|
| 1 | **CPU address** | `void *` | A pointer the ARM cores can dereference. | Not a GPU address. Not stable across a remap. |
| 2 | **Aligned host allocation** | `void *` + `size_t` | Page-aligned, page-sized storage obtained from the heap. Owns real pages. | Not yet visible to the GPU at all. |
| 3 | **`NvMap` object** | `NvMap` | A kernel-side memory object registered over (2). The unit the `nv` driver understands. | Not a mapping. Registering does not give the GPU an address. |
| 4 | **`NvMap` id** | `uint32_t` | A process-external identifier for (3), used to hand the buffer to another service (the VI compositor). Obtained via `nvMapGetId`. | Not the `NvMap` *handle*. The handle is the in-process kernel reference used for mapping. |
| 5 | **GPU virtual address** | `uint64_t` | An address inside the channel's address space, produced by mapping (3) at a chosen VA. | Not a physical address. Not an offset. Meaningless outside its address space. |
| 6 | **Offset** | `uint64_t` | A byte displacement *within* an `NvMap` object. VM binds map `NvMap[offset .. offset+range)`. | Not a GPU VA and not a CPU pointer. |
| 7 | **Pitch / row stride** | `uint32_t` bytes | Bytes between the start of consecutive rows of an image. | Not width. Not width × bpp in general. Not "stride in pixels" — the `NvGraphicBuffer.stride` field *is* in pixels, and that is a different quantity. |
| 8 | **Block height** | `uint32_t` (log2) | Block-linear tiling parameter: the number of 8-row GOBs stacked vertically per block, expressed as log2. | Not the image height. Not the PTE kind. |
| 9 | **PTE kind** | `NvKind` (`uint8_t`) | The page-table attribute telling the MMU/compression how bytes in that page are laid out. Set per *mapping*, not per allocation. | Not the tiling layout by itself, and **not** the same as the display kind handed to the compositor. |
| 10 | **Cache coherency state** | — | Whether ARM D-cache and GPU L2 agree on the contents. | Not implied by a successful map, a completed fence, or a `HOST_COHERENT` Vulkan memory type. |

### 1.1 The two "kind" values are different things

The reference sets the page-table kind from NVK's `pte_kind`
(`drm_shim.c:805`) but hands the compositor an unconditional
`NvKind_Generic_16BX2` (`0xfe`) (`wsi_common_switch.c:97`), and never cross-checks them.
We keep both, name them separately, and validate:

- `pte_kind` — what the GPU MMU uses for the mapping.
- `display_kind` — what the VI compositor is told about the scanout surface.

`horizon/surface/` is the only place allowed to derive one from the other, and it must
reject combinations it cannot prove are compatible rather than forcing `0xfe`.

---

## 2. The binding chain

```
  (2) aligned host allocation
        │  nvMapCreate(ptr, size, align, kind, cacheable)
        ▼
  (3) NvMap object ──── nvMapGetId ────▶ (4) NvMap id   [for the compositor]
        │
        │  map(address_space, nvmap_handle, offset, range, va, pte_kind, page_size)
        ▼
  (5) GPU virtual address
```

Each arrow is an explicit, separately-destroyable step. No step is implied by another.

```c
horizon_gpu_mem      *mem;  /* owns (2) and (3)                    */
horizon_gpu_va_range *va;   /* owns a reserved interval of (5)     */
horizon_gpu_mapping  *map;  /* owns the binding mem[offset,range) → va */
```

Destroying `map` invalidates the binding **and clears the recorded VA on `mem`**. This is
precisely the invariant the reference violates.

---

## 3. GPU address space layout

Tegra's `nvgpu` rejects a bare fixed-VA reservation: a `FIXED` map is only legal inside a
range previously reserved non-fixed. The reference discovered this and reserves one 8 GiB
small-page arena at bring-up (`drm_shim.c:181-185`), then points NVK's VA heap at it via a
patch. We keep the constraint but not the single hardcoded arena:

- `horizon_gpu_vm_reserve(dev, size, page_size, align, &range)` reserves non-fixed and
  returns the base the kernel chose. Multiple ranges may exist.
- `horizon_gpu_vm_map(range, offset_in_range, mem, mem_offset, size, pte_kind)` performs a
  `FIXED` map strictly inside a reservation the caller owns.
- Mapping outside any reservation is a programming error, detected and reported, not
  attempted.

### 3.1 Page sizes

The address space has a small-page half (4 KiB) and a big-page half. The big-page size is
**queried** from `nvGpuGetCharacteristics()->big_page_size`; the reference falls back to a
hardcoded `0x10000` when the query returns NULL (`drm_shim.c:170-171`), while its own notes
say the Tegra big-page half is 128 KiB — a contradiction we do not inherit. `horizon/device/`
fails device creation if the characteristics query fails, rather than guessing.

The reference hardcodes 4 KiB for *every* bind (`drm_shim.c:810`) and its own notes list
that as an open suspect for a GR fault. Our `horizon_gpu_vm_map` takes the page size from
the reservation that contains the VA, so a big-page reservation produces big-page maps.

### 3.2 VA is never reused while a mapping may be live

The VA allocator returns a range to the free pool only after:
1. the mapping has been destroyed, and
2. every submit that referenced it has retired (see `docs/synchronization.md` § 3).

---

## 4. Alignment and overflow

- Every allocation size is rounded up to the page size; the rounding is checked for
  overflow before it is performed, not after.
- Alignment must be a power of two and at least the page size; anything else is rejected.
- `offset + range <= mem->size` is checked on every map. The reference does not check this.
- `NvGraphicBuffer` carries 32-bit size and offset fields. The reference truncates a
  `uint64_t` NIL size into them without a range check
  (`wsi_common_switch.c:89, :96`). `horizon/surface/` validates the value fits before
  narrowing, and fails the surface otherwise.
- Row stride in **bytes** and the graphic-buffer stride in **pixels** are separate fields
  with separate types. The reference computes `row_stride_B / 4`, silently assuming
  4 bytes per pixel (`wsi_common_switch.c:88`). We compute pixels from bytes using the
  format's actual block size and reject formats where the division is not exact.

---

## 5. Cache coherency

**GM20B is not IO-coherent.** The ARM D-cache and the GPU do not snoop each other.

The reference's position is inconsistent in a way that matters:

- Backing store is ordinary cached heap memory: `memalign(0x1000, size)`
  (`drm_shim.c:455`).
- It is registered with `nvMapCreate(..., cacheable = false)` at **both** call sites
  (`drm_shim.c:460`, called from `:479` and `:506`) — declaring to `nvmap` the opposite of
  what the memory actually is.
- The winsys performs **no** CPU cache maintenance at all. `armDCacheInvalidate` and
  `armDCacheFlushInvalidate` appear nowhere in the tree; there is exactly one
  `armDCacheFlush`, in the WSI copy fallback (`wsi_common_switch.c:394`), whose comment
  says "invalidate".
- `nouveau_gem_cpu_prep` — the natural place for an invalidate before a CPU read — is a
  documented no-op (`drm_shim.c:561-567`).
- The substitute is a GPU L2 flush bit inside the fence command list
  (`drm_shim.c:616-622`). That makes GPU writes visible to *memory*; it cannot invalidate
  the ARM D-cache, so a CPU read of GPU-written data can still hit a stale line. The smoke
  tests compensate by hand, which is an application working around a driver gap.

### Our rules

1. Cacheability is declared **once**, at `NvMap` creation, and must match how the memory is
   actually allocated and used. `horizon_gpu_mem_create` takes an explicit
   `horizon_gpu_cache_policy` and records it on the object.
2. **CPU writes → GPU reads:** `horizon_gpu_mem_flush(mem, offset, size)` before the submit
   that reads it. Required for cached memory; a no-op for uncached, decided from the
   recorded policy, not from the call site.
3. **GPU writes → CPU reads:** `horizon_gpu_mem_invalidate(mem, offset, size)` after the
   fence for the writing submit has been reached. Required for cached memory.
4. Both take an explicit range. Whole-object maintenance is available but must be a
   deliberate choice — the reference flushes ~3.5 MB per frame
   (`wsi_common_switch.c:393`).
5. The GPU-side L2 flush stays in the fence command list — it is necessary — but it is
   documented as covering GPU→memory only, never as a substitute for (3).
6. Vulkan `HOST_COHERENT` memory types are only advertised for memory we can genuinely make
   coherent. If that means advertising uncached memory for `HOST_COHERENT` and cached
   memory only for `HOST_VISIBLE | HOST_CACHED`, that is the correct trade, and it is
   recorded in `STATUS.md` as a measured decision.

---

## 6. Tiling

- **Linear / pitch**: rows are `pitch` bytes apart; `pte_kind = NvKind_Pitch (0)`.
- **Block-linear**: the Maxwell GOB is 64 bytes × 8 rows. Blocks stack vertically by
  `1 << block_height_log2` GOBs. The PTE kind encodes the format-specific block-linear
  layout.

This project does **not** compute tiling itself. NIL (Mesa's NVIDIA image library) owns
that computation, and `horizon/` receives already-computed values: `row_stride_B`,
`block_height_log2` (`tiling.y_log2`), `offset_B`, `size_B`, `pte_kind`. `horizon/` treats
them as opaque and validates ranges; it never second-guesses NIL.

One upstream Mesa dependency is required for this to work at all and must be reproduced in
`mesa-patches/`: `vk_image.c`/`vk_image.h` set `drm_format_mod` on Linux/BSD only, leaving
it `0` (= `DRM_FORMAT_MOD_LINEAR`) elsewhere, which forces NIL to make every image linear.
Horizon must be added to that condition (reference: `patches/switch-nvk-mesa-25.0.7.patch`,
`vk_image.c`/`vk_image.h` hunks). This is a fact about Mesa, independently verifiable and
re-derivable; we write our own patch.

---

## 7. Ownership

| Object | Owner | Destroyed by | Destroys |
|---|---|---|---|
| `horizon_gpu_device` | caller | `horizon_gpu_device_destroy` | address space, `nv` session — **asserts** no live children |
| `horizon_gpu_mem` | caller | `horizon_gpu_mem_destroy` | `NvMap`, host allocation — **asserts** no live mappings |
| `horizon_gpu_va_range` | caller | `horizon_gpu_vm_release` | the reservation — **asserts** no live mappings |
| `horizon_gpu_mapping` | caller | `horizon_gpu_vm_unmap` | the binding; clears `mem`'s recorded VA |
| `horizon_gpu_channel` | caller | `horizon_gpu_channel_destroy` | channel, its cmdbuf and Zcull memory — **asserts** all submits retired |

Rules:

- A CPU pointer obtained from a `horizon_gpu_mem` is valid exactly until that object is
  destroyed. There is no separate map/unmap of the CPU view, so there is no window in
  which a stale pointer is legitimate. The reference exposes `drm_shim_mmap` returning the
  raw backing pointer and makes `munmap` an unconditional no-op
  (`drm_shim.c:310-326`), leaving any retained pointer dangling after close.
- Destroying an object with live children is a programming error: it asserts in debug
  builds and returns an error in release builds. It never silently leaks and never
  silently succeeds.
- Teardown is symmetric: whatever `create` did in order, `destroy` undoes in reverse. The
  reference's teardown only unmaps memory it mapped itself, leaving NVK-bound memory to be
  cleaned up implicitly by closing the address space (`drm_shim.c:222-235`).

## 8. Leak accounting

`horizon_gpu_device` maintains counters of live `NvMap` objects, reservations, mappings and
channels. `horizon_gpu_device_destroy` logs any non-zero counter and returns an error.
Phase 1 test 10 asserts all counters return to zero, and that a second full
create/destroy cycle in the same process succeeds.
