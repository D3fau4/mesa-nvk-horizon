/*
 * Several command buffers in one vkQueueSubmit, and what the backend
 * does with them.
 *
 * WHAT CHANGED UNDERNEATH THIS. nvkmd_horizon_ctx_exec used to hand its
 * spans straight to horizon_gpu_submit, so a submission of N command
 * buffers became N kickoffs — N ioctls through the nv service, and on
 * the GPU N-1 extra WFI_SCOPE_ALL drains and L2 round trips that
 * nothing had asked for. It now accumulates and sends the batch at the
 * boundaries that need it. This is the case that runs that path.
 *
 * WHAT IT CAN PROVE AND WHAT IT CANNOT. It can prove that the work all
 * arrives, in order, with the dependencies the application asked for
 * still honoured, across the shapes the backend has to get right:
 * several command buffers, more of them than the batch's own cap, a
 * submission with a wait and a signal and no work at all, and a
 * submission behind a cross-channel dependency. It CANNOT prove that
 * batching is faster: it prints the wall time of each half of section
 * E, and one console's numbers for one workload is what that is.
 *
 * THE ORDERING IS ASKED FOR, NOT ASSUMED. Command buffers in one
 * VkSubmitInfo execute in order, but Vulkan puts no implicit memory
 * dependency between them — so a test where two of them write the same
 * word without a barrier would be measuring undefined behaviour, and
 * would "pass" for the wrong reason exactly when the batching was
 * wrong. Every command buffer here opens with a real
 * vkCmdPipelineBarrier over the transfer stage. What is being checked
 * is that a dependency the application expressed survives the change,
 * which is the only ordering that was ever promised.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include "common/vkfw.h"

/* Generated from tests/shaders/comp_write_id.spvasm by
 * scripts/spv-embed.py. Section D creates a pipeline from it and never
 * dispatches it: what that section is about is the shader-heap upload
 * that creation performs on the upload queue's channel, and the wait
 * the next submission then has to take on it. Dispatching is
 * vk_shaders/compute_dispatch's job and is not repeated here. */
#include "comp_write_id.spv.h"

/* Long enough for a submission of eighty command buffers on a console
 * that is also compositing, short enough that a lost signal ends the
 * case instead of the run. */
#define SB_WAIT_NS  UINT64_C(5000000000)

/* One word per command buffer, plus one they all write. */
#define SB_MAX_CBS  80u
#define SB_SHARED   SB_MAX_CBS
#define SB_WORDS    (SB_MAX_CBS + 1u)

/* The value command buffer i writes into its own word. Distinct per
 * index and not derivable from zero or from the poison, so a word that
 * matches can only have been written by that command buffer. */
static uint32_t sb_word_for(uint32_t i)
{
   return (i * UINT32_C(2654435761)) ^ UINT32_C(0x5eed0000);
}

/* Records one command buffer of the chain: a barrier that makes this
 * fill depend on every fill before it, then two fills.
 *
 * THE BARRIER IS THE POINT. Without it the two writes to SB_SHARED from
 * different command buffers are unsynchronised and the last value is
 * undefined — with or without batching. With it, Vulkan requires the
 * later fill to see the earlier one, and that requirement is what the
 * batch has to keep.
 */
static bool sb_record(vkfw *fw, test_ctx *t, VkCommandBuffer cb,
                      const vkfw_buffer *dst, uint32_t i)
{
   const VkMemoryBarrier barrier = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT |
                       VK_ACCESS_TRANSFER_READ_BIT,
   };
   fw->vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                               1, &barrier, 0, NULL, 0, NULL);

   fw->vk.vkCmdFillBuffer(cb, dst->buf, (VkDeviceSize)i * 4u, 4u,
                          sb_word_for(i));
   /* Every command buffer overwrites the same word with its own index,
    * so the value left behind names the one that ran last. */
   fw->vk.vkCmdFillBuffer(cb, dst->buf, (VkDeviceSize)SB_SHARED * 4u, 4u, i);

   (void)t;
   return true;
}

