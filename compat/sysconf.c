/* sysconf() for devkitA64's newlib on Horizon OS.
 *
 * WHY THIS FILE IS ALLOWED TO EXIST. CLAUDE.md bans libc interposition
 * as an architecture — no linker-level symbol wrapping, no fake
 * open/stat/mmap — but leaves one door open: "a genuinely missing
 * newlib function may still be provided in compat/, documented
 * individually". This is that case, and it is not interposition:
 * nothing here shadows a function that exists. newlib *declares* sysconf
 * (aarch64-none-elf/include/sys/unistd.h:236) and defines every _SC_
 * name used below; it simply never defines the symbol. Measured: a
 * program that calls sysconf compiles cleanly and fails to link with
 *
 *     undefined reference to `sysconf'
 *
 * Nothing here overrides anything. We define a symbol the C library
 * leaves undefined, with the prototype the C library published.
 *
 * WHO NEEDS IT. Mesa's src/util/os_misc.c answers os_get_page_size(),
 * os_get_total_physical_memory() and os_get_available_system_memory()
 * from sysconf when HAVE_SYSCONF is set, and #errors out on a platform
 * that has no other route. Those three are not optional for NVK: a
 * failing os_get_total_physical_memory() makes
 * nvk_physical_device_create() return VK_ERROR_INITIALIZATION_FAILED,
 * so no Vulkan device is enumerated at all.
 *
 * Mesa's src/util/u_cpu_detect.c is the second caller: it counts
 * processors with sysconf(_SC_NPROCESSORS_ONLN), and util/u_queue.c
 * sizes its thread pool from the result. Answering it here rather than
 * inside Mesa is what keeps <switch.h> out of Mesa's generic src/util —
 * a layering rule of this project, and the reason such a patch could
 * never go upstream.
 *
 * Layer rules (CLAUDE.md): compat/ may use newlib and libnx, and must
 * not touch Mesa, NVK or horizon/. This file uses exactly libnx's svc.h
 * and newlib.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <unistd.h>

#include <switch.h>

/* Horizon's page granularity on AArch64.
 *
 * Source: switchbrew's SVC documentation — every memory-manipulating
 * svc (svcMapMemory, svcUnmapMemory, svcSetMemoryPermission, …) is
 * documented as taking sizes "aligned to 0x1000 bytes", which libnx
 * repeats verbatim in switch/kernel/svc.h. ARMv8-A's smallest
 * translation granule is 4 KiB, so this cannot be smaller.
 *
 * Corroborated by this project's own hardware runs rather than taken on
 * faith: horizon_gpu_mem_create() allocates with
 * aligned_alloc(HORIZON_GPU_SMALL_PAGE_SIZE = 0x1000, …) and hands the
 * result to nvMapCreate, which requires page-aligned CPU memory. It was
 * accepted on a real console across t_alloc (21/21), t_nvmap (16/16)
 * and t_map (26/26) — a larger CPU page would have made those
 * allocations unaligned and the ioctl would have rejected them.
 *
 * tests/t_sysinfo.c re-checks it on hardware without assuming it: it
 * asserts that every region svcGetInfo reports is a multiple of the
 * value returned here, which a wrong page size would break.
 *
 * Deliberately a separate constant from horizon/'s
 * HORIZON_GPU_SMALL_PAGE_SIZE, which is the GPU MMU's small page.
 * The two happen to agree; they are not the same quantity, and compat/
 * may not include horizon/ headers anyway.
 */
#define HORIZON_CPU_PAGE_SIZE UINT64_C(0x1000)

/* Query a u64 from svcGetInfo for the current process.
 *
 * svcGetInfo is a raw syscall: it needs no service session and no
 * __appInit, so sysconf() is safe to call before libnx's service
 * initialisation, which matters because a C library function may be
 * called from anywhere.
 */
static bool
horizon_process_info(u32 id0, u64 *out)
{
    Result rc = svcGetInfo(out, id0, CUR_PROCESS_HANDLE, 0);
    return R_SUCCEEDED(rc);
}

/* Pages, saturated to LONG_MAX and never negative: sysconf returns a
 * long, and -1 is reserved for "unavailable". */
static long
bytes_to_pages(u64 bytes)
{
    u64 pages = bytes / HORIZON_CPU_PAGE_SIZE;

    if (pages > (u64)LONG_MAX)
        return LONG_MAX;

    return (long)pages;
}

