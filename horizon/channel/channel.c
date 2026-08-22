/*
 * horizon_gpu — GPFIFO channel implementation.
 *
 * Creation order (torn down in reverse on every error path):
 *   nvGpuChannelCreate -> syncpoint identity + shadow init -> internal
 *   command buffer (mem, reservation, mapping, fence-increment list) ->
 *   optional Zcull (mem, mapping, bind).
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>

#include "channel_priv.h"
#include "horizon_gpu/submit.h"
#include "../device/device_priv.h"
#include "../memory/mem_priv.h"
#include "../memory/align.h"
#include "horizon_gpu/cmds.h"
#include "../sync/syncpt_math.h"

/* Internal command buffer layout (byte offsets). */
#define CHANNEL_CMDBUF_SIZE        UINT64_C(0x1000)
#define CHANNEL_FENCE_CMDS_OFFSET  UINT64_C(0x000)
#define CHANNEL_SETOBJ_CMDS_OFFSET UINT64_C(0x100)
#define CHANNEL_PROLOGUE_CMDS_OFFSET UINT64_C(0x200)

/* Zcull context buffer VA alignment. Source: the reference's
 * hardware-tested channel bring-up (reference-analysis § 4: Zcull BO
 * aligned 0x20000). */
#define CHANNEL_ZCULL_ALIGN UINT64_C(0x20000)

/* The fence-increment list and the SET_OBJECT list must not overlap
 * inside the shared cmdbuf page; catch it at compile time rather than as
 * a GPU fault the first time either list's size grows. */
_Static_assert(HORIZON_CMDS_FENCE_INCR_DWORDS * 4 <=
               CHANNEL_SETOBJ_CMDS_OFFSET - CHANNEL_FENCE_CMDS_OFFSET,
               "fence-increment list would overrun the SET_OBJECT list");
_Static_assert(CHANNEL_SETOBJ_CMDS_OFFSET +
               HORIZON_CMDS_SET_OBJECTS_DWORDS * 4 <=
               CHANNEL_PROLOGUE_CMDS_OFFSET,
               "SET_OBJECT list would overrun the prologue list");
_Static_assert(CHANNEL_PROLOGUE_CMDS_OFFSET +
               HORIZON_CMDS_MEM_OP_DWORDS * 4 <= CHANNEL_CMDBUF_SIZE,
               "prologue list would overrun the cmdbuf page");
_Static_assert(CHANNEL_PROLOGUE_CMDS_OFFSET +
               HORIZON_CMDS_MEM_OP_DWORDS * 4 <= HORIZON_CHANNEL_WAIT_CMDS_OFFSET,
               "prologue list would overrun the wait ring");
/* The wait ring takes what is left of the page, and how much that is has
 * to be checked rather than believed: the slot count and the per-slot
 * size are two constants in another header, and either growing would
 * otherwise put GPU-fetched command lists on top of each other. */
_Static_assert(HORIZON_CHANNEL_WAIT_CMDS_OFFSET +
               (uint64_t)HORIZON_CHANNEL_WAIT_SLOTS *
               HORIZON_CHANNEL_WAIT_SLOT_DWORDS * 4 <= CHANNEL_CMDBUF_SIZE,
               "wait ring would overrun the cmdbuf page");

/* Wait loop chunk: 100 ms per kernel wait so the error notifier is
 * re-checked at a useful rate without busy-polling
 * (docs/synchronization.md § 6). */
#define CHANNEL_WAIT_CHUNK_US INT32_C(100000)

horizon_gpu_result horizon_channel_read_syncpt(horizon_gpu_channel *chan,
                                               uint32_t *out_hw)
{
    /* /dev/nvhost-ctrl fd owned by libnx's fence module (open since
     * device bring-up step 2). */
    Result rc = nvioctlNvhostCtrl_SyncptRead(nvFenceGetFd(),
                                             chan->syncpt_id, out_hw);
    if (R_FAILED(rc))
        return horizon_gpu_err_nv(rc);
    return horizon_gpu_ok();
}

static const char *channel_error_desc(uint32_t type)
{
    switch (type) {
    case 0:                                        return "none";
    case NvNotificationType_FifoErrorIdleTimeout:  return "fifo idle timeout";
    case NvNotificationType_GrErrorSwNotify:       return "graphics sw notify";
    case NvNotificationType_GrSemaphoreTimeout:    return "graphics semaphore timeout";
    case NvNotificationType_GrIllegalNotify:       return "illegal method";
    case NvNotificationType_FifoErrorMmuErrFlt:    return "MMU fault";
    case NvNotificationType_PbdmaError:            return "PBDMA error";
    case NvNotificationType_ResetChannelVerifError:return "channel reset (verif error)";
    case NvNotificationType_PbdmaPushbufferCrcMismatch:
        return "PBDMA pushbuffer CRC mismatch";
    default:                                       return "unknown notification";
    }
}

