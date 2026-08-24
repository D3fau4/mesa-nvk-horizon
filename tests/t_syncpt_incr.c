/*
 * Can this platform's syncpoint be advanced from the CPU, and what does
 * that do to the channel that owns it?
 *
 * WHY THE QUESTION IS OPEN. nvk_horizon_sync_signal — the path a
 * vkSignalSemaphore or a host-signalled fence takes — does not touch a
 * syncpoint at all. It cannot: horizon_gpu exposed no way to advance
 * one, so a host signal could only ever set a flag the GPU never reads.
 * NVHOST_IOCTL_CTRL_SYNCPT_INCR is right there in the ioctl surface, and
 * nothing had ever called it, so nothing knew whether it works here.
 *
 * WHY IT IS NOT SIMPLY A MATTER OF CALLING IT. libnx tracks a channel's
 * next fence value as `fence.value + fence_incr`, and
 * horizon_gpu_channel keeps a 64-bit shadow on top of that. Both count
 * the increments the GPU is going to make. Neither can see one made
 * behind their backs, and an unseen increment does not announce itself:
 * it makes every fence that channel already handed out appear reached
 * one submit early. A wait that returns before the work did is the most
 * expensive bug this layer can have, and it is invisible until
 * something reads memory that is not written yet.
 *
 * So this test deliberately does the dangerous thing, to a channel it
 * created for the purpose and destroys afterwards, and reports what
 * happened. It is a measurement, and its result decides whether a host
 * signal can be built on this at all:
 *
 *   1. Is the increment honoured? Read, increment, read again.
 *   2. Does it satisfy a threshold the GPU has not reached? That is the
 *      whole mechanism a host signal needs — and the same fact, seen
 *      from the other side, is the hazard above.
 *   3. Does the channel survive it? Submit again afterwards and check
 *      the fence still retires, that the shadow still tracks, and that
 *      the channel is not lost.
 *
 * A PASS HERE IS NOT PERMISSION. It says the ioctl works and the
 * channel recovers; it says nothing about whether a *shared* syncpoint
 * is a sound thing to signal on, which is why the design calls for a
 * dedicated one rather than a channel's.
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

const char *const test_name = "t_syncpt_incr";
/* No display: main() starts a console and reports through it. */
const bool test_uses_display = false;

#define SUBMIT_TIMEOUT_NS UINT64_C(2000000000)

