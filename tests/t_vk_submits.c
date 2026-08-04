/*
 * Phase 5 item 9 — several submits in flight at the same time.
 *
 * The exit criterion for this item is not "nothing went wrong". It is
 * two or more submits in flight with no CPU wait between them,
 * demonstrated. This test demonstrates it twice, in two different
 * currencies, because either one alone can be argued with.
 *
 * MEASUREMENT 1 — a fence that is still unsignalled after every submit
 * has been issued. Eight compute jobs, each calibrated to take tens of
 * milliseconds, are submitted back to back with nothing between them.
 * Immediately after the eighth vkQueueSubmit returns, the FIRST job's
 * fence is read: it must be VK_NOT_READY. That is the statement
 * directly — job 1 had not finished, jobs 2 to 8 were already
 * submitted, so eight submits were outstanding at one instant. The
 * time spent issuing all eight is also compared against the time they
 * took to run: issuing must be a small fraction, because a backend that
 * waited after each submit would have spent the whole execution time
 * inside the loop.
 *
 * MEASUREMENT 2 — wall clock, on work small enough that latency is all
 * there is. Sixteen empty command buffers submitted all at once and
 * then waited for, against the same sixteen submitted and waited for
 * one at a time. If submission were synchronous the two would cost the
 * same; they do not, and the ratio is what asynchronous submission is
 * worth here. This is also where the per-submit overhead shows: every
 * submit in this project carries an L2-invalidate prologue and a
 * syncpoint-increment epilogue, and the serial figure divided by
 * sixteen is a round trip with both of them in it.
 *
 * NOTHING PRINTS INSIDE A TIMED REGION. t_check writes to stdout and to
 * a file on the SD card, so a check inside the loop being timed would
 * be measuring the SD card. Every timed loop below calls Vulkan
 * directly and stores its results in an array; the checks happen
 * afterwards, on what the array holds.
 *
 * CALIBRATION, SO THE MEASUREMENT DOES NOT DEPEND ON THE CLOCK. One
 * job is run and timed first, and the trip count for the eight is
 * chosen from it. A hard-coded trip count would be either too short to
 * observe anything on a fast clock or long enough to trip the channel
 * watchdog on a slow one. The calibration figures are printed, so the
 * run says what it actually did rather than what it intended.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <string.h>

#include <switch.h>

#include "common/vkfw.h"

#include "comp_spin.spv.h"

const char *const test_name = "t_vk_submits";
/* No display: main() starts a console and reports through it. */
const bool test_uses_display = false;

#define WORDS        4096u                  /* per job */
#define BUF_B        (WORDS * 4u)
#define GROUPS       (WORDS / 64u)          /* LocalSize is 64 */
#define POISON       0xdeadbeefu

/* Eight jobs. Enough that "several in flight" is not two by luck, few
 * enough that the total stays well inside any channel watchdog. */
#define JOBS         8u

/* Sixteen empty submits for measurement 2. Small work, so the figure
 * is round-trip latency and not throughput. */
#define TRIVIAL      16u

/* What one job should take. Long enough that the submit loop cannot
 * possibly outlast it, short enough that eight of them plus the
 * calibration stay under a second of GPU time. */
#define TARGET_JOB_NS   UINT64_C(60000000)   /* 60 ms */

/* Big enough that the shader dominates the round trip it is measured
 * through. A calibration whose time was mostly submit latency would
 * scale the trip count down and produce jobs far shorter than the
 * target -- the direction that loses the measurement. */
#define CALIBRATE_ITERS 200000u
#define MIN_ITERS       10000u

/* The ceiling is set by the CPU, not the GPU: the results are checked
 * by running the same recurrence here, so eight jobs at four million
 * iterations cost about a third of a second of verification. Nothing
 * on this chip should need a trip count anywhere near it. */
#define MAX_ITERS       4000000u

/* The recurrence comp_spin runs, so the CPU can reproduce an
 * invocation's answer. uint32_t wraps, which is the same arithmetic the
 * shader does. */