horizon_gpu_result
horizon_gpu_channel_get_error(horizon_gpu_channel *chan, uint32_t *out_type,
                              const char **out_desc)
{
    if (!chan || !out_type)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);

    NvNotification notif;
    memset(&notif, 0, sizeof(notif));
    Result rc = nvGpuChannelGetErrorNotification(&chan->gc, &notif);
    if (R_FAILED(rc)) {
        /* libnx's nvGpuChannelGetErrorNotification (nx/source/nvidia/
         * gpu_channel.c) does a non-blocking eventWait(..., 0) on the
         * channel's error event before the ioctl; "no notification
         * pending" surfaces as that wait's own KERNELRESULT(TimedOut),
         * not an nv-service error. Only that specific result means
         * "none" — anything else (a real ioctl/service failure) must be
         * reported, not swallowed as a false-healthy channel. */
        if (rc == KERNELRESULT(TimedOut)) {
            /* Nothing pending *now*. That is not the same as nothing
             * ever, because reading a notification consumes it: the
             * wait path's own check is usually the reader, and it
             * leaves nothing for the caller who then asks why the
             * channel is lost. Answer from the latch when there is one
             * (see last_error_type in channel_priv.h, and t_fault). */
            *out_type = chan->last_error_type;
            if (out_desc)
                *out_desc = channel_error_desc(*out_type);
            return horizon_gpu_ok();
        }
        horizon_logf(&chan->dev->log, HORIZON_LOG_ERROR,
                     "GetErrorNotification failed: 0x%08x", rc);
        return horizon_gpu_err_nv(rc);
    }

    /* A notification with a zero timestamp has never fired — and that
     * is the same "nothing pending now" the TimedOut branch above
     * reports, so it gets the same answer. The first version latched
     * only in the other branch, which left this one telling a lost
     * channel it had no error; found in review of PR #7. */
    const uint32_t live = (notif.timestamp != 0) ? notif.info32 : 0;
    if (live != 0)
        chan->last_error_type = live;
    *out_type = (live != 0) ? live : chan->last_error_type;
    if (out_desc)
        *out_desc = channel_error_desc(*out_type);
    return horizon_gpu_ok();
}

/* Checks the notifier; marks the channel lost when it reports an error. */
static bool channel_check_fault(horizon_gpu_channel *chan)
{
    uint32_t type = 0;
    const char *desc = NULL;
    horizon_gpu_result res = horizon_gpu_channel_get_error(chan, &type,
                                                           &desc);
    if (horizon_gpu_succeeded(res) && type != 0) {
        if (!chan->lost)
            horizon_logf(&chan->dev->log, HORIZON_LOG_ERROR,
                         "channel %p: fault notification %u (%s) — marking "
                         "lost", (void *)chan, type, desc);
        chan->lost = true;
    }
    return chan->lost;
}

/* "The fence says reached" is the answer a waiter wants, and on this
 * platform it is not sufficient on its own.
 *
 * MEASURED ON HARDWARE, 2026-08-04 (t_vk_image run 1). A render pass
 * MMU-faulted, nothing it was supposed to write was written, and
 * vkWaitForFences returned VK_SUCCESS. The syncpoint had reached the
 * threshold — because nvgpu's channel recovery force-increments a
 * faulted channel's syncpoints to their maximum submitted value, so
 * that waiters do not hang forever. The counter therefore says
 * "finished" for work that never ran, and the CPU then read a buffer
 * still holding its poison and had no way to know why. The fault only
 * surfaced at the *next* kickoff, one test case later, by which time
 * the case that caused it had already been reported as passing.
 *
 * So the notifier is consulted on the success path too, and it decides.
 * A reached threshold plus a fault notification is CHANNEL_LOST, not OK.
 *
 * This is deliberately stricter than "the fence completed before the
 * fault": the notifier is sticky and per-channel, so work that genuinely
 * finished before a later fault will also be reported lost. That is the
 * safe direction. The unsafe one is what was measured — reporting
 * success for work that did not happen — and no caller of this function
 * can tell the two apart from the outside. */
static horizon_gpu_result channel_reached_or_lost(horizon_gpu_channel *chan)
{
    if (channel_check_fault(chan))
        return horizon_gpu_err(HORIZON_GPU_ERR_CHANNEL_LOST);
    return horizon_gpu_ok();
}

/* Logs a teardown step's failure during horizon_gpu_channel_create's error
 * unwind instead of discarding it. A failure here means the corresponding
 * object (and its live_* counter) cannot be un-wound any further from this
 * function — chan is never published through *out_chan, so nothing else can
 * reference it, but the leak must be visible rather than silent
 * (CLAUDE.md: never discard a Result; never silently leak). */
static void channel_create_unwind_step(horizon_gpu_device *dev,
                                       const char *step,
                                       horizon_gpu_result res)
{
    if (horizon_gpu_failed(res))
        horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                     "channel_create unwind: %s failed (status=%s "
                     "nv=0x%08x) — leaking that object", step,
                     horizon_gpu_status_str(res.status), res.nv);
}

