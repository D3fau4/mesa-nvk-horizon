/*
 * Phase 1 test 8 — t_syncpt: the syncpoint increases monotonically by
 * exactly one per submit — one increment requested, one emitted, never
 * two (milestones.md; known-risks R4).
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <inttypes.h>

#include "horizon_gpu/channel.h"
#include "horizon_gpu/device.h"
#include "horizon_gpu/submit.h"
#include "horizon_gpu/sync.h"
#include "common/testfw.h"

const char *const test_name = "t_syncpt";
/* No display: main() starts a console and reports through it. */
const bool test_uses_display = false;

#define WAIT_NS UINT64_C(2000000000)
#define NUM_SUBMITS 10

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

    uint32_t id = horizon_gpu_channel_syncpt_id(chan);
    uint32_t before = 0;
    res = horizon_gpu_syncpt_read(dev, id, &before);
    t_check(t, horizon_gpu_succeeded(res), "initial syncpt read (%u)",
            before);
    t_check(t, before == horizon_gpu_channel_syncpt_value_at_create(chan),
            "no stray increments between create and first read");

    /* Exactly +1 per submit. */
    uint32_t prev = before;
    bool monotonic_by_one = true;
    for (int i = 0; i < NUM_SUBMITS; i++) {
        horizon_gpu_fence f;
        res = horizon_gpu_submit(chan, NULL, 0, HORIZON_GPU_SUBMIT_DEFAULT,
                                 &f);
        if (!t_check(t, horizon_gpu_succeeded(res), "submit %d accepted", i))
            break;
        if (!t_check(t, f.threshold == prev + 1,
                     "submit %d fence threshold is previous+1 (%u -> %u)",
                     i, prev, f.threshold)) {
            monotonic_by_one = false;
            break;
        }
        res = horizon_gpu_channel_wait_fence(chan, f, WAIT_NS);
        if (!t_check(t, horizon_gpu_succeeded(res),
                     "submit %d completed (status=%s)", i,
                     horizon_gpu_status_str(res.status)))
            break;
        uint32_t now = 0;
        horizon_gpu_result read_res = horizon_gpu_syncpt_read(dev, id, &now);
        if (!t_check(t, horizon_gpu_succeeded(read_res) && now == prev + 1,
                     "hardware counter advanced by exactly one (status=%s, "
                     "%u -> %u)", horizon_gpu_status_str(read_res.status),
                     prev, now)) {
            monotonic_by_one = false;
            break;
        }
        prev = now;
    }
    t_check(t, monotonic_by_one && prev == before + NUM_SUBMITS,
            "%d submits produced exactly %d increments (%u -> %u)",
            NUM_SUBMITS, NUM_SUBMITS, before, prev);

    /* The 64-bit shadow agrees with hardware once idle. */
    t_check(t, (uint32_t)horizon_gpu_channel_shadow_target(chan) == prev,
            "shadow target low word matches hardware (%" PRIu64 " vs %u)",
            horizon_gpu_channel_shadow_target(chan), prev);

    t_check(t, horizon_gpu_succeeded(horizon_gpu_channel_destroy(chan)),
            "channel destroy");
    res = horizon_gpu_device_destroy(dev);
    t_check(t, horizon_gpu_succeeded(res), "device_destroy (status=%s)",
            horizon_gpu_status_str(res.status));
    return 0;
}
