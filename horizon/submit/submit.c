/*
 * horizon_gpu — asynchronous GPFIFO submission.
 *
 * The increment discipline (known-risks R4): telling libnx the expected
 * fence value grew (nvGpuChannelIncrFence) and emitting the actual
 * in-stream syncpoint-increment command list are ONE indivisible
 * operation here — requesting without emitting stalls the channel
 * forever, emitting without requesting double-counts.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <switch.h>

#include "horizon_gpu/submit.h"
#include "../channel/channel_priv.h"
#include "../device/device_priv.h"
#include "../sync/syncpt_math.h"

/* Debug-synchronous mode bound: generous but finite
 * (docs/synchronization.md § 8). */
#define SUBMIT_DEBUG_SYNC_TIMEOUT_NS UINT64_C(2000000000)

horizon_gpu_result horizon_gpu_submit(horizon_gpu_channel *chan,
                                      const horizon_gpu_cmd_span *spans,
                                      uint32_t num_spans,
                                      horizon_gpu_submit_flags flags,
                                      horizon_gpu_fence *out_fence)
{
    if (!chan || (num_spans > 0 && !spans))
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);
    if (flags != HORIZON_GPU_SUBMIT_DEFAULT &&
        flags != HORIZON_GPU_SUBMIT_ENTRY_FLAGS_ZERO)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);
    for (uint32_t i = 0; i < num_spans; i++) {
        if (spans[i].gpu_va == 0 || spans[i].num_dwords == 0)
            return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);
    }

    if (chan->lost)
        return horizon_gpu_err(HORIZON_GPU_ERR_CHANNEL_LOST);

    horizon_gpu_device *dev = chan->dev;

    /* Cheap reap first: one syncpoint read
     * (docs/synchronization.md § 3). */
    horizon_gpu_result res = horizon_gpu_channel_reap(chan, NULL);
    if (horizon_gpu_failed(res))
        return res;

    /* Known-quantity back-pressure instead of retry loops: refuse when
     * the entry queue cannot take this submit; the caller may wait on an
     * older fence and retry (docs/synchronization.md § 2.1). */
    if (chan->gc.num_entries + num_spans + 1 > GPFIFO_QUEUE_SIZE) {
        horizon_logf(&dev->log, HORIZON_LOG_WARN,
                     "channel %p: GPFIFO entry queue full (%u queued, %u "
                     "requested)", (void *)chan, chan->gc.num_entries,
                     num_spans + 1);
        return horizon_gpu_err(HORIZON_GPU_ERR_BUSY);
    }

    /* R3 stays measurable: default is the combination the reference
     * proved on hardware; ZERO submits NVK's upstream default so test 7
     * can compare. */
    u32 entry_flags = (flags == HORIZON_GPU_SUBMIT_ENTRY_FLAGS_ZERO)
                          ? 0
                          : (GPFIFO_ENTRY_NOT_MAIN |
                             GPFIFO_ENTRY_NO_PREFETCH);

    u32 entries_before = chan->gc.num_entries;
    Result rc;

    for (uint32_t i = 0; i < num_spans; i++) {
        rc = nvGpuChannelAppendEntry(&chan->gc, spans[i].gpu_va,
                                     spans[i].num_dwords, entry_flags, 0);
        if (R_FAILED(rc)) {
            chan->gc.num_entries = entries_before; /* drop partial append */
            horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                         "AppendEntry(va=0x%llx n=%u) failed: 0x%08x",
                         (unsigned long long)spans[i].gpu_va,
                         spans[i].num_dwords, rc);
            return horizon_gpu_err_nv(rc);
        }
    }

    /* --- indivisible: request the increment AND emit its command --- */
    nvGpuChannelIncrFence(&chan->gc);
    rc = nvGpuChannelAppendEntry(&chan->gc, chan->fence_cmds_va,
                                 chan->fence_cmds_dwords, entry_flags, 0);
    if (R_FAILED(rc)) {
        chan->gc.fence_incr--; /* undo the request: keep the two in step */
        chan->gc.num_entries = entries_before;
        horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                     "AppendEntry(fence cmdlist) failed: 0x%08x", rc);
        return horizon_gpu_err_nv(rc);
    }
    /* --------------------------------------------------------------- */

    rc = nvGpuChannelKickoff(&chan->gc);
    if (R_FAILED(rc)) {
        /* Surface the rejection; never retried blindly, never slept on
         * (the reference retries 400 times holding its global lock —
         * drm_shim.c:666-676). Whether a distinct "ring full" kickoff
         * code exists is measured on hardware by t_submit. */
        chan->gc.fence_incr--;
        chan->gc.num_entries = entries_before;
        uint32_t err_type = 0;
        const char *desc = "unavailable";
        (void)horizon_gpu_channel_get_error(chan, &err_type, &desc);
        if (err_type != 0)
            chan->lost = true;
        horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                     "Kickoff failed: 0x%08x (notifier: %u '%s')", rc,
                     err_type, desc);
        return horizon_gpu_err_nv(rc);
    }

    chan->shadow_target += 1;

    horizon_gpu_fence fence = {
        .syncpt_id = chan->syncpt_id,
        .threshold = (uint32_t)chan->shadow_target,
    };

    /* libnx tracks the absolute fence value the kernel returned; our
     * shadow must agree with its low word. A mismatch means the kernel
     * incremented differently than requested, which invalidates every
     * future threshold derived from the shadow (R5) — the work already
     * reached hardware (kickoff succeeded), but this and every later
     * fence from this channel can no longer be trusted, so the channel
     * is marked lost rather than returning a fence callers would wait on
     * incorrectly. */
    if (chan->gc.fence.value != fence.threshold) {
        horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                     "shadow (%u) and kernel fence (%u) disagree on "
                     "syncpt %u — marking channel lost", fence.threshold,
                     chan->gc.fence.value, chan->syncpt_id);
        chan->lost = true;
        return horizon_gpu_err(HORIZON_GPU_ERR_CHANNEL_LOST);
    }

    horizon_logf(&dev->log, HORIZON_LOG_DEBUG,
                 "channel %p: submitted %u span(s)+incr, fence=%u:%u",
                 (void *)chan, num_spans, fence.syncpt_id, fence.threshold);

    /* Diagnostic mode only — compiled in, never taken otherwise, and no
     * test may need it to pass (docs/synchronization.md § 8). */
    if (dev->debug_synchronous) {
        horizon_gpu_result wres =
            horizon_gpu_channel_wait_fence(chan, fence,
                                           SUBMIT_DEBUG_SYNC_TIMEOUT_NS);
        uint32_t err_type = 0;
        const char *desc = "none";
        (void)horizon_gpu_channel_get_error(chan, &err_type, &desc);
        horizon_logf(&dev->log, HORIZON_LOG_INFO,
                     "[sync-mode] fence %u:%u wait=%s notifier=%u '%s'",
                     fence.syncpt_id, fence.threshold,
                     horizon_gpu_status_str(wres.status), err_type, desc);
    }

    if (out_fence)
        *out_fence = fence;
    return horizon_gpu_ok();
}
