/*
 * horizon_gpu — VM implementation over /dev/nvhost-as-gpu.
 *
 * Reservation: NVGPU_AS_IOCTL_ALLOC_SPACE, non-fixed, kernel picks the
 * base (known-risks R8). Mapping: NVGPU_AS_IOCTL_MAP_BUFFER_EX with
 * FixedOffset inside the reservation, page size taken from the
 * reservation (known-risks R9). Unmap checks its Result — the reference
 * ignores it and leaves the stale VA recorded, its most concrete memory
 * bug (reference-analysis § 5).
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>

#include <switch.h>

#include "vm_priv.h"
#include "../device/device_priv.h"
#include "../memory/mem_priv.h"
#include "../memory/align.h"

static bool vm_page_size_valid(const horizon_gpu_device *dev,
                               uint32_t page_size)
{
    return page_size == HORIZON_GPU_SMALL_PAGE_SIZE ||
           page_size == dev->info.big_page_size;
}

horizon_gpu_result horizon_gpu_vm_reserve(horizon_gpu_device *dev,
                                          uint64_t size, uint32_t page_size,
                                          uint64_t align,
                                          horizon_gpu_va_range **out_range)
{
    if (!dev || !out_range || size == 0)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);
    if (!vm_page_size_valid(dev, page_size))
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);

    if (align == 0)
        align = page_size;
    if (!horizon_is_pow2_u64(align) || align < page_size)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);

    uint64_t rounded;
    if (!horizon_align_up_u64(size, page_size, &rounded))
        return horizon_gpu_err(HORIZON_GPU_ERR_OVERFLOW);

    /* ALLOC_SPACE takes a 32-bit page count. */
    uint64_t pages = rounded / page_size;
    if (pages > UINT32_MAX)
        return horizon_gpu_err(HORIZON_GPU_ERR_OVERFLOW);

    horizon_gpu_va_range *range = calloc(1, sizeof(*range));
    if (!range)
        return horizon_gpu_err(HORIZON_GPU_ERR_OUT_OF_MEMORY);

    /* flags = 0: non-fixed, non-sparse; align_or_offset carries the
     * alignment in that case (Linux nvgpu NVGPU_AS_IOCTL_ALLOC_SPACE
     * semantics, matched by the reference's hardware-tested usage). */
    uint64_t base = 0;
    Result rc = nvioctlNvhostAsGpu_AllocSpace(dev->as.fd, (u32)pages,
                                              page_size, 0, align, &base);
    if (R_FAILED(rc)) {
        horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                     "AllocSpace(pages=0x%llx page=0x%x align=0x%llx) "
                     "failed: 0x%08x", (unsigned long long)pages, page_size,
                     (unsigned long long)align, rc);
        free(range);
        return horizon_gpu_err_nv(rc);
    }

    range->dev = dev;
    range->base = base;
    range->size = rounded;
    range->page_size = page_size;
    horizon_va_set_init(&range->live);

    atomic_fetch_add(&dev->live_va_ranges, 1);
    horizon_logf(&dev->log, HORIZON_LOG_DEBUG,
                 "va_range %p: reserved [0x%llx, 0x%llx) page=0x%x",
                 (void *)range, (unsigned long long)base,
                 (unsigned long long)(base + rounded), page_size);

    *out_range = range;
    return horizon_gpu_ok();
}

uint64_t horizon_gpu_va_range_base(const horizon_gpu_va_range *range)
{
    return range ? range->base : 0;
}

uint64_t horizon_gpu_va_range_size(const horizon_gpu_va_range *range)
{
    return range ? range->size : 0;
}

uint32_t horizon_gpu_va_range_page_size(const horizon_gpu_va_range *range)
{
    return range ? range->page_size : 0;
}