horizon_gpu_result
horizon_gpu_channel_create(horizon_gpu_device *dev,
                           const horizon_gpu_channel_create_info *create_info,
                           horizon_gpu_channel **out_chan)
{
    if (!dev || !out_chan)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);

    horizon_gpu_channel_create_info defaults = {
        .prio = HORIZON_GPU_CHANNEL_PRIO_MEDIUM,
        .bind_zcull = false,
    };
    if (!create_info)
        create_info = &defaults;

    NvChannelPriority prio;
    switch (create_info->prio) {
    case HORIZON_GPU_CHANNEL_PRIO_LOW:    prio = NvChannelPriority_Low;    break;
    case HORIZON_GPU_CHANNEL_PRIO_MEDIUM: prio = NvChannelPriority_Medium; break;
    case HORIZON_GPU_CHANNEL_PRIO_HIGH:   prio = NvChannelPriority_High;   break;
    default:
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);
    }

    horizon_gpu_channel *chan = calloc(1, sizeof(*chan));
    if (!chan)
        return horizon_gpu_err(HORIZON_GPU_ERR_OUT_OF_MEMORY);
    chan->dev = dev;

    horizon_gpu_result res;

    /* libnx allocates the GPFIFO and the 3D object context here; calling
     * AllocObjCtx again would be rejected (known-risks R7). */
    Result rc = nvGpuChannelCreate(&chan->gc, &dev->as, prio);
    if (R_FAILED(rc)) {
        horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                     "nvGpuChannelCreate failed: 0x%08x", rc);
        res = horizon_gpu_err_nv(rc);
        goto fail_free;
    }

    chan->syncpt_id = nvGpuChannelGetSyncpointId(&chan->gc);

    /* Logged at creation, not only on failure. The fourth hardware run
     * of the Phase 4 sequence failed here with syncpoint id 1, where
     * t_channel had reported 26 on the same console — so the channel
     * came back from libnx reporting success without a real syncpoint.
     * NVK creates two or three channels during vkCreateDevice (upload
     * queue, exec, and bind when the queue family advertises sparse),
     * and one line per creation is what says which one degrades. Cheap,
     * and the alternative is another console round trip per guess. */
    horizon_logf(&dev->log, HORIZON_LOG_INFO,
                 "channel %p: created, syncpt id=%u (live channels now %u)",
                 (void *)chan, chan->syncpt_id,
                 atomic_load(&dev->live_channels) + 1u);

    /* Shadow initialisation from the observed hardware value; whether
     * Horizon resets the counter at channel creation is the R5 open
     * question — t_channel reports this number. */
    res = horizon_channel_read_syncpt(chan, &chan->syncpt_value_at_create);
    if (horizon_gpu_failed(res)) {
        horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                     "initial SyncptRead(%u) failed: 0x%08x",
                     chan->syncpt_id, res.nv);
        if (!dev->allow_untrusted_syncpt_baseline)
            goto fail_channel;
        /* Opt-in path only (docs/synchronization.md § 9). The baseline is
         * whatever calloc left — zero — and it is recorded as untrusted so
         * no caller can mistake this channel's fences for measurements. */
        chan->syncpt_value_at_create = 0;
        atomic_store(&dev->untrusted_syncpt_seen, true);
        horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                     "channel %p: continuing with an UNTRUSTED syncpoint "
                     "baseline of 0 (opt-in); its fences are not "
                     "measurements", (void *)chan);
    } else {
        chan->syncpt_baseline_trusted = true;
    }
    chan->shadow_target = chan->syncpt_value_at_create;

    /* Internal command buffer. */
    res = horizon_gpu_mem_create(dev, CHANNEL_CMDBUF_SIZE, 0,
                                 HORIZON_GPU_MEM_CACHED, &chan->cmdbuf_mem);
    if (horizon_gpu_failed(res))
        goto fail_channel;

    /* Without Zcull, the reservation only needs to hold the cmdbuf page,
     * at ordinary small-page alignment. With Zcull, one reservation holds
     * both: cmdbuf at base+0, Zcull at base+0x20000, with the base
     * aligned so the Zcull VA meets CHANNEL_ZCULL_ALIGN. Sizing/aligning
     * every channel's reservation to CHANNEL_ZCULL_ALIGN regardless would
     * reserve 128 KiB of GPU VA for a 4 KiB cmdbuf in the common
     * (bind_zcull == false) case. */
    uint64_t zcull_size = 0;
    uint64_t reserve_size = CHANNEL_CMDBUF_SIZE;
    uint64_t reserve_align = 0; /* 0 = vm_reserve defaults to page_size */
    if (create_info->bind_zcull) {
        zcull_size = nvGpuGetZcullCtxSize();
        if (zcull_size == 0) {
            horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                         "Zcull requested but nvGpuGetZcullCtxSize() == 0");
            res = horizon_gpu_err(HORIZON_GPU_ERR_NV);
            goto fail_cmdbuf_mem;
        }
        uint64_t zcull_rounded;
        if (!horizon_align_up_u64(zcull_size, CHANNEL_ZCULL_ALIGN,
                                  &zcull_rounded) ||
            !horizon_add_u64(CHANNEL_ZCULL_ALIGN, zcull_rounded,
                             &reserve_size)) {
            res = horizon_gpu_err(HORIZON_GPU_ERR_OVERFLOW);
            goto fail_cmdbuf_mem;
        }
        reserve_align = CHANNEL_ZCULL_ALIGN;
    }

    res = horizon_gpu_vm_reserve(dev, reserve_size,
                                 (uint32_t)HORIZON_GPU_SMALL_PAGE_SIZE,
                                 reserve_align, &chan->internal_range);
    if (horizon_gpu_failed(res))
        goto fail_cmdbuf_mem;

    res = horizon_gpu_vm_map(chan->internal_range, 0, chan->cmdbuf_mem, 0,
                             CHANNEL_CMDBUF_SIZE, HORIZON_GPU_PTE_KIND_PITCH,
                             true, &chan->cmdbuf_map);
    if (horizon_gpu_failed(res))
        goto fail_range;

    /* Write the per-submit fence block once; its content only depends on
     * the syncpoint id.
     *
     * horizon_cmds_fence_incr emits wait-for-idle (SCOPE_ALL), then a
     * dirty-L2 writeback, then the increment — see its comment for the
     * hardware measurement that put the flush there and for why the order
     * is load-bearing. The consequence here is the one that matters: on
     * this channel a fence means the work is done *and* its writes are
     * visible to a CPU, which is the only thing a fence can usefully
     * mean.
     */
    uint32_t *cmds = horizon_gpu_mem_cpu_ptr(chan->cmdbuf_mem);
    chan->fence_cmds_dwords =
        horizon_cmds_fence_incr(cmds + CHANNEL_FENCE_CMDS_OFFSET / 4,
                                chan->syncpt_id);
    if (chan->fence_cmds_dwords == 0) {
        horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                     "syncpoint id %u out of encoding range",
                     chan->syncpt_id);
        res = horizon_gpu_err(HORIZON_GPU_ERR_NV);
        goto fail_cmdbuf_map;
    }
    /* And the per-submit PROLOGUE, which runs before the caller's work:
     * one MEM_OP, L2_SYSMEM_INVALIDATE.
     *
     * WHY IT EXISTS, MEASURED ON HARDWARE 2026-08-04
     * (docs/hw-logs/t_vk_transfer-run3-FAIL.log). The fence block above
     * writes dirty L2 back after the work, so a GPU write reaches memory.
     * Nothing did the other direction: a line the GPU has touched stays
     * resident in L2 after that writeback, *clean*, and a later CPU write
     * to the same address updates memory and leaves that line alone. The
     * GPU then reads its own stale copy.
     *
     * It bites hardest where a GPU write covers only part of an L2 line.
     * The engine merges into the resident stale line instead of fetching
     * memory, the writeback then sends the whole line back, and the bytes
     * the caller never asked to be written are silently reverted to what
     * the GPU last saw there. That is what t_vk_transfer measured, and
     * the numbers are exact:
     *
     *   copy [0, 4)  after a copy that had touched [0, 32):
     *                bytes [4, 32) came back holding the *previous*
     *                copy's data — 28 bytes, one line minus the four
     *                that were written
     *   copy [0, 12) likewise: 20 bytes, again to the 32-byte boundary
     *   copy [4, 260), [8, 264), [16, 272): 4, 8 and 16 bytes before the
     *                region, each exactly the distance back to a
     *                32-byte boundary
     *   copy [32, 288), [64, 324), [1028, 3480): exact, because either
     *                both ends are 32-byte aligned or the partial line
     *                was not resident from an earlier submit
     *
     * Eleven data points, one model, and it puts this GPU's L2 line at 32
     * bytes. The same measurement is what withdrew the earlier reading of
     * this data as a "copy engine transfer granularity": the copy engine
     * is exact, and the staleness is ours.
     *
     * Cost: one MEM_OP per submit, which is the same shape and order of
     * cost as the writeback that has always been there. It is
     * unconditional for the same reason the writeback is — this layer
     * does not know what the caller's memory has been used for, and a
     * conditional invalidate would be a guess with silent corruption as
     * its failure mode.
     */
    chan->prologue_cmds_dwords =
        horizon_cmds_mem_op(cmds + CHANNEL_PROLOGUE_CMDS_OFFSET / 4,
                            HORIZON_MEM_OP_L2_SYSMEM_INVALIDATE);
    if (chan->prologue_cmds_dwords == 0) {
        horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                     "the L2 invalidate prologue could not be encoded");
        res = horizon_gpu_err(HORIZON_GPU_ERR_STATE);
        goto fail_cmdbuf_map;
    }

    /* One flush covering both blocks: they share the page, and the
     * cmdbuf is CPU-cached like everything else here.
     *
     * The third argument is a LENGTH. It was written as
     * CHANNEL_PROLOGUE_CMDS_OFFSET + n*4, which is a length only
     * because the fence block starts at zero — correct by coincidence,
     * and silently short if the fence block ever moved. Every other
     * layout relationship in this file is pinned by a _Static_assert;
     * this one was not. Found in review of PR #7. */
    _Static_assert(CHANNEL_PROLOGUE_CMDS_OFFSET > CHANNEL_FENCE_CMDS_OFFSET,
                   "the prologue block must follow the fence block");
    const uint64_t flush_len =
        (CHANNEL_PROLOGUE_CMDS_OFFSET - CHANNEL_FENCE_CMDS_OFFSET) +
        (uint64_t)chan->prologue_cmds_dwords * 4;
    res = horizon_gpu_mem_flush(chan->cmdbuf_mem, CHANNEL_FENCE_CMDS_OFFSET,
                                flush_len);
    if (horizon_gpu_failed(res))
        goto fail_cmdbuf_map;

    chan->fence_cmds_va = horizon_gpu_mapping_va(chan->cmdbuf_map) +
                          CHANNEL_FENCE_CMDS_OFFSET;
    chan->setobj_cmds_va = horizon_gpu_mapping_va(chan->cmdbuf_map) +
                           CHANNEL_SETOBJ_CMDS_OFFSET;
    chan->prologue_cmds_va = horizon_gpu_mapping_va(chan->cmdbuf_map) +
                             CHANNEL_PROLOGUE_CMDS_OFFSET;
    chan->wait_cmds_va = horizon_gpu_mapping_va(chan->cmdbuf_map) +
                         HORIZON_CHANNEL_WAIT_CMDS_OFFSET;
    /* The channel is callocated, so every slot already reads as free and
     * the ring already starts at 0; nothing else needs initialising. */

    /* Optional Zcull context. */
    if (create_info->bind_zcull) {
        res = horizon_gpu_mem_create(dev, zcull_size, 0,
                                     HORIZON_GPU_MEM_CACHED,
                                     &chan->zcull_mem);
        if (horizon_gpu_failed(res))
            goto fail_cmdbuf_map;

        res = horizon_gpu_vm_map(chan->internal_range, CHANNEL_ZCULL_ALIGN,
                                 chan->zcull_mem, 0,
                                 horizon_gpu_mem_size(chan->zcull_mem),
                                 HORIZON_GPU_PTE_KIND_PITCH, true,
                                 &chan->zcull_map);
        if (horizon_gpu_failed(res))
            goto fail_zcull_mem;

        rc = nvGpuChannelZcullBind(&chan->gc,
                                   horizon_gpu_mapping_va(chan->zcull_map));
        if (R_FAILED(rc)) {
            horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                         "nvGpuChannelZcullBind failed: 0x%08x", rc);
            res = horizon_gpu_err_nv(rc);
            goto fail_zcull_map;
        }
    }

    atomic_fetch_add(&dev->live_channels, 1);
    horizon_logf(&dev->log, HORIZON_LOG_INFO,
                 "channel %p: up, syncpt=%u initial=%u%s zcull=%s",
                 (void *)chan, chan->syncpt_id,
                 chan->syncpt_value_at_create,
                 chan->syncpt_baseline_trusted ? "" : " (UNTRUSTED)",
                 create_info->bind_zcull ? "bound" : "off");

    *out_chan = chan;
    return horizon_gpu_ok();

