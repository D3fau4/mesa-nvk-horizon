/*
 * horizon_gpu — memory objects: aligned CPU backing + NvMap registration.
 *
 * A horizon_gpu_mem owns concepts (2) and (3) of memory-model § 1: the
 * page-aligned host allocation and the NvMap object registered over it.
 * It is NOT a GPU mapping — mapping at a GPU VA is a separate, explicit
 * step through horizon_gpu/vm.h, and the PTE kind is a property of the
 * *mapping*, not of this object (memory-model § 1 #9).
 *
 * The NvMap *id* (for handing a buffer to another service, e.g. the VI
 * compositor) and the NvMap *handle* (the in-process reference used for
 * mapping) are distinct and exposed separately (memory-model § 1 #4).
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef HORIZON_GPU_MEMORY_H
#define HORIZON_GPU_MEMORY_H

#include <stdbool.h>
#include <stdint.h>

#include "device.h"
#include "result.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct horizon_gpu_mem horizon_gpu_mem;

/* Minimum alignment/granularity of nvmap-backed memory: the small page.
 * Source: NVGPU_AS GetVARegions small-page region on GM20B and the nvmap
 * ioctl contract (switchbrew "NV services", /dev/nvmap NVMAP_IOC_ALLOC
 * align field). */
#define HORIZON_GPU_SMALL_PAGE_SIZE UINT64_C(0x1000)

/* How the CPU side of the memory is cached. Declared once, at creation,
 * and it must match how the memory is actually allocated (memory-model
 * § 5 rule 1 — the reference registers cached heap memory as uncached at
 * every call site, which we do not repeat).
 *
 * Homebrew heap memory on Horizon is CPU-cached when it is allocated, so
 * CACHED describes the storage as it comes. UNCACHED asks the kernel to
 * change that for the range, with svcSetMemoryAttribute and
 * MemoryAttribute_Uncached — the same mechanism deko3d uses for its
 * CpuUncached memory blocks.
 *
 * WHY BOTH EXIST, and it is not a preference. Vulkan requires at least
 * one memory type with HOST_VISIBLE and HOST_COHERENT
 * (VkPhysicalDeviceMemoryProperties), and NVK advertises one on an SoC
 * meaning exactly an uncached map:
 *
 *   /​* On Tegra, we only have sysmem ... The only difference in memory
 *      types is between cached and uncached (but coherent) maps. *​/
 *          -- nvk_physical_device.c:1571-1575
 *
 * With only CACHED, that memory type is a promise the platform cannot
 * keep: NVKMD_MEM_COHERENT makes nvkmd skip cache maintenance entirely
 * (nvkmd.c:457), so nothing would ever flush or invalidate and neither
 * side would see the other's writes. Decision D14. */
typedef enum horizon_gpu_cache_policy {
    HORIZON_GPU_MEM_CACHED = 1,
    HORIZON_GPU_MEM_UNCACHED = 2,
} horizon_gpu_cache_policy;

/* Which policy this object was created with. Needed by callers that must
 * decide whether cache maintenance is required at all — for UNCACHED
 * memory the flush and invalidate below are no-ops, and doing them would
 * be wasted work rather than wrong. */
horizon_gpu_cache_policy
horizon_gpu_mem_policy(const horizon_gpu_mem *mem);

/* Allocates zero-initialised, page-aligned storage of `size` bytes rounded
 * up to `align` (power of two, >= HORIZON_GPU_SMALL_PAGE_SIZE; 0 means
 * page alignment) and registers it with nvmap. Every rounding is
 * overflow-checked before use. The caller owns the object. */
horizon_gpu_result horizon_gpu_mem_create(horizon_gpu_device *dev,
                                          uint64_t size, uint64_t align,
                                          horizon_gpu_cache_policy policy,
                                          horizon_gpu_mem **out_mem);

/* The same, WITHOUT the zero fill. Everything else — the rounding, the
 * nvmap registration, the cache maintenance, the error paths and who
 * owns what — is identical.
 *
 * WHAT THE CALLER IS ASSERTING, and it is not "I do not mind what is in
 * there". It is: every byte anything will ever read from this object is
 * written first, by this process or by the GPU, before it is read. A
 * consumer that reads a byte it never wrote gets whatever the heap held
 * before — this process's own earlier allocations, since aligned_alloc
 * hands back memory the process has used. Nothing crosses a process
 * boundary, but "our own stale data" and "zero" are different answers
 * and only one of them is deterministic.
 *
 * WHAT IS *NOT* SKIPPED, and this is the part that makes the path safe
 * rather than merely fast. The armDCacheFlush that follows the fill in
 * horizon_gpu_mem_create is not the fill's cleanup: it is there because
 * the heap this allocation came from may hold DIRTY CACHE LINES from
 * its previous tenant, and a dirty line is a write that has not
 * happened yet. Left alone it lands later, at an eviction nobody chose,
 * on top of whatever the GPU has since written there — which is the
 * silent wrong answer measured on a console on 2026-08-24. That flush
 * happens on this path too, and unconditionally. Skipping the fill
 * makes it cheaper as a side effect: a line nothing wrote is not dirty,
 * so there is nothing to write back.
 *
 * NOT THE DEFAULT, AND NOT A TUNABLE. There is no environment variable
 * that turns this on for allocations that did not ask for it; a caller
 * either promises the above at the call site or does not use it. See
 * horizon/memory/mem.c for the audit of who in this tree does. */
horizon_gpu_result
horizon_gpu_mem_create_uninit(horizon_gpu_device *dev, uint64_t size,
                              uint64_t align, horizon_gpu_cache_policy policy,
                              horizon_gpu_mem **out_mem);

