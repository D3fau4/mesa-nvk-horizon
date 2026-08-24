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
 * re-checkable. */
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

horizon_gpu_result horizon_gpu_syncpt_incr(horizon_gpu_device *dev,
                                           uint32_t syncpt_id)
{
    if (!dev)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);

    /* Same fd as the read above: /dev/nvhost-ctrl, open since bring-up
     * step 2. The ioctl takes the id and nothing else — there is no way
     * for it, or for us, to tell a syncpoint the caller owns from one a
     * channel is counting. sync.h says whose job that is. */
    Result rc = nvioctlNvhostCtrl_SyncptIncr(nvFenceGetFd(), syncpt_id);
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
    /* How much of the last chunk the kernel did not spend blocking. See
     * the long comment at the bottom of this loop. */
    uint64_t unslept_ns = 0;
    bool reported_spin = false;

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

        /* THE LAST CHUNK DID NOT BLOCK, AND THE COUNTER STILL DISAGREES.
         *
         * Sleeping out what the chunk did not spend is what keeps this
         * loop a wait instead of a spin. It happens here, after the
         * counter has been re-read and found short and after the
         * deadline has been re-checked, so a fence that genuinely
         * retired during the chunk returns above at full speed and pays
         * none of this. Then round the loop rather than waiting: the
         * counter is what decides, and it is read at the top. */
        if (unslept_ns != 0) {
            uint64_t nap_ns = unslept_ns;
            if (timeout_ns != HORIZON_GPU_NO_TIMEOUT &&
                nap_ns > timeout_ns - elapsed_ns)
                nap_ns = timeout_ns - elapsed_ns;
            unslept_ns = 0;
            svcSleepThread((s64)nap_ns);
            continue;
        }

        int32_t chunk_us = SYNC_WAIT_CHUNK_US;
        if (timeout_ns != HORIZON_GPU_NO_TIMEOUT) {
            int32_t rem_us =
                horizon_timeout_ns_to_us_clamped(timeout_ns - elapsed_ns);
            if (rem_us < chunk_us)
                chunk_us = rem_us;
        }

        /* WHAT THE CHUNK RETURNED IS NOT DISCARDABLE, AND THIS IS WHY.
         *
         * TimedOut means the wait was armed and the whole chunk elapsed:
         * the loop's normal pulse, nothing to do. Anything else means
         * the kernel came back without having blocked, and issuing the
         * next chunk immediately turns a bounded wait into a hot loop
         * of two ioctls for the caller's entire deadline.
         *
         * That is not hypothetical. MEASURED on 2026-08-24
         * (t_fence_wait_many): nvFenceWait returns *success* in 0 ms for
         * a threshold past the syncpoint's maximum — nvhost reports such
         * a threshold as expired rather than block forever on an
         * increment nothing will make. The counter says otherwise, and
         * the counter is what this function answers with, so the loop
         * span 300 ms of ioctls per thread, 32 threads at once, and
         * reported the right answer the whole time. Nothing above could
         * have seen it.
         *
         * The success case is deliberately NOT turned into "reached".
         * The counter is the fence's truth: returning ok on the kernel's
         * word here would tell a caller the GPU had written memory it
         * had not written. Honour the deadline, report, and stop
         * burning the core.
         */
        uint64_t chunk_start = armGetSystemTick();
        Result rc = nvFenceWait(&nvf, chunk_us);
        if (rc == KERNELRESULT(TimedOut))
            continue;

        uint64_t spent_ns = armTicksToNs(armGetSystemTick() - chunk_start);
        uint64_t asked_ns = (uint64_t)(uint32_t)chunk_us * 1000u;
        unslept_ns = spent_ns < asked_ns ? asked_ns - spent_ns : 0;

        if (!reported_spin && unslept_ns != 0) {
            reported_spin = true;
            horizon_logf(&dev->log, HORIZON_LOG_WARN,
                         "fence wait: syncpt %u chunk returned 0x%08x after "
                         "%llu us of %d, and the counter has not reached "
                         "%u — pacing the rest of the deadline",
                         fence.syncpt_id, rc,
                         (unsigned long long)(spent_ns / 1000), chunk_us,
                         fence.threshold);
        }
    }
}
