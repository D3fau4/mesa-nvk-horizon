/*
 * t_vk_suboptimal — VK_SUBOPTIMAL_KHR on the Horizon WSI, and the line
 * between it and VK_ERROR_OUT_OF_DATE_KHR.
 *
 * THE RULE THIS FILE EXISTS TO CHECK. Vulkan separates the two results
 * by one question — can the swapchain still present?
 *
 *   VK_SUBOPTIMAL_KHR         the swapchain no longer matches the
 *                             surface's properties exactly and can
 *                             still be used to present successfully;
 *   VK_ERROR_OUT_OF_DATE_KHR  the surface has changed so that the
 *                             swapchain is no longer compatible, and
 *                             further presentation requests will fail.
 *
 * On this backend exactly one condition falls on the first side: the VI
 * consumer reporting a layer size other than the one the swapchain
 * registered its buffers at, which on real hardware means the console
 * being docked or undocked. Everything else this backend refuses —
 * another swapchain taking the window, a queue that will not take a
 * buffer, an application blocking for an image only it can free — falls
 * on the second, because in each of those the next present really
 * cannot happen.
 *
 * THE ORACLE IS THE SURFACE, NOT A FLAG IN THE DRIVER. Every check here
 * is written against VkSurfaceCapabilitiesKHR::currentExtent, which is
 * the surface property the specification's own definition of
 * VK_SUBOPTIMAL_KHR refers to, and which this backend answers from
 * NWindow::default_*, the size the compositor itself reports through
 * the BufferQueue. So the invariant asserted per frame is
 *
 *   SUBOPTIMAL  <=>  currentExtent != the swapchain's imageExtent
 *
 * in BOTH directions, on both present paths. That is what makes
 * "section A saw no SUBOPTIMAL" mean something: it did not see one
 * *and* the surface it was measured against matched, so the absence is
 * a result rather than a check that cannot fire. A driver that returned
 * SUBOPTIMAL on a whim fails the forward direction; one that never
 * returns it fails the reverse the moment the console is docked.
 *
 * WHAT IS NOT INVENTED HERE. Nothing in this file fakes a resize.
 * There is no debug hook, no injected extent and no environment
 * variable that makes the driver pretend the window changed — the only
 * thing that produces the condition is a real VI layer resize, and the
 * only way to produce one from a process is to have the console docked
 * or undocked while the test runs. Section D therefore asks the
 * operator, waits a bounded time, and says in the log when the change
 * did not happen rather than reporting coverage it did not get. The
 * producer side of a BufferQueue cannot resize itself: the consumer's
 * default buffer size is the consumer's, and VI is the consumer.
 *
 * The sections:
 *
 *   A. Normal operation, zero-copy. 120 frames in which every acquire
 *      and every present must return exactly VK_SUCCESS, with the rule
 *      above checked on each one. This is the "no false SUBOPTIMAL"
 *      measurement and it is the one that runs on every console.
 *
 *   B. The same on the copy fallback, forced. The two paths learn the
 *      consumer's size through different calls — BqBufferOutput from
 *      the driver's own bqQueueBuffer against NWindow::default_* that
 *      libnx fills from framebufferEnd — so a defect in one is
 *      invisible in the other.
 *
 *   C. The results stay distinct. A superseded swapchain reports
 *      VK_ERROR_OUT_OF_DATE_KHR from both acquire and present while
 *      the surface's extent is unchanged, so that result cannot be
 *      coming from the resize check; and an acquire that cannot be
 *      satisfied reports VK_TIMEOUT rather than dressing a refusal up
 *      as a success code. Both present paths.
 *
 *   D. A real display mode change, with the operator. Bounded, and
 *      skippable. When it happens: the first SUBOPTIMAL is recorded,
 *      presentation must keep succeeding for many frames afterwards,
 *      the surface must now refuse a swapchain at the old extent, and
 *      recreating at the new one must go back to plain VK_SUCCESS.
 *
 *   E. Recreation, repeatedly, and nothing retained. Eight generations
 *      alternating present path, each presenting its frames in full —
 *      a slot retained or a window not given back shows up as the next
 *      generation failing to acquire — and then the log is read back
 *      for anything the driver said it could not tear down.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include "common/vkfw.h"

const char *const test_name = "t_vk_suboptimal";

/* This test owns the display: no console, and the SD-card log is the
 * whole record (testfw.h). */
const bool test_uses_display = true;

/* Long enough that a spurious SUBOPTIMAL has plenty of chances to
 * appear, short enough that two of them plus everything else stays
 * inside a minute of the operator's time. Two seconds a piece at 60 Hz.
 */
#define SUB_FRAMES        120u
/* Where what is being checked is behaviour and not how often. */
#define SUB_SHORT_FRAMES   20u

#define SUB_WAIT_NS       UINT64_C(2000000000)
#define SUB_MAX_IMAGES     4u

/* How long section D waits for somebody to dock or undock the console.
 * A bound, not a deadline for the operator: the section ends early the
 * moment the change is seen, and B skips it. */
#define SUB_MODE_WAIT_NS  UINT64_C(30000000000)

/* Generations in section E. Eight is enough that a slot leaked once per
 * generation would exhaust a four-image window twice over. */
#define SUB_GENERATIONS     8u

/* ------------------------------------------------------------------ */
/* One swapchain and the per-image resources a frame needs.            */
/* ------------------------------------------------------------------ */

typedef struct sub_frame_res {
   VkCommandBuffer cb;
   VkSemaphore render_done;
   VkFence in_flight;
   bool in_flight_valid;
} sub_frame_res;

typedef struct sub_swapchain {
   VkSwapchainKHR handle;
   uint32_t image_count;
   VkImage images[SUB_MAX_IMAGES];
   sub_frame_res res[SUB_MAX_IMAGES];
   VkFence acquire_fence;
   /* What the swapchain was created with. Every rule in this file is a
    * statement about this against the surface's currentExtent. */
   VkExtent2D extent;
} sub_swapchain;

/* What one measured run of frames observed.
 *
 * `rule_broken` is the finding this file is for: a frame on which the
 * driver's result and the surface's own extent disagreed, in either
 * direction. It is counted rather than aborted on, so the log says how
 * many of how many rather than only where the first one was. */
typedef struct sub_run {
   const char *what;

   uint32_t frames;              /* presented                          */
   uint32_t acquire_suboptimal;
   uint32_t present_suboptimal;
   uint32_t rule_broken;
   uint32_t extent_changes;      /* times currentExtent moved under us  */

   VkExtent2D first_extent;      /* what the surface said at entry      */
   VkExtent2D last_extent;       /* and at the last frame               */
   VkExtent2D swapchain_extent;  /* what the swapchain was built for    */
   /* The frame on which a present first reported SUBOPTIMAL, and
    * whether one ever did. */
   uint32_t first_suboptimal_frame;
   bool saw_suboptimal;

   bool failed;
   uint32_t fail_frame;
   VkResult fail_result;
   const char *fail_what;
} sub_run;

