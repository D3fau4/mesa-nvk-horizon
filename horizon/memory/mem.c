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
    if (policy != HORIZON_GPU_MEM_CACHED)
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

    /* The NvMap kind is NvKind_Pitch here; block-linear layouts are a
     * property of each GPU *mapping* (its PTE kind), never of the memory
     * object (memory-model § 1 #9). is_cpu_cacheable=true tells nvmap the
     * truth about heap memory, unlike the reference's cacheable=false at
     * every call site (drm_shim.c:460). */
    Result rc = nvMapCreate(&mem->nvmap, mem->cpu, (u32)rounded, (u32)align,
                            NvKind_Pitch, true);
    if (R_FAILED(rc)) {
        horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                     "nvMapCreate(size=0x%llx align=0x%llx) failed: 0x%08x",
                     (unsigned long long)rounded, (unsigned long long)align,
                     rc);
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
    if (!mem || !mem->cpu)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);

    uint32_t live = atomic_load(&mem->live_mappings);
    if (live != 0) {
        horizon_logf(&mem->dev->log, HORIZON_LOG_ERROR,
                     "mem %p: destroy refused, %u live mapping(s) at "
                     "va=0x%llx", (void *)mem, live,
                     (unsigned long long)mem->mapped_va);
        return horizon_gpu_err(HORIZON_GPU_ERR_BUSY);
    }

    nvMapClose(&mem->nvmap);
    free(mem->cpu);
    mem->cpu = NULL; /* make a double-destroy fail INVALID_ARG, not UAF */
    atomic_fetch_sub(&mem->dev->live_mem, 1);
    free(mem);
    return horizon_gpu_ok();
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
