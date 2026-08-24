/*
 * How many syncpoint waits this platform will hold at once, and what
 * happens to horizon_gpu_fence_wait when that number is exceeded.
 *
 * WHY THE QUESTION MATTERS HERE. horizon_gpu_fence_wait (sync/syncpt.c)
 * is a loop: read the counter, and if the fence is not reached, block in
 * nvFenceWait for a 100 ms chunk before reading again. A chunk that
 * expires is the loop's pulse. A chunk that comes back without having
 * blocked is not, and treating it as one turns a bounded wait into a
 * hot loop of two ioctls for the caller's whole deadline — answering
 * correctly the entire time, burning a core, with nothing above able to
 * see it.
 *
 * THE FIRST RUN OF THIS TEST FOUND THAT, AND NOT WHERE IT LOOKED. The
 * probe was a threshold FUTURE_GAP past the counter, on the assumption
 * that a fence nothing will ever reach is a fence you can wait on.
 * Measured 2026-08-24:
 *
 *   note raw   N= 1:  0/ 1 timed out in     0 ms
 *   note N=1: 0/1 timed out; first divergent thread 0 returned Result
 *        0x00000000 (module 0, desc 0)
 *
 * nvFenceWait answers *success*, immediately, for a threshold past the
 * syncpoint's maximum — nvhost calls such a threshold expired rather
 * than block forever on an increment nothing will make. So that probe
 * never armed a wait, at any N, and the ceiling it was meant to find
 * could not be reached with it. It did reach the spin, at N=1, which is
 * what sync/syncpt.c and channel/channel.c were fixed for.
 *
 * SO THE FENCE HAS TO BE ONE THE GPU WILL REACH AND HAS NOT. That needs
 * the GPU held still, which this platform can do and t_submit already
 * proves it does (its R10 section): a channel whose command list opens
 * with a GPU-side wait on *another* channel's syncpoint stops there
 * until that other channel is submitted to. Every fence queued behind
 * that wait is then pending, in the syncpoint's live window, and a wait
 * on it arms. The release is one submit on the producer and this test
 * always makes it.
 *
 * WHAT THIS MEASURES, IN THREE PARTS.
 *
 *   0. What a threshold past the maximum answers, and how fast. One
 *      call. It is the reason the loops below no longer discard what a
 *      chunk returned, and it belongs in the log next to that.
 *
 *   1. The ceiling, probed underneath our own layer. N threads call
 *      nvFenceWait directly, each on its own pending fence of the
 *      stalled channel, and report their Result. A clean run is every
 *      thread blocking for the whole timeout and then saying so — see
 *      raw_timed_out() for which Result that is here, which is not the
 *      one this test first assumed. The first N where something else
 *      appears is the ceiling, and the Result is named in the log.
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
#include "horizon_gpu/cmds.h"
#include "horizon_gpu/device.h"
#include "horizon_gpu/memory.h"
#include "horizon_gpu/submit.h"
#include "horizon_gpu/sync.h"
#include "horizon_gpu/vm.h"
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

/* A wait that returns in less than this did not block, whatever it
 * returned. Two ioctls and a thread start are microseconds; a chunk that
 * was armed and expired is WAIT_US. Anything in between is not
 * something this platform has produced. */
#define ARMED_FLOOR_NS UINT64_C(50000000)

/* Part 0 only: distance past the counter for the "past the maximum"
 * probe. Well inside the 2^31 window horizon_gpu_syncpt_reached is
 * valid over, and far outside anything the channel has reserved. */
#define FUTURE_GAP UINT32_C(100000)

#define CMD_BYTES  UINT32_C(0x1000)

/* What this platform returns for a wait that was armed and expired.
 *
 * MEASURED 2026-08-24: 0x00000d5c — module 348, description 6, which
 * switch/result.h names Module_LibnxNvidia and LibnxNvidiaError_Timeout.
 * nvFenceWait reports the nv service's timeout, not the kernel's, and an
 * earlier version of this test called every armed wait divergent for it.
 *
 * Spelled out here rather than taken from horizon/'s own helper: this
 * arm measures the platform underneath that layer, and borrowing the
 * layer's opinion of a Result would stop it being an independent
 * reading. KERNELRESULT(TimedOut) stays accepted — libnx routes some
 * waits through svcWaitSynchronization. */
static bool raw_timed_out(Result rc)
{
    return rc == KERNELRESULT(TimedOut) ||
           rc == MAKERESULT(Module_LibnxNvidia, LibnxNvidiaError_Timeout);
}

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

/* Runs `n` waiters, one per pending fence, and reports. Returns the
 * number that came back the way a healthy platform answers a fence the
 * GPU has not reached: blocked for the whole timeout and said so. */