static void sub_run_init(sub_run *r, const char *what)
{
   memset(r, 0, sizeof(*r));
   r->what = what;
}

static void sub_fail(sub_run *r, uint32_t frame, VkResult res,
                     const char *what)
{
   if (r->failed)
      return;
   r->failed = true;
   r->fail_frame = frame;
   r->fail_result = res;
   r->fail_what = what;
}

static bool sub_extent_eq(VkExtent2D a, VkExtent2D b)
{
   return a.width == b.width && a.height == b.height;
}

/* The surface's own answer, which is the oracle every rule here is
 * written against. Returns false only when the query itself failed,
 * which is a different finding and is reported as one. */
static bool sub_surface_extent(vkfw *fw, VkSurfaceKHR surface,
                               VkExtent2D *out, VkResult *r_out)
{
   VkSurfaceCapabilitiesKHR caps;
   memset(&caps, 0, sizeof(caps));
   const VkResult r =
      fw->wsi.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(fw->pdev, surface,
                                                        &caps);
   if (r_out != NULL)
      *r_out = r;
   if (r != VK_SUCCESS)
      return false;
   *out = caps.currentExtent;
   return true;
}

/* ------------------------------------------------------------------ */

static void sub_destroy(vkfw *fw, sub_swapchain *sc)
{
   for (uint32_t i = 0; i < sc->image_count; i++) {
      if (sc->res[i].render_done != VK_NULL_HANDLE)
         fw->wsi.vkDestroySemaphore(fw->dev, sc->res[i].render_done, NULL);
      if (sc->res[i].in_flight != VK_NULL_HANDLE)
         fw->vk.vkDestroyFence(fw->dev, sc->res[i].in_flight, NULL);
      if (sc->res[i].cb != VK_NULL_HANDLE)
         fw->vk.vkFreeCommandBuffers(fw->dev, fw->pool, 1, &sc->res[i].cb);
   }
   if (sc->acquire_fence != VK_NULL_HANDLE)
      fw->vk.vkDestroyFence(fw->dev, sc->acquire_fence, NULL);
   if (sc->handle != VK_NULL_HANDLE)
      fw->wsi.vkDestroySwapchainKHR(fw->dev, sc->handle, NULL);

   memset(sc, 0, sizeof(*sc));
}

/* Everything the device could still be using is finished with before
 * sub_destroy runs.
 *
 * vkQueueWaitIdle and not only the in-flight fences: the fence says the
 * render submission retired, and says nothing about the
 * vkQueuePresentKHR still waiting on render_done[i], which
 * vkDestroySemaphore requires to have completed. This is the shape the
 * PR 9 review put into t_vk_wsi_mt for the same reason. Returns the
 * result so the caller can make a check out of it rather than destroy
 * objects the device may still hold. */
static VkResult sub_quiesce(vkfw *fw)
{
   const VkResult r = fw->vk.vkQueueWaitIdle(fw->queue);
   if (r != VK_SUCCESS)
      return r;
   return fw->vk.vkDeviceWaitIdle(fw->dev);
}

static bool sub_create(vkfw *fw, VkSurfaceKHR surface, uint32_t want_images,
                       VkExtent2D extent, VkSwapchainKHR old,
                       const char *what, sub_swapchain *sc)
{
   test_ctx *t = fw->t;
   memset(sc, 0, sizeof(*sc));
   sc->extent = extent;

   const VkSwapchainCreateInfoKHR ci = {
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .surface = surface,
      .minImageCount = want_images,
      .imageFormat = VK_FORMAT_R8G8B8A8_UNORM,
      .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
      .imageExtent = extent,
      .imageArrayLayers = 1,
      .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      .presentMode = VK_PRESENT_MODE_FIFO_KHR,
      .clipped = VK_TRUE,
      .oldSwapchain = old,
   };

   VkResult r = fw->wsi.vkCreateSwapchainKHR(fw->dev, &ci, NULL, &sc->handle);
   if (!t_check(t, r == VK_SUCCESS, "%s: vkCreateSwapchainKHR(%" PRIu32
                " images, %" PRIu32 "x%" PRIu32 ") -> %s", what, want_images,
                extent.width, extent.height, vkfw_result_str(r)))
      return false;

   uint32_t count = 0;
   r = fw->wsi.vkGetSwapchainImagesKHR(fw->dev, sc->handle, &count, NULL);
   if (!t_check(t, r == VK_SUCCESS && count >= 2 && count <= SUB_MAX_IMAGES,
                "%s: the swapchain has %" PRIu32 " image(s) -> %s", what,
                count, vkfw_result_str(r)))
      goto fail;

   r = fw->wsi.vkGetSwapchainImagesKHR(fw->dev, sc->handle, &count,
                                       sc->images);
   if (!t_check(t, r == VK_SUCCESS, "%s: vkGetSwapchainImagesKHR(list) -> %s",
                what, vkfw_result_str(r)))
      goto fail;
   sc->image_count = count;

   const VkFenceCreateInfo fci = {
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
   };
   r = fw->vk.vkCreateFence(fw->dev, &fci, NULL, &sc->acquire_fence);
   if (!t_check(t, r == VK_SUCCESS, "%s: the acquire fence -> %s", what,
                vkfw_result_str(r)))
      goto fail;

   for (uint32_t i = 0; i < sc->image_count; i++) {
      const VkCommandBufferAllocateInfo cbai = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = fw->pool,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 1,
      };
      r = fw->vk.vkAllocateCommandBuffers(fw->dev, &cbai, &sc->res[i].cb);
      if (!t_check(t, r == VK_SUCCESS, "%s: command buffer %" PRIu32 " -> %s",
                   what, i, vkfw_result_str(r)))
         goto fail;

      const VkSemaphoreCreateInfo sci = {
         .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
      };
      r = fw->wsi.vkCreateSemaphore(fw->dev, &sci, NULL,
                                    &sc->res[i].render_done);
      if (!t_check(t, r == VK_SUCCESS, "%s: semaphore %" PRIu32 " -> %s",
                   what, i, vkfw_result_str(r)))
         goto fail;

      r = fw->vk.vkCreateFence(fw->dev, &fci, NULL, &sc->res[i].in_flight);
      if (!t_check(t, r == VK_SUCCESS, "%s: fence %" PRIu32 " -> %s", what, i,
                   vkfw_result_str(r)))
         goto fail;
   }

   return true;

fail:
   sub_destroy(fw, sc);
   return false;
}

/* A visible colour that changes every frame, so the screen is doing
 * something an operator can see rather than sitting still. */
