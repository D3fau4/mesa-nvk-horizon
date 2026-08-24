/*
 * Phase 1 test 6 — t_channel: channel create/destroy, syncpoint id
 * assigned, several channels coexisting, Zcull bind (milestones.md).
 * Records the syncpoint value observed at creation — the R5 open
 * question about shadow initialisation.
 *
 * AND HOW MANY CHANNELS A PROCESS MAY OPEN AT ONCE, which is a number
 * two deferred decisions are waiting on: has_transfer_queue is false
 * because this driver opens one channel, and patch 0022 records that
 * has_sparse = true would make NVK ask for a bind context on top. Both
 * were left off rather than promise something about a number nobody had
 * read; the ramp at the end of this file reads it.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
/* R_MODULE/R_DESCRIPTION, to name the Result that ends the ramp. */
#include <switch.h>

#include "horizon_gpu/channel.h"
#include "horizon_gpu/device.h"
#include "common/testfw.h"

const char *const test_name = "t_channel";
/* No display: main() starts a console and reports through it. */
const bool test_uses_display = false;

#define NUM_CHANNELS 4

/* Ceiling on the "how many at once" ramp below. Not a guess at the
 * answer — a bound on how much of the console's memory this test is
 * willing to hold while finding out, since every channel carries a
 * GPFIFO and a pushbuffer. If the ramp ever reaches it, the log says so
 * and the real number is "at least this". */
#define MAX_PROBE_CHANNELS 64

