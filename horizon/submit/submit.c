/*
 * horizon_gpu — asynchronous GPFIFO submission.
 *
 * The increment discipline: telling libnx the expected
 * fence value grew (nvGpuChannelIncrFence) and emitting the actual
 * in-stream syncpoint-increment command list are ONE indivisible
 * operation here — requesting without emitting stalls the channel
 * forever, emitting without requesting double-counts.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <switch.h>

#include "horizon_gpu/submit.h"
#include "../channel/channel_priv.h"
#include "../device/device_priv.h"
#include "../sync/syncpt_math.h"

/* Debug-synchronous mode bound: generous but finite. */
#define SUBMIT_DEBUG_SYNC_TIMEOUT_NS UINT64_C(2000000000)

horizon_gpu_result horizon_gpu_submit(horizon_gpu_channel *chan,
                                      const horizon_gpu_cmd_span *spans,
                                      uint32_t num_spans,
                                      horizon_gpu_submit_flags flags,
                                      horizon_gpu_fence *out_fence)
{
    if (!chan || (num_spans > 0 && !spans))
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);
    if (flags != HORIZON_GPU_SUBMIT_DEFAULT &&
        flags != HORIZON_GPU_SUBMIT_ENTRY_FLAGS_ZERO)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);
    /* Bound num_spans against the queue capacity before it is used for
     * anything, so the back-pressure arithmetic further down cannot
     * wrap silently instead of refusing the submit.
     *
     * This used to also claim an unbounded count "would let the
     * validation loop below read spans[] out of the caller's array".
     * It would not: the loop reads exactly the num_spans entries the
     * caller declared, and a caller who lies about that has already
     * lost. The wrap is the real reason and it is sufficient. A guard
     * with a wrong stated rationale invites the next person to weaken
     * it — found in review of PR #7.
     *
     * SUBTRACTION, NOT ADDITION, and that is the whole point. This
     * check was written as `num_spans + 2 > GPFIFO_QUEUE_SIZE`, which
     * wraps: num_spans is uint32_t, so UINT32_MAX + 2 is 1, the guard
     * lets it through, and the loop below walks four billion entries of
     * the caller's array. The check written to stop an overflow
     * overflowed. Found in review of PR #7.
     *
     * GPFIFO_QUEUE_SIZE is libnx's (0x800), so the subtraction cannot
     * underflow; the assertion says so rather than trusting it. */
    _Static_assert(GPFIFO_QUEUE_SIZE >= 2,
                   "the two entries every submit adds must fit the queue");
    if (num_spans > GPFIFO_QUEUE_SIZE - 2)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);
    for (uint32_t i = 0; i < num_spans; i++) {
        if (spans[i].gpu_va == 0 || spans[i].num_dwords == 0)
            return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);
    }

    if (chan->lost)
        return horizon_gpu_err(HORIZON_GPU_ERR_CHANNEL_LOST);

    horizon_gpu_device *dev = chan->dev;

    /* Cheap reap first: one syncpoint read. */
    horizon_gpu_result res = horizon_gpu_channel_reap(chan, NULL);
    if (horizon_gpu_failed(res))
        return res;

    /* Known-quantity back-pressure instead of retry loops: refuse when
     * the entry queue cannot take this submit; the caller may wait on an
     * older fence and retry.
     *
     * TWO entries beyond the caller's spans, not one: the L2-invalidate
     * prologue before the work and the fence block after it. Counting
     * only the fence would let a submit be accepted that then fails to
     * append, and the partial-append unwind below would have to undo an
     * entry the arithmetic said would fit. */
    const uint32_t own_entries = 2;
    /* Safe to add here, and only because of the bound above: num_spans
     * is at most GPFIFO_QUEUE_SIZE - 2 and num_entries at most
     * GPFIFO_QUEUE_SIZE, so the sum is at most 2*0x800 + 2. Written
     * down because the same sum without that bound is what wrapped. */
    if (chan->gc.num_entries + num_spans + own_entries > GPFIFO_QUEUE_SIZE) {
        horizon_logf(&dev->log, HORIZON_LOG_WARN,
                     "channel %p: GPFIFO entry queue full (%u queued, %u "
                     "requested)", (void *)chan, chan->gc.num_entries,
                     num_spans + own_entries);
        return horizon_gpu_err(HORIZON_GPU_ERR_BUSY);
    }

    /* R3 stays measurable: default is the combination the reference
     * proved on hardware; ZERO submits NVK's upstream default so test 7
     * can compare. */
    u32 entry_flags = (flags == HORIZON_GPU_SUBMIT_ENTRY_FLAGS_ZERO)
                          ? 0
                          : (GPFIFO_ENTRY_NOT_MAIN |
                             GPFIFO_ENTRY_NO_PREFETCH);

    u32 entries_before = chan->gc.num_entries;
    Result rc;

    /* The prologue goes first, before any of the caller's work: one
     * L2_SYSMEM_INVALIDATE, so the GPU cannot read an L2 line that the
     * CPU has overwritten since the GPU last touched it. The fence block
     * appended after the work is the other direction — dirty lines back
     * to memory — and until this existed only that half was done.
     * horizon_gpu_channel_create carries the hardware measurement.
     *
     * Appended even when num_spans is zero: a submit with no work still
     * signals a fence, and a caller may be using that fence to conclude
     * that everything it wrote before is visible to the GPU.
     */
    rc = nvGpuChannelAppendEntry(&chan->gc, chan->prologue_cmds_va,
                                 chan->prologue_cmds_dwords, entry_flags, 0);
    if (R_FAILED(rc)) {
        chan->gc.num_entries = entries_before;
        horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                     "AppendEntry(L2 invalidate prologue) failed: 0x%08x", rc);
        return horizon_gpu_err_nv(rc);
    }

    for (uint32_t i = 0; i < num_spans; i++) {
        rc = nvGpuChannelAppendEntry(&chan->gc, spans[i].gpu_va,
                                     spans[i].num_dwords, entry_flags, 0);
        if (R_FAILED(rc)) {
            chan->gc.num_entries = entries_before; /* drop partial append */
            horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                         "AppendEntry(va=0x%llx n=%u) failed: 0x%08x",
                         (unsigned long long)spans[i].gpu_va,
                         spans[i].num_dwords, rc);
            return horizon_gpu_err_nv(rc);
        }
    }

    /* --- indivisible: request the increment AND emit its command --- */
    nvGpuChannelIncrFence(&chan->gc);
    rc = nvGpuChannelAppendEntry(&chan->gc, chan->fence_cmds_va,
                                 chan->fence_cmds_dwords, entry_flags, 0);
    if (R_FAILED(rc)) {
        chan->gc.fence_incr--; /* undo the request: keep the two in step */
        chan->gc.num_entries = entries_before;
        horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                     "AppendEntry(fence cmdlist) failed: 0x%08x", rc);
        return horizon_gpu_err_nv(rc);
    }
    /* --------------------------------------------------------------- */

    rc = nvGpuChannelKickoff(&chan->gc);
    if (R_FAILED(rc)) {
        /* Surface the rejection; never retried blindly, never slept on
         * (the reference retries 400 times holding its global lock —
         * drm_shim.c:666-676). Whether a distinct "ring full" kickoff
         * code exists is measured on hardware by t_submit. */
        chan->gc.fence_incr--;
        chan->gc.num_entries = entries_before;
        uint32_t err_type = 0;
        const char *desc = "unavailable";
        (void)horizon_gpu_channel_get_error(chan, &err_type, &desc);
        if (err_type != 0)
            chan->lost = true;
        horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                     "Kickoff failed: 0x%08x (notifier: %u '%s')", rc,
                     err_type, desc);
        return horizon_gpu_err_nv(rc);
    }

    chan->shadow_target += 1;

    horizon_gpu_fence fence = {
        .syncpt_id = chan->syncpt_id,
        .threshold = (uint32_t)chan->shadow_target,
    };

    /* libnx tracks the absolute fence value the kernel returned; our
     * shadow must agree with its low word. A mismatch means the kernel
     * incremented differently than requested, which invalidates every
     * future threshold derived from the shadow (R5) — the work already
     * reached hardware (kickoff succeeded), but this and every later
     * fence from this channel can no longer be trusted, so the channel
     * is marked lost rather than returning a fence callers would wait on
     * incorrectly. */
    if (chan->gc.fence.value != fence.threshold) {
        horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                     "shadow (%u) and kernel fence (%u) disagree on "
                     "syncpt %u — marking channel lost", fence.threshold,
                     chan->gc.fence.value, chan->syncpt_id);
        chan->lost = true;
        return horizon_gpu_err(HORIZON_GPU_ERR_CHANNEL_LOST);
    }

    horizon_logf(&dev->log, HORIZON_LOG_DEBUG,
                 "channel %p: submitted %u span(s)+incr, fence=%u:%u",
                 (void *)chan, num_spans, fence.syncpt_id, fence.threshold);

    /* Diagnostic mode only — compiled in, never taken otherwise, and no
     * test may need it to pass. */
    if (dev->debug_synchronous) {
        horizon_gpu_result wres =
            horizon_gpu_channel_wait_fence(chan, fence,
                                           SUBMIT_DEBUG_SYNC_TIMEOUT_NS);
        uint32_t err_type = 0;
        const char *desc = "none";
        (void)horizon_gpu_channel_get_error(chan, &err_type, &desc);
        horizon_logf(&dev->log, HORIZON_LOG_INFO,
                     "[sync-mode] fence %u:%u wait=%s notifier=%u '%s'",
                     fence.syncpt_id, fence.threshold,
                     horizon_gpu_status_str(wres.status), err_type, desc);
    }

    if (out_fence)
        *out_fence = fence;
    return horizon_gpu_ok();
}

