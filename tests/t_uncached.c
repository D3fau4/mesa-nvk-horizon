/*
 * Phase 4 test — t_uncached: the UNCACHED cache policy (decision D14) on
 * real memory.
 *
 * NVK advertises a HOST_VISIBLE|HOST_COHERENT memory type, and on an SoC
 * that means an uncached map (nvk_physical_device.c:1571-1575). nvkmd
 * then skips cache maintenance entirely for NVKMD_MEM_COHERENT
 * (nvkmd.c:457), so the promise is kept by the allocation itself or not
 * at all. horizon_gpu keeps it with svcSetMemoryAttribute and
 * MemAttr_IsUncached over the heap range.
 *
 * Three things about that can only be answered by the kernel, and this
 * test asks each one separately so a failure names which:
 *
 *   1. Does svcSetMemoryAttribute accept our allocations at all? It is
 *      documented as requiring the whole range to be in one memory block
 *      with a compatible state, and our storage comes from aligned_alloc
 *      out of the heap. A refusal here is a creation failure, so it
 *      surfaces as mem_create failing rather than as a wrong result.
 *   2. Is the resulting mapping usable by ordinary C? Uncached on AArch64
 *      is Normal-NC if the kernel is generous and Device-nGnRE if it is
 *      not, and unaligned or multi-register accesses to Device memory
 *      fault. memcpy and a dword loop over the range are the check.
 *   3. Does the GPU see CPU writes to it with NO cache maintenance? This
 *      is the actual coherency claim. The test builds a command list in
 *      uncached memory, never calls horizon_gpu_mem_flush, submits it and
 *      waits: completion means the GPU read what the CPU wrote.
 *
 * Point 3 is the one that matters, and it needs no new engine methods —
 * the command list the GPU fetches IS the CPU write being tested.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <inttypes.h>
#include <string.h>

#include <switch.h>

#include "horizon_gpu/channel.h"
#include "horizon_gpu/cmds.h"
#include "horizon_gpu/device.h"
#include "horizon_gpu/memory.h"
#include "horizon_gpu/submit.h"
#include "horizon_gpu/sync.h"
#include "horizon_gpu/vm.h"
#include "common/testfw.h"

const char *const test_name = "t_uncached";

#define BUF_SIZE   UINT64_C(0x1000)
#define WAIT_NS    UINT64_C(2000000000) /* 2 s, as in t_submit */
#define NOP_PAIRS  64u
#define PATTERN    UINT32_C(0xA5C3F00D)