horizon_gpu_result horizon_gpu_vm_release(horizon_gpu_va_range *range)
{
    if (!range || !range->dev)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);

    horizon_gpu_device *dev = range->dev;
    uint32_t live = horizon_va_set_count(&range->live);
    if (live != 0) {
        horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                     "va_range %p: release refused, %u live mapping(s)",
                     (void *)range, live);
        return horizon_gpu_err(HORIZON_GPU_ERR_BUSY);
    }

    Result rc = nvioctlNvhostAsGpu_FreeSpace(dev->as.fd, range->base,
                                             (u32)(range->size /
                                                   range->page_size),
                                             range->page_size);
    if (R_FAILED(rc)) {
        horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                     "FreeSpace(base=0x%llx) failed: 0x%08x",
                     (unsigned long long)range->base, rc);
        return horizon_gpu_err_nv(rc);
    }

    horizon_va_set_fini(&range->live);
    atomic_fetch_sub(&dev->live_va_ranges, 1);
    range->dev = NULL; /* double-release fails INVALID_ARG, not UAF */
    free(range);
    return horizon_gpu_ok();
}

horizon_gpu_result horizon_gpu_vm_map(horizon_gpu_va_range *range,
                                      uint64_t offset_in_range,
                                      horizon_gpu_mem *mem,
                                      uint64_t mem_offset, uint64_t size,
                                      uint8_t pte_kind, bool gpu_cacheable,
                                      horizon_gpu_mapping **out_mapping)
{
    if (!range || !range->dev || !mem || !out_mapping || size == 0)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);

    horizon_gpu_device *dev = range->dev;
    uint32_t page = range->page_size;

    if (!horizon_is_aligned_u64(offset_in_range, page) ||
        !horizon_is_aligned_u64(mem_offset, page))
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);

    uint64_t rounded;
    if (!horizon_align_up_u64(size, page, &rounded))
        return horizon_gpu_err(HORIZON_GPU_ERR_OVERFLOW);

    /* offset + range <= mem->size, checked on every map (the reference
     * does not check this at all — memory-model § 4). */
    if (!horizon_range_fits_u64(mem_offset, rounded, mem->size))
        return horizon_gpu_err(HORIZON_GPU_ERR_OVERFLOW);
    /* The map must lie strictly inside the reservation. */
    if (!horizon_range_fits_u64(offset_in_range, rounded, range->size))
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);

    uint64_t va;
    if (!horizon_add_u64(range->base, offset_in_range, &va))
        return horizon_gpu_err(HORIZON_GPU_ERR_OVERFLOW);

    horizon_gpu_mapping *mapping = calloc(1, sizeof(*mapping));
    if (!mapping)
        return horizon_gpu_err(HORIZON_GPU_ERR_OUT_OF_MEMORY);

    /* Claim the interval first so an overlapping request never reaches
     * the kernel. */
    horizon_gpu_status st =
        horizon_va_set_add(&range->live, offset_in_range, rounded);
    if (st != HORIZON_GPU_OK) {
        free(mapping);
        return horizon_gpu_err(st);
    }

    u32 flags = NvMapBufferFlags_FixedOffset;
    if (gpu_cacheable)
        flags |= NvMapBufferFlags_IsCacheable;

    uint64_t offset_out = 0;
    Result rc = nvioctlNvhostAsGpu_MapBufferEx(dev->as.fd, flags, pte_kind,
                                               mem->nvmap.handle, page,
                                               mem_offset, rounded, va,
                                               &offset_out);
    if (R_FAILED(rc)) {
        horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                     "MapBufferEx(va=0x%llx size=0x%llx kind=0x%02x "
                     "page=0x%x) failed: 0x%08x", (unsigned long long)va,
                     (unsigned long long)rounded, pte_kind, page, rc);
        horizon_va_set_remove(&range->live, offset_in_range);
        free(mapping);
        return horizon_gpu_err_nv(rc);
    }
    if (offset_out != va) {
        /* The kernel honoured the map somewhere else than requested —
         * treat as an error and undo rather than record a lie. */
        horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                     "MapBufferEx returned va 0x%llx, requested 0x%llx",
                     (unsigned long long)offset_out, (unsigned long long)va);
        Result urc = nvioctlNvhostAsGpu_UnmapBuffer(dev->as.fd, offset_out);
        if (R_FAILED(urc)) {
            /* The kernel mapping at offset_out is still live and we have
             * no handle to it (we never recorded a mapping object at that
             * VA), so it cannot be retried or targeted by a future unmap.
             * Leaking the VA interval and the mapping struct is the safe
             * failure mode here: keep both un-reusable and keep the leak
             * counted, rather than freeing them and letting the range or
             * the device report zero live objects while the kernel still
             * holds this mapping (never silently leak — memory-model § 7).
             */
            horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                         "cleanup UnmapBuffer(0x%llx) failed: 0x%08x — "
                         "leaking the interval and mapping object, kernel "
                         "mapping cannot be reached again",
                         (unsigned long long)offset_out, urc);
            mapping->range = range;
            mapping->mem = mem;
            mapping->va = offset_out;
            mapping->size = rounded;
            /* mem must also refuse destroy: its backing memory is still
             * mapped at the kernel level even though no mapping handle
             * was returned to reach it. */
            atomic_fetch_add(&mem->live_mappings, 1);
            atomic_fetch_add(&dev->live_mappings, 1);
            return horizon_gpu_err(HORIZON_GPU_ERR_LEAK);
        }
        horizon_va_set_remove(&range->live, offset_in_range);
        free(mapping);
        return horizon_gpu_err(HORIZON_GPU_ERR_NV);
    }

    mapping->range = range;
    mapping->mem = mem;
    mapping->va = va;
    mapping->size = rounded;

    mapping->mem_prev = NULL;
    mapping->mem_next = mem->mapping_list_head;
    if (mem->mapping_list_head)
        mem->mapping_list_head->mem_prev = mapping;
    mem->mapping_list_head = mapping;

    atomic_fetch_add(&mem->live_mappings, 1);
    mem->mapped_va = va;
    atomic_fetch_add(&dev->live_mappings, 1);

    horizon_logf(&dev->log, HORIZON_LOG_DEBUG,
                 "mapping %p: mem %p [0x%llx+0x%llx) -> va 0x%llx kind=0x%02x "
                 "page=0x%x", (void *)mapping, (void *)mem,
                 (unsigned long long)mem_offset, (unsigned long long)rounded,
                 (unsigned long long)va, pte_kind, page);

    *out_mapping = mapping;
    return horizon_gpu_ok();
}