/* Finds a slot in the channel's wait ring that the GPU is provably done
 * with, or reports BUSY.
 *
 * "Provably" is one syncpoint read, not a guess: a slot records the
 * fence of the submit that used it, and the GPFIFO is in order, so once
 * the counter has passed that fence the list in the slot has been
 * fetched and executed. A slot that has never been used is free without
 * any read at all, which is the whole of a channel's first frames.
 *
 * The scan starts at wait_slot_next and wraps, so slots are consumed in
 * the order they were freed rather than always retrying the same one.
 */
static horizon_gpu_result
horizon_wait_ring_take(horizon_gpu_channel *chan, uint32_t *out_slot)
{
    uint32_t hw = 0;
    bool have_hw = false;

    for (uint32_t i = 0; i < HORIZON_CHANNEL_WAIT_SLOTS; i++) {
        const uint32_t slot =
            (chan->wait_slot_next + i) % HORIZON_CHANNEL_WAIT_SLOTS;

        if (!chan->wait_slots[slot].busy) {
            chan->wait_slot_next =
                (slot + 1u) % HORIZON_CHANNEL_WAIT_SLOTS;
            *out_slot = slot;
            return horizon_gpu_ok();
        }

        /* Read the counter at most once per call, and only when a busy
         * slot is actually in the way. */
        if (!have_hw) {
            horizon_gpu_result res = horizon_channel_read_syncpt(chan, &hw);
            if (horizon_gpu_failed(res))
                return res;
            have_hw = true;
        }

        if (horizon_gpu_syncpt_reached(hw,
                                       chan->wait_slots[slot].fence.threshold)) {
            chan->wait_slots[slot].busy = false;
            chan->wait_slot_next =
                (slot + 1u) % HORIZON_CHANNEL_WAIT_SLOTS;
            *out_slot = slot;
            return horizon_gpu_ok();
        }
    }

    return horizon_gpu_err(HORIZON_GPU_ERR_BUSY);
}

