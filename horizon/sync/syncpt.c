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

/* "Has (id, threshold) been reached?", asked of the kernel directly.
 *
 * This is a *different question* from reading the counter, not a weaker
 * one: NVHOST_IOCTL_CTRL_SYNCPT_WAIT with a zero timeout answers exactly
 * what a fence needs, and needs no counter value and no shadow. It
 * exists here because a platform can implement one and not the other —
 * the emulator measured on 2026-07-28 answers SyncptRead with
 * NotImplemented while games on it wait on fences constantly, which is
 * the same ioctl underneath nvFenceWait.
 *
 * Used only when the read has already failed. Where the read works it
 * stays the primary path: it is one ioctl for any number of fences on
 * the same syncpoint, and it feeds the 64-bit shadow, which this cannot.
 */
static horizon_gpu_result sync_fence_reached_via_wait(horizon_gpu_fence fence,
                                                      int32_t timeout_us,
                                                      bool *out_reached)
{
    NvFence nvf = { .id = fence.syncpt_id, .value = fence.threshold };
    Result rc = nvFenceWait(&nvf, timeout_us);
    if (R_SUCCEEDED(rc)) {
        *out_reached = true;
        return horizon_gpu_ok();
    }
    if (rc == KERNELRESULT(TimedOut)) {
        *out_reached = false;
        return horizon_gpu_ok();
    }
    return horizon_gpu_err_nv(rc);
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
    if (horizon_gpu_failed(res)) {
        if (!dev->allow_untrusted_syncpt_baseline)
            return res;
        return sync_fence_reached_via_wait(fence, 0, out_signalled);
    }
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
        if (horizon_gpu_failed(res)) {
            if (!dev->allow_untrusted_syncpt_baseline)
                return res;
            /* No counter on this platform. Ask the kernel the fence's
             * own question instead, for what is left of the deadline —
             * nvFenceWait blocks until reached or the timeout, so this
             * replaces the loop rather than joining it. */
            int32_t wait_us = SYNC_WAIT_CHUNK_US;
            if (timeout_ns != HORIZON_GPU_NO_TIMEOUT) {
                uint64_t used = armTicksToNs(armGetSystemTick() - start);
                if (used >= timeout_ns)
                    return horizon_gpu_err(HORIZON_GPU_ERR_TIMEOUT);
                wait_us = horizon_timeout_ns_to_us_clamped(timeout_ns - used);
            }
            bool reached = false;
            res = sync_fence_reached_via_wait(fence, wait_us, &reached);
            if (horizon_gpu_failed(res))
                return res;
            if (reached)
                return horizon_gpu_ok();
            if (timeout_ns != HORIZON_GPU_NO_TIMEOUT)
                return horizon_gpu_err(HORIZON_GPU_ERR_TIMEOUT);
            continue;
        }
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
