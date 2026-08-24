/*
 * Decision D12, asked of the hardware instead of assumed.
 *
 * WHAT D12 CLOSED, AND WHY IT MIGHT NOT NEED TO BE CLOSED. Vulkan sparse
 * residency is off in this driver — nvkmd_info::has_sparse is false and
 * seven VkPhysicalDeviceFeatures follow it — because the layer had no
 * way to unbind part of a reservation and leave the rest addressable.
 * The reservation half was never the problem:
 * NvAllocSpaceFlags_Sparse exists, and MapBufferEx has taken a buffer
 * offset and a mapping size since this layer was written. What nobody
 * knew is what the chip does at the edges, and building sparse on a
 * guess about that is how you get an MMU fault a year later.
 *
 * THE INSTRUMENT IS A WRITE, NOT A READ. Nothing in this layer makes the
 * GPU read memory, but the channel's own semaphore release makes it
 * write — a host method, no engine object, the same instrument
 * t_gpuwrite uses. Pointed at an address the page tables do not resolve,
 * it faults; pointed at a sparse page that resolves to nothing, it is
 * swallowed. So "did the channel survive" is the answer to "is this
 * address defined", and it needs no readback at all.
 *
 * Each probe therefore gets its own channel: a fault loses the channel
 * it happened on, and a lost channel cannot answer the next question.
 *
 * THE THREE MEASUREMENTS.
 *
 *   A  Write to a sparse page nothing was ever bound to.
 *      Survives  -> the reservation really is backed by
 *                   resolve-to-nothing entries.
 *      Faults    -> NvAllocSpaceFlags_Sparse does not do that here, and
 *                   D12 stays closed for the reason it was opened.
 *
 *   B  Bind memory into the middle block and write to it.
 *      Survives and the payload arrives -> an ordinary mapping works
 *      inside a sparse reservation, which sparse binding needs.
 *
 *   C  Unbind that block and write to the same address again.
 *      THIS IS THE CRUX. Survives -> unbinding restores the sparse
 *      state, partial residency is expressible, and D12 can be
 *      reopened. Faults -> unbinding punches a hole, and everything
 *      built on top would fault the moment an application unbound a
 *      tile. That is a real answer and it closes D12 properly, with a
 *      measurement behind it instead of an absence.
 *
 * A FAULT IS NOT A FAILING TEST. Arm A reports what happened. Arm B has
 * an outcome it demands, because a mapping that does not work inside a
 * sparse reservation would mean the reservation itself is broken rather
 * than that sparse is unsupported — and because without it arm C has no
 * control and cannot answer anything. Arm C demands only that its write
 * stayed out of the unbound memory; whether it faulted is the reading,
 * not the verdict.
 *
 * ARM B IS THE CONTROL, AND IT IS EASY TO GET WRONG. It failed on run 17
 * for a reason that had nothing to do with sparse: the backing NvMap was
 * left at the default 4 KiB alignment while the reservation binds in
 * 128 KiB pages. MapBufferEx accepted that and returned the requested
 * address; the GPU's write to it went nowhere. See the allocation below.
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

const char *const test_name = "t_sparse";
/* No display: main() starts a console and reports through it. */
const bool test_uses_display = false;

#define WAIT_NS  UINT64_C(2000000000)
/* Two payloads, not one. Arm C writes to the address arm B's memory was
 * bound at, and the question is whether that write reached the memory.
 * With a single payload it cannot be asked: the memory already holds
 * what C would write, so "unchanged" and "written again" are the same
 * bytes. Run 17 printed exactly that ambiguity and it is why C's answer
 * from that run was not usable. */
#define PAYLOAD_B UINT32_C(0x5A5AA5A5)
#define PAYLOAD_C UINT32_C(0xC3C33C3C)
#define PREFILL   UINT32_C(0xA5A55A5A)
#define CMD_B    UINT32_C(0x1000)

