/*
 * Test 11 — compat/sysconf.c against the real kernel.
 *
 * compat/sysconf.c is the only code in this project that answers a
 * question with a number nobody can check by compiling: the CPU page
 * size is a cited constant, and the memory figures come from svcGetInfo
 * at run time. Phase 3 could only cross-build it. This test is what
 * turns those into measurements.
 *
 * It deliberately avoids asserting the constant against itself, and it
 * bounds the page size from both sides, because either check alone
 * proves only half of it:
 *
 *   too large   every region the kernel reports must be a whole number
 *               of pages. A page size bigger than the real one puts a
 *               region boundary off it.
 *   too small   divisibility cannot see this — a 64 KiB-aligned region
 *               is also divisible by 4 KiB — so the smallest size
 *               svcMapMemory accepts is what answers it.
 *
 * Uses no horizon_gpu: this is the C library and the kernel, nothing
 * else. It needs no nv services, so it is also the cheapest test to run
 * first when triaging a console.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include <switch.h>

#include "common/testfw.h"

const char *const test_name = "t_sysinfo";
/* No display: main() starts a console and reports through it. */
const bool test_uses_display = false;

/* The InfoType values whose address/size pairs describe a memory region.
 * Every one of them must be page-granular. */
typedef struct region_probe {
    const char *name;
    u32 addr_id;
    u32 size_id;
} region_probe;

static const region_probe regions[] = {
    { "heap", InfoType_HeapRegionAddress, InfoType_HeapRegionSize },
    { "alias", InfoType_AliasRegionAddress, InfoType_AliasRegionSize },
    { "aslr", InfoType_AslrRegionAddress, InfoType_AslrRegionSize },
    { "stack", InfoType_StackRegionAddress, InfoType_StackRegionSize },
};

static bool
get_info(test_ctx *t, u32 id, u64 *out, const char *what)
{
    Result rc = svcGetInfo(out, id, CUR_PROCESS_HANDLE, 0);

    if (R_FAILED(rc)) {
        t_note(t, "svcGetInfo(%s) failed: 0x%08x", what, (unsigned)rc);
        return false;
    }
    return true;
}

/* How many multiples of the reported page size to try mapping. 16 covers
 * 4 KiB through 64 KiB, which spans every granule AArch64 defines. */
#define GRANULARITY_LADDER 16

/* Measure the smallest granularity the kernel actually accepts.
 *
 * The divisibility checks below bound the page size from ONE side only:
 * a region aligned to 64 KiB is also divisible by 4 KiB, so they catch a
 * reported page size that is too LARGE and say nothing whatever about
 * one that is too small. (An earlier version of this file claimed the
 * opposite. It was wrong, and the claim is what the review caught.)
 *
 * svcMapMemory is granularity-sensitive: it rejects a size the kernel
 * cannot express. So the other bound is the smallest size it accepts.
 * Walking a ladder rather than testing one size is what makes this
 * self-diagnosing — a failure caused by anything other than granularity
 * (wrong region, no free address space, permissions) fails at *every*
 * rung, and that is visibly different from a granularity answer.
 */
