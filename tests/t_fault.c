/*
 * A deliberate MMU fault, and the fence that must not lie about it.
 *
 * WHY THIS EXISTS. horizon_gpu_channel_wait_fence re-checks the
 * channel's error notifier and reports HORIZON_GPU_ERR_CHANNEL_LOST
 * instead of success when the channel has faulted. That code was
 * written because of a real hardware run — 2026-08-04, t_vk_image run
 * 1, where vkWaitForFences returned VK_SUCCESS for work that had
 * MMU-faulted and written nothing — and since it was fixed, **nothing
 * has faulted**. A check that has never fired is not a check. This test
 * makes one fire on purpose.
 *
 * The mechanism nvgpu uses is what makes the lie possible: when a
 * channel faults, its recovery force-increments the channel's
 * syncpoints so that everything waiting on them stops waiting. A fence
 * therefore gets *reached* whether the work ran or not, and a wait that
 * only looks at the syncpoint cannot tell the two apart.
 *
 * HOW THE FAULT IS PRODUCED. A VA range is reserved and nothing is
 * bound into it, then a semaphore release is aimed at its base. The
 * address is valid to name and maps nothing, so the GPU takes an MMU
 * fault on the write. That is a data fault in the engine, not a fetch
 * fault in the command processor: the push buffer itself stays mapped
 * and correct, so what is being tested is the notifier path and not the
 * kernel's reaction to an unreadable channel.
 *
 * WHAT IS ASSERTED, IN ORDER:
 *
 *   1. the submit is accepted — the fault happens on the GPU, later,
 *      not at kickoff;
 *   2. the wait does NOT report success. This is the whole point. A
 *      pass here before the fix would have been a failure;
 *   3. the notifier still names the fault *after* the wait has
 *      consumed it. Run 1 failed exactly here — see the comment at that
 *      check — and the fix is a latch in horizon_gpu_channel_get_error.
 *      The decoded description is printed either way, so a future
 *      reader knows which fault this hardware raises: **31, MMU
 *      fault**, measured 2026-08-04;
 *   4. a submit on the lost channel is refused rather than accepted
 *      into a channel that can no longer run anything;
 *   5. the channel and the device tear down cleanly afterwards, which
 *      is the part a fault could plausibly break and which no other
 *      test can reach.
 *
 * THIS TEST INTENTIONALLY DESTROYS ITS CHANNEL. It creates one of its
 * own for that reason and never shares it, so nothing else in the
 * binary is affected — and it is the last thing the binary does.
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

const char *const test_name = "t_fault";
/* No display: main() starts a console and reports through it. */
const bool test_uses_display = false;

/* Generous: the fault has to be raised by the GPU, noticed by nvgpu,
 * and written into the notifier. Long enough that a slow path is not
 * mistaken for a hang, short enough that a hang is still reported. */
#define WAIT_NS       UINT64_C(3000000000)
#define CMDBUF_B      UINT64_C(0x1000)
#define UNMAPPED_B    UINT64_C(0x10000)
#define PAYLOAD       UINT32_C(0xbadf00d5)

/* A mapped scratch object, used only for the push buffer. */
typedef struct scratch {
   horizon_gpu_mem *mem;
   horizon_gpu_va_range *range;
   horizon_gpu_mapping *map;
   uint64_t gpu_va;
   uint32_t *cpu;
} scratch;

static horizon_gpu_result scratch_create(horizon_gpu_device *dev,
                                         scratch *out)
{
   horizon_gpu_result res =
      horizon_gpu_mem_create(dev, CMDBUF_B, 0, HORIZON_GPU_MEM_CACHED,
                             &out->mem);
   if (horizon_gpu_failed(res))
      return res;
   res = horizon_gpu_vm_reserve(dev, 0x10000, 0x1000, 0, &out->range);
   if (horizon_gpu_failed(res))
      return res;
   res = horizon_gpu_vm_map(out->range, 0, out->mem, 0, CMDBUF_B,
                            HORIZON_GPU_PTE_KIND_PITCH, true, &out->map);
   if (horizon_gpu_failed(res))
      return res;
   out->gpu_va = horizon_gpu_mapping_va(out->map);
   out->cpu = horizon_gpu_mem_cpu_ptr(out->mem);
   return horizon_gpu_ok();
}

static void scratch_destroy(scratch *s)
{
   if (s->map != NULL)
      horizon_gpu_vm_unmap(s->map);
   if (s->range != NULL)
      horizon_gpu_vm_release(s->range);
   if (s->mem != NULL)
      horizon_gpu_mem_destroy(s->mem);
   s->map = NULL;
   s->range = NULL;
   s->mem = NULL;
}

