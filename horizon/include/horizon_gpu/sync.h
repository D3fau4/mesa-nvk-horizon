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
