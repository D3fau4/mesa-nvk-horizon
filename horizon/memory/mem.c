/*
 * horizon_gpu — memory object implementation.
 *
 * Ownership (memory-model § 7): horizon_gpu_mem owns the host allocation
 * and the NvMap; destroy refuses while mappings are alive, then closes the
 * NvMap and frees the backing, in that (reverse-of-create) order.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>

#include "mem_priv.h"
#include "align.h"
#include "../device/device_priv.h"

horizon_gpu_result horizon_gpu_mem_create(horizon_gpu_device *dev,
                                          uint64_t size, uint64_t align,
                                          horizon_gpu_cache_policy policy,
                                          horizon_gpu_mem **out_mem)
{
    if (!dev || !out_mem || size == 0)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);
    if (policy != HORIZON_GPU_MEM_CACHED &&
        policy != HORIZON_GPU_MEM_UNCACHED)
        return horizon_gpu_err(HORIZON_GPU_ERR_UNSUPPORTED);

    if (align == 0)
        align = HORIZON_GPU_SMALL_PAGE_SIZE;
    if (!horizon_is_pow2_u64(align) || align < HORIZON_GPU_SMALL_PAGE_SIZE)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);

    /* Round the size up to the alignment, overflow-checked before use
     * (aligned_alloc additionally requires size % align == 0). */
    uint64_t rounded;
    if (!horizon_align_up_u64(size, align, &rounded))
        return horizon_gpu_err(HORIZON_GPU_ERR_OVERFLOW);

    /* nvMapCreate takes 32-bit size and alignment; reject rather than
     * truncate (memory-model § 4). */
    if (rounded > UINT32_MAX || align > UINT32_MAX)
        return horizon_gpu_err(HORIZON_GPU_ERR_OVERFLOW);

    horizon_gpu_mem *mem = calloc(1, sizeof(*mem));
    if (!mem)
        return horizon_gpu_err(HORIZON_GPU_ERR_OUT_OF_MEMORY);

    mem->dev = dev;
    mem->size = rounded;
    mem->align = align;
    mem->policy = policy;

    mem->cpu = aligned_alloc(align, rounded);
    if (!mem->cpu) {
        free(mem);
        return horizon_gpu_err(HORIZON_GPU_ERR_OUT_OF_MEMORY);
    }
    /* Zero-fill: deterministic content for tests and no stale data handed
     * to the GPU. */
    memset(mem->cpu, 0, rounded);

    /* AND THE ZEROING HAS TO REACH MEMORY, FOR EVERY POLICY.
     *
     * The memset above went through the CPU cache and left every line
     * of this object dirty. aligned_alloc hands back heap the process
     * has used before, so some of those lines were dirty already. A
     * dirty line is a write that has not happened yet, and it will
     * happen later, at an eviction nobody chose.
     *
     * For an object the GPU writes and the CPU then reads — a Vulkan
     * query pool is exactly that, and it is the first thing in this
     * project to be one — the consequence is a silent wrong answer.
     * The GPU writes the value to memory; the CPU invalidates before
     * reading, and the invalidate on aarch64 is `dc civac`, which
     * CLEANS the line before invalidating it. Cleaning a line still
     * holding the memset's zeros writes those zeros over what the GPU
     * just wrote, and the read that follows returns them.
     *
     * MEASURED ON A CONSOLE 2026-08-24: t_vk_timestamp's
     * vkGetQueryPoolResults answered VK_NOT_READY for two full seconds
     * of polling, on some runs and not others — the run-to-run
     * difference being whether those lines happened to have been
     * evicted in the meantime. With this flush it stops happening.
     *
     * This used to run only for UNCACHED, where the same hazard is
     * sharper still: after svcSetMemoryAttribute remaps the range, a
     * dirty line evicted later lands on top of whatever was written
     * uncached in between. Both are the same bug and one flush answers
     * both. armDCacheFlush cleans and invalidates, which is what is
     * wanted: nothing of this object should survive in the cache.
     */
    armDCacheFlush(mem->cpu, rounded);

    /* UNCACHED: hand the range to the kernel to remap without CPU
     * caching.
     *
     * svcSetMemoryAttribute wants a page-aligned range; `align` is at
     * least HORIZON_GPU_SMALL_PAGE_SIZE and `rounded` is a multiple of
     * it, so both hold by construction rather than by check.
     *
     * Failure is not survivable half-done: the storage would be
     * allocated with a cache policy that does not match what the caller
     * asked for, which is exactly the mismatch memory-model § 5 rule 1
     * exists to forbid. So it unwinds. */
    if (policy == HORIZON_GPU_MEM_UNCACHED) {
        Result arc = svcSetMemoryAttribute(mem->cpu, rounded,
                                           MemAttr_IsUncached,
                                           MemAttr_IsUncached);
        if (R_FAILED(arc)) {
            horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                         "svcSetMemoryAttribute(uncached, %p, 0x%llx) "
                         "failed: 0x%08x", mem->cpu,
                         (unsigned long long)rounded, arc);
            free(mem->cpu);
            free(mem);
            return horizon_gpu_err_nv(arc);
        }
    }

    /* The NvMap kind is NvKind_Pitch here; block-linear layouts are a
     * property of each GPU *mapping* (its PTE kind), never of the memory
     * object (memory-model § 1 #9). is_cpu_cacheable tells nvmap the
     * truth about this range — which is now the policy the caller asked
     * for, not an assumption about heap memory, and unlike the
     * reference's cacheable=false at every call site (drm_shim.c:460). */
    Result rc = nvMapCreate(&mem->nvmap, mem->cpu, (u32)rounded, (u32)align,
                            NvKind_Pitch,
                            policy == HORIZON_GPU_MEM_CACHED);
    if (R_FAILED(rc)) {
        horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                     "nvMapCreate(size=0x%llx align=0x%llx) failed: 0x%08x",
                     (unsigned long long)rounded, (unsigned long long)align,
                     rc);
        /* Reverse order: the attribute was set after the allocation, so
         * it comes off before the storage goes back. Returning uncached
         * memory to the heap would leave every later allocation that
         * reuses it silently uncached.
         *
         * Which means the restore's own Result decides whether the
         * storage may go back at all, so it is checked rather than
         * issued and forgotten (CLAUDE.md: never discard a libnx
         * Result). If it fails, the pages are deliberately *not* freed:
         * a leak costs this process some address space, while handing
         * uncached pages to malloc costs every later allocation that
         * lands on them, in unrelated code, with no symptom near the
         * cause. Of the two, the leak is the one that can be found. */
        if (policy == HORIZON_GPU_MEM_UNCACHED) {
            Result arc = svcSetMemoryAttribute(mem->cpu, rounded,
                                               MemAttr_IsUncached, 0);
            if (R_FAILED(arc)) {
                /* Logged, not returned. The caller asked why the
                 * allocation failed, and the answer is `rc` from
                 * nvMapCreate; returning the cleanup's error instead
                 * would replace the diagnosis with a footnote about the
                 * unwind. Both appear here, and the one that answers
                 * the caller's question is the one that propagates. */
                horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                             "svcSetMemoryAttribute(restore cached, %p, "
                             "0x%llx) failed: 0x%08x — leaking 0x%llx "
                             "bytes rather than returning uncached pages "
                             "to the heap (unwinding an nvMapCreate that "
                             "failed with 0x%08x)", mem->cpu,
                             (unsigned long long)rounded, arc,
                             (unsigned long long)rounded, rc);
                free(mem);
                return horizon_gpu_err_nv(rc);
            }
        }
        free(mem->cpu);
        free(mem);
        return horizon_gpu_err_nv(rc);
    }

    atomic_fetch_add(&dev->live_mem, 1);
    horizon_logf(&dev->log, HORIZON_LOG_DEBUG,
                 "mem %p: created size=0x%llx align=0x%llx handle=%u id=%u",
                 (void *)mem, (unsigned long long)rounded,
                 (unsigned long long)align, mem->nvmap.handle, mem->nvmap.id);

    *out_mem = mem;
    return horizon_gpu_ok();
}