horizon_gpu_result horizon_gpu_submit_waits(horizon_gpu_channel *chan,
                                            const horizon_gpu_fence *fences,
                                            uint32_t num_fences,
                                            horizon_gpu_fence *out_fence)
{
    if (!chan || !fences || num_fences == 0 ||
        num_fences > HORIZON_GPU_MAX_WAIT_FENCES)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);

    if (chan->lost)
        return horizon_gpu_err(HORIZON_GPU_ERR_CHANNEL_LOST);

    uint32_t slot = 0;
    horizon_gpu_result res = horizon_wait_ring_take(chan, &slot);
    if (horizon_gpu_failed(res))
        return res;

    const uint64_t slot_offset =
        HORIZON_CHANNEL_WAIT_CMDS_OFFSET +
        (uint64_t)slot * HORIZON_CHANNEL_WAIT_SLOT_DWORDS * 4;

    uint32_t *base = horizon_gpu_mem_cpu_ptr(chan->cmdbuf_mem);
    if (base == NULL)
        return horizon_gpu_err(HORIZON_GPU_ERR_STATE);
    uint32_t *cmds = base + slot_offset / 4;

    uint32_t n = 0;
    for (uint32_t i = 0; i < num_fences; i++) {
        /* A syncpoint id the encoding cannot hold would be silently
         * masked into a *different, valid* id and waited on, which is a
         * hang on a counter nobody is incrementing. Refuse instead, and
         * leave the slot free: nothing has been submitted yet. */
        const uint32_t emitted =
            horizon_cmds_syncpt_wait(cmds + n, fences[i].syncpt_id,
                                     fences[i].threshold);
        if (emitted == 0)
            return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);
        n += emitted;
    }

    /* Before the submit, never after it — patch 0082's lesson, and it is
     * the same hardware property: the command page is mapped CPU-cached,
     * the host engine does not snoop, and a kickoff starts the fetch
     * immediately. */
    res = horizon_gpu_mem_flush(chan->cmdbuf_mem, slot_offset, n * 4);
    if (horizon_gpu_failed(res))
        return res;

    const horizon_gpu_cmd_span span = {
        .gpu_va = chan->wait_cmds_va +
                  (uint64_t)slot * HORIZON_CHANNEL_WAIT_SLOT_DWORDS * 4,
        .num_dwords = n,
    };

    horizon_gpu_fence fence;
    res = horizon_gpu_submit(chan, &span, 1, HORIZON_GPU_SUBMIT_DEFAULT,
                             &fence);
    if (horizon_gpu_failed(res))
        return res;

    /* Claimed only now. A slot marked busy before a submit that was
     * refused would never come back: nothing would ever reach its
     * fence. */
    chan->wait_slots[slot].fence = fence;
    chan->wait_slots[slot].busy = true;

    if (out_fence)
        *out_fence = fence;
    return horizon_gpu_ok();
}