static void sub_frame_colour(uint32_t frame, VkClearColorValue *out)
{
   out->float32[0] = (float)((frame * 3u) & 0xffu) / 255.0f;
   out->float32[1] = (float)((frame * 5u + 85u) & 0xffu) / 255.0f;
   out->float32[2] = (float)((frame * 7u + 170u) & 0xffu) / 255.0f;
   out->float32[3] = 1.0f;
}

/* Clears the acquired image and leaves it in PRESENT_SRC. No pipeline
 * and no shader: the subject here is the present, and anything else
 * would be another thing that could fail. */
static VkResult sub_record_and_submit(vkfw *fw, sub_swapchain *sc,
                                      uint32_t index, uint32_t frame)
{
   sub_frame_res *res = &sc->res[index];
   VkResult r;

   if (res->in_flight_valid) {
      r = fw->vk.vkWaitForFences(fw->dev, 1, &res->in_flight, VK_TRUE,
                                 SUB_WAIT_NS);
      if (r != VK_SUCCESS)
         return r;
   }
   r = fw->vk.vkResetFences(fw->dev, 1, &res->in_flight);
   if (r != VK_SUCCESS)
      return r;

   const VkCommandBufferBeginInfo bi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   r = fw->vk.vkBeginCommandBuffer(res->cb, &bi);
   if (r != VK_SUCCESS)
      return r;

   const VkImageSubresourceRange range = {
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .levelCount = 1,
      .layerCount = 1,
   };

   VkImageMemoryBarrier to_dst = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = 0,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = sc->images[index],
      .subresourceRange = range,
   };
   fw->vk.vkCmdPipelineBarrier(res->cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                               0, NULL, 0, NULL, 1, &to_dst);

   VkClearColorValue colour;
   sub_frame_colour(frame, &colour);
   fw->vk.vkCmdClearColorImage(res->cb, sc->images[index],
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               &colour, 1, &range);

   VkImageMemoryBarrier to_present = to_dst;
   to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
   to_present.dstAccessMask = 0;
   to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
   fw->vk.vkCmdPipelineBarrier(res->cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                               0, NULL, 0, NULL, 1, &to_present);

   r = fw->vk.vkEndCommandBuffer(res->cb);
   if (r != VK_SUCCESS)
      return r;

   const VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &res->cb,
      .signalSemaphoreCount = 1,
      .pSignalSemaphores = &res->render_done,
   };
   r = fw->vk.vkQueueSubmit(fw->queue, 1, &si, res->in_flight);
   if (r == VK_SUCCESS)
      res->in_flight_valid = true;
   return r;
}

/* ------------------------------------------------------------------ */
/* One frame, and the rule checked on both of its results.             */
/* ------------------------------------------------------------------ */

/* `before` is the surface extent as the previous frame left it, which
 * is the state the acquire below is answering about: the consumer's
 * size reaches this backend at a present and nowhere else, so nothing
 * between the previous present and this acquire can have changed it.
 * On return it holds what the surface says after this frame's present,
 * ready to be the next frame's `before`.
 *
 * Returns false when the frame did not complete, with the run's failure
 * recorded. A SUBOPTIMAL on either call is not a failure — it is the
 * subject. */
static bool sub_frame(vkfw *fw, sub_swapchain *sc, VkSurfaceKHR surface,
                      uint32_t frame, uint64_t acquire_timeout_ns,
                      VkExtent2D *before, sub_run *run)
{
   run->swapchain_extent = sc->extent;

   uint32_t index = 0;
   VkResult r = fw->wsi.vkAcquireNextImageKHR(fw->dev, sc->handle,
                                              acquire_timeout_ns,
                                              VK_NULL_HANDLE,
                                              sc->acquire_fence, &index);
   if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
      sub_fail(run, frame, r, "vkAcquireNextImageKHR");
      return false;
   }

   /* THE RULE, ON THE ACQUIRE. `*before` is the surface as of the last
    * present, and the acquire either agreed with it or did not. Both
    * directions: a SUBOPTIMAL with a matching surface is a false
    * positive, and a plain VK_SUCCESS with a surface that has moved is
    * a missed one. */
   const bool acq_sub = (r == VK_SUBOPTIMAL_KHR);
   if (acq_sub)
      run->acquire_suboptimal++;
   if (acq_sub != !sub_extent_eq(*before, sc->extent))
      run->rule_broken++;

   /* THE FENCE IS DEALT WITH BEFORE ANY OTHER FAILURE CAN LEAVE, and
    * that ordering is not incidental. The acquire took a reference to
    * it on both success codes, so any path that returns without waiting
    * on it and resetting it leaves a signal pending on a fence
    * sub_destroy then destroys — which is invalid usage, and is the
    * defect the PR 9 review found in t_vk_wsi_mt. The index check
    * below is one such path, so it comes after this. */
   r = fw->vk.vkWaitForFences(fw->dev, 1, &sc->acquire_fence, VK_TRUE,
                              SUB_WAIT_NS);
   if (r != VK_SUCCESS) {
      sub_fail(run, frame, r, "waiting for the acquire fence");
      return false;
   }
   r = fw->vk.vkResetFences(fw->dev, 1, &sc->acquire_fence);
   if (r != VK_SUCCESS) {
      sub_fail(run, frame, r, "resetting the acquire fence");
      return false;
   }

   /* An image came back on both success codes, so the index must be
    * usable on both. VK_SUBOPTIMAL_KHR is a success code and the
    * specification says *pImageIndex is valid. */
   if (index >= sc->image_count) {
      sub_fail(run, frame, VK_ERROR_UNKNOWN, "an image index out of range");
      return false;
   }

   r = sub_record_and_submit(fw, sc, index, frame);
   if (r != VK_SUCCESS) {
      sub_fail(run, frame, r, "recording and submitting the frame");
      return false;
   }

   const VkPresentInfoKHR pi = {
      .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &sc->res[index].render_done,
      .swapchainCount = 1,
      .pSwapchains = &sc->handle,
      .pImageIndices = &index,
   };
   r = fw->wsi.vkQueuePresentKHR(fw->queue, &pi);
   if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
      sub_fail(run, frame, r, "vkQueuePresentKHR");
      return false;
   }
   run->frames++;

   /* THE RULE, ON THE PRESENT, and this one is exact in time: the call
    * that just returned is the call that read the consumer's size, so
    * the surface below and the result above describe the same instant.
    */
   VkExtent2D after;
   VkResult qr = VK_SUCCESS;
   if (!sub_surface_extent(fw, surface, &after, &qr)) {
      sub_fail(run, frame, qr, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
      return false;
   }

   const bool pres_sub = (r == VK_SUBOPTIMAL_KHR);
   if (pres_sub) {
      run->present_suboptimal++;
      if (!run->saw_suboptimal) {
         run->saw_suboptimal = true;
         run->first_suboptimal_frame = frame;
      }
   }
   if (pres_sub != !sub_extent_eq(after, sc->extent))
      run->rule_broken++;

   if (!sub_extent_eq(after, *before))
      run->extent_changes++;
   *before = after;
   run->last_extent = after;
   return true;
}

