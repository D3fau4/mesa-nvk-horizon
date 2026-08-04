/*
 * Phase 1 test 5 — t_map: map/unmap at a fixed VA, explicit kind and page
 * size, unmap invalidates (milestones.md). Exercises big-page maps
 * (known-risks R9) and proves the recorded-VA invariant of
 * memory-model § 2.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <inttypes.h>

#include "horizon_gpu/device.h"
#include "horizon_gpu/memory.h"
#include "horizon_gpu/vm.h"
#include "common/testfw.h"

const char *const test_name = "t_map";
/* No display: main() starts a console and reports through it. */
const bool test_uses_display = false;

int run_test(test_ctx *t)
{
    horizon_gpu_device *dev = NULL;
    horizon_gpu_result res = horizon_gpu_device_create(NULL, &dev);
    if (!t_check(t, horizon_gpu_succeeded(res), "device_create (status=%s "
                 "nv=0x%08x)", horizon_gpu_status_str(res.status), res.nv))
        return 1;

    horizon_gpu_device_info info;
    res = horizon_gpu_device_get_info(dev, &info);
    t_check(t, horizon_gpu_succeeded(res), "get_info");

    horizon_gpu_mem *mem = NULL;
    horizon_gpu_va_range *range = NULL;
    res = horizon_gpu_mem_create(dev, 0x4000, 0, HORIZON_GPU_MEM_CACHED,
                                 &mem);
    t_check(t, horizon_gpu_succeeded(res), "mem_create 16 KiB");
    res = horizon_gpu_vm_reserve(dev, 0x100000, 0x1000, 0, &range);
    t_check(t, horizon_gpu_succeeded(res), "reserve 1 MiB");
    if (!mem || !range)
        return 1;

    uint64_t base = horizon_gpu_va_range_base(range);

    /* FIXED map at an explicit offset inside the reservation. */
    horizon_gpu_mapping *map1 = NULL;
    res = horizon_gpu_vm_map(range, 0x10000, mem, 0, 0x4000,
                             HORIZON_GPU_PTE_KIND_PITCH, true, &map1);
    t_check(t, horizon_gpu_succeeded(res), "map pitch at base+0x10000 "
            "(status=%s nv=0x%08x)", horizon_gpu_status_str(res.status),
            res.nv);
    if (map1) {
        t_check(t, horizon_gpu_mapping_va(map1) == base + 0x10000,
                "mapping VA is the requested fixed VA (0x%" PRIx64 ")",
                horizon_gpu_mapping_va(map1));
        t_check(t, horizon_gpu_mem_mapped_va(mem) == base + 0x10000,
                "mem records the mapped VA");
    }

    /* Overlap is refused before reaching the kernel. */
    horizon_gpu_mapping *bad = NULL;
    res = horizon_gpu_vm_map(range, 0x12000, mem, 0, 0x1000,
                             HORIZON_GPU_PTE_KIND_PITCH, true, &bad);
    t_check(t, res.status == HORIZON_GPU_ERR_BUSY,
            "overlapping map rejected (%s)",
            horizon_gpu_status_str(res.status));

    /* Out-of-object range is refused. */
    res = horizon_gpu_vm_map(range, 0x30000, mem, 0x2000, 0x3000,
                             HORIZON_GPU_PTE_KIND_PITCH, true, &bad);
    t_check(t, res.status == HORIZON_GPU_ERR_OVERFLOW,
            "map past the end of the memory object rejected (%s)",
            horizon_gpu_status_str(res.status));

    /* Second, disjoint mapping of the same object. */
    horizon_gpu_mapping *map2 = NULL;
    res = horizon_gpu_vm_map(range, 0x40000, mem, 0x1000, 0x1000,
                             HORIZON_GPU_PTE_KIND_GENERIC_16BX2, true,
                             &map2);
    t_note(t, "map with kind Generic_16BX2: status=%s nv=0x%08x",
           horizon_gpu_status_str(res.status), res.nv);
    t_check(t, horizon_gpu_succeeded(res),
            "second disjoint map (block-linear generic kind)");

    /* Destroying mem or releasing the range while mapped must refuse. */
    res = horizon_gpu_mem_destroy(mem);
    t_check(t, res.status == HORIZON_GPU_ERR_BUSY,
            "mem_destroy while mapped refused (%s)",
            horizon_gpu_status_str(res.status));
    res = horizon_gpu_vm_release(range);
    t_check(t, res.status == HORIZON_GPU_ERR_BUSY,
            "vm_release while mapped refused (%s)",
            horizon_gpu_status_str(res.status));

    /* Unmap clears the recorded VA and frees the interval for reuse —
     * the kernel must accept a fresh map at the same fixed VA. Unmapping
     * the more-recently-created sibling (map2) first must restore
     * mapped_va to the still-live map1, not clear it out from under it —
     * the exact invariant whose absence causes the reference's
     * double-unmap bug (memory-model § 2). This is the state the
     * intrusive mem_next/mem_prev list exists for; check it, not just the
     * final all-unmapped state below. */
    if (map2 && map1) {
        res = horizon_gpu_vm_unmap(map2);
        t_check(t, horizon_gpu_succeeded(res), "unmap second mapping "
                "(status=%s nv=0x%08x)",
                horizon_gpu_status_str(res.status), res.nv);
        t_check(t, horizon_gpu_mem_mapped_va(mem) == horizon_gpu_mapping_va(map1),
                "unmapping the newer sibling restores mapped_va to the "
                "still-live one, not zero (0x%" PRIx64 " vs 0x%" PRIx64 ")",
                horizon_gpu_mem_mapped_va(mem), horizon_gpu_mapping_va(map1));
    } else if (map2) {
        res = horizon_gpu_vm_unmap(map2);
        t_check(t, horizon_gpu_succeeded(res), "unmap second mapping "
                "(status=%s nv=0x%08x)",
                horizon_gpu_status_str(res.status), res.nv);
    }
    if (map1) {
        res = horizon_gpu_vm_unmap(map1);
        t_check(t, horizon_gpu_succeeded(res), "unmap first mapping");
        t_check(t, horizon_gpu_mem_mapped_va(mem) == 0,
                "unmap cleared the recorded VA (memory-model §2 invariant)");
    }
    horizon_gpu_mapping *remap = NULL;
    res = horizon_gpu_vm_map(range, 0x10000, mem, 0, 0x4000,
                             HORIZON_GPU_PTE_KIND_PITCH, true, &remap);
    t_check(t, horizon_gpu_succeeded(res),
            "same fixed VA mappable again after unmap (unmap really "
            "invalidated)");
    if (remap)
        t_check(t, horizon_gpu_succeeded(horizon_gpu_vm_unmap(remap)),
                "unmap remap");

    /* Big-page mapping: page size comes from the reservation (R9). */
    horizon_gpu_mem *big_mem = NULL;
    horizon_gpu_va_range *big_range = NULL;
    res = horizon_gpu_mem_create(dev, info.big_page_size,
                                 info.big_page_size,
                                 HORIZON_GPU_MEM_CACHED, &big_mem);
    t_check(t, horizon_gpu_succeeded(res), "mem_create one big page "
            "(0x%x, aligned)", info.big_page_size);
    res = horizon_gpu_vm_reserve(dev, (uint64_t)info.big_page_size * 4,
                                 info.big_page_size, 0, &big_range);
    t_check(t, horizon_gpu_succeeded(res), "reserve big-page range");
    if (big_mem && big_range) {
        horizon_gpu_mapping *big_map = NULL;
        res = horizon_gpu_vm_map(big_range, 0, big_mem, 0,
                                 info.big_page_size,
                                 HORIZON_GPU_PTE_KIND_PITCH, true,
                                 &big_map);
        t_note(t, "big-page map: status=%s nv=0x%08x",
               horizon_gpu_status_str(res.status), res.nv);
        t_check(t, horizon_gpu_succeeded(res),
                "map with the reservation's big page size (R9)");
        if (big_map)
            t_check(t,
                    horizon_gpu_succeeded(horizon_gpu_vm_unmap(big_map)),
                    "unmap big-page mapping");
    }
    if (big_range)
        t_check(t, horizon_gpu_succeeded(horizon_gpu_vm_release(big_range)),
                "release big-page range");
    if (big_mem)
        t_check(t, horizon_gpu_succeeded(horizon_gpu_mem_destroy(big_mem)),
                "destroy big-page mem");

    t_check(t, horizon_gpu_succeeded(horizon_gpu_vm_release(range)),
            "release range");
    t_check(t, horizon_gpu_succeeded(horizon_gpu_mem_destroy(mem)),
            "destroy mem");

    horizon_gpu_device_counters c;
    horizon_gpu_device_get_counters(dev, &c);
    t_check(t, c.live_mem == 0 && c.live_va_ranges == 0 &&
            c.live_mappings == 0, "all counters back to zero "
            "(mem=%u ranges=%u mappings=%u)", c.live_mem, c.live_va_ranges,
            c.live_mappings);

    res = horizon_gpu_device_destroy(dev);
    t_check(t, horizon_gpu_succeeded(res), "device_destroy (status=%s)",
            horizon_gpu_status_str(res.status));
    return 0;
}