long
sysconf(int name)
{
    u64 total, used;

    switch (name) {
    case _SC_PAGESIZE: /* _SC_PAGE_SIZE is the same value */
        return (long)HORIZON_CPU_PAGE_SIZE;

    /* WHAT "TOTAL PHYSICAL" MEANS HERE, stated rather than implied.
     *
     * InfoType_TotalMemorySize is "Total amount of memory available for
     * process" (libnx switch/kernel/svc.h:191) — the process's own
     * limit, not the console's DRAM. That is deliberately the number
     * reported, for two reasons. It is what a caller sizing a memory
     * heap actually needs: memory the process cannot allocate is not
     * usable however much of it the machine has. And it is mode-aware —
     * an application and an applet get very different limits on the same
     * console — which is precisely why a hardcoded "4 GiB" would have
     * been wrong rather than merely approximate.
     *
     * SystemInfoType_TotalPhysicalMemorySize (svcGetSystemInfo) would
     * report the console's DRAM, but it is privileged and not reliably
     * available to homebrew.
     */
    case _SC_PHYS_PAGES:
        if (!horizon_process_info(InfoType_TotalMemorySize, &total))
            break;
        return bytes_to_pages(total);

    /* WHERE THE PROCESSOR COUNT COMES FROM.
     *
     * InfoType_CoreMask is "Bitmask of allowed Core IDs" (libnx
     * switch/kernel/svc.h:185) — the set of CPU cores this process may
     * be scheduled on, which is what svcSetThreadCoreMask validates a
     * thread's affinity against. Its population count is therefore the
     * number of processors usable by this process, which is exactly the
     * quantity _SC_NPROCESSORS_ONLN names.
     *
     * ONLN and CONF get the same answer, deliberately, and the reason
     * is a measurement rather than an argument. The kernel's only
     * unprivileged answer about cores is this mask; the wider question —
     * "cores the SoC has, whether or not you may use them" — has no
     * query behind it. svcGetSystemInfo is the candidate, and libnx
     * enumerates everything it accepts:
     *
     *   SystemInfoType_TotalPhysicalMemorySize  = 0
     *   SystemInfoType_UsedPhysicalMemorySize   = 1
     *   SystemInfoType_InitialProcessIdRange    = 2
     *
     * (switch/kernel/svc.h:222-225 in the pinned toolchain image; read
     * out of the image itself, not from documentation.) None of the
     * three is a processor count, so there is nothing to report for CONF
     * that ONLN does not already answer.
     *
     * Reporting the SoC's four Cortex-A57s instead would be a constant
     * nobody queried, and the only caller in sight — Mesa's
     * util_cpu_detect, which uses CONF to size a CPU mask — is better
     * served by a number that is true than by one that is larger. The
     * cost of sharing the case is real and is stated rather than hidden:
     * a caller cannot distinguish "the SoC has four, you may use three",
     * which is the ordinary applet situation on this console. Nothing
     * this project builds needs that distinction, and inventing an
     * answer for it would be worse than not offering it.
     *
     * tests/t_threads.c prints the raw mask, so a console log records
     * the bitmask itself and not only its population count, and it is
     * the place that knows the SoC has four cores — a test may know
     * which console it was launched on; a C library function may not
     * hand a caller a number nobody asked the kernel for. What that test
     * checks about ONLN and CONF is the wiring of this function, not the
     * core count; it says so.
     *
     * Not clamped to any expected value: if a future firmware hands a
     * process more or fewer cores, the measurement should say so.
     */
    case _SC_NPROCESSORS_ONLN:
    case _SC_NPROCESSORS_CONF: {
        u64 core_mask;

        if (!horizon_process_info(InfoType_CoreMask, &core_mask))
            break;
        /* A mask of zero would mean the process may run on no core at
         * all, which cannot be true of a process that is running. Treat
         * it as a failed query rather than reporting zero processors,
         * which callers read as "unknown" only if it is negative. */
        if (core_mask == 0)
            break;
        return (long)__builtin_popcountll(core_mask);
    }

    case _SC_AVPHYS_PAGES:
        if (!horizon_process_info(InfoType_TotalMemorySize, &total))
            break;
        if (!horizon_process_info(InfoType_UsedMemorySize, &used))
            break;
        /* Clamp rather than wrap: the two queries are not atomic with
         * respect to each other, so used > total is observable. */
        if (used >= total)
            return 0;
        return bytes_to_pages(total - used);

    default:
        /* POSIX: an unrecognised name is EINVAL. Everything else this
         * project has not needed is reported as unavailable rather than
         * guessed at, so a caller finds out instead of acting on a
         * number nobody measured. */
        errno = EINVAL;
        return -1;
    }

    /* A recognised name whose svcGetInfo query failed. POSIX allows
     * returning -1 with errno unchanged for an indeterminate limit; set
     * it anyway so the failure is not silent. */
    errno = EIO;
    return -1;
}
