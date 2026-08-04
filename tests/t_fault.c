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
 * WARNING, MEASURED 2026-08-04: RUNNING THIS CRASHES THE CONSOLE ON
 * EXIT. Every check passes (15/15), the log is written and closed, and
 * then pressing + to return to the menu takes the system down. The
 * measurement is banked; this warning is here so nobody runs it
 * casually for the passing line.
 *
 * The crash is strictly after everything this test reports. What it is
 * not: it is not the teardown failing, because the channel, the
 * reservation and the device all report ok. What it might be is a race
 * with nvgpu's own recovery. horizon_gpu_channel_destroy deliberately
 * skips the drain for a lost channel — its counter may never advance
 * again and teardown has to remain possible (architecture.md § 6) — so
 * the channel handle is closed while the kernel may still be
 * recovering it, and the process's nv session is closed by __appExit a
 * moment later.
 *
 * TWO THINGS WERE ADDED TO NARROW IT, and neither belongs in
 * horizon/: a sleep in the library would be a failure hidden behind a
 * sleep, which this project does not do.
 *
 *   - a bounded settle between the fault and the teardown. If the exit
 *     stops crashing, it is a race with recovery, and the fix is a
 *     bounded wait on something observable rather than a delay.
 *   - a fresh channel and a real submit AFTER the teardown. If the GPU
 *     is still usable in this process once a faulted channel has been
 *     torn down, the session survived and the crash belongs to process
 *     exit; if it is not, the session never recovered and the exit
 *     crash is a consequence rather than the cause.
 *
 * BOTH WERE RUN (2026-08-04, 20/20) AND THE ANSWERS ARE:
 *
 *   - the session survives completely. A second device, channel,
 *     submit and readback all work after the faulted channel is torn
 *     down, so the process is healthy when it reaches the exit path
 *     and nothing broken is being carried forward. That was the worse
 *     possibility and it is ruled out.
 *   - the settle changes nothing. It still crashes with two seconds of
 *     slack, so it is not a simple race with nvgpu's recovery.
 *
 * WHICH LEAVES THE EXIT PATH ITSELF, and the third experiment is here
 * so the next person to run this gets the answer for free rather than
 * spending a reboot on it. An atexit handler writes a marker file. It
 * runs after main returns and after consoleExit, and before libnx's
 * __appExit tears the services down. So:
 *
 *   marker present  ->  the crash is in __appExit or later — service
 *                       teardown, which for this process means giving
 *                       the GPU back to a compositor that shares it
 *   marker absent   ->  the crash is earlier: consoleExit, or the
 *                       return out of main
 *
 * Not worth a run of its own. Worth having already written down when
 * somebody runs this again.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

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
#define GOOD_PAYLOAD  UINT32_C(0x600d0001)

/* How long to let nvgpu's recovery run before closing the channel. Long
 * enough to matter if the crash is a race, short enough that a run is
 * still a run. */
#define SETTLE_NS     UINT64_C(2000000000)

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

#define EXIT_MARKER "sdmc:/horizon_gpu_tests/t_fault-exit.log"

/* Runs at exit(), after main has returned and consoleExit has run, and
 * before libnx's __appExit. Its own file, because the test's log is
 * closed by then. See the header for what its presence and absence
 * each mean. */
static void note_reached_atexit(void)
{
   FILE *f = fopen(EXIT_MARKER, "w");
   if (f == NULL)
      return;
   fprintf(f, "reached atexit: main returned and consoleExit ran.\n"
              "If the console still went down, it went down in "
              "__appExit or later.\n");
   fflush(f);
   fclose(f);
}

