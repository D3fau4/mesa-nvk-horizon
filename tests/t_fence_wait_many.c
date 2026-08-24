/*
 * How many syncpoint waits this platform will hold at once, and what
 * happens to horizon_gpu_fence_wait when that number is exceeded.
 *
 * WHY THE QUESTION MATTERS HERE. horizon_gpu_fence_wait (sync/syncpt.c)
 * is a loop: read the counter, and if the fence is not reached, block in
 * nvFenceWait for a 100 ms chunk before reading again. The chunk's
 * expiry is the loop's pulse, so the wait deliberately ignores what
 * nvFenceWait returned — genuine failures are meant to surface through
 * the SyncptRead above it.
 *
 * That is correct for a chunk that timed out. It is NOT correct for a
 * chunk that could not be armed at all: if nvFenceWait fails
 * immediately, the loop keeps its deadline and its result, and spends
 * the whole time re-reading a counter as fast as the ioctl returns. The
 * wait still answers correctly and still honours its timeout — it just
 * burns a core doing it, silently, and no test has ever looked.
 *
 * Whether that can happen depends on a number nothing in this tree
 * knows: how many concurrent syncpoint waits the driver will register.
 * A Vulkan application with a thread per queue and a fence per frame in
 * flight reaches whatever that number is as a matter of course.
 *
 * WHAT THIS MEASURES, IN TWO PARTS.
 *
 *   1. The ceiling, probed underneath our own layer. N threads call
 *      nvFenceWait directly on N distinct thresholds of one syncpoint
 *      and report their Result. A clean run is every thread coming back
 *      KERNELRESULT(TimedOut). The first N where something else appears
 *      is the ceiling, and the Result is named in the log.
 *
 *   2. The consequence, through our layer. At the ceiling and beyond,
 *      the same fan-out through horizon_gpu_fence_wait, timing the
 *      batch. Every thread must still return TIMEOUT — correctness is
 *      not in question — and the wall time says whether the waits ran
 *      concurrently or degenerated into the spin described above.
 *
 * WHAT IT DOES NOT DO. It does not fail on a low ceiling. The number is
 * a property of the platform, not a bug in this code, and the point of
 * running it is to learn the number. It fails only if a wait returns
 * something other than TIMEOUT, which would be a correctness break.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <inttypes.h>
#include <pthread.h>
#include <string.h>

#include <switch.h>

#include "horizon_gpu/channel.h"
#include "horizon_gpu/device.h"
#include "horizon_gpu/submit.h"
#include "horizon_gpu/sync.h"
#include "common/testfw.h"

const char *const test_name = "t_fence_wait_many";
/* No display: main() starts a console and reports through it. */
const bool test_uses_display = false;

/* Upper bound on the fan-out. Chosen so the worst case — every wait
 * serialised at WAIT_US each — stays inside a run a person will sit
 * through, not because anything says 32 is enough. */
#define MAX_THREADS 32u

/* Per-wait timeout. Long enough that a wait which was armed really does
 * block, short enough that a fully serialised batch of MAX_THREADS
 * finishes in about ten seconds. */
#define WAIT_US    INT32_C(300000)
#define WAIT_NS    UINT64_C(300000000)

/* Distance from the current counter value to the thresholds waited on.
 * Far enough that nothing in flight can reach them, well inside the
 * 2^31 window horizon_gpu_syncpt_reached is valid over. */
#define FUTURE_GAP UINT32_C(100000)

typedef struct {
    horizon_gpu_device *dev;
    horizon_gpu_fence fence;
    bool through_layer;      /* false: raw nvFenceWait; true: our wait */
    Result nv_rc;            /* raw path only */
    horizon_gpu_status status; /* layer path only */
    uint64_t elapsed_ns;
} waiter;

static void *waiter_main(void *arg)
{
    waiter *w = arg;
    uint64_t t0 = armGetSystemTick();

    if (w->through_layer) {
        horizon_gpu_result res =
            horizon_gpu_fence_wait(w->dev, w->fence, WAIT_NS);
        w->status = res.status;
        w->nv_rc = res.nv;
    } else {
        NvFence nvf = { .id = w->fence.syncpt_id,
                        .value = w->fence.threshold };
        w->nv_rc = nvFenceWait(&nvf, WAIT_US);
    }

    w->elapsed_ns = armTicksToNs(armGetSystemTick() - t0);
    return NULL;
}

/* Runs `n` waiters and reports. Returns the number that came back the
 * way a healthy platform answers an unreachable fence. */
