/*
 * How large a single GPFIFO entry this hardware will execute — the
 * measurement decision D15 turns on.
 *
 * WHAT D15 ASKED. `nxvk` calibrates a freshly created channel by
 * kicking off synthetic fence-only push buffers of increasing size (32,
 * 128, 512, … 8192 dwords) and CPU-waiting each, so that a ring-size
 * fault is pinned to the size that caused it at channel-creation time
 * rather than surfacing later as a mystery
 * (`docs/reference-analysis.md` § 12.5.2). D15 recorded the idea as
 * genuinely new and left it undesigned.
 *
 * WHY THIS PROJECT SHOULD NOT ADOPT THE RAMP AND SHOULD ADOPT THE
 * NUMBER. Two reasons.
 *
 * The failure the ramp diagnoses is half-covered already: the entry
 * *count* is bounded by construction here — horizon_gpu_submit refuses
 * a submit whose spans plus its own two entries would not fit
 * GPFIFO_QUEUE_SIZE, and t_submit measures that. What is not bounded is
 * the *size in dwords of one entry*: horizon_gpu_submit checks only
 * that it is non-zero. That is the real gap, and a ramp at every
 * channel creation is not what closes it — a number is, checked once,
 * against a bound in the code.
 *
 * And the ramp costs a CPU wait per rung at every channel creation.
 * CLAUDE.md permits a stall only where Vulkan's semantics require one
 * or in the documented debug-synchronous mode; a bring-up diagnostic is
 * neither, and it is exactly the sort of cost that gets adopted once
 * and never removed.
 *
 * So the ramp lives here, in a test, run once by a person who wants the
 * number — not in horizon/channel/, run forever by everybody.
 *
 * WHAT THIS MEASURES, AND WHY EACH RUNG IS VERIFIED RATHER THAN
 * COUNTED. A rung is a push buffer of N dwords: N-4 dwords of NOP
 * methods followed by a semaphore release of a payload unique to that
 * rung. Accepting the submit proves nothing — the interesting failure
 * is an entry the channel takes and then does not finish. So each rung
 * waits for its fence and then reads the payload back, and a rung
 * passes only when the last dword of a very long entry actually
 * executed.
 *
 * Each rung gets a fresh channel. A rung that faults loses its channel,
 * and a lost channel would fail every rung after it for a reason that
 * has nothing to do with size.
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

const char *const test_name = "t_pbsize";
/* No display: main() starts a console and reports through it. */
const bool test_uses_display = false;

#define WAIT_NS      UINT64_C(3000000000)
#define TARGET_B     UINT64_C(0x1000)

/* The rungs, in dwords. Doubling from nxvk's smallest to well past its
 * largest: the point of going further is that 8192 is where somebody
 * else stopped looking, not where the hardware stops. */
static const uint32_t RUNGS[] = {
   32, 128, 512, 2048, 8192, 32768, 131072, 524288,
};
#define NUM_RUNGS (sizeof(RUNGS) / sizeof(RUNGS[0]))

/* Capacity for the largest rung, in bytes. */
#define PB_B  ((uint64_t)524288u * 4u)

typedef struct region {
   horizon_gpu_mem *mem;
   horizon_gpu_va_range *range;
   horizon_gpu_mapping *map;
   uint64_t gpu_va;
   uint32_t *cpu;
} region;

static horizon_gpu_result region_create(horizon_gpu_device *dev,
                                        uint64_t size_B, region *out)
{
   horizon_gpu_result res =
      horizon_gpu_mem_create(dev, size_B, 0, HORIZON_GPU_MEM_CACHED,
                             &out->mem);
   if (horizon_gpu_failed(res))
      return res;
   /* Reserve generously and align to the small page: the push buffer is
    * half a megabyte at the top rung. */
   res = horizon_gpu_vm_reserve(dev, size_B + 0x10000, 0x1000, 0,
                                &out->range);
   if (horizon_gpu_failed(res))
      return res;
   res = horizon_gpu_vm_map(out->range, 0, out->mem, 0, size_B,
                            HORIZON_GPU_PTE_KIND_PITCH, true, &out->map);
   if (horizon_gpu_failed(res))
      return res;
   out->gpu_va = horizon_gpu_mapping_va(out->map);
   out->cpu = horizon_gpu_mem_cpu_ptr(out->mem);
   return horizon_gpu_ok();
}

static void region_destroy(region *r)
{
   if (r->map != NULL)
      horizon_gpu_vm_unmap(r->map);
   if (r->range != NULL)
      horizon_gpu_vm_release(r->range);
   if (r->mem != NULL)
      horizon_gpu_mem_destroy(r->mem);
   r->map = NULL;
   r->range = NULL;
   r->mem = NULL;
}

