/*
 * horizon_gpu — device implementation: ordered `nv` bring-up, GM20B query,
 * reverse-order teardown with leak accounting.
 *
 * Bring-up order (verified against the reference port's hardware-tested
 * sequence, re-expressed on the public libnx API):
 *
 *   nvInitialize -> nvFenceInit -> nvMapInit -> nvGpuInit
 *     -> nvGpuGetCharacteristics (REQUIRED; failure is an error, never a
 *        fallback to a hardcoded big-page size)
 *     -> nvAddressSpaceCreate(big_page_size)
 *     -> GetVARegions
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>

#include "device_priv.h"
#include "../memory/align.h"
#include "horizon_gpu/cmds.h"

/* result.h mirrors libnx Result as uint32_t; hold that to be true. */
_Static_assert(sizeof(horizon_gpu_nv_result) == sizeof(Result),
               "horizon_gpu_nv_result must mirror libnx Result");

/* NVGPU_GPU_FLAGS_HAS_SYNCPOINTS, bit 0 of gpu characteristics `flags`
 * (Linux nvgpu <uapi/linux/nvgpu.h>). */
#define HORIZON_NVGPU_FLAGS_HAS_SYNCPOINTS (1ull << 0)

static void device_fill_info(horizon_gpu_device *dev,
                             const nvioctl_gpu_characteristics *ch)
{
    horizon_gpu_device_info *info = &dev->info;

    info->arch = ch->arch;
    info->impl = ch->impl;
    info->rev = ch->rev;
    info->num_gpc = ch->num_gpc;
    info->num_tpc_per_gpc = ch->num_tpc_per_gpc;
    info->l2_cache_size = ch->L2_cache_size;
    info->big_page_size = ch->big_page_size;
    info->compression_page_size = ch->compression_page_size;
    info->available_big_page_sizes = ch->available_big_page_sizes;
    info->gpu_va_bit_count = ch->gpu_va_bit_count;
    info->twod_class = ch->twod_class;
    info->threed_class = ch->threed_class;
    info->compute_class = ch->compute_class;
    info->gpfifo_class = ch->gpfifo_class;
    info->inline_to_memory_class = ch->inline_to_memory_class;
    info->dma_copy_class = ch->dma_copy_class;
    info->has_syncpoints = (ch->flags & HORIZON_NVGPU_FLAGS_HAS_SYNCPOINTS) != 0;

    /* chipname is a little-endian byte string packed in a u64 ("gm20b"). */
    memcpy(info->chipname, &ch->chipname, 8);
    info->chipname[8] = '\0';
}

