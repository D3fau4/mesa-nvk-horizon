/*
 * t_gpuwrite: does a GPU write to memory become visible to the CPU?
 *
 * Nothing below t_vulkan had ever asked this. t_submit proves a command
 * list executes, that fences order correctly, and even that a GPU-side
 * cross-channel syncpoint wait unblocks (R10) — but every one of those
 * observations is made through a *syncpoint*, which on Tegra is a host1x
 * counter, not memory. The first time this project asked memory a
 * question was t_vulkan's readback, and on 2026-08-04 that readback came
 * back holding every byte of its poison.
 *
 * The instrument is the channel's own semaphore release: a *host* method,
 * so no engine object has to be bound to any subchannel first, which
 * makes it the smallest possible GPU write to memory. Vulkan, Mesa and
 * NVK are not in the picture at all.
 *
 * WHAT THE FIRST RUN MEASURED (2026-08-04, hardware). The write did not
 * arrive, and it did not arrive identically on CPU-cached and on
 * CPU-uncached memory. That kills the cache-maintenance explanation: the
 * CPU-side policy is not the variable, so this revision stops varying it
 * and varies what is left instead.
 *
 * WHAT THE SECOND RUN MEASURED (2026-08-04, hardware). The matrix below
 * answered all four ways at once, and the cause is the GPU's own L2: a
 * GPU write lands there first, and nothing about a syncpoint increment
 * obliges that L2 to reach the memory the CPU reads.
 *
 *   A  L2-cacheable mapping, nothing else      FAILED, payload nowhere
 *   B  NON-cacheable mapping                   PASSED
 *   C  L2-cacheable + MEMBAR                   FAILED — a barrier is not enough
 *   D  L2-cacheable + L2_FLUSH_DIRTY           PASSED
 *
 * The fix went where the defect was: horizon_gpu_channel now begins every
 * submit's fence block with L2_FLUSH_DIRTY, in exactly the order arm D
 * measured. A fence whose writes are not visible is a fence that lies,
 * and this layer cannot know which submits a CPU will read from.
 *
 * SO WHAT THESE ARMS DO NOW. With that flush in place all four are
 * expected to pass, and the matrix has stopped diagnosing and started
 * guarding: arm A is the direct regression — an L2-cacheable mapping with
 * no flush of its own, which works only because the channel flushes — and
 * B, C and D check the fix composes with the other configurations rather
 * than depending on them.
 *
 * The arms can no longer prove *why* it works, only that it does; the
 * proof is the hardware run above. A failing arm still
 * scans the whole allocation for the payload and reports where it landed,
 * which separates "wrong offset" from "did not land" without a round trip.
 *
 * Each arm reads the target twice, before and after CPU cache
 * maintenance, and names which of four cases it saw:
 *
 *   before      after       what it means
 *   ---------------------------------------------------------------
 *   payload     payload     the GPU wrote; no maintenance was needed
 *   poison      payload     the GPU wrote; maintenance is REQUIRED and works
 *   poison      poison      the write never reached this memory
 *   payload     poison      the invalidate is destroying data
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <inttypes.h>

#include <switch.h>

#include "horizon_gpu/channel.h"
#include "horizon_gpu/cmds.h"
#include "horizon_gpu/device.h"
#include "horizon_gpu/memory.h"
#include "horizon_gpu/submit.h"
#include "horizon_gpu/sync.h"
#include "horizon_gpu/vm.h"
#include "common/testfw.h"

const char *const test_name = "t_gpuwrite";
/* No display: main() starts a console and reports through it. */
const bool test_uses_display = false;

#define WAIT_NS   UINT64_C(2000000000) /* 2 s bound for every wait here */
#define ALLOC_B   UINT64_C(0x1000)
#define PAYLOAD   UINT32_C(0xc0ffee01)
/* Dwords watched after the target. A 4-byte release must leave all of
 * them poisoned; a 16-byte release would write a timestamp over the next
 * three, so a wrong RELEASE_SIZE is reported rather than mistaken for
 * something else. */