static void
probe_map_granularity(test_ctx *t, u64 page_size)
{
    u64 max_size = page_size * GRANULARITY_LADDER;
    u64 smallest_mapped = 0;
    void *src;

    /* Aligned to the largest candidate, so every smaller one is aligned
     * too and alignment is never what is being measured. */
    src = aligned_alloc((size_t)max_size, (size_t)max_size);
    if (!t_check(t, src != NULL, "allocated 0x%llx to probe map granularity",
                 (unsigned long long)max_size))
        return;

    for (u64 size = page_size; size <= max_size; size *= 2) {
        void *dst;
        Result rc;

        virtmemLock();
        /* The STACK region, not the ASLR one. svcMapMemory is documented
         * as "mainly used for adding guard pages around stack" and only
         * accepts a destination there — measured: with virtmemFindAslr
         * every rung came back 0x0000dc01, which decodes as kernel
         * description 110, KernelError_InvalidMemoryRange. Notably NOT
         * KernelError_InvalidSize (101), which is what a granularity
         * refusal would be, so the first hardware run proved the region
         * was wrong rather than the page size. libnx uses
         * virtmemFindStack for exactly this call. */
        dst = virtmemFindStack((size_t)size, 0);
        rc = dst ? svcMapMemory(dst, src, size) : 1;
        if (dst && R_SUCCEEDED(rc)) {
            Result urc = svcUnmapMemory(dst, src, size);
            virtmemUnlock();
            t_check(t, R_SUCCEEDED(urc),
                    "unmapped the 0x%llx probe (rc=0x%08x)",
                    (unsigned long long)size, (unsigned)urc);
            smallest_mapped = size;
            t_note(t, "svcMapMemory(0x%llx): OK", (unsigned long long)size);
            break;
        }
        virtmemUnlock();
        /* Name the kernel description rather than only printing the hex.
         * InvalidSize is the only one that is a statement about
         * granularity; anything else means the probe itself is wrong,
         * which is what the first hardware run turned out to be. */
        t_note(t, "svcMapMemory(0x%llx): %s 0x%08x%s",
               (unsigned long long)size,
               dst ? "rejected" : "no address space,", (unsigned)rc,
               !dst                                   ? ""
               : R_MODULE(rc) != Module_Kernel        ? " (not a kernel error)"
               : R_DESCRIPTION(rc) == KernelError_InvalidSize
                   ? " InvalidSize — a granularity refusal"
               : R_DESCRIPTION(rc) == KernelError_InvalidMemoryRange
                   ? " InvalidMemoryRange — wrong region, not granularity"
               : R_DESCRIPTION(rc) == KernelError_InvalidMemoryState
                   ? " InvalidMemoryState — source not mappable"
                   : " (other kernel error)");
    }

    free(src);

    /* Zero means the ladder never mapped anything, which is not a
     * statement about granularity — say so instead of blaming the page
     * size. The note lines above carry each rung's Result for triage. */
    if (!t_check(t, smallest_mapped != 0,
                 "the kernel mapped at least one size up to 0x%llx",
                 (unsigned long long)max_size)) {
        t_note(t, "granularity NOT measured: no rung mapped, so the cause "
                  "is something other than the page size");
        return;
    }

    t_check(t, smallest_mapped == page_size,
            "smallest granularity the kernel accepts is 0x%llx, and "
            "sysconf reports 0x%llx",
            (unsigned long long)smallest_mapped,
            (unsigned long long)page_size);
}