static uint32_t spin(uint32_t seed, uint32_t iters)
{
   uint32_t acc = seed;
   for (uint32_t i = 0; i < iters; i++)
      acc = acc * 1664525u + 1013904223u;
   return acc;
}

/* Push constants: trip count, then the per-job seed bias that makes
 * each job's output distinguishable from every other's. */
struct push_data {
   uint32_t iters;
   uint32_t bias;
};

static uint64_t now_ns(void)
{
   return armTicksToNs(armGetSystemTick());
}

/* One job's resources. Each job gets its own buffer and its own
 * descriptor set: eight submits are in flight at once, so nothing they
 * touch may be shared and rewritten between them. */
struct job {
   vkfw_buffer buf;
   VkDescriptorSet set;
   VkCommandBuffer cb;
   VkFence fence;
   VkResult submit_result;
   struct push_data pc;
};

/* Records a dispatch into `j`. Leaves the command buffer ended and
 * ready to submit; does not submit it, because when it is submitted is
 * the thing being measured. */
static bool record_job(vkfw *fw, VkPipeline pipeline,
                       VkPipelineLayout layout, struct job *j)
{
   test_ctx *t = fw->t;

   if (!vkfw_cmd_begin(fw, &j->cb))
      return false;

   fw->vk.vkCmdBindPipeline(j->cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
   fw->vk.vkCmdBindDescriptorSets(j->cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                                  layout, 0, 1, &j->set, 0, NULL);
   fw->vk.vkCmdPushConstants(j->cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                             (uint32_t)sizeof(j->pc), &j->pc);
   fw->vk.vkCmdDispatch(j->cb, GROUPS, 1, 1);

   const VkMemoryBarrier mb = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
   };
   fw->vk.vkCmdPipelineBarrier(j->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_HOST_BIT, 0,
                               1, &mb, 0, NULL, 0, NULL);

   VkResult r = fw->vk.vkEndCommandBuffer(j->cb);
   return t_check(t, r == VK_SUCCESS, "vkEndCommandBuffer(job) -> %s",
                  vkfw_result_str(r));
}

/* Spot-checks a job's output. Eight invocations, not four thousand:
 * reproducing every one of them costs the CPU what the GPU spent, and
 * the recurrence is a bijection, so a wrong trip count or a wrong seed
 * misses on the first one. */
#define SPOT_CHECKS 8u

static void check_job(vkfw *fw, const struct job *j, uint32_t index)
{
   const uint32_t *out = (const uint32_t *)j->buf.map;
   uint32_t wrong = 0, first = 0;

   for (uint32_t k = 0; k < SPOT_CHECKS; k++) {
      const uint32_t id = k * (WORDS / SPOT_CHECKS);
      const uint32_t want = spin(id + j->pc.bias, j->pc.iters);
      if (out[id] != want) {
         if (wrong == 0)
            first = id;
         wrong++;
      }
   }
   t_check(fw->t, wrong == 0,
           "job %u: %u/%u spot-checked invocations are right",
           index, SPOT_CHECKS - wrong, SPOT_CHECKS);
   if (wrong != 0) {
      t_note(fw->t, "job %u: invocation %u got 0x%08x, want 0x%08x",
             index, first, out[first], spin(first + j->pc.bias, j->pc.iters));
   }

   /* And the poison is gone everywhere, so "the dispatch ran" covers
    * all 4096 words and not only the eight that were recomputed. A
    * genuine result could collide with the poison by chance -- 32768
    * words against 2^32 values, about one run in 130000 -- which is
    * rare enough to note here and not worth designing around. */
   uint32_t left = 0;
   for (uint32_t i = 0; i < WORDS; i++)
      if (out[i] == POISON)
         left++;
   t_check(fw->t, left == 0,
           "job %u: no word still holds the poison (%u do)", index, left);
}