static uint32_t fan_out(test_ctx *t, horizon_gpu_device *dev,
                        uint32_t syncpt_id, uint32_t base,
                        uint32_t n, bool through_layer,
                        uint64_t *out_batch_ns)
{
    static waiter w[MAX_THREADS];
    static pthread_t th[MAX_THREADS];

    memset(w, 0, sizeof(w));
    for (uint32_t i = 0; i < n; i++) {
        w[i].dev = dev;
        /* A distinct threshold per thread: one registration each, which
         * is what a queue with several fences in flight produces. */
        w[i].fence.syncpt_id = syncpt_id;
        w[i].fence.threshold = base + FUTURE_GAP + i;
        w[i].through_layer = through_layer;
    }

    uint64_t t0 = armGetSystemTick();
    uint32_t started = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (pthread_create(&th[i], NULL, waiter_main, &w[i]) != 0)
            break;
        started++;
    }
    for (uint32_t i = 0; i < started; i++)
        pthread_join(th[i], NULL);
    *out_batch_ns = armTicksToNs(armGetSystemTick() - t0);

    if (started != n) {
        t_note(t, "N=%u: only %u threads started; pthread_create refused "
               "the rest, so this row measures %u", n, started, started);
    }

    uint32_t clean = 0;
    Result first_other = 0;
    uint32_t first_other_at = 0;
    horizon_gpu_status first_bad_status = HORIZON_GPU_OK;

    for (uint32_t i = 0; i < started; i++) {
        bool ok;
        if (through_layer) {
            ok = (w[i].status == HORIZON_GPU_ERR_TIMEOUT);
            if (!ok && first_bad_status == HORIZON_GPU_OK) {
                first_bad_status = w[i].status;
                first_other = w[i].nv_rc;
                first_other_at = i;
            }
        } else {
            ok = (w[i].nv_rc == KERNELRESULT(TimedOut));
            if (!ok && first_other == 0) {
                first_other = w[i].nv_rc;
                first_other_at = i;
            }
        }
        if (ok)
            clean++;
    }

    if (clean != started) {
        if (through_layer) {
            t_note(t, "N=%u: %u/%u as expected; first divergent thread %u "
                   "returned status=%s nv=0x%08x", n, clean, started,
                   first_other_at,
                   horizon_gpu_status_str(first_bad_status),
                   (unsigned)first_other);
        } else {
            t_note(t, "N=%u: %u/%u timed out; first divergent thread %u "
                   "returned Result 0x%08x (module %u, desc %u)", n, clean,
                   started, first_other_at, (unsigned)first_other,
                   (unsigned)R_MODULE(first_other),
                   (unsigned)R_DESCRIPTION(first_other));
        }
    }
    return clean;
}

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

    /* One submit, only to obtain a syncpoint id and a value known to
     * have been reached; the thresholds waited on are far past it. */
    horizon_gpu_fence f;
    res = horizon_gpu_submit(chan, NULL, 0, HORIZON_GPU_SUBMIT_DEFAULT, &f);
    if (!t_check(t, horizon_gpu_succeeded(res), "submit (status=%s)",
                 horizon_gpu_status_str(res.status)))
        return 1;
    res = horizon_gpu_fence_wait(dev, f, UINT64_C(2000000000));
    t_check(t, horizon_gpu_succeeded(res), "the submitted fence retired");

    uint32_t base = 0;
    res = horizon_gpu_syncpt_read(dev, f.syncpt_id, &base);
    if (!t_check(t, horizon_gpu_succeeded(res),
                 "syncpt_read for the baseline (status=%s nv=0x%08x)",
                 horizon_gpu_status_str(res.status), res.nv))
        return 1;
    t_note(t, "syncpoint %u at %u; waiting on +%u and up", f.syncpt_id,
           base, (unsigned)FUTURE_GAP);

    /* Part 1: the ceiling, underneath our layer. */
    static const uint32_t ramp[] = { 1, 2, 4, 8, 12, 16, 20, 24, 32 };
    uint32_t highest_clean = 0;

    for (size_t i = 0; i < sizeof(ramp) / sizeof(ramp[0]); i++) {
        uint32_t n = ramp[i];
        uint64_t batch_ns = 0;
        uint32_t clean = fan_out(t, dev, f.syncpt_id, base, n, false,
                                 &batch_ns);
        t_note(t, "raw   N=%2u: %2u/%2u timed out in %5" PRIu64 " ms",
               n, clean, n, batch_ns / 1000000);
        if (clean == n)
            highest_clean = n;
    }

    t_note(t, "MEASURED: %u concurrent nvFenceWait calls were all armed; "
           "this is the number the layer above has to live within",
           highest_clean);
    t_check(t, highest_clean >= 1,
            "at least one wait can be armed (highest all-clean N = %u)",
            highest_clean);

    /* Part 2: the consequence, through our layer.
     *
     * Both rows must answer TIMEOUT — that is the correctness claim.
     * The wall time is the diagnosis: a batch that took about one
     * WAIT_NS ran its waits concurrently, and a batch that took
     * appreciably longer serialised them. Neither outcome fails the
     * test; an answer other than TIMEOUT does. */
    const uint32_t probe_n[2] = {
        highest_clean ? highest_clean : 1u,
        MAX_THREADS,
    };

    for (size_t i = 0; i < 2; i++) {
        uint32_t n = probe_n[i];
        if (i == 1 && n == probe_n[0])
            continue;
        uint64_t batch_ns = 0;
        uint32_t clean = fan_out(t, dev, f.syncpt_id, base, n, true,
                                 &batch_ns);
        t_note(t, "layer N=%2u: %2u/%2u TIMEOUT in %5" PRIu64 " ms "
               "(one wait is %" PRIu64 " ms)", n, clean, n,
               batch_ns / 1000000, WAIT_NS / 1000000);
        t_check(t, clean == n,
                "N=%u: every wait through the layer returned TIMEOUT", n);
    }

    t_check(t, horizon_gpu_succeeded(horizon_gpu_channel_destroy(chan)),
            "channel destroy");
    res = horizon_gpu_device_destroy(dev);
    t_check(t, horizon_gpu_succeeded(res), "device_destroy (status=%s)",
            horizon_gpu_status_str(res.status));
    return 0;
}