horizon_gpu_result horizon_gpu_mem_destroy(horizon_gpu_mem *mem)
{
    if (!mem)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);

    uint32_t live = atomic_load(&mem->live_mappings);
    if (live != 0) {
        horizon_logf(&mem->dev->log, HORIZON_LOG_ERROR,
                     "mem %p: destroy refused, %u live mapping(s) at "
                     "va=0x%llx", (void *)mem, live,
                     (unsigned long long)mem->mapped_va);
        return horizon_gpu_err(HORIZON_GPU_ERR_BUSY);
    }

    /* `mem` has exactly one documented owner (memory-model § 7): a second
     * destroy call on the same pointer is a caller bug, not a case this
     * layer defends against — `mem` is freed below, so any check reading
     * back through the pointer afterwards would itself be a use-after-free. */
    nvMapClose(&mem->nvmap);
    /* Same reason as the error path in create: the heap gets its pages
     * back as it lent them, cached. Leaving the attribute on would make
     * every later allocation that reuses this address silently uncached
     * — a fault that appears in unrelated code, long afterwards.
     *
     * And, as there, the restore's Result decides whether the pages may
     * go back: on failure they are leaked on purpose and the caller is
     * told. The object itself is destroyed either way — nvMapClose has
     * already run and this must not be retried — so the returned error
     * means "destroyed, and it cost you the backing", not "try again".
     */
    if (mem->policy == HORIZON_GPU_MEM_UNCACHED) {
        Result arc = svcSetMemoryAttribute(mem->cpu, mem->size,
                                           MemAttr_IsUncached, 0);
        if (R_FAILED(arc)) {
            horizon_logf(&mem->dev->log, HORIZON_LOG_ERROR,
                         "mem %p: svcSetMemoryAttribute(restore cached, "
                         "%p, 0x%llx) failed: 0x%08x — leaking 0x%llx "
                         "bytes rather than returning uncached pages to "
                         "the heap", (void *)mem, mem->cpu,
                         (unsigned long long)mem->size, arc,
                         (unsigned long long)mem->size);
            atomic_fetch_sub(&mem->dev->live_mem, 1);
            free(mem);
            return horizon_gpu_err_nv(arc);
        }
    }
    free(mem->cpu);
    atomic_fetch_sub(&mem->dev->live_mem, 1);
    free(mem);
    return horizon_gpu_ok();
}