horizon_gpu_result
horizon_gpu_device_create(const horizon_gpu_device_create_info *create_info,
                          horizon_gpu_device **out_dev)
{
    if (!out_dev)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);

    horizon_gpu_device_create_info defaults = {0};
    if (!create_info)
        create_info = &defaults;

    horizon_gpu_device *dev = calloc(1, sizeof(*dev));
    if (!dev)
        return horizon_gpu_err(HORIZON_GPU_ERR_OUT_OF_MEMORY);

    horizon_log_init_from_env(&dev->log, NULL);

    const char *sync_env = getenv("HORIZON_GPU_SYNC");
    dev->debug_synchronous = create_info->debug_synchronous ||
                             (sync_env && sync_env[0] == '1');

    /* Read once, at device creation, so a submit path never calls
     * getenv: it walks environ, and the submit path is the hot one. */
    const char *barrier_env = getenv("HORIZON_GPU_FULL_BARRIER_WAITS");
    dev->full_barrier_waits = barrier_env && barrier_env[0] == '1';
    const char *untrusted_env = getenv("HORIZON_GPU_UNTRUSTED_SYNCPT_BASELINE");
    dev->allow_untrusted_syncpt_baseline =
        create_info->allow_untrusted_syncpt_baseline ||
        (untrusted_env && untrusted_env[0] == '1');
    if (dev->allow_untrusted_syncpt_baseline) {
        /* Announced at ERROR level on purpose: this is the one setting that
         * makes a fence a guess, so it must be impossible to read a log from
         * such a run and mistake it for a measurement. */
        horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                     "untrusted syncpoint baselines are ALLOWED on this "
                     "device: fences from a degraded channel are not "
                     "measurements (synchronization.md § 9)");
    }

    horizon_gpu_result res;
    Result rc;

    /* Bring-up step 1/5: nvdrv session. */
    rc = nvInitialize();
    if (R_FAILED(rc)) {
        horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                     "nvInitialize failed: 0x%08x", rc);
        res = horizon_gpu_err_nv(rc);
        goto fail_free;
    }

    /* Step 2/5: /dev/nvhost-ctrl — must precede any fence use
     * (reference-analysis.md § 4, ordering constraint 1). */
    rc = nvFenceInit();
    if (R_FAILED(rc)) {
        horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                     "nvFenceInit failed: 0x%08x", rc);
        res = horizon_gpu_err_nv(rc);
        goto fail_nv;
    }

    /* Step 3/5: /dev/nvmap. */
    rc = nvMapInit();
    if (R_FAILED(rc)) {
        horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                     "nvMapInit failed: 0x%08x", rc);
        res = horizon_gpu_err_nv(rc);
        goto fail_fence;
    }

    /* Step 4/5: /dev/nvhost-gpu + /dev/nvhost-ctrl-gpu. */
    rc = nvGpuInit();
    if (R_FAILED(rc)) {
        horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                     "nvGpuInit failed: 0x%08x", rc);
        res = horizon_gpu_err_nv(rc);
        goto fail_map;
    }

    /* GM20B characteristics are REQUIRED. The reference falls back to a
     * hardcoded 0x10000 big-page size when this returns NULL
     * (drm_shim.c:170-171) and its own notes contradict that value; we
     * fail device creation instead (memory-model § 3.1). */
    const nvioctl_gpu_characteristics *ch = nvGpuGetCharacteristics();
    if (!ch || ch->big_page_size == 0 ||
        !horizon_is_pow2_u64(ch->big_page_size)) {
        horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                     "nvGpuGetCharacteristics unavailable or implausible "
                     "(big_page_size=0x%x)", ch ? ch->big_page_size : 0);
        res = horizon_gpu_err(HORIZON_GPU_ERR_NV);
        goto fail_gpu;
    }
    device_fill_info(dev, ch);

    /* The command builder encodes a GPU address in SEMAPHOREA/B, which
     * together hold exactly HORIZON_CMDS_GPU_VA_BITS bits, and it rejects
     * anything wider rather than truncating it. That builder is libnx-free
     * and cannot query the width, so the two have to agree — and until now
     * nothing checked that they did. An address space wider than the
     * builder can encode would submit truncated addresses; a narrower one
     * would let the builder accept addresses the hardware cannot reach.
     * Either way the failure belongs here, at init, not at submit time. */
    if (ch->gpu_va_bit_count != HORIZON_CMDS_GPU_VA_BITS) {
        horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                     "gpu_va_bit_count is %u; the command builder encodes "
                     "%u", ch->gpu_va_bit_count,
                     (unsigned)HORIZON_CMDS_GPU_VA_BITS);
        res = horizon_gpu_err(HORIZON_GPU_ERR_NV);
        goto fail_gpu;
    }

    uint32_t as_big_page = create_info->as_big_page_size;
    if (as_big_page == 0) {
        as_big_page = ch->big_page_size;
    } else if (!horizon_is_pow2_u64(as_big_page) ||
               (ch->available_big_page_sizes & as_big_page) == 0) {
        /* available_big_page_sizes is a bitmask of the supported sizes
         * OR-ed together (e.g. 0x30000 = 0x10000 | 0x20000). */
        horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                     "requested big page size 0x%x not in available mask 0x%x",
                     as_big_page, ch->available_big_page_sizes);
        res = horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);
        goto fail_gpu;
    }
    /* device_fill_info() recorded the characteristics' queried default in
     * big_page_size; that stays untouched (the public contract is "never
     * defaulted"). The size actually selected for this address space —
     * which may differ — is recorded separately so vm_page_size_valid()
     * validates reservations against the size in effect. */
    dev->info.as_big_page_size = as_big_page;

    /* Step 5/5: /dev/nvhost-as-gpu with the *queried* big-page size. */
    rc = nvAddressSpaceCreate(&dev->as, as_big_page);
    if (R_FAILED(rc)) {
        horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                     "nvAddressSpaceCreate(0x%x) failed: 0x%08x",
                     as_big_page, rc);
        res = horizon_gpu_err_nv(rc);
        goto fail_gpu;
    }

    /* VA regions: [0] small-page half, [1] big-page half. */
    nvioctl_va_region regions[2];
    memset(regions, 0, sizeof(regions));
    rc = nvioctlNvhostAsGpu_GetVARegions(dev->as.fd, regions);
    if (R_FAILED(rc)) {
        horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                     "GetVARegions failed: 0x%08x", rc);
        res = horizon_gpu_err_nv(rc);
        goto fail_as;
    }
    for (size_t i = 0; i < 2; i++) {
        dev->info.va_regions[i] = (horizon_gpu_va_region_info){
            .base = regions[i].offset,
            .pages = regions[i].pages,
            .page_size = regions[i].page_size,
        };
    }

    horizon_logf(&dev->log, HORIZON_LOG_INFO,
                 "device up: %s arch=0x%x impl=0x%x rev=0x%x gpc=%u tpc/gpc=%u "
                 "big_page=0x%x as_big_page=0x%x va_bits=%u%s",
                 dev->info.chipname, dev->info.arch, dev->info.impl,
                 dev->info.rev, dev->info.num_gpc, dev->info.num_tpc_per_gpc,
                 dev->info.big_page_size, dev->info.as_big_page_size,
                 dev->info.gpu_va_bit_count,
                 dev->debug_synchronous ? " [debug-synchronous]" : "");

    *out_dev = dev;
    return horizon_gpu_ok();

