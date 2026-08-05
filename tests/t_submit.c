/*
 * Phase 1 test 7 — t_submit: a minimal command list executes (no-op +
 * fence increment), multiple submits stay in flight with no CPU wait
 * between them, and the R3 GPFIFO entry-flag question plus the R10
 * GPU-side syncpoint-wait encoding are measured on our own code
 * (milestones.md, known-risks.md).
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <inttypes.h>

#include <switch.h>

#include "horizon_gpu/channel.h"
#include "horizon_gpu/cmds.h"
#include "horizon_gpu/device.h"
#include "horizon_gpu/memory.h"
#include "horizon_gpu/submit.h"
#include "horizon_gpu/sync.h"
#include "horizon_gpu/vm.h"
#include "common/testfw.h"

const char *const test_name = "t_submit";
/* No display: main() starts a console and reports through it. */
const bool test_uses_display = false;

#define WAIT_NS UINT64_C(2000000000) /* 2 s bound for every wait here */
#define NOP_PAIRS 64u

/* Builds a NOP command list in fresh memory mapped at a fresh VA. */
static horizon_gpu_result make_nop_list(horizon_gpu_device *dev,
                                        horizon_gpu_mem **out_mem,
                                        horizon_gpu_va_range **out_range,
                                        horizon_gpu_mapping **out_map,
                                        horizon_gpu_cmd_span *out_span)
{
    horizon_gpu_result res = horizon_gpu_mem_create(dev, 0x1000, 0,
                                                    HORIZON_GPU_MEM_CACHED,
                                                    out_mem);
    if (horizon_gpu_failed(res))
        return res;
    res = horizon_gpu_vm_reserve(dev, 0x10000, 0x1000, 0, out_range);
    if (horizon_gpu_failed(res))
        return res;
    res = horizon_gpu_vm_map(*out_range, 0, *out_mem, 0, 0x1000,
                             HORIZON_GPU_PTE_KIND_PITCH, true, out_map);
    if (horizon_gpu_failed(res))
        return res;

    uint32_t *cmds = horizon_gpu_mem_cpu_ptr(*out_mem);
    uint32_t n = horizon_cmds_nop(cmds, 0x1000 / 4, NOP_PAIRS);
    res = horizon_gpu_mem_flush(*out_mem, 0, n * 4);
    if (horizon_gpu_failed(res))
        return res;

    out_span->gpu_va = horizon_gpu_mapping_va(*out_map);
    out_span->num_dwords = n;
    return horizon_gpu_ok();
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

    /* 1. Fence-only submit: kickoff + the increment command list alone.
     * This is the indivisible-increment discipline (R4) executing for
     * real: if the emitted SYNCPOINTB encoding were wrong, this wait
     * would time out. */
    horizon_gpu_fence f0;
    res = horizon_gpu_submit(chan, NULL, 0, HORIZON_GPU_SUBMIT_DEFAULT,
                             &f0);
    t_check(t, horizon_gpu_succeeded(res), "fence-only submit (status=%s "
            "nv=0x%08x)", horizon_gpu_status_str(res.status), res.nv);
    res = horizon_gpu_channel_wait_fence(chan, f0, WAIT_NS);
    t_check(t, horizon_gpu_succeeded(res),
            "fence-only submit completed (status=%s)",
            horizon_gpu_status_str(res.status));

    /* 1b. A span count that used to wrap the guard meant to stop it.
     *
     * The bound was `num_spans + 2 > GPFIFO_QUEUE_SIZE` on a uint32_t:
     * UINT32_MAX + 2 is 1, the guard let it through, and the validation
     * loop then walked four billion entries of an array with one in it.
     * The check written to stop an overflow overflowed. Found in review
     * of PR #7 and now subtraction-form.
     *
     * A real address is passed on purpose. Rejecting NULL would be a
     * different guard doing the work, and the whole question is whether
     * the *count* is refused before anything indexes with it — on the
     * old code this call did not return. */
    {
        const horizon_gpu_cmd_span dummy = { .gpu_va = 0x1000, .num_dwords = 1 };
        res = horizon_gpu_submit(chan, &dummy, UINT32_MAX,
                                 HORIZON_GPU_SUBMIT_DEFAULT, NULL);
        t_check(t, res.status == HORIZON_GPU_ERR_INVALID_ARG,
                "a span count of UINT32_MAX is refused, not wrapped "
                "(status=%s)", horizon_gpu_status_str(res.status));

        /* And the boundary either side of it, so the bound is the right
         * one and not merely some bound. */
        res = horizon_gpu_submit(chan, &dummy, GPFIFO_QUEUE_SIZE - 1,
                                 HORIZON_GPU_SUBMIT_DEFAULT, NULL);
        t_check(t, res.status == HORIZON_GPU_ERR_INVALID_ARG,
                "one span more than the queue can hold with its own two "
                "entries is refused (status=%s)",
                horizon_gpu_status_str(res.status));
    }

    /* 2. NOP command list from caller memory. */
    horizon_gpu_mem *mem = NULL;
    horizon_gpu_va_range *range = NULL;
    horizon_gpu_mapping *map = NULL;
    horizon_gpu_cmd_span span;
    res = make_nop_list(dev, &mem, &range, &map, &span);
    bool nop_ok = t_check(t, horizon_gpu_succeeded(res),
                          "NOP list built and mapped (status=%s nv=0x%08x)",
                          horizon_gpu_status_str(res.status), res.nv);
    if (nop_ok) {
        horizon_gpu_fence f1;
        res = horizon_gpu_submit(chan, &span, 1,
                                 HORIZON_GPU_SUBMIT_DEFAULT, &f1);
        t_check(t, horizon_gpu_succeeded(res), "NOP submit accepted");
        res = horizon_gpu_channel_wait_fence(chan, f1, WAIT_NS);
        t_check(t, horizon_gpu_succeeded(res), "NOP submit completed "
                "(status=%s)", horizon_gpu_status_str(res.status));
    }

    /* 3. Multiple submits in flight, no CPU wait between them (the
     * milestone's exit criterion; the reference drains after every
     * submit). Both horizon_gpu_submit calls return before any wait;
     * we then wait once, on the LAST fence only. Depends on `span` from
     * step 2, so it is skipped rather than run on an uninitialised span
     * if that setup failed. */
    if (!nop_ok) {
        t_note(t, "skipping multi-submit-in-flight check: NOP list from "
               "step 2 was never built");
    } else {
        horizon_gpu_fence fa, fb;
        uint64_t tick0 = armGetSystemTick();
        res = horizon_gpu_submit(chan, &span, 1, HORIZON_GPU_SUBMIT_DEFAULT,
                                 &fa);
        horizon_gpu_result res2 = horizon_gpu_submit(chan, &span, 1,
                                                     HORIZON_GPU_SUBMIT_DEFAULT,
                                                     &fb);
        uint64_t submit_ns = armTicksToNs(armGetSystemTick() - tick0);
        t_check(t, horizon_gpu_succeeded(res) && horizon_gpu_succeeded(res2),
                "two back-to-back submits accepted without waiting");
        t_check(t, fb.threshold == fa.threshold + 1,
                "fences ordered by submission (%u then %u)", fa.threshold,
                fb.threshold);
        /* The milestone's exit criterion, actually asserted: a regression
         * that reintroduced a drain-per-submit wait (CLAUDE.md rejected
         * design #6) must fail this, not just print a different number in
         * a note nobody diffs. 50 ms is generous — hardware measured
         * ~148 us for both submits together (STATUS.md) — while being far
         * below any realistic full CPU-wait-drain regression. */
        t_check(t, submit_ns < UINT64_C(50000000),
                "both submits issued in %" PRIu64 " us, well under a "
                "drain-per-submit regression threshold (no intervening CPU "
                "wait)", submit_ns / 1000);
        res = horizon_gpu_channel_wait_fence(chan, fb, WAIT_NS);
        t_check(t, horizon_gpu_succeeded(res), "second fence reached");
        bool first_done = false;
        res = horizon_gpu_fence_poll(dev, fa, &first_done);
        t_check(t, horizon_gpu_succeeded(res) && first_done,
                "first fence implied by the second (single-channel "
                "ordering)");
    }

    /* 4. Engine binds execute in-stream (R7): SET_OBJECT on subch 0-4. */
    horizon_gpu_fence fbind;
    res = horizon_gpu_channel_bind_engines(chan, &fbind);
    t_check(t, horizon_gpu_succeeded(res), "bind_engines submit (status=%s "
            "nv=0x%08x)", horizon_gpu_status_str(res.status), res.nv);
    res = horizon_gpu_channel_wait_fence(chan, fbind, WAIT_NS);
    t_check(t, horizon_gpu_succeeded(res),
            "SET_OBJECT list completed without fault (status=%s)",
            horizon_gpu_status_str(res.status));
    uint32_t err_type = 0;
    const char *desc = NULL;
    res = horizon_gpu_channel_get_error(chan, &err_type, &desc);
    t_check(t, horizon_gpu_succeeded(res) && err_type == 0,
            "no error notification after engine binds (status=%s type=%u "
            "'%s')", horizon_gpu_status_str(res.status), err_type,
            desc ? desc : "?");

    /* 5. R3 measurement, on a dedicated channel so a wedge cannot poison
     * the main one: NVK's upstream default entry flags (0) vs the
     * NOT_MAIN|NO_PREFETCH combination used above. Every outcome is
     * recorded; the requirement is a clean surfaced result, not success. */
    horizon_gpu_channel *r3chan = NULL;
    res = horizon_gpu_channel_create(dev, NULL, &r3chan);
    t_check(t, horizon_gpu_succeeded(res), "R3 experiment channel created");
    if (r3chan) {
        horizon_gpu_fence f3;
        res = horizon_gpu_submit(r3chan, NULL, 0,
                                 HORIZON_GPU_SUBMIT_ENTRY_FLAGS_ZERO, &f3);
        if (horizon_gpu_succeeded(res)) {
            res = horizon_gpu_channel_wait_fence(r3chan, f3, WAIT_NS);
            t_note(t, "R3: entry flags 0 -> submit accepted, wait=%s",
                   horizon_gpu_status_str(res.status));
        } else {
            t_note(t, "R3: entry flags 0 -> submit rejected cleanly "
                   "(status=%s nv=0x%08x)",
                   horizon_gpu_status_str(res.status), res.nv);
        }
        res = horizon_gpu_channel_destroy(r3chan);
        t_note(t, "R3 channel destroy: %s",
               horizon_gpu_status_str(res.status));
    }

    /* 6. R10 measurement: GPU-side syncpoint wait encoding, exercised as
     * an actual cross-channel dependency. A wait against the *consumer's
     * own* already-reached counter (the original version of this test)
     * proves nothing: the fence it waits on is satisfied whether or not
     * the encoded wait is honoured, ignored, or has the wrong threshold
     * semantics — so instead this waits on a *producer* channel's
     * counter at a threshold the producer has not yet reached. The
     * consumer's own completion fence cannot signal until its whole
     * command list — including the embedded cross-channel wait — has
     * finished executing, so:
     *   1. a short wait on the consumer's fence must time out while the
     *      producer is still behind (proves the wait actually blocks);
     *   2. incrementing the producer past the threshold must then let
     *      the same fence be reached (proves it unblocks correctly). */
    #define R10_SHORT_WAIT_NS UINT64_C(300000000) /* 300 ms: several poll
                                                    * chunks, well under
                                                    * WAIT_NS */
    horizon_gpu_channel *r10prod = NULL, *r10cons = NULL;
    res = horizon_gpu_channel_create(dev, NULL, &r10prod);
    t_check(t, horizon_gpu_succeeded(res), "R10 producer channel created");
    res = horizon_gpu_channel_create(dev, NULL, &r10cons);
    t_check(t, horizon_gpu_succeeded(res), "R10 consumer channel created");
    if (r10prod && r10cons) {
        horizon_gpu_mem *wmem = NULL;
        horizon_gpu_va_range *wrange = NULL;
        horizon_gpu_mapping *wmap = NULL;
        horizon_gpu_cmd_span wspan;
        res = make_nop_list(dev, &wmem, &wrange, &wmap, &wspan);
        if (t_check(t, horizon_gpu_succeeded(res),
                    "R10 wait cmdlist built and mapped")) {
            uint32_t prod_id = horizon_gpu_channel_syncpt_id(r10prod);
            uint32_t cur = 0;
            res = horizon_gpu_syncpt_read(dev, prod_id, &cur);
            uint32_t future = cur + 1; /* producer has not reached this */

            uint32_t *cmds = horizon_gpu_mem_cpu_ptr(wmem);
            uint32_t n = horizon_cmds_syncpt_wait(cmds, prod_id, future);
            horizon_gpu_result flush_res = horizon_gpu_mem_flush(wmem, 0,
                                                                  n * 4);
            wspan.num_dwords = n;

            horizon_gpu_fence fw;
            if (t_check(t, horizon_gpu_succeeded(res) &&
                        horizon_gpu_succeeded(flush_res),
                        "R10 producer syncpt read and wait cmdlist flush "
                        "(read status=%s, flush status=%s)",
                        horizon_gpu_status_str(res.status),
                        horizon_gpu_status_str(flush_res.status))) {
                res = horizon_gpu_submit(r10cons, &wspan, 1,
                                         HORIZON_GPU_SUBMIT_DEFAULT, &fw);
                if (t_check(t, horizon_gpu_succeeded(res),
                            "R10 cross-channel wait submit accepted")) {
                    res = horizon_gpu_channel_wait_fence(r10cons, fw,
                                                         R10_SHORT_WAIT_NS);
                    t_check(t, res.status == HORIZON_GPU_ERR_TIMEOUT,
                            "R10: consumer stays blocked on producer syncpt "
                            "%u threshold %u not yet reached (status=%s)",
                            prod_id, future,
                            horizon_gpu_status_str(res.status));

                    horizon_gpu_fence pf;
                    res = horizon_gpu_submit(r10prod, NULL, 0,
                                             HORIZON_GPU_SUBMIT_DEFAULT, &pf);
                    t_check(t, horizon_gpu_succeeded(res) &&
                            horizon_gpu_syncpt_reached(pf.threshold, future),
                            "R10: producer submit reaches the awaited "
                            "threshold (producer now %u, awaited %u)",
                            pf.threshold, future);

                    res = horizon_gpu_channel_wait_fence(r10cons, fw,
                                                         WAIT_NS);
                    t_check(t, horizon_gpu_succeeded(res),
                            "R10: GPU-side cross-channel wait unblocks once "
                            "the producer reaches the threshold (status=%s)",
                            horizon_gpu_status_str(res.status));
                }
            }
        }
        if (wmap)
            horizon_gpu_vm_unmap(wmap);
        if (wrange)
            horizon_gpu_vm_release(wrange);
        if (wmem)
            horizon_gpu_mem_destroy(wmem);
    }
    if (r10cons) {
        res = horizon_gpu_channel_wait_idle(r10cons, WAIT_NS);
        t_note(t, "R10 consumer wait_idle: %s",
               horizon_gpu_status_str(res.status));
        res = horizon_gpu_channel_destroy(r10cons);
        t_note(t, "R10 consumer channel destroy: %s",
               horizon_gpu_status_str(res.status));
    }
    if (r10prod) {
        res = horizon_gpu_channel_wait_idle(r10prod, WAIT_NS);
        t_note(t, "R10 producer wait_idle: %s",
               horizon_gpu_status_str(res.status));
        res = horizon_gpu_channel_destroy(r10prod);
        t_note(t, "R10 producer channel destroy: %s",
               horizon_gpu_status_str(res.status));
    }
    #undef R10_SHORT_WAIT_NS

    /* Teardown. */
    res = horizon_gpu_channel_wait_idle(chan, WAIT_NS);
    t_check(t, horizon_gpu_succeeded(res), "wait_idle before teardown");
    if (map)
        t_check(t, horizon_gpu_succeeded(horizon_gpu_vm_unmap(map)),
                "unmap NOP list");
    if (range)
        t_check(t, horizon_gpu_succeeded(horizon_gpu_vm_release(range)),
                "release NOP range");
    if (mem)
        t_check(t, horizon_gpu_succeeded(horizon_gpu_mem_destroy(mem)),
                "destroy NOP mem");
    t_check(t, horizon_gpu_succeeded(horizon_gpu_channel_destroy(chan)),
            "destroy main channel");

    res = horizon_gpu_device_destroy(dev);
    t_check(t, horizon_gpu_succeeded(res), "device_destroy (status=%s)",
            horizon_gpu_status_str(res.status));
    return 0;
}
