/*
 * horizon_gpu — GPFIFO channels: creation, Zcull, explicit engine binds,
 * retirement, error decoding.
 *
 * A channel owns an NvGpuChannel, its syncpoint, a small internal command
 * buffer (the per-submit syncpoint-increment list) and, optionally, its
 * Zcull context. Engine binding is an explicit operation the caller must
 * invoke before using engine methods — never hidden inside the first
 * submit (known-risks R7). A faulted channel is marked lost and every
 * subsequent submit fails fast with the decoded reason
 * (architecture.md § 6).
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef HORIZON_GPU_CHANNEL_H
#define HORIZON_GPU_CHANNEL_H

#include <stdbool.h>
#include <stdint.h>

#include "device.h"
#include "result.h"
#include "sync.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct horizon_gpu_channel horizon_gpu_channel;

typedef enum horizon_gpu_channel_prio {
    HORIZON_GPU_CHANNEL_PRIO_LOW = 0,
    HORIZON_GPU_CHANNEL_PRIO_MEDIUM = 1,
    HORIZON_GPU_CHANNEL_PRIO_HIGH = 2,
} horizon_gpu_channel_prio;

typedef struct horizon_gpu_channel_create_info {
    horizon_gpu_channel_prio prio;
    /* Bind a Zcull context (queried size, 128 KiB-aligned VA). Not needed
     * until real 3D work; t_channel exercises both settings. */
    bool bind_zcull;
} horizon_gpu_channel_create_info;

/* `create_info` may be NULL: medium priority, no Zcull. */
horizon_gpu_result
horizon_gpu_channel_create(horizon_gpu_device *dev,
                           const horizon_gpu_channel_create_info *create_info,
                           horizon_gpu_channel **out_chan);

/* Fails with HORIZON_GPU_ERR_BUSY while submits are in flight (wait idle
 * first); on success tears down Zcull/cmdbuf mappings, reservations and
 * memory in reverse creation order, then closes the channel. */
horizon_gpu_result horizon_gpu_channel_destroy(horizon_gpu_channel *chan);

uint32_t horizon_gpu_channel_syncpt_id(const horizon_gpu_channel *chan);

/* Hardware syncpoint value observed when the channel was created —
 * measurement input for the R5 open question (shadow initialisation). */
uint32_t horizon_gpu_channel_syncpt_value_at_create(
    const horizon_gpu_channel *chan);

/* 64-bit shadow of the syncpoint value once every submitted increment has
 * retired (docs/synchronization.md § 1.1). */
uint64_t horizon_gpu_channel_shadow_target(const horizon_gpu_channel *chan);

/* Fence that signals when everything submitted so far has completed. */
horizon_gpu_fence
horizon_gpu_channel_last_fence(const horizon_gpu_channel *chan);

bool horizon_gpu_channel_is_lost(const horizon_gpu_channel *chan);

/* Emits the in-stream SET_OBJECT binds for subchannels 0..4 using the
 * device's queried class numbers and submits them asynchronously
 * (known-risks R7). Must be called once before engine methods are used;
 * calling it again is HORIZON_GPU_ERR_STATE. */
horizon_gpu_result
horizon_gpu_channel_bind_engines(horizon_gpu_channel *chan,
                                 horizon_gpu_fence *out_fence);

/* Non-blocking: reads the syncpoint once and runs every retirement
 * callback whose fence has been reached (docs/synchronization.md § 3). */
horizon_gpu_result horizon_gpu_channel_reap(horizon_gpu_channel *chan,
                                            uint32_t *out_retired);

/* Registers (fence, callback, context) for retirement at reap time.
 * The callback runs from reap/wait contexts; it must not submit. */
horizon_gpu_result
horizon_gpu_channel_add_retirement(horizon_gpu_channel *chan,
                                   horizon_gpu_fence fence,
                                   void (*fn)(void *ctx), void *ctx);

/* Fence wait that re-checks this channel's error notifier while waiting,
 * so a faulted channel yields HORIZON_GPU_ERR_CHANNEL_LOST instead of an
 * eternal timeout (docs/synchronization.md § 6). */
horizon_gpu_result
horizon_gpu_channel_wait_fence(horizon_gpu_channel *chan,
                               horizon_gpu_fence fence, uint64_t timeout_ns);

/* Waits on the fence of the most recent submit, then reaps. It does not
 * iterate or sleep (docs/synchronization.md § 5). */
horizon_gpu_result horizon_gpu_channel_wait_idle(horizon_gpu_channel *chan,
                                                 uint64_t timeout_ns);

/* Latest error notification: raw type (NvNotificationType) and a decoded
 * description. `*out_type` == 0 means no error recorded. */
horizon_gpu_result
horizon_gpu_channel_get_error(horizon_gpu_channel *chan, uint32_t *out_type,
                              const char **out_desc);

#ifdef __cplusplus
}
#endif

#endif /* HORIZON_GPU_CHANNEL_H */