static uint32_t fan_out(test_ctx *t, horizon_gpu_device *dev,
                        const horizon_gpu_fence *fences, uint32_t n,
                        bool through_layer, uint64_t *out_batch_ns,
                        uint32_t *out_unarmed)
{
    static waiter w[MAX_THREADS];
    static pthread_t th[MAX_THREADS];

    memset(w, 0, sizeof(w));
    for (uint32_t i = 0; i < n; i++) {
        w[i].dev = dev;
        /* A distinct fence per thread: one registration each, which is
         * what a queue with several fences in flight produces. */
        w[i].fence = fences[i];
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
    uint32_t unarmed = 0;
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
            ok = raw_timed_out(w[i].nv_rc);
            if (!ok && first_other == 0) {
                first_other = w[i].nv_rc;
                first_other_at = i;
            }
        }
        /* Returning the right answer without having blocked is the
         * failure mode this test exists to catch, and it is invisible
         * in the Result alone. Counted separately so a row that spun
         * cannot be read as a row that waited. */
        if (w[i].elapsed_ns < ARMED_FLOOR_NS)
            unarmed++;
        if (ok)
            clean++;
    }

    *out_unarmed = unarmed;
    if (unarmed != 0) {
        t_note(t, "N=%u: %u/%u threads returned without blocking (under "
               "%llu ms); those waits were not armed", n, unarmed, started,
               (unsigned long long)(ARMED_FLOOR_NS / 1000000));
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

    /* ---- part 0: what a threshold past the maximum answers ---------
     *
     * On its own channel, so the value read and the maximum reserved
     * belong to each other and nothing else is in flight on them. */
    horizon_gpu_channel *probe_chan = NULL;
    res = horizon_gpu_channel_create(dev, NULL, &probe_chan);
    if (!t_check(t, horizon_gpu_succeeded(res),
                 "probe channel_create (status=%s nv=0x%08x)",
                 horizon_gpu_status_str(res.status), res.nv))
        return 1;

    horizon_gpu_fence pf;
    res = horizon_gpu_submit(probe_chan, NULL, 0, HORIZON_GPU_SUBMIT_DEFAULT,
                             &pf);
    t_check(t, horizon_gpu_succeeded(res), "probe priming submit (status=%s)",
            horizon_gpu_status_str(res.status));
    res = horizon_gpu_fence_wait(dev, pf, UINT64_C(2000000000));
    t_check(t, horizon_gpu_succeeded(res), "the probe fence retired");

    uint32_t probe_base = 0;
    res = horizon_gpu_syncpt_read(dev, pf.syncpt_id, &probe_base);
    if (t_check(t, horizon_gpu_succeeded(res),
                "syncpt_read for the past-the-maximum probe (status=%s "
                "nv=0x%08x)", horizon_gpu_status_str(res.status), res.nv)) {
        NvFence far = { .id = pf.syncpt_id,
                        .value = probe_base + FUTURE_GAP };
        uint64_t t0 = armGetSystemTick();
        Result rc = nvFenceWait(&far, WAIT_US);
        uint64_t took = armTicksToNs(armGetSystemTick() - t0);
        t_note(t, "MEASURED: syncpoint %u is at %u; nvFenceWait on %u — "
               "%u past it and past everything reserved — returned "
               "0x%08x (module %u, desc %u) in %llu us, asked for %d us",
               pf.syncpt_id, probe_base, probe_base + FUTURE_GAP,
               (unsigned)FUTURE_GAP, (unsigned)rc,
               (unsigned)R_MODULE(rc), (unsigned)R_DESCRIPTION(rc),
               (unsigned long long)(took / 1000), WAIT_US);
        t_note(t, "MEASURED: a wait on such a threshold %s, which is why "
               "neither wait loop treats a chunk that did not block as "
               "its pulse",
               took < ARMED_FLOOR_NS ? "does NOT block" : "blocks");
    }
    t_check(t, horizon_gpu_succeeded(horizon_gpu_channel_destroy(probe_chan)),
            "probe channel destroy");

    /* ---- the stall: a fence the GPU will reach and has not ---------- */
    horizon_gpu_mem *cmd_mem = NULL;
    horizon_gpu_va_range *cmd_range = NULL;
    horizon_gpu_mapping *cmd_map = NULL;
    horizon_gpu_channel *prod = NULL;
    horizon_gpu_channel *cons = NULL;
    int rv = 0;

    res = horizon_gpu_mem_create(dev, CMD_BYTES, 0, HORIZON_GPU_MEM_CACHED,
                                 &cmd_mem);
    if (!t_check(t, horizon_gpu_succeeded(res), "wait cmdlist allocated"))
        goto out_dev;
    res = horizon_gpu_vm_reserve(dev, CMD_BYTES, HORIZON_GPU_SMALL_PAGE_SIZE,
                                 0, &cmd_range);
    if (!t_check(t, horizon_gpu_succeeded(res), "wait cmdlist VA reserved"))
        goto out_mem;
    res = horizon_gpu_vm_map(cmd_range, 0, cmd_mem, 0, CMD_BYTES,
                             HORIZON_GPU_PTE_KIND_PITCH, false, &cmd_map);
    if (!t_check(t, horizon_gpu_succeeded(res), "wait cmdlist mapped"))
        goto out_range;

    res = horizon_gpu_channel_create(dev, NULL, &prod);
    if (!t_check(t, horizon_gpu_succeeded(res), "producer channel created"))
        goto out_map;
    res = horizon_gpu_channel_create(dev, NULL, &cons);
    if (!t_check(t, horizon_gpu_succeeded(res), "consumer channel created"))
        goto out_prod;

    horizon_gpu_fence prod_f;
    res = horizon_gpu_submit(prod, NULL, 0, HORIZON_GPU_SUBMIT_DEFAULT,
                             &prod_f);
    if (!t_check(t, horizon_gpu_succeeded(res), "producer priming submit"))
        goto out_cons;
    res = horizon_gpu_fence_wait(dev, prod_f, UINT64_C(2000000000));
    t_check(t, horizon_gpu_succeeded(res), "the producer's own fence retired");

    uint32_t prod_now = 0;
    res = horizon_gpu_syncpt_read(dev, prod_f.syncpt_id, &prod_now);
    if (!t_check(t, horizon_gpu_succeeded(res), "producer syncpt read"))
        goto out_cons;

    horizon_gpu_fence cons_f;
    res = horizon_gpu_submit(cons, NULL, 0, HORIZON_GPU_SUBMIT_DEFAULT,
                             &cons_f);
    if (!t_check(t, horizon_gpu_succeeded(res), "consumer priming submit"))
        goto out_cons;
    res = horizon_gpu_fence_wait(dev, cons_f, UINT64_C(2000000000));
    t_check(t, horizon_gpu_succeeded(res), "the consumer's priming fence "
            "retired, before anything stalls it");

    /* The two channels have to be counting different syncpoints, or the
     * consumer would be waiting on its own increments and nothing could
     * release it. Measured 2026-08-24 (t_submit): they are, 26 and 27.
     * Checked rather than assumed, because the whole stall below is
     * unreleasable if it is ever not true. */
    if (!t_check(t, prod_f.syncpt_id != cons_f.syncpt_id,
                 "the producer and the consumer count different "
                 "syncpoints (%u and %u)", prod_f.syncpt_id,
                 cons_f.syncpt_id))
        goto out_cons;

    const uint32_t awaited = prod_now + 1;
    uint32_t *cpu = horizon_gpu_mem_cpu_ptr(cmd_mem);
    uint32_t n_dw = horizon_cmds_syncpt_wait(cpu, prod_f.syncpt_id, awaited);
    if (!t_check(t, n_dw == HORIZON_CMDS_SYNCPT_WAIT_DWORDS,
                 "GPU-side wait on syncpoint %u threshold %u encoded "
                 "(%u dwords)", prod_f.syncpt_id, awaited, n_dw))
        goto out_cons;
    res = horizon_gpu_mem_flush(cmd_mem, 0, n_dw * 4);
    if (!t_check(t, horizon_gpu_succeeded(res), "wait cmdlist flushed"))
        goto out_cons;

    const horizon_gpu_cmd_span span = {
        .gpu_va = horizon_gpu_mapping_va(cmd_map), .num_dwords = n_dw };
    horizon_gpu_fence stall_f;
    res = horizon_gpu_submit(cons, &span, 1, HORIZON_GPU_SUBMIT_DEFAULT,
                             &stall_f);
    if (!t_check(t, horizon_gpu_succeeded(res),
                 "the consumer is submitted and now stopped at the wait "
                 "(status=%s nv=0x%08x)", horizon_gpu_status_str(res.status),
                 res.nv))
        goto out_cons;

    /* MAX_THREADS fences queued behind the stall: distinct thresholds,
     * every one of them inside the syncpoint's reserved window and none
     * of them reachable until the producer is submitted to. This is the
     * instrument the first version of this test did not have. */
    static horizon_gpu_fence pending[MAX_THREADS];
    uint32_t n_pending = 0;
    for (uint32_t i = 0; i < MAX_THREADS; i++) {
        res = horizon_gpu_submit(cons, NULL, 0, HORIZON_GPU_SUBMIT_DEFAULT,
                                 &pending[i]);
        if (horizon_gpu_failed(res))
            break;
        n_pending++;
    }
    t_note(t, "queued %u pending fences behind the stall, thresholds %u..%u "
           "on syncpoint %u", n_pending,
           n_pending ? pending[0].threshold : 0,
           n_pending ? pending[n_pending - 1].threshold : 0,
           stall_f.syncpt_id);
    if (!t_check(t, n_pending >= 1,
                 "at least one fence could be queued behind the stall"))
        goto out_release;

    /* ---- part 1: the ceiling, underneath our layer ------------------ */
    static const uint32_t ramp[] = { 1, 2, 4, 8, 12, 16, 20, 24, 32 };
    uint32_t highest_clean = 0;

    for (size_t i = 0; i < sizeof(ramp) / sizeof(ramp[0]); i++) {
        uint32_t n = ramp[i];
        if (n > n_pending)
            break;
        uint64_t batch_ns = 0;
        uint32_t unarmed = 0;
        uint32_t clean = fan_out(t, dev, pending, n, false, &batch_ns,
                                 &unarmed);
        t_note(t, "raw   N=%2u: %2u/%2u armed and timed out in %5" PRIu64
               " ms", n, clean, n, batch_ns / 1000000);
        if (clean == n && unarmed == 0)
            highest_clean = n;
    }

    t_note(t, "MEASURED: %u concurrent nvFenceWait calls were all armed; "
           "this is the number the layer above has to live within",
           highest_clean);
    t_check(t, highest_clean >= 1,
            "at least one wait can be armed (highest all-clean N = %u)",
            highest_clean);

    /* ---- part 2: the consequence, through our layer -----------------
     *
     * Both rows must answer TIMEOUT — that is the correctness claim.
     * The wall time is the diagnosis: a batch that took about one
     * WAIT_NS ran its waits concurrently, and a batch that took
     * appreciably longer serialised them. Neither outcome fails the
     * test; an answer other than TIMEOUT does. */
    const uint32_t probe_n[2] = {
        highest_clean ? highest_clean : 1u,
        n_pending < MAX_THREADS ? n_pending : MAX_THREADS,
    };

    for (size_t i = 0; i < 2; i++) {
        uint32_t n = probe_n[i];
        if (n == 0 || n > n_pending)
            continue;
        if (i == 1 && n == probe_n[0])
            continue;
        uint64_t batch_ns = 0;
        uint32_t unarmed = 0;
        uint32_t clean = fan_out(t, dev, pending, n, true, &batch_ns,
                                 &unarmed);
        t_note(t, "layer N=%2u: %2u/%2u TIMEOUT in %5" PRIu64 " ms "
               "(one wait is %" PRIu64 " ms)", n, clean, n,
               batch_ns / 1000000, WAIT_NS / 1000000);
        t_check(t, clean == n,
                "N=%u: every wait through the layer returned TIMEOUT", n);
    }

out_release:
    /* ALWAYS. The consumer is stopped inside the GPU on a threshold only
     * this submit produces, and every fence queued behind it is stopped
     * with it. Skipping this would leave a channel that no teardown path
     * can drain. */
    {
        horizon_gpu_fence rel;
        horizon_gpu_result r =
            horizon_gpu_submit(prod, NULL, 0, HORIZON_GPU_SUBMIT_DEFAULT,
                               &rel);
        t_check(t, horizon_gpu_succeeded(r),
                "the producer is submitted, releasing the consumer "
                "(status=%s)", horizon_gpu_status_str(r.status));
        r = horizon_gpu_channel_wait_fence(cons, stall_f,
                                           UINT64_C(3000000000));
        t_check(t, horizon_gpu_succeeded(r),
                "the consumer's stalled submit retired once released "
                "(status=%s)", horizon_gpu_status_str(r.status));
        r = horizon_gpu_channel_wait_idle(cons, UINT64_C(3000000000));
        t_check(t, horizon_gpu_succeeded(r),
                "the consumer drained everything queued behind it "
                "(status=%s)", horizon_gpu_status_str(r.status));
    }

out_cons:
    t_check(t, horizon_gpu_succeeded(horizon_gpu_channel_destroy(cons)),
            "consumer channel destroy");
out_prod:
    t_check(t, horizon_gpu_succeeded(horizon_gpu_channel_destroy(prod)),
            "producer channel destroy");
out_map:
    horizon_gpu_vm_unmap(cmd_map);
out_range:
    horizon_gpu_vm_release(cmd_range);
out_mem:
    horizon_gpu_mem_destroy(cmd_mem);
out_dev:
    res = horizon_gpu_device_destroy(dev);
    t_check(t, horizon_gpu_succeeded(res), "device_destroy (status=%s)",
            horizon_gpu_status_str(res.status));
    return rv;
}
