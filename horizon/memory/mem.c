/*
 * horizon_gpu — memory object implementation.
 *
 * Ownership (memory-model § 7): horizon_gpu_mem owns the host allocation
 * and the NvMap; destroy refuses while mappings are alive, then closes the
 * NvMap and frees the backing, in that (reverse-of-create) order.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>

#include "mem_priv.h"
#include "align.h"
#include "../device/device_priv.h"

/* THE HEAP IS NOT ALL OURS, AND THE ALLOCATOR DOES NOT KNOW.
 *
 * nx-hbloader runs one .nro after another in the SAME process and hands
 * each of them the same heap, so anything the previous program left
 * behind is inside the next program's malloc arena. The kind that
 * matters here is a page lent to another process: a TransferMemory made
 * from heap — which is how libnx gives the socket and audio services
 * their buffers — leaves its source pages `attr=IsBorrowed perm=None`
 * for as long as the borrow lasts, and a program that died without
 * closing one never ends it. malloc knows nothing about that and hands
 * the range out again.
 *
 * Writing such a page is a CPU Data Abort with no return path: the
 * process dies on the HOME menu with the crash dialog. So is CLEANING
 * it — on aarch64 `dc civac` faults exactly as a store does — which is
 * why the check below guards the whole block and not just the fill.
 *
 * MEASURED: Godot on this driver died before its first frame on roughly
 * one launch in three, and the Atmosphère report put the fault in
 * memset called from horizon_gpu_mem_create, filling the swapchain
 * image. The faulting page was `type=Heap attr=IsBorrowed(0x1)
 * perm=None(0x0)` — present in the process and unwritable. When patch
 * 0072 let that allocation skip its zero fill the deaths continued,
 * with the log now ending one line later: the fill was gone and
 * armDCacheFlush was left standing on the same page.
 *
 * A REJECTED BLOCK IS NEVER FREED. Freeing it would return it to the
 * allocator that just offered it, and the next call would get it
 * straight back. It is set aside instead, which is a deliberate leak of
 * a block this process cannot use for anything anyway, counted so a log
 * can say how much.
 */
static _Atomic uint64_t mem_quarantine_B;
static _Atomic uint32_t mem_quarantine_n;

/* How far mem_create will walk before giving up. Each rejected block
 * stays allocated, so every attempt gets an address past the last one
 * and a run of them walks THROUGH the borrowed span rather than
 * retrying the same place — which is why the meaningful bound is bytes
 * and not attempts.
 *
 * MEASURED ON A CONSOLE 2026-09-08, twice, and THE SPAN IS NOT A
 * CONSTANT. gpu_memory/borrowed_pages section E on one netloaded suite:
 * three borrowed heap regions, 0x47000 + 0x800000 + 0x21000 = 8814592
 * bytes. The device-creation scan on a later launch of the same build:
 * three regions again, 23404544 bytes, the first of them 0xe31000 on
 * its own. hbloader runs the .nro inside its own process and is holding
 * its netloader sockets the whole time, so how much of the heap is lent
 * depends on what it has been doing, not on anything this program can
 * see.
 *
 * So the budget is set well above the largest span observed rather than
 * at it, because the cost of being wrong in one direction is a leaked
 * block and in the other is the process dying. The attempt count is
 * only there to stop an allocator that hands back the same address
 * forever. */
#define MEM_HEAP_CHECK_TRIES    32u
#define MEM_HEAP_CHECK_BUDGET_B (UINT64_C(48) << 20)

