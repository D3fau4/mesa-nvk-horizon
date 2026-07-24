/*
 * Phase 1 test 4 — t_va_reserve: VA reserve/release, exhaustion behaviour,
 * alignment (milestones.md). Also measures R8's open question about
 * practical reservation sizes by probing a large reservation and noting
 * the outcome.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <inttypes.h>

#include "horizon_gpu/device.h"
#include "horizon_gpu/memory.h"
#include "horizon_gpu/vm.h"
#include "common/testfw.h"

const char *const test_name = "t_va_reserve";

int run_test(test_ctx *t)
{
    horizon_gpu_device *dev = NULL;
    horizon_gpu_result res = horizon_gpu_device_create(NULL, &dev);
    if (!t_check(t, horizon_gpu_succeeded(res), "device_create (status=%s "
                 "nv=0x%08x)", horizon_gpu_status_str(res.status), res.nv))
        return 1;

    horizon_gpu_device_info info;
    horizon_gpu_device_get_info(dev, &info);

    /* Small-page reservation. */
    horizon_gpu_va_range *r1 = NULL;
    res = horizon_gpu_vm_reserve(dev, 0x100000, 0x1000, 0, &r1);
    t_check(t, horizon_gpu_succeeded(res), "reserve 1 MiB small-page "
            "(status=%s nv=0x%08x)", horizon_gpu_status_str(res.status),
            res.nv);
    if (r1) {
        uint64_t base = horizon_gpu_va_range_base(r1);
        t_note(t, "small reservation base=0x%" PRIx64, base);
        t_check(t, base != 0, "base nonzero");
        t_check(t, (base & 0xFFF) == 0, "base page-aligned");
        t_check(t, horizon_gpu_va_range_size(r1) == 0x100000,
                "size as requested");
    }

    /* Alignment honoured. */
    horizon_gpu_va_range *r2 = NULL;
    res = horizon_gpu_vm_reserve(dev, 0x10000, 0x1000, 0x100000, &r2);
    t_check(t, horizon_gpu_succeeded(res), "reserve with 1 MiB alignment");
    if (r2) {
        uint64_t base = horizon_gpu_va_range_base(r2);
        t_check(t, (base & 0xFFFFF) == 0,
                "base honours 1 MiB alignment (0x%" PRIx64 ")", base);
    }

    /* Big-page reservation lands in the big-page half. */
    horizon_gpu_va_range *r3 = NULL;
    res = horizon_gpu_vm_reserve(dev, (uint64_t)info.big_page_size * 4,
                                 info.big_page_size, 0, &r3);
    t_check(t, horizon_gpu_succeeded(res), "reserve big-page range "
            "(page=0x%x status=%s nv=0x%08x)", info.big_page_size,
            horizon_gpu_status_str(res.status), res.nv);
    if (r3) {
        uint64_t base = horizon_gpu_va_range_base(r3);
        uint64_t big_lo = info.va_regions[1].base;
        uint64_t big_hi = big_lo + info.va_regions[1].pages *
                          (uint64_t)info.va_regions[1].page_size;
        t_note(t, "big reservation base=0x%" PRIx64 " (big region [0x%"
               PRIx64 ", 0x%" PRIx64 "))", base, big_lo, big_hi);
        t_check(t, base >= big_lo && base < big_hi,
                "base inside the queried big-page region");
    }

    /* Rejections. */
    res = horizon_gpu_vm_reserve(dev, 0x1000, 0x2000, 0, &r1);
    t_check(t, res.status == HORIZON_GPU_ERR_INVALID_ARG,
            "page size neither small nor big rejected (%s)",
            horizon_gpu_status_str(res.status));
    res = horizon_gpu_vm_reserve(dev, 0x1000, 0x1000, 0x1800, &r1);
    t_check(t, res.status == HORIZON_GPU_ERR_INVALID_ARG,
            "non-pow2 alignment rejected (%s)",
            horizon_gpu_status_str(res.status));

    /* Exhaustion: ask for more pages than the small-page region has.
     * The kernel must refuse; what matters is a clean surfaced error,
     * not a hang or a bogus success (R8 open question — record it). */
    uint64_t region_bytes = info.va_regions[0].pages * UINT64_C(0x1000);
    horizon_gpu_va_range *huge = NULL;
    res = horizon_gpu_vm_reserve(dev, region_bytes + UINT64_C(0x100000000),
                                 0x1000, 0, &huge);
    t_note(t, "oversized reservation (region+4GiB): status=%s nv=0x%08x",
           horizon_gpu_status_str(res.status), res.nv);
    t_check(t, horizon_gpu_failed(res),
            "oversized reservation surfaced an error, not success");
    if (horizon_gpu_succeeded(res))
        horizon_gpu_vm_release(huge);

    /* Release everything; releases must succeed and counters return to 0. */
    if (r3)
        t_check(t, horizon_gpu_succeeded(horizon_gpu_vm_release(r3)),
                "release big-page range");
    if (r2)
        t_check(t, horizon_gpu_succeeded(horizon_gpu_vm_release(r2)),
                "release aligned range");
    if (r1)
        t_check(t, horizon_gpu_succeeded(horizon_gpu_vm_release(r1)),
                "release small range");

    horizon_gpu_device_counters c;
    horizon_gpu_device_get_counters(dev, &c);
    t_check(t, c.live_va_ranges == 0, "live_va_ranges back to zero (%u)",
            c.live_va_ranges);

    res = horizon_gpu_device_destroy(dev);
    t_check(t, horizon_gpu_succeeded(res), "device_destroy (status=%s)",
            horizon_gpu_status_str(res.status));
    return 0;
}
