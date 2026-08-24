/*
 * horizon_gpu — synchronisation: syncpoints and fences.
 *
 * A fence is a value type {syncpoint id, threshold}; it is signalled when
 * the counter has reached the threshold in the modular sense. Comparisons
 * are wrap-safe; direct `<`/`>=` on thresholds is forbidden project-wide.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef HORIZON_GPU_SYNC_H
#define HORIZON_GPU_SYNC_H

#include <stdbool.h>
#include <stdint.h>

#include "device.h"
#include "result.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct horizon_gpu_fence {
    uint32_t syncpt_id;
    uint32_t threshold;
} horizon_gpu_fence;

/* "No deadline". Implemented as a bounded loop that keeps re-checking —
 * never as an infinite kernel wait. */
#define HORIZON_GPU_NO_TIMEOUT UINT64_MAX

/* True when `current` has reached or passed `threshold`, tolerating
 * 32-bit wraparound. Valid while the in-flight window stays below 2^31
 * increments. */
static inline bool horizon_gpu_syncpt_reached(uint32_t current,
                                              uint32_t threshold)
{
    return (int32_t)(current - threshold) >= 0;
}

/* Reads the current value of a syncpoint (NVHOST_IOCTL_CTRL_SYNCPT_READ).
 * Never blocks. */
horizon_gpu_result horizon_gpu_syncpt_read(horizon_gpu_device *dev,
                                           uint32_t syncpt_id,
                                           uint32_t *out_value);

/* Increments a syncpoint from the CPU
 * (NVHOST_IOCTL_CTRL_SYNCPT_INCR). Never blocks.
 *
 * WHAT THIS IS FOR. A Vulkan semaphore or fence signalled from the host
 * has to become visible to waiters on the GPU side, and a syncpoint is
 * the only thing on this platform both sides can see. Without this the
 * host-signal path can only set a flag the GPU never reads.
 *
 * WHICH SYNCPOINT IT MAY BE CALLED ON, AND THIS IS THE WHOLE DANGER.
 * NOT a channel's own syncpoint. libnx tracks a channel's next fence
 * value as `fence.value + fence_incr` and horizon_gpu_channel keeps a
 * 64-bit shadow on top of that; both count the increments the GPU will
 * make, and neither can see one made behind their backs. An increment
 * here on a channel's syncpoint makes every fence that channel has
 * already handed out appear reached one submit early — which is not a
 * failure that reports itself, it is a wait that returns before the work
 * did.
 *
 * So the caller owns the syncpoint it names. This function does not and
 * cannot check that; nothing in the ioctl distinguishes the two.
 *
 * Whether an increment from the CPU is honoured at all, and whether it
 * disturbs the channel that owns the syncpoint, is what t_syncpt_incr
 * measures. Until that has run on a console this is cross-compiled
 * code, not a working mechanism. */
horizon_gpu_result horizon_gpu_syncpt_incr(horizon_gpu_device *dev,
                                           uint32_t syncpt_id);

/* Non-blocking fence query. */
horizon_gpu_result horizon_gpu_fence_poll(horizon_gpu_device *dev,
                                          horizon_gpu_fence fence,
                                          bool *out_signalled);

/* Blocks until the fence is reached or `timeout_ns` elapses
 * (HORIZON_GPU_ERR_TIMEOUT — a timeout is returned, never downgraded to
 * success). The public boundary is nanoseconds, matching Vulkan; the
 * single conversion to libnx's microsecond waits saturates instead of
 * truncating. For waits that should detect
 * a faulted channel, prefer horizon_gpu_channel_wait_fence. */
horizon_gpu_result horizon_gpu_fence_wait(horizon_gpu_device *dev,
                                          horizon_gpu_fence fence,
                                          uint64_t timeout_ns);

#ifdef __cplusplus
}
#endif

#endif /* HORIZON_GPU_SYNC_H */