/* The command list lives in its own ordinary reservation, so a fault in
 * the sparse one cannot be blamed on where the commands were. */
typedef struct {
    horizon_gpu_mem *mem;
    horizon_gpu_va_range *range;
    horizon_gpu_mapping *map;
    uint64_t gpu_va;
    uint32_t *cpu;
} buf;

static horizon_gpu_result buf_create(horizon_gpu_device *dev, uint32_t size,
                                     buf *out)
{
    memset(out, 0, sizeof(*out));
    horizon_gpu_result res =
        horizon_gpu_mem_create(dev, size, 0, HORIZON_GPU_MEM_CACHED,
                               &out->mem);
    if (horizon_gpu_failed(res))
        return res;
    res = horizon_gpu_vm_reserve(dev, size, HORIZON_GPU_SMALL_PAGE_SIZE, 0,
                                 &out->range);
    if (horizon_gpu_failed(res))
        return res;
    res = horizon_gpu_vm_map(out->range, 0, out->mem, 0, size,
                             HORIZON_GPU_PTE_KIND_PITCH, false, &out->map);
    if (horizon_gpu_failed(res))
        return res;
    out->gpu_va = horizon_gpu_mapping_va(out->map);
    out->cpu = horizon_gpu_mem_cpu_ptr(out->mem);
    return horizon_gpu_ok();
}

static void buf_destroy(buf *b)
{
    if (b->map)
        horizon_gpu_vm_unmap(b->map);
    if (b->range)
        horizon_gpu_vm_release(b->range);
    if (b->mem)
        horizon_gpu_mem_destroy(b->mem);
    memset(b, 0, sizeof(*b));
}

/* Makes the GPU write `payload` to `gpu_va` on a channel of its own.
 * Sets *out_survived to whether the channel came through it. */
static void probe_write(test_ctx *t, horizon_gpu_device *dev, buf *cmd,
                        uint64_t gpu_va, uint32_t payload, const char *what,
                        bool *out_survived)
{
    *out_survived = false;

    horizon_gpu_channel *chan = NULL;
    horizon_gpu_result res = horizon_gpu_channel_create(dev, NULL, &chan);
    if (!t_check(t, horizon_gpu_succeeded(res),
                 "%s: channel_create (status=%s nv=0x%08x)", what,
                 horizon_gpu_status_str(res.status), res.nv))
        return;

    uint32_t n = horizon_cmds_semaphore_release(cmd->cpu, gpu_va, payload);
    if (!t_check(t, n == HORIZON_CMDS_SEM_RELEASE_DWORDS,
                 "%s: release encoded for 0x%" PRIx64 " (%u dwords)",
                 what, gpu_va, n))
        goto out;

    res = horizon_gpu_mem_flush(cmd->mem, 0, n * 4);
    if (!t_check(t, horizon_gpu_succeeded(res),
                 "%s: command list flushed", what))
        goto out;

    const horizon_gpu_cmd_span span = { .gpu_va = cmd->gpu_va,
                                        .num_dwords = n };
    horizon_gpu_fence fence;
    res = horizon_gpu_submit(chan, &span, 1, HORIZON_GPU_SUBMIT_DEFAULT,
                             &fence);
    if (!t_check(t, horizon_gpu_succeeded(res),
                 "%s: submit accepted (status=%s nv=0x%08x)", what,
                 horizon_gpu_status_str(res.status), res.nv))
        goto out;

    res = horizon_gpu_channel_wait_fence(chan, fence, WAIT_NS);
    const bool lost = horizon_gpu_channel_is_lost(chan);
    *out_survived = horizon_gpu_succeeded(res) && !lost;

    if (!*out_survived) {
        uint32_t type = 0;
        const char *desc = NULL;
        horizon_gpu_result e = horizon_gpu_channel_get_error(chan, &type,
                                                             &desc);
        t_note(t, "%s: the channel did NOT survive — wait status=%s, "
               "lost=%s, notifier type=%u (%s)", what,
               horizon_gpu_status_str(res.status), lost ? "yes" : "no",
               type, horizon_gpu_succeeded(e) && desc ? desc : "none read");
    }

out:
    horizon_gpu_channel_destroy(chan);
}