bool horizon_gpu_heap_range_is_ours(const void *p, uint64_t size,
                                    horizon_gpu_heap_region *bad)
{
    if (!p || size == 0)
        return false;

    uint64_t addr = (uint64_t)(uintptr_t)p;
    const uint64_t end = addr + size;
    if (end < addr)                     /* wrapped: not a range we made */
        return false;

    while (addr < end) {
        MemoryInfo info = { 0 };
        u32 pageinfo = 0;
        const Result rc = svcQueryMemory(&info, &pageinfo, addr);
        if (R_FAILED(rc)) {
            if (bad)
                *bad = (horizon_gpu_heap_region){ .addr = addr };
            return false;
        }

        const horizon_gpu_heap_region region = {
            .addr = info.addr, .size = info.size,
            .type = info.type, .attr = info.attr, .perm = info.perm,
        };

        /* Two ways to fail, and they are different questions. Not
         * writable is what faults. Borrowed but writable would not
         * fault, and writing it would still be writing into a buffer
         * another process is reading, so it is refused too. */
        if ((info.perm & Perm_Rw) != Perm_Rw ||
            (info.attr & MemAttr_IsBorrowed) != 0) {
            if (bad)
                *bad = region;
            return false;
        }

        /* The kernel always describes the region CONTAINING addr, so
         * this advances — unless it answers something degenerate, and
         * then the honest answer is "cannot prove it" rather than a
         * loop that never ends. */
        const uint64_t next = info.addr + info.size;
        if (info.size == 0 || next <= addr) {
            if (bad)
                *bad = region;
            return false;
        }
        addr = next;
    }
    return true;
}

uint32_t horizon_gpu_heap_borrowed_regions(uint64_t *bytes,
                                           horizon_gpu_heap_region *first)
{
    uint64_t addr = 0, total = 0;
    uint32_t found = 0;

    for (;;) {
        MemoryInfo info = { 0 };
        u32 pageinfo = 0;
        if (R_FAILED(svcQueryMemory(&info, &pageinfo, addr)))
            break;
        if (info.size == 0)
            break;

        if ((info.attr & MemAttr_IsBorrowed) != 0) {
            if (found == 0 && first) {
                *first = (horizon_gpu_heap_region){
                    .addr = info.addr, .size = info.size,
                    .type = info.type, .attr = info.attr,
                    .perm = info.perm,
                };
            }
            found++;
            total += info.size;
        }

        const uint64_t next = info.addr + info.size;
        if (next <= addr)
            break;
        addr = next;
    }

    if (bytes)
        *bytes = total;
    return found;
}

void horizon_gpu_heap_quarantine_stats(uint64_t *bytes, uint32_t *blocks)
{
    if (bytes)
        *bytes = atomic_load(&mem_quarantine_B);
    if (blocks)
        *blocks = atomic_load(&mem_quarantine_n);
}

/* WHO MAY ASK FOR UNINITIALISED STORAGE, AND WHO IN THIS TREE DOES.
 *
 * The rule is one sentence: every byte anything will ever read must be
 * written before it is read. Four callers reach this file, and the
 * audit of them is here rather than in a document because it is the
 * thing a fifth caller has to redo.
 *
 *   channel.c, the hang recorder's buffer (hang_mem)
 *     NO. The CPU reads slots the GPU may never have reached, and zero
 *     is exactly how "never reached" is recognised. Uninitialised, a
 *     slot the GPU never wrote would decode as a marker it did.
 *
 *   channel.c, the internal command buffer (cmdbuf_mem)
 *     QUALIFIES, and is not worth it. Every list the GPU fetches from
 *     it — the L2 prologue, the two fence blocks, a wait-ring slot — is
 *     written before the GPFIFO entry naming it is appended. But it is
 *     one 4 KiB object per channel, created a handful of times in a
 *     process, so the saving is unmeasurable and the risk of the
 *     promise going stale is not.
 *
 *   channel.c, the Zcull context (zcull_mem)
 *     NO. The hardware saves and restores Zcull state into it across a
 *     context switch, and nothing here establishes that the first
 *     access is a save rather than a restore. "The GPU probably writes
 *     it first" is not the promise this path takes.
 *
 *   nvkmd_horizon_mem.c, every Vulkan allocation
 *     NOT PROVABLE AS A WHOLE, and it is the only one where the saving
 *     would show. It is one entry point for VkDeviceMemory the
 *     application maps, for NVK's descriptor tables, query pools and
 *     shader heap, and for the command-buffer and mem-stream chunks.
 *     The last of those DOES qualify — nv_push writes the dwords and
 *     the submit names exactly [addr, addr+range), so nothing unwritten
 *     is ever fetched — but nvkmd has no flag that distinguishes it
 *     from the rest, so routing it needs an NVKMD_MEM_* bit through
 *     nvkmd.h and both backends. Vulkan does not promise zeroed device
 *     memory, but NVK is entitled to rely on its own allocations, and
 *     auditing that is a separate piece of work with a failure mode
 *     that is a wrong pixel rather than an error.
 *
 * So nothing in this tree uses this path today. It is here because the
 * measurement that decides whether routing the command-buffer case is
 * worth doing needs both halves to exist — gpu_memory/alloc times them
 * against each other on a console.
 */