fail_zcull_map:
    channel_create_unwind_step(dev, "vm_unmap(zcull_map)",
                               horizon_gpu_vm_unmap(chan->zcull_map));
fail_zcull_mem:
    channel_create_unwind_step(dev, "mem_destroy(zcull_mem)",
                               horizon_gpu_mem_destroy(chan->zcull_mem));
fail_cmdbuf_map:
    channel_create_unwind_step(dev, "vm_unmap(cmdbuf_map)",
                               horizon_gpu_vm_unmap(chan->cmdbuf_map));
fail_range:
    channel_create_unwind_step(dev, "vm_release(internal_range)",
                               horizon_gpu_vm_release(chan->internal_range));
fail_cmdbuf_mem:
    channel_create_unwind_step(dev, "mem_destroy(cmdbuf_mem)",
                               horizon_gpu_mem_destroy(chan->cmdbuf_mem));
fail_channel:
    nvGpuChannelClose(&chan->gc);
fail_free:
    free(chan);
    return res;
}

uint32_t horizon_gpu_channel_syncpt_id(const horizon_gpu_channel *chan)
{
    return chan ? chan->syncpt_id : 0;
}

uint32_t
horizon_gpu_channel_syncpt_value_at_create(const horizon_gpu_channel *chan)
{
    return chan ? chan->syncpt_value_at_create : 0;
}

