/*
 * horizon_gpu — device: `nv` service bring-up, GM20B query, teardown.
 *
 * The device owns the nv session, the GPU address space and the leak
 * accounting (memory-model § 8). Every other object is created from a
 * device pointer passed explicitly — there is no ambient device state.
 *
 * This header is libnx-free on purpose: consumers above this layer
 * (nvkmd_horizon) must never see libnx types (architecture.md § 3).
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef HORIZON_GPU_DEVICE_H
#define HORIZON_GPU_DEVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "result.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct horizon_gpu_device horizon_gpu_device;

/* One GPU VA region as reported by the address space (small-page and
 * big-page halves; memory-model § 3.1). */
typedef struct horizon_gpu_va_region_info {
    uint64_t base;      /* first GPU VA of the region                  */
    uint64_t pages;     /* region length in pages                      */
    uint32_t page_size; /* bytes per page in this region               */
} horizon_gpu_va_region_info;

/* GM20B facts. Everything here is *queried* from the nv services at device
 * creation; nothing is hardcoded (CLAUDE.md; docs/memory-model.md § 3.1). */
typedef struct horizon_gpu_device_info {
    /* From NVGPU_GPU_IOCTL_GET_CHARACTERISTICS (libnx
     * nvGpuGetCharacteristics; field meanings per Linux nvgpu uapi). */
    uint32_t arch;                   /* 0x120 = NVGPU_GPU_ARCH_GM200 family */
    uint32_t impl;                   /* 0xB   = NVGPU_GPU_IMPL_GM20B        */
    uint32_t rev;
    uint32_t num_gpc;
    uint32_t num_tpc_per_gpc;
    uint64_t l2_cache_size;
    uint32_t big_page_size;          /* bytes; required, never defaulted    */
    uint32_t compression_page_size;
    uint32_t available_big_page_sizes; /* bitmask of supported big-page sizes */
    uint32_t gpu_va_bit_count;
    /* Engine class numbers, queried rather than assumed (used for the
     * in-stream SET_OBJECT binds, docs/known-risks.md R7). */
    uint32_t twod_class;
    uint32_t threed_class;
    uint32_t compute_class;
    uint32_t gpfifo_class;
    uint32_t inline_to_memory_class;
    uint32_t dma_copy_class;
    /* NVGPU_GPU_FLAGS_HAS_SYNCPOINTS (bit 0 of characteristics.flags,
     * Linux nvgpu uapi <uapi/linux/nvgpu.h>). */
    bool has_syncpoints;
    char chipname[9];                /* e.g. "gm20b", NUL-terminated        */

    /* From NVGPU_AS_IOCTL_GET_VA_REGIONS (libnx GetVARegions):
     * [0] = small-page region, [1] = big-page region. */
    horizon_gpu_va_region_info va_regions[2];
} horizon_gpu_device_info;

/* Live-object counters (memory-model § 8). All must be zero for
 * horizon_gpu_device_destroy to succeed. */
typedef struct horizon_gpu_device_counters {
    uint32_t live_mem;
    uint32_t live_va_ranges;
    uint32_t live_mappings;
    uint32_t live_channels;
} horizon_gpu_device_counters;

typedef struct horizon_gpu_device_create_info {
    /* 0 = use the queried characteristics.big_page_size for the address
     * space; a non-zero value must be one of available_big_page_sizes. */
    uint32_t as_big_page_size;
    /* Debug-synchronous diagnostic mode (docs/synchronization.md § 8).
     * Also enabled by the HORIZON_GPU_SYNC=1 environment variable. */
    bool debug_synchronous;
} horizon_gpu_device_create_info;

/* Brings up the nv services in order (nvInitialize, fence, map, gpu,
 * address space), queries the GM20B characteristics — a query failure is a
 * creation failure, never a fallback — and returns a device owned by the
 * caller. `create_info` may be NULL for defaults.
 * On failure everything already initialised is torn down in reverse order
 * and *out_dev is left untouched. */
horizon_gpu_result
horizon_gpu_device_create(const horizon_gpu_device_create_info *create_info,
                          horizon_gpu_device **out_dev);

horizon_gpu_result
horizon_gpu_device_get_info(const horizon_gpu_device *dev,
                            horizon_gpu_device_info *out_info);

horizon_gpu_result
horizon_gpu_device_get_counters(const horizon_gpu_device *dev,
                                horizon_gpu_device_counters *out_counters);

/* Fails with HORIZON_GPU_ERR_LEAK — after logging every non-zero counter —
 * if any child object is still alive; nothing is torn down in that case.
 * On success the nv services are released in reverse bring-up order and
 * `dev` is freed. */
horizon_gpu_result horizon_gpu_device_destroy(horizon_gpu_device *dev);

#ifdef __cplusplus
}
#endif

#endif /* HORIZON_GPU_DEVICE_H */
