/*
 * Phase 1 test 2 — t_alloc: aligned allocation, size/alignment overflow
 * rejection (milestones.md).
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>

#include "horizon_gpu/device.h"
#include "horizon_gpu/memory.h"
#include "common/testfw.h"

const char *const test_name = "t_alloc";
/* No display: main() starts a console and reports through it. */
const bool test_uses_display = false;

int run_test(test_ctx *t)
{
    horizon_gpu_device *dev = NULL;
    horizon_gpu_result res = horizon_gpu_device_create(NULL, &dev);
    if (!t_check(t, horizon_gpu_succeeded(res), "device_create (status=%s "
                 "nv=0x%08x)", horizon_gpu_status_str(res.status), res.nv))
        return 1;

    /* 1-byte request rounds up to one small page, pointer page-aligned. */
    horizon_gpu_mem *mem = NULL;
    res = horizon_gpu_mem_create(dev, 1, 0, HORIZON_GPU_MEM_CACHED, &mem);
    t_check(t, horizon_gpu_succeeded(res), "create(1 byte) (status=%s)",
            horizon_gpu_status_str(res.status));
    if (mem) {
        t_check(t, horizon_gpu_mem_size(mem) == 0x1000,
                "size rounded to one page (0x%llx)",
                (unsigned long long)horizon_gpu_mem_size(mem));
        uintptr_t p = (uintptr_t)horizon_gpu_mem_cpu_ptr(mem);
        t_check(t, p != 0 && (p & 0xFFF) == 0, "cpu ptr page-aligned (%p)",
                (void *)p);
        res = horizon_gpu_mem_destroy(mem);
        t_check(t, horizon_gpu_succeeded(res), "destroy");
        mem = NULL;
    }

    /* Big alignment honoured. */
    res = horizon_gpu_mem_create(dev, 0x11000, 0x10000,
                                 HORIZON_GPU_MEM_CACHED, &mem);
    t_check(t, horizon_gpu_succeeded(res), "create(0x11000, align 0x10000)");
    if (mem) {
        uintptr_t p = (uintptr_t)horizon_gpu_mem_cpu_ptr(mem);
        t_check(t, (p & 0xFFFF) == 0, "cpu ptr 64 KiB-aligned (%p)",
                (void *)p);
        t_check(t, horizon_gpu_mem_size(mem) == 0x20000,
                "size rounded to alignment (0x%llx)",
                (unsigned long long)horizon_gpu_mem_size(mem));
        res = horizon_gpu_mem_destroy(mem);
        t_check(t, horizon_gpu_succeeded(res), "destroy");
        mem = NULL;
    }

    /* Rejections — each must fail cleanly with the right status. */
    res = horizon_gpu_mem_create(dev, 0, 0, HORIZON_GPU_MEM_CACHED, &mem);
    t_check(t, res.status == HORIZON_GPU_ERR_INVALID_ARG,
            "size 0 rejected (%s)", horizon_gpu_status_str(res.status));

    res = horizon_gpu_mem_create(dev, 0x1000, 0x3000,
                                 HORIZON_GPU_MEM_CACHED, &mem);
    t_check(t, res.status == HORIZON_GPU_ERR_INVALID_ARG,
            "non-pow2 alignment rejected (%s)",
            horizon_gpu_status_str(res.status));

    res = horizon_gpu_mem_create(dev, 0x1000, 0x800,
                                 HORIZON_GPU_MEM_CACHED, &mem);
    t_check(t, res.status == HORIZON_GPU_ERR_INVALID_ARG,
            "sub-page alignment rejected (%s)",
            horizon_gpu_status_str(res.status));

    res = horizon_gpu_mem_create(dev, UINT64_MAX - 5, 0x1000,
                                 HORIZON_GPU_MEM_CACHED, &mem);
    t_check(t, res.status == HORIZON_GPU_ERR_OVERFLOW,
            "size near UINT64_MAX rejected as overflow (%s)",
            horizon_gpu_status_str(res.status));

    res = horizon_gpu_mem_create(dev, (uint64_t)UINT32_MAX + 1, 0x1000,
                                 HORIZON_GPU_MEM_CACHED, &mem);
    t_check(t, res.status == HORIZON_GPU_ERR_OVERFLOW,
            "size beyond the 32-bit nvmap limit rejected (%s)",
            horizon_gpu_status_str(res.status));

    /* Cache maintenance range checking. */
    res = horizon_gpu_mem_create(dev, 0x2000, 0, HORIZON_GPU_MEM_CACHED,
                                 &mem);
    t_check(t, horizon_gpu_succeeded(res), "create(0x2000)");
    if (mem) {
        res = horizon_gpu_mem_flush(mem, 0x1000, 0x1000);
        t_check(t, horizon_gpu_succeeded(res), "flush in-range ok");
        res = horizon_gpu_mem_flush(mem, 0x1800, 0x1000);
        t_check(t, res.status == HORIZON_GPU_ERR_OVERFLOW,
                "flush past the end rejected (%s)",
                horizon_gpu_status_str(res.status));
        res = horizon_gpu_mem_invalidate(mem, UINT64_MAX, 2);
        t_check(t, res.status == HORIZON_GPU_ERR_OVERFLOW,
                "invalidate with wrapping range rejected (%s)",
                horizon_gpu_status_str(res.status));
        res = horizon_gpu_mem_destroy(mem);
        t_check(t, horizon_gpu_succeeded(res), "destroy");
    }

    horizon_gpu_device_counters c;
    horizon_gpu_device_get_counters(dev, &c);
    t_check(t, c.live_mem == 0, "no leaked memory objects (%u)", c.live_mem);

    res = horizon_gpu_device_destroy(dev);
    t_check(t, horizon_gpu_succeeded(res), "device_destroy (status=%s)",
            horizon_gpu_status_str(res.status));
    return 0;
}
