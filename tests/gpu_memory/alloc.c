/*
 * Phase 1 test 2 — t_alloc: aligned allocation, size/alignment overflow
 * rejection (milestones.md).
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>

#include <switch.h>

#include "horizon_gpu/device.h"
#include "horizon_gpu/memory.h"
#include "common/testfw.h"

TEST_CASE_DECL(gpu_memory, alloc)
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

    /* --- what the zero fill costs, and what skipping it buys ---------
     *
     * horizon_gpu_mem_create fills every byte with zero and then flushes
     * the range out of the CPU cache. horizon_gpu_mem_create_uninit
     * skips only the fill — the flush stays, because it is there for
     * the dirty lines the *previous* tenant of that heap left behind
     * and not for the fill's own (see horizon/memory/mem.c).
     *
     * SO THE SAVING IS BIGGER THAN THE STORES. A line the fill never
     * dirtied has nothing to write back, so `dc civac` over the range
     * costs less as well. Whether that adds up to a frame spike worth
     * chasing is what these numbers are for: nothing in this tree uses
     * the uninitialised path yet, and the audit in mem.c says the one
     * consumer that would show a difference needs an NVKMD_MEM_* bit
     * through nvkmd before it can be routed. This is the measurement
     * that decides whether that is worth writing.
     *
     * Each size is allocated and destroyed several times: the first
     * allocation of a size has the heap growing underneath it, and that
     * is a different measurement from the steady state a driver is in.
     */
    {
        static const uint64_t sizes[] = {
            UINT64_C(0x10000),    /*  64 KiB */
            UINT64_C(0x100000),   /*   1 MiB */
            UINT64_C(0x800000),   /*   8 MiB */
        };
        static const uint32_t reps = 8;

        for (size_t si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
            const uint64_t bytes = sizes[si];
            uint64_t zero_ticks = 0, uninit_ticks = 0;
            bool ok = true;

            for (uint32_t r = 0; r < reps && ok; r++) {
                horizon_gpu_mem *m = NULL;
                uint64_t t0 = armGetSystemTick();
                horizon_gpu_result zres =
                    horizon_gpu_mem_create(dev, bytes, 0,
                                           HORIZON_GPU_MEM_CACHED, &m);
                zero_ticks += armGetSystemTick() - t0;
                if (!horizon_gpu_succeeded(zres)) {
                    ok = t_check(t, false, "create(0x%llx) failed (%s)",
                                 (unsigned long long)bytes,
                                 horizon_gpu_status_str(zres.status));
                    break;
                }
                horizon_gpu_mem_destroy(m);
                m = NULL;

                t0 = armGetSystemTick();
                horizon_gpu_result ures =
                    horizon_gpu_mem_create_uninit(dev, bytes, 0,
                                                  HORIZON_GPU_MEM_CACHED,
                                                  &m);
                uninit_ticks += armGetSystemTick() - t0;
                if (!horizon_gpu_succeeded(ures)) {
                    ok = t_check(t, false,
                                 "create_uninit(0x%llx) failed (%s)",
                                 (unsigned long long)bytes,
                                 horizon_gpu_status_str(ures.status));
                    break;
                }

                /* The uninitialised object is a real object: same size
                 * rounding, same alignment, and writable and readable
                 * through its own map. What is NOT checked is its
                 * initial content — there is nothing to check, which is
                 * the whole contract. */
                if (r == 0) {
                    t_check(t, horizon_gpu_mem_size(m) == bytes,
                            "uninit size rounds the same way (0x%llx)",
                            (unsigned long long)horizon_gpu_mem_size(m));
                    uintptr_t pp = (uintptr_t)horizon_gpu_mem_cpu_ptr(m);
                    t_check(t, pp != 0 && (pp & 0xFFF) == 0,
                            "uninit cpu ptr page-aligned (%p)", (void *)pp);
                    if (pp != 0) {
                        volatile uint32_t *w = (volatile uint32_t *)pp;
                        w[0] = UINT32_C(0xa5a5a5a5);
                        w[bytes / 4u - 1u] = UINT32_C(0x5a5a5a5a);
                        t_check(t, w[0] == UINT32_C(0xa5a5a5a5) &&
                                   w[bytes / 4u - 1u] == UINT32_C(0x5a5a5a5a),
                                "uninit storage holds what is written to it");
                    }
                }
                horizon_gpu_mem_destroy(m);
            }

            if (!ok)
                continue;

            const uint64_t freq = armGetSystemTickFreq();
            const uint64_t zero_us = freq ? (zero_ticks * UINT64_C(1000000))
                                            / freq / reps : 0;
            const uint64_t uninit_us = freq ? (uninit_ticks *
                                               UINT64_C(1000000)) / freq / reps
                                            : 0;
            t_note(t, "allocation of 0x%llx bytes: %llu us zero-filled, "
                      "%llu us uninitialised (mean of %u, cached policy)",
                   (unsigned long long)bytes, (unsigned long long)zero_us,
                   (unsigned long long)uninit_us, reps);
        }
    }

    horizon_gpu_device_counters c;
    horizon_gpu_device_get_counters(dev, &c);
    t_check(t, c.live_mem == 0, "no leaked memory objects (%u)", c.live_mem);

    res = horizon_gpu_device_destroy(dev);
    t_check(t, horizon_gpu_succeeded(res), "device_destroy (status=%s)",
            horizon_gpu_status_str(res.status));
    return 0;
}
