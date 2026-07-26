/*
 * Phase 1 test 6 — t_channel: channel create/destroy, syncpoint id
 * assigned, several channels coexisting, Zcull bind (milestones.md).
 * Records the syncpoint value observed at creation — the R5 open
 * question about shadow initialisation.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include "horizon_gpu/channel.h"
#include "horizon_gpu/device.h"
#include "common/testfw.h"

const char *const test_name = "t_channel";

#define NUM_CHANNELS 4

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
