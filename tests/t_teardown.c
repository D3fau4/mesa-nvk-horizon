/*
 * Phase 1 test 10 — t_teardown: a full stack (device, memory, VA,
 * mapping, channel, submits) torn down in reverse order with zero leaked
 * objects, twice in one process; destroying out of order is refused
 * (milestones.md; memory-model §§ 7-8).
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include "horizon_gpu/channel.h"
#include "horizon_gpu/cmds.h"
#include "horizon_gpu/device.h"
#include "horizon_gpu/memory.h"
#include "horizon_gpu/submit.h"
#include "horizon_gpu/vm.h"
#include "common/testfw.h"

const char *const test_name = "t_teardown";

#define WAIT_NS UINT64_C(2000000000)

static void count_retire(void *ctx)
{
    (*(int *)ctx)++;
}

static int full_cycle(test_ctx *t, int cycle)
{
    horizon_gpu_device *dev = NULL;
    horizon_gpu_result res = horizon_gpu_device_create(NULL, &dev);
    if (!t_check(t, horizon_gpu_succeeded(res),
                 "cycle %d: device_create (status=%s nv=0x%08x)", cycle,
                 horizon_gpu_status_str(res.status), res.nv))
        return 1;

    /* Build the whole object graph. */
    horizon_gpu_mem *mem = NULL;
    horizon_gpu_va_range *range = NULL;
    horizon_gpu_mapping *map = NULL;
    horizon_gpu_channel *chan = NULL;

    res = horizon_gpu_mem_create(dev, 0x1000, 0, HORIZON_GPU_MEM_CACHED,
                                 &mem);
    t_check(t, horizon_gpu_succeeded(res), "cycle %d: mem", cycle);
    res = horizon_gpu_vm_reserve(dev, 0x10000, 0x1000, 0, &range);
    t_check(t, horizon_gpu_succeeded(res), "cycle %d: range", cycle);
    if (mem && range) {
        uint32_t *cmds = horizon_gpu_mem_cpu_ptr(mem);
        uint32_t n = horizon_cmds_nop(cmds, 8);
        horizon_gpu_mem_flush(mem, 0, n * 4);
        res = horizon_gpu_vm_map(range, 0, mem, 0, 0x1000,
                                 HORIZON_GPU_PTE_KIND_PITCH, true, &map);
        t_check(t, horizon_gpu_succeeded(res), "cycle %d: map", cycle);
    }
    horizon_gpu_channel_create_info cinfo = {
        .prio = HORIZON_GPU_CHANNEL_PRIO_MEDIUM,
        .bind_zcull = true,
    };
    res = horizon_gpu_channel_create(dev, &cinfo, &chan);
    t_check(t, horizon_gpu_succeeded(res), "cycle %d: channel (+zcull)",
            cycle);

    /* Some real work plus a retirement callback that must fire. */
    int retired = 0;
    if (chan && map) {
        horizon_gpu_cmd_span span = {
            .gpu_va = horizon_gpu_mapping_va(map),
            .num_dwords = 16,
        };
        horizon_gpu_fence f1, f2;
        res = horizon_gpu_submit(chan, &span, 1,
                                 HORIZON_GPU_SUBMIT_DEFAULT, &f1);
        t_check(t, horizon_gpu_succeeded(res), "cycle %d: submit 1", cycle);
        horizon_gpu_channel_add_retirement(chan, f1, count_retire,
                                           &retired);
        res = horizon_gpu_submit(chan, &span, 1,
                                 HORIZON_GPU_SUBMIT_DEFAULT, &f2);
        t_check(t, horizon_gpu_succeeded(res), "cycle %d: submit 2", cycle);

        /* Destroying with work in flight must be refused... */
        res = horizon_gpu_channel_destroy(chan);
        if (res.status == HORIZON_GPU_ERR_BUSY)
            t_note(t, "cycle %d: channel destroy while in flight refused "
                   "(BUSY) as required", cycle);
        else
            t_note(t, "cycle %d: in-flight destroy returned %s (work may "
                   "already have retired)", cycle,
                   horizon_gpu_status_str(res.status));

        res = horizon_gpu_channel_wait_idle(chan, WAIT_NS);
        t_check(t, horizon_gpu_succeeded(res), "cycle %d: wait_idle "
                "(status=%s)", cycle, horizon_gpu_status_str(res.status));
        t_check(t, retired == 1,
                "cycle %d: retirement callback ran exactly once (%d)",
                cycle, retired);
    }

    /* Out-of-order teardown is refused, never silently leaked. */
    res = horizon_gpu_device_destroy(dev);
    t_check(t, res.status == HORIZON_GPU_ERR_LEAK,
            "cycle %d: device destroy with live children refused (%s)",
            cycle, horizon_gpu_status_str(res.status));

    /* Reverse-order teardown. */
    if (chan)
        t_check(t, horizon_gpu_succeeded(horizon_gpu_channel_destroy(chan)),
                "cycle %d: destroy channel", cycle);
    if (map)
        t_check(t, horizon_gpu_succeeded(horizon_gpu_vm_unmap(map)),
                "cycle %d: unmap", cycle);
    if (range)
        t_check(t, horizon_gpu_succeeded(horizon_gpu_vm_release(range)),
                "cycle %d: release range", cycle);
    if (mem)
        t_check(t, horizon_gpu_succeeded(horizon_gpu_mem_destroy(mem)),
                "cycle %d: destroy mem", cycle);

    horizon_gpu_device_counters c;
    horizon_gpu_device_get_counters(dev, &c);
    t_check(t, c.live_mem == 0 && c.live_va_ranges == 0 &&
            c.live_mappings == 0 && c.live_channels == 0,
            "cycle %d: every counter zero (mem=%u ranges=%u maps=%u "
            "chans=%u)", cycle, c.live_mem, c.live_va_ranges,
            c.live_mappings, c.live_channels);

    res = horizon_gpu_device_destroy(dev);
    t_check(t, horizon_gpu_succeeded(res),
            "cycle %d: device_destroy now succeeds (status=%s)", cycle,
            horizon_gpu_status_str(res.status));
    return 0;
}

int run_test(test_ctx *t)
{
    /* Twice in one process: nothing may survive the first cycle. */
    if (full_cycle(t, 1))
        return 1;
    return full_cycle(t, 2);
}
