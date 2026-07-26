/*
 * horizon_gpu — syncpoint read and device-level fence wait/poll.
 *
 * The nanosecond -> microsecond conversion happens exactly once, in
 * horizon_timeout_ns_to_us_clamped (the reference turned an intended 2 s
 * wait into 2000 s by mixing the units — drm_shim.c:980-982).
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <switch.h>

#include "horizon_gpu/sync.h"
#include "../device/device_priv.h"
#include "syncpt_math.h"

/* Bounded kernel wait per loop iteration; keeps even "no deadline" waits
 * re-checkable (docs/synchronization.md § 6). */
#define SYNC_WAIT_CHUNK_US INT32_C(100000)

horizon_gpu_result horizon_gpu_syncpt_read(horizon_gpu_device *dev,
                                           uint32_t syncpt_id,
                                           uint32_t *out_value)
{
    if (!dev || !out_value)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);

    /* /dev/nvhost-ctrl, open since device bring-up step 2 (nvFenceInit). */
    Result rc = nvioctlNvhostCtrl_SyncptRead(nvFenceGetFd(), syncpt_id,
                                             out_value);
    if (R_FAILED(rc))
        return horizon_gpu_err_nv(rc);
    return horizon_gpu_ok();
}

horizon_gpu_result horizon_gpu_fence_poll(horizon_gpu_device *dev,
                                          horizon_gpu_fence fence,
                                          bool *out_signalled)
{
    if (!dev || !out_signalled)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);

    uint32_t value;
    horizon_gpu_result res = horizon_gpu_syncpt_read(dev, fence.syncpt_id,
                                                     &value);
    if (horizon_gpu_failed(res))
        return res;
    *out_signalled = horizon_gpu_syncpt_reached(value, fence.threshold);
    return horizon_gpu_ok();
}

horizon_gpu_result horizon_gpu_fence_wait(horizon_gpu_device *dev,
                                          horizon_gpu_fence fence,
                                          uint64_t timeout_ns)
{
    if (!dev)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);

    uint64_t start = armGetSystemTick();
    NvFence nvf = { .id = fence.syncpt_id, .value = fence.threshold };

    for (;;) {
        uint32_t value;
        horizon_gpu_result res = horizon_gpu_syncpt_read(dev,
                                                         fence.syncpt_id,
                                                         &value);
        if (horizon_gpu_failed(res))
            return res;
        if (horizon_gpu_syncpt_reached(value, fence.threshold))
            return horizon_gpu_ok();

        uint64_t elapsed_ns = armTicksToNs(armGetSystemTick() - start);
        if (timeout_ns != HORIZON_GPU_NO_TIMEOUT && elapsed_ns >= timeout_ns)
            return horizon_gpu_err(HORIZON_GPU_ERR_TIMEOUT);

        int32_t chunk_us = SYNC_WAIT_CHUNK_US;
        if (timeout_ns != HORIZON_GPU_NO_TIMEOUT) {
            int32_t rem_us =
                horizon_timeout_ns_to_us_clamped(timeout_ns - elapsed_ns);
            if (rem_us < chunk_us)
                chunk_us = rem_us;
        }
        /* Chunk expiry is the loop's pulse, not an error; genuine
         * failures surface through SyncptRead above. */
        (void)nvFenceWait(&nvf, chunk_us);
    }
}