bool
horizon_gpu_channel_syncpt_baseline_trusted(const horizon_gpu_channel *chan)
{
    return chan ? chan->syncpt_baseline_trusted : false;
}

uint64_t horizon_gpu_channel_shadow_target(const horizon_gpu_channel *chan)
{
    return chan ? chan->shadow_target : 0;
}

horizon_gpu_fence
horizon_gpu_channel_last_fence(const horizon_gpu_channel *chan)
{
    if (!chan)
        return (horizon_gpu_fence){ 0, 0 };
    return (horizon_gpu_fence){
        .syncpt_id = chan->syncpt_id,
        .threshold = (uint32_t)chan->shadow_target,
    };
}

bool horizon_gpu_channel_is_lost(const horizon_gpu_channel *chan)
{
    return chan ? chan->lost : true;
}

horizon_gpu_result
horizon_gpu_channel_bind_engines(horizon_gpu_channel *chan,
                                 horizon_gpu_fence *out_fence)
{
    if (!chan)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);
    if (chan->engines_bound)
        return horizon_gpu_err(HORIZON_GPU_ERR_STATE);
    if (chan->lost)
        return horizon_gpu_err(HORIZON_GPU_ERR_CHANNEL_LOST);

    const horizon_gpu_device_info *info = &chan->dev->info;
    /* Subchannel order per cmds.h: 3D, compute, inline-to-memory, 2D,
     * copy — every class number queried from the characteristics. */
    const uint32_t classes[HORIZON_CMDS_NUM_SUBCHANNELS] = {
        info->threed_class,
        info->compute_class,
        info->inline_to_memory_class,
        info->twod_class,
        info->dma_copy_class,
    };

    uint32_t *cmds = (uint32_t *)((uint8_t *)
        horizon_gpu_mem_cpu_ptr(chan->cmdbuf_mem) +
        CHANNEL_SETOBJ_CMDS_OFFSET);
    uint32_t n = horizon_cmds_set_objects(cmds, classes);

    horizon_gpu_result res = horizon_gpu_mem_flush(chan->cmdbuf_mem,
                                                   CHANNEL_SETOBJ_CMDS_OFFSET,
                                                   n * 4);
    if (horizon_gpu_failed(res))
        return res;

    horizon_gpu_cmd_span span = {
        .gpu_va = chan->setobj_cmds_va,
        .num_dwords = n,
    };
    horizon_gpu_fence fence;
    res = horizon_gpu_submit(chan, &span, 1, HORIZON_GPU_SUBMIT_DEFAULT,
                             &fence);
    if (horizon_gpu_failed(res))
        return res;

    chan->engines_bound = true;
    if (out_fence)
        *out_fence = fence;
    return horizon_gpu_ok();
}