int
run_test(test_ctx *t)
{
    long page_size, phys_pages, avail_pages;
    u64 total_bytes = 0, used_bytes = 0;

    /* ---- page size ---------------------------------------------- */

    errno = 0;
    page_size = sysconf(_SC_PAGESIZE);
    t_check(t, page_size > 0, "sysconf(_SC_PAGESIZE) = %ld (errno %d)",
            page_size, errno);
    if (page_size <= 0)
        return 1; /* everything below is expressed in pages */

    t_check(t, (page_size & (page_size - 1)) == 0,
            "page size 0x%lx is a power of two", page_size);

    /* _SC_PAGE_SIZE is documented as the same value, and newlib defines
     * it as an alias. Checking it costs nothing and catches a compat/
     * switch statement that handled only one of the two spellings. */
    t_check(t, sysconf(_SC_PAGE_SIZE) == page_size,
            "_SC_PAGE_SIZE agrees with _SC_PAGESIZE");

    t_note(t, "page size reported as 0x%lx (%ld bytes)", page_size, page_size);

    /* Lower bound: the smallest granularity the kernel accepts. Without
     * this, everything below passes just as happily for a page size
     * reported too small. */
    probe_map_granularity(t, (u64)page_size);

    /* Upper bound: the kernel's own region boundaries have to be whole
     * pages of it. Measured against the kernel, not against the constant
     * compat/ was compiled with. */
    for (size_t i = 0; i < sizeof(regions) / sizeof(regions[0]); i++) {
        u64 addr = 0, size = 0;

        if (!get_info(t, regions[i].addr_id, &addr, regions[i].name) ||
            !get_info(t, regions[i].size_id, &size, regions[i].name)) {
            t_check(t, false, "%s region readable", regions[i].name);
            continue;
        }

        t_note(t, "%s region: addr=0x%llx size=0x%llx", regions[i].name,
               (unsigned long long)addr, (unsigned long long)size);
        t_check(t, (addr % (u64)page_size) == 0,
                "%s region address 0x%llx is page-aligned", regions[i].name,
                (unsigned long long)addr);
        t_check(t, (size % (u64)page_size) == 0,
                "%s region size 0x%llx is a whole number of pages",
                regions[i].name, (unsigned long long)size);
    }

    /* ---- total and available memory ------------------------------ */

    if (!get_info(t, InfoType_TotalMemorySize, &total_bytes, "TotalMemorySize"))
        return t_check(t, false, "InfoType_TotalMemorySize readable") ? 0 : 1;
    if (!get_info(t, InfoType_UsedMemorySize, &used_bytes, "UsedMemorySize"))
        return t_check(t, false, "InfoType_UsedMemorySize readable") ? 0 : 1;

    t_note(t, "svcGetInfo raw: total=0x%llx (%llu MiB) used=0x%llx (%llu MiB)",
           (unsigned long long)total_bytes,
           (unsigned long long)(total_bytes >> 20),
           (unsigned long long)used_bytes,
           (unsigned long long)(used_bytes >> 20));

    errno = 0;
    phys_pages = sysconf(_SC_PHYS_PAGES);
    t_check(t, phys_pages > 0, "sysconf(_SC_PHYS_PAGES) = %ld (errno %d)",
            phys_pages, errno);

    /* This is the claim compat/ makes about what it reports: the
     * process's own limit, not the console's DRAM. Stated in the file,
     * checked here. */
    t_check(t, (u64)phys_pages == total_bytes / (u64)page_size,
            "_SC_PHYS_PAGES x page size = InfoType_TotalMemorySize "
            "(%llu vs %llu bytes)",
            (unsigned long long)((u64)phys_pages * (u64)page_size),
            (unsigned long long)total_bytes);

    errno = 0;
    avail_pages = sysconf(_SC_AVPHYS_PAGES);
    t_check(t, avail_pages >= 0, "sysconf(_SC_AVPHYS_PAGES) = %ld (errno %d)",
            avail_pages, errno);
    t_check(t, avail_pages <= phys_pages,
            "available pages (%ld) do not exceed total (%ld)", avail_pages,
            phys_pages);

    /* Used memory moves between the two svcGetInfo calls compat/ makes,
     * so this is a bound rather than an equality: available must not
     * claim more than total minus what was used at the time.
     *
     * The clamp has to happen here too, and for the same reason compat/
     * applies one: used > total is observable between two non-atomic
     * queries, and on u64 the subtraction would then wrap to something
     * enormous, making the bound trivially true and the check worthless
     * in precisely the case it exists to cover. */
    u64 free_pages = used_bytes >= total_bytes
                         ? 0
                         : (total_bytes - used_bytes) / (u64)page_size;

    t_check(t, (u64)avail_pages <= free_pages + 1,
            "available pages agree with total - used (%llu pages)",
            (unsigned long long)free_pages);

    t_note(t, "reported free: %llu MiB of %llu MiB",
           (unsigned long long)(((u64)avail_pages * (u64)page_size) >> 20),
           (unsigned long long)(((u64)phys_pages * (u64)page_size) >> 20));

    /* ---- POSIX behaviour for an unknown name --------------------- */

    /* -1 with EINVAL, not a plausible-looking guess. A caller has to be
     * able to tell "unsupported" from "the answer is small". */
    errno = 0;
    t_check(t, sysconf(INT_MAX) == -1 && errno == EINVAL,
            "unknown name returns -1 and sets EINVAL (errno %d)", errno);

    return 0;
}