/* `frames` frames, or fewer if one fails. The surface is read once
 * before the loop so the first acquire has a `before` to be judged
 * against. */
static void sub_present_frames(vkfw *fw, sub_swapchain *sc,
                               VkSurfaceKHR surface, uint32_t frames,
                               sub_run *run)
{
   VkExtent2D before;
   VkResult qr = VK_SUCCESS;
   if (!sub_surface_extent(fw, surface, &before, &qr)) {
      sub_fail(run, 0, qr, "the surface extent before the run");
      return;
   }
   run->first_extent = before;
   run->last_extent = before;
   run->swapchain_extent = sc->extent;

   for (uint32_t frame = 0; frame < frames; frame++) {
      if (!sub_frame(fw, sc, surface, frame, SUB_WAIT_NS, &before, run))
         return;
   }
}

/* One check for the run's frames, one for the rule, one for the run
 * having been an ordinary one, and a note with the numbers behind all
 * three.
 *
 * EVERY CALLER IS AN ORDINARY RUN, which is why the third check is not
 * a parameter. The run that is *meant* to be suboptimal is section D's,
 * and it is reported by section D's own checks — it has no frame count
 * to compare against, because how long it takes somebody to reach the
 * dock is not a number this file can assert on. A parameter with one
 * value at every call site is the constant written twice. */
static bool sub_report(test_ctx *t, const sub_run *run, uint32_t expected)
{
   bool ok = t_check(t, !run->failed && run->frames == expected,
                     "%s: %" PRIu32 " of %" PRIu32 " frames presented%s",
                     run->what, run->frames, expected,
                     run->failed ? " (see the note below)" : "");
   if (run->failed) {
      t_note(t, "%s: %s at frame %" PRIu32 " -> %s", run->what,
             run->fail_what, run->fail_frame,
             vkfw_result_str(run->fail_result));
   }

   t_note(t, "%s: surface %" PRIu32 "x%" PRIu32 " -> %" PRIu32 "x%" PRIu32
             " (%" PRIu32 " change(s)); swapchain %" PRIu32 "x%" PRIu32
             "; SUBOPTIMAL: %" PRIu32 " acquire, %" PRIu32 " present",
          run->what, run->first_extent.width, run->first_extent.height,
          run->last_extent.width, run->last_extent.height,
          run->extent_changes, run->swapchain_extent.width,
          run->swapchain_extent.height, run->acquire_suboptimal,
          run->present_suboptimal);

   /* THE CHECK THIS FILE IS FOR. Not "no SUBOPTIMAL appeared" — that
    * would also pass on a driver whose resize check is dead — but
    * "every result matched what the surface said at the time". */
   ok = t_check(t, run->rule_broken == 0,
                "%s: every acquire and present agreed with the surface: "
                "SUBOPTIMAL exactly when currentExtent differs from the "
                "swapchain's extent (%" PRIu32 " disagreement(s) in %"
                PRIu32 " frames)", run->what, run->rule_broken,
                run->frames) && ok;

   ok = t_check(t, run->acquire_suboptimal == 0 &&
                run->present_suboptimal == 0 && run->extent_changes == 0,
                "%s: nothing changed under this run and nothing reported "
                "SUBOPTIMAL", run->what) && ok;
   return ok;
}

/* The copy fallback is selected the same way the rest of the suite
 * selects it, and the driver is asked to confirm which path it took
 * rather than being assumed to have obeyed. */
static bool sub_path_is_copy(vkfw *fw)
{
   const char *msg = NULL;
   return vkfw_saw_message(fw, "copy fallback: the rendered image", &msg);
}

/* ------------------------------------------------------------------ */