horizon_gpu_result horizon_gpu_channel_reap(horizon_gpu_channel *chan,
                                            uint32_t *out_retired)
{
    if (!chan)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);

    /* THE FAULT CHECK BELONGS HERE TOO, and for the same reason it
     * belongs in the wait: nvgpu's recovery force-increments a faulted
     * channel's syncpoints, so the counter this function is about to
     * read says "finished" for work that never ran. Without this,
     * every retirement whose threshold that counter passed fired and
     * reap returned ok — buffers recycled for work the GPU abandoned,
     * with no caller told. horizon_gpu_submit reaps before it queues
     * and wait_idle returns this function's result, so two more paths
     * were answering "fine" about a dead channel.
     *
     * Fixed in the wait only, first time round; found in review of
     * PR #7. The callbacks are not run here — horizon_gpu_channel_
     * destroy still runs them, which is the documented path
     * (add_retirement's comment) and the one that lets a callback ask
     * horizon_gpu_channel_is_lost() and behave differently.
     *
     * COST, stated because it is a hot path: one non-blocking event
     * wait and one ioctl per reap, therefore per submit. Measured
     * against the 85 us of CPU a vkQueueSubmit already costs on this
     * platform (t_vk_submits), it is not the expensive part — and a
     * cheap wrong answer here is what this whole fix is about. */
    if (channel_check_fault(chan))
        return horizon_gpu_err(HORIZON_GPU_ERR_CHANNEL_LOST);

    uint32_t hw;
    horizon_gpu_result res = horizon_channel_read_syncpt(chan, &hw);
    if (horizon_gpu_failed(res)) {
        /* An untrusted-baseline channel (docs/synchronization.md § 9) is
         * one whose platform has no syncpoint read at all, so this fails
         * every time and would fail every submit with it —
         * horizon_gpu_submit reaps before it queues.
         *
         * Report "nothing retired" and carry on. Retirement on such a
         * channel is meaningless anyway, and of the two possible
         * answers this is the safe one: the alternative — treating
         * everything as complete — would make a wait return without the
         * GPU having run, which is the one answer that must never be
         * given. The cost is that nothing is ever recycled, which a
         * bring-up run can afford.
         */
        if (!chan->syncpt_baseline_trusted) {
            if (out_retired)
                *out_retired = 0;
            return horizon_gpu_ok();
        }
        return res;
    }
    uint64_t now64 = horizon_syncpt_extend(chan->shadow_target, hw);

    uint32_t retired = 0;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < chan->retire_count; i++) {
        horizon_retire_entry e = chan->retire[i];
        if (e.threshold64 <= now64) {
            if (e.fn)
                e.fn(e.ctx);
            retired++;
        } else {
            chan->retire[kept++] = e;
        }
    }
    chan->retire_count = kept;

    if (out_retired)
        *out_retired = retired;
    return horizon_gpu_ok();
}

horizon_gpu_result
horizon_gpu_channel_add_retirement(horizon_gpu_channel *chan,
                                   horizon_gpu_fence fence,
                                   void (*fn)(void *ctx), void *ctx)
{
    if (!chan || fence.syncpt_id != chan->syncpt_id)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);

    if (chan->retire_count == chan->retire_capacity) {
        uint32_t cap = chan->retire_capacity ? chan->retire_capacity * 2 : 8;
        horizon_retire_entry *list =
            realloc(chan->retire, (size_t)cap * sizeof(*list));
        if (!list)
            return horizon_gpu_err(HORIZON_GPU_ERR_OUT_OF_MEMORY);
        chan->retire = list;
        chan->retire_capacity = cap;
    }

    chan->retire[chan->retire_count++] = (horizon_retire_entry){
        .threshold64 = horizon_syncpt_extend(chan->shadow_target,
                                             fence.threshold),
        .fn = fn,
        .ctx = ctx,
    };
    return horizon_gpu_ok();
}