#define NEIGHBOURS 3u
/* No extra barrier or flush in this arm. MEM_OP operation codes are
 * 5-bit and 0 is not one of them (clb06f.h:130-136), so it is free to
 * mean "none" here. */
#define NO_MEM_OP  UINT32_C(0)

typedef struct target {
    horizon_gpu_mem *mem;
    horizon_gpu_va_range *range;
    horizon_gpu_mapping *map;
    uint64_t gpu_va;
    uint32_t *cpu;
} target;

static horizon_gpu_result target_create(horizon_gpu_device *dev,
                                        bool gpu_cacheable, target *out)
{
    /* CPU-side policy is fixed at CACHED: the first hardware run showed
     * cached and uncached failing identically, so it is not the
     * variable. */
    horizon_gpu_result res =
        horizon_gpu_mem_create(dev, ALLOC_B, 0, HORIZON_GPU_MEM_CACHED,
                               &out->mem);
    if (horizon_gpu_failed(res))
        return res;
    res = horizon_gpu_vm_reserve(dev, 0x10000, 0x1000, 0, &out->range);
    if (horizon_gpu_failed(res))
        return res;
    res = horizon_gpu_vm_map(out->range, 0, out->mem, 0, ALLOC_B,
                             HORIZON_GPU_PTE_KIND_PITCH, gpu_cacheable,
                             &out->map);
    if (horizon_gpu_failed(res))
        return res;
    out->gpu_va = horizon_gpu_mapping_va(out->map);
    out->cpu = horizon_gpu_mem_cpu_ptr(out->mem);
    return horizon_gpu_ok();
}

static void target_destroy(target *tg)
{
    if (tg->map != NULL)
        horizon_gpu_vm_unmap(tg->map);
    if (tg->range != NULL)
        horizon_gpu_vm_release(tg->range);
    if (tg->mem != NULL)
        horizon_gpu_mem_destroy(tg->mem);
    tg->map = NULL;
    tg->range = NULL;
    tg->mem = NULL;
}

