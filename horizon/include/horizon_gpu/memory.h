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
 * Homebrew heap memory on Horizon is always CPU-cached, so CACHED is the
 * only honest policy this phase offers; uncached backings are a pending
 * decision (D5) blocked on the R6 measurements. */
typedef enum horizon_gpu_cache_policy {
    HORIZON_GPU_MEM_CACHED = 1,
} horizon_gpu_cache_policy;

/* Allocates zero-initialised, page-aligned storage of `size` bytes rounded
 * up to `align` (power of two, >= HORIZON_GPU_SMALL_PAGE_SIZE; 0 means
 * page alignment) and registers it with nvmap. Every rounding is
 * overflow-checked before use. The caller owns the object. */
horizon_gpu_result horizon_gpu_mem_create(horizon_gpu_device *dev,
                                          uint64_t size, uint64_t align,
                                          horizon_gpu_cache_policy policy,
                                          horizon_gpu_mem **out_mem);

/* Fails with HORIZON_GPU_ERR_BUSY while GPU mappings of this object are
 * alive. Closing invalidates the CPU pointer (memory-model § 7). */
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

#ifdef __cplusplus
}
#endif

#endif /* HORIZON_GPU_MEMORY_H */
