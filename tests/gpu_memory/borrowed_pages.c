/*
 * The heap page check, and what it is guarding against.
 *
 * WHY THIS CASE EXISTS. Godot on this driver died before its first
 * frame on roughly one launch in three, and the Atmosphère report put
 * the fault in a `memset` called from `horizon_gpu_mem_create` while it
 * was zero-filling a swapchain image. The faulting page was
 * `type=Heap attr=IsBorrowed(0x1) perm=None(0x0)` — present in the
 * process, and unwritable. When mesa patch `0072` let that allocation
 * skip its zero fill the deaths carried on, with the log ending one
 * line later: the fill was gone and `armDCacheFlush` was left standing
 * on the same page, because `dc civac` faults exactly as a store does.
 *
 * Where the borrow comes from is not this project's to fix.
 * nx-hbloader runs one `.nro` after another in the SAME process and
 * hands each of them the same heap, so a TransferMemory made from heap
 * — which is how libnx gives the socket and audio services their
 * buffers — outlives the program that made it if that program died
 * before closing it, and the next program's malloc arena has a hole in
 * it that malloc knows nothing about.
 *
 * What IS this project's to fix is that a GPU allocator must not write
 * pages the process does not own. `horizon_gpu_heap_range_is_ours`
 * asks the kernel, and `mem_create` sets aside any block that fails
 * rather than faulting on it.
 *
 * WHAT THIS CASE ASKS, AND WHY IT CAN ASK IT AT ALL. The condition can
 * be created deliberately: `svcCreateTransferMemory` over a heap range
 * with `Perm_None` puts the source pages into exactly the state the
 * crash report showed. So the predicate can be tested against a real
 * borrowed page rather than against a description of one:
 *
 *   A  an ordinary heap block is ours
 *   B  the same block, lent with Perm_None, is not — and the region the
 *      check reports is printed, so the log carries the shape to
 *      compare against the crash report
 *   C  lent with Perm_Rw it is still refused: writable is not the same
 *      as unshared, and writing into a buffer another process is
 *      reading is a different bug from faulting on it
 *   D  closing the lend gives the block back
 *   E  what THIS launch's address space actually holds — every region
 *      with an attribute, and every heap region that is not writable.
 *      This is the diagnostic the crash needs: it says whether the
 *      process was handed a borrowed hole at all, and where. Run twice:
 *      once inside section B's lend, where there is a borrowed region
 *      by construction, and once at the end for the launch itself
 *   F  how much heap horizon_gpu has had to set aside so far, which is
 *      0 on a healthy launch and is the only number that says the guard
 *      did something
 *
 * AND E IS ALSO WHAT CHECKS horizon_gpu_heap_borrowed_regions. That
 * function is called once per device creation to emit the warning that
 * explains the crash, and until this case compared them it had no test
 * of any class: the walk here and the walk there are twins that have
 * always been written out twice, and the one in horizon/ used to be
 * missing the address-space ceiling the one here has always had. The
 * comparison is made inside section B's lend on purpose — with no lend
 * open, a healthy launch makes both answers 0, and 0 == 0 would agree
 * whatever either function did.
 *
 * NOTHING HERE WRITES A BORROWED PAGE. The whole point is that doing so
 * ends the process, so the case reads the kernel's description of the
 * pages and never touches them; section D writes the block only after
 * the lend is closed.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include "horizon_gpu/memory.h"
#include "common/testfw.h"

/* Page-aligned and a whole number of pages, which is what
 * svcCreateTransferMemory requires of its source range. */
#define BP_PAGE   UINT64_C(0x1000)
#define BP_SIZE   UINT64_C(0x10000)

static void bp_report(test_ctx *t, const char *what,
                      const horizon_gpu_heap_region *r)
{
    t_note(t, "%s: region 0x%" PRIx64 "+0x%" PRIx64
              " type=%u attr=0x%x perm=0x%x",
           what, r->addr, r->size, (unsigned)r->type, (unsigned)r->attr,
           (unsigned)r->perm);
}

/* Every region of this process's address space that is not plainly
 * ours, reported. Costs one syscall per region and changes nothing.
 *
 * The borrowed ones are also counted and handed back, because they are
 * what horizon_gpu_heap_borrowed_regions answers and this is the walk
 * it is compared against. `label` names the section in the log; the
 * three out-parameters may not be NULL. */