horizon_gpu_result
horizon_gpu_channel_wait_fence(horizon_gpu_channel *chan,
                               horizon_gpu_fence fence, uint64_t timeout_ns)
{
    if (!chan || fence.syncpt_id != chan->syncpt_id)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);
    if (chan->lost)
        return horizon_gpu_err(HORIZON_GPU_ERR_CHANNEL_LOST);

    uint64_t start = armGetSystemTick();
    NvFence nvf = { .id = fence.syncpt_id, .value = fence.threshold };

    for (;;) {
        uint32_t hw;
        horizon_gpu_result res = horizon_channel_read_syncpt(chan, &hw);
        if (horizon_gpu_failed(res)) {
            if (chan->syncpt_baseline_trusted)
                return res;
            /* No counter on this platform (§ 9). The fence's own
             * question can still be asked, through the wait ioctl that
             * nvFenceWait drives — a different call, which a platform
             * can implement while leaving the read out. The notifier is
             * still re-checked between chunks, so a faulted channel does
             * not hang here either.
             *
             * What this does NOT rescue is the threshold: it was
             * computed from a baseline nobody read, so "reached" is only
             * as good as that assumption. The channel stays untrusted
             * and t_vulkan still refuses to call such a run a pass. */
            uint64_t used = armTicksToNs(armGetSystemTick() - start);
            if (timeout_ns != HORIZON_GPU_NO_TIMEOUT && used >= timeout_ns)
                return horizon_gpu_err(HORIZON_GPU_ERR_TIMEOUT);

            int32_t w_us = CHANNEL_WAIT_CHUNK_US;
            if (timeout_ns != HORIZON_GPU_NO_TIMEOUT) {
                int32_t rem =
                    horizon_timeout_ns_to_us_clamped(timeout_ns - used);
                if (rem < w_us)
                    w_us = rem;
            }

            Result rc = nvFenceWait(&nvf, w_us);
            if (R_SUCCEEDED(rc))
                return channel_reached_or_lost(chan);
            if (rc != KERNELRESULT(TimedOut))
                return horizon_gpu_err_nv(rc);

            if (channel_check_fault(chan))
                return horizon_gpu_err(HORIZON_GPU_ERR_CHANNEL_LOST);
            /* Round the loop rather than returning: one expired chunk is
             * not the caller's deadline. Returning TIMEOUT here made
             * every finite wait on this path last CHANNEL_WAIT_CHUNK_US
             * and no longer — a requested two seconds failed after 100
             * ms, reporting a timeout for a fence that had 1.9 seconds
             * left to signal in. The top of this branch recomputes
             * `used` and returns TIMEOUT there when the deadline has
             * genuinely passed, which is the same shape the syncpoint-
             * read path below uses; the two disagreed only here.
             */
            continue;
        }
        if (horizon_gpu_syncpt_reached(hw, fence.threshold))
            return channel_reached_or_lost(chan);

        if (channel_check_fault(chan))
            return horizon_gpu_err(HORIZON_GPU_ERR_CHANNEL_LOST);

        uint64_t elapsed_ns = armTicksToNs(armGetSystemTick() - start);
        if (timeout_ns != HORIZON_GPU_NO_TIMEOUT &&
            elapsed_ns >= timeout_ns)
            return horizon_gpu_err(HORIZON_GPU_ERR_TIMEOUT);

        /* Bounded kernel wait per iteration; the loop re-checks the
         * notifier so a faulted channel cannot hang us forever, even
         * with HORIZON_GPU_NO_TIMEOUT (docs/synchronization.md § 6). */
        int32_t chunk_us = CHANNEL_WAIT_CHUNK_US;
        if (timeout_ns != HORIZON_GPU_NO_TIMEOUT) {
            uint64_t remaining_ns = timeout_ns - elapsed_ns;
            int32_t rem_us = horizon_timeout_ns_to_us_clamped(remaining_ns);
            if (rem_us < chunk_us)
                chunk_us = rem_us;
        }
        /* Result deliberately not treated as fatal: a timeout of this
         * chunk is the loop's normal pulse; real failures surface via
         * SyncptRead or the notifier above. */
        (void)nvFenceWait(&nvf, chunk_us);
    }
}

horizon_gpu_result horizon_gpu_channel_wait_idle(horizon_gpu_channel *chan,
                                                 uint64_t timeout_ns)
{
    if (!chan)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);
    horizon_gpu_result res =
        horizon_gpu_channel_wait_fence(chan,
                                       horizon_gpu_channel_last_fence(chan),
                                       timeout_ns);
    if (horizon_gpu_failed(res))
        return res;
    return horizon_gpu_channel_reap(chan, NULL);
}

