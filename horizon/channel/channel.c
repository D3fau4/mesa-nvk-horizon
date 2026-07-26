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

/* Zcull context buffer VA alignment. Source: the reference's
 * hardware-tested channel bring-up (reference-analysis § 4: Zcull BO
 * aligned 0x20000). */
#define CHANNEL_ZCULL_ALIGN UINT64_C(0x20000)

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
            *out_type = 0;
            if (out_desc)
                *out_desc = channel_error_desc(0);
            return horizon_gpu_ok();
        }
        horizon_logf(&chan->dev->log, HORIZON_LOG_ERROR,
                     "GetErrorNotification failed: 0x%08x", rc);
        return horizon_gpu_err_nv(rc);
    }

    /* A notification with a zero timestamp has never fired. */
    *out_type = (notif.timestamp != 0) ? notif.info32 : 0;
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

    /* Shadow initialisation from the observed hardware value; whether
     * Horizon resets the counter at channel creation is the R5 open
     * question — t_channel reports this number. */
    res = horizon_channel_read_syncpt(chan, &chan->syncpt_value_at_create);
    if (horizon_gpu_failed(res)) {
        horizon_logf(&dev->log, HORIZON_LOG_ERROR,
                     "initial SyncptRead(%u) failed: 0x%08x",
                     chan->syncpt_id, res.nv);
        goto fail_channel;
    }
    chan->shadow_target = chan->syncpt_value_at_create;

    /* Internal command buffer. */
    res = horizon_gpu_mem_create(dev, CHANNEL_CMDBUF_SIZE, 0,
                                 HORIZON_GPU_MEM_CACHED, &chan->cmdbuf_mem);
    if (horizon_gpu_failed(res))
        goto fail_channel;

    /* One reservation holds the cmdbuf slot and (optionally) Zcull:
     * cmdbuf at base+0, Zcull at base+0x20000, with the base aligned so
     * the Zcull VA meets CHANNEL_ZCULL_ALIGN. */
    uint64_t zcull_size = 0;
    uint64_t reserve_size = CHANNEL_ZCULL_ALIGN;
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
    }

    res = horizon_gpu_vm_reserve(dev, reserve_size,
                                 (uint32_t)HORIZON_GPU_SMALL_PAGE_SIZE,
                                 CHANNEL_ZCULL_ALIGN, &chan->internal_range);
    if (horizon_gpu_failed(res))
        goto fail_cmdbuf_mem;

    res = horizon_gpu_vm_map(chan->internal_range, 0, chan->cmdbuf_mem, 0,
                             CHANNEL_CMDBUF_SIZE, HORIZON_GPU_PTE_KIND_PITCH,
                             true, &chan->cmdbuf_map);
    if (horizon_gpu_failed(res))
        goto fail_range;

    /* Write the per-submit fence-increment list once; its content only
     * depends on the syncpoint id. */
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
    res = horizon_gpu_mem_flush(chan->cmdbuf_mem, CHANNEL_FENCE_CMDS_OFFSET,
                                chan->fence_cmds_dwords * 4);
    if (horizon_gpu_failed(res))
        goto fail_cmdbuf_map;

    chan->fence_cmds_va = horizon_gpu_mapping_va(chan->cmdbuf_map) +
                          CHANNEL_FENCE_CMDS_OFFSET;
    chan->setobj_cmds_va = horizon_gpu_mapping_va(chan->cmdbuf_map) +
                           CHANNEL_SETOBJ_CMDS_OFFSET;

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
                 "channel %p: up, syncpt=%u initial=%u zcull=%s",
                 (void *)chan, chan->syncpt_id,
                 chan->syncpt_value_at_create,
                 create_info->bind_zcull ? "bound" : "off");

    *out_chan = chan;
    return horizon_gpu_ok();

fail_zcull_map:
    horizon_gpu_vm_unmap(chan->zcull_map);
fail_zcull_mem:
    horizon_gpu_mem_destroy(chan->zcull_mem);
fail_cmdbuf_map:
    horizon_gpu_vm_unmap(chan->cmdbuf_map);
fail_range:
    horizon_gpu_vm_release(chan->internal_range);
fail_cmdbuf_mem:
    horizon_gpu_mem_destroy(chan->cmdbuf_mem);
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

    uint32_t hw;
    horizon_gpu_result res = horizon_channel_read_syncpt(chan, &hw);
    if (horizon_gpu_failed(res))
        return res;
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
        if (horizon_gpu_failed(res))
            return res;
        if (horizon_gpu_syncpt_reached(hw, fence.threshold))
            return horizon_gpu_ok();

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
    if (!chan || !chan->dev)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);

    horizon_gpu_device *dev = chan->dev;

    /* All submitted work must have retired — unless the channel is lost,
     * in which case its counter may never advance again and teardown must
     * still be possible (architecture.md § 6). */
    if (!chan->lost) {
        uint32_t hw;
        horizon_gpu_result res = horizon_channel_read_syncpt(chan, &hw);
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

    nvGpuChannelClose(&chan->gc);
    free(chan->retire);
    atomic_fetch_sub(&dev->live_channels, 1);
    chan->dev = NULL;
    free(chan);
    return horizon_gpu_ok();
}
