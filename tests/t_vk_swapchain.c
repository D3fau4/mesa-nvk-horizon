/*
 * Phase 6 test — t_vk_swapchain: a VK_KHR_swapchain on Horizon's
 * compositor, and the four things the phase has to be able to show.
 *
 * WHAT THIS MEASURES, and it is one section each:
 *
 *   A. The surface exists and answers. VK_NN_vi_surface over the
 *      application's own NWindow, and capabilities that say what this
 *      backend actually offers rather than what is convenient: a fixed
 *      extent, two to four images, the four formats an NvGraphicBuffer
 *      can describe.
 *
 *   B. A rotating colour presents at the display's refresh rate. The
 *      colour is a vkCmdClearColorImage, which needs no pipeline and no
 *      shader, so a failure here is the swapchain's and nothing else's.
 *
 *   C. Double and triple buffering differ, in numbers. With nothing to
 *      render both simply track the display, so the difference is made
 *      visible the way it appears in a real application: a load that
 *      alternates between cheap and expensive frames, averaging under
 *      one refresh and peaking over it. Two buffers must round every
 *      overrun up to a whole extra refresh; three can absorb it.
 *
 *   D. Two swapchains over one window coexist and are destroyed
 *      independently. This is the case the reference ports fixed with a
 *      file-scope pointer: Vulkan creates the new swapchain before
 *      destroying the old, so both exist over one window at once, and
 *      exactly one of them may present. The test checks that the older
 *      one starts reporting VK_ERROR_OUT_OF_DATE_KHR, that the newer
 *      one keeps working, and that destroying them in either order
 *      leaves the survivor presenting.
 *
 *   F. A PATTERN THE OPERATOR CAN DESCRIBE, and it is the only check
 *      here that can catch a wrong memory layout. Everything else this
 *      test presents is a solid colour — which is the same image under
 *      any block-linear swizzle, so a wrong GOB sector ordering, a
 *      wrong block height or a wrong stride would pass every one of
 *      them. Nothing can read a presented frame back, and a readback
 *      through the GPU would cancel the error out: it would write and
 *      read with the same layout and agree with itself. The compositor
 *      is the other party to the agreement and the eye is the only
 *      instrument that sees its side, so the test presents a pattern
 *      whose correct appearance is written down in the log, and the
 *      operator says whether that is what appeared. A layout error is
 *      not subtle when it happens: the sector ordering scrambles the
 *      image at 16-byte granularity.
 *
 *   E. The zero-copy decision is observable at run time and says why.
 *      Observable means the application can hear it, not that a log
 *      contains it: the backend reports the decision through the
 *      debug-utils messenger, and this test asserts on the message.
 *      Both paths are exercised in the same run — MESA_VK_WSI_HORIZON_
 *      FORCE_COPY takes the other one — because a decision that can
 *      only come out one way is a constant, not a decision.
 *
 * HOW A PRESENT IS PROVED, given that nothing can be read back. A
 * presented frame is gone: there is no buffer to compare against and
 * "no errors were reported" is not evidence. Four independent things
 * are recorded instead, and none of them can be produced by this
 * process alone:
 *
 *   - the frame cadence, which can only lock to the display's refresh
 *     if something is consuming at that rate;
 *   - the acquire, which can only return a buffer the compositor has
 *     released, so presenting more frames than the swapchain has images
 *     requires the compositor to have consumed the difference;
 *   - the pacing difference between two and three buffers, which is a
 *     property of a real queue and not of a loop;
 *   - and the operator, who sees the colours.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include "common/vkfw.h"

const char *const test_name = "t_vk_swapchain";

/* This test owns the display: no console, and the SD-card log is the
 * whole record (testfw.h). */
const bool test_uses_display = true;

/* Frames per measured run. 90 at 60 Hz is a second and a half — long
 * enough for a mean to mean something, short enough that six runs and
 * teardown stay inside a quarter-minute of the operator's time. */
#define SC_FRAMES        90u
/* Shorter runs, where what is being checked is behaviour and not
 * pacing. */
#define SC_SHORT_FRAMES  20u

#define SC_REFRESH_HZ    60u
#define SC_REFRESH_NS    (UINT64_C(1000000000) / SC_REFRESH_HZ)

/* A wait that is long enough to be a hang detector and short enough
 * that a hang does not cost the operator the session. */
#define SC_WAIT_NS       UINT64_C(2000000000)

#define SC_MAX_IMAGES    4u

/* ------------------------------------------------------------------ */

typedef struct sc_stats {
   const char *what;

   uint32_t frames_presented;
   uint32_t intervals;
   uint64_t total_ns;
   uint64_t min_ns;
   uint64_t max_ns;
   uint32_t within_10pct;
   uint32_t over_1p5_refresh;

   uint64_t acquire_total_ns;
   uint64_t acquire_max_ns;
   uint64_t cpu_total_ns;     /* record + submit, the frame's own cost */

   uint32_t suboptimal;       /* presents that came back SUBOPTIMAL     */

   bool failed;
   uint32_t fail_frame;
   VkResult fail_result;
   const char *fail_what;
} sc_stats;

static void sc_stats_init(sc_stats *s, const char *what)
{
   memset(s, 0, sizeof(*s));
   s->what = what;
   s->min_ns = UINT64_MAX;
}

static void sc_fail(sc_stats *s, uint32_t frame, VkResult r, const char *what)
{
   if (s->failed)
      return;
   s->failed = true;
   s->fail_frame = frame;
   s->fail_result = r;
   s->fail_what = what;
}