int run_test(test_ctx *t)
{
    horizon_gpu_device *dev = NULL;
    horizon_gpu_result res = horizon_gpu_device_create(NULL, &dev);
    if (!t_check(t, horizon_gpu_succeeded(res), "device_create (status=%s "
                 "nv=0x%08x)", horizon_gpu_status_str(res.status), res.nv))
        return 1;

    /* Plain channel. */
    horizon_gpu_channel *chan = NULL;
    res = horizon_gpu_channel_create(dev, NULL, &chan);
    if (!t_check(t, horizon_gpu_succeeded(res), "channel_create (status=%s "
                 "nv=0x%08x)", horizon_gpu_status_str(res.status), res.nv))
        goto out_dev;

    uint32_t id = horizon_gpu_channel_syncpt_id(chan);
    uint32_t initial = horizon_gpu_channel_syncpt_value_at_create(chan);
    t_note(t, "syncpt id=%u, value at create=%u (R5 measurement)", id,
           initial);
    t_check(t, id != 0 && id < 0x1000,
            "syncpoint id assigned and encodable (%u)", id);
    t_check(t, !horizon_gpu_channel_is_lost(chan), "fresh channel not lost");

    uint32_t err_type = 0xdead;
    const char *desc = NULL;
    res = horizon_gpu_channel_get_error(chan, &err_type, &desc);
    t_note(t, "fresh channel notifier: status=%s nv=0x%08x type=%u '%s'",
           horizon_gpu_status_str(res.status), res.nv, err_type,
           desc ? desc : "?");
    t_check(t, horizon_gpu_succeeded(res) && err_type == 0,
            "fresh channel reports no error notification");

    /* Channel with a Zcull context bound. */
    horizon_gpu_channel_create_info zinfo = {
        .prio = HORIZON_GPU_CHANNEL_PRIO_MEDIUM,
        .bind_zcull = true,
    };
    horizon_gpu_channel *zchan = NULL;
    res = horizon_gpu_channel_create(dev, &zinfo, &zchan);
    t_check(t, horizon_gpu_succeeded(res), "channel_create with Zcull "
            "(status=%s nv=0x%08x)", horizon_gpu_status_str(res.status),
            res.nv);
    if (zchan) {
        t_check(t, horizon_gpu_channel_syncpt_id(zchan) != id,
                "second channel got a distinct syncpoint (%u vs %u)",
                horizon_gpu_channel_syncpt_id(zchan), id);
        res = horizon_gpu_channel_destroy(zchan);
        t_check(t, horizon_gpu_succeeded(res), "destroy Zcull channel "
                "(status=%s)", horizon_gpu_status_str(res.status));
    }

    /* N channels coexisting, all with distinct syncpoints. */
    horizon_gpu_channel *many[NUM_CHANNELS] = {0};
    uint32_t ids[NUM_CHANNELS + 1];
    ids[0] = id;
    bool all_created = true, all_distinct = true;
    for (int i = 0; i < NUM_CHANNELS; i++) {
        res = horizon_gpu_channel_create(dev, NULL, &many[i]);
        if (horizon_gpu_failed(res)) {
            t_note(t, "channel %d creation failed: status=%s nv=0x%08x", i,
                   horizon_gpu_status_str(res.status), res.nv);
            all_created = false;
            break;
        }
        ids[i + 1] = horizon_gpu_channel_syncpt_id(many[i]);
        for (int j = 0; j <= i; j++)
            if (ids[j] == ids[i + 1])
                all_distinct = false;
    }
    t_check(t, all_created, "%d additional channels created", NUM_CHANNELS);
    t_check(t, all_distinct, "every channel has a distinct syncpoint id");
    for (int i = 0; i < NUM_CHANNELS; i++) {
        if (many[i])
            t_check(t,
                    horizon_gpu_succeeded(horizon_gpu_channel_destroy(many[i])),
                    "destroy extra channel %d", i);
    }

    /* ---- HOW MANY CHANNELS A PROCESS MAY OPEN --------------------
     *
     * Never measured before, and two decisions are waiting on it.
     * nvkmd_info::has_transfer_queue is false because this driver opens
     * one channel and a transfer queue would be a second; patch 0022
     * records that has_sparse = true makes NVK ask for a bind context,
     * which would be a third. Both were left off rather than promise
     * something about a number nobody had read. This reads it.
     *
     * Ramped one at a time and stopped at the first refusal, so the
     * answer is the count that succeeded and the reason is the Result
     * that did not. Everything is destroyed afterwards and the device's
     * own counters are checked back to zero, because a test that
     * measures a resource limit by exhausting it has to give it back. */
    static horizon_gpu_channel *probe[MAX_PROBE_CHANNELS];
    uint32_t opened = 0;
    horizon_gpu_result stop_res = horizon_gpu_ok();

    for (; opened < MAX_PROBE_CHANNELS; opened++) {
        stop_res = horizon_gpu_channel_create(dev, NULL, &probe[opened]);
        if (horizon_gpu_failed(stop_res)) {
            probe[opened] = NULL;
            break;
        }
    }

    /* `chan` is still open, so the process is holding opened + 1. */
    if (opened == MAX_PROBE_CHANNELS) {
        t_note(t, "MEASURED: %u channels open at once (this test's own "
               "ceiling, %u, was reached — the real limit is at least "
               "this and was not found)", opened + 1,
               (unsigned)MAX_PROBE_CHANNELS);
    } else {
        t_note(t, "MEASURED: %u channels can be open at once; the next "
               "channel_create failed with status=%s nv=0x%08x "
               "(module %u, desc %u)", opened + 1,
               horizon_gpu_status_str(stop_res.status), stop_res.nv,
               (unsigned)R_MODULE(stop_res.nv),
               (unsigned)R_DESCRIPTION(stop_res.nv));
    }

    /* Three is the number the two deferred decisions need between them:
     * the render channel, a transfer queue, and the bind context
     * has_sparse would make NVK ask for. */
    t_check(t, opened + 1 >= 3,
            "at least three channels can be open at once, which is what a "
            "transfer queue and a sparse bind context would cost together "
            "(%u)", opened + 1);

    for (uint32_t i = 0; i < opened; i++) {
        if (probe[i] && !t_check(t, horizon_gpu_succeeded(
                                 horizon_gpu_channel_destroy(probe[i])),
                                 "destroy probe channel %u", i))
            break;
    }

    res = horizon_gpu_channel_destroy(chan);
    t_check(t, horizon_gpu_succeeded(res), "destroy first channel "
            "(status=%s)", horizon_gpu_status_str(res.status));

    horizon_gpu_device_counters c;
    horizon_gpu_device_get_counters(dev, &c);
    t_check(t, c.live_channels == 0 && c.live_mem == 0 &&
            c.live_va_ranges == 0 && c.live_mappings == 0,
            "all counters zero after channel teardown (chan=%u mem=%u "
            "ranges=%u maps=%u)", c.live_channels, c.live_mem,
            c.live_va_ranges, c.live_mappings);

out_dev:
    res = horizon_gpu_device_destroy(dev);
    t_check(t, horizon_gpu_succeeded(res), "device_destroy (status=%s)",
            horizon_gpu_status_str(res.status));
    return 0;
}