int run_test(test_ctx *t)
{
   vkfw fw;
   if (!vkfw_init(&fw, t, NULL))
      return 1;

   VkShaderModule module = VK_NULL_HANDLE;
   VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
   VkPipelineLayout layout = VK_NULL_HANDLE;
   VkPipeline pipeline = VK_NULL_HANDLE;
   VkDescriptorPool pool = VK_NULL_HANDLE;
   struct job jobs[JOBS];
   VkCommandBuffer trivial_cb[TRIVIAL];
   VkFence trivial_fence[TRIVIAL];
   VkResult r;

   memset(jobs, 0, sizeof(jobs));
   for (uint32_t i = 0; i < TRIVIAL; i++) {
      trivial_cb[i] = VK_NULL_HANDLE;
      trivial_fence[i] = VK_NULL_HANDLE;
   }

   /* ---- pipeline ------------------------------------------------- */
   const VkShaderModuleCreateInfo smci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(comp_spin_spv),
      .pCode = comp_spin_spv,
   };
   r = fw.vk.vkCreateShaderModule(fw.dev, &smci, NULL, &module);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateShaderModule -> %s",
                vkfw_result_str(r)))
      goto out;

   const VkDescriptorSetLayoutBinding binding = {
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
   };
   const VkDescriptorSetLayoutCreateInfo dslci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = &binding,
   };
   r = fw.vk.vkCreateDescriptorSetLayout(fw.dev, &dslci, NULL, &set_layout);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateDescriptorSetLayout -> %s",
                vkfw_result_str(r)))
      goto out;

   const VkPushConstantRange pcr = {
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      .offset = 0,
      .size = (uint32_t)sizeof(struct push_data),
   };
   const VkPipelineLayoutCreateInfo plci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &set_layout,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &pcr,
   };
   r = fw.vk.vkCreatePipelineLayout(fw.dev, &plci, NULL, &layout);
   if (!t_check(t, r == VK_SUCCESS, "vkCreatePipelineLayout -> %s",
                vkfw_result_str(r)))
      goto out;

   const VkComputePipelineCreateInfo cpci = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_COMPUTE_BIT,
         .module = module,
         .pName = "main",
      },
      .layout = layout,
   };
   r = fw.vk.vkCreateComputePipelines(fw.dev, VK_NULL_HANDLE, 1, &cpci, NULL,
                                      &pipeline);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateComputePipelines -> %s",
                vkfw_result_str(r)))
      goto out;

   /* ---- one buffer and one descriptor set per job ---------------- */
   const VkDescriptorPoolSize psize = {
      .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = JOBS,
   };
   const VkDescriptorPoolCreateInfo dpci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = JOBS,
      .poolSizeCount = 1,
      .pPoolSizes = &psize,
   };
   r = fw.vk.vkCreateDescriptorPool(fw.dev, &dpci, NULL, &pool);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateDescriptorPool -> %s",
                vkfw_result_str(r)))
      goto out;

   for (uint32_t i = 0; i < JOBS; i++) {
      if (!vkfw_buffer_create(&fw, BUF_B,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                              &jobs[i].buf))
         goto out;

      const VkDescriptorSetAllocateInfo dsai = {
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
         .descriptorPool = pool,
         .descriptorSetCount = 1,
         .pSetLayouts = &set_layout,
      };
      r = fw.vk.vkAllocateDescriptorSets(fw.dev, &dsai, &jobs[i].set);
      if (!t_check(t, r == VK_SUCCESS, "vkAllocateDescriptorSets(%u) -> %s",
                   i, vkfw_result_str(r)))
         goto out;

      const VkDescriptorBufferInfo dbi = {
         .buffer = jobs[i].buf.buf, .offset = 0, .range = BUF_B,
      };
      const VkWriteDescriptorSet write = {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = jobs[i].set,
         .dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &dbi,
      };
      fw.vk.vkUpdateDescriptorSets(fw.dev, 1, &write, 0, NULL);

      const VkFenceCreateInfo fci = {
         .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
      };
      r = fw.vk.vkCreateFence(fw.dev, &fci, NULL, &jobs[i].fence);
      if (!t_check(t, r == VK_SUCCESS, "vkCreateFence(job %u) -> %s", i,
                   vkfw_result_str(r)))
         goto out;
   }

   /* ---- calibration ---------------------------------------------- */
   jobs[0].pc = (struct push_data){ .iters = CALIBRATE_ITERS, .bias = 0 };
   if (!vkfw_buffer_poison(&fw, &jobs[0].buf, POISON))
      goto out;
   if (!record_job(&fw, pipeline, layout, &jobs[0]))
      goto out;

   const uint64_t cal_start = now_ns();
   const VkSubmitInfo cal_si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &jobs[0].cb,
   };
   r = fw.vk.vkQueueSubmit(fw.queue, 1, &cal_si, jobs[0].fence);
   if (!t_check(t, r == VK_SUCCESS, "calibration: vkQueueSubmit -> %s",
                vkfw_result_str(r)))
      goto out;
   r = fw.vk.vkWaitForFences(fw.dev, 1, &jobs[0].fence, VK_TRUE,
                             UINT64_C(5000000000));
   const uint64_t cal_ns = now_ns() - cal_start;
   fw.last_submit_result = r;
   if (!t_check(t, r == VK_SUCCESS, "calibration: vkWaitForFences -> %s",
                vkfw_result_str(r)))
      goto out;
   if (!vkfw_buffer_invalidate(&fw, &jobs[0].buf))
      goto out;
   check_job(&fw, &jobs[0], 0);

   /* Scale the trip count so a job lasts about TARGET_JOB_NS. The
    * calibration figure includes the round trip, not just the shader,
    * which makes the estimate slightly pessimistic -- jobs come out a
    * little longer than asked for, which is the safe direction. */
   uint32_t iters = CALIBRATE_ITERS;
   if (cal_ns > 0) {
      const uint64_t scaled =
         (uint64_t)CALIBRATE_ITERS * TARGET_JOB_NS / cal_ns;
      iters = scaled < MIN_ITERS ? MIN_ITERS
            : (scaled > MAX_ITERS ? MAX_ITERS : (uint32_t)scaled);
   }
   t_note(t, "calibration: %u iterations over %u invocations took %llu us; "
             "using %u iterations for a target of %llu us per job",
          CALIBRATE_ITERS, WORDS, (unsigned long long)(cal_ns / 1000),
          iters, (unsigned long long)(TARGET_JOB_NS / 1000));

   /* ---- measurement 1: eight jobs, no wait between them ---------- */
   for (uint32_t i = 0; i < JOBS; i++) {
      jobs[i].pc = (struct push_data){ .iters = iters, .bias = i * 7919u };
      jobs[i].submit_result = VK_NOT_READY;   /* overwritten below */
      if (!vkfw_buffer_poison(&fw, &jobs[i].buf, POISON))
         goto out;
      /* jobs[0]'s command buffer and fence were used by the
       * calibration and cannot be reused: a one-time-submit command
       * buffer is spent, and a fence that has been signalled stays
       * signalled until it is reset. */
      if (i == 0) {
         fw.vk.vkDestroyFence(fw.dev, jobs[0].fence, NULL);
         jobs[0].fence = VK_NULL_HANDLE;
         const VkFenceCreateInfo fci = {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
         };
         r = fw.vk.vkCreateFence(fw.dev, &fci, NULL, &jobs[0].fence);
         if (!t_check(t, r == VK_SUCCESS, "vkCreateFence(job 0, again) -> %s",
                      vkfw_result_str(r)))
            goto out;
      }
      if (!record_job(&fw, pipeline, layout, &jobs[i]))
         goto out;
   }

   VkSubmitInfo si[JOBS];
   for (uint32_t i = 0; i < JOBS; i++) {
      si[i] = (VkSubmitInfo){
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .commandBufferCount = 1,
         .pCommandBuffers = &jobs[i].cb,
      };
   }

   /* THE TIMED REGION. Nothing in here prints, allocates or checks:
    * every submit's result goes into the job and is examined after. */
   const uint64_t issue_start = now_ns();
   for (uint32_t i = 0; i < JOBS; i++)
      jobs[i].submit_result =
         fw.vk.vkQueueSubmit(fw.queue, 1, &si[i], jobs[i].fence);
   const uint64_t issue_end = now_ns();
   const VkResult first_status =
      fw.vk.vkGetFenceStatus(fw.dev, jobs[0].fence);
   /* ------------------------------------------------------------- */

   VkFence all[JOBS];
   for (uint32_t i = 0; i < JOBS; i++)
      all[i] = jobs[i].fence;
   r = fw.vk.vkWaitForFences(fw.dev, JOBS, all, VK_TRUE,
                             UINT64_C(10000000000));
   const uint64_t run_end = now_ns();
   fw.last_submit_result = r;

   uint32_t refused = 0;
   for (uint32_t i = 0; i < JOBS; i++)
      if (jobs[i].submit_result != VK_SUCCESS)
         refused++;
   t_check(t, refused == 0,
           "all %u submits were accepted with no wait between them "
           "(%u refused)", JOBS, refused);

   t_check(t, r == VK_SUCCESS, "vkWaitForFences(all %u) -> %s", JOBS,
           vkfw_result_str(r));

   const uint64_t issue_ns = issue_end - issue_start;
   const uint64_t run_ns = run_end - issue_start;

   /* THE ITEM, STATED DIRECTLY. Job 0 had not finished at the moment
    * job 7 had already been submitted, so eight submits were
    * outstanding at once. VK_SUCCESS here would mean the first job
    * completed while the loop was still issuing -- not a driver bug,
    * but a measurement that did not get to observe what it came for,
    * and it is reported as a failure rather than quietly passing. */
   t_check(t, first_status == VK_NOT_READY,
           "job 0 was still running when all %u submits had been issued "
           "(vkGetFenceStatus -> %s)", JOBS, vkfw_result_str(first_status));

   /* And the quantitative half: issuing cost a small fraction of
    * running. A backend that waited after each submit would have spent
    * essentially all of run_ns inside the issue loop. */
   t_check(t, issue_ns * 4 < run_ns,
           "issuing %u submits took %llu us against %llu us of execution "
           "(under a quarter)", JOBS,
           (unsigned long long)(issue_ns / 1000),
           (unsigned long long)(run_ns / 1000));
   t_note(t, "issue %llu us, total %llu us, %llu us per job",
          (unsigned long long)(issue_ns / 1000),
          (unsigned long long)(run_ns / 1000),
          (unsigned long long)(run_ns / 1000 / JOBS));

   if (r == VK_SUCCESS) {
      for (uint32_t i = 0; i < JOBS; i++) {
         if (!vkfw_buffer_invalidate(&fw, &jobs[i].buf))
            break;
         check_job(&fw, &jobs[i], i);
      }
   }

   /* ---- measurement 2: latency, batched against serialised ------ */
   if (vkfw_device_lost(&fw)) {
      t_note(t, "device lost; the latency measurement was not attempted");
      goto out;
   }

   for (uint32_t i = 0; i < TRIVIAL; i++) {
      const VkFenceCreateInfo fci = {
         .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
      };
      r = fw.vk.vkCreateFence(fw.dev, &fci, NULL, &trivial_fence[i]);
      if (!t_check(t, r == VK_SUCCESS, "vkCreateFence(trivial %u) -> %s", i,
                   vkfw_result_str(r)))
         goto out;
   }

   uint64_t batch_ns = 0, serial_ns = 0;
   VkResult batch_wait = VK_SUCCESS, serial_worst = VK_SUCCESS;

   for (uint32_t round = 0; round < 2; round++) {
      /* Fresh command buffers each round: ONE_TIME_SUBMIT means a
       * recorded buffer is spent once it has been submitted. */
      for (uint32_t i = 0; i < TRIVIAL; i++) {
         if (!vkfw_cmd_begin(&fw, &trivial_cb[i]))
            goto out;
         VkResult e = fw.vk.vkEndCommandBuffer(trivial_cb[i]);
         if (!t_check(t, e == VK_SUCCESS,
                      "vkEndCommandBuffer(trivial %u) -> %s", i,
                      vkfw_result_str(e)))
            goto out;
      }
      r = fw.vk.vkResetFences(fw.dev, TRIVIAL, trivial_fence);
      if (!t_check(t, r == VK_SUCCESS, "vkResetFences(%u) -> %s", TRIVIAL,
                   vkfw_result_str(r)))
         goto out;

      VkSubmitInfo tsi[TRIVIAL];
      for (uint32_t i = 0; i < TRIVIAL; i++) {
         tsi[i] = (VkSubmitInfo){
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &trivial_cb[i],
         };
      }

      if (round == 0) {
         /* Timed: submit all, then wait once. */
         const uint64_t t0 = now_ns();
         for (uint32_t i = 0; i < TRIVIAL; i++)
            fw.vk.vkQueueSubmit(fw.queue, 1, &tsi[i], trivial_fence[i]);
         batch_wait = fw.vk.vkWaitForFences(fw.dev, TRIVIAL, trivial_fence,
                                            VK_TRUE, UINT64_C(5000000000));
         batch_ns = now_ns() - t0;
      } else {
         /* Timed: submit one, wait for it, repeat. */
         const uint64_t t0 = now_ns();
         for (uint32_t i = 0; i < TRIVIAL; i++) {
            fw.vk.vkQueueSubmit(fw.queue, 1, &tsi[i], trivial_fence[i]);
            const VkResult w =
               fw.vk.vkWaitForFences(fw.dev, 1, &trivial_fence[i], VK_TRUE,
                                     UINT64_C(5000000000));
            if (w != VK_SUCCESS)
               serial_worst = w;
         }
         serial_ns = now_ns() - t0;
      }
   }

   t_check(t, batch_wait == VK_SUCCESS,
           "batched: vkWaitForFences(all %u) -> %s", TRIVIAL,
           vkfw_result_str(batch_wait));
   t_check(t, serial_worst == VK_SUCCESS,
           "serialised: every vkWaitForFences succeeded (worst %s)",
           vkfw_result_str(serial_worst));

   t_note(t, "%u empty submits: %llu us batched, %llu us one at a time "
             "(%llu us per round trip serialised)",
          TRIVIAL, (unsigned long long)(batch_ns / 1000),
          (unsigned long long)(serial_ns / 1000),
          (unsigned long long)(serial_ns / 1000 / TRIVIAL));

   /* If submission were synchronous the two figures would be the same.
    * They are compared with a wide margin because this is latency on a
    * shared console, not a benchmark: the claim is only that batching
    * is cheaper, not by how much. */
   t_check(t, batch_ns < serial_ns,
           "batching %u submits beat submitting them one at a time "
           "(%llu us against %llu us)", TRIVIAL,
           (unsigned long long)(batch_ns / 1000),
           (unsigned long long)(serial_ns / 1000));