fail_as:
    nvAddressSpaceClose(&dev->as);
fail_gpu:
    nvGpuExit();
fail_map:
    nvMapExit();
fail_fence:
    nvFenceExit();
fail_nv:
    nvExit();
fail_free:
    free(dev);
    return res;
}

horizon_gpu_result
horizon_gpu_device_get_info(const horizon_gpu_device *dev,
                            horizon_gpu_device_info *out_info)
{
    if (!dev || !out_info)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);
    *out_info = dev->info;
    return horizon_gpu_ok();
}

horizon_gpu_result
horizon_gpu_device_get_counters(const horizon_gpu_device *dev,
                                horizon_gpu_device_counters *out_counters)
{
    if (!dev || !out_counters)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);
    out_counters->live_mem = atomic_load(&dev->live_mem);
    out_counters->live_va_ranges = atomic_load(&dev->live_va_ranges);
    out_counters->live_mappings = atomic_load(&dev->live_mappings);
    out_counters->live_channels = atomic_load(&dev->live_channels);
    return horizon_gpu_ok();
}

bool horizon_gpu_device_untrusted_syncpt_seen(const horizon_gpu_device *dev)
{
    return dev ? atomic_load(&dev->untrusted_syncpt_seen) : false;
}

horizon_gpu_result horizon_gpu_device_destroy(horizon_gpu_device *dev)
{
    if (!dev)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);

    horizon_gpu_device_counters c;
    (void)horizon_gpu_device_get_counters(dev, &c);
    if (c.live_mem || c.live_va_ranges || c.live_mappings ||
        c.live_channels) {
        /* Never silently leak, never silently succeed (memory-model § 7).
         * Nothing is torn down; the caller must destroy the children. */
        horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                     "device destroy refused: live mem=%u va_ranges=%u "
                     "mappings=%u channels=%u",
                     c.live_mem, c.live_va_ranges, c.live_mappings,
                     c.live_channels);
        return horizon_gpu_err(HORIZON_GPU_ERR_LEAK);
    }

    /* Reverse of bring-up. */
    nvAddressSpaceClose(&dev->as);
    nvGpuExit();
    nvMapExit();
    nvFenceExit();
    nvExit();
    free(dev);
    return horizon_gpu_ok();
}

horizon_gpu_result horizon_gpu_device_get_timestamp(horizon_gpu_device *dev,
                                                    uint64_t *out_ts)
{
    if (!dev || !out_ts)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);

    /* /dev/nvhost-ctrl-gpu, open since bring-up step 4 (nvGpuInit).
     * libnx wraps NVGPU_GPU_IOCTL_GET_GPU_TIME as nvGpuGetTimestamp. */
    Result rc = nvGpuGetTimestamp(out_ts);
    if (R_FAILED(rc)) {
        horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                     "nvGpuGetTimestamp failed: 0x%08x", rc);
        return horizon_gpu_err_nv(rc);
    }
    return horizon_gpu_ok();
}

horizon_gpu_result
horizon_gpu_device_get_zcull_info(horizon_gpu_device *dev,
                                  horizon_gpu_zcull_info *out_info)
{
    if (!dev || !out_info)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);

    /* Queried once by nvGpuInit and cached; no ioctl from here. A NULL
     * means the query failed at bring-up, which is not fatal — nouveau
     * lets this one fail too (nouveau_device.c:484) and NVK carries on
     * with has_zcull_info false. */
    const nvioctl_zcull_info *zi = nvGpuGetZcullInfo();
    if (!zi) {
        horizon_logf(&dev->log, HORIZON_LOG_WARN,
                     "Zcull info unavailable (advisory query)");
        return horizon_gpu_err(HORIZON_GPU_ERR_UNSUPPORTED);
    }

    /* Field by field, never a struct copy: nouveau's
     * drm_nouveau_get_zcull_info carries the same ten numbers in a
     * different order — subregion_count is last here and third from last
     * there — so a memcpy would put a count where an alignment goes. */
    out_info->width_align_pixels = zi->width_align_pixels;
    out_info->height_align_pixels = zi->height_align_pixels;
    out_info->pixel_squares_by_aliquots = zi->pixel_squares_by_aliquots;
    out_info->aliquot_total = zi->aliquot_total;
    out_info->region_byte_multiplier = zi->region_byte_multiplier;
    out_info->region_header_size = zi->region_header_size;
    out_info->subregion_header_size = zi->subregion_header_size;
    out_info->subregion_width_align_pixels = zi->subregion_width_align_pixels;
    out_info->subregion_height_align_pixels = zi->subregion_height_align_pixels;
    out_info->subregion_count = zi->subregion_count;
    out_info->ctx_size = nvGpuGetZcullCtxSize();

    return horizon_gpu_ok();
}