/* Returns true when the rung executed to its last dword. */
static bool run_rung(test_ctx *t, horizon_gpu_device *dev, region *pb,
                     region *tgt, uint32_t dwords)
{
   horizon_gpu_channel *chan = NULL;
   bool ok = false;

   horizon_gpu_result res = horizon_gpu_channel_create(dev, NULL, &chan);
   if (!t_check(t, horizon_gpu_succeeded(res),
                "%u dwords: channel create (status=%s)", dwords,
                horizon_gpu_status_str(res.status)))
      return false;

   /* NOP filler, then the release. The payload names the rung, so a
    * stale value from the previous one cannot be mistaken for this
    * one's. */
   const uint32_t payload = 0x5a5a0000u | (dwords & 0xffffu);
   const uint32_t rel_dwords = HORIZON_CMDS_SEM_RELEASE_DWORDS;
   if (!t_check(t, dwords > rel_dwords + 2u,
                "%u dwords: the rung has room for filler and a release",
                dwords))
      goto out;

   const uint32_t filler_dwords = dwords - rel_dwords;
   const uint32_t pairs = filler_dwords / 2u;
   const uint32_t n_nop = horizon_cmds_nop(pb->cpu, filler_dwords, pairs);
   if (!t_check(t, n_nop == pairs * 2u,
                "%u dwords: %u NOP pair(s) encoded (%u dwords)", dwords,
                pairs, n_nop))
      goto out;

   const uint32_t n_rel =
      horizon_cmds_semaphore_release(pb->cpu + n_nop, tgt->gpu_va, payload);
   if (!t_check(t, n_rel == rel_dwords,
                "%u dwords: the release encoded at the end", dwords))
      goto out;

   const uint32_t total = n_nop + n_rel;

   tgt->cpu[0] = 0;
   res = horizon_gpu_mem_flush(tgt->mem, 0, TARGET_B);
   if (horizon_gpu_failed(res))
      goto out;
   res = horizon_gpu_mem_flush(pb->mem, 0, (uint64_t)total * 4u);
   if (!t_check(t, horizon_gpu_succeeded(res),
                "%u dwords: push buffer flushed (status=%s)", dwords,
                horizon_gpu_status_str(res.status)))
      goto out;

   const horizon_gpu_cmd_span span = {
      .gpu_va = pb->gpu_va,
      .num_dwords = total,
   };
   horizon_gpu_fence fence = { 0 };
   res = horizon_gpu_submit(chan, &span, 1, HORIZON_GPU_SUBMIT_DEFAULT,
                            &fence);
   if (!t_check(t, horizon_gpu_succeeded(res),
                "%u dwords: the submit is accepted (status=%s nv=0x%08x)",
                dwords, horizon_gpu_status_str(res.status), res.nv))
      goto out;

   res = horizon_gpu_channel_wait_fence(chan, fence, WAIT_NS);
   if (!t_check(t, horizon_gpu_succeeded(res),
                "%u dwords: the entry completes (status=%s)", dwords,
                horizon_gpu_status_str(res.status))) {
      uint32_t err_type = 0;
      const char *desc = "none";
      (void)horizon_gpu_channel_get_error(chan, &err_type, &desc);
      t_note(t, "%u dwords: notifier type=%u '%s'", dwords, err_type, desc);
      goto out;
   }

   res = horizon_gpu_mem_invalidate(tgt->mem, 0, TARGET_B);
   if (horizon_gpu_failed(res))
      goto out;

   /* THE POINT. Acceptance is not execution: this is the last dword of
    * the entry arriving in memory. */
   ok = t_check(t, tgt->cpu[0] == payload,
                "%u dwords: the release at the END of the entry ran "
                "(0x%08" PRIx32 ")", dwords, tgt->cpu[0]);

out:
   if (chan != NULL)
      (void)horizon_gpu_channel_destroy(chan);
   return ok;
}

int run_test(test_ctx *t)
{
   horizon_gpu_device *dev = NULL;
   region pb = { 0 }, tgt = { 0 };

   horizon_gpu_result res = horizon_gpu_device_create(NULL, &dev);
   if (!t_check(t, horizon_gpu_succeeded(res), "device create (status=%s)",
                horizon_gpu_status_str(res.status)))
      return 1;

   res = region_create(dev, PB_B, &pb);
   if (!t_check(t, horizon_gpu_succeeded(res),
                "a %" PRIu64 "-byte push buffer is mapped (status=%s)",
                PB_B, horizon_gpu_status_str(res.status)))
      goto out;

   res = region_create(dev, TARGET_B, &tgt);
   if (!t_check(t, horizon_gpu_succeeded(res),
                "the release target is mapped (status=%s)",
                horizon_gpu_status_str(res.status)))
      goto out;

   uint32_t largest = 0;
   uint32_t first_failed = 0;
   for (uint32_t i = 0; i < NUM_RUNGS; i++) {
      if (run_rung(t, dev, &pb, &tgt, RUNGS[i])) {
         largest = RUNGS[i];
      } else {
         first_failed = RUNGS[i];
         t_note(t, "stopping the ramp at %u dwords: a rung that fails "
                   "makes every larger one fail for the same reason",
                first_failed);
         break;
      }
   }

   /* The number D15 wanted, stated plainly whichever way it came out. */
   if (first_failed == 0) {
      t_note(t, "D15: every rung up to %u dwords (%u KiB) executed to its "
                "last dword; no entry-size limit was found in this range",
             largest, largest / 256u);
   } else {
      t_note(t, "D15: the largest entry that executed is %u dwords "
                "(%u KiB); %u dwords did not. horizon_gpu_submit should "
                "refuse spans above the limit rather than let the channel "
                "find it", largest, largest / 256u, first_failed);
   }
   t_check(t, largest >= 8192u,
           "entries at least as large as nxvk's top rung (8192 dwords) "
           "execute; the largest measured here is %u", largest);

out:
   region_destroy(&tgt);
   region_destroy(&pb);
   if (dev != NULL) {
      res = horizon_gpu_device_destroy(dev);
      t_check(t, horizon_gpu_succeeded(res), "device destroy (status=%s)",
              horizon_gpu_status_str(res.status));
   }
   return 0;
}