static uint64_t sc_mean_ns(const sc_stats *s)
{
   return s->intervals ? s->total_ns / s->intervals : 0;
}

/* Per-image resources: one command buffer, one semaphore the present
 * waits on, and one fence so a slot is not recorded into while its
 * previous submit is still running. */
typedef struct sc_frame_res {
   VkCommandBuffer cb;
   VkSemaphore render_done;
   VkFence in_flight;
   bool in_flight_valid;   /* false until the first submit */
} sc_frame_res;

typedef struct sc_swapchain {
   VkSwapchainKHR handle;
   uint32_t image_count;
   VkImage images[SC_MAX_IMAGES];
   sc_frame_res res[SC_MAX_IMAGES];
   VkFence acquire_fence;
   VkExtent2D extent;
} sc_swapchain;

/* A visible colour that changes every frame: three phase-shifted ramps,
 * so the screen sweeps through hues rather than flickering. */
static void sc_frame_colour(uint32_t frame, VkClearColorValue *out)
{
   out->float32[0] = (float)((frame * 3u) & 0xffu) / 255.0f;
   out->float32[1] = (float)((frame * 5u + 85u) & 0xffu) / 255.0f;
   out->float32[2] = (float)((frame * 7u + 170u) & 0xffu) / 255.0f;
   out->float32[3] = 1.0f;
}

/* The pattern of section F, written into a host-visible buffer in plain
 * row order. What reaches the screen is the GPU's block-linear
 * encoding of it, so this describes what the operator should see and
 * nothing about how it is stored.
 *
 * Every element is here to fail visibly in a different way: the bars
 * catch a wrong stride, the border catches a wrong height or a wrong
 * block height, the diagonal catches a wrong sector ordering (it breaks
 * into steps), and the corner square says which way up the image is. */
static void sc_fill_pattern(uint32_t *px, uint32_t width, uint32_t height)
{
   const uint32_t RED    = 0xff0000ffu;   /* ABGR in memory order */
   const uint32_t GREEN  = 0xff00ff00u;
   const uint32_t BLUE   = 0xffff0000u;
   const uint32_t WHITE  = 0xffffffffu;
   const uint32_t BLACK  = 0xff000000u;
   const uint32_t YELLOW = 0xff00ffffu;

   const uint32_t border = 16;
   const uint32_t corner = 64;
   const uint32_t bar = width / 4u;

   for (uint32_t y = 0; y < height; y++) {
      for (uint32_t x = 0; x < width; x++) {
         uint32_t c;
         if (x < bar)            c = RED;
         else if (x < 2u * bar)  c = GREEN;
         else if (x < 3u * bar)  c = BLUE;
         else                    c = WHITE;

         /* A thick diagonal, in image coordinates rather than in
          * pixels, so it stays a diagonal at any size. */
         const uint32_t dx = (x * height) / width;
         if (dx + 4u >= y && y + 4u >= dx)
            c = BLACK;

         if (x < border || y < border ||
             x + border >= width || y + border >= height)
            c = WHITE;

         if (x >= border && y >= border &&
             x < border + corner && y < border + corner)
            c = YELLOW;

         px[(size_t)y * width + x] = c;
      }
   }
}

/* Burns `ns` of CPU, read from the tick counter rather than counted in
 * iterations, so the load is a duration and not a guess about this
 * core's speed. */
static void sc_busy(uint64_t ns)
{
   if (ns == 0)
      return;
   const u64 start = armGetSystemTick();
   volatile uint32_t sink = 0;
   while (armTicksToNs(armGetSystemTick() - start) < ns)
      sink++;
   (void)sink;
}

/* ------------------------------------------------------------------ */

static void sc_destroy(vkfw *fw, sc_swapchain *sc)
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

static bool sc_create(vkfw *fw, VkSurfaceKHR surface, uint32_t want_images,
                      VkPresentModeKHR mode, VkFormat format,
                      VkExtent2D extent, VkSwapchainKHR old,
                      const char *what, sc_swapchain *sc)
{
   test_ctx *t = fw->t;
   memset(sc, 0, sizeof(*sc));
   sc->extent = extent;

   const VkSwapchainCreateInfoKHR ci = {
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .surface = surface,
      .minImageCount = want_images,
      .imageFormat = format,
      .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
      .imageExtent = extent,
      .imageArrayLayers = 1,
      /* TRANSFER_DST because the frame is a clear; COLOR_ATTACHMENT
       * because that is what a swapchain image is for and asking for it
       * is what proves the backend offers a renderable image rather
       * than a staging buffer. */
      .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      .presentMode = mode,
      .clipped = VK_TRUE,
      .oldSwapchain = old,
   };

   VkResult r = fw->wsi.vkCreateSwapchainKHR(fw->dev, &ci, NULL, &sc->handle);
   if (!t_check(t, r == VK_SUCCESS, "%s: vkCreateSwapchainKHR(%u images) "
                "-> %s", what, want_images, vkfw_result_str(r)))
      return false;

   uint32_t count = 0;
   r = fw->wsi.vkGetSwapchainImagesKHR(fw->dev, sc->handle, &count, NULL);
   if (!t_check(t, r == VK_SUCCESS && count >= 2 && count <= SC_MAX_IMAGES,
                "%s: the swapchain has %u image(s) -> %s", what, count,
                vkfw_result_str(r)))
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
      if (!t_check(t, r == VK_SUCCESS, "%s: command buffer %u -> %s", what,
                   i, vkfw_result_str(r)))
         goto fail;

      const VkSemaphoreCreateInfo sci = {
         .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
      };
      r = fw->wsi.vkCreateSemaphore(fw->dev, &sci, NULL,
                                    &sc->res[i].render_done);
      if (!t_check(t, r == VK_SUCCESS, "%s: semaphore %u -> %s", what, i,
                   vkfw_result_str(r)))
         goto fail;

      r = fw->vk.vkCreateFence(fw->dev, &fci, NULL, &sc->res[i].in_flight);
      if (!t_check(t, r == VK_SUCCESS, "%s: fence %u -> %s", what, i,
                   vkfw_result_str(r)))
         goto fail;
   }

   return true;

