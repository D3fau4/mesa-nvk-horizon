/*
 * Phase 1 test 1 — t_init: nv bring-up / teardown, GM20B fields sane,
 * repeatable twice in one process, all counters zero (milestones.md).
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "horizon_gpu/device.h"
#include "common/testfw.h"

const char *const test_name = "t_init";

static int one_cycle(test_ctx *t, int cycle)
{
    horizon_gpu_device *dev = NULL;
    horizon_gpu_result res = horizon_gpu_device_create(NULL, &dev);
    if (!t_check(t, horizon_gpu_succeeded(res),
                 "cycle %d: device_create (status=%s nv=0x%08x)", cycle,
                 horizon_gpu_status_str(res.status), res.nv))
        return 1;

    horizon_gpu_device_info info;
    res = horizon_gpu_device_get_info(dev, &info);
    t_check(t, horizon_gpu_succeeded(res), "cycle %d: get_info", cycle);

    /* This project's target is GM20B specifically (CLAUDE.md), so the
     * chipname, has_syncpoints and the two page-size consistency checks
     * below are hard requirements, not hedges — a different chip, or an
     * internally inconsistent characteristics query, is a real failure
     * here. num_gpc/num_tpc_per_gpc and the engine class numbers are
     * genuine plausibility bounds (nonzero is all that is asserted; their
     * exact values are recorded, not required). Print what we saw either
     * way. */
    t_note(t, "chip='%s' arch=0x%x impl=0x%x rev=0x%x", info.chipname,
           info.arch, info.impl, info.rev);
    t_note(t, "gpc=%u tpc/gpc=%u l2=0x%llx va_bits=%u", info.num_gpc,
           info.num_tpc_per_gpc, (unsigned long long)info.l2_cache_size,
           info.gpu_va_bit_count);
    t_note(t, "big_page=0x%x avail_big=0x%x compression_page=0x%x",
           info.big_page_size, info.available_big_page_sizes,
           info.compression_page_size);
    t_note(t, "classes: 3d=0x%x compute=0x%x 2d=0x%x gpfifo=0x%x i2m=0x%x "
           "copy=0x%x", info.threed_class, info.compute_class,
           info.twod_class, info.gpfifo_class, info.inline_to_memory_class,
           info.dma_copy_class);
    t_note(t, "va_region[small]: base=0x%llx pages=0x%llx page=0x%x",
           (unsigned long long)info.va_regions[0].base,
           (unsigned long long)info.va_regions[0].pages,
           info.va_regions[0].page_size);
    t_note(t, "va_region[big]:   base=0x%llx pages=0x%llx page=0x%x",
           (unsigned long long)info.va_regions[1].base,
           (unsigned long long)info.va_regions[1].pages,
           info.va_regions[1].page_size);

    /* The two device queries closed as Phase 4 extensions 1 and 6. Both
     * run at device level and neither had ever been executed anywhere
     * when they were written, so they are exercised here rather than
     * left to be discovered by the first caller.
     */
    uint64_t ts1 = 0, ts2 = 0;
    horizon_gpu_result tres = horizon_gpu_device_get_timestamp(dev, &ts1);
    if (t_check(t, tres.status == HORIZON_GPU_OK,
                "cycle %d: GPU timestamp readable (status=%s)", cycle,
                horizon_gpu_status_str(tres.status))) {
        t_note(t, "GPU timestamp: %llu ns", (unsigned long long)ts1);
        t_check(t, ts1 != 0, "cycle %d: GPU timestamp is not zero", cycle);
        /* Monotonic within one call pair. Not a rate measurement — that
         * needs a known interval and belongs in t_ostime — only that the
         * clock is a clock and not a constant. */
        tres = horizon_gpu_device_get_timestamp(dev, &ts2);
        if (tres.status == HORIZON_GPU_OK) {
            t_check(t, ts2 >= ts1,
                    "cycle %d: GPU timestamp did not go backwards "
                    "(%llu then %llu)", cycle,
                    (unsigned long long)ts1, (unsigned long long)ts2);
        }
    }

    horizon_gpu_zcull_info zc;
    horizon_gpu_result zres = horizon_gpu_device_get_zcull_info(dev, &zc);
    if (zres.status == HORIZON_GPU_OK) {
        t_note(t, "zcull: align=%ux%u aliquots=%u total=%u subregions=%u "
               "ctx_size=0x%llx", zc.width_align_pixels,
               zc.height_align_pixels, zc.pixel_squares_by_aliquots,
               zc.aliquot_total, zc.subregion_count,
               (unsigned long long)zc.ctx_size);
        /* The context size is what channel.c reserves a Zcull buffer of,
         * so it has to be a size a buffer can have. "Nonzero" was the
         * first version of this check and it passed on the emulator's
         * 0x1 — a one-byte Zcull context is not a small answer, it is a
         * stub. A check that cannot reject an obviously wrong value is
         * not checking anything.
         *
         * One page is the floor: horizon_gpu_mem_create allocates in
         * pages, so anything under that cannot describe a real buffer.
         */
        t_check(t, zc.ctx_size >= 0x1000,
                "cycle %d: zcull context size is a plausible buffer size "
                "(0x%llx)", cycle, (unsigned long long)zc.ctx_size);
        t_check(t, zc.width_align_pixels > 0 && zc.height_align_pixels > 0,
                "cycle %d: zcull alignments nonzero", cycle);
    } else {
        /* Advisory, exactly as nouveau treats it — reported, not failed. */
        t_note(t, "zcull info unavailable (status=%s); this is advisory and "
               "NVK carries on with has_zcull_info false",
               horizon_gpu_status_str(zres.status));
    }

    t_check(t, strcmp(info.chipname, "gm20b") == 0, "cycle %d: chipname is "
            "gm20b (got '%s')", cycle, info.chipname);
    t_check(t, info.num_gpc >= 1 && info.num_tpc_per_gpc >= 1,
            "cycle %d: GPC/TPC counts nonzero", cycle);
    t_check(t, info.big_page_size >= 0x10000 &&
            (info.big_page_size & (info.big_page_size - 1)) == 0,
            "cycle %d: big page size plausible (0x%x)", cycle,
            info.big_page_size);
    t_check(t, info.threed_class != 0 && info.compute_class != 0 &&
            info.dma_copy_class != 0 && info.inline_to_memory_class != 0,
            "cycle %d: engine classes queried nonzero", cycle);
    t_check(t, info.has_syncpoints, "cycle %d: HAS_SYNCPOINTS flag set",
            cycle);
    t_check(t, info.va_regions[0].page_size == 0x1000,
            "cycle %d: small-page region is 4 KiB pages (0x%x)", cycle,
            info.va_regions[0].page_size);
    t_check(t, info.va_regions[1].page_size == info.big_page_size,
            "cycle %d: big-page region matches characteristics (0x%x)",
            cycle, info.va_regions[1].page_size);

    horizon_gpu_device_counters c;
    res = horizon_gpu_device_get_counters(dev, &c);
    t_check(t, horizon_gpu_succeeded(res) && c.live_mem == 0 &&
            c.live_va_ranges == 0 && c.live_mappings == 0 &&
            c.live_channels == 0,
            "cycle %d: all live-object counters zero", cycle);

    res = horizon_gpu_device_destroy(dev);
    t_check(t, horizon_gpu_succeeded(res),
            "cycle %d: device_destroy (status=%s)", cycle,
            horizon_gpu_status_str(res.status));
    return 0;
}

int run_test(test_ctx *t)
{
    /* Two full cycles in one process prove teardown is complete
     * (milestones.md, test 1). */
    if (one_cycle(t, 1))
        return 1;
    return one_cycle(t, 2);
}