/* Builds `n` command buffers, submits them as ONE VkSubmitInfo, waits
 * on a fence and checks every word. `elapsed_ns_out` may be NULL.
 */
static bool sb_run_chain(vkfw *fw, test_ctx *t, uint32_t n,
                         const char *what, uint64_t *elapsed_ns_out)
{
   vkfw_buffer dst;
   if (!vkfw_buffer_create(fw, SB_WORDS * 4u,
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &dst))
      return false;

   bool ok = false;
   VkFence fence = VK_NULL_HANDLE;
   VkCommandBuffer *cbs = calloc(n, sizeof(*cbs));
   if (cbs == NULL) {
      t_check(t, false, "%s: out of host memory for %" PRIu32
              " command buffer handles", what, n);
      goto out_buffer;
   }

   /* Poison first, and flushed to memory by the helper: a word that
    * still holds the poison is a fill that never ran, and a word that
    * holds its value cannot have got it any other way. */
   if (!vkfw_buffer_poison(fw, &dst, UINT32_C(0xdeadbeef)))
      goto out_cbs;

   for (uint32_t i = 0; i < n; i++) {
      if (!vkfw_cmd_begin(fw, &cbs[i]))
         goto out_cbs;
      if (!sb_record(fw, t, cbs[i], &dst, i))
         goto out_cbs;
      const VkResult er = fw->vk.vkEndCommandBuffer(cbs[i]);
      if (!t_check(t, er == VK_SUCCESS, "%s: vkEndCommandBuffer(%" PRIu32
                   ") -> %s", what, i, vkfw_result_str(er)))
         goto out_cbs;
   }

   const VkFenceCreateInfo fci = { .sType =
      VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
   VkResult r = fw->vk.vkCreateFence(fw->dev, &fci, NULL, &fence);
   if (!t_check(t, r == VK_SUCCESS, "%s: vkCreateFence -> %s", what,
                vkfw_result_str(r)))
      goto out_cbs;

   /* ONE VkSubmitInfo WITH n COMMAND BUFFERS. That is the whole shape
    * this case exists for: nvk_queue_submit_locked calls exec() once
    * per command buffer and signal() once at the end, so this is n
    * exec() calls against one signal. */
   const VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = n,
      .pCommandBuffers = cbs,
   };

   const uint64_t start_tick = armGetSystemTick();
   r = fw->vk.vkQueueSubmit(fw->queue, 1, &si, fence);
   fw->last_submit_result = r;
   if (!t_check(t, r == VK_SUCCESS, "%s: vkQueueSubmit(%" PRIu32
                " command buffer(s)) -> %s", what, n, vkfw_result_str(r)))
      goto out_fence;

   r = fw->vk.vkWaitForFences(fw->dev, 1, &fence, VK_TRUE, SB_WAIT_NS);
   fw->last_submit_result = r;
   const uint64_t elapsed_tick = armGetSystemTick() - start_tick;
   if (!t_check(t, r == VK_SUCCESS, "%s: the submission completed -> %s",
                what, vkfw_result_str(r)))
      goto out_fence;

   const uint64_t freq = armGetSystemTickFreq();
   if (elapsed_ns_out != NULL)
      *elapsed_ns_out = freq ? (elapsed_tick * UINT64_C(1000000000)) / freq
                             : 0;

   if (!vkfw_buffer_invalidate(fw, &dst))
      goto out_fence;

   const uint32_t *got = dst.map;
   uint32_t bad = 0;
   for (uint32_t i = 0; i < n; i++) {
      if (got[i] != sb_word_for(i))
         bad++;
   }
   t_check(t, bad == 0, "%s: every one of the %" PRIu32 " command buffers "
           "wrote its own word (%" PRIu32 " wrong)", what, n, bad);

   /* The ordering check. The barrier in each command buffer makes the
    * last fill of SB_SHARED the one from command buffer n-1; anything
    * else means the chain was reordered or a command buffer was
    * dropped. */
   t_check(t, got[SB_SHARED] == n - 1u,
           "%s: the last command buffer's write to the shared word won "
           "(%" PRIu32 ", wanted %" PRIu32 ")", what, got[SB_SHARED],
           n - 1u);

   ok = (bad == 0) && (got[SB_SHARED] == n - 1u);

out_fence:
   if (fence != VK_NULL_HANDLE)
      fw->vk.vkDestroyFence(fw->dev, fence, NULL);
out_cbs:
   /* The pool is reset by vkfw at teardown; the handles themselves are
    * owned by it, so only the array is freed here. */
   free(cbs);
out_buffer:
   vkfw_buffer_destroy(fw, &dst);
   return ok;
}