horizon_gpu_result horizon_gpu_channel_destroy(horizon_gpu_channel *chan)
{
    if (!chan)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);

    horizon_gpu_device *dev = chan->dev;

    /* All submitted work must have retired — unless the channel is lost,
     * in which case its counter may never advance again and teardown must
     * still be possible (architecture.md § 6). */
    if (!chan->lost) {
        uint32_t hw;
        horizon_gpu_result res = horizon_channel_read_syncpt(chan, &hw);
        if (horizon_gpu_failed(res) && !chan->syncpt_baseline_trusted) {
            /* Same reasoning as reap (§ 9): the read never works on this
             * platform, so refusing the destroy would strand the channel
             * and, through the live-object count, the device with it.
             * Whether work is still in flight is unknowable here — say
             * so rather than imply it was checked.
             *
             * A REJECTED ALTERNATIVE, recorded because it looks right
             * and is not. It was briefly true that this polled the last
             * fence with nvFenceWait and returned BUSY when it had not
             * been reached: the counter cannot be *read* here, but the
             * wait ioctl is a different call a platform might still
             * implement.
             *
             * The value it would have to ask about is the problem. When
             * the baseline is untrusted, the read at channel creation
             * failed too, so syncpt_value_at_create is 0 and
             * shadow_target counts *submits from zero* — it is not the
             * hardware counter and the code that sets it says so:
             * "its fences are not measurements". Whether Horizon resets
             * a syncpoint at channel creation is R5, still open. If it
             * does not, a threshold of 3 is compared against a live
             * counter in the thousands, which is already past it, so the
             * poll succeeds instantly and the destroy proceeds reporting
             * that work retired when nothing was verified.
             *
             * That is worse than not checking. horizon_gpu_channel_
             * wait_fence already says of this same number that "reached"
             * is only as good as an assumption nobody verified; turning
             * that into a hard BUSY-or-OK decision would have two
             * functions in this file drawing opposite conclusions from
             * one unreliable value, with the destroy biased toward false
             * assurance.
             *
             * A real check needs a trustworthy baseline, which is R5's
             * job, not this function's.
             */
            horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                         "channel %p: destroying with an UNTRUSTED "
                         "baseline; whether submitted work retired was "
                         "not checked", (void *)chan);
            goto skip_inflight_check;
        }
        if (horizon_gpu_failed(res))
            return res;
        uint64_t now64 = horizon_syncpt_extend(chan->shadow_target, hw);
        if (now64 < chan->shadow_target) {
            horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                         "channel %p: destroy refused, %llu increment(s) in "
                         "flight", (void *)chan,
                         (unsigned long long)(chan->shadow_target - now64));
            return horizon_gpu_err(HORIZON_GPU_ERR_BUSY);
        }
    }
skip_inflight_check:
    /* Reap even on a lost channel: whatever did retire before the fault
     * still fires normally. This is a plain syncpoint read, harmless
     * regardless of channel health. */
    horizon_gpu_channel_reap(chan, NULL);
    if (chan->retire_count != 0) {
        if (chan->lost) {
            /* A lost channel's syncpoint can never advance further and
             * there is no cancellation ioctl, so these entries can never
             * retire through the normal path — refusing destroy here
             * would leak the channel (and, transitively, the device)
             * forever. Run them anyway: on Horizon a channel fault
             * abandons every not-yet-retired submit on it, so "retired"
             * here means "abandoned", not "completed" — callers relying
             * on the callback to mean GPU-visible completion must check
             * horizon_gpu_channel_is_lost() first (architecture.md § 6). */
            horizon_logf(&dev->log, HORIZON_LOG_WARN,
                         "channel %p: lost with %u unretired entries; "
                         "force-retiring them as abandoned so teardown can "
                         "proceed", (void *)chan, chan->retire_count);
            for (uint32_t i = 0; i < chan->retire_count; i++) {
                horizon_retire_entry e = chan->retire[i];
                if (e.fn)
                    e.fn(e.ctx);
            }
            chan->retire_count = 0;
        } else {
            horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                         "channel %p: destroy refused, %u unretired "
                         "entries", (void *)chan, chan->retire_count);
            return horizon_gpu_err(HORIZON_GPU_ERR_BUSY);
        }
    }

    /* Reverse of creation. */
    horizon_gpu_result res;
    if (chan->zcull_map) {
        res = horizon_gpu_vm_unmap(chan->zcull_map);
        if (horizon_gpu_failed(res))
            return res;
        chan->zcull_map = NULL;
    }
    if (chan->zcull_mem) {
        res = horizon_gpu_mem_destroy(chan->zcull_mem);
        if (horizon_gpu_failed(res))
            return res;
        chan->zcull_mem = NULL;
    }
    res = horizon_gpu_vm_unmap(chan->cmdbuf_map);
    if (horizon_gpu_failed(res))
        return res;
    chan->cmdbuf_map = NULL;
    res = horizon_gpu_vm_release(chan->internal_range);
    if (horizon_gpu_failed(res))
        return res;
    chan->internal_range = NULL;
    res = horizon_gpu_mem_destroy(chan->cmdbuf_mem);
    if (horizon_gpu_failed(res))
        return res;
    chan->cmdbuf_mem = NULL;

    /* `chan` has exactly one documented owner: a second destroy call on
     * the same pointer is a caller bug, not a case this layer defends
     * against — `chan` is freed below, so a check reading back through
     * the pointer afterwards would itself be a use-after-free. */
    nvGpuChannelClose(&chan->gc);
    free(chan->retire);
    atomic_fetch_sub(&dev->live_channels, 1);
    free(chan);
    return horizon_gpu_ok();
}