static horizon_gpu_result mem_create(horizon_gpu_device *dev,
                                     uint64_t size, uint64_t align,
                                     horizon_gpu_cache_policy policy,
                                     bool zero_fill,
                                     horizon_gpu_mem **out_mem)
{
    if (!dev || !out_mem || size == 0)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);
    if (policy != HORIZON_GPU_MEM_CACHED &&
        policy != HORIZON_GPU_MEM_UNCACHED)
        return horizon_gpu_err(HORIZON_GPU_ERR_UNSUPPORTED);

    if (align == 0)
        align = HORIZON_GPU_SMALL_PAGE_SIZE;
    if (!horizon_is_pow2_u64(align) || align < HORIZON_GPU_SMALL_PAGE_SIZE)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);

    /* Round the size up to the alignment, overflow-checked before use
     * (aligned_alloc additionally requires size % align == 0). */
    uint64_t rounded;
    if (!horizon_align_up_u64(size, align, &rounded))
        return horizon_gpu_err(HORIZON_GPU_ERR_OVERFLOW);

    /* nvMapCreate takes 32-bit size and alignment; reject rather than
     * truncate (memory-model § 4). */
    if (rounded > UINT32_MAX || align > UINT32_MAX)
        return horizon_gpu_err(HORIZON_GPU_ERR_OVERFLOW);

    horizon_gpu_mem *mem = calloc(1, sizeof(*mem));
    if (!mem)
        return horizon_gpu_err(HORIZON_GPU_ERR_OUT_OF_MEMORY);

    mem->dev = dev;
    mem->size = rounded;
    mem->align = align;
    mem->policy = policy;

    /* Every block is proved writable before anything touches it, and
     * one that is not is set aside rather than freed — the long comment
     * at the top of this file is why. */
    uint64_t set_aside_B = 0;
    for (uint32_t attempt = 0; ; attempt++) {
        mem->cpu = aligned_alloc(align, rounded);
        if (!mem->cpu) {
            free(mem);
            return horizon_gpu_err(HORIZON_GPU_ERR_OUT_OF_MEMORY);
        }
        if (!dev->heap_page_check)
            break;

        horizon_gpu_heap_region bad = { 0 };
        if (horizon_gpu_heap_range_is_ours(mem->cpu, rounded, &bad))
            break;

        atomic_fetch_add(&mem_quarantine_B, rounded);
        const uint32_t n_set_aside =
            atomic_fetch_add(&mem_quarantine_n, 1u) + 1u;
        horizon_logf(&dev->log, HORIZON_LOG_WARN,
                     "heap block %p+0x%llx holds memory this process may "
                     "not write: region 0x%llx+0x%llx type=%u attr=0x%x "
                     "perm=0x%x. Setting it aside and asking again "
                     "(%u block(s), %llu bytes set aside so far); "
                     "HORIZON_GPU_HEAP_CHECK=0 takes the fault instead",
                     mem->cpu, (unsigned long long)rounded,
                     (unsigned long long)bad.addr,
                     (unsigned long long)bad.size,
                     (unsigned)bad.type, (unsigned)bad.attr,
                     (unsigned)bad.perm, (unsigned)n_set_aside,
                     (unsigned long long)atomic_load(&mem_quarantine_B));

        mem->cpu = NULL;                /* set aside, never freed */
        set_aside_B += rounded;

        if (attempt + 1u >= MEM_HEAP_CHECK_TRIES ||
            set_aside_B >= MEM_HEAP_CHECK_BUDGET_B) {
            horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                         "%u heap block(s) of 0x%llx bytes in a row held "
                         "memory this process may not write (%llu bytes "
                         "set aside for this one allocation); refusing it "
                         "rather than faulting on it",
                         (unsigned)(attempt + 1u),
                         (unsigned long long)rounded,
                         (unsigned long long)set_aside_B);
            free(mem);
            return horizon_gpu_err(HORIZON_GPU_ERR_OUT_OF_MEMORY);
        }
    }
    /* Zero-fill: deterministic content for tests and no stale data handed
     * to the GPU. Skipped only by horizon_gpu_mem_create_uninit, whose
     * caller has promised that everything read here is written first;
     * the flush below is NOT skipped with it. */
    if (zero_fill)
        memset(mem->cpu, 0, rounded);

    /* AND THE CACHE HAS TO BE CLEARED OF THIS RANGE, FOR EVERY POLICY
     * AND WHETHER OR NOT IT WAS FILLED.
     *
     * A zero fill goes through the CPU cache and leaves every line of
     * this object dirty. aligned_alloc hands back heap the process has
     * used before, so some of those lines were dirty ALREADY — and that
     * half is true with no fill at all, which is why this runs on both
     * paths. A dirty line is a write that has not happened yet, and it
     * will happen later, at an eviction nobody chose.
     *
     * For an object the GPU writes and the CPU then reads — a Vulkan
     * query pool is exactly that, and it is the first thing in this
     * project to be one — the consequence is a silent wrong answer.
     * The GPU writes the value to memory; the CPU invalidates before
     * reading, and the invalidate on aarch64 is `dc civac`, which
     * CLEANS the line before invalidating it. Cleaning a line still
     * holding the memset's zeros writes those zeros over what the GPU
     * just wrote, and the read that follows returns them.
     *
     * MEASURED ON A CONSOLE 2026-08-24: vk_core/timestamps'
     * vkGetQueryPoolResults answered VK_NOT_READY for two full seconds
     * of polling, on some runs and not others — the run-to-run
     * difference being whether those lines happened to have been
     * evicted in the meantime. With this flush it stops happening.
     *
     * IT IS CHEAPER WITHOUT THE FILL, which is most of what skipping the
     * fill saves. armDCacheFlush is `dc civac` per line either way, but a
     * line the fill never dirtied has nothing to write back — so the
     * uninitialised path skips both the stores and the writeback they
     * would have caused, not just the stores.
     *
     * This used to run only for UNCACHED, where the same hazard is
     * sharper still: after svcSetMemoryAttribute remaps the range, a
     * dirty line evicted later lands on top of whatever was written
     * uncached in between. Both are the same bug and one flush answers
     * both. armDCacheFlush cleans and invalidates, which is what is
     * wanted: nothing of this object should survive in the cache.
     */
    armDCacheFlush(mem->cpu, rounded);

    /* UNCACHED: hand the range to the kernel to remap without CPU
     * caching.
     *
     * svcSetMemoryAttribute wants a page-aligned range; `align` is at
     * least HORIZON_GPU_SMALL_PAGE_SIZE and `rounded` is a multiple of
     * it, so both hold by construction rather than by check.
     *
     * Failure is not survivable half-done: the storage would be
     * allocated with a cache policy that does not match what the caller
     * asked for, which is exactly the mismatch memory-model § 5 rule 1
     * exists to forbid. So it unwinds. */
    if (policy == HORIZON_GPU_MEM_UNCACHED) {
        Result arc = svcSetMemoryAttribute(mem->cpu, rounded,
                                           MemAttr_IsUncached,
                                           MemAttr_IsUncached);
        if (R_FAILED(arc)) {
            horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                         "svcSetMemoryAttribute(uncached, %p, 0x%llx) "
                         "failed: 0x%08x", mem->cpu,
                         (unsigned long long)rounded, arc);
            free(mem->cpu);
            free(mem);
            return horizon_gpu_err_nv(arc);
        }
    }

    /* The NvMap kind is NvKind_Pitch here; block-linear layouts are a
     * property of each GPU *mapping* (its PTE kind), never of the memory
     * object (memory-model § 1 #9). is_cpu_cacheable tells nvmap the
     * truth about this range — which is now the policy the caller asked
     * for, not an assumption about heap memory, and unlike the
     * reference's cacheable=false at every call site (drm_shim.c:460). */
    Result rc = nvMapCreate(&mem->nvmap, mem->cpu, (u32)rounded, (u32)align,
                            NvKind_Pitch,
                            policy == HORIZON_GPU_MEM_CACHED);
    if (R_FAILED(rc)) {
        horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                     "nvMapCreate(size=0x%llx align=0x%llx) failed: 0x%08x",
                     (unsigned long long)rounded, (unsigned long long)align,
                     rc);
        /* Reverse order: the attribute was set after the allocation, so
         * it comes off before the storage goes back. Returning uncached
         * memory to the heap would leave every later allocation that
         * reuses it silently uncached.
         *
         * Which means the restore's own Result decides whether the
         * storage may go back at all, so it is checked rather than
         * issued and forgotten (CLAUDE.md: never discard a libnx
         * Result). If it fails, the pages are deliberately *not* freed:
         * a leak costs this process some address space, while handing
         * uncached pages to malloc costs every later allocation that
         * lands on them, in unrelated code, with no symptom near the
         * cause. Of the two, the leak is the one that can be found. */
        if (policy == HORIZON_GPU_MEM_UNCACHED) {
            Result arc = svcSetMemoryAttribute(mem->cpu, rounded,
                                               MemAttr_IsUncached, 0);
            if (R_FAILED(arc)) {
                /* Logged, not returned. The caller asked why the
                 * allocation failed, and the answer is `rc` from
                 * nvMapCreate; returning the cleanup's error instead
                 * would replace the diagnosis with a footnote about the
                 * unwind. Both appear here, and the one that answers
                 * the caller's question is the one that propagates. */
                horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                             "svcSetMemoryAttribute(restore cached, %p, "
                             "0x%llx) failed: 0x%08x — leaking 0x%llx "
                             "bytes rather than returning uncached pages "
                             "to the heap (unwinding an nvMapCreate that "
                             "failed with 0x%08x)", mem->cpu,
                             (unsigned long long)rounded, arc,
                             (unsigned long long)rounded, rc);
                free(mem);
                return horizon_gpu_err_nv(rc);
            }
        }
        free(mem->cpu);
        free(mem);
        return horizon_gpu_err_nv(rc);
    }

    atomic_fetch_add(&dev->live_mem, 1);
    horizon_logf(&dev->log, HORIZON_LOG_DEBUG,
                 "mem %p: created size=0x%llx align=0x%llx handle=%u id=%u",
                 (void *)mem, (unsigned long long)rounded,
                 (unsigned long long)align, mem->nvmap.handle, mem->nvmap.id);

    *out_mem = mem;
    return horizon_gpu_ok();
}