/* Sections A through D, on whichever device the caller has brought up.
 * Returns the wall time of the eight-command-buffer chain so the two
 * halves of section E can be compared.
 */
static void sb_sections(vkfw *fw, test_ctx *t, uint64_t *eight_ns_out)
{
   /* --- A: one, two, four and eight command buffers in one submit --- */
   static const uint32_t counts[] = { 1u, 2u, 4u, 8u };
   for (uint32_t k = 0; k < (sizeof(counts) / sizeof(counts[0])); k++) {
      char what[48];
      snprintf(what, sizeof(what), "A/%" PRIu32 " cb", counts[k]);
      uint64_t ns = 0;
      (void)sb_run_chain(fw, t, counts[k], what, &ns);
      if (counts[k] == 8u && eight_ns_out != NULL)
         *eight_ns_out = ns;
      if (vkfw_device_lost(fw))
         return;
   }

   /* --- B: past the batch's own cap ---------------------------------
    *
    * NVKMD_HORIZON_BATCH_SPANS is 64 pushes, and each of these command
    * buffers is at least one push, so eighty of them cross it. What
    * that exercises is the kickoff the backstop takes in the MIDDLE of
    * a submission: the first sixty-four or so pushes go out, the rest
    * follow in a second one, and the ordering the barriers ask for has
    * to hold across that seam as well as within a batch.
    */
   (void)sb_run_chain(fw, t, SB_MAX_CBS, "B/80 cb", NULL);
   if (vkfw_device_lost(fw))
      return;

   /* --- C: a submission with waits and signals and no work ----------
    *
    * "Tratar correctamente submissions sin command buffers que
    * contienen esperas y señales": nvk_queue_submit_locked still calls
    * wait() and signal() for one of these, and signal() is what sends
    * the batch. With no command buffers there is nothing in the batch,
    * so signal() falls through to its fence-only submit — and the chain
    * below is what says the signal actually happened rather than being
    * dropped along with the absent work.
    *
    * Three submissions: one that signals A with work, one that waits on
    * A and signals B with NO work, and one that waits on B and writes
    * the word this checks. If the middle one lost the signal, the third
    * never runs and the fence times out.
    */
   {
      VkSemaphore sem[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
      const VkSemaphoreCreateInfo sci = { .sType =
         VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
      bool made = true;
      for (uint32_t i = 0; i < 2; i++) {
         const VkResult r = fw->vk.vkCreateSemaphore(fw->dev, &sci, NULL,
                                                     &sem[i]);
         if (!t_check(t, r == VK_SUCCESS, "C: vkCreateSemaphore(%" PRIu32
                      ") -> %s", i, vkfw_result_str(r)))
            made = false;
      }

      vkfw_buffer dst;
      if (made && vkfw_buffer_create(fw, 4u,
                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                     &dst)) {
         if (vkfw_buffer_poison(fw, &dst, UINT32_C(0xdeadbeef))) {
            VkCommandBuffer first = VK_NULL_HANDLE, last = VK_NULL_HANDLE;
            VkFence fence = VK_NULL_HANDLE;
            const VkFenceCreateInfo fci = { .sType =
               VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            bool built = vkfw_cmd_begin(fw, &first);
            if (built) {
               fw->vk.vkCmdFillBuffer(first, dst.buf, 0, 4u,
                                      UINT32_C(0x11111111));
               built = fw->vk.vkEndCommandBuffer(first) == VK_SUCCESS;
            }
            if (built)
               built = vkfw_cmd_begin(fw, &last);
            if (built) {
               fw->vk.vkCmdFillBuffer(last, dst.buf, 0, 4u,
                                      UINT32_C(0xc0ffee01));
               built = fw->vk.vkEndCommandBuffer(last) == VK_SUCCESS;
            }
            if (built)
               built = fw->vk.vkCreateFence(fw->dev, &fci, NULL, &fence)
                       == VK_SUCCESS;

            if (t_check(t, built, "C: the three submissions were built")) {
               const VkPipelineStageFlags stage =
                  VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
               const VkSubmitInfo si[3] = {
                  { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                    .commandBufferCount = 1, .pCommandBuffers = &first,
                    .signalSemaphoreCount = 1, .pSignalSemaphores = &sem[0] },
                  /* No command buffers at all, and both a wait and a
                   * signal on it. */
                  { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                    .waitSemaphoreCount = 1, .pWaitSemaphores = &sem[0],
                    .pWaitDstStageMask = &stage,
                    .signalSemaphoreCount = 1, .pSignalSemaphores = &sem[1] },
                  { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                    .waitSemaphoreCount = 1, .pWaitSemaphores = &sem[1],
                    .pWaitDstStageMask = &stage,
                    .commandBufferCount = 1, .pCommandBuffers = &last },
               };
               VkResult r = fw->vk.vkQueueSubmit(fw->queue, 3, si, fence);
               fw->last_submit_result = r;
               if (t_check(t, r == VK_SUCCESS,
                           "C: three submissions, the middle one with a "
                           "wait, a signal and no work -> %s",
                           vkfw_result_str(r))) {
                  r = fw->vk.vkWaitForFences(fw->dev, 1, &fence, VK_TRUE,
                                             SB_WAIT_NS);
                  fw->last_submit_result = r;
                  if (t_check(t, r == VK_SUCCESS,
                              "C: the chain completed -> %s (a timeout here "
                              "is the empty submission's signal never "
                              "arriving)", vkfw_result_str(r)) &&
                      vkfw_buffer_invalidate(fw, &dst)) {
                     const uint32_t got = *(const uint32_t *)dst.map;
                     t_check(t, got == UINT32_C(0xc0ffee01),
                             "C: the submission behind the empty one ran "
                             "(0x%08" PRIx32 ")", got);
                  }
               }
            }
            if (fence != VK_NULL_HANDLE)
               fw->vk.vkDestroyFence(fw->dev, fence, NULL);
         }
         vkfw_buffer_destroy(fw, &dst);
      }

      for (uint32_t i = 0; i < 2; i++) {
         if (sem[i] != VK_NULL_HANDLE)
            fw->vk.vkDestroySemaphore(fw->dev, sem[i], NULL);
      }
   }
   if (vkfw_device_lost(fw))
      return;

   /* --- D: a batched submission behind a cross-channel dependency ---
    *
    * Creating a compute pipeline makes NAK compile and NVK upload the
    * result into its shader heap, and that upload goes through
    * dev->upload — a DIFFERENT nvkmd context on a DIFFERENT channel.
    * The next vkQueueSubmit therefore opens with
    * nvkmd_horizon_ctx_wait on the upload queue's timeline before any
    * of its command buffers is accumulated, which is the one shape
    * where a wait and a batch meet.
    *
    * The pipeline is created and destroyed and never dispatched:
    * vk_shaders/compute_dispatch owns the question of whether the
    * shader computes the right thing, and repeating it here would make
    * a failure ambiguous between the two.
    */
   {
      VkShaderModule module = VK_NULL_HANDLE;
      VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
      VkPipelineLayout layout = VK_NULL_HANDLE;
      VkPipeline pipeline = VK_NULL_HANDLE;

      const VkShaderModuleCreateInfo smci = {
         .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
         .codeSize = sizeof(comp_write_id_spv),
         .pCode = comp_write_id_spv,
      };
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

      bool built =
         fw->vk.vkCreateShaderModule(fw->dev, &smci, NULL, &module)
            == VK_SUCCESS &&
         fw->vk.vkCreateDescriptorSetLayout(fw->dev, &dslci, NULL,
                                            &set_layout) == VK_SUCCESS;
      if (built) {
         const VkPipelineLayoutCreateInfo plci = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &set_layout,
         };
         built = fw->vk.vkCreatePipelineLayout(fw->dev, &plci, NULL, &layout)
                 == VK_SUCCESS;
      }
      if (built) {
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
         built = fw->vk.vkCreateComputePipelines(fw->dev, VK_NULL_HANDLE, 1,
                                                 &cpci, NULL, &pipeline)
                 == VK_SUCCESS;
      }

      if (t_check(t, built, "D: a compute pipeline was created, so the "
                            "upload queue has work on another channel")) {
         /* Straight into a batched submission. Whatever the upload
          * queue left pending is waited for by exec_ctx before the
          * first of these four command buffers is accumulated. */
         (void)sb_run_chain(fw, t, 4u,
                            "D/4 cb behind the upload queue", NULL);
      }

      if (pipeline != VK_NULL_HANDLE)
         fw->vk.vkDestroyPipeline(fw->dev, pipeline, NULL);
      if (layout != VK_NULL_HANDLE)
         fw->vk.vkDestroyPipelineLayout(fw->dev, layout, NULL);
      if (set_layout != VK_NULL_HANDLE)
         fw->vk.vkDestroyDescriptorSetLayout(fw->dev, set_layout, NULL);
      if (module != VK_NULL_HANDLE)
         fw->vk.vkDestroyShaderModule(fw->dev, module, NULL);
   }
}

TEST_CASE_DECL(vk_core, submit_batching)
{
   /* The backend's own counters, so the log carries exec calls, pushes
    * and kickoffs beside these checks. Set before vkCreateDevice: the
    * meter is created with the nvkmd context and reads this once. */
   setenv("MESA_VK_NVKMD_HORIZON_SUBMIT_STATS", "1", 1);

   uint64_t batched_ns = 0;
   {
      vkfw fw;
      if (!vkfw_init(&fw, t, NULL))
         return 1;
      t_note(t, "accumulation is on (the default); "
                "NVK_HORIZON_BATCH_EXEC=0 is the other half, below");
      sb_sections(&fw, t, &batched_ns);
      vkfw_finish(&fw);
   }

   /* --- E: the same work with accumulation off ----------------------
    *
    * A SECOND DEVICE, because the option is read once per nvkmd
    * context, at vkCreateDevice. Running both halves in one process on
    * one console in one launch is the only way the two numbers are
    * about the same clock, the same thermal state and the same cache.
    *
    * WHAT THE TWO TIMES ARE AND ARE NOT. Each is one submission of
    * eight command buffers, measured from vkQueueSubmit to the fence.
    * That is the interval batching acts on, so it is the honest thing
    * to compare — and it is a single sample of one workload on one
    * console, not a frame rate. A frame-rate claim needs a frame, and
    * vk_present is where those live.
    */
   setenv("NVK_HORIZON_BATCH_EXEC", "0", 1);
   {
      vkfw fw;
      if (!vkfw_init(&fw, t, NULL)) {
         unsetenv("NVK_HORIZON_BATCH_EXEC");
         return 1;
      }
      t_note(t, "accumulation is off (NVK_HORIZON_BATCH_EXEC=0): one "
                "kickoff per command buffer, which is what this backend "
                "did before");
      uint64_t unbatched_ns = 0;
      (void)sb_run_chain(&fw, t, 8u, "E/8 cb unbatched", &unbatched_ns);
      vkfw_finish(&fw);

      t_note(t, "E: eight command buffers in one submission took %" PRIu64
                " us batched and %" PRIu64 " us unbatched (one sample each; "
                "the meter lines above say how many kickoffs each was)",
             batched_ns / 1000u, unbatched_ns / 1000u);
   }
   unsetenv("NVK_HORIZON_BATCH_EXEC");

   return 0;
}