int run_test(test_ctx *t)
{
   vkfw fw;
   int rv = 0;
   VkSurfaceKHR surface = VK_NULL_HANDLE;

   const char *const instance_exts[] = {
      VK_KHR_SURFACE_EXTENSION_NAME,
      VK_NN_VI_SURFACE_EXTENSION_NAME,
   };
   const char *const device_exts[] = {
      VK_KHR_SWAPCHAIN_EXTENSION_NAME,
   };

   if (!vkfw_init_full(&fw, t, NULL, instance_exts, 2, device_exts, 1)) {
      t_note(t, "if this failed with EXTENSION_NOT_PRESENT, the build has "
                "no VI surface: -Dplatforms=vi is what creates it");
      return 1;
   }

   if (!vkfw_wsi_load(&fw)) {
      rv = 1;
      goto out;
   }

   /* This test never wants the copy path except where it says so, and
    * it runs after other tests in the same session have set the
    * variable in their own process. Cleared explicitly so section A
    * measures the path the hardware chooses. */
   unsetenv("MESA_VK_WSI_HORIZON_FORCE_COPY");

   NWindow *win = nwindowGetDefault();
   if (!t_check(t, win != NULL && nwindowIsValid(win),
                "nwindowGetDefault() returned a valid window")) {
      rv = 1;
      goto out;
   }

   const VkViSurfaceCreateInfoNN sci = {
      .sType = VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN,
      .window = win,
   };
   VkResult r = fw.wsi.vkCreateViSurfaceNN(fw.instance, &sci, NULL, &surface);
   if (!t_check(t, r == VK_SUCCESS && surface != VK_NULL_HANDLE,
                "vkCreateViSurfaceNN over the default window -> %s",
                vkfw_result_str(r))) {
      rv = 1;
      goto out;
   }

   VkExtent2D extent;
   VkResult qr = VK_SUCCESS;
   if (!t_check(t, sub_surface_extent(&fw, surface, &extent, &qr),
                "the surface reports a current extent -> %s",
                vkfw_result_str(qr))) {
      rv = 1;
      goto out_surface;
   }
   t_check(t, extent.width > 0 && extent.height > 0 &&
           extent.width != UINT32_MAX,
           "the surface's currentExtent is a real size (%" PRIu32 "x%" PRIu32
           ")", extent.width, extent.height);

   /* The consumer's own answer, printed beside the surface's, because
    * every rule below is a statement about the second and the first is
    * where it comes from. They are the same number by construction —
    * wsi_horizon_get_extent reads exactly this field — and printing
    * both is what lets a reader of the log see the VI state rather than
    * only the Vulkan one. */
   t_note(t, "the window's consumer reports %" PRIu32 "x%" PRIu32
             "; appletGetOperationMode() is %d (0 handheld, 1 docked)",
          win->default_width, win->default_height,
          (int)appletGetOperationMode());

   /* --- A: normal operation, whichever path the driver picks ------- */

   vkfw_forget_messages(&fw);
   {
      sub_swapchain sc;
      if (!sub_create(&fw, surface, 3, extent, VK_NULL_HANDLE,
                      "A: the default path", &sc)) {
         rv = 1;
         goto out_surface;
      }

      const bool copy = sub_path_is_copy(&fw);
      t_note(t, "A: the driver chose the %s path",
             copy ? "copy fallback" : "zero-copy");

      sub_run run;
      sub_run_init(&run, copy ? "A: normal operation (copy fallback)"
                              : "A: normal operation (zero-copy)");
      sub_present_frames(&fw, &sc, surface, SUB_FRAMES, &run);
      if (!sub_report(t, &run, SUB_FRAMES))
         rv = 1;

      const VkResult q = sub_quiesce(&fw);
      t_check(t, q == VK_SUCCESS, "A: the device is idle before teardown "
              "-> %s", vkfw_result_str(q));
      if (q == VK_SUCCESS)
         sub_destroy(&fw, &sc);
   }

   /* --- B: the same on the copy fallback --------------------------- */

   /* Only meaningful if the driver actually takes the other path, and
    * that is asserted rather than assumed: a run that silently stayed
    * on zero-copy would be section A measured twice. */
   setenv("MESA_VK_WSI_HORIZON_FORCE_COPY", "1", 1);
   vkfw_forget_messages(&fw);
   {
      sub_swapchain sc;
      if (sub_create(&fw, surface, 3, extent, VK_NULL_HANDLE,
                     "B: the forced copy path", &sc)) {
         t_check(t, sub_path_is_copy(&fw),
                 "B: MESA_VK_WSI_HORIZON_FORCE_COPY produced the copy path");

         sub_run run;
         sub_run_init(&run, "B: normal operation (copy fallback, forced)");
         sub_present_frames(&fw, &sc, surface, SUB_FRAMES, &run);
         if (!sub_report(t, &run, SUB_FRAMES))
            rv = 1;

         const VkResult q = sub_quiesce(&fw);
         t_check(t, q == VK_SUCCESS, "B: the device is idle before teardown "
                 "-> %s", vkfw_result_str(q));
         if (q == VK_SUCCESS)
            sub_destroy(&fw, &sc);
      } else {
         rv = 1;
      }
   }
   unsetenv("MESA_VK_WSI_HORIZON_FORCE_COPY");

   /* --- C: the two results stay distinct --------------------------- */

   /* WHY THIS IS IN A FILE ABOUT SUBOPTIMAL. The change that introduced
    * VK_SUBOPTIMAL_KHR took the resize out of the acquire's
    * VK_ERROR_OUT_OF_DATE_KHR paths. If it had taken anything else with
    * it — if a superseded swapchain, or an acquire that cannot be
    * satisfied, had started answering with a success code — the
    * application would present into buffers it no longer owns, or use
    * an image index it never got. Both are checked here, on both
    * present paths, and the surface's extent is checked to be unchanged
    * throughout so that the OUT_OF_DATE below cannot be coming from the
    * resize check at all. */
   for (uint32_t pass = 0; pass < 2; pass++) {
      const bool force_copy = (pass == 1);
      const char *const path = force_copy ? "copy" : "zero-copy";

      if (force_copy)
         setenv("MESA_VK_WSI_HORIZON_FORCE_COPY", "1", 1);
      else
         unsetenv("MESA_VK_WSI_HORIZON_FORCE_COPY");
      vkfw_forget_messages(&fw);

      sub_swapchain a;
      if (!sub_create(&fw, surface, 3, extent, VK_NULL_HANDLE,
                      "C: the first swapchain", &a)) {
         rv = 1;
         break;
      }

      /* One frame, so the swapchain is live and holding an image when
       * it is superseded. The acquire's own result is not the subject
       * here; it is checked only for being a success code. */
      uint32_t held = 0;
      VkResult ar = fw.wsi.vkAcquireNextImageKHR(fw.dev, a.handle,
                                                 SUB_WAIT_NS, VK_NULL_HANDLE,
                                                 a.acquire_fence, &held);
      /* The fence first and unconditionally on a success code, for the
       * reason in sub_frame: the acquire took a reference to it, and a
       * fence destroyed with a signal pending is invalid usage. */
      if (ar == VK_SUCCESS || ar == VK_SUBOPTIMAL_KHR) {
         fw.vk.vkWaitForFences(fw.dev, 1, &a.acquire_fence, VK_TRUE,
                               SUB_WAIT_NS);
         fw.vk.vkResetFences(fw.dev, 1, &a.acquire_fence);
      }
      const bool have_held = (ar == VK_SUCCESS || ar == VK_SUBOPTIMAL_KHR) &&
                             held < a.image_count;
      t_check(t, have_held, "C/%s: an image acquired on the swapchain that "
              "is about to be superseded -> %s", path, vkfw_result_str(ar));
      if (have_held) {
         const VkResult sr = sub_record_and_submit(&fw, &a, held, 0);
         t_check(t, sr == VK_SUCCESS, "C/%s: the held image was rendered "
                 "into -> %s", path, vkfw_result_str(sr));
      }

      /* The recreation Vulkan asks for: the new swapchain exists before
       * the old is destroyed, and naming the old one is what makes the
       * window change hands rather than the creation being refused. */
      sub_swapchain b;
      if (!sub_create(&fw, surface, 3, extent, a.handle,
                      "C: the second swapchain", &b)) {
         sub_destroy(&fw, &a);
         rv = 1;
         break;
      }

      /* The surface has not moved, so nothing below can be a resize. */
      VkExtent2D now;
      if (sub_surface_extent(&fw, surface, &now, NULL)) {
         t_check(t, sub_extent_eq(now, extent),
                 "C/%s: the surface's extent is unchanged (%" PRIu32 "x%"
                 PRIu32 "), so what follows is not a resize", path,
                 now.width, now.height);
      }

      if (have_held) {
         const VkPresentInfoKHR pi = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &a.res[held].render_done,
            .swapchainCount = 1,
            .pSwapchains = &a.handle,
            .pImageIndices = &held,
         };
         const VkResult pr = fw.wsi.vkQueuePresentKHR(fw.queue, &pi);
         t_check(t, pr == VK_ERROR_OUT_OF_DATE_KHR,
                 "C/%s: presenting on the superseded swapchain is "
                 "VK_ERROR_OUT_OF_DATE_KHR and not a success code -> %s",
                 path, vkfw_result_str(pr));
      }

      {
         uint32_t idx = 0;
         const VkResult r2 =
            fw.wsi.vkAcquireNextImageKHR(fw.dev, a.handle, 0, VK_NULL_HANDLE,
                                         a.acquire_fence, &idx);
         t_check(t, r2 == VK_ERROR_OUT_OF_DATE_KHR,
                 "C/%s: acquiring on the superseded swapchain is "
                 "VK_ERROR_OUT_OF_DATE_KHR -> %s", path,
                 vkfw_result_str(r2));
         if (r2 == VK_SUCCESS || r2 == VK_SUBOPTIMAL_KHR) {
            fw.vk.vkWaitForFences(fw.dev, 1, &a.acquire_fence, VK_TRUE,
                                  SUB_WAIT_NS);
            fw.vk.vkResetFences(fw.dev, 1, &a.acquire_fence);
         }
      }

      /* AN ACQUIRE THAT CANNOT BE SATISFIED IS A REFUSAL, NOT A
       * SUCCESS. Drive the live swapchain's acquire until it is
       * refused, whatever number of images that takes — the premise
       * that both images of a two-image swapchain can be held at once
       * is what hung run 15, so it is measured rather than assumed —
       * and check that the refusal is VK_TIMEOUT. VK_SUBOPTIMAL_KHR
       * here would mean an image the application never received. */
      uint32_t got[SUB_MAX_IMAGES] = { 0 };
      uint32_t held_count = 0;
      VkResult refusal = VK_SUCCESS;
      for (uint32_t i = 0; i < b.image_count + 1u; i++) {
         uint32_t idx = 0;
         const VkResult r3 =
            fw.wsi.vkAcquireNextImageKHR(fw.dev, b.handle, SUB_WAIT_NS / 4u,
                                         VK_NULL_HANDLE, b.acquire_fence,
                                         &idx);
         if (r3 != VK_SUCCESS && r3 != VK_SUBOPTIMAL_KHR) {
            refusal = r3;
            break;
         }
         fw.vk.vkWaitForFences(fw.dev, 1, &b.acquire_fence, VK_TRUE,
                               SUB_WAIT_NS);
         fw.vk.vkResetFences(fw.dev, 1, &b.acquire_fence);
         if (idx < b.image_count && held_count < SUB_MAX_IMAGES)
            got[held_count++] = idx;
      }
      t_check(t, refusal == VK_TIMEOUT,
              "C/%s: an acquire that cannot be satisfied is VK_TIMEOUT, not "
              "a success code, after %" PRIu32 " image(s) were held -> %s",
              path, held_count, vkfw_result_str(refusal));

      /* EVERY IMAGE GOES BACK. An acquired image that is never
       * presented is not a leak — it is a swapchain that cannot run,
       * and the next section would inherit it. */
      uint32_t returned = 0;
      for (uint32_t i = 0; i < held_count; i++) {
         const uint32_t idx = got[i];
         if (sub_record_and_submit(&fw, &b, idx, i) != VK_SUCCESS)
            continue;
         const VkPresentInfoKHR pi = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &b.res[idx].render_done,
            .swapchainCount = 1,
            .pSwapchains = &b.handle,
            .pImageIndices = &idx,
         };
         const VkResult pr = fw.wsi.vkQueuePresentKHR(fw.queue, &pi);
         if (pr == VK_SUCCESS || pr == VK_SUBOPTIMAL_KHR)
            returned++;
      }
      t_check(t, returned == held_count,
              "C/%s: every image the refusal test held was presented back "
              "(%" PRIu32 " of %" PRIu32 ")", path, returned, held_count);

      /* And the survivor still runs, which is what says the superseded
       * swapchain's refusals cost it nothing. */
      sub_run run;
      sub_run_init(&run, force_copy
                   ? "C: the survivor after eviction (copy fallback)"
                   : "C: the survivor after eviction (zero-copy)");
      sub_present_frames(&fw, &b, surface, SUB_SHORT_FRAMES, &run);
      if (!sub_report(t, &run, SUB_SHORT_FRAMES))
         rv = 1;

      VkResult q = sub_quiesce(&fw);
      t_check(t, q == VK_SUCCESS, "C/%s: the device is idle before the "
              "superseded swapchain is destroyed -> %s", path,
              vkfw_result_str(q));
      if (q != VK_SUCCESS) {
         rv = 1;
         break;
      }
      sub_destroy(&fw, &a);

      /* Destroying the superseded one must not have taken the window
       * from the survivor — the defect patch 0070 fixed, checked again
       * here because this file creates the same pairing on the copy
       * path where it happened. */
      sub_run after;
      sub_run_init(&after, force_copy
                   ? "C: the survivor after the old one is destroyed (copy)"
                   : "C: the survivor after the old one is destroyed "
                     "(zero-copy)");
      sub_present_frames(&fw, &b, surface, SUB_SHORT_FRAMES, &after);
      if (!sub_report(t, &after, SUB_SHORT_FRAMES))
         rv = 1;

      q = sub_quiesce(&fw);
      t_check(t, q == VK_SUCCESS, "C/%s: the device is idle before teardown "
              "-> %s", path, vkfw_result_str(q));
      if (q == VK_SUCCESS)
         sub_destroy(&fw, &b);
      else
         rv = 1;
   }
   unsetenv("MESA_VK_WSI_HORIZON_FORCE_COPY");

   /* --- D: a real display mode change ------------------------------ */

   /* THE ONLY THING THAT PRODUCES THE CONDITION, and it is not
    * something this process can do to itself. The consumer's default
    * buffer size belongs to VI: the producer side of a BufferQueue can
    * read it and cannot set it, so no sequence of libnx calls available
    * here changes it. Docking or undocking the console does, and that
    * is a hand on a physical console.
    *
    * So this section asks, waits a bounded time, and reports honestly
    * either way. A run in which nobody docks records that the coverage
    * did not happen; it does not quietly pass. */
   {
      t_note(t, "D: DOCK OR UNDOCK THE CONSOLE NOW to exercise "
                "VK_SUBOPTIMAL_KHR. This waits up to %" PRIu64 " s while "
                "presenting; press B to skip it.",
             SUB_MODE_WAIT_NS / UINT64_C(1000000000));

      PadState pad;
      padInitializeDefault(&pad);

      /* Asked again rather than reusing the extent from the top of the
       * file: sections A to C have run since, and if the console was
       * docked during one of them the swapchain to build now is the one
       * for the size the surface reports *now* — a creation at any
       * other size is refused, which would lose this section for a
       * reason that has nothing to do with it. */
      VkExtent2D d_extent = extent;
      (void)sub_surface_extent(&fw, surface, &d_extent, NULL);

      sub_swapchain sc;
      if (sub_create(&fw, surface, 3, d_extent, VK_NULL_HANDLE,
                     "D: the swapchain the mode change happens under",
                     &sc)) {
         sub_run run;
         sub_run_init(&run, "D: presenting while waiting for a mode change");

         VkExtent2D before = d_extent;
         const u64 start = armGetSystemTick();
         bool skipped = false;
         uint32_t frame = 0;
         /* Frames after the change, so "it still presents" is measured
          * over more than the one frame that noticed. */
         uint32_t after_change = 0;

         run.first_extent = d_extent;
         run.last_extent = d_extent;

         while (appletMainLoop()) {
            /* A LONGER ACQUIRE THAN ANYWHERE ELSE IN THIS FILE, and
             * deliberately so. The compositor is being reconfigured
             * around this loop — the display changes mode, VI resizes
             * the layer — and a buffer that takes longer than the two
             * seconds every other section allows is not by itself a
             * defect. It is still bounded: a stall past this is a real
             * finding and is reported as one rather than hanging. */
            if (!sub_frame(&fw, &sc, surface, frame, SUB_WAIT_NS * 3u,
                           &before, &run))
               break;
            frame++;

            if (run.saw_suboptimal) {
               after_change++;
               if (after_change >= SUB_FRAMES)
                  break;
               continue;
            }

            padUpdate(&pad);
            if (padGetButtonsDown(&pad) & HidNpadButton_B) {
               skipped = true;
               break;
            }
            if (armTicksToNs(armGetSystemTick() - start) > SUB_MODE_WAIT_NS)
               break;
         }

         if (run.saw_suboptimal) {
            t_note(t, "D: the mode change happened: the surface went %" PRIu32
                      "x%" PRIu32 " -> %" PRIu32 "x%" PRIu32 " and the first "
                      "SUBOPTIMAL was frame %" PRIu32,
                   run.first_extent.width, run.first_extent.height,
                   run.last_extent.width, run.last_extent.height,
                   run.first_suboptimal_frame);

            /* THE CONTRACT, and every part of it is a separate check.
             *
             * 1. It still presents. `after_change` frames went through
             *    after the first SUBOPTIMAL with no error at all —
             *    which is the difference between this result and
             *    VK_ERROR_OUT_OF_DATE_KHR, and the reason an
             *    application may ignore it. */
            t_check(t, !run.failed && after_change >= SUB_FRAMES,
                    "D: presentation kept succeeding after SUBOPTIMAL "
                    "(%" PRIu32 " further frames, none refused)",
                    after_change);

            /* 2. Both entry points reported it, not only one. */
            t_check(t, run.present_suboptimal > 0 &&
                    run.acquire_suboptimal > 0,
                    "D: both vkQueuePresentKHR and vkAcquireNextImageKHR "
                    "reported SUBOPTIMAL (%" PRIu32 " and %" PRIu32 ")",
                    run.present_suboptimal, run.acquire_suboptimal);

            /* 3. And every one of them agreed with the surface. */
            t_check(t, run.rule_broken == 0,
                    "D: every result agreed with the surface across the "
                    "change (%" PRIu32 " disagreement(s))", run.rule_broken);

            /* 4. The surface now describes the new display, so an
             *    application that asks what to build gets the answer
             *    that ends the loop rather than the size it already
             *    has. */
            t_check(t, !sub_extent_eq(run.last_extent, d_extent),
                    "D: the surface reports the new extent, so recreating "
                    "cannot reproduce the suboptimal swapchain");

            /* 5. A swapchain at the OLD extent is refused now. */
            {
               const VkSwapchainCreateInfoKHR ci = {
                  .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                  .surface = surface,
                  .minImageCount = 3,
                  .imageFormat = VK_FORMAT_R8G8B8A8_UNORM,
                  .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
                  .imageExtent = d_extent,
                  .imageArrayLayers = 1,
                  .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                  .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                  .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
                  .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                  .presentMode = VK_PRESENT_MODE_FIFO_KHR,
                  .clipped = VK_TRUE,
                  .oldSwapchain = sc.handle,
               };
               VkSwapchainKHR h = VK_NULL_HANDLE;
               const VkResult sr =
                  fw.wsi.vkCreateSwapchainKHR(fw.dev, &ci, NULL, &h);
               t_check(t, sr != VK_SUCCESS,
                       "D: a swapchain at the superseded extent (%" PRIu32
                       "x%" PRIu32 ") is refused -> %s", d_extent.width,
                       d_extent.height, vkfw_result_str(sr));
               if (sr == VK_SUCCESS)
                  fw.wsi.vkDestroySwapchainKHR(fw.dev, h, NULL);
            }

            /* 6. AND THE RECOVERY AN APPLICATION ACTUALLY PERFORMS.
             *    Recreate at what the surface now says, present, and
             *    the results go back to plain VK_SUCCESS — which is
             *    the whole point of having reported SUBOPTIMAL rather
             *    than nothing. */
            sub_swapchain fresh;
            if (sub_create(&fw, surface, 3, run.last_extent, sc.handle,
                           "D: the recreated swapchain", &fresh)) {
               sub_run again;
               sub_run_init(&again, "D: after recreating at the new extent");
               sub_present_frames(&fw, &fresh, surface, SUB_SHORT_FRAMES,
                                  &again);
               if (!sub_report(t, &again, SUB_SHORT_FRAMES))
                  rv = 1;

               VkResult q = sub_quiesce(&fw);
               t_check(t, q == VK_SUCCESS, "D: the device is idle before "
                       "the suboptimal swapchain is destroyed -> %s",
                       vkfw_result_str(q));
               if (q == VK_SUCCESS) {
                  sub_destroy(&fw, &sc);
                  memset(&sc, 0, sizeof(sc));

                  /* And destroying it left the new one presenting. */
                  sub_run last;
                  sub_run_init(&last, "D: after the suboptimal swapchain "
                                      "was destroyed");
                  sub_present_frames(&fw, &fresh, surface, SUB_SHORT_FRAMES,
                                     &last);
                  if (!sub_report(t, &last, SUB_SHORT_FRAMES))
                     rv = 1;
               } else {
                  rv = 1;
               }

               q = sub_quiesce(&fw);
               if (q == VK_SUCCESS)
                  sub_destroy(&fw, &fresh);
               else
                  rv = 1;
            } else {
               rv = 1;
            }
         } else {
            /* NOT A PASS AND NOT A FAILURE: coverage that did not run.
             * Said in the log in as many words, because a section that
             * quietly emitted nothing would be indistinguishable from
             * one that ran and found nothing. */
            t_note(t, "D: no mode change happened (%s after %" PRIu32
                      " frames), so SUBOPTIMAL was never provoked and THIS "
                      "COVERAGE DID NOT RUN. Nothing in this process can "
                      "resize a VI layer; only docking the console can.",
                   skipped ? "skipped with B" : "the wait expired", frame);
            /* What can still be said: nothing went wrong while waiting,
             * and no SUBOPTIMAL appeared without a reason to. */
            t_check(t, !run.failed && run.rule_broken == 0,
                    "D: %" PRIu32 " frames presented while waiting, with no "
                    "unexplained SUBOPTIMAL and no failure", run.frames);
         }

         if (sc.handle != VK_NULL_HANDLE) {
            const VkResult q = sub_quiesce(&fw);
            t_check(t, q == VK_SUCCESS, "D: the device is idle before "
                    "teardown -> %s", vkfw_result_str(q));
            if (q == VK_SUCCESS)
               sub_destroy(&fw, &sc);
            else
               rv = 1;
         }
      } else {
         rv = 1;
      }
   }

   /* --- E: recreation, repeatedly, and nothing retained ------------- */

   /* The recovery sequence an application runs after SUBOPTIMAL, done
    * enough times that anything retained accumulates: create naming the
    * previous one, present, destroy the previous one. A slot never
    * given back, a window never released or an image never freed shows
    * up as a later generation failing to acquire — which is a stronger
    * statement than counting handles, because it is the failure an
    * application would actually hit.
    *
    * The present path alternates, because the two paths retain
    * different things: the zero-copy path holds BufferQueue slots and
    * the fallback holds a libnx Framebuffer. */
   {
      sub_swapchain cur;
      memset(&cur, 0, sizeof(cur));
      bool have_cur = false;
      uint32_t generations = 0;
      uint32_t full_runs = 0;

      for (uint32_t g = 0; g < SUB_GENERATIONS; g++) {
         const bool force_copy = ((g & 1u) != 0);
         if (force_copy)
            setenv("MESA_VK_WSI_HORIZON_FORCE_COPY", "1", 1);
         else
            unsetenv("MESA_VK_WSI_HORIZON_FORCE_COPY");

         /* The surface is asked every generation rather than reusing
          * `extent`: if the console was docked in section D and left
          * docked, the right size to build is the one it says now. */
         VkExtent2D want = extent;
         if (!sub_surface_extent(&fw, surface, &want, NULL))
            break;

         sub_swapchain next;
         char what[64];
         snprintf(what, sizeof(what), "E: generation %" PRIu32 " (%s)", g,
                  force_copy ? "copy" : "zero-copy");
         if (!sub_create(&fw, surface, 3, want,
                         have_cur ? cur.handle : VK_NULL_HANDLE, what,
                         &next)) {
            rv = 1;
            break;
         }
         generations++;

         /* The previous generation goes now, while the new one is live
          * — the ordinary recreate sequence, and the one that took the
          * console down before patch 0070. */
         if (have_cur) {
            const VkResult q = sub_quiesce(&fw);
            if (q != VK_SUCCESS) {
               t_check(t, false, "%s: the device is idle before the previous "
                       "generation is destroyed -> %s", what,
                       vkfw_result_str(q));
               sub_destroy(&fw, &next);
               rv = 1;
               break;
            }
            sub_destroy(&fw, &cur);
         }
         cur = next;
         have_cur = true;

         sub_run run;
         sub_run_init(&run, what);
         sub_present_frames(&fw, &cur, surface, SUB_SHORT_FRAMES, &run);
         if (run.rule_broken != 0 || run.failed)
            t_note(t, "%s: %" PRIu32 " frames, %" PRIu32 " rule "
                      "disagreement(s)", what, run.frames, run.rule_broken);
         if (!run.failed && run.frames == SUB_SHORT_FRAMES &&
             run.rule_broken == 0)
            full_runs++;
      }

      unsetenv("MESA_VK_WSI_HORIZON_FORCE_COPY");

      t_check(t, generations == SUB_GENERATIONS &&
              full_runs == SUB_GENERATIONS,
              "E: %" PRIu32 " of %" PRIu32 " recreation generations each "
              "presented %" PRIu32 " frames with no retained slot and no "
              "rule disagreement", full_runs, SUB_GENERATIONS,
              SUB_SHORT_FRAMES);

      if (have_cur) {
         const VkResult q = sub_quiesce(&fw);
         t_check(t, q == VK_SUCCESS, "E: the device is idle before teardown "
                 "-> %s", vkfw_result_str(q));
         if (q == VK_SUCCESS)
            sub_destroy(&fw, &cur);
         else
            rv = 1;
      }

      /* THE WINDOW CAME BACK, and that is a different claim from "the
       * generations passed": every one of them handed the window on to
       * a successor, and none of them was the last. This one is, so if
       * teardown does not give the window back, the swapchain after it
       * cannot have it. */
      sub_swapchain final_sc;
      VkExtent2D want = extent;
      if (sub_surface_extent(&fw, surface, &want, NULL) &&
          sub_create(&fw, surface, 2, want, VK_NULL_HANDLE,
                     "E: a swapchain after every generation was destroyed",
                     &final_sc)) {
         sub_run run;
         sub_run_init(&run, "E: after every swapchain was destroyed");
         sub_present_frames(&fw, &final_sc, surface, SUB_SHORT_FRAMES, &run);
         if (!sub_report(t, &run, SUB_SHORT_FRAMES))
            rv = 1;

         const VkResult q = sub_quiesce(&fw);
         t_check(t, q == VK_SUCCESS, "E: the device is idle before the last "
                 "teardown -> %s", vkfw_result_str(q));
         if (q == VK_SUCCESS)
            sub_destroy(&fw, &final_sc);
         else
            rv = 1;
      } else {
         rv = 1;
      }
   }

   /* The driver says whether it kept anything, and it says so on
    * stderr — which main() has dup2'd into this log. Asked before the
    * device is destroyed as well as after, because a slot the WSI
    * refused to give back is reported at the moment it happens. */
   {
      bool found = false;
      if (t_log_scan(t, "stays ACQUIRED", &found)) {
         t_check(t, !found, "no slot was left with the compositor: nothing "
                 "in this log says a cancel failed");
      } else {
         t_check(t, false, "the log could not be read back, so whether a "
                 "slot was retained was NOT checked");
      }
   }

out_surface:
   if (surface != VK_NULL_HANDLE)
      fw.wsi.vkDestroySurfaceKHR(fw.instance, surface, NULL);

out:
   vkfw_finish(&fw);

   /* The teardown check the rest of the suite uses, and it runs after
    * the device is gone because that is when horizon_gpu says so: it
    * refuses to destroy a memory object with a live GPU mapping and
    * refuses to destroy the device while any memory survives, both as
    * mesa_loge lines in this file. The message deliberately does not
    * contain the string it looks for, so a failure cannot make a later
    * scan match itself. */
   bool could_not_destroy = false;
   if (t_log_scan(t, "destroy refused", &could_not_destroy)) {
      t_check(t, !could_not_destroy,
              "the driver tore down every object it created: nothing in "
              "this log says it could not");
   } else {
      t_check(t, false,
              "the log could not be read back, so whether the driver tore "
              "everything down was NOT checked");
   }
   return rv;
}