/* The two public entry points. They differ in one bool and share every
 * check, every rounding and every error path, so a fix to one is a fix
 * to both by construction — which is the reason this is a parameter and
 * not a second copy of the function. */
horizon_gpu_result horizon_gpu_mem_create(horizon_gpu_device *dev,
                                          uint64_t size, uint64_t align,
                                          horizon_gpu_cache_policy policy,
                                          horizon_gpu_mem **out_mem)
{
    return mem_create(dev, size, align, policy, true, out_mem);
}

horizon_gpu_result
horizon_gpu_mem_create_uninit(horizon_gpu_device *dev, uint64_t size,
                              uint64_t align, horizon_gpu_cache_policy policy,
                              horizon_gpu_mem **out_mem)
{
    return mem_create(dev, size, align, policy, false, out_mem);
}

horizon_gpu_result horizon_gpu_mem_destroy(horizon_gpu_mem *mem)
{
    if (!mem)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);

    uint32_t live = atomic_load(&mem->live_mappings);
    if (live != 0) {
        horizon_logf(&mem->dev->log, HORIZON_LOG_ERROR,
                     "mem %p: destroy refused, %u live mapping(s) at "
                     "va=0x%llx", (void *)mem, live,
                     (unsigned long long)mem->mapped_va);
        return horizon_gpu_err(HORIZON_GPU_ERR_BUSY);
    }

    /* `mem` has exactly one documented owner (memory-model § 7): a second
     * destroy call on the same pointer is a caller bug, not a case this
     * layer defends against — `mem` is freed below, so any check reading
     * back through the pointer afterwards would itself be a use-after-free. */
    nvMapClose(&mem->nvmap);
    /* Same reason as the error path in create: the heap gets its pages
     * back as it lent them, cached. Leaving the attribute on would make
     * every later allocation that reuses this address silently uncached
     * — a fault that appears in unrelated code, long afterwards.
     *
     * And, as there, the restore's Result decides whether the pages may
     * go back: on failure they are leaked on purpose and the caller is
     * told. The object itself is destroyed either way — nvMapClose has
     * already run and this must not be retried — so the returned error
     * means "destroyed, and it cost you the backing", not "try again".
     */
    if (mem->policy == HORIZON_GPU_MEM_UNCACHED) {
        Result arc = svcSetMemoryAttribute(mem->cpu, mem->size,
                                           MemAttr_IsUncached, 0);
        if (R_FAILED(arc)) {
            horizon_logf(&mem->dev->log, HORIZON_LOG_ERROR,
                         "mem %p: svcSetMemoryAttribute(restore cached, "
                         "%p, 0x%llx) failed: 0x%08x — leaking 0x%llx "
                         "bytes rather than returning uncached pages to "
                         "the heap", (void *)mem, mem->cpu,
                         (unsigned long long)mem->size, arc,
                         (unsigned long long)mem->size);
            atomic_fetch_sub(&mem->dev->live_mem, 1);
            free(mem);
            return horizon_gpu_err_nv(arc);
        }
    }
    free(mem->cpu);
    atomic_fetch_sub(&mem->dev->live_mem, 1);
    free(mem);
    return horizon_gpu_ok();
}

