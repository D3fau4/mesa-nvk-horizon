/*
 * Phase 5 probe — t_va_window: what the Horizon address-space allocator
 * actually does around the shader local/shared memory window.
 *
 * WHY THIS TEST EXISTS. NVK programs two apertures at addresses the
 * hardware fixes, not addresses anybody allocates:
 *
 *   SET_SHADER_SHARED_MEMORY_WINDOW = 0xfe000000  (nvk_cmd_dispatch.c:85)
 *   SET_SHADER_LOCAL_MEMORY_WINDOW  = 0xff000000  (nvk_cmd_dispatch.c:82,
 *                                                  nvk_cmd_draw.c:583)
 *
 * A shader's LDL/STL/LDS/STS reach local and shared memory through them,
 * so anything else bound in [0xfe000000, 0x100000000) is read as locals
 * and vice versa. Upstream knows nothing protects that range and says so
 * in a comment; on this platform NVK's own shader heap reservation
 * covers it, because that heap is 4 GiB *contiguous* on pre-Volta
 * (nvk_device.c:336 passes cls_eng3d < VOLTA_A; nvk_mem_arena_init
 * reserves NVK_MEM_ARENA_MAX_SIZE = 4 GiB of VA up front) and the
 * small-page region starts far below the window.
 *
 * Blocking the window off was tried once and broke vkCreateDevice:
 * mesa-patches 0039 reserved it, and the 4 GiB reservation that follows
 * then failed with LibnxNvidiaError_InsufficientMemory. 0040 backed it
 * out. Nobody has ever asked the allocator *why*, and the arithmetic
 * says the space is there: t_va_reserve measured the small-page region
 * at region_pages + 0x100000 = 0x4f7fff pages, i.e. ~15.87 GiB, so
 * roughly 12 GiB sits above 0x100000000 with the window below it.
 *
 * So this test asks. It is a measurement, not a feature: the findings
 * are notes, and the checks are the invariants that must hold whatever
 * the findings turn out to be — above all that the kernel never hands
 * out a range overlapping a reservation that is still live, which is
 * the property the whole VA design rests on.
 *
 * No Vulkan, no Mesa: this is horizon_gpu and the nv services only, so
 * it answers the question without a driver in the way.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <inttypes.h>
#include <stddef.h>

#include "horizon_gpu/device.h"
#include "horizon_gpu/vm.h"
#include "common/testfw.h"

const char *const test_name = "t_va_window";

/* The aperture, as NVK programs it on Maxwell. Two 16 MiB windows that
 * happen to be adjacent, so the range to keep clear is one 32 MiB span
 * ending exactly at 4 GiB. Both methods are 32-bit on pre-Volta
 * (NVA0C0_SET_SHADER_*_MEMORY_WINDOW), which is why the aperture is
 * inside the low 4 GiB at all. */
#define SHADER_SHARED_WINDOW_BASE  UINT64_C(0xfe000000)
#define SHADER_LOCAL_WINDOW_BASE   UINT64_C(0xff000000)
#define SHADER_WINDOW_END          UINT64_C(0x100000000)
#define SHADER_WINDOW_BASE         SHADER_SHARED_WINDOW_BASE
#define SHADER_WINDOW_SIZE         (SHADER_WINDOW_END - SHADER_WINDOW_BASE)

/* What NVK's shader heap reserves: NVK_MEM_ARENA_MAX_SIZE, 1 << 32
 * (nvk_mem_arena.h:17-19), contiguous, small-page. */
#define SHADER_HEAP_VA_SIZE        UINT64_C(0x100000000)

/* The two fixed placements worth trying for that heap. Both are 4 GiB
 * aligned and both are entirely above the window, so a heap placed at
 * either one cannot collide however full it gets. The second exists
 * because the first is the block immediately above the aperture and an
 * allocator may treat a boundary specially. */
#define HEAP_FIXED_TRY_A           UINT64_C(0x100000000)
#define HEAP_FIXED_TRY_B           UINT64_C(0x200000000)

