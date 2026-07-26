/*
 * Test 11 — compat/sysconf.c against the real kernel.
 *
 * compat/sysconf.c is the only code in this project that answers a
 * question with a number nobody can check by compiling: the CPU page
 * size is a cited constant, and the memory figures come from svcGetInfo
 * at run time. Phase 3 could only cross-build it. This test is what
 * turns those into measurements.
 *
 * It deliberately avoids asserting the constant against itself. The
 * page-size check that matters is the consistency one: every memory
 * region the kernel reports must be a whole number of pages. If the
 * real page size were larger than what sysconf returns, a region
 * boundary would eventually land off it.
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
#include <unistd.h>

#include <switch.h>

#include "common/testfw.h"

const char *const test_name = "t_sysinfo";

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

    /* The real check on the page size: the kernel's own region
     * boundaries have to be whole pages of it. This is measured against
     * the kernel, not against the constant compat/ was compiled with. */
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
     * claim more than total minus what was used at the time. */
    t_check(t, (u64)avail_pages <= (total_bytes - used_bytes) / (u64)page_size + 1,
            "available pages agree with total - used (%llu pages)",
            (unsigned long long)((total_bytes - used_bytes) / (u64)page_size));

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
