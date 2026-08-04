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
 * That failure has at least three candidate causes and the Vulkan test
 * cannot separate them: the GPU may not have written, it may have
 * written somewhere else, or it may have written correctly and the CPU
 * may be reading a stale cache line. This test removes Vulkan, Mesa and
 * NVK from the picture entirely and asks the bottom layer directly.
 *
 * The instrument is the channel's own semaphore release: a *host* method,
 * so no engine object has to be bound to any subchannel first, which
 * makes it the smallest possible GPU write to memory. If this fails,
 * everything above it is chasing a ghost.
 *
 * The experiment is controlled rather than pass/fail. The target is
 * poisoned with the complement of the payload and read twice — once
 * before any cache maintenance and once after — and the pair of readings
 * is what identifies the cause:
 *
 *   before      after       what it means
 *   ---------------------------------------------------------------
 *   payload     payload     the GPU wrote; this mapping needed no
 *                           maintenance to see it
 *   poison      payload     the GPU wrote; cache maintenance is
 *                           REQUIRED here and it WORKS
 *   poison      poison      the write never reached this memory
 *   payload     poison      the invalidate is destroying data
 *
 * and the whole thing runs twice, once on cached memory and once on
 * uncached, because "cached fails, uncached works" is a different defect
 * from "both fail" and points at a different fix.
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

#define WAIT_NS   UINT64_C(2000000000) /* 2 s bound for every wait here */
#define ALLOC_B   UINT64_C(0x1000)
#define PAYLOAD   UINT32_C(0xc0ffee01)
/* Dwords watched after the target. A 4-byte release must leave all of
 * them poisoned; a 16-byte release would write a timestamp over the next
 * three, so a wrong RELEASE_SIZE is reported rather than mistaken for
 * something else. */
#define NEIGHBOURS 3u

typedef struct target {
    horizon_gpu_mem *mem;
    horizon_gpu_va_range *range;
    horizon_gpu_mapping *map;
    uint64_t gpu_va;
    uint32_t *cpu;
} target;

static horizon_gpu_result target_create(horizon_gpu_device *dev,
                                        horizon_gpu_cache_policy policy,
                                        target *out)
{
    horizon_gpu_result res =
        horizon_gpu_mem_create(dev, ALLOC_B, 0, policy, &out->mem);
    if (horizon_gpu_failed(res))
        return res;
    res = horizon_gpu_vm_reserve(dev, 0x10000, 0x1000, 0, &out->range);
    if (horizon_gpu_failed(res))
        return res;
    res = horizon_gpu_vm_map(out->range, 0, out->mem, 0, ALLOC_B,
                             HORIZON_GPU_PTE_KIND_PITCH, true, &out->map);
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

/* One full experiment on one cache policy. */
static void run_policy(test_ctx *t, horizon_gpu_device *dev,
                       horizon_gpu_channel *chan,
                       horizon_gpu_cache_policy policy, const char *what)
{
    target tg = { 0 };
    target cmd = { 0 };

    horizon_gpu_result res = target_create(dev, policy, &tg);
    if (!t_check(t, horizon_gpu_succeeded(res),
                 "%s: target allocated and mapped (status=%s nv=0x%08x)",
                 what, horizon_gpu_status_str(res.status), res.nv))
        goto out;

    /* The command list is always cached: it is CPU-written and
     * GPU-*read*, which is the direction t_submit already exercises. Only
     * the target's policy is under test here. */
    res = target_create(dev, HORIZON_GPU_MEM_CACHED, &cmd);
    if (!t_check(t, horizon_gpu_succeeded(res),
                 "%s: command list allocated and mapped (status=%s)",
                 what, horizon_gpu_status_str(res.status)))
        goto out;

    t_note(t, "%s: target gpu_va=0x%" PRIx64 " cpu=%p, cmdlist gpu_va=0x%"
              PRIx64, what, tg.gpu_va, (void *)tg.cpu, cmd.gpu_va);

    /* Poison, then push it out of the CPU cache. This is not a
     * formality: a dirty line evicted *after* the GPU's write would
     * overwrite the result with the poison, and the test would report a
     * GPU failure that was really its own. */
    for (uint32_t i = 0; i < 1u + NEIGHBOURS; i++)
        tg.cpu[i] = ~PAYLOAD;
    res = horizon_gpu_mem_flush(tg.mem, 0, ALLOC_B);
    if (!t_check(t, horizon_gpu_succeeded(res),
                 "%s: poison flushed out of the CPU cache (status=%s)",
                 what, horizon_gpu_status_str(res.status)))
        goto out;

    /* semaphore release -> target, then the fence increment. */
    uint32_t *dw = cmd.cpu;
    uint32_t n = horizon_cmds_semaphore_release(dw, tg.gpu_va, PAYLOAD);
    if (!t_check(t, n == HORIZON_CMDS_SEM_RELEASE_DWORDS,
                 "%s: semaphore release encoded for 0x%" PRIx64 " (%u dwords)",
                 what, tg.gpu_va, n))
        goto out;
    uint32_t nf = horizon_cmds_fence_incr(dw + n,
                                          horizon_gpu_channel_syncpt_id(chan));
    if (!t_check(t, nf == HORIZON_CMDS_FENCE_INCR_DWORDS,
                 "%s: fence increment encoded (%u dwords)", what, nf))
        goto out;
    res = horizon_gpu_mem_flush(cmd.mem, 0, (n + nf) * 4);
    if (!t_check(t, horizon_gpu_succeeded(res),
                 "%s: command list flushed (status=%s)",
                 what, horizon_gpu_status_str(res.status)))
        goto out;

    const horizon_gpu_cmd_span span = {
        .gpu_va = cmd.gpu_va,
        .num_dwords = n + nf,
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

    /* Reading 1: no cache maintenance at all. */
    const uint32_t before = tg.cpu[0];

    res = horizon_gpu_mem_invalidate(tg.mem, 0, ALLOC_B);
    if (!t_check(t, horizon_gpu_succeeded(res),
                 "%s: invalidate (status=%s)", what,
                 horizon_gpu_status_str(res.status)))
        goto out;

    /* Reading 2: after it. */
    const uint32_t after = tg.cpu[0];

    t_note(t, "%s: target[0] before invalidate = 0x%08" PRIx32
              ", after = 0x%08" PRIx32 " (payload 0x%08" PRIx32
              ", poison 0x%08" PRIx32 ")",
           what, before, after, PAYLOAD, ~PAYLOAD);

    /* The verdict is the pair, so say which of the four cases this is
     * rather than only whether it passed. */
    if (after == PAYLOAD && before == PAYLOAD) {
        t_note(t, "%s: VERDICT the GPU wrote, and this mapping needed no "
                  "cache maintenance to see it", what);
    } else if (after == PAYLOAD) {
        t_note(t, "%s: VERDICT the GPU wrote, and cache maintenance is "
                  "REQUIRED here and works", what);
    } else if (before == PAYLOAD) {
        t_note(t, "%s: VERDICT the invalidate DESTROYED the result — it is "
                  "writing a stale line back over it", what);
    } else {
        t_note(t, "%s: VERDICT the write never reached this memory", what);
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

    /* Cached first: it is what every ordinary allocation uses, and what
     * t_vulkan's failing readback was made of. */
    run_policy(t, dev, chan, HORIZON_GPU_MEM_CACHED, "cached");
    /* Then uncached, because "cached fails, uncached works" is a
     * different defect from "both fail" and has a different fix. */
    run_policy(t, dev, chan, HORIZON_GPU_MEM_UNCACHED, "uncached");

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