/* One arm of the matrix. */
static void run_arm(test_ctx *t, horizon_gpu_device *dev,
                    horizon_gpu_channel *chan, bool gpu_cacheable,
                    uint32_t mem_op, const char *what)
{
    target tg = { 0 };
    target cmd = { 0 };

    horizon_gpu_result res = target_create(dev, gpu_cacheable, &tg);
    if (!t_check(t, horizon_gpu_succeeded(res),
                 "%s: target allocated and mapped (status=%s nv=0x%08x)",
                 what, horizon_gpu_status_str(res.status), res.nv))
        goto out;

    /* The command list is always L2-cacheable: it is CPU-written and
     * GPU-*read*, the direction t_submit already proves works. Only the
     * target's mapping is under test. */
    res = target_create(dev, true, &cmd);
    if (!t_check(t, horizon_gpu_succeeded(res),
                 "%s: command list allocated and mapped (status=%s)",
                 what, horizon_gpu_status_str(res.status)))
        goto out;

    t_note(t, "%s: target gpu_va=0x%" PRIx64 " cpu=%p gpu_cacheable=%d "
              "mem_op=0x%02" PRIx32, what, tg.gpu_va, (void *)tg.cpu,
           (int)gpu_cacheable, mem_op);

    /* Poison the whole allocation, so the scan below can tell "landed
     * elsewhere in this object" from "did not land". Then push it out of
     * the CPU cache: a dirty line evicted *after* the GPU's write would
     * overwrite the result, and the test would report a GPU failure that
     * was really its own. */
    for (uint32_t i = 0; i < ALLOC_B / 4; i++)
        tg.cpu[i] = ~PAYLOAD;
    res = horizon_gpu_mem_flush(tg.mem, 0, ALLOC_B);
    if (!t_check(t, horizon_gpu_succeeded(res),
                 "%s: poison flushed out of the CPU cache (status=%s)",
                 what, horizon_gpu_status_str(res.status)))
        goto out;

    /* semaphore release -> target, optional barrier/flush, fence. */
    uint32_t *dw = cmd.cpu;
    uint32_t n = horizon_cmds_semaphore_release(dw, tg.gpu_va, PAYLOAD);
    if (!t_check(t, n == HORIZON_CMDS_SEM_RELEASE_DWORDS,
                 "%s: semaphore release encoded for 0x%" PRIx64 " (%u dwords)",
                 what, tg.gpu_va, n))
        goto out;
    if (mem_op != NO_MEM_OP) {
        uint32_t nm = horizon_cmds_mem_op(dw + n, mem_op);
        if (!t_check(t, nm == HORIZON_CMDS_MEM_OP_DWORDS,
                     "%s: MEM_OP 0x%02" PRIx32 " encoded (%u dwords)",
                     what, mem_op, nm))
            goto out;
        n += nm;
    }
    /* No fence increment here on purpose. horizon_gpu_submit appends the
     * channel's own fence block to every submit and advances
     * shadow_target by exactly one; a second increment in this span
     * would advance the *hardware* counter twice per submit while the
     * accounting moved once, leaving hardware permanently one ahead.
     *
     * The failure that causes is silent and it is this test's own
     * measurement that it destroys: from the second arm onwards the
     * threshold handed to wait_fence has already been passed by the
     * previous arm's extra increment, so the wait returns before this
     * arm's release has run, and the readback below races the GPU
     * instead of following it. The first arm is unaffected — which is
     * exactly why the run still looked plausible.
     */
    res = horizon_gpu_mem_flush(cmd.mem, 0, n * 4);
    if (!t_check(t, horizon_gpu_succeeded(res),
                 "%s: command list flushed (status=%s)",
                 what, horizon_gpu_status_str(res.status)))
        goto out;

    const horizon_gpu_cmd_span span = {
        .gpu_va = cmd.gpu_va,
        .num_dwords = n,
    };
    horizon_gpu_fence fence;
    res = horizon_gpu_submit(chan, &span, 1, HORIZON_GPU_SUBMIT_DEFAULT,
                             &fence);
    if (!t_check(t, horizon_gpu_succeeded(res),
                 "%s: submit accepted (status=%s nv=0x%08x)",
                 what, horizon_gpu_status_str(res.status), res.nv))
        goto out;

    res = horizon_gpu_channel_wait_fence(chan, fence, WAIT_NS);
    if (!t_check(t, horizon_gpu_succeeded(res),
                 "%s: fence reached, so the release executed (status=%s)",
                 what, horizon_gpu_status_str(res.status)))
        goto out;

    /* Reading 1: no CPU cache maintenance at all. */
    const uint32_t before = tg.cpu[0];

    res = horizon_gpu_mem_invalidate(tg.mem, 0, ALLOC_B);
    if (!t_check(t, horizon_gpu_succeeded(res),
                 "%s: invalidate (status=%s)", what,
                 horizon_gpu_status_str(res.status)))
        goto out;

    /* Reading 2: after it. */
    const uint32_t after = tg.cpu[0];

    t_note(t, "%s: target[0] before invalidate = 0x%08" PRIx32
              ", after = 0x%08" PRIx32, what, before, after);

    if (after == PAYLOAD && before == PAYLOAD) {
        t_note(t, "%s: VERDICT the GPU wrote, and this mapping needed no "
                  "cache maintenance to see it", what);
    } else if (after == PAYLOAD) {
        t_note(t, "%s: VERDICT the GPU wrote, and CPU cache maintenance is "
                  "REQUIRED here and works", what);
    } else if (before == PAYLOAD) {
        t_note(t, "%s: VERDICT the invalidate DESTROYED the result — it is "
                  "writing a stale line back over it", what);
    } else {
        t_note(t, "%s: VERDICT the write never reached this memory", what);
        /* Did it land somewhere else in the same object? That would be a
         * mapping offset defect rather than an L2 one, and it is the
         * question the arms cannot answer between them. */
        uint32_t found = 0;
        for (uint32_t i = 0; i < ALLOC_B / 4; i++) {
            if (tg.cpu[i] == PAYLOAD) {
                t_note(t, "%s: the payload IS present at byte offset 0x%x — "
                          "the write landed, at the wrong offset",
                       what, i * 4u);
                found++;
                if (found == 4)
                    break;
            }
        }
        if (found == 0)
            t_note(t, "%s: the payload appears nowhere in the %" PRIu64
                      "-byte allocation", what, ALLOC_B);
    }

    t_check(t, after == PAYLOAD,
            "%s: the CPU reads what the GPU wrote (0x%08" PRIx32 ")",
            what, after);

    /* A 4-byte release must not have touched anything after the target. */
    bool neighbours_intact = true;
    for (uint32_t i = 1; i <= NEIGHBOURS; i++) {
        if (tg.cpu[i] != ~PAYLOAD) {
            neighbours_intact = false;
            t_note(t, "%s: target[%u] = 0x%08" PRIx32 ", expected the poison "
                      "0x%08" PRIx32 " — a 16-byte release would write a "
                      "timestamp here", what, i, tg.cpu[i], ~PAYLOAD);
        }
    }
    t_check(t, neighbours_intact,
            "%s: the release wrote 4 bytes and disturbed nothing after them",
            what);

out:
    target_destroy(&cmd);
    target_destroy(&tg);
}

