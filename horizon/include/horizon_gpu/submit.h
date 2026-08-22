/*
 * horizon_gpu — asynchronous GPFIFO submission.
 *
 * horizon_gpu_submit appends the caller's command spans, performs exactly
 * one indivisible syncpoint-increment (libnx expected-value bump + the
 * in-stream increment command list — known-risks R4), kicks off, and
 * returns the completion fence WITHOUT WAITING. It never calls
 * nvFenceWait (docs/synchronization.md § 2; CLAUDE.md rejected design 6).
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef HORIZON_GPU_SUBMIT_H
#define HORIZON_GPU_SUBMIT_H

#include <stdint.h>

#include "channel.h"
#include "result.h"
#include "sync.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One span of GPU-visible command dwords (built by the caller, already
 * flushed to memory, mapped in the device's address space). */
typedef struct horizon_gpu_cmd_span {
    uint64_t gpu_va;
    uint32_t num_dwords;
} horizon_gpu_cmd_span;

/* GPFIFO entry flags. The reference found that every submit which worked
 * on hardware used NOT_MAIN | NO_PREFETCH and that 0 failed
 * (known-risks R3); the flags stay caller-visible so Phase 1 test 7 can
 * measure both combinations on our own code instead of inheriting
 * folklore. Values match libnx's GPFIFO_ENTRY_* bits. */
typedef enum horizon_gpu_submit_flags {
    HORIZON_GPU_SUBMIT_DEFAULT = 0,          /* NOT_MAIN | NO_PREFETCH */
    HORIZON_GPU_SUBMIT_ENTRY_FLAGS_ZERO = 1, /* R3 experiment: raw 0    */
} horizon_gpu_submit_flags;

/* Appends `num_spans` spans (may be 0: fence-only submit), one increment,
 * kicks off, returns the fence. Fails fast with CHANNEL_LOST on a lost
 * channel; a rejected kickoff is surfaced (BUSY for a full ring — the
 * caller decides whether to wait on the oldest fence — NV otherwise),
 * never retried blindly and never hidden behind a sleep
 * (docs/synchronization.md § 2.1). */
horizon_gpu_result horizon_gpu_submit(horizon_gpu_channel *chan,
                                      const horizon_gpu_cmd_span *spans,
                                      uint32_t num_spans,
                                      horizon_gpu_submit_flags flags,
                                      horizon_gpu_fence *out_fence);

/* How many foreign fences one horizon_gpu_submit_waits call can take.
 * The channel's wait ring is sized from this; a caller with more must
 * split, and one that asks for more is refused rather than truncated. */
#define HORIZON_GPU_MAX_WAIT_FENCES 8u

/* Submits a GPU-SIDE wait: the channel's host engine blocks on each
 * fence's syncpoint until its threshold is reached, and everything
 * submitted to this channel afterwards is ordered behind it because a
 * GPFIFO executes in order.
 *
 * THIS IS THE POINT: the calling thread does not block. A CPU wait on
 * another channel's fence stalls the thread that is about to record the
 * work, which on a swapchain acquire is the whole frame; the host engine
 * waiting instead costs nothing anybody is waiting for.
 *
 * The wait command list is written into a ring of slots inside the
 * channel's own command page, so a submitted list cannot be overwritten
 * while the GPU may still be fetching it. A slot is reused only once the
 * syncpoint has passed the fence of the submit that used it.
 *
 * Returns HORIZON_GPU_ERR_BUSY when every slot is still in flight. That
 * is a real answer and not a failure: the caller may wait on the CPU
 * instead, which is what the driver does.
 *
 * A fence from a channel that has faulted is not distinguished here.
 * nvgpu's recovery force-increments a lost channel's syncpoints, so such
 * a wait unblocks rather than hanging — but it unblocks on a counter,
 * not on work, which is the same caveat horizon_gpu_channel_wait_fence
 * documents and the reason that call reports CHANNEL_LOST where this one
 * cannot. */
horizon_gpu_result horizon_gpu_submit_waits(horizon_gpu_channel *chan,
                                            const horizon_gpu_fence *fences,
                                            uint32_t num_fences,
                                            horizon_gpu_fence *out_fence);

#ifdef __cplusplus
}
#endif

#endif /* HORIZON_GPU_SUBMIT_H */
