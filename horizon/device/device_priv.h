/*
 * horizon_gpu — device internals shared by the sibling implementation
 * modules (memory/, vm/, channel/, submit/, sync/). Never installed;
 * everything above the layer sees only horizon_gpu/device.h.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef HORIZON_DEVICE_PRIV_H
#define HORIZON_DEVICE_PRIV_H

#include <stdatomic.h>

#include <switch.h>

#include "horizon_gpu/device.h"
#include "../debug/log.h"

struct horizon_gpu_device {
    /* GPU address space every allocation and channel of this device lives
     * in. Owned here; closed last-but-one at destroy (before nvGpuExit). */
    NvAddressSpace as;

    horizon_gpu_device_info info;

    horizon_log log;
    bool debug_synchronous;

    /* Two opt-outs that exist so a single console run can measure the
     * change against the behaviour it replaced, rather than a number
     * from this build being compared with a number from another one.
     * Both restore exactly what the code did before; neither is a
     * supported configuration.
     *
     * full_barrier_waits  HORIZON_GPU_FULL_BARRIER_WAITS=1 — put the
     *                     L2-invalidate prologue and the wait-for-idle
     *                     + L2-writeback fence block back on a
     *                     memory-free wait submit.
     * eager_reap          HORIZON_GPU_EAGER_REAP=1 — read the syncpoint
     *                     on every reap even when the retirement list
     *                     is empty and nothing can retire. */
    bool full_barrier_waits;
    bool eager_reap;

    /* Opt-in (device.h) and the sticky record of it having been used. The
     * flag is written by channel creation from any thread, so it is atomic
     * like the counters below. */
    bool allow_untrusted_syncpt_baseline;
    _Atomic bool untrusted_syncpt_seen;

    /* Leak accounting (memory-model § 8) only. These counters are atomic
     * so they can be read (horizon_gpu_device_get_counters) while another
     * thread is creating or destroying an unrelated object, but that is
     * the full extent of the thread-safety this API provides: the
     * structures each counter describes (a mem's mapping list, a range's
     * live-interval set, a channel's retirement list) are plain,
     * unsynchronized data. Concurrent calls that touch the *same* object,
     * or two objects that share one (e.g. two mappings of one mem), are
     * the caller's responsibility to serialize. */
    _Atomic uint32_t live_mem;
    _Atomic uint32_t live_va_ranges;
    _Atomic uint32_t live_mappings;
    _Atomic uint32_t live_channels;
};

#endif /* HORIZON_DEVICE_PRIV_H */