out:
   for (uint32_t i = 0; i < TRIVIAL; i++)
      if (trivial_fence[i] != VK_NULL_HANDLE)
         fw.vk.vkDestroyFence(fw.dev, trivial_fence[i], NULL);
   for (uint32_t i = 0; i < JOBS; i++) {
      if (jobs[i].fence != VK_NULL_HANDLE)
         fw.vk.vkDestroyFence(fw.dev, jobs[i].fence, NULL);
      vkfw_buffer_destroy(&fw, &jobs[i].buf);
   }
   if (pool != VK_NULL_HANDLE)
      fw.vk.vkDestroyDescriptorPool(fw.dev, pool, NULL);
   if (pipeline != VK_NULL_HANDLE)
      fw.vk.vkDestroyPipeline(fw.dev, pipeline, NULL);
   if (layout != VK_NULL_HANDLE)
      fw.vk.vkDestroyPipelineLayout(fw.dev, layout, NULL);
   if (set_layout != VK_NULL_HANDLE)
      fw.vk.vkDestroyDescriptorSetLayout(fw.dev, set_layout, NULL);
   if (module != VK_NULL_HANDLE)
      fw.vk.vkDestroyShaderModule(fw.dev, module, NULL);
   vkfw_finish(&fw);
   return 0;
}