/* A small fixed reservation well above 4 GiB, to separate "FixedOffset
 * does not work up there" from "a 4 GiB request does not fit up there".
 * Deliberately not 4 GiB aligned in the size sense: 64 KiB is the big
 * page size's order of magnitude but this is a small-page request, so it
 * is the smallest interesting probe. */
#define SMALL_FIXED_TRY            UINT64_C(0x180000000)
#define SMALL_FIXED_SIZE           UINT64_C(0x10000)

static bool overlaps_window(uint64_t base, uint64_t size)
{
    return base < SHADER_WINDOW_END && SHADER_WINDOW_BASE < base + size;
}

/* Reserve, report, and hand the range back to the caller (NULL on
 * failure). Every probe in this file is "did it work, and where did it
 * land", so the printing belongs in one place. */
static horizon_gpu_va_range *probe_reserve(test_ctx *t,
                                           horizon_gpu_device *dev,
                                           uint64_t size, uint64_t align,
                                           const char *what)
{
    horizon_gpu_va_range *r = NULL;
    horizon_gpu_result res = horizon_gpu_vm_reserve(dev, size, 0x1000, align,
                                                    &r);
    if (horizon_gpu_failed(res)) {
        t_note(t, "%s: FAILED status=%s nv=0x%08x", what,
               horizon_gpu_status_str(res.status), res.nv);
        return NULL;
    }
    uint64_t base = horizon_gpu_va_range_base(r);
    t_note(t, "%s: ok base=0x%" PRIx64 " size=0x%" PRIx64 " %s the window",
           what, base, horizon_gpu_va_range_size(r),
           overlaps_window(base, horizon_gpu_va_range_size(r)) ? "COVERS"
                                                               : "clears");
    return r;
}

static horizon_gpu_va_range *probe_reserve_fixed(test_ctx *t,
                                                 horizon_gpu_device *dev,
                                                 uint64_t base, uint64_t size,
                                                 const char *what)
{
    horizon_gpu_va_range *r = NULL;
    horizon_gpu_result res = horizon_gpu_vm_reserve_fixed(dev, base, size,
                                                          0x1000, &r);
    if (horizon_gpu_failed(res)) {
        t_note(t, "%s @0x%" PRIx64 ": FAILED status=%s nv=0x%08x", what, base,
               horizon_gpu_status_str(res.status), res.nv);
        return NULL;
    }
    t_note(t, "%s @0x%" PRIx64 ": ok size=0x%" PRIx64, what, base,
           horizon_gpu_va_range_size(r));
    /* horizon_gpu_vm_reserve_fixed promises this and reports a mismatch
     * as a failure rather than returning it. Checked here anyway: it is
     * the one property a fixed request cannot absorb being wrong about,
     * and this test is the only place that asks for fixed ranges at
     * addresses nothing else has claimed. */
    t_check(t, horizon_gpu_va_range_base(r) == base,
            "%s landed exactly where it was asked (0x%" PRIx64 ")", what,
            horizon_gpu_va_range_base(r));
    return r;
}

/* Release and check, tolerating NULL so a failed probe does not need an
 * `if` at every call site. */
static void probe_release(test_ctx *t, horizon_gpu_va_range *r,
                          const char *what)
{
    if (r == NULL)
        return;
    horizon_gpu_result res = horizon_gpu_vm_release(r);
    t_check(t, horizon_gpu_succeeded(res), "release %s (status=%s nv=0x%08x)",
            what, horizon_gpu_status_str(res.status), res.nv);
}