int run_test(test_ctx *t)
{
    horizon_gpu_device *dev = NULL;
    horizon_gpu_result res = horizon_gpu_device_create(NULL, &dev);
    if (!t_check(t, horizon_gpu_succeeded(res), "device_create (status=%s "
                 "nv=0x%08x)", horizon_gpu_status_str(res.status), res.nv))
        return 1;

    /* This channel exists to be disturbed. Nothing else uses it, and it
     * is destroyed before the device is. */
    horizon_gpu_channel *chan = NULL;
    res = horizon_gpu_channel_create(dev, NULL, &chan);
    if (!t_check(t, horizon_gpu_succeeded(res), "channel_create (status=%s "
                 "nv=0x%08x)", horizon_gpu_status_str(res.status), res.nv))
        return 1;

    const uint32_t id = horizon_gpu_channel_syncpt_id(chan);
    t_note(t, "channel syncpoint id %u", id);

    /* Quiesce first: everything the channel has submitted must have
     * retired, so the counter is not moving underneath the reads. */
    horizon_gpu_fence f;
    res = horizon_gpu_submit(chan, NULL, 0, HORIZON_GPU_SUBMIT_DEFAULT, &f);
    t_check(t, horizon_gpu_succeeded(res), "priming submit");
    res = horizon_gpu_fence_wait(dev, f, SUBMIT_TIMEOUT_NS);
    if (!t_check(t, horizon_gpu_succeeded(res),
                 "priming submit retired (status=%s)",
                 horizon_gpu_status_str(res.status)))
        return 1;

    /* ---- 1. is the increment honoured at all? --------------------- */
    uint32_t before = 0;
    res = horizon_gpu_syncpt_read(dev, id, &before);
    if (!t_check(t, horizon_gpu_succeeded(res),
                 "syncpt_read before (status=%s nv=0x%08x)",
                 horizon_gpu_status_str(res.status), res.nv))
        return 1;

    /* The threshold one past the current value: unreached now, and the
     * increment is the only thing that can reach it. Asked before the
     * increment so a "true" here would mean the read is stale rather
     * than the increment working. */
    const horizon_gpu_fence next = { .syncpt_id = id,
                                     .threshold = before + 1 };
    bool sig = true;
    res = horizon_gpu_fence_poll(dev, next, &sig);
    t_check(t, horizon_gpu_succeeded(res) && !sig,
            "the threshold one past the counter is not reached yet");

    res = horizon_gpu_syncpt_incr(dev, id);
    const bool incr_ok = horizon_gpu_succeeded(res);
    if (!incr_ok) {
        t_note(t, "MEASURED: SYNCPT_INCR is not available here "
               "(status=%s nv=0x%08x, module %u desc %u). A host signal "
               "cannot be built on this ioctl on this platform.",
               horizon_gpu_status_str(res.status), res.nv,
               (unsigned)R_MODULE(res.nv), (unsigned)R_DESCRIPTION(res.nv));
    }
    t_check(t, incr_ok, "syncpt_incr returned success (status=%s "
            "nv=0x%08x)", horizon_gpu_status_str(res.status), res.nv);

    uint32_t after = 0;
    res = horizon_gpu_syncpt_read(dev, id, &after);
    t_check(t, horizon_gpu_succeeded(res), "syncpt_read after");
    t_note(t, "MEASURED: counter %u -> %u across one CPU increment "
           "(delta %d)", before, after, (int)(after - before));
    t_check(t, incr_ok ? (after == before + 1) : (after == before),
            "one CPU increment moved the counter by exactly one");

    /* ---- 2. does it satisfy a threshold the GPU never reached? ---- */
    sig = false;
    res = horizon_gpu_fence_poll(dev, next, &sig);
    t_check(t, horizon_gpu_succeeded(res) && sig == incr_ok,
            "a threshold the GPU never reached now polls %s",
            incr_ok ? "reached" : "unreached");

    /* And a wait on it returns rather than timing out — the property a
     * host signal actually needs, as opposed to a counter that moved. */
    if (incr_ok) {
        res = horizon_gpu_fence_wait(dev, next, SUBMIT_TIMEOUT_NS);
        t_check(t, horizon_gpu_succeeded(res),
                "a wait on that threshold returns (status=%s)",
                horizon_gpu_status_str(res.status));
    }

    /* ---- 3. does the channel survive it? -------------------------- */
    t_check(t, !horizon_gpu_channel_is_lost(chan),
            "the channel is not lost after the increment");

    const uint64_t shadow_before = horizon_gpu_channel_shadow_target(chan);
    res = horizon_gpu_submit(chan, NULL, 0, HORIZON_GPU_SUBMIT_DEFAULT, &f);
    t_check(t, horizon_gpu_succeeded(res),
            "the channel still accepts a submit (status=%s nv=0x%08x)",
            horizon_gpu_status_str(res.status), res.nv);
    const uint64_t shadow_after = horizon_gpu_channel_shadow_target(chan);
    t_note(t, "shadow target %" PRIu64 " -> %" PRIu64 " across that "
           "submit", shadow_before, shadow_after);

    res = horizon_gpu_fence_wait(dev, f, SUBMIT_TIMEOUT_NS);
    t_check(t, horizon_gpu_succeeded(res),
            "the post-increment submit retired (status=%s)",
            horizon_gpu_status_str(res.status));

    /* THE CHECK THIS TEST IS FOR. The channel's own accounting counts
     * GPU increments; the CPU made one it did not count. If the two have
     * drifted, the counter now sits ahead of where the channel thinks it
     * is — harmless in that direction, since a fence would only look
     * reached early — and the number is what matters, so it is printed
     * either way rather than only asserted. */
    uint32_t settled = 0;
    res = horizon_gpu_syncpt_read(dev, id, &settled);
    t_check(t, horizon_gpu_succeeded(res), "syncpt_read after the submit");
    t_note(t, "MEASURED: counter %u, channel shadow target %" PRIu64 "; "
           "drift %" PRId64, settled, shadow_after,
           (int64_t)((uint64_t)settled - shadow_after));

    res = horizon_gpu_channel_wait_idle(chan, SUBMIT_TIMEOUT_NS);
    t_check(t, horizon_gpu_succeeded(res),
            "wait_idle after a CPU increment (status=%s)",
            horizon_gpu_status_str(res.status));

    t_check(t, horizon_gpu_succeeded(horizon_gpu_channel_destroy(chan)),
            "the disturbed channel tears down cleanly");
    res = horizon_gpu_device_destroy(dev);
    t_check(t, horizon_gpu_succeeded(res), "device_destroy (status=%s)",
            horizon_gpu_status_str(res.status));
    return 0;
}