/* Fails with HORIZON_GPU_ERR_BUSY while GPU mappings of this object are
 * alive. Closing invalidates the CPU pointer (memory-model § 7).
 *
 * TWO FAILURES WITH OPPOSITE MEANINGS, because the caller must not treat
 * them alike:
 *
 *   HORIZON_GPU_ERR_BUSY — nothing happened. The object is intact and
 *     still owned by the caller; unmap its mappings and call again.
 *
 *   HORIZON_GPU_ERR_NV — the object is GONE and must NOT be passed here
 *     again; a second call is a use-after-free, since `mem` has exactly
 *     one owner (memory-model § 7). It reports that restoring an
 *     UNCACHED range's cacheability failed, so the backing pages were
 *     deliberately leaked rather than returned to the heap uncached,
 *     where they would corrupt whatever allocated them next. Only an
 *     UNCACHED object can answer this.
 *
 * BUSY is therefore the only retryable one. Everything else means the
 * destroy completed, and the error describes what it cost. */
horizon_gpu_result horizon_gpu_mem_destroy(horizon_gpu_mem *mem);

void *horizon_gpu_mem_cpu_ptr(const horizon_gpu_mem *mem);
uint64_t horizon_gpu_mem_size(const horizon_gpu_mem *mem);

/* Process-external NvMap id (concept #4) — for other services. */
uint32_t horizon_gpu_mem_get_id(const horizon_gpu_mem *mem);
/* In-process NvMap handle (concept #3) — for GPU mapping. */
uint32_t horizon_gpu_mem_get_handle(const horizon_gpu_mem *mem);

/* GPU VA of the most recent live mapping of this object, 0 when none.
 * horizon_gpu_vm_unmap clears it — the invariant the reference violates
 * (memory-model § 2). */
uint64_t horizon_gpu_mem_mapped_va(const horizon_gpu_mem *mem);

/* CPU cache maintenance over an explicit byte range (memory-model § 5).
 * flush: make CPU writes visible to the GPU (call before the submit that
 * reads them). invalidate: make GPU writes visible to CPU reads (call
 * after the writing submit's fence has been reached). Both are no-ops for
 * policies that do not require them — decided from the recorded policy,
 * not the call site. */
horizon_gpu_result horizon_gpu_mem_flush(horizon_gpu_mem *mem,
                                         uint64_t offset, uint64_t size);
horizon_gpu_result horizon_gpu_mem_invalidate(horizon_gpu_mem *mem,
                                              uint64_t offset, uint64_t size);

/* One region of this process's address space, as the kernel describes
 * it. The three flag words are the kernel's own values, passed through
 * rather than interpreted, so a log carrying them can be read against
 * libnx's MemoryState/MemoryAttribute/Permission lists. */
typedef struct horizon_gpu_heap_region {
    uint64_t addr;
    uint64_t size;
    uint32_t type;
    uint32_t attr;
    uint32_t perm;
} horizon_gpu_heap_region;

/* Whether every page of [p, p + size) is memory THIS process may write.
 *
 * WHY A GPU ALLOCATOR HAS TO ASK. nx-hbloader runs one .nro after
 * another in the SAME process and hands each of them the same heap, so
 * whatever the previous program left behind is inside the next
 * program's malloc arena. A page lent to another process — a
 * TransferMemory made from heap, which is how libnx gives the socket
 * and audio services their buffers — reads back as
 * `attr=IsBorrowed perm=None`, and it is still borrowed if the program
 * that lent it died before closing it. malloc knows nothing about any
 * of that and will hand the range out again.
 *
 * Writing such a page is a CPU Data Abort, and so is cleaning it: on
 * aarch64 `dc civac` faults exactly as a store does. That is the whole
 * of the "Godot dies right after the Vulkan API line" crash — it landed
 * in the zero fill while there was one, and moved to the cache flush
 * when the fill was skipped, because the flush runs on both paths.
 *
 * `bad` is filled in with the offending region when the answer is
 * false *because a page was rejected*, and left alone when the answer
 * is true. A range the kernel refuses to describe is false as well:
 * not being able to prove a page is ours is the same as knowing it is
 * not, and `bad` then names the address the query failed at.
 *
 * THE TWO TRIVIAL REFUSALS LEAVE `bad` ALONE, and they are false
 * rather than true on purpose: a null pointer, a zero size, and a
 * range whose end wraps are all "this is not a range we made", and
 * there is no offending region to name. No caller in this tree reaches
 * them; said here so the next one does not read `bad` after them.
 */
bool horizon_gpu_heap_range_is_ours(const void *p, uint64_t size,
                                    horizon_gpu_heap_region *bad);

/* How much heap horizon_gpu has set aside because it failed the check
 * above, and how many blocks that is. Both only ever grow: a block that
 * fails is never freed, because freeing it returns it to the allocator
 * that just offered it. Zero on a healthy launch. */
void horizon_gpu_heap_quarantine_stats(uint64_t *bytes, uint32_t *blocks);

/* How many regions of this process's address space are lent to another
 * process, their total size, and the first of them.
 *
 * A launch that answers 0 was handed a clean heap. A launch that
 * answers more than that inherited a borrow from whatever ran in this
 * process before it — hbloader runs each .nro in the same process — and
 * that is the condition every allocation here then has to be checked
 * against. Called once at device creation so a log carries the answer
 * without a test having to be run; costs one syscall per region and
 * changes nothing. `first` may be NULL. */
uint32_t horizon_gpu_heap_borrowed_regions(uint64_t *bytes,
                                           horizon_gpu_heap_region *first);

#ifdef __cplusplus
}
#endif

#endif /* HORIZON_GPU_MEMORY_H */
