/*
 * horizon_gpu — GPU virtual address management: reservations and mappings.
 *
 * Tegra's nvgpu only accepts a FIXED map inside a previously reserved
 * non-fixed range (known-risks R8), so reservations are first-class
 * objects here: horizon_gpu_vm_reserve returns the base the kernel chose,
 * and horizon_gpu_vm_map performs FIXED maps strictly inside a reservation
 * the caller owns (memory-model § 3). The map's page size derives from the
 * containing reservation, never from a constant (known-risks R9).
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef HORIZON_GPU_VM_H
#define HORIZON_GPU_VM_H

#include <stdbool.h>
#include <stdint.h>

#include "device.h"
#include "memory.h"
#include "result.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct horizon_gpu_va_range horizon_gpu_va_range;
typedef struct horizon_gpu_mapping horizon_gpu_mapping;

/* PTE kinds this phase exercises. Values are the NVIDIA page-table kinds
 * as named by libnx's NvKind enum (switch/nvidia/types.h, matching
 * envytools' kind lists for Maxwell). The full range of NvKind values is
 * accepted and passed through opaquely — NIL owns tiling decisions
 * (memory-model § 6). */
#define HORIZON_GPU_PTE_KIND_PITCH          UINT8_C(0x00) /* NvKind_Pitch */
#define HORIZON_GPU_PTE_KIND_GENERIC_16BX2  UINT8_C(0xfe) /* NvKind_Generic_16BX2 */

/* Reserves a non-fixed VA range of `size` bytes (rounded up to
 * `page_size`) with the given alignment (0 = page_size; power of two).
 * `page_size` selects the address-space half: HORIZON_GPU_SMALL_PAGE_SIZE
 * or the device's queried big_page_size. The kernel chooses the base.
 * The caller owns the reservation. */
horizon_gpu_result horizon_gpu_vm_reserve(horizon_gpu_device *dev,
                                          uint64_t size, uint32_t page_size,
                                          uint64_t align,
                                          horizon_gpu_va_range **out_range);

/* Like horizon_gpu_vm_reserve, but asks the kernel for a SPARSE
 * reservation: the whole interval gets page-table entries that resolve
 * to nothing, rather than being left unmapped, so an access to a part of
 * it that no memory object is bound to is defined instead of being a
 * fault.
 *
 * That is the primitive Vulkan sparse residency is built on, and both
 * halves of it have been measured: an unbound page swallows a write,
 * and unbinding a bound block puts the sparse state back.
 * gpu_fault/sparse asks it of this layer, vk_core/sparse_binding asks
 * it through vkQueueBindSparse, and docs/MEASURED-ON-HARDWARE.md
 * carries both.
 *
 * SO THIS IS REACHABLE AND PROVEN. mesa-patches/0055 calls it for
 * NVKMD_VA_SPARSE, 0056 sets nvkmd_info::has_sparse, and sparseBinding
 * and sparseResidencyBuffer are advertised. This paragraph said the
 * opposite — "nothing in nvkmd_horizon calls it, has_sparse is still
 * false, and no Vulkan sparse feature is advertised" — until
 * 2026-09-09, having been wrong since those two patches landed, and
 * it named the case t_sparse, which the suite refactor renamed.
 *
 * Sparse exists only in big pages: a reservation with
 * NvAllocSpaceFlags_Sparse and a 0x1000 page size is refused, which is
 * why 0055 forces the big-page size before it calls this. */
horizon_gpu_result
horizon_gpu_vm_reserve_sparse(horizon_gpu_device *dev, uint64_t size,
                              uint32_t page_size, uint64_t align,
                              horizon_gpu_va_range **out_range);

/* Reserves exactly [base, base+size) — the caller chooses the address,
 * the kernel does not. `base` must be page_size-aligned; `size` is
 * rounded up to page_size. Fails if the range is taken.
 *
 * This is NvAllocSpaceFlags_FixedOffset. It exists for the two things a
 * kernel-chosen address cannot express: reserving an aperture the
 * hardware fixes for us (the shader local/shared memory windows are at
 * constant addresses and must not be handed to anything else), and
 * replaying a capture at the address it was captured from.
 *
 * A request that comes back at a different address is reported as a
 * failure rather than accepted, because a fixed request is exactly the
 * one that cannot absorb a different answer. */
horizon_gpu_result horizon_gpu_vm_reserve_fixed(horizon_gpu_device *dev,
                                                uint64_t base, uint64_t size,
                                                uint32_t page_size,
                                                horizon_gpu_va_range **out_range);

uint64_t horizon_gpu_va_range_base(const horizon_gpu_va_range *range);
uint64_t horizon_gpu_va_range_size(const horizon_gpu_va_range *range);
uint32_t horizon_gpu_va_range_page_size(const horizon_gpu_va_range *range);

/* Fails with HORIZON_GPU_ERR_BUSY while mappings inside it are alive.
 * Note that Phase 1 offers no deferred recycling: the caller must also
 * ensure every submit that referenced the range has retired before
 * releasing it. */
horizon_gpu_result horizon_gpu_vm_release(horizon_gpu_va_range *range);

/* FIXED-maps mem[mem_offset, mem_offset+size) at
 * range base + offset_in_range, with the given PTE kind, using the
 * reservation's page size. Both offsets must be aligned to that page
 * size; `size` is rounded up to it (overflow-checked, and the rounded
 * range must still fit the memory object). `gpu_cacheable` selects GPU L2
 * cacheability of the mapping (NVGPU_AS_MAP_BUFFER_FLAGS_CACHEABLE).
 * Overlapping a live mapping is reported as HORIZON_GPU_ERR_BUSY, never
 * attempted. */
horizon_gpu_result horizon_gpu_vm_map(horizon_gpu_va_range *range,
                                      uint64_t offset_in_range,
                                      horizon_gpu_mem *mem,
                                      uint64_t mem_offset, uint64_t size,
                                      uint8_t pte_kind, bool gpu_cacheable,
                                      horizon_gpu_mapping **out_mapping);

uint64_t horizon_gpu_mapping_va(const horizon_gpu_mapping *mapping);
uint64_t horizon_gpu_mapping_size(const horizon_gpu_mapping *mapping);

/* Unmaps and destroys the mapping, clearing the memory object's recorded
 * VA — the invariant the reference violates (memory-model § 2). The
 * caller must ensure submits referencing the VA have retired. */
horizon_gpu_result horizon_gpu_vm_unmap(horizon_gpu_mapping *mapping);

#ifdef __cplusplus
}
#endif

#endif /* HORIZON_GPU_VM_H */
