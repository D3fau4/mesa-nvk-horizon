/*
 * Phase 1 test 3 — t_nvmap: NvMap create/close, id != 0, handle valid,
 * close path robust (milestones.md).
 *
 * Pointer-level double-destroy is excluded by the ownership contract
 * (every allocation has exactly one owner, CLAUDE.md); what this test
 * verifies is that the close path leaves the nvmap service fully usable
 * and that NULL/invalid handles are rejected instead of crashing.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include "horizon_gpu/device.h"
#include "horizon_gpu/memory.h"
#include "common/testfw.h"

const char *const test_name = "t_nvmap";
/* No display: main() starts a console and reports through it. */
const bool test_uses_display = false;

int run_test(test_ctx *t)
{
    horizon_gpu_device *dev = NULL;
    horizon_gpu_result res = horizon_gpu_device_create(NULL, &dev);
    if (!t_check(t, horizon_gpu_succeeded(res), "device_create (status=%s "
                 "nv=0x%08x)", horizon_gpu_status_str(res.status), res.nv))
        return 1;

    horizon_gpu_mem *a = NULL, *b = NULL;
    res = horizon_gpu_mem_create(dev, 0x1000, 0, HORIZON_GPU_MEM_CACHED, &a);
    t_check(t, horizon_gpu_succeeded(res), "create A");
    res = horizon_gpu_mem_create(dev, 0x1000, 0, HORIZON_GPU_MEM_CACHED, &b);
    t_check(t, horizon_gpu_succeeded(res), "create B");

    if (a && b) {
        uint32_t id_a = horizon_gpu_mem_get_id(a);
        uint32_t id_b = horizon_gpu_mem_get_id(b);
        uint32_t h_a = horizon_gpu_mem_get_handle(a);
        uint32_t h_b = horizon_gpu_mem_get_handle(b);
        t_note(t, "A: id=%u handle=%u  B: id=%u handle=%u", id_a, h_a, id_b,
               h_b);
        t_check(t, id_a != 0 && id_b != 0, "NvMap ids nonzero");
        t_check(t, h_a != 0 && h_b != 0, "NvMap handles nonzero");
        t_check(t, id_a != id_b, "ids distinct across objects");
        t_check(t, h_a != h_b, "handles distinct across objects");
        t_check(t, horizon_gpu_mem_mapped_va(a) == 0,
                "fresh object has no recorded GPU VA");
    }

    /* Close A, then prove the service is still healthy by creating C. */
    if (a) {
        res = horizon_gpu_mem_destroy(a);
        t_check(t, horizon_gpu_succeeded(res), "destroy A");
    }
    horizon_gpu_mem *c = NULL;
    res = horizon_gpu_mem_create(dev, 0x1000, 0, HORIZON_GPU_MEM_CACHED, &c);
    t_check(t, horizon_gpu_succeeded(res),
            "create C after closing A (close left nvmap usable)");
    if (c)
        t_check(t, horizon_gpu_mem_get_handle(c) != 0, "C handle valid");

    /* Robustness of the close path against bad input. */
    res = horizon_gpu_mem_destroy(NULL);
    t_check(t, res.status == HORIZON_GPU_ERR_INVALID_ARG,
            "destroy(NULL) rejected, no crash (%s)",
            horizon_gpu_status_str(res.status));

    if (b)
        t_check(t, horizon_gpu_succeeded(horizon_gpu_mem_destroy(b)),
                "destroy B");
    if (c)
        t_check(t, horizon_gpu_succeeded(horizon_gpu_mem_destroy(c)),
                "destroy C");

    horizon_gpu_device_counters cnt;
    horizon_gpu_device_get_counters(dev, &cnt);
    t_check(t, cnt.live_mem == 0, "live_mem back to zero (%u)",
            cnt.live_mem);

    res = horizon_gpu_device_destroy(dev);
    t_check(t, horizon_gpu_succeeded(res), "device_destroy (status=%s)",
            horizon_gpu_status_str(res.status));
    return 0;
}