static void bp_scan_process(test_ctx *t, const char *label,
                            uint32_t *out_n, uint64_t *out_B,
                            horizon_gpu_heap_region *out_first)
{
    uint64_t addr = 0;
    uint32_t regions = 0, flagged = 0, borrowed_n = 0;
    uint64_t borrowed_B = 0;

    *out_n = 0;
    *out_B = 0;
    memset(out_first, 0, sizeof(*out_first));

    for (;;) {
        MemoryInfo info = { 0 };
        u32 pageinfo = 0;
        if (R_FAILED(svcQueryMemory(&info, &pageinfo, addr)))
            break;
        if (info.size == 0)
            break;

        regions++;

        const bool borrowed = (info.attr & MemAttr_IsBorrowed) != 0;
        const bool unwritable_heap =
            info.type == MemType_Heap &&
            (info.perm & Perm_Rw) != Perm_Rw;

        if (borrowed) {
            if (borrowed_n == 0) {
                *out_first = (horizon_gpu_heap_region){
                    .addr = info.addr, .size = info.size,
                    .type = info.type, .attr = info.attr,
                    .perm = info.perm,
                };
            }
            borrowed_n++;
            borrowed_B += info.size;
        }

        if (borrowed || unwritable_heap || (info.attr != 0)) {
            flagged++;
            t_note(t, "%s: 0x%" PRIx64 "+0x%" PRIx64 " type=%u attr=0x%x "
                      "perm=0x%x%s%s", label, info.addr, info.size,
                   (unsigned)info.type, (unsigned)info.attr,
                   (unsigned)info.perm,
                   borrowed ? " BORROWED" : "",
                   unwritable_heap ? " HEAP-NOT-WRITABLE" : "");
        }

        const uint64_t next = info.addr + info.size;
        if (next <= addr)
            break;
        addr = next;
        if (addr >= (UINT64_C(1) << 39))
            break;
    }

    t_note(t, "%s: %u region(s) walked, %u with an attribute or an "
              "unwritable heap page, %u borrowed, %" PRIu64 " bytes "
              "borrowed", label, regions, flagged, borrowed_n, borrowed_B);
    t_check(t, regions > 0, "%s: the address space could be walked", label);

    *out_n = borrowed_n;
    *out_B = borrowed_B;
}

/* The walk above against horizon_gpu's own, which is the one a device
 * creation runs. Same address space, same instant, no allocation in
 * between: the two must agree on all three answers. */
static void bp_compare_walks(test_ctx *t, const char *label)
{
    uint32_t mine_n = 0;
    uint64_t mine_B = 0;
    horizon_gpu_heap_region mine_first = { 0 };
    uint64_t theirs_B = 0;
    horizon_gpu_heap_region theirs_first = { 0 };

    bp_scan_process(t, label, &mine_n, &mine_B, &mine_first);

    const uint32_t theirs_n =
        horizon_gpu_heap_borrowed_regions(&theirs_B, &theirs_first);

    t_note(t, "%s: horizon_gpu_heap_borrowed_regions says %u region(s), "
              "%" PRIu64 " bytes; this case's own walk says %u, %" PRIu64,
           label, theirs_n, theirs_B, mine_n, mine_B);

    t_check(t, theirs_n == mine_n,
            "%s: horizon_gpu_heap_borrowed_regions counts the regions this "
            "case's own walk counts (%u)", label, mine_n);
    t_check(t, theirs_B == mine_B,
            "%s: and the same number of bytes (%" PRIu64 ")", label, mine_B);
    t_check(t, theirs_first.addr == mine_first.addr &&
               theirs_first.size == mine_first.size &&
               theirs_first.attr == mine_first.attr &&
               theirs_first.perm == mine_first.perm,
            "%s: and reports the same first region", label);
}

