/*
 * An allocation offered a borrowed block, and what mem_create does with
 * it.
 *
 * `gpu_memory/borrowed_pages` establishes the predicate:
 * `horizon_gpu_heap_range_is_ours` refuses a page this process has lent
 * away, whether the lend left it writable or not. THIS case is about
 * the loop built on top of it — `mem_create`'s walk, which sets a
 * rejected block aside, never frees it, and asks the allocator again
 * until it gets memory the process may write. That walk is what stands
 * between a Vulkan application and the Data Abort that killed Godot on
 * roughly one launch in three, and until this case its own suite had no
 * assertion about it: section F read the quarantine counters and then
 * asserted `t_check(t, true, ...)`, which is a line in the log and not
 * a check.
 *
 * HOW A CONSOLE IS MADE TO OFFER ONE, WITHOUT RISKING THE PROCESS.
 *
 *   1. allocate a block, page-aligned, sixty-four times the size of the
 *      allocation to come;
 *   2. free it, so the allocator is holding it and will hand it out
 *      again — this happens BEFORE the lend, so nothing writes into a
 *      borrowed page to do it;
 *   3. lend it with `svcCreateTransferMemory` — and with **Perm_Rw**,
 *      not Perm_None. That is the whole safety argument of this case.
 *      A lent range is refused by the predicate because it is
 *      *borrowed*, which `borrowed_pages` section C measured on a
 *      console; with Perm_Rw it also stays readable and writable by
 *      this process, so malloc's own bookkeeping inside a chunk it
 *      considers free — free-list pointers, chunk headers, the split
 *      of a chunk to serve the next request — is ordinary memory
 *      traffic and cannot fault. With Perm_None every one of those
 *      would be a store to an unwritable page, which ends the process
 *      with no return path;
 *   4. ask `horizon_gpu_mem_create` for one page of it. The allocator
 *      hands back the chunk it just took in, the guard refuses it, the
 *      walk sets it aside and asks again — and because nothing rejected
 *      is ever freed, each attempt gets an address past the last one,
 *      so the walk marches through the lend rather than retrying the
 *      same place. It succeeds on the first block beyond it.
 *
 * WHAT IT ASSERTS. That the allocation still succeeded; that the
 * quarantine counters grew, which is the only externally visible
 * evidence that the walk ran at all; that they grew by exactly the
 * bytes those blocks are, so a counter that double-counts or forgets is
 * caught; that the walk said so once in the summary line its per-block
 * warnings stop repeating, which is the only record a support log would
 * carry of a long walk; and that the memory finally handed back is
 * memory this process may write.
 *
 * IT FAILS IF THE ALLOCATOR NEVER OFFERS THE LENT BLOCK, and that is
 * deliberate. The case allocated the block, freed it and lent it, so
 * there is no state left for it to be wrong about; if malloc hands back
 * something else entirely then this case did not ask its question, and
 * a passing line saying nothing is what it exists to replace. The log
 * says which of the two happened.
 *
 * THE LEAK IS THE POINT AND IS BOUNDED. Every block the walk sets aside
 * stays allocated for the life of the process — freeing it would return
 * it to the allocator that just offered it. This case therefore leaks
 * up to BR_LEND_B, a quarter of a megabyte, on a console with gigabytes
 * of heap, and it leaks it after `borrowed_pages` has already reported
 * this launch's own quarantine figure, which is why it is the case
 * after it rather than a section inside it.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include "horizon_gpu/device.h"
#include "horizon_gpu/memory.h"
#include "common/testfw.h"

/* The allocation under test, and the lend it has to land in. Both are
 * whole numbers of pages, which svcCreateTransferMemory requires of its
 * source range.
 *
 * ONE PAGE, AND A LEND OF SIXTY-FOUR OF THEM, for two reasons. A
 * four-kilobyte request is the small-allocation case: the walk's old
 * 32-attempt cap bound it long before the byte budget did, so a request
 * landing at the start of a multi-megabyte borrowed span refused with
 * OUT_OF_MEMORY while 46 MiB of budget was still unspent — a device
 * creation or an application start-up that fails. And a lend this size
 * makes the walk step over dozens of blocks rather than one, which is
 * the length nothing in the tree has ever produced on purpose (see
 * docs/PENDING-HARDWARE-RUNS.md section 8). The whole of it is leaked,
 * a quarter of a megabyte, and the paragraph at the bottom of this
 * comment says why that is the design and not an accident. */
#define BR_ALLOC_B UINT64_C(0x1000)      /* one small page  */
#define BR_LEND_B  (BR_ALLOC_B * 64u)    /* 256 KiB         */