int run_test(test_ctx *t)
{
    horizon_gpu_device *dev = NULL;
    horizon_gpu_result res = horizon_gpu_device_create(NULL, &dev);
    if (!t_check(t, horizon_gpu_succeeded(res), "device_create (status=%s "
                 "nv=0x%08x)", horizon_gpu_status_str(res.status), res.nv))
        return 1;

    horizon_gpu_device_info info;
    res = horizon_gpu_device_get_info(dev, &info);
    if (!t_check(t, horizon_gpu_succeeded(res), "device_get_info"))
        return 1;

    /* Three blocks at the address space's own binding granularity: the
     * size a partial bind would have to work in. */
    const uint32_t page = info.as_big_page_size;
    const uint64_t blk = page;
    t_note(t, "block size = as_big_page_size = 0x%" PRIx64, blk);

    buf cmd;
    res = buf_create(dev, CMD_B, &cmd);
    if (!t_check(t, horizon_gpu_succeeded(res),
                 "command buffer mapped (status=%s nv=0x%08x)",
                 horizon_gpu_status_str(res.status), res.nv))
        return 1;

    /* ---- the sparse reservation itself ---------------------------- */
    horizon_gpu_va_range *sp = NULL;
    res = horizon_gpu_vm_reserve_sparse(dev, blk * 3, page, 0, &sp);
    if (!t_check(t, horizon_gpu_succeeded(res),
                 "MEASURED: a sparse reservation of 3 blocks was accepted "
                 "(status=%s nv=0x%08x)", horizon_gpu_status_str(res.status),
                 res.nv)) {
        t_note(t, "MEASURED: NvAllocSpaceFlags_Sparse is refused here. "
               "D12 stays closed, and this is the reason to record.");
        buf_destroy(&cmd);
        horizon_gpu_device_destroy(dev);
        return 0;
    }
    const uint64_t mid_va = horizon_gpu_va_range_base(sp) + blk;
    t_note(t, "sparse range at 0x%" PRIx64 ", middle block at 0x%" PRIx64,
           horizon_gpu_va_range_base(sp), mid_va);

    /* ---- A: a page nothing was ever bound to ---------------------- */
    bool a_survived = false;
    probe_write(t, dev, &cmd, mid_va, PAYLOAD_B, "A (never bound)",
                &a_survived);
    t_note(t, "MEASURED A: a write to a never-bound sparse page %s",
           a_survived ? "was swallowed; the reservation resolves to nothing"
                      : "FAULTED; the reservation is not backed here");

    /* ---- B: bind the middle block and write to it ------------------ */
    /* ALIGNED TO THE PAGE THE RESERVATION BINDS IN, not to 4 KiB.
     *
     * Run 17 passed 0 here, which horizon_gpu_mem_create reads as
     * HORIZON_GPU_SMALL_PAGE_SIZE. MapBufferEx was then handed a
     * 4 KiB-aligned buffer and a 128 KiB page size, accepted it, and
     * returned the requested VA — and the GPU's write to that VA went
     * nowhere. Arm B failed and took arm C's answer down with it: a
     * write that vanishes at an address that was never resolving says
     * nothing about what unbinding does.
     *
     * A big-page mapping needs big-page-aligned backing. That is the
     * first thing this test measured and it is worth stating in the
     * log, because a Vulkan sparse implementation would allocate every
     * tile this way. */
    horizon_gpu_mem *mem = NULL;
    res = horizon_gpu_mem_create(dev, (uint32_t)blk, blk,
                                 HORIZON_GPU_MEM_CACHED, &mem);
    if (!t_check(t, horizon_gpu_succeeded(res),
                 "backing memory created, aligned to the 0x%" PRIx64
                 " page the reservation binds in", blk))
        goto done;

    uint32_t *cpu = horizon_gpu_mem_cpu_ptr(mem);
    cpu[0] = PREFILL;
    horizon_gpu_mem_flush(mem, 0, 4);

    horizon_gpu_mapping *mid = NULL;
    res = horizon_gpu_vm_map(sp, blk, mem, 0, blk,
                             HORIZON_GPU_PTE_KIND_PITCH, false, &mid);
    if (!t_check(t, horizon_gpu_succeeded(res),
                 "MEASURED B: memory binds into the middle of a sparse "
                 "reservation (status=%s nv=0x%08x)",
                 horizon_gpu_status_str(res.status), res.nv))
        goto done_mem;

    bool b_survived = false;
    probe_write(t, dev, &cmd, mid_va, PAYLOAD_B, "B (bound)", &b_survived);
    t_check(t, b_survived,
            "B: a write to the bound block executed without a fault");

    horizon_gpu_mem_invalidate(mem, 0, 4);
    const uint32_t after_b = cpu[0];
    const bool b_landed = t_check(t, after_b == PAYLOAD_B,
            "B: the payload arrived in the bound block (0x%08" PRIx32
            ", expected 0x%08" PRIx32 ")", after_b, PAYLOAD_B);
    if (!b_landed)
        t_note(t, "B did not land, so arm C below cannot answer D12: a "
               "write that disappears at an address that was never "
               "resolving proves nothing about unbinding");

    /* ---- C: unbind it and write to the same address ---------------- */
    res = horizon_gpu_vm_unmap(mid);
    mid = NULL;
    if (!t_check(t, horizon_gpu_succeeded(res),
                 "C: the middle block unbound (status=%s nv=0x%08x)",
                 horizon_gpu_status_str(res.status), res.nv))
        goto done_mem;

    bool c_survived = false;
    probe_write(t, dev, &cmd, mid_va, PAYLOAD_C, "C (unbound again)",
                &c_survived);

    /* The memory must still hold what arm B left there: a write that was
     * swallowed did not reach it. PAYLOAD_C is a value nothing has ever
     * put in this buffer, so finding it here would mean the unmap left
     * the old translation live — which is a different failure from a
     * fault and would be invisible with one payload. */
    horizon_gpu_mem_invalidate(mem, 0, 4);
    const uint32_t after_c = cpu[0];
    t_note(t, "C: the previously bound memory reads 0x%08" PRIx32
           " (it read 0x%08" PRIx32 " right after arm B)",
           after_c, after_b);
    t_check(t, after_c != PAYLOAD_C,
            "C: the write did not reach the memory that was unbound");

    if (!b_landed) {
        t_note(t, "MEASURED C: not usable — arm B's control did not land, "
               "so this address was not shown to resolve before the "
               "unbind. D12 is unchanged by this run.");
    } else {
        t_note(t, "MEASURED C — THE ANSWER TO D12: after unbinding, a "
               "write to that address %s", c_survived
               ? "was swallowed. Unbinding restores the sparse state, so "
                 "partial residency is expressible and D12 can be "
                 "reopened."
               : "FAULTED. Unbinding punches a hole, so sparse residency "
                 "cannot be built on this without a way to re-establish "
                 "the sparse entry, and D12 stays closed — now with a "
                 "measurement behind it.");
    }

done_mem:
    if (mid)
        horizon_gpu_vm_unmap(mid);
    if (mem)
        horizon_gpu_mem_destroy(mem);
done:
    t_check(t, horizon_gpu_succeeded(horizon_gpu_vm_release(sp)),
            "the sparse reservation released");
    buf_destroy(&cmd);
    res = horizon_gpu_device_destroy(dev);
    t_check(t, horizon_gpu_succeeded(res), "device_destroy (status=%s)",
            horizon_gpu_status_str(res.status));
    return 0;
}