int run_test(test_ctx *t)
{
   horizon_gpu_device *dev = NULL;
   horizon_gpu_channel *chan = NULL;
   horizon_gpu_va_range *unmapped = NULL;
   scratch cmd = { 0 };
   horizon_gpu_result res;

   res = horizon_gpu_device_create(NULL, &dev);
   if (!t_check(t, horizon_gpu_succeeded(res), "device create (status=%s)",
                horizon_gpu_status_str(res.status)))
      return 1;

   res = horizon_gpu_channel_create(dev, NULL, &chan);
   if (!t_check(t, horizon_gpu_succeeded(res), "channel create (status=%s)",
                horizon_gpu_status_str(res.status)))
      goto out;

   res = scratch_create(dev, &cmd);
   if (!t_check(t, horizon_gpu_succeeded(res),
                "push buffer created and mapped (status=%s)",
                horizon_gpu_status_str(res.status)))
      goto out;

   /* Reserved and left empty on purpose. horizon_gpu_vm_reserve takes
    * the address space; horizon_gpu_vm_map is what would put memory
    * behind it, and is not called. */
   res = horizon_gpu_vm_reserve(dev, UNMAPPED_B, 0x1000, 0, &unmapped);
   if (!t_check(t, horizon_gpu_succeeded(res),
                "a VA range is reserved with nothing bound into it "
                "(status=%s)", horizon_gpu_status_str(res.status)))
      goto out;

   const uint64_t bad_va = horizon_gpu_va_range_base(unmapped);
   t_note(t, "aiming a semaphore release at 0x%" PRIx64 ", which names an "
             "address and maps nothing", bad_va);

   /* Sanity on the channel before the fault, so "no error recorded" is
    * a measured starting point and not an assumption. */
   uint32_t err_type = 0;
   const char *desc = "none";
   res = horizon_gpu_channel_get_error(chan, &err_type, &desc);
   t_check(t, horizon_gpu_succeeded(res) && err_type == 0,
           "the notifier is clear before the fault (type=%u '%s')",
           err_type, desc);

   uint32_t *dw = cmd.cpu;
   const uint32_t n = horizon_cmds_semaphore_release(dw, bad_va, PAYLOAD);
   if (!t_check(t, n > 0, "semaphore release encoded (%u dwords)", n))
      goto out;

   res = horizon_gpu_mem_flush(cmd.mem, 0, CMDBUF_B);
   if (!t_check(t, horizon_gpu_succeeded(res),
                "push buffer flushed (status=%s)",
                horizon_gpu_status_str(res.status)))
      goto out;

   const horizon_gpu_cmd_span span = {
      .gpu_va = cmd.gpu_va,
      .num_dwords = n,
   };
   horizon_gpu_fence fence = { 0 };
   res = horizon_gpu_submit(chan, &span, 1, HORIZON_GPU_SUBMIT_DEFAULT,
                            &fence);

   /* 1. The submit itself is fine: nothing about this command is
    * invalid, and the address it names is one the kernel is happy to
    * accept. The fault happens when the engine executes it. */
   if (!t_check(t, horizon_gpu_succeeded(res),
                "the submit is accepted — the fault is the GPU's, not the "
                "kickoff's (status=%s)", horizon_gpu_status_str(res.status)))
      goto out;
   t_note(t, "fence %u:%u", fence.syncpt_id, fence.threshold);

   /* 2. THE MEASUREMENT. nvgpu's recovery force-increments the
    * syncpoint, so the threshold is reached whether the work ran or
    * not. A wait that reports success here is reporting that a write
    * which never happened has happened. */
   res = horizon_gpu_channel_wait_fence(chan, fence, WAIT_NS);
   t_note(t, "wait_fence -> %s (nv 0x%08x)",
          horizon_gpu_status_str(res.status), res.nv);
   t_check(t, horizon_gpu_failed(res),
           "the wait does NOT report success for work that faulted");
   t_check(t, res.status == HORIZON_GPU_ERR_CHANNEL_LOST,
           "and it names the channel lost rather than timing out "
           "(status=%s)", horizon_gpu_status_str(res.status));

   /* 3. What the hardware actually raised, recorded for whoever reads
    * this log next.
    *
    * RUN 1 FAILED HERE, AND IT WAS RIGHT TO. type came back 0 for a
    * channel that had just been marked lost by an MMU fault — because
    * reading a notification consumes it, and the wait's own check had
    * already read it. The type existed only in a log line, where no
    * program could reach it. horizon_gpu_channel_get_error now latches
    * it; this check is what says the latch works. */
   err_type = 0;
   desc = "none";
   res = horizon_gpu_channel_get_error(chan, &err_type, &desc);
   t_check(t, horizon_gpu_succeeded(res) && err_type != 0,
           "the notifier recorded the fault (type=%u '%s')", err_type, desc);
   t_note(t, "notifier: type=%u '%s'", err_type, desc);

   /* 4. A lost channel refuses further work instead of quietly
    * accepting it. */
   res = horizon_gpu_submit(chan, &span, 1, HORIZON_GPU_SUBMIT_DEFAULT,
                            NULL);
   t_check(t, res.status == HORIZON_GPU_ERR_CHANNEL_LOST,
           "a submit to the lost channel is refused (status=%s)",
           horizon_gpu_status_str(res.status));

out:
   /* 5. Teardown after a fault, which nothing else exercises. Reported
    * as checks rather than done silently: a leak or a hang here would
    * otherwise show up as a mysterious failure in whatever ran next. */
   scratch_destroy(&cmd);
   if (unmapped != NULL) {
      res = horizon_gpu_vm_release(unmapped);
      t_check(t, horizon_gpu_succeeded(res),
              "the empty reservation is released after the fault "
              "(status=%s)", horizon_gpu_status_str(res.status));
   }
   if (chan != NULL) {
      res = horizon_gpu_channel_destroy(chan);
      t_check(t, horizon_gpu_succeeded(res),
              "the faulted channel tears down cleanly (status=%s)",
              horizon_gpu_status_str(res.status));
   }
   if (dev != NULL) {
      res = horizon_gpu_device_destroy(dev);
      t_check(t, horizon_gpu_succeeded(res),
              "the device tears down after a faulted channel (status=%s)",
              horizon_gpu_status_str(res.status));
   }
   return 0;
}