TEST_CASE_DECL(gpu_memory, borrowed_retry)
{
    /* On its own rather than inherited: homebrew is launched with no
     * environment, but a run driven from a shell may carry anything,
     * and this case is meaningless with the guard off. The framework
     * restores the environment around every case. */
    setenv("HORIZON_GPU_HEAP_CHECK", "1", 1);

    horizon_gpu_device *dev = NULL;
    horizon_gpu_result res = horizon_gpu_device_create(NULL, &dev);
    if (!t_check(t, horizon_gpu_succeeded(res), "device_create (status=%s "
                 "nv=0x%08x)", horizon_gpu_status_str(res.status), res.nv))
        return 1;

    uint64_t q_bytes_before = 0, q_bytes_after = 0;
    uint32_t q_blocks_before = 0, q_blocks_after = 0;
    horizon_gpu_heap_quarantine_stats(&q_bytes_before, &q_blocks_before);
    t_note(t, "before: %u block(s), %" PRIu64 " bytes set aside in this "
              "process", q_blocks_before, q_bytes_before);

    void *block = aligned_alloc((size_t)0x1000, (size_t)BR_LEND_B);
    if (!t_check(t, block != NULL, "a 0x%" PRIx64 "-byte page-aligned block "
                 "to lend", BR_LEND_B)) {
        horizon_gpu_device_destroy(dev);
        return 1;
    }

    /* Freed FIRST, lent SECOND. The other order would leave free() and
     * every allocator operation after it writing into a range the
     * kernel has marked borrowed.
     *
     * AND THE ADDRESS IS KEPT AS AN INTEGER ACROSS THE free(). What is
     * being described from here on is a *range of the heap* that the
     * allocator is holding and will hand out again, not an object:
     * nothing in this case dereferences it, and only the kernel (which
     * is told the range) and malloc (which owns it) touch those bytes.
     *
     * `volatile` is what makes that expressible. Without it the
     * compiler follows the value through the cast and reports
     * -Wuse-after-free — correctly, for a pointer; this is not one, and
     * a volatile read is the standard way of saying "the value, not the
     * object". It is not a warning being switched off: the diagnostic
     * would be right about any other use of this address, and there is
     * no other use of it here. */
    volatile uintptr_t lend_addr = (uintptr_t)block;
    free(block);
    block = NULL;
    void *const lend = (void *)lend_addr;

    TransferMemory tmem = { 0 };
    Result rc = tmemCreateFromMemory(&tmem, lend, (size_t)BR_LEND_B,
                                     Perm_Rw);
    if (!t_check(t, R_SUCCEEDED(rc),
                 "the freed block is lent to another process with Perm_Rw, "
                 "so it is borrowed and still writable (rc=0x%08x)", rc)) {
        horizon_gpu_device_destroy(dev);
        return 1;
    }

    horizon_gpu_heap_region bad = { 0 };
    t_check(t, !horizon_gpu_heap_range_is_ours(lend, BR_LEND_B, &bad),
            "and horizon_gpu refuses it, which is what makes it the block "
            "the walk has to step past");

    horizon_gpu_mem *mem = NULL;
    res = horizon_gpu_mem_create(dev, BR_ALLOC_B, 0x1000,
                                 HORIZON_GPU_MEM_CACHED, &mem);
    horizon_gpu_heap_quarantine_stats(&q_bytes_after, &q_blocks_after);

    const uint32_t set_aside_n = q_blocks_after - q_blocks_before;
    const uint64_t set_aside_B = q_bytes_after - q_bytes_before;
    t_note(t, "after: the walk set aside %u block(s), %" PRIu64 " bytes, "
              "for one 0x%" PRIx64 "-byte allocation", set_aside_n,
           set_aside_B, BR_ALLOC_B);

    t_check(t, horizon_gpu_succeeded(res),
            "an allocation offered a borrowed block still succeeds "
            "(status=%s nv=0x%08x)", horizon_gpu_status_str(res.status),
            res.nv);

    t_check(t, set_aside_n > 0,
            "the allocator did offer the lent block and the guard set it "
            "aside — a zero here is not a guard failure, it is malloc "
            "having served this request from somewhere else, and then "
            "this case asked nothing");

    if (set_aside_n > 0) {
        /* Every rejected block is the same size, because every attempt
         * is the same aligned_alloc: the total is exactly the count
         * times the request rounded to its alignment, and BR_ALLOC_B
         * is already a multiple of the page. */
        t_check(t, set_aside_B == (uint64_t)set_aside_n * BR_ALLOC_B,
                "and the bytes set aside are exactly those blocks "
                "(%" PRIu64 " for %u of 0x%" PRIx64 ")", set_aside_B,
                set_aside_n, BR_ALLOC_B);

        /* THE SUMMARY LINE IS PART OF THE CONTRACT. A walk of dozens of
         * blocks logs the first rejection in full and then counts, so
         * the line naming the total is the only record a crash report
         * or a support log would carry. Read back out of this case's
         * own portion of the log, not asserted from memory. */
        bool found = false;
        if (t_log_scan(t, "walked past", &found)) {
            t_check(t, found,
                    "and the walk said so once, in the summary line the "
                    "per-block warnings stop repeating");
        } else {
            t_note(t, "the log could not be scanned; the summary line is "
                      "unchecked rather than absent");
        }
    }

    if (mem) {
        void *cpu = horizon_gpu_mem_cpu_ptr(mem);
        memset(&bad, 0, sizeof(bad));
        t_check(t, cpu != NULL &&
                   horizon_gpu_heap_range_is_ours(cpu, BR_ALLOC_B, &bad),
                "and what it handed back is memory this process may write");
        t_check(t, horizon_gpu_succeeded(horizon_gpu_mem_destroy(mem)),
                "destroy the allocation the walk earned");
    }

    rc = tmemClose(&tmem);
    t_check(t, R_SUCCEEDED(rc), "the lend closes (rc=0x%08x)", rc);

    horizon_gpu_device_counters c;
    horizon_gpu_device_get_counters(dev, &c);
    t_check(t, c.live_mem == 0 && c.live_va_ranges == 0 &&
               c.live_mappings == 0,
            "all counters back to zero (mem=%u ranges=%u mappings=%u)",
            c.live_mem, c.live_va_ranges, c.live_mappings);

    res = horizon_gpu_device_destroy(dev);
    t_check(t, horizon_gpu_succeeded(res), "device_destroy (status=%s)",
            horizon_gpu_status_str(res.status));
    return 0;
}