int run_test(test_ctx *t)
{
    horizon_gpu_device *dev = NULL;
    horizon_gpu_result res = horizon_gpu_device_create(NULL, &dev);
    if (!t_check(t, horizon_gpu_succeeded(res), "device_create (status=%s "
                 "nv=0x%08x)", horizon_gpu_status_str(res.status), res.nv))
        return 1;

    /* --- 1. the allocation itself -------------------------------------
     *
     * Both policies, same size and alignment, so the only difference
     * between them is the attribute change. CACHED first: if it fails
     * too, the problem is the allocator and not D14.
     */
    horizon_gpu_mem *cached = NULL;
    res = horizon_gpu_mem_create(dev, BUF_SIZE, 0, HORIZON_GPU_MEM_CACHED,
                                 &cached);
    bool cached_ok = t_check(t, horizon_gpu_succeeded(res),
                             "control: CACHED allocation of 0x%llx "
                             "(status=%s nv=0x%08x)",
                             (unsigned long long)BUF_SIZE,
                             horizon_gpu_status_str(res.status), res.nv);

    horizon_gpu_mem *mem = NULL;
    res = horizon_gpu_mem_create(dev, BUF_SIZE, 0, HORIZON_GPU_MEM_UNCACHED,
                                 &mem);
    if (!t_check(t, horizon_gpu_succeeded(res),
                 "UNCACHED allocation of 0x%llx — svcSetMemoryAttribute "
                 "accepted the range (status=%s nv=0x%08x)",
                 (unsigned long long)BUF_SIZE,
                 horizon_gpu_status_str(res.status), res.nv)) {
        t_note(t, "D14 does not hold on this system: without an uncached "
                  "allocation NVK's HOST_COHERENT memory type is a promise "
                  "nothing keeps, because nvkmd skips maintenance for it");
        if (cached_ok)
            horizon_gpu_mem_destroy(cached);
        horizon_gpu_device_destroy(dev);
        return 1;
    }

    t_check(t, horizon_gpu_mem_policy(mem) == HORIZON_GPU_MEM_UNCACHED,
            "the object reports the policy it was created with");
    if (cached_ok) {
        t_check(t, horizon_gpu_mem_policy(cached) == HORIZON_GPU_MEM_CACHED,
                "control: the CACHED object reports CACHED");
    }

    /* --- 2. is it usable by ordinary C? -------------------------------
     *
     * If Horizon mapped this Device-nGnRE rather than Normal-NC, one of
     * these faults rather than failing, and the .nro dies here with a
     * data abort. That is itself the answer — the log stops at the last
     * line printed, which is why the note precedes the accesses.
     */
    volatile uint32_t *p = horizon_gpu_mem_cpu_ptr(mem);
    if (t_check(t, p != NULL, "uncached mapping has a CPU pointer")) {
        t_note(t, "about to touch the uncached range; if the log ends here, "
                  "the kernel gave us Device memory and normal loads and "
                  "stores fault on it");

        for (uint64_t i = 0; i < BUF_SIZE / 4; i++)
            p[i] = PATTERN;

        uint64_t bad = 0;
        for (uint64_t i = 0; i < BUF_SIZE / 4; i++) {
            if (p[i] != PATTERN)
                bad++;
        }
        t_check(t, bad == 0, "dword stores and loads read back (%llu/%llu "
                "wrong)", (unsigned long long)bad,
                (unsigned long long)(BUF_SIZE / 4));

        /* memcpy is the one that picks the wide multi-register forms. */
        static const char msg[] = "uncached memcpy";
        memcpy((void *)p, msg, sizeof(msg));
        t_check(t, memcmp((const void *)p, msg, sizeof(msg)) == 0,
                "memcpy over the uncached range round-trips");
    }

    /* --- 3. maintenance is a no-op, and says so --------------------- */
    res = horizon_gpu_mem_flush(mem, 0, BUF_SIZE);
    t_check(t, horizon_gpu_succeeded(res),
            "flush on uncached memory succeeds as a no-op (status=%s)",
            horizon_gpu_status_str(res.status));
    res = horizon_gpu_mem_invalidate(mem, 0, BUF_SIZE);
    t_check(t, horizon_gpu_succeeded(res),
            "invalidate on uncached memory succeeds as a no-op (status=%s)",
            horizon_gpu_status_str(res.status));

    /* --- 4. the coherency claim, executed --------------------------- */
    horizon_gpu_channel *chan = NULL;
    res = horizon_gpu_channel_create(dev, NULL, &chan);
    bool chan_ok = t_check(t, horizon_gpu_succeeded(res),
                           "channel_create (status=%s nv=0x%08x)",
                           horizon_gpu_status_str(res.status), res.nv);

    horizon_gpu_va_range *range = NULL;
    horizon_gpu_mapping *map = NULL;
    bool mapped = false;
    if (chan_ok) {
        res = horizon_gpu_vm_reserve(dev, 0x10000, 0x1000, 0, &range);
        if (t_check(t, horizon_gpu_succeeded(res),
                    "reserved VA for the uncached buffer (status=%s)",
                    horizon_gpu_status_str(res.status))) {
            res = horizon_gpu_vm_map(range, 0, mem, 0, BUF_SIZE,
                                     HORIZON_GPU_PTE_KIND_PITCH, true, &map);
            mapped = t_check(t, horizon_gpu_succeeded(res),
                             "mapped uncached memory at a GPU VA (status=%s "
                             "nv=0x%08x)", horizon_gpu_status_str(res.status),
                             res.nv);
        }
    }

    if (mapped) {
        /* Write the command list and submit it WITHOUT flushing. On
         * cached memory this would be a bug — the list could still sit in
         * the CPU's cache when the GPU fetches it. That it works here is
         * the property D14 exists for, so the absence of the flush call
         * is the test, not an oversight. */
        uint32_t *cmds = horizon_gpu_mem_cpu_ptr(mem);
        uint32_t n = horizon_cmds_nop(cmds, (uint32_t)(BUF_SIZE / 4),
                                      NOP_PAIRS);
        if (t_check(t, n != 0, "built a %u-pair NOP list in uncached memory",
                    NOP_PAIRS)) {
            horizon_gpu_cmd_span span = {
                .gpu_va = horizon_gpu_mapping_va(map),
                .num_dwords = n,
            };
            horizon_gpu_fence f;
            res = horizon_gpu_submit(chan, &span, 1,
                                     HORIZON_GPU_SUBMIT_DEFAULT, &f);
            if (t_check(t, horizon_gpu_succeeded(res),
                        "submitted it with NO cache maintenance (status=%s "
                        "nv=0x%08x)", horizon_gpu_status_str(res.status),
                        res.nv)) {
                res = horizon_gpu_channel_wait_fence(chan, f, WAIT_NS);
                if (t_check(t, horizon_gpu_succeeded(res),
                            "the GPU read the un-flushed list and completed "
                            "(status=%s)",
                            horizon_gpu_status_str(res.status))) {
                    t_note(t, "D14 holds: CPU writes to UNCACHED memory are "
                              "visible to the GPU without a flush");
                } else {
                    uint32_t type = 0;
                    const char *desc = NULL;
                    if (horizon_gpu_succeeded(
                            horizon_gpu_channel_get_error(chan, &type, &desc)))
                        t_note(t, "channel error notifier: %u (%s)", type,
                               desc ? desc : "?");
                }
            }
        }
    }

    /* --- teardown, reverse order; destroy must restore the attribute --
     *
     * If horizon_gpu_mem_destroy left the range uncached, the heap gets
     * that page back with the attribute still set. Allocating again after
     * the destroy is what would notice.
     */
    if (map)
        horizon_gpu_vm_unmap(map);
    if (range)
        horizon_gpu_vm_release(range);
    if (chan_ok)
        horizon_gpu_channel_destroy(chan);

    res = horizon_gpu_mem_destroy(mem);
    t_check(t, horizon_gpu_succeeded(res),
            "uncached memory destroyed (status=%s)",
            horizon_gpu_status_str(res.status));

    horizon_gpu_mem *again = NULL;
    res = horizon_gpu_mem_create(dev, BUF_SIZE, 0, HORIZON_GPU_MEM_UNCACHED,
                                 &again);
    if (t_check(t, horizon_gpu_succeeded(res),
                "a second uncached allocation after the first was freed "
                "(status=%s nv=0x%08x)", horizon_gpu_status_str(res.status),
                res.nv)) {
        horizon_gpu_mem_destroy(again);
    }

    if (cached_ok)
        horizon_gpu_mem_destroy(cached);

    res = horizon_gpu_device_destroy(dev);
    t_check(t, horizon_gpu_succeeded(res),
            "device destroyed with no live objects (status=%s)",
            horizon_gpu_status_str(res.status));
    return 0;
}