uint64_t horizon_gpu_mapping_va(const horizon_gpu_mapping *mapping)
{
    return mapping ? mapping->va : 0;
}

uint64_t horizon_gpu_mapping_size(const horizon_gpu_mapping *mapping)
{
    return mapping ? mapping->size : 0;
}

horizon_gpu_result horizon_gpu_vm_unmap(horizon_gpu_mapping *mapping)
{
    if (!mapping || !mapping->range)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);

    horizon_gpu_va_range *range = mapping->range;
    horizon_gpu_device *dev = range->dev;

    /* The Result is checked; the reference discards it (drm_shim.c:783). */
    Result rc = nvioctlNvhostAsGpu_UnmapBuffer(dev->as.fd, mapping->va);
    if (R_FAILED(rc)) {
        horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                     "UnmapBuffer(0x%llx) failed: 0x%08x",
                     (unsigned long long)mapping->va, rc);
        return horizon_gpu_err_nv(rc);
    }

    horizon_va_set_remove(&range->live, mapping->va - range->base);

    /* Unlink from mem->mapping_list_head and restore mapped_va to another
     * still-live mapping of the same object, if one exists — the exact
     * invariant whose absence causes the reference's double-unmap bug
     * (memory-model § 2): 0 means no live mapping, never a stale VA, and
     * never a false zero while a sibling mapping is still live. */
    horizon_gpu_mem *mem = mapping->mem;
    if (mapping->mem_prev)
        mapping->mem_prev->mem_next = mapping->mem_next;
    else
        mem->mapping_list_head = mapping->mem_next;
    if (mapping->mem_next)
        mapping->mem_next->mem_prev = mapping->mem_prev;

    atomic_fetch_sub(&mem->live_mappings, 1);
    mem->mapped_va = mem->mapping_list_head ? mem->mapping_list_head->va : 0;

    atomic_fetch_sub(&dev->live_mappings, 1);
    mapping->range = NULL; /* double-unmap fails INVALID_ARG, not UAF */
    free(mapping);
    return horizon_gpu_ok();
}
