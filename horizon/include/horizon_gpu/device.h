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
 * creation; nothing is hardcoded. */
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
    /* Big-page size this device's address space was actually created
     * with: big_page_size unless horizon_gpu_device_create_info::
     * as_big_page_size overrode it to a different member of
     * available_big_page_sizes. horizon_gpu_vm_reserve validates its
     * page_size argument against this field, not against big_page_size,
     * so the queried hardware default above stays discoverable even when
     * a non-default address-space granularity is in effect. */
    uint32_t as_big_page_size;
    uint32_t gpu_va_bit_count;
    /* Engine class numbers, queried rather than assumed (used for the
     * in-stream SET_OBJECT binds). */
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

/* Zcull geometry, as the GPU reports it
 * (NVGPU_GPU_IOCTL_ZCULL_GET_INFO). Field names follow the ioctl's, and
 * consumers must map them by name: the same ten numbers appear in a
 * different order in nouveau's drm_nouveau_get_zcull_info, so copying
 * the struct wholesale would be quietly wrong. */
typedef struct horizon_gpu_zcull_info {
    uint32_t width_align_pixels;
    uint32_t height_align_pixels;
    uint32_t pixel_squares_by_aliquots;
    uint32_t aliquot_total;
    uint32_t region_byte_multiplier;
    uint32_t region_header_size;
    uint32_t subregion_header_size;
    uint32_t subregion_width_align_pixels;
    uint32_t subregion_height_align_pixels;
    uint32_t subregion_count;
    /* Context buffer size, from nvGpuGetZcullCtxSize; not part of the
     * geometry ioctl but wanted by the same callers. */
    uint64_t ctx_size;
} horizon_gpu_zcull_info;

/* Live-object counters. All must be zero for
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
    /* Debug-synchronous diagnostic mode. Also enabled by the
     * HORIZON_GPU_SYNC=1 environment variable. */
    bool debug_synchronous;
    /* Let a channel come up when the initial syncpoint read fails, with an
     * untrusted shadow baseline. Also enabled by
     * HORIZON_GPU_UNTRUSTED_SYNCPT_BASELINE=1.
     *
     * This exists for environments that do not implement
     * NVHOST_IOCTL_CTRL_SYNCPT_READ at all, so the code above a channel can
     * still be exercised there. It does not make such a channel usable: a
     * fence taken from it is arithmetic on a baseline nobody measured, and
     * anything that waits on one is reporting a guess. Never enable it to
     * obtain a result that will be reported as hardware behaviour. */
    bool allow_untrusted_syncpt_baseline;
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

/* True once at least one channel of this device came up with an untrusted
 * syncpoint baseline. Sticky: it stays true after that channel is
 * destroyed, because results already derived from it do not become
 * trustworthy again. Always false unless the opt-in above was
 * requested. */
bool horizon_gpu_device_untrusted_syncpt_seen(const horizon_gpu_device *dev);

/* The GPU's own timestamp counter, in nanoseconds
 * (NVGPU_GPU_IOCTL_GET_GPU_TIME). This is the GPU's clock, not the CPU's
 * — the point of asking for it is to correlate the two, which is what
 * VK_EXT_calibrated_timestamps exists to do. */
horizon_gpu_result horizon_gpu_device_get_timestamp(horizon_gpu_device *dev,
                                                    uint64_t *out_ts);

/* Zcull geometry. A failure here is not fatal to anything: the query is
 * advisory and nouveau lets it fail too, so callers are expected to
 * carry on without it rather than refuse to start. */
horizon_gpu_result
horizon_gpu_device_get_zcull_info(horizon_gpu_device *dev,
                                  horizon_gpu_zcull_info *out_info);

/* Fails with HORIZON_GPU_ERR_LEAK — after logging every non-zero counter —
 * if any child object is still alive; nothing is torn down in that case.
 * On success the nv services are released in reverse bring-up order and
 * `dev` is freed. */
horizon_gpu_result horizon_gpu_device_destroy(horizon_gpu_device *dev);

#ifdef __cplusplus
}
#endif

#endif /* HORIZON_GPU_DEVICE_H */
