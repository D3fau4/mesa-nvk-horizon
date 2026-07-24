/*
 * Phase 1 test 9 — t_fence_wait: a wait returns after completion; the
 * timeout path returns HORIZON_GPU_ERR_TIMEOUT after (approximately) the
 * requested time — which also catches ns/µs unit bugs, the reference's
 * 2 s -> 2000 s mistake (milestones.md; docs/synchronization.md § 6).
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <inttypes.h>

#include <switch.h>

#include "horizon_gpu/channel.h"
#include "horizon_gpu/device.h"
#include "horizon_gpu/submit.h"
#include "horizon_gpu/sync.h"
#include "common/testfw.h"

const char *const test_name = "t_fence_wait";

int run_test(test_ctx *t)
{
    horizon_gpu_device *dev = NULL;
    horizon_gpu_result res = horizon_gpu_device_create(NULL, &dev);
    if (!t_check(t, horizon_gpu_succeeded(res), "device_create (status=%s "
                 "nv=0x%08x)", horizon_gpu_status_str(res.status), res.nv))
        return 1;

    horizon_gpu_channel *chan = NULL;
    res = horizon_gpu_channel_create(dev, NULL, &chan);
    if (!t_check(t, horizon_gpu_succeeded(res), "channel_create (status=%s "
                 "nv=0x%08x)", horizon_gpu_status_str(res.status), res.nv))
        return 1;

    /* Wait returns after completion. */
    horizon_gpu_fence f;
    res = horizon_gpu_submit(chan, NULL, 0, HORIZON_GPU_SUBMIT_DEFAULT, &f);
    t_check(t, horizon_gpu_succeeded(res), "submit");
    res = horizon_gpu_fence_wait(dev, f, UINT64_C(2000000000));
    t_check(t, horizon_gpu_succeeded(res),
            "wait on submitted fence returned success (status=%s)",
            horizon_gpu_status_str(res.status));

    /* Already-signalled fence returns promptly. */
    uint64_t tick0 = armGetSystemTick();
    res = horizon_gpu_fence_wait(dev, f, UINT64_C(2000000000));
    uint64_t quick_ns = armTicksToNs(armGetSystemTick() - tick0);
    t_check(t, horizon_gpu_succeeded(res) && quick_ns < UINT64_C(50000000),
            "already-signalled wait returns immediately (%" PRIu64 " us)",
            quick_ns / 1000);

    /* Poll: signalled true, future false. */
    bool sig = false;
    res = horizon_gpu_fence_poll(dev, f, &sig);
    t_check(t, horizon_gpu_succeeded(res) && sig, "poll(signalled) == true");
    horizon_gpu_fence future = { .syncpt_id = f.syncpt_id,
                                 .threshold = f.threshold + 1000 };
    res = horizon_gpu_fence_poll(dev, future, &sig);
    t_check(t, horizon_gpu_succeeded(res) && !sig, "poll(future) == false");

    /* Timeout path: 200 ms request must return TIMEOUT and take roughly
     * 200 ms — not 0 (unit truncation) and not minutes (unit inflation).
     * The bound is generous for scheduler noise. */
    const uint64_t timeout_ns = UINT64_C(200000000);
    tick0 = armGetSystemTick();
    res = horizon_gpu_fence_wait(dev, future, timeout_ns);
    uint64_t waited_ns = armTicksToNs(armGetSystemTick() - tick0);
    t_note(t, "200 ms timeout wait took %" PRIu64 " ms",
           waited_ns / 1000000);
    t_check(t, res.status == HORIZON_GPU_ERR_TIMEOUT,
            "unsignalled wait returned TIMEOUT (status=%s)",
            horizon_gpu_status_str(res.status));
    t_check(t, waited_ns >= timeout_ns, "wait honoured the full timeout");
    t_check(t, waited_ns < UINT64_C(2000000000),
            "wait did not inflate the timeout (unit bug guard)");

    /* Channel-aware wait behaves identically on a healthy channel. */
    res = horizon_gpu_channel_wait_fence(chan, future, timeout_ns);
    t_check(t, res.status == HORIZON_GPU_ERR_TIMEOUT,
            "channel wait also times out cleanly (status=%s)",
            horizon_gpu_status_str(res.status));

    /* Zero timeout degenerates to a poll. */
    res = horizon_gpu_fence_wait(dev, future, 0);
    t_check(t, res.status == HORIZON_GPU_ERR_TIMEOUT,
            "zero timeout returns TIMEOUT immediately (status=%s)",
            horizon_gpu_status_str(res.status));

    t_check(t, horizon_gpu_succeeded(horizon_gpu_channel_destroy(chan)),
            "channel destroy");
    res = horizon_gpu_device_destroy(dev);
    t_check(t, horizon_gpu_succeeded(res), "device_destroy (status=%s)",
            horizon_gpu_status_str(res.status));
    return 0;
}