TEST_CASE_DECL(gpu_memory, borrowed_pages)
{
    void *buf = aligned_alloc((size_t)BP_PAGE, (size_t)BP_SIZE);
    if (!t_check(t, buf != NULL, "a 0x%" PRIx64 "-byte page-aligned heap "
                 "block", BP_SIZE))
        return 1;

    /* --- A: an ordinary heap block is ours ------------------------- */
    horizon_gpu_heap_region bad = { 0 };
    bool ours = horizon_gpu_heap_range_is_ours(buf, BP_SIZE, &bad);
    if (!t_check(t, ours, "A: a freshly allocated heap block is memory "
                 "this process may write")) {
        bp_report(t, "A", &bad);
        /* Not fatal to the case: E below is exactly the report that
         * explains it, and it is more useful than stopping here. */
    }

    /* Writing it is safe while A holds, and proves the check is not
     * simply always true. */
    if (ours) {
        memset(buf, 0x5a, (size_t)BP_SIZE);
        armDCacheFlush(buf, (size_t)BP_SIZE);
        t_check(t, ((const unsigned char *)buf)[BP_SIZE - 1] == 0x5a,
                "A: and writing and flushing it does what it says");
    }

    /* --- B: lent with Perm_None, it is not ------------------------- */
    TransferMemory tmem = { 0 };
    Result rc = tmemCreateFromMemory(&tmem, buf, (size_t)BP_SIZE,
                                     Perm_None);
    if (t_check(t, R_SUCCEEDED(rc), "B: svcCreateTransferMemory over the "
                "block with Perm_None (rc=0x%08x)", rc)) {
        memset(&bad, 0, sizeof(bad));
        const bool still_ours =
            horizon_gpu_heap_range_is_ours(buf, BP_SIZE, &bad);
        t_check(t, !still_ours,
                "B: a page lent to another process is refused — this is "
                "the state the Godot crash report showed");
        if (!still_ours)
            bp_report(t, "B", &bad);
        t_check(t, !still_ours &&
                   ((bad.attr & 0x1u) != 0 || (bad.perm & 0x3u) != 0x3u),
                "B: and it is refused for the reason claimed: borrowed, "
                "or not readable and writable");

        /* THE ONE MOMENT THIS CASE KNOWS A BORROWED REGION EXISTS: it
         * made one. Everything horizon_gpu_heap_borrowed_regions has to
         * get right — finding it, sizing it, describing it — is
         * answerable here and nowhere else in a healthy launch. */
        bp_compare_walks(t, "B/E");

        rc = tmemClose(&tmem);
        t_check(t, R_SUCCEEDED(rc), "B: the lend closes (rc=0x%08x)", rc);
    }

    /* --- C: lent with Perm_Rw, it is still refused ----------------- */
    memset(&tmem, 0, sizeof(tmem));
    rc = tmemCreateFromMemory(&tmem, buf, (size_t)BP_SIZE, Perm_Rw);
    if (t_check(t, R_SUCCEEDED(rc), "C: the same block lent with Perm_Rw "
                "(rc=0x%08x)", rc)) {
        memset(&bad, 0, sizeof(bad));
        const bool still_ours =
            horizon_gpu_heap_range_is_ours(buf, BP_SIZE, &bad);
        t_check(t, !still_ours,
                "C: writable is not unshared — a borrowed page is refused "
                "whatever its permissions");
        if (!still_ours)
            bp_report(t, "C", &bad);

        rc = tmemClose(&tmem);
        t_check(t, R_SUCCEEDED(rc), "C: the lend closes (rc=0x%08x)", rc);
    }

    /* --- D: and closing it gives the block back -------------------- */
    memset(&bad, 0, sizeof(bad));
    const bool ours_again =
        horizon_gpu_heap_range_is_ours(buf, BP_SIZE, &bad);
    if (t_check(t, ours_again,
                "D: with the lend closed the block is ours again")) {
        memset(buf, 0xa5, (size_t)BP_SIZE);
        armDCacheFlush(buf, (size_t)BP_SIZE);
        t_check(t, ((const unsigned char *)buf)[0] == 0xa5,
                "D: and it takes a write and a cache flush");
    } else {
        bp_report(t, "D", &bad);
    }

    free(buf);

    /* --- E: what this launch was actually handed ------------------- *
     *
     * The same comparison again, on the address space as the launcher
     * left it. On a healthy launch both answers are 0 and it proves
     * nothing on its own — B/E above is where the comparison has
     * something to compare — but on a netloaded launch this is the
     * hole itself, and then the two walks are being checked against
     * each other over regions neither of them made. */
    bp_compare_walks(t, "E");

    /* --- F: what the allocator has had to set aside ---------------- */
    uint64_t q_bytes = 0;
    uint32_t q_blocks = 0;
    horizon_gpu_heap_quarantine_stats(&q_bytes, &q_blocks);
    t_note(t, "F: horizon_gpu has set aside %u block(s), %" PRIu64
              " bytes, in this process so far", q_blocks, q_bytes);
    t_check(t, true, "F: the quarantine counters are readable — 0 blocks "
            "means no allocation in this process has been offered a page "
            "it may not write, which is the healthy answer");

    return 0;
}