horizon_gpu_cache_policy horizon_gpu_mem_policy(const horizon_gpu_mem *mem)
{
    return mem ? mem->policy : HORIZON_GPU_MEM_CACHED;
}

void *horizon_gpu_mem_cpu_ptr(const horizon_gpu_mem *mem)
{
    return mem ? mem->cpu : NULL;
}

uint64_t horizon_gpu_mem_size(const horizon_gpu_mem *mem)
{
    return mem ? mem->size : 0;
}

uint32_t horizon_gpu_mem_get_id(const horizon_gpu_mem *mem)
{
    return mem ? mem->nvmap.id : 0;
}

uint32_t horizon_gpu_mem_get_handle(const horizon_gpu_mem *mem)
{
    return mem ? mem->nvmap.handle : 0;
}

uint64_t horizon_gpu_mem_mapped_va(const horizon_gpu_mem *mem)
{
    return mem ? mem->mapped_va : 0;
}

static horizon_gpu_result mem_range_check(const horizon_gpu_mem *mem,
                                          uint64_t offset, uint64_t size)
{
    if (!mem || size == 0)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);
    if (!horizon_range_fits_u64(offset, size, mem->size))
        return horizon_gpu_err(HORIZON_GPU_ERR_OVERFLOW);
    return horizon_gpu_ok();
}

horizon_gpu_result horizon_gpu_mem_flush(horizon_gpu_mem *mem,
                                         uint64_t offset, uint64_t size)
{
    horizon_gpu_result res = mem_range_check(mem, offset, size);
    if (horizon_gpu_failed(res))
        return res;
    /* CPU -> GPU: clean is sufficient; no need to lose the lines. */
    if (mem->policy == HORIZON_GPU_MEM_CACHED)
        armDCacheClean((uint8_t *)mem->cpu + offset, size);
    return horizon_gpu_ok();
}

horizon_gpu_result horizon_gpu_mem_invalidate(horizon_gpu_mem *mem,
                                              uint64_t offset, uint64_t size)
{
    horizon_gpu_result res = mem_range_check(mem, offset, size);
    if (horizon_gpu_failed(res))
        return res;
    /* GPU -> CPU: libnx exposes clean+invalidate (DC CIVAC — the only
     * data-cache maintenance usable from EL0) as armDCacheFlush; the
     * clean half is harmless when the caller respected the contract of
     * not dirtying GPU-owned lines. */
    if (mem->policy == HORIZON_GPU_MEM_CACHED)
        armDCacheFlush((uint8_t *)mem->cpu + offset, size);
    return horizon_gpu_ok();
}