fail:
   sc_destroy(fw, sc);
   return false;
}

/* Records and submits one frame: clear the acquired image to `colour`
 * and leave it in PRESENT_SRC. No pipeline, no shader, no attachment —
 * the point of the frame is the present, and everything else is
 * something else that could fail. */
static VkResult sc_record_and_submit(vkfw *fw, sc_swapchain *sc,
                                     uint32_t index, uint32_t frame)
{
   sc_frame_res *res = &sc->res[index];
   VkResult r;

   if (res->in_flight_valid) {
      /* This slot's previous submit has to be finished before its
       * command buffer is recorded again. The application's own
       * throttle, and the one CPU wait Vulkan puts here. */
      r = fw->vk.vkWaitForFences(fw->dev, 1, &res->in_flight, VK_TRUE,
                                 SC_WAIT_NS);
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

   /* UNDEFINED as the old layout: every pixel is about to be written,
    * so there is nothing in the image worth preserving, and this is
    * valid for an image whose previous contents are not needed. */
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
   sc_frame_colour(frame, &colour);
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

/* Records and submits one frame that copies `src` into the acquired
 * image and leaves it in PRESENT_SRC. The buffer holds the pattern in
 * plain row order; the copy is what puts it into whatever layout the
 * image has, so this test needs no swizzle of its own and the layout
 * under test is the driver's rather than a second implementation of
 * it. */
static VkResult sc_record_pattern(vkfw *fw, sc_swapchain *sc, uint32_t index,
                                  VkBuffer src)
{
   sc_frame_res *res = &sc->res[index];
   VkResult r;

   if (res->in_flight_valid) {
      r = fw->vk.vkWaitForFences(fw->dev, 1, &res->in_flight, VK_TRUE,
                                 SC_WAIT_NS);
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
   VkImageMemoryBarrier bar = {
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
                               0, NULL, 0, NULL, 1, &bar);

   const VkBufferImageCopy copy = {
      .bufferOffset = 0,
      .bufferRowLength = 0,     /* tightly packed */
      .bufferImageHeight = 0,
      .imageSubresource = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .layerCount = 1,
      },
      .imageExtent = { sc->extent.width, sc->extent.height, 1 },
   };
   fw->vk.vkCmdCopyBufferToImage(res->cb, src, sc->images[index],
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 1, &copy);

   bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
   bar.dstAccessMask = 0;
   bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   bar.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
   fw->vk.vkCmdPipelineBarrier(res->cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                               0, NULL, 0, NULL, 1, &bar);

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

/* Holds the pattern on screen for `frames` presents. Returns the number
 * that were presented. */
static uint32_t sc_show_pattern(vkfw *fw, sc_swapchain *sc, VkBuffer src,
                                uint32_t frames, VkResult *fail_out)
{
   uint32_t done = 0;
   *fail_out = VK_SUCCESS;

   for (uint32_t frame = 0; frame < frames; frame++) {
      uint32_t index = 0;
      VkResult r = fw->wsi.vkAcquireNextImageKHR(fw->dev, sc->handle,
                                                 SC_WAIT_NS, VK_NULL_HANDLE,
                                                 sc->acquire_fence, &index);
      if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
         *fail_out = r;
         return done;
      }
      r = fw->vk.vkWaitForFences(fw->dev, 1, &sc->acquire_fence, VK_TRUE,
                                 SC_WAIT_NS);
      if (r == VK_SUCCESS)
         r = fw->vk.vkResetFences(fw->dev, 1, &sc->acquire_fence);
      if (r != VK_SUCCESS) {
         *fail_out = r;
         return done;
      }
      if (index >= sc->image_count) {
         *fail_out = VK_ERROR_UNKNOWN;
         return done;
      }

      r = sc_record_pattern(fw, sc, index, src);
      if (r != VK_SUCCESS) {
         *fail_out = r;
         return done;
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
         *fail_out = r;
         return done;
      }
      done++;
   }

   return done;
}

/* One measured run of `frames` presents. Records its failure rather
 * than aborting the test, so the counts afterwards are falsifiable. */
static void sc_run(vkfw *fw, sc_swapchain *sc, uint32_t frames,
                   uint64_t busy_ns_odd, sc_stats *st)
{
   u64 prev_tick = 0;
   bool have_prev = false;

   for (uint32_t frame = 0; frame < frames; frame++) {
      const u64 acq_start = armGetSystemTick();

      uint32_t index = 0;
      VkResult r = fw->wsi.vkAcquireNextImageKHR(fw->dev, sc->handle,
                                                 SC_WAIT_NS,
                                                 VK_NULL_HANDLE,
                                                 sc->acquire_fence, &index);
      if (r == VK_SUBOPTIMAL_KHR) {
         st->suboptimal++;
      } else if (r != VK_SUCCESS) {
         sc_fail(st, frame, r, "vkAcquireNextImageKHR");
         return;
      }

      /* The acquire fence has to be waited on and reset before it is
       * used again — it is the object the swapchain signalled, and
       * reusing a signalled fence unreset is invalid usage. */
      r = fw->vk.vkWaitForFences(fw->dev, 1, &sc->acquire_fence, VK_TRUE,
                                 SC_WAIT_NS);
      if (r != VK_SUCCESS) {
         sc_fail(st, frame, r, "waiting for the acquire fence");
         return;
      }
      r = fw->vk.vkResetFences(fw->dev, 1, &sc->acquire_fence);
      if (r != VK_SUCCESS) {
         sc_fail(st, frame, r, "resetting the acquire fence");
         return;
      }

      const uint64_t acq_ns = armTicksToNs(armGetSystemTick() - acq_start);
      st->acquire_total_ns += acq_ns;
      if (acq_ns > st->acquire_max_ns)
         st->acquire_max_ns = acq_ns;

      if (index >= sc->image_count) {
         sc_fail(st, frame, VK_ERROR_UNKNOWN, "an image index out of range");
         return;
      }

      const u64 cpu_start = armGetSystemTick();
      r = sc_record_and_submit(fw, sc, index, frame);
      if (r != VK_SUCCESS) {
         sc_fail(st, frame, r, "recording and submitting the frame");
         return;
      }
      if ((frame & 1u) != 0)
         sc_busy(busy_ns_odd);
      st->cpu_total_ns += armTicksToNs(armGetSystemTick() - cpu_start);

      const VkPresentInfoKHR pi = {
         .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
         .waitSemaphoreCount = 1,
         .pWaitSemaphores = &sc->res[index].render_done,
         .swapchainCount = 1,
         .pSwapchains = &sc->handle,
         .pImageIndices = &index,
      };
      r = fw->wsi.vkQueuePresentKHR(fw->queue, &pi);
      if (r == VK_SUBOPTIMAL_KHR) {
         st->suboptimal++;
      } else if (r != VK_SUCCESS) {
         sc_fail(st, frame, r, "vkQueuePresentKHR");
         return;
      }
      st->frames_presented++;

      const u64 now = armGetSystemTick();
      if (have_prev) {
         const uint64_t dt = armTicksToNs(now - prev_tick);
         st->total_ns += dt;
         if (dt < st->min_ns)
            st->min_ns = dt;
         if (dt > st->max_ns)
            st->max_ns = dt;
         if (dt >= SC_REFRESH_NS - SC_REFRESH_NS / 10 &&
             dt <= SC_REFRESH_NS + SC_REFRESH_NS / 10)
            st->within_10pct++;
         if (dt > SC_REFRESH_NS + SC_REFRESH_NS / 2)
            st->over_1p5_refresh++;
         st->intervals++;
      }
      prev_tick = now;
      have_prev = true;
   }
}

static bool sc_report(test_ctx *t, const sc_stats *st, uint32_t expected)
{
   const bool ok = t_check(t, !st->failed && st->frames_presented == expected,
                           "%s: %" PRIu32 " of %" PRIu32 " frames presented%s",
                           st->what, st->frames_presented, expected,
                           st->failed ? " (see the note below)" : "");
   if (st->failed)
      t_note(t, "%s: %s at frame %" PRIu32 " -> %s", st->what, st->fail_what,
             st->fail_frame, vkfw_result_str(st->fail_result));

   if (st->intervals == 0) {
      t_note(t, "%s: no intervals recorded", st->what);
      return ok;
   }

   t_note(t, "%s: %" PRIu32 " intervals, mean %" PRIu64 " us, min %" PRIu64
             " us, max %" PRIu64 " us; %" PRIu32 " within 10%% of %" PRIu64
             " us; %" PRIu32 " longer than 1.5 refreshes",
          st->what, st->intervals, sc_mean_ns(st) / 1000, st->min_ns / 1000,
          st->max_ns / 1000, st->within_10pct, SC_REFRESH_NS / 1000,
          st->over_1p5_refresh);
   t_note(t, "%s: acquire mean %" PRIu64 " us / max %" PRIu64 " us; "
             "record+submit mean %" PRIu64 " us; %" PRIu32 " SUBOPTIMAL",
          st->what, (st->acquire_total_ns / st->frames_presented) / 1000,
          st->acquire_max_ns / 1000,
          (st->cpu_total_ns / st->frames_presented) / 1000, st->suboptimal);
   return ok;
}

/* The odd-frame load for the pacing comparison, derived from what a
 * frame already costs rather than guessed. Same shape as t_nwindow's:
 * mean cost comfortably under one refresh, peak over it, so a
 * two-buffer producer has to round every peak up to a whole extra
 * refresh while a three-buffer one absorbs it. Returns 0 when no such
 * load exists, and the caller then says so instead of pretending to
 * have measured a difference. */
static uint64_t sc_choose_load(uint64_t frame_cost_ns)
{
   const uint64_t target = (SC_REFRESH_NS * 3u) / 4u;
   if (frame_cost_ns >= target)
      return 0;

   const uint64_t busy = 2u * (target - frame_cost_ns);
   if (frame_cost_ns + busy <= SC_REFRESH_NS)
      return 0;

   return busy;
}

/* ------------------------------------------------------------------ */

int run_test(test_ctx *t)
{
   vkfw fw;
   int rv = 0;
   /* At function scope and zeroed here, not where it is filled: every
    * early `goto out_surface` above section F passes through the
    * teardown that destroys it. */
   vkfw_buffer pattern;
   memset(&pattern, 0, sizeof(pattern));
   VkSurfaceKHR surface = VK_NULL_HANDLE;

   t_note(t, "this test owns the display: no console was started, and "
             "this file is the whole record");

   const char *const instance_exts[] = {
      VK_KHR_SURFACE_EXTENSION_NAME,
      VK_NN_VI_SURFACE_EXTENSION_NAME,
   };
   const char *const device_exts[] = {
      VK_KHR_SWAPCHAIN_EXTENSION_NAME,
   };

   /* Dynamic rendering is not used here — the frame is a clear — so the
    * fixture is asked for nothing beyond the extensions. */
   if (!vkfw_init_full(&fw, t, NULL, instance_exts, 2, device_exts, 1)) {
      t_note(t, "if this failed with EXTENSION_NOT_PRESENT, the build has "
                "no VI surface: -Dplatforms=vi is what creates it");
      return 1;
   }

   if (!vkfw_wsi_load(&fw)) {
      rv = 1;
      goto out;
   }

   /* --- A: the surface, and what it says about itself -------------- */

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

   VkBool32 supported = VK_FALSE;
   r = fw.wsi.vkGetPhysicalDeviceSurfaceSupportKHR(fw.pdev, fw.queue_family,
                                                   surface, &supported);
   t_check(t, r == VK_SUCCESS && supported == VK_TRUE,
           "queue family %" PRIu32 " can present to this surface -> %s",
           fw.queue_family, vkfw_result_str(r));

   VkSurfaceCapabilitiesKHR caps;
   memset(&caps, 0, sizeof(caps));
   r = fw.wsi.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(fw.pdev, surface,
                                                        &caps);
   if (!t_check(t, r == VK_SUCCESS, "surface capabilities -> %s",
                vkfw_result_str(r))) {
      rv = 1;
      goto out_surface;
   }

   t_note(t, "surface: %" PRIu32 "x%" PRIu32 ", %" PRIu32 " to %" PRIu32
             " images, usage 0x%08x", caps.currentExtent.width,
          caps.currentExtent.height, caps.minImageCount, caps.maxImageCount,
          caps.supportedUsageFlags);

   /* Two, because that is what makes double buffering expressible; four,
    * because that is the cap docs/wsi.md § 2.3 fixes. Both are asserted
    * rather than noted: they are the contract every count below rests
    * on, and Vulkan derives from minImageCount how many images an
    * application may hold at once. */
   t_check(t, caps.minImageCount == 2,
           "minImageCount is 2, so double buffering exists (%" PRIu32 ")",
           caps.minImageCount);
   t_check(t, caps.maxImageCount == 4,
           "maxImageCount is 4 (%" PRIu32 ")", caps.maxImageCount);
   t_check(t, caps.currentExtent.width > 0 && caps.currentExtent.height > 0 &&
           caps.currentExtent.width != UINT32_MAX,
           "the surface reports a real current extent");
   t_check(t, (caps.supportedUsageFlags &
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0 &&
           (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0,
           "swapchain images can be rendered to and copied into");
   t_check(t, caps.maxImageArrayLayers >= 1 &&
           caps.currentTransform == VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
           "the surface has no transform of its own");

   const VkExtent2D extent = caps.currentExtent;

   uint32_t format_count = 0;
   r = fw.wsi.vkGetPhysicalDeviceSurfaceFormatsKHR(fw.pdev, surface,
                                                   &format_count, NULL);
   VkSurfaceFormatKHR formats[8];
   if (format_count > 8)
      format_count = 8;
   if (r == VK_SUCCESS && format_count > 0) {
      r = fw.wsi.vkGetPhysicalDeviceSurfaceFormatsKHR(fw.pdev, surface,
                                                      &format_count, formats);
   }
   bool have_rgba8 = false;
   if (r == VK_SUCCESS) {
      for (uint32_t i = 0; i < format_count; i++) {
         if (formats[i].format == VK_FORMAT_R8G8B8A8_UNORM)
            have_rgba8 = true;
      }
   }
   t_check(t, r == VK_SUCCESS && have_rgba8,
           "the surface offers %" PRIu32 " format(s), including "
           "R8G8B8A8_UNORM -> %s", format_count, vkfw_result_str(r));

   uint32_t mode_count = 0;
   r = fw.wsi.vkGetPhysicalDeviceSurfacePresentModesKHR(fw.pdev, surface,
                                                        &mode_count, NULL);
   VkPresentModeKHR modes[8];
   if (mode_count > 8)
      mode_count = 8;
   if (r == VK_SUCCESS && mode_count > 0) {
      r = fw.wsi.vkGetPhysicalDeviceSurfacePresentModesKHR(fw.pdev, surface,
                                                           &mode_count,
                                                           modes);
   }
   bool have_fifo = false, have_immediate = false;
   if (r != VK_SUCCESS)
      mode_count = 0;
   for (uint32_t i = 0; i < mode_count; i++) {
      if (modes[i] == VK_PRESENT_MODE_FIFO_KHR)
         have_fifo = true;
      if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR)
         have_immediate = true;
   }
   t_check(t, r == VK_SUCCESS && have_fifo,
           "VK_PRESENT_MODE_FIFO_KHR is offered (%" PRIu32 " mode(s)) -> %s",
           mode_count, vkfw_result_str(r));
   t_note(t, "VK_PRESENT_MODE_IMMEDIATE_KHR is %soffered",
          have_immediate ? "" : "not ");

   /* --- B: a rotating colour, three images, FIFO ------------------- */

   sc_swapchain sc3;
   sc_stats st3, st2, st3_load, st2_load, st_zc, st_copy;
   sc_stats_init(&st3, "3 images, FIFO");
   sc_stats_init(&st2, "2 images, FIFO");
   sc_stats_init(&st3_load, "3 images, FIFO, bursty load");
   sc_stats_init(&st2_load, "2 images, FIFO, bursty load");
   sc_stats_init(&st_zc, "the default path");
   sc_stats_init(&st_copy, "the forced copy path");
   bool ran3 = false, ran3_load = false, ran2_load = false;
   uint64_t frame_cost_ns = 0;

   vkfw_forget_messages(&fw);
   if (!sc_create(&fw, surface, 3, VK_PRESENT_MODE_FIFO_KHR,
                  VK_FORMAT_R8G8B8A8_UNORM, extent, VK_NULL_HANDLE,
                  "3 images, FIFO", &sc3)) {
      rv = 1;
      goto out_surface;
   }

   t_check(t, sc3.image_count == 3,
           "asking for 3 images produced 3 (%" PRIu32 ")", sc3.image_count);

   /* THE DECISION, HEARD BY THE APPLICATION. Not read out of a log: the
    * backend reports it through the debug-utils messenger, which is the
    * difference between "observable at run time" and "written down
    * somewhere". */
   const char *decision = NULL;
   const bool heard_zero_copy =
      vkfw_saw_message(&fw, "zero-copy: the swapchain images", &decision);
   const bool heard_fallback =
      vkfw_saw_message(&fw, "copy fallback", &decision);
   t_check(t, heard_zero_copy || heard_fallback,
           "the swapchain reported which present path it got");
   if (decision != NULL)
      t_note(t, "the driver said: %s", decision);

   /* --- F: the pattern, which is what says the layout is right ----- */

   /* Held for two seconds so the operator has time to look at it. This
    * is the only check in the file that can catch a wrong memory
    * layout: every other frame here is a solid colour, and a solid
    * colour is the same image under any swizzle. */
   {
      const VkDeviceSize pattern_B =
         (VkDeviceSize)extent.width * extent.height * 4u;
      if (vkfw_buffer_create(&fw, pattern_B, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &pattern) &&
          pattern.map != NULL) {
         sc_fill_pattern((uint32_t *)pattern.map, extent.width, extent.height);
         if (vkfw_buffer_flush(&fw, &pattern)) {
            t_note(t, "SHOWING THE PATTERN FOR TWO SECONDS. What should be "
                      "on screen, and what a wrong memory layout would "
                      "destroy: four full-height vertical bars, red then "
                      "green then blue then white, left to right; a white "
                      "border 16 pixels wide all the way round; a black "
                      "diagonal from the top-left corner to the "
                      "bottom-right; and a yellow square just inside the "
                      "top-left corner. Anything scrambled, striped or "
                      "blocky means the compositor and the driver disagree "
                      "about the block-linear layout");

            VkResult pattern_fail = VK_SUCCESS;
            const uint32_t shown = sc_show_pattern(&fw, &sc3, pattern.buf,
                                                   2u * SC_REFRESH_HZ,
                                                   &pattern_fail);
            t_check(t, shown == 2u * SC_REFRESH_HZ,
                    "the pattern was presented %" PRIu32 " times -> %s",
                    shown, vkfw_result_str(pattern_fail));
            t_note(t, "OPERATOR: say in the report whether the four bars, "
                      "the border, the diagonal and the yellow corner were "
                      "all there and in that order. That answer is the "
                      "whole of the layout evidence for this phase");
         }
      } else {
         t_note(t, "the pattern buffer could not be created or mapped; "
                   "the layout has no evidence in this run");
      }
   }

   sc_run(&fw, &sc3, SC_FRAMES, 0, &st3);
   ran3 = sc_report(t, &st3, SC_FRAMES);

   if (st3.frames_presented > 0)
      frame_cost_ns = st3.cpu_total_ns / st3.frames_presented;

   if (ran3) {
      /* Presenting at the refresh rate, as a proportion rather than a
       * mean that one stall could move. */
      t_check(t, st3.within_10pct * 10 >= st3.intervals * 9,
              "3 images, FIFO: %" PRIu32 " of %" PRIu32 " intervals are "
              "within 10%% of %" PRIu64 " us (>= 90%% needed)",
              st3.within_10pct, st3.intervals, SC_REFRESH_NS / 1000);

      /* Something outside this process consumed the frames: with three
       * images and 90 presents, at least 87 acquires had to return a
       * buffer the compositor had released, and this process releases
       * none of them. */
      t_check(t, st3.frames_presented > sc3.image_count,
              "%" PRIu32 " frames were presented through %" PRIu32
              " images, so the compositor released at least %" PRIu32
              " of them", st3.frames_presented, sc3.image_count,
              st3.frames_presented - sc3.image_count);
   }

   /* --- C: three images against two, under the same bursty load ---- */

   const uint64_t load_ns = sc_choose_load(frame_cost_ns);
   t_note(t, "a frame's own CPU cost measured %" PRIu64 " us; the bursty "
             "load adds %" PRIu64 " us to every odd frame, for a mean of "
             "%" PRIu64 " us against a %" PRIu64 " us refresh",
          frame_cost_ns / 1000, load_ns / 1000,
          (frame_cost_ns + load_ns / 2) / 1000, SC_REFRESH_NS / 1000);

   if (load_ns == 0) {
      t_note(t, "NO PACING COMPARISON WAS MADE: a frame already costs "
                "%" PRIu64 " us of a %" PRIu64 " us refresh, so no load "
                "averages under one refresh and peaks over it. Skipped "
                "rather than reported from numbers that cannot show it",
             frame_cost_ns / 1000, SC_REFRESH_NS / 1000);
   } else {
      sc_run(&fw, &sc3, SC_FRAMES, load_ns, &st3_load);
      ran3_load = sc_report(t, &st3_load, SC_FRAMES);
   }

   sc_destroy(&fw, &sc3);

   sc_swapchain sc2;
   if (!sc_create(&fw, surface, 2, VK_PRESENT_MODE_FIFO_KHR,
                  VK_FORMAT_R8G8B8A8_UNORM, extent, VK_NULL_HANDLE,
                  "2 images, FIFO", &sc2)) {
      rv = 1;
      goto out_surface;
   }
   t_check(t, sc2.image_count == 2,
           "asking for 2 images produced 2 (%" PRIu32 ")", sc2.image_count);

   sc_run(&fw, &sc2, SC_FRAMES, 0, &st2);
   sc_report(t, &st2, SC_FRAMES);

   if (load_ns != 0) {
      sc_run(&fw, &sc2, SC_FRAMES, load_ns, &st2_load);
      ran2_load = sc_report(t, &st2_load, SC_FRAMES);
   }

   if (ran3_load && ran2_load) {
      const uint64_t m3 = sc_mean_ns(&st3_load);
      const uint64_t m2 = sc_mean_ns(&st2_load);
      t_note(t, "bursty load: mean interval %" PRIu64 " us with 3 images, "
                "%" PRIu64 " us with 2; intervals longer than 1.5 refreshes: "
                "%" PRIu32 " with 3, %" PRIu32 " with 2",
             m3 / 1000, m2 / 1000, st3_load.over_1p5_refresh,
             st2_load.over_1p5_refresh);
      /* 10% is far below the 50% the arithmetic predicts and far above
       * measurement noise. */
      t_check(t, m2 * 10 > m3 * 11,
              "under the same bursty load two images pace at least 10%% "
              "slower than three (%" PRIu64 " us vs %" PRIu64 " us)",
              m2 / 1000, m3 / 1000);
   }

   /* --- D: two swapchains over one window -------------------------- */

   /* sc2 is still alive and presenting. A second swapchain is created
    * over the same surface, which is exactly Vulkan's recreation
    * contract — the new one exists before the old one is destroyed —
    * and exactly the collision the reference ports fixed with a
    * file-scope pointer. */
   sc_swapchain scB;
   if (!sc_create(&fw, surface, 3, VK_PRESENT_MODE_FIFO_KHR,
                  VK_FORMAT_R8G8B8A8_UNORM, extent, sc2.handle,
                  "the second swapchain", &scB)) {
      rv = 1;
      goto out_sc2;
   }

   t_check(t, scB.handle != sc2.handle && scB.image_count == 3,
           "two swapchains exist over one window at the same time");

   /* The older one must stop presenting, and say so rather than
    * silently drawing nowhere. */
   {
      uint32_t index = 0;
      VkResult old_r =
         fw.wsi.vkAcquireNextImageKHR(fw.dev, sc2.handle, 0, VK_NULL_HANDLE,
                                      sc2.acquire_fence, &index);
      t_check(t, old_r == VK_ERROR_OUT_OF_DATE_KHR,
              "the superseded swapchain reports VK_ERROR_OUT_OF_DATE_KHR "
              "rather than presenting into buffers it no longer owns "
              "-> %s", vkfw_result_str(old_r));
      if (old_r == VK_SUCCESS) {
         /* It handed one over anyway; put it back so teardown is not
          * left holding it. */
         fw.vk.vkWaitForFences(fw.dev, 1, &sc2.acquire_fence, VK_TRUE,
                               SC_WAIT_NS);
         fw.vk.vkResetFences(fw.dev, 1, &sc2.acquire_fence);
      }
   }

   /* And the newer one must work. */
   sc_stats st_b;
   sc_stats_init(&st_b, "the second swapchain, while the first still exists");
   sc_run(&fw, &scB, SC_SHORT_FRAMES, 0, &st_b);
   sc_report(t, &st_b, SC_SHORT_FRAMES);

   /* Destroy the OLDER one first, which is the order Vulkan
    * applications use and the order that would break if the window's
    * registration were owned by whoever destroyed last. */
   r = fw.vk.vkDeviceWaitIdle(fw.dev);
   t_check(t, r == VK_SUCCESS, "the device is idle before destroying the "
           "superseded swapchain -> %s", vkfw_result_str(r));
   sc_destroy(&fw, &sc2);

   sc_stats st_b2;
   sc_stats_init(&st_b2, "the second swapchain, after the first is destroyed");
   sc_run(&fw, &scB, SC_SHORT_FRAMES, 0, &st_b2);
   const bool b2_ok = sc_report(t, &st_b2, SC_SHORT_FRAMES);
   t_check(t, b2_ok,
           "destroying the superseded swapchain left the live one "
           "presenting");

   r = fw.vk.vkDeviceWaitIdle(fw.dev);
   t_check(t, r == VK_SUCCESS, "the device is idle before teardown -> %s",
           vkfw_result_str(r));
   sc_destroy(&fw, &scB);

   /* --- E: the decision, both ways --------------------------------- */

   /* The path the hardware chooses on its own. */
   vkfw_forget_messages(&fw);
   sc_swapchain sc_a;
   if (sc_create(&fw, surface, 3, VK_PRESENT_MODE_FIFO_KHR,
                 VK_FORMAT_R8G8B8A8_UNORM, extent, VK_NULL_HANDLE,
                 "the default path", &sc_a)) {
      const char *msg = NULL;
      const bool zc = vkfw_saw_message(&fw, "zero-copy: the swapchain images",
                                       &msg);
      t_note(t, "with nothing forced, the driver chose: %s",
             msg != NULL ? msg : "(no decision message)");

      st_zc.what = zc ? "the default path (zero-copy)"
                      : "the default path (copy fallback)";
      sc_run(&fw, &sc_a, SC_FRAMES, 0, &st_zc);
      sc_report(t, &st_zc, SC_FRAMES);

      fw.vk.vkDeviceWaitIdle(fw.dev);
      sc_destroy(&fw, &sc_a);

      /* And the other path, on purpose. */
      setenv("MESA_VK_WSI_HORIZON_FORCE_COPY", "1", 1);
      vkfw_forget_messages(&fw);

      sc_swapchain sc_c;
      if (sc_create(&fw, surface, 3, VK_PRESENT_MODE_FIFO_KHR,
                    VK_FORMAT_R8G8B8A8_UNORM, extent, VK_NULL_HANDLE,
                    "the forced copy path", &sc_c)) {
         const char *copy_msg = NULL;
         const bool copied = vkfw_saw_message(&fw, "copy fallback",
                                              &copy_msg);
         t_check(t, copied,
                 "MESA_VK_WSI_HORIZON_FORCE_COPY produced the copy path, "
                 "and the driver said so");
         if (copy_msg != NULL)
            t_note(t, "the driver said: %s", copy_msg);

         /* The two paths must be distinguishable at run time, which is
          * the exit criterion. The strongest available evidence that
          * they are different code is that the driver named them
          * differently for the same application; the timings below are
          * the second, weaker kind, and are reported without a
          * threshold because a copy that happens to be fast is not a
          * failure. */
         sc_run(&fw, &sc_c, SC_FRAMES, 0, &st_copy);
         sc_report(t, &st_copy, SC_FRAMES);

         if (zc) {
            t_check(t, copied,
                    "the same application got a different present path when "
                    "it asked for one: zero-copy by default, the copy "
                    "fallback when forced");
         } else {
            t_note(t, "the default path was ALREADY the copy fallback, so "
                      "forcing it changes nothing. The reason zero-copy was "
                      "declined is in the message above, and it is the "
                      "finding of this section");
         }

         t_note(t, "record+submit cost per frame: %" PRIu64 " us on the "
                   "default path, %" PRIu64 " us on the forced copy path",
                st_zc.frames_presented ?
                   (st_zc.cpu_total_ns / st_zc.frames_presented) / 1000 : 0,
                st_copy.frames_presented ?
                   (st_copy.cpu_total_ns / st_copy.frames_presented) / 1000
                   : 0);

         fw.vk.vkDeviceWaitIdle(fw.dev);
         sc_destroy(&fw, &sc_c);
      }
      /* Set to "0" rather than unset: debug_get_bool_option reads it
       * either way, and setenv is the call this tree already knows
       * works on this C library. */
      setenv("MESA_VK_WSI_HORIZON_FORCE_COPY", "0", 1);
   }

   goto out_surface;

out_sc2:
   fw.vk.vkDeviceWaitIdle(fw.dev);
   sc_destroy(&fw, &sc2);

out_surface:
   fw.vk.vkDeviceWaitIdle(fw.dev);
   vkfw_buffer_destroy(&fw, &pattern);
   if (surface != VK_NULL_HANDLE)
      fw.wsi.vkDestroySurfaceKHR(fw.instance, surface, NULL);

out:
   vkfw_finish(&fw);

   /* THE CHECK THAT WOULD HAVE CAUGHT THE LEAK, and it runs after the
    * device is gone because that is when the driver says so.
    *
    * Some of what a driver reports has no Vulkan representation at all.
    * horizon_gpu refuses to destroy a memory object while a GPU mapping
    * of it is alive, and refuses to destroy the device while any memory
    * survives; both are mesa_loge lines on stderr, which main() has
    * dup2'd into this log. On the first hardware run every swapchain
    * image leaked — fourteen of them — and vkDestroyDevice then refused
    * outright, with every check in this file still passing. Reading the
    * log back is how that becomes a failure instead of a paragraph
    * somebody has to notice.
    *
    * The message deliberately does not contain the string it looks for,
    * so a failure cannot make a later scan match itself. */
   bool could_not_destroy = false;
   if (t_log_scan(t, "destroy refused", &could_not_destroy)) {
      t_check(t, !could_not_destroy,
              "the driver tore down every object it created: nothing in "
              "this log says it could not");
   } else {
      /* NOT a pass. The scan is the only way this test can see a
       * teardown failure at all, and one that could not run has
       * verified nothing. */
      t_check(t, false,
              "the log could not be read back, so whether the driver tore "
              "everything down was NOT checked");
   }
   return rv;
}