int run_test(test_ctx *t)
{
    horizon_gpu_device *dev = NULL;
    horizon_gpu_result res = horizon_gpu_device_create(NULL, &dev);
    if (!t_check(t, horizon_gpu_succeeded(res), "device_create (status=%s "
                 "nv=0x%08x)", horizon_gpu_status_str(res.status), res.nv))
        return 1;

    horizon_gpu_device_info info;
    res = horizon_gpu_device_get_info(dev, &info);
    if (!t_check(t, horizon_gpu_succeeded(res), "get_info"))
        goto out;

    const uint64_t small_lo = info.va_regions[0].base;
    const uint64_t small_hi = small_lo + info.va_regions[0].pages *
                              (uint64_t)info.va_regions[0].page_size;
    t_note(t, "small-page region [0x%" PRIx64 ", 0x%" PRIx64 ") page=0x%x",
           small_lo, small_hi, info.va_regions[0].page_size);
    t_note(t, "big-page region   [0x%" PRIx64 ", 0x%" PRIx64 ") page=0x%x",
           info.va_regions[1].base,
           info.va_regions[1].base + info.va_regions[1].pages *
           (uint64_t)info.va_regions[1].page_size,
           info.va_regions[1].page_size);
    t_note(t, "shader window     [0x%" PRIx64 ", 0x%" PRIx64 ") = shared "
           "0x%" PRIx64 " + local 0x%" PRIx64, SHADER_WINDOW_BASE,
           SHADER_WINDOW_END, SHADER_SHARED_WINDOW_BASE,
           SHADER_LOCAL_WINDOW_BASE);

    /* The premise of everything below: the window is inside the region
     * the small-page allocator hands out. If it were not, there would be
     * no collision to reason about and the rest of this test would be
     * measuring nothing. */
    const bool window_in_small_region = SHADER_WINDOW_BASE >= small_lo &&
                                        SHADER_WINDOW_END <= small_hi;
    t_check(t, window_in_small_region,
            "the shader window lies inside the small-page region");
    t_note(t, "space above the window inside the region: 0x%" PRIx64 " bytes",
           small_hi > SHADER_WINDOW_END ? small_hi - SHADER_WINDOW_END : 0);

    /* ---- Probe 1: the baseline. What NVK does today, unmodified. ---- */
    horizon_gpu_va_range *heap0 =
        probe_reserve(t, dev, SHADER_HEAP_VA_SIZE, 0,
                      "P1 heap 4 GiB non-fixed, nothing held");
    bool p1_ok = heap0 != NULL;
    bool p1_covers = false;
    if (heap0 != NULL) {
        uint64_t b = horizon_gpu_va_range_base(heap0);
        p1_covers = overlaps_window(b, horizon_gpu_va_range_size(heap0));
        t_check(t, b >= small_lo && b + horizon_gpu_va_range_size(heap0) <=
                small_hi, "P1 reservation lies inside the small-page region");
    }
    probe_release(t, heap0, "P1");

    /* ---- Probe 2: can the aperture be reserved at all? ---- */
    horizon_gpu_va_range *window =
        probe_reserve_fixed(t, dev, SHADER_WINDOW_BASE, SHADER_WINDOW_SIZE,
                            "P2 window fixed");
    const bool p2_ok = window != NULL;

    /* Everything from here to the matching release runs with the window
     * held, which is the configuration patch 0039 created. */

    /* ---- Probe 3: 0039's failure, measured on hardware. ---- */
    horizon_gpu_va_range *heap1 =
        probe_reserve(t, dev, SHADER_HEAP_VA_SIZE, 0,
                      "P3 heap 4 GiB non-fixed, window held");
    const bool p3_ok = heap1 != NULL;
    if (heap1 != NULL && p2_ok) {
        /* The allocator must not hand out a range that overlaps a live
         * reservation. If this fails, the finding is far bigger than
         * the window: no reservation in this driver means anything. */
        t_check(t, !overlaps_window(horizon_gpu_va_range_base(heap1),
                                    horizon_gpu_va_range_size(heap1)),
                "P3 did not overlap the reservation already held");
    }
    probe_release(t, heap1, "P3");

    /* ---- Probes 4 and 5: place the heap above the window by name. ---- */
    horizon_gpu_va_range *heapA =
        probe_reserve_fixed(t, dev, HEAP_FIXED_TRY_A, SHADER_HEAP_VA_SIZE,
                            "P4 heap 4 GiB fixed");
    const bool p4_ok = heapA != NULL;
    probe_release(t, heapA, "P4");

    horizon_gpu_va_range *heapB =
        probe_reserve_fixed(t, dev, HEAP_FIXED_TRY_B, SHADER_HEAP_VA_SIZE,
                            "P5 heap 4 GiB fixed");
    const bool p5_ok = heapB != NULL;
    probe_release(t, heapB, "P5");

    /* ---- Probe 6: is FixedOffset honoured above 4 GiB at all? ----
     * Separates "4 GiB does not fit up there" from "fixed placement
     * does not work up there", which are different defects with
     * different answers. */
    horizon_gpu_va_range *small_high =
        probe_reserve_fixed(t, dev, SMALL_FIXED_TRY, SMALL_FIXED_SIZE,
                            "P6 64 KiB fixed");
    const bool p6_ok = small_high != NULL;
    probe_release(t, small_high, "P6");

    probe_release(t, window, "P2 window");

    /* ---- Probe 7: the order the real fix would use. ----
     * Strategy A reserves the heap high *first* and the window second,
     * which is not the order 0039 used. If the allocator's answer
     * depends on order, this is where it shows.
     */
    horizon_gpu_va_range *heap2 =
        probe_reserve_fixed(t, dev, HEAP_FIXED_TRY_A, SHADER_HEAP_VA_SIZE,
                            "P7 heap first, fixed");
    horizon_gpu_va_range *window2 = NULL;
    if (heap2 != NULL)
        window2 = probe_reserve_fixed(t, dev, SHADER_WINDOW_BASE,
                                      SHADER_WINDOW_SIZE,
                                      "P7 then window fixed");
    const bool p7_ok = heap2 != NULL && window2 != NULL;
    probe_release(t, window2, "P7 window");
    probe_release(t, heap2, "P7 heap");

    /* ---- What the run decided. ----
     * Strategy A is "reserve the window and put the shader heap above
     * it", and it needs a fixed 4 GiB placement above the aperture to
     * work in the order the driver would use it. Strategy B is "leave
     * the aperture alone, keep detecting the overlap, and fail loudly
     * if anything is ever *bound* inside it" — always available,
     * because it changes no reservation. */
    t_note(t, "VERDICT window reservable=%s | 4 GiB non-fixed with window "
           "held=%s | 4 GiB fixed above window: A=%s B=%s | 64 KiB fixed "
           "high=%s | heap-then-window=%s",
           p2_ok ? "yes" : "no", p3_ok ? "yes" : "no", p4_ok ? "yes" : "no",
           p5_ok ? "yes" : "no", p6_ok ? "yes" : "no", p7_ok ? "yes" : "no");
    t_note(t, "VERDICT strategy A (block the window, place the heap above "
           "it) is %s on this hardware", p7_ok ? "VIABLE" : "NOT viable");
    t_note(t, "VERDICT today's layout %s the window with the shader heap's "
           "reservation", p1_covers ? "covers" : "does NOT cover");

    /* The one thing that must be true whichever way the findings went:
     * the probe finished and gave the address space back. A run that
     * failed to reserve anything at all measured nothing, so P1 — the
     * reservation NVK makes today and which is known to work — is the
     * check that the probe itself ran. */
    t_check(t, p1_ok, "P1 succeeded, so the probe was able to measure");

    horizon_gpu_device_counters c;
    res = horizon_gpu_device_get_counters(dev, &c);
    t_check(t, horizon_gpu_succeeded(res) && c.live_va_ranges == 0,
            "live_va_ranges back to zero (%u)", c.live_va_ranges);

out:
    res = horizon_gpu_device_destroy(dev);
    t_check(t, horizon_gpu_succeeded(res), "device_destroy (status=%s)",
            horizon_gpu_status_str(res.status));
    return 0;
}