horizon_gpu_cache_policy horizon_gpu_mem_policy(const horizon_gpu_mem *mem)
{
    return mem ? mem->policy : HORIZON_GPU_MEM_CACHED;
}

void *horizon_gpu_mem_cpu_ptr(const horizon_gpu_mem *mem)
{
    return mem ? mem->cpu : NULL;
}

uint64_t horizon_gpu_mem_size(const horizon_gpu_mem *mem)
{
    return mem ? mem->size : 0;
}

uint32_t horizon_gpu_mem_get_id(const horizon_gpu_mem *mem)
{
    return mem ? mem->nvmap.id : 0;
}

uint32_t horizon_gpu_mem_get_handle(const horizon_gpu_mem *mem)
{
    return mem ? mem->nvmap.handle : 0;
}

uint64_t horizon_gpu_mem_mapped_va(const horizon_gpu_mem *mem)
{
    return mem ? mem->mapped_va : 0;
}

static horizon_gpu_result mem_range_check(const horizon_gpu_mem *mem,
                                          uint64_t offset, uint64_t size)
{
    if (!mem || size == 0)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);
    if (!horizon_range_fits_u64(offset, size, mem->size))
        return horizon_gpu_err(HORIZON_GPU_ERR_OVERFLOW);
    return horizon_gpu_ok();
}

horizon_gpu_result horizon_gpu_mem_flush(horizon_gpu_mem *mem,
                                         uint64_t offset, uint64_t size)
{
    horizon_gpu_result res = mem_range_check(mem, offset, size);
    if (horizon_gpu_failed(res))
        return res;
    /* CPU -> GPU: clean is sufficient; no need to lose the lines. */
    if (mem->policy == HORIZON_GPU_MEM_CACHED)
        armDCacheClean((uint8_t *)mem->cpu + offset, size);
    return horizon_gpu_ok();
}

horizon_gpu_result horizon_gpu_mem_invalidate(horizon_gpu_mem *mem,
                                              uint64_t offset, uint64_t size)
{
    horizon_gpu_result res = mem_range_check(mem, offset, size);
    if (horizon_gpu_failed(res))
        return res;
    /* GPU -> CPU: libnx exposes clean+invalidate (DC CIVAC — the only
     * data-cache maintenance usable from EL0) as armDCacheFlush; the
     * clean half is harmless when the caller respected the contract of
     * not dirtying GPU-owned lines. */
    if (mem->policy == HORIZON_GPU_MEM_CACHED)
        armDCacheFlush((uint8_t *)mem->cpu + offset, size);
    return horizon_gpu_ok();
}