int run_test(test_ctx *t)
{
    horizon_gpu_device *dev = NULL;
    horizon_gpu_result res = horizon_gpu_device_create(NULL, &dev);
    if (!t_check(t, horizon_gpu_succeeded(res),
                 "device_create (status=%s nv=0x%08x)",
                 horizon_gpu_status_str(res.status), res.nv))
        return 1;

    horizon_gpu_channel *chan = NULL;
    res = horizon_gpu_channel_create(dev, NULL, &chan);
    if (!t_check(t, horizon_gpu_succeeded(res),
                 "channel_create (status=%s nv=0x%08x)",
                 horizon_gpu_status_str(res.status), res.nv)) {
        horizon_gpu_device_destroy(dev);
        return 1;
    }

    t_note(t, "poison 0x%08" PRIx32 ", payload 0x%08" PRIx32,
           ~PAYLOAD, PAYLOAD);

    /* A: the failing baseline, reproduced so the matrix has a control
     * measured in the same run rather than remembered from the last. */
    run_arm(t, dev, chan, true,  NO_MEM_OP, "A/cacheable");
    /* B: same write, mapping not L2-cacheable. */
    run_arm(t, dev, chan, false, NO_MEM_OP, "B/noncacheable");
    /* C: cacheable, with a memory barrier before the fence. */
    run_arm(t, dev, chan, true,  HORIZON_MEM_OP_MEMBAR, "C/membar");
    /* D: cacheable, with an explicit dirty-L2 writeback before the
     * fence. This is the fix to want if L2 is the cause — it keeps GPU
     * caching on and pays only where a CPU is going to read. */
    run_arm(t, dev, chan, true,  HORIZON_MEM_OP_L2_FLUSH_DIRTY, "D/l2flush");

    res = horizon_gpu_channel_wait_idle(chan, WAIT_NS);
    t_check(t, horizon_gpu_succeeded(res), "wait_idle before teardown");
    res = horizon_gpu_channel_destroy(chan);
    t_check(t, horizon_gpu_succeeded(res), "channel_destroy (status=%s)",
            horizon_gpu_status_str(res.status));
    res = horizon_gpu_device_destroy(dev);
    t_check(t, horizon_gpu_succeeded(res), "device_destroy (status=%s)",
            horizon_gpu_status_str(res.status));
    return 0;
}