int run_test(test_ctx *t)
{
   /* Registered first, so it is in place however the rest of this
    * ends. */
   if (atexit(note_reached_atexit) != 0)
      t_note(t, "atexit registration failed; the exit marker will not be "
                "written and its absence means nothing this run");
   else
      t_note(t, "an atexit marker will be written to " EXIT_MARKER
                " — send it back with this log");

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

   /* 5. Let the kernel's recovery run before anything is closed. This
    * is an experiment, not a fix: if the exit crash goes away, the
    * cause is a race and the answer is to wait on something
    * observable. If it does not, the delay costs two seconds and rules
    * one thing out. */
   t_note(t, "settling for %llu ms before teardown, to see whether the "
             "exit crash is a race with nvgpu's recovery",
          (unsigned long long)(SETTLE_NS / 1000000));
   svcSleepThread(SETTLE_NS);

out:
   /* 6. Teardown after a fault, which nothing else exercises. Reported
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
      dev = NULL;
   }

   /* 7. IS THIS PROCESS'S GPU STILL USABLE? A whole new device,
    * channel and submit, after a faulted channel has been torn down.
    *
    * This is the question the exit crash turns on. If real work runs
    * here, the nv session survived the fault and the crash belongs to
    * process exit — __appExit closing a session the kernel is not
    * finished with. If it does not, the session never recovered and
    * the crash at + is a consequence of a state that was already
    * broken by the time this test said everything was ok. */
   {
      horizon_gpu_device *dev2 = NULL;
      horizon_gpu_channel *chan2 = NULL;
      scratch cmd2 = { 0 }, tgt2 = { 0 };

      res = horizon_gpu_device_create(NULL, &dev2);
      if (!t_check(t, horizon_gpu_succeeded(res),
                   "after the fault: a second device opens (status=%s)",
                   horizon_gpu_status_str(res.status)))
         goto probe_out;

      res = horizon_gpu_channel_create(dev2, NULL, &chan2);
      if (!t_check(t, horizon_gpu_succeeded(res),
                   "after the fault: a second channel opens (status=%s)",
                   horizon_gpu_status_str(res.status)))
         goto probe_out;

      if (horizon_gpu_failed(scratch_create(dev2, &cmd2)) ||
          horizon_gpu_failed(scratch_create(dev2, &tgt2))) {
         t_check(t, false, "after the fault: memory for the probe");
         goto probe_out;
      }

      tgt2.cpu[0] = 0;
      (void)horizon_gpu_mem_flush(tgt2.mem, 0, CMDBUF_B);

      const uint32_t n2 =
         horizon_cmds_semaphore_release(cmd2.cpu, tgt2.gpu_va, GOOD_PAYLOAD);
      (void)horizon_gpu_mem_flush(cmd2.mem, 0, CMDBUF_B);

      const horizon_gpu_cmd_span span2 = {
         .gpu_va = cmd2.gpu_va, .num_dwords = n2,
      };
      horizon_gpu_fence f2 = { 0 };
      res = horizon_gpu_submit(chan2, &span2, 1, HORIZON_GPU_SUBMIT_DEFAULT,
                               &f2);
      if (!t_check(t, horizon_gpu_succeeded(res),
                   "after the fault: a submit on the new channel is "
                   "accepted (status=%s)", horizon_gpu_status_str(res.status)))
         goto probe_out;

      res = horizon_gpu_channel_wait_fence(chan2, f2, WAIT_NS);
      if (!t_check(t, horizon_gpu_succeeded(res),
                   "after the fault: the new channel's work completes "
                   "(status=%s)", horizon_gpu_status_str(res.status)))
         goto probe_out;

      (void)horizon_gpu_mem_invalidate(tgt2.mem, 0, CMDBUF_B);
      t_check(t, tgt2.cpu[0] == GOOD_PAYLOAD,
              "after the fault: THE GPU STILL WORKS IN THIS PROCESS "
              "(0x%08" PRIx32 ")", tgt2.cpu[0]);

probe_out:
      scratch_destroy(&tgt2);
      scratch_destroy(&cmd2);
      if (chan2 != NULL)
         (void)horizon_gpu_channel_destroy(chan2);
      if (dev2 != NULL)
         (void)horizon_gpu_device_destroy(dev2);
   }

   t_note(t, "everything this test can measure is done. If the console "
             "goes down when you press +, it goes down after this line "
             "and after the log was closed");
   return 0;
}
