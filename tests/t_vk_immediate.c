/*
 * t_vk_immediate — VK_PRESENT_MODE_IMMEDIATE_KHR on the Horizon WSI, and
 * whether this platform can honestly claim it.
 *
 * THE QUESTION. The backend advertises two present modes and maps them
 * onto the one knob VI gives a producer, the per-queue swap interval
 * (`BqBufferInput::swapInterval`, or `nwindowSetSwapInterval` on the
 * copy fallback): FIFO is interval 1, IMMEDIATE is interval 0. libnx
 * documents interval 0 as honoured only with three or more buffers
 * registered, "otherwise the system enforces a minimum of 1" — so a
 * two-buffer swapchain asking for IMMEDIATE would get FIFO behaviour and
 * a success code, which is FIFO wearing another mode's name. The backend
 * answers that by taking a third image; this file is where that answer
 * is checked against a console instead of believed.
 *
 * WHAT COUNTS AS IMMEDIATE HERE, STATED BEFORE IT IS MEASURED. The
 * specification says the presentation engine "does not wait for a
 * vertical blanking period to update the current image, meaning this
 * mode may result in visible tearing", and that no internal queuing of
 * presentation requests is needed. Two halves, and only one of them is
 * observable from a process on this platform:
 *
 *   - Tearing is not. VI composites and flips at the vertical blank;
 *     nothing a producer can do puts its buffer on a scanline mid-frame.
 *     The specification says "may", not "must", so its absence is not a
 *     violation — but it is a real difference from a desktop IMMEDIATE
 *     and it is documented rather than glossed over.
 *
 *   - Not being throttled to the refresh IS observable, and it is the
 *     whole of what this file asserts: an application presenting as fast
 *     as it can under IMMEDIATE must not be paced at one frame per
 *     vertical blank, while the same loop under FIFO must be.
 *
 * So the measurement is a comparison, taken in one run on one console
 * against one swapchain shape, and the threshold is stated as a ratio
 * rather than a number of microseconds — the point is not that a frame
 * takes 8 ms, it is that FIFO cannot go faster than a refresh and this
 * does.
 *
 * THE DISCRIMINATOR THAT IS NOT A MEAN. A mean interval can be dragged
 * under a refresh by a burst, so the check that carries the argument is
 * the count of intervals shorter than *half* a refresh. FIFO cannot
 * produce one at all: the compositor holds each buffer for a whole
 * vertical blank, so the producer's next buffer cannot arrive early.
 * IMMEDIATE produces them constantly, because the queue hands back more
 * than one buffer per refresh and the producer takes them as they come.
 * The mean is reported beside it as throughput, which is what an
 * application actually feels.
 *
 * WHAT THIS FILE WILL DO IF THE ANSWER IS NO. Nothing is forced. Every
 * threshold is checked against numbers printed in the same log, and a
 * run in which IMMEDIATE paces like FIFO fails the checks and says so —
 * the conclusion is then that the mode must stop being advertised, and
 * that conclusion belongs in the driver, not in a relaxed threshold
 * here.
 *
 * The sections:
 *
 *   A. What the surface offers and what a swapchain does with it. Both
 *      modes advertised; IMMEDIATE asked for with 2, 3 and 4 images and
 *      the resulting image count checked against the promise; FIFO with
 *      2 images beside it, so the image-count bump is shown to be
 *      IMMEDIATE's and not a floor everything gets; and the swap
 *      interval the driver reports for each, read out of its own
 *      decision message rather than assumed.
 *
 *   B. Sustained presentation, and the comparison. FIFO, then
 *      IMMEDIATE, then FIFO again — the second FIFO run is what makes
 *      the middle one a measurement rather than a coincidence of
 *      whatever the console was doing at the time.
 *
 *   C. The two-image request, which is the case libnx's note is about.
 *      A swapchain asked for with minImageCount 2 and IMMEDIATE, run to
 *      the same thresholds as B; and a genuine two-image FIFO swapchain
 *      beside it.
 *
 *   D. Holding more than one image at once, and giving them all back.
 *      Acquire without presenting up to one short of the image count,
 *      then present them in order. A slot the driver retains shows up
 *      here as an acquire that cannot be satisfied.
 *
 *   E. Recreation, alternating the mode. Six generations, each
 *      presenting its own frames, each checked for the interval its own
 *      mode implies — which is what catches a swap interval that stuck
 *      to the window instead of following the swapchain.
 *
 *   F. The copy fallback, forced, in both modes. It reaches the
 *      compositor through nwindowSetSwapInterval rather than through
 *      the queue input, so a defect in one path is invisible in the
 *      other. IT IS NOT JUDGED AGAINST THE THRESHOLDS ABOVE: run 24
 *      measured this path at the refresh in *both* modes, 16732 us
 *      against 16626 us, because a present here costs most of a refresh
 *      on its own and interval 0 has nothing surplus to drop.
 *      That is a property of the path, not a swap interval that went
 *      missing, and the section asserts the measured state so a console
 *      that behaves differently is reported rather than passed over.
 *
 *   G. Nothing retained. A last swapchain created after all of the
 *      above must still get the zero-copy path and present its frames,
 *      and the driver must not have said anything about a slot or a
 *      registration it could not give back.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include "common/vkfw.h"

const char *const test_name = "t_vk_immediate";

/* This test owns the display: no console, and the SD-card log is the
 * whole record (testfw.h). */
const bool test_uses_display = true;

/* The display's nominal refresh. Nothing is asserted to be exactly
 * this; it is the unit the thresholds are expressed in, and the same
 * value wsi_horizon.c and t_vk_swapchain.c use. */
#define IMM_REFRESH_NS   UINT64_C(16666666)

/* Four seconds of FIFO, about two of IMMEDIATE. Long enough that the
 * mean is not one lucky burst, short enough that six measured runs plus
 * everything else stays inside a minute. */
#define IMM_FRAMES        240u
/* Where what is measured is behaviour rather than pacing. */
#define IMM_SHORT_FRAMES   60u
/* Per generation in section E: enough intervals for the mode to be
 * visible in them, short enough that six generations are cheap. */
#define IMM_GEN_FRAMES     60u
#define IMM_GENERATIONS     6u

#define IMM_MAX_IMAGES      4u
#define IMM_WAIT_NS      UINT64_C(2000000000)

/* THE THRESHOLDS, AS FRACTIONS, ALL IN ONE PLACE.
 *
 * IMM_FAST_NUM/DEN: how much of a refresh an IMMEDIATE swapchain's mean
 * interval may be, and how much of the FIFO run's mean. Three quarters
 * is well clear of the measurement's own noise and nowhere near the
 * ratio the mechanism predicts (a queue that hands back more than one
 * buffer per refresh gives a half or less), so it separates "faster
 * than a refresh" from "a refresh with jitter" without being tuned to
 * an expected number.
 *
 * IMM_BURST_NUM/DEN: what fraction of an IMMEDIATE run's intervals must
 * be shorter than half a refresh. A quarter, because the pattern this
 * produces is a run of short intervals ended by one long wait for the
 * compositor, and with three buffers that is two short in every three.
 * A quarter passes that comfortably and is still impossible for FIFO.
 */
#define IMM_FAST_NUM   3u
#define IMM_FAST_DEN   4u
#define IMM_BURST_NUM  1u
#define IMM_BURST_DEN  4u

/* How many of a FIFO run's intervals must land within 10% of a refresh
 * for the run to be usable as the reference the comparison is made
 * against. Not a check on the driver so much as on the measurement: if
 * FIFO is not paced at a refresh in this run, nothing below it means
 * anything, and the log should say that rather than report a ratio
 * against a number that was never the refresh. */
#define IMM_FIFO_PACED_PCT 90u

/* ------------------------------------------------------------------ */
/* One swapchain, and the per-image resources a frame needs.           */
/* ------------------------------------------------------------------ */

typedef struct imm_frame_res {
   VkCommandBuffer cb;
   VkSemaphore render_done;
   VkFence in_flight;
   bool in_flight_valid;
} imm_frame_res;

typedef struct imm_swapchain {
   VkSwapchainKHR handle;
   uint32_t image_count;
   VkImage images[IMM_MAX_IMAGES];
   imm_frame_res res[IMM_MAX_IMAGES];

   /* One per image, because section D holds several images at once and
    * each acquire needs a fence of its own. The ordinary frame loop
    * uses [0] and nothing else. */
   VkFence acquire_fences[IMM_MAX_IMAGES];

   VkExtent2D extent;
   VkPresentModeKHR mode;
   uint32_t requested_images;

   /* What the driver said about this swapchain at creation, read out of
    * its debug-utils messages: which present path it took, and which
    * swap interval it registered. `interval` is -1 when the decision
    * message did not name one, which is itself a finding. */
   bool zero_copy;
   bool decision_seen;
   int interval;
} imm_swapchain;

/* One measured run of frames. */
typedef struct imm_stats {
   const char *what;
   VkPresentModeKHR mode;
   uint32_t image_count;

   uint32_t frames;
   uint32_t intervals;
   uint64_t total_ns;
   uint64_t min_ns;
   uint64_t max_ns;
   /* The FIFO signature: an interval within 10% of one refresh. */
   uint32_t within_10pct;
   /* The IMMEDIATE signature: an interval shorter than half a refresh,
    * which a mode that waits for a vertical blank cannot produce.
    *
    * TWO COUNTS, AND THE SECOND IS THE ONE THE VERDICTS USE. A
    * swapchain begins with every image free, so the first `image_count`
    * frames go out without anything to wait for — under FIFO too, and
    * legitimately: FIFO throttles a producer at a *full* queue, and the
    * queue is not full yet. Measured on hardware (run 22): exactly
    * `image_count - 1` early intervals at the head of every FIFO run,
    * 2 of 239 with three images and 1 of 239 with two, and none
    * afterwards. `under_half` counts all of them; `under_half_steady`
    * counts only those from frame `image_count` onwards, which is the
    * statement that actually separates the two modes. */
   uint32_t under_half;
   uint32_t under_half_steady;
   uint32_t steady_intervals;
   uint32_t under_refresh;

   uint64_t acquire_total_ns;
   uint64_t acquire_max_ns;
   uint64_t present_total_ns;
   uint64_t present_max_ns;
   /* The floor of what vkQueuePresentKHR costs on this path, which is
    * the number section F's argument rests on: a present that cannot go
    * below most of a refresh cannot build the queue depth interval 0
    * exists to drain. */
   uint64_t present_min_ns;

   uint32_t suboptimal;

   /* Which image indices the acquire ever handed over, and the longest
    * run of one index repeating. A driver that had stopped rotating its
    * slots would show up in both. */
   uint32_t used_mask;
   uint32_t repeat_max;

   bool failed;
   uint32_t fail_frame;
   VkResult fail_result;
   const char *fail_what;
} imm_stats;

static void imm_stats_init(imm_stats *s, const char *what,
                           const imm_swapchain *sc)
{
   memset(s, 0, sizeof(*s));
   s->what = what;
   s->mode = sc->mode;
   s->image_count = sc->image_count;
   s->min_ns = UINT64_MAX;
   s->present_min_ns = UINT64_MAX;
}

static void imm_fail(imm_stats *s, uint32_t frame, VkResult r,
                     const char *what)
{
   if (s->failed)
      return;
   s->failed = true;
   s->fail_frame = frame;
   s->fail_result = r;
   s->fail_what = what;
}

static uint64_t imm_mean_ns(const imm_stats *s)
{
   return s->intervals != 0 ? s->total_ns / s->intervals : 0;
}

/* ------------------------------------------------------------------ */

/* Which path the driver took, and which interval it registered, asked
 * of the driver's own decision message rather than assumed from what
 * was requested. The caller forgets the message ring first, so what is
 * found here belongs to the swapchain just created and not to an
 * earlier one. */
static bool imm_path_is_copy(vkfw *fw)
{
   const char *msg = NULL;
   return vkfw_saw_message(fw, "copy fallback: the rendered image", &msg);
}

static int imm_reported_interval(vkfw *fw, const char **line_out)
{
   if (vkfw_saw_message(fw, "swap interval 0", line_out))
      return 0;
   if (vkfw_saw_message(fw, "swap interval 1", line_out))
      return 1;
   return -1;
}

static const char *imm_mode_name(VkPresentModeKHR mode)
{
   switch (mode) {
   case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
   case VK_PRESENT_MODE_FIFO_KHR:      return "FIFO";
   case VK_PRESENT_MODE_MAILBOX_KHR:   return "MAILBOX";
   case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
   default:                            return "other";
   }
}

/* The swap interval each mode is supposed to reach the compositor with.
 * One place, because three sections check it. */
static int imm_expected_interval(VkPresentModeKHR mode)
{
   return mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? 0 : 1;
}

/* ------------------------------------------------------------------ */

static void imm_destroy(vkfw *fw, imm_swapchain *sc)
{
   for (uint32_t i = 0; i < sc->image_count; i++) {
      if (sc->res[i].render_done != VK_NULL_HANDLE)
         fw->wsi.vkDestroySemaphore(fw->dev, sc->res[i].render_done, NULL);
      if (sc->res[i].in_flight != VK_NULL_HANDLE)
         fw->vk.vkDestroyFence(fw->dev, sc->res[i].in_flight, NULL);
      if (sc->res[i].cb != VK_NULL_HANDLE)
         fw->vk.vkFreeCommandBuffers(fw->dev, fw->pool, 1, &sc->res[i].cb);
   }
   for (uint32_t i = 0; i < IMM_MAX_IMAGES; i++) {
      if (sc->acquire_fences[i] != VK_NULL_HANDLE)
         fw->vk.vkDestroyFence(fw->dev, sc->acquire_fences[i], NULL);
   }
   if (sc->handle != VK_NULL_HANDLE)
      fw->wsi.vkDestroySwapchainKHR(fw->dev, sc->handle, NULL);

   memset(sc, 0, sizeof(*sc));
}

/* Everything the device could still be using is finished with before a
 * swapchain is destroyed: the in-flight fence says the render retired
 * and says nothing about a vkQueuePresentKHR still waiting on
 * render_done[i], which vkDestroySemaphore requires to have completed.
 */
static VkResult imm_quiesce(vkfw *fw)
{
   const VkResult r = fw->vk.vkQueueWaitIdle(fw->queue);
   if (r != VK_SUCCESS)
      return r;
   return fw->vk.vkDeviceWaitIdle(fw->dev);
}

static bool imm_create(vkfw *fw, VkSurfaceKHR surface, VkPresentModeKHR mode,
                       uint32_t want_images, VkExtent2D extent,
                       VkSwapchainKHR old, const char *what,
                       imm_swapchain *sc)
{
   test_ctx *t = fw->t;
   memset(sc, 0, sizeof(*sc));
   sc->extent = extent;
   sc->mode = mode;
   sc->requested_images = want_images;

   /* So the decision read back below is this swapchain's. */
   vkfw_forget_messages(fw);

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
      .presentMode = mode,
      .clipped = VK_TRUE,
      .oldSwapchain = old,
   };

   VkResult r = fw->wsi.vkCreateSwapchainKHR(fw->dev, &ci, NULL, &sc->handle);
   if (!t_check(t, r == VK_SUCCESS,
                "%s: vkCreateSwapchainKHR(%s, minImageCount %" PRIu32
                ", %" PRIu32 "x%" PRIu32 ") -> %s", what,
                imm_mode_name(mode), want_images, extent.width, extent.height,
                vkfw_result_str(r)))
      return false;

   const char *line = NULL;
   sc->interval = imm_reported_interval(fw, &line);
   sc->decision_seen = line != NULL;
   sc->zero_copy = !imm_path_is_copy(fw);

   uint32_t count = 0;
   r = fw->wsi.vkGetSwapchainImagesKHR(fw->dev, sc->handle, &count, NULL);
   if (!t_check(t, r == VK_SUCCESS && count >= 2 && count <= IMM_MAX_IMAGES,
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
   for (uint32_t i = 0; i < IMM_MAX_IMAGES; i++) {
      r = fw->vk.vkCreateFence(fw->dev, &fci, NULL, &sc->acquire_fences[i]);
      if (!t_check(t, r == VK_SUCCESS, "%s: acquire fence %" PRIu32 " -> %s",
                   what, i, vkfw_result_str(r)))
         goto fail;
   }

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

   t_note(t, "%s: %s, %" PRIu32 " image(s) for a request of %" PRIu32
             ", %s, swap interval %s", what, imm_mode_name(mode),
          sc->image_count, want_images,
          sc->zero_copy ? "zero-copy" : "copy fallback",
          sc->interval == 0 ? "0" : (sc->interval == 1 ? "1" : "unreported"));
   return true;

fail:
   imm_destroy(fw, sc);
   return false;
}

/* A visible colour that changes every frame, so the screen is doing
 * something an operator can see. The two modes are given different hue
 * sweeps on purpose: at interval 0 the sweep visibly runs faster, which
 * is the measurement made watchable. */
static void imm_frame_colour(VkPresentModeKHR mode, uint32_t frame,
                             VkClearColorValue *out)
{
   const uint32_t k = mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? 3u : 1u;
   out->float32[0] = (float)((frame * k) & 0xffu) / 255.0f;
   out->float32[1] = (float)((frame * 5u + 85u) & 0xffu) / 255.0f;
   out->float32[2] = (float)((frame * 7u + 170u) & 0xffu) / 255.0f;
   out->float32[3] = 1.0f;
}

/* Clears the acquired image and leaves it in PRESENT_SRC. No pipeline
 * and no shader: the subject is the present, and anything else would be
 * another thing that could fail — and another thing that could make the
 * loop slower than the compositor, which is exactly what must not
 * happen in a test about pacing. */
static VkResult imm_record_and_submit(vkfw *fw, imm_swapchain *sc,
                                      uint32_t index, uint32_t frame)
{
   imm_frame_res *res = &sc->res[index];
   VkResult r;

   if (res->in_flight_valid) {
      r = fw->vk.vkWaitForFences(fw->dev, 1, &res->in_flight, VK_TRUE,
                                 IMM_WAIT_NS);
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
   imm_frame_colour(sc->mode, frame, &colour);
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

/* `frames` frames as fast as the loop can issue them, with no sleep and
 * no pacing of its own — anything this measures that is not the
 * application's own work is the presentation engine's. */
static void imm_run(vkfw *fw, imm_swapchain *sc, uint32_t frames,
                    imm_stats *st)
{
   u64 prev_tick = 0;
   bool have_prev = false;
   uint32_t prev_index = UINT32_MAX;
   uint32_t repeat = 0;

   for (uint32_t frame = 0; frame < frames; frame++) {
      const u64 acq_start = armGetSystemTick();

      uint32_t index = 0;
      VkResult r = fw->wsi.vkAcquireNextImageKHR(fw->dev, sc->handle,
                                                 IMM_WAIT_NS, VK_NULL_HANDLE,
                                                 sc->acquire_fences[0],
                                                 &index);
      if (r == VK_SUBOPTIMAL_KHR) {
         st->suboptimal++;
      } else if (r != VK_SUCCESS) {
         imm_fail(st, frame, r, "vkAcquireNextImageKHR");
         return;
      }

      /* The acquire took a reference to the fence on both success
       * codes, so every path out of this iteration has to have waited
       * on it and reset it — including the index check below, which is
       * why that comes after. */
      r = fw->vk.vkWaitForFences(fw->dev, 1, &sc->acquire_fences[0], VK_TRUE,
                                 IMM_WAIT_NS);
      if (r != VK_SUCCESS) {
         imm_fail(st, frame, r, "waiting for the acquire fence");
         return;
      }
      r = fw->vk.vkResetFences(fw->dev, 1, &sc->acquire_fences[0]);
      if (r != VK_SUCCESS) {
         imm_fail(st, frame, r, "resetting the acquire fence");
         return;
      }

      const uint64_t acq_ns = armTicksToNs(armGetSystemTick() - acq_start);
      st->acquire_total_ns += acq_ns;
      if (acq_ns > st->acquire_max_ns)
         st->acquire_max_ns = acq_ns;

      if (index >= sc->image_count) {
         imm_fail(st, frame, VK_ERROR_UNKNOWN, "an image index out of range");
         return;
      }
      st->used_mask |= 1u << index;
      if (index == prev_index) {
         repeat++;
      } else {
         repeat = 1;
         prev_index = index;
      }
      if (repeat > st->repeat_max)
         st->repeat_max = repeat;

      r = imm_record_and_submit(fw, sc, index, frame);
      if (r != VK_SUCCESS) {
         imm_fail(st, frame, r, "recording and submitting the frame");
         return;
      }

      const VkPresentInfoKHR pi = {
         .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
         .waitSemaphoreCount = 1,
         .pWaitSemaphores = &sc->res[index].render_done,
         .swapchainCount = 1,
         .pSwapchains = &sc->handle,
         .pImageIndices = &index,
      };
      const u64 pres_start = armGetSystemTick();
      r = fw->wsi.vkQueuePresentKHR(fw->queue, &pi);
      const uint64_t pres_ns = armTicksToNs(armGetSystemTick() - pres_start);
      if (r == VK_SUBOPTIMAL_KHR) {
         st->suboptimal++;
      } else if (r != VK_SUCCESS) {
         imm_fail(st, frame, r, "vkQueuePresentKHR");
         return;
      }
      st->present_total_ns += pres_ns;
      if (pres_ns > st->present_max_ns)
         st->present_max_ns = pres_ns;
      if (pres_ns < st->present_min_ns)
         st->present_min_ns = pres_ns;
      st->frames++;

      const u64 now = armGetSystemTick();
      if (have_prev) {
         const uint64_t dt = armTicksToNs(now - prev_tick);
         st->total_ns += dt;
         if (dt < st->min_ns)
            st->min_ns = dt;
         if (dt > st->max_ns)
            st->max_ns = dt;
         if (dt >= IMM_REFRESH_NS - IMM_REFRESH_NS / 10 &&
             dt <= IMM_REFRESH_NS + IMM_REFRESH_NS / 10)
            st->within_10pct++;
         if (dt < IMM_REFRESH_NS / 2)
            st->under_half++;
         if (dt < IMM_REFRESH_NS)
            st->under_refresh++;
         st->intervals++;
         /* Past the point where every image was free to begin with. */
         if (frame >= sc->image_count) {
            st->steady_intervals++;
            if (dt < IMM_REFRESH_NS / 2)
               st->under_half_steady++;
         }
      }
      prev_tick = now;
      have_prev = true;
   }
}

/* The numbers, always; the "it ran at all" check, always. The pacing
 * verdict is a separate call, because a run that did not complete has
 * no pacing worth judging. */
static bool imm_report(test_ctx *t, const imm_stats *s, uint32_t expected)
{
   bool ok = t_check(t, !s->failed && s->frames == expected,
                     "%s: %" PRIu32 " of %" PRIu32 " frames presented%s",
                     s->what, s->frames, expected,
                     s->failed ? " (see the note below)" : "");
   if (s->failed) {
      t_note(t, "%s: %s at frame %" PRIu32 " -> %s", s->what, s->fail_what,
             s->fail_frame, vkfw_result_str(s->fail_result));
   }

   t_note(t, "%s [%s, %" PRIu32 " images]: mean %" PRIu64 " us "
             "(min %" PRIu64 ", max %" PRIu64 "), %" PRIu32 " of %" PRIu32
             " intervals under half a refresh (%" PRIu32 " of %" PRIu32
             " once the queue is full), %" PRIu32 " under a refresh, "
             "%" PRIu32 " within 10%% of one; acquire mean %" PRIu64
             " us (max %" PRIu64 "), present mean %" PRIu64 " us (min %"
             PRIu64 ", max %" PRIu64 "); %" PRIu32 " SUBOPTIMAL",
          s->what, imm_mode_name(s->mode), s->image_count,
          imm_mean_ns(s) / 1000,
          s->min_ns == UINT64_MAX ? 0 : s->min_ns / 1000, s->max_ns / 1000,
          s->under_half, s->intervals, s->under_half_steady,
          s->steady_intervals, s->under_refresh, s->within_10pct,
          s->frames != 0 ? s->acquire_total_ns / s->frames / 1000 : 0,
          s->acquire_max_ns / 1000,
          s->frames != 0 ? s->present_total_ns / s->frames / 1000 : 0,
          s->present_min_ns == UINT64_MAX ? 0 : s->present_min_ns / 1000,
          s->present_max_ns / 1000, s->suboptimal);

   /* Every image the swapchain has must have been used. A driver that
    * had retained one would present the same frames through fewer
    * slots, and nothing else in this run would say so. */
   const uint32_t all = (1u << s->image_count) - 1u;
   ok = t_check(t, s->used_mask == all,
                "%s: every one of the %" PRIu32 " images was acquired at "
                "least once (mask 0x%x, longest run of one index %" PRIu32
                ")", s->what, s->image_count, s->used_mask, s->repeat_max)
        && ok;

   return ok;
}

/* THE VERDICT ON A FIFO RUN, which is also the check that the reference
 * this file measures against is the thing it is assumed to be. */
static bool imm_report_fifo(test_ctx *t, const imm_stats *s)
{
   if (s->intervals == 0)
      return t_check(t, false, "%s: no intervals to judge", s->what);

   const bool paced = s->within_10pct * 100u >= s->intervals *
                                                IMM_FIFO_PACED_PCT;
   bool ok = t_check(t, paced,
                     "%s: FIFO is paced at the refresh — %" PRIu32 " of %"
                     PRIu32 " intervals within 10%% of %" PRIu64 " us "
                     "(>= %u%% needed)", s->what, s->within_10pct,
                     s->intervals, IMM_REFRESH_NS / 1000, IMM_FIFO_PACED_PCT);

   /* The other half of "FIFO is FIFO": once the queue is full it never
    * gets ahead again. One early frame there would mean the compositor
    * is not holding each buffer for a vertical blank, and then the
    * section-B comparison is measuring something other than what it
    * claims. Before the queue is full, running ahead is what FIFO is
    * supposed to do, so the head of the run is excluded and the count
    * that was excluded is printed rather than dropped. */
   ok = t_check(t, s->under_half_steady == 0,
                "%s: once the queue was full FIFO produced no interval "
                "shorter than half a refresh (%" PRIu32 " of %" PRIu32
                "; %" PRIu32 " of the first %" PRIu32 " frames went out "
                "early, which is the queue filling)", s->what,
                s->under_half_steady, s->steady_intervals,
                s->under_half - s->under_half_steady, s->image_count) && ok;
   return ok;
}

/* THE VERDICT ON AN IMMEDIATE RUN. `fifo_mean_ns` is the reference
 * measured in the same session on the same console; pass 0 when there
 * is none, and only the absolute threshold is applied.
 *
 * Both thresholds have to hold. The mean is throughput — what an
 * application gets — and the count of short intervals is the mechanism:
 * a mode that waits for a vertical blank cannot produce a frame in less
 * than half of one, however good its average looks.
 */
static bool imm_report_immediate(test_ctx *t, const imm_stats *s,
                                 uint64_t fifo_mean_ns)
{
   if (s->intervals == 0)
      return t_check(t, false, "%s: no intervals to judge", s->what);

   const uint64_t mean = imm_mean_ns(s);

   bool ok = t_check(t, mean * IMM_FAST_DEN <= IMM_REFRESH_NS * IMM_FAST_NUM,
                     "%s: IMMEDIATE is not paced at the refresh — mean %"
                     PRIu64 " us, must be at most %" PRIu64 " us (%u/%u of a "
                     "refresh)", s->what, mean / 1000,
                     IMM_REFRESH_NS * IMM_FAST_NUM / IMM_FAST_DEN / 1000,
                     IMM_FAST_NUM, IMM_FAST_DEN);

   if (fifo_mean_ns != 0) {
      ok = t_check(t, mean * IMM_FAST_DEN <= fifo_mean_ns * IMM_FAST_NUM,
                   "%s: IMMEDIATE presents the same frames faster than FIFO "
                   "did in this run — %" PRIu64 " us against %" PRIu64
                   " us, must be at most %u/%u of it", s->what, mean / 1000,
                   fifo_mean_ns / 1000, IMM_FAST_NUM, IMM_FAST_DEN) && ok;
   }

   /* Counted from the point where the queue is full, so this cannot be
    * satisfied by the head of the run — which FIFO also produces. */
   ok = t_check(t, s->under_half_steady * IMM_BURST_DEN >=
                   s->steady_intervals * IMM_BURST_NUM,
                "%s: frames keep arriving faster than a vertical blank can "
                "carry them, with the queue full — %" PRIu32 " of %" PRIu32
                " intervals under half a refresh (>= %u/%u needed)",
                s->what, s->under_half_steady, s->steady_intervals,
                IMM_BURST_NUM, IMM_BURST_DEN) && ok;
   return ok;
}

/* One measured run, from creation to teardown, with the pacing verdict
 * the mode implies. Returns the mean interval, or 0 if the run did not
 * happen; `stats_out` receives the whole record when non-NULL. */
static uint64_t imm_measure(vkfw *fw, VkSurfaceKHR surface,
                            VkPresentModeKHR mode, uint32_t want_images,
                            VkExtent2D extent, uint32_t frames,
                            const char *what, uint64_t fifo_mean_ns,
                            bool judge, imm_stats *stats_out)
{
   test_ctx *t = fw->t;
   imm_swapchain sc;

   if (!imm_create(fw, surface, mode, want_images, extent, VK_NULL_HANDLE,
                   what, &sc))
      return 0;

   /* THE INTERVAL THE DRIVER REGISTERED, CHECKED AT EVERY CREATION.
    * This is the one that catches "IMMEDIATE silently became FIFO"
    * without any timing at all, and it is checked on the copy fallback
    * too, where the interval reaches the compositor by another call
    * entirely. */
   t_check(t, sc.interval == imm_expected_interval(mode),
           "%s: the driver registered swap interval %d, which is what %s "
           "means here", what, sc.interval, imm_mode_name(mode));

   imm_stats st;
   imm_stats_init(&st, what, &sc);
   imm_run(fw, &sc, frames, &st);

   const bool ran = imm_report(t, &st, frames);
   if (judge && ran) {
      if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR)
         imm_report_immediate(t, &st, fifo_mean_ns);
      else
         imm_report_fifo(t, &st);
   }

   const VkResult qr = imm_quiesce(fw);
   t_check(t, qr == VK_SUCCESS, "%s: the queue and device went idle -> %s",
           what, vkfw_result_str(qr));
   imm_destroy(fw, &sc);

   if (stats_out != NULL)
      *stats_out = st;
   return ran ? imm_mean_ns(&st) : 0;
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

   /* Section F sets this and clears it again; everything else must
    * measure the path the hardware chooses. */
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

   VkSurfaceCapabilitiesKHR caps;
   memset(&caps, 0, sizeof(caps));
   r = fw.wsi.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(fw.pdev, surface,
                                                        &caps);
   if (!t_check(t, r == VK_SUCCESS, "surface capabilities -> %s",
                vkfw_result_str(r))) {
      rv = 1;
      goto out_surface;
   }
   const VkExtent2D extent = caps.currentExtent;
   t_note(t, "the surface is %" PRIu32 "x%" PRIu32 ", image count %" PRIu32
             "..%" PRIu32 "; operation mode %d (0 handheld, 1 docked)",
          extent.width, extent.height, caps.minImageCount, caps.maxImageCount,
          (int)appletGetOperationMode());
   if (!t_check(t, extent.width > 0 && extent.height > 0 &&
                extent.width != UINT32_MAX,
                "the surface's currentExtent is a real size")) {
      rv = 1;
      goto out_surface;
   }

   /* ---------------------------------------------------------------- */
   /* A — what is offered, and what a swapchain does with it.          */
   /* ---------------------------------------------------------------- */

   uint32_t mode_count = 0;
   r = fw.wsi.vkGetPhysicalDeviceSurfacePresentModesKHR(fw.pdev, surface,
                                                        &mode_count, NULL);
   t_check(t, r == VK_SUCCESS && mode_count > 0,
           "A: the surface offers %" PRIu32 " present mode(s) -> %s",
           mode_count, vkfw_result_str(r));

   VkPresentModeKHR modes[8];
   bool has_immediate = false, has_fifo = false;
   if (r == VK_SUCCESS && mode_count > 0) {
      uint32_t got = mode_count < 8 ? mode_count : 8;
      r = fw.wsi.vkGetPhysicalDeviceSurfacePresentModesKHR(fw.pdev, surface,
                                                           &got, modes);
      t_check(t, r == VK_SUCCESS || r == VK_INCOMPLETE,
              "A: the present-mode list -> %s", vkfw_result_str(r));
      for (uint32_t i = 0; i < got; i++) {
         t_note(t, "A: present mode %" PRIu32 ": %s (%d)", i,
                imm_mode_name(modes[i]), (int)modes[i]);
         if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR)
            has_immediate = true;
         if (modes[i] == VK_PRESENT_MODE_FIFO_KHR)
            has_fifo = true;
      }
   }

   /* FIFO is required of every implementation; IMMEDIATE is what this
    * file is about. If the driver has stopped advertising it, the rest
    * of this test has nothing to measure and says so rather than
    * failing check after check. */
   t_check(t, has_fifo, "A: VK_PRESENT_MODE_FIFO_KHR is offered");
   if (!t_check(t, has_immediate,
                "A: VK_PRESENT_MODE_IMMEDIATE_KHR is offered")) {
      t_note(t, "A: IMMEDIATE is not advertised, so there is nothing here "
                "to measure. That is a legitimate state for this backend "
                "to be in, and this test then has no verdict to give "
                "beyond this line");
      goto out_surface;
   }

   /* The image counts, which is where libnx's three-buffer rule turns
    * into a promise this backend has to keep. */
   static const struct {
      VkPresentModeKHR mode;
      uint32_t want;
      uint32_t least;      /* the image count the result must reach */
      bool exact;          /* and must not exceed                   */
      const char *what;
   } creations[] = {
      /* The case the rule is about: two images asked for under
       * IMMEDIATE must not produce a two-image swapchain, because a
       * two-buffer window gets interval 1 whatever it asks for. */
      { VK_PRESENT_MODE_IMMEDIATE_KHR, 2, 3, false, "A: IMMEDIATE, 2 asked" },
      { VK_PRESENT_MODE_IMMEDIATE_KHR, 3, 3, true,  "A: IMMEDIATE, 3 asked" },
      { VK_PRESENT_MODE_IMMEDIATE_KHR, 4, 4, true,  "A: IMMEDIATE, 4 asked" },
      /* Beside them, so the bump above is shown to belong to IMMEDIATE
       * and not to be a floor every swapchain gets. */
      { VK_PRESENT_MODE_FIFO_KHR,      2, 2, true,  "A: FIFO, 2 asked" },
   };

   for (uint32_t i = 0; i < sizeof(creations) / sizeof(creations[0]); i++) {
      imm_swapchain sc;
      if (!imm_create(&fw, surface, creations[i].mode, creations[i].want,
                      extent, VK_NULL_HANDLE, creations[i].what, &sc))
         continue;

      t_check(t, sc.image_count >= creations[i].least &&
              (!creations[i].exact || sc.image_count == creations[i].least),
              "%s: got %" PRIu32 " image(s), needed %s%" PRIu32,
              creations[i].what, sc.image_count,
              creations[i].exact ? "exactly " : "at least ",
              creations[i].least);

      t_check(t, sc.decision_seen,
              "%s: the driver reported which swap interval it registered",
              creations[i].what);
      t_check(t, sc.interval == imm_expected_interval(creations[i].mode),
              "%s: swap interval %d, which is what %s means here",
              creations[i].what, sc.interval,
              imm_mode_name(creations[i].mode));

      /* An IMMEDIATE swapchain must have enough buffers for the
       * compositor to honour interval 0 at all. Stated as its own check
       * because it is the platform rule the image count exists to
       * satisfy, and reading it beside the count is what makes the
       * count mean something. */
      if (creations[i].mode == VK_PRESENT_MODE_IMMEDIATE_KHR) {
         t_check(t, sc.image_count >= 3,
                 "%s: three or more buffers are registered, which is what "
                 "makes interval 0 reach the compositor", creations[i].what);
      }

      const VkResult qr = imm_quiesce(&fw);
      t_check(t, qr == VK_SUCCESS, "%s: idle before teardown -> %s",
              creations[i].what, vkfw_result_str(qr));
      imm_destroy(&fw, &sc);
   }

   /* ---------------------------------------------------------------- */
   /* B — sustained presentation, and the comparison.                  */
   /* ---------------------------------------------------------------- */

   imm_stats fifo1, imm1, fifo2;
   const uint64_t m_fifo1 =
      imm_measure(&fw, surface, VK_PRESENT_MODE_FIFO_KHR, 3, extent,
                  IMM_FRAMES, "B: FIFO, 3 images (before)", 0, true, &fifo1);
   const uint64_t m_imm1 =
      imm_measure(&fw, surface, VK_PRESENT_MODE_IMMEDIATE_KHR, 3, extent,
                  IMM_FRAMES, "B: IMMEDIATE, 3 images", m_fifo1, true, &imm1);
   const uint64_t m_fifo2 =
      imm_measure(&fw, surface, VK_PRESENT_MODE_FIFO_KHR, 3, extent,
                  IMM_FRAMES, "B: FIFO, 3 images (after)", 0, true, &fifo2);

   if (m_fifo1 != 0 && m_fifo2 != 0) {
      /* The reference is stable across the run, so the middle
       * measurement is about the mode and not about the console. */
      const uint64_t lo = m_fifo1 < m_fifo2 ? m_fifo1 : m_fifo2;
      const uint64_t hi = m_fifo1 < m_fifo2 ? m_fifo2 : m_fifo1;
      t_check(t, hi * 9 <= lo * 10,
              "B: the two FIFO runs agree within 10%% (%" PRIu64 " us and %"
              PRIu64 " us), so the IMMEDIATE run between them is comparable "
              "with both", m_fifo1 / 1000, m_fifo2 / 1000);
   }
   if (m_imm1 != 0 && m_fifo2 != 0) {
      t_check(t, m_imm1 * IMM_FAST_DEN <= m_fifo2 * IMM_FAST_NUM,
              "B: IMMEDIATE also beats the FIFO run taken after it (%"
              PRIu64 " us against %" PRIu64 " us)", m_imm1 / 1000,
              m_fifo2 / 1000);
   }
   if (m_imm1 != 0 && m_fifo1 != 0) {
      t_note(t, "B: THE NUMBER — %" PRIu32 " frames at interval 1 took a "
                "mean of %" PRIu64 " us each and at interval 0 a mean of %"
                PRIu64 " us; with the queue full, %" PRIu32 " of %" PRIu32
                " IMMEDIATE intervals were shorter than half a refresh, "
                "against %" PRIu32 " of %" PRIu32 " under FIFO",
             IMM_FRAMES, m_fifo1 / 1000, m_imm1 / 1000,
             imm1.under_half_steady, imm1.steady_intervals,
             fifo1.under_half_steady, fifo1.steady_intervals);
   }

   /* ---------------------------------------------------------------- */
   /* C — the two-image request.                                       */
   /* ---------------------------------------------------------------- */

   imm_stats imm2;
   const uint64_t m_imm2 =
      imm_measure(&fw, surface, VK_PRESENT_MODE_IMMEDIATE_KHR, 2, extent,
                  IMM_FRAMES, "C: IMMEDIATE, 2 asked for", m_fifo1, true,
                  &imm2);
   (void)m_imm2;

   imm_stats fifo_two;
   const uint64_t m_fifo_two =
      imm_measure(&fw, surface, VK_PRESENT_MODE_FIFO_KHR, 2, extent,
                  IMM_FRAMES, "C: FIFO, 2 images", 0, true, &fifo_two);
   (void)m_fifo_two;

   /* ---------------------------------------------------------------- */
   /* D — holding more than one image, and giving them all back.       */
   /* ---------------------------------------------------------------- */
   {
      imm_swapchain sc;
      const char *what = "D: IMMEDIATE, several images held at once";
      if (imm_create(&fw, surface, VK_PRESENT_MODE_IMMEDIATE_KHR, 3, extent,
                     VK_NULL_HANDLE, what, &sc)) {
         /* One short of the image count: the compositor keeps at least
          * one, so this is a legitimate thing for an application to do
          * rather than the deadlock the driver refuses. */
         const uint32_t hold = sc.image_count - 1;
         uint32_t held[IMM_MAX_IMAGES];
         uint32_t got = 0;
         bool clash = false;

         for (uint32_t k = 0; k < hold; k++) {
            uint32_t index = 0;
            VkResult ar =
               fw.wsi.vkAcquireNextImageKHR(fw.dev, sc.handle, IMM_WAIT_NS,
                                            VK_NULL_HANDLE,
                                            sc.acquire_fences[k], &index);
            if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
               t_check(t, false, "%s: acquire %" PRIu32 " of %" PRIu32
                       " -> %s", what, k + 1, hold, vkfw_result_str(ar));
               break;
            }
            const VkResult wr =
               fw.vk.vkWaitForFences(fw.dev, 1, &sc.acquire_fences[k],
                                     VK_TRUE, IMM_WAIT_NS);
            if (!t_check(t, wr == VK_SUCCESS,
                         "%s: the acquire fence for image %" PRIu32 " -> %s",
                         what, k + 1, vkfw_result_str(wr)))
               break;
            fw.vk.vkResetFences(fw.dev, 1, &sc.acquire_fences[k]);

            if (index >= sc.image_count) {
               t_check(t, false, "%s: image index %" PRIu32 " out of range",
                       what, index);
               break;
            }
            for (uint32_t p = 0; p < got; p++) {
               if (held[p] == index)
                  clash = true;
            }
            held[got++] = index;
         }

         t_check(t, got == hold,
                 "%s: %" PRIu32 " of %" PRIu32 " images acquired without "
                 "presenting any of them", what, got, hold);
         t_check(t, !clash,
                 "%s: no image was handed over twice while it was held",
                 what);

         uint32_t presented = 0;
         for (uint32_t k = 0; k < got; k++) {
            const VkResult sr =
               imm_record_and_submit(&fw, &sc, held[k], k);
            if (!t_check(t, sr == VK_SUCCESS,
                         "%s: recording image %" PRIu32 " -> %s", what,
                         held[k], vkfw_result_str(sr)))
               break;

            const VkPresentInfoKHR pi = {
               .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
               .waitSemaphoreCount = 1,
               .pWaitSemaphores = &sc.res[held[k]].render_done,
               .swapchainCount = 1,
               .pSwapchains = &sc.handle,
               .pImageIndices = &held[k],
            };
            const VkResult pr = fw.wsi.vkQueuePresentKHR(fw.queue, &pi);
            if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR) {
               t_check(t, false, "%s: presenting image %" PRIu32 " -> %s",
                       what, held[k], vkfw_result_str(pr));
               break;
            }
            presented++;
         }
         t_check(t, presented == got,
                 "%s: all %" PRIu32 " held images went back to the "
                 "compositor", what, got);

         /* And the swapchain still works afterwards: a slot that was
          * not really given back shows up as the next run stalling. */
         imm_stats after;
         imm_stats_init(&after, "D: after the held images went back", &sc);
         imm_run(&fw, &sc, IMM_SHORT_FRAMES, &after);
         imm_report(t, &after, IMM_SHORT_FRAMES);
         imm_report_immediate(t, &after, m_fifo1);

         const VkResult qr = imm_quiesce(&fw);
         t_check(t, qr == VK_SUCCESS, "%s: idle before teardown -> %s", what,
                 vkfw_result_str(qr));
         imm_destroy(&fw, &sc);
      }
   }

   /* ---------------------------------------------------------------- */
   /* E — recreation, alternating the mode.                            */
   /* ---------------------------------------------------------------- */
   {
      imm_swapchain cur;
      memset(&cur, 0, sizeof(cur));
      bool have_cur = false;
      uint32_t generations = 0;
      uint32_t right_interval = 0;
      uint32_t right_pacing = 0;
      uint32_t full_runs = 0;

      for (uint32_t g = 0; g < IMM_GENERATIONS; g++) {
         const VkPresentModeKHR mode = (g & 1u) != 0
            ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_IMMEDIATE_KHR;

         char what[64];
         snprintf(what, sizeof(what), "E: generation %" PRIu32 " (%s)", g,
                  imm_mode_name(mode));

         imm_swapchain next;
         const VkSwapchainKHR old = have_cur ? cur.handle : VK_NULL_HANDLE;
         if (!imm_create(&fw, surface, mode, 3, extent, old, what, &next))
            break;

         /* The old one goes only after the new one exists, which is the
          * order Vulkan's recreation contract describes and the order
          * the surface's single-owner rule is written for. */
         if (have_cur) {
            const VkResult qr = imm_quiesce(&fw);
            t_check(t, qr == VK_SUCCESS, "%s: idle before retiring the "
                    "previous generation -> %s", what, vkfw_result_str(qr));
            imm_destroy(&fw, &cur);
         }
         cur = next;
         have_cur = true;
         generations++;

         if (cur.interval == imm_expected_interval(mode))
            right_interval++;

         imm_stats st;
         imm_stats_init(&st, what, &cur);
         imm_run(&fw, &cur, IMM_GEN_FRAMES, &st);
         if (!st.failed && st.frames == IMM_GEN_FRAMES)
            full_runs++;

         /* THE CHECK THIS SECTION IS FOR, and it is about the pacing
          * rather than the interval field: a swap interval that stuck
          * to the window instead of following the swapchain would still
          * be reported correctly at creation and would show up here. */
         const uint64_t mean = imm_mean_ns(&st);
         const bool ok = mode == VK_PRESENT_MODE_IMMEDIATE_KHR
            ? (st.intervals != 0 &&
               mean * IMM_FAST_DEN <= IMM_REFRESH_NS * IMM_FAST_NUM)
            : (st.intervals != 0 &&
               st.within_10pct * 100u >= st.intervals * IMM_FIFO_PACED_PCT);
         if (ok)
            right_pacing++;

         t_note(t, "%s: %" PRIu32 " frames, mean %" PRIu64 " us, %" PRIu32
                   " of %" PRIu32 " under half a refresh, %" PRIu32
                   " within 10%% of one, interval %d", what, st.frames,
                mean / 1000, st.under_half, st.intervals, st.within_10pct,
                cur.interval);
      }

      t_check(t, generations == IMM_GENERATIONS,
              "E: %" PRIu32 " of %" PRIu32 " generations were created",
              generations, IMM_GENERATIONS);
      t_check(t, full_runs == generations,
              "E: %" PRIu32 " of %" PRIu32 " generations presented all %"
              PRIu32 " of their frames", full_runs, generations,
              IMM_GEN_FRAMES);
      t_check(t, right_interval == generations,
              "E: %" PRIu32 " of %" PRIu32 " generations registered the "
              "swap interval their own mode implies", right_interval,
              generations);
      t_check(t, right_pacing == generations,
              "E: %" PRIu32 " of %" PRIu32 " generations were paced by "
              "their own mode — the interval follows the swapchain, not "
              "the window", right_pacing, generations);

      if (have_cur) {
         const VkResult qr = imm_quiesce(&fw);
         t_check(t, qr == VK_SUCCESS, "E: idle before the last teardown -> "
                 "%s", vkfw_result_str(qr));
         imm_destroy(&fw, &cur);
      }
   }

   /* ---------------------------------------------------------------- */
   /* F — the copy fallback, in both modes.                            */
   /* ---------------------------------------------------------------- */
   {
      setenv("MESA_VK_WSI_HORIZON_FORCE_COPY", "1", 1);

      imm_swapchain probe;
      bool forced = false;
      if (imm_create(&fw, surface, VK_PRESENT_MODE_IMMEDIATE_KHR, 3, extent,
                     VK_NULL_HANDLE, "F: forcing the copy path", &probe)) {
         forced = !probe.zero_copy;
         t_check(t, forced,
                 "F: MESA_VK_WSI_HORIZON_FORCE_COPY produced the copy path");
         t_check(t, probe.interval == 0,
                 "F: the copy fallback registered swap interval %d for "
                 "IMMEDIATE", probe.interval);
         const VkResult qr = imm_quiesce(&fw);
         t_check(t, qr == VK_SUCCESS, "F: idle before teardown -> %s",
                 vkfw_result_str(qr));
         imm_destroy(&fw, &probe);
      }

      if (forced) {
         /* NOT JUDGED AGAINST THE IMMEDIATE THRESHOLDS, and the reason
          * is a measurement rather than a concession.
          *
          * Run 24 (2026-08-10) put 60 frames through this path in each
          * mode: mean 16732 us at interval 1 and 16626 us at interval 0,
          * with the cheapest present costing 12480 us and 12317 us
          * against a 16666 us refresh. That floor is the path's own
          * cost — every
          * present copies the image row by row and then has libnx
          * swizzle it into the block-linear scanout buffer on the CPU —
          * and against a 16.6 ms refresh it leaves no room to get a
          * second frame into the queue. Interval 0's effect is the
          * compositor dropping surplus queued frames; with none queued
          * there is nothing to drop, so the mode has nothing to give
          * here and the swap interval is not what is missing.
          *
          * Applying the section-B thresholds would therefore report a
          * driver defect that is not one, and relaxing them would say
          * nothing. What is asserted instead is the state that was
          * measured, so that a console on which this path *does* get
          * ahead fails these checks and sends somebody to update the
          * design documentation — which is the only outcome worth being
          * told about. The hard checks stay: every frame is presented,
          * and the interval the mode implies is registered. */
         imm_stats cf, ci;
         const uint64_t m_cf =
            imm_measure(&fw, surface, VK_PRESENT_MODE_FIFO_KHR, 3, extent,
                        IMM_SHORT_FRAMES, "F: copy fallback, FIFO", 0, false,
                        &cf);
         const uint64_t m_ci =
            imm_measure(&fw, surface, VK_PRESENT_MODE_IMMEDIATE_KHR, 3,
                        extent, IMM_SHORT_FRAMES,
                        "F: copy fallback, IMMEDIATE", 0, false, &ci);

         if (m_cf != 0 && m_ci != 0) {
            t_note(t, "F: on the copy fallback the same %" PRIu32 " frames "
                      "took a mean of %" PRIu64 " us at interval 1 and %"
                      PRIu64 " us at interval 0; the cheapest present cost "
                      "%" PRIu64 " us and %" PRIu64 " us, against a refresh "
                      "of %" PRIu64 " us", IMM_SHORT_FRAMES, m_cf / 1000,
                   m_ci / 1000,
                   cf.present_min_ns == UINT64_MAX ? 0
                                                   : cf.present_min_ns / 1000,
                   ci.present_min_ns == UINT64_MAX ? 0
                                                   : ci.present_min_ns / 1000,
                   IMM_REFRESH_NS / 1000);

            /* WHY IT CANNOT HELP, AS A NUMBER. A present that never
             * costs less than half a refresh cannot put a second frame
             * in the queue within one, so the compositor has nothing
             * surplus to drop and interval 0 and interval 1 must come
             * out the same. This is the premise of the check below, so
             * it is asserted rather than assumed. */
            t_check(t, ci.present_min_ns >= IMM_REFRESH_NS / 2,
                    "F: the copy fallback's cheapest present is %" PRIu64
                    " us, at least half a refresh — the path cannot build "
                    "the queue depth interval 0 exists to drain",
                    ci.present_min_ns == UINT64_MAX ? 0
                                                    : ci.present_min_ns / 1000);

            /* And so the two modes pace alike here. Within 10% of each
             * other, and both at the refresh rather than under it. */
            const uint64_t lo = m_ci < m_cf ? m_ci : m_cf;
            const uint64_t hi = m_ci < m_cf ? m_cf : m_ci;
            t_check(t, hi * 9 <= lo * 10 &&
                    m_ci * IMM_FAST_DEN > IMM_REFRESH_NS * IMM_FAST_NUM,
                    "F: on the copy fallback the swap interval does not "
                    "change the pacing (%" PRIu64 " us at 0 against %" PRIu64
                    " us at 1) — a known limitation of the copy fallback. "
                    "If this check fails, that limitation no longer holds",
                    m_ci / 1000, m_cf / 1000);
         }
      }

      unsetenv("MESA_VK_WSI_HORIZON_FORCE_COPY");
   }

   /* ---------------------------------------------------------------- */
   /* G — nothing retained.                                            */
   /* ---------------------------------------------------------------- */
   {
      imm_swapchain sc;
      const char *what = "G: a swapchain created after everything above";
      if (imm_create(&fw, surface, VK_PRESENT_MODE_IMMEDIATE_KHR, 3, extent,
                     VK_NULL_HANDLE, what, &sc)) {
         t_check(t, sc.zero_copy,
                 "%s: still gets the zero-copy path — no slot and no "
                 "registration was retained by any of them", what);
         t_check(t, sc.interval == 0, "%s: still registers interval 0",
                 what);

         imm_stats st;
         imm_stats_init(&st, what, &sc);
         imm_run(&fw, &sc, IMM_SHORT_FRAMES, &st);
         imm_report(t, &st, IMM_SHORT_FRAMES);
         imm_report_immediate(t, &st, m_fifo1);

         const VkResult qr = imm_quiesce(&fw);
         t_check(t, qr == VK_SUCCESS, "%s: idle before teardown -> %s", what,
                 vkfw_result_str(qr));
         imm_destroy(&fw, &sc);
      }
   }

   /* What the driver said it could not give back, if anything. These
    * have no Vulkan representation at all — a present that succeeded
    * beside a slot the queue still holds looks identical to one that
    * did not — so the log is the only oracle. */
   static const char *const complaints[] = {
      "stays ACQUIRED",
      "destroy refused",
      "nwindowReleaseBuffers",
      "the compositor refused",
      "cancelling slot",
   };
   for (uint32_t i = 0; i < 5; i++) {
      bool found = true;
      const bool scanned = t_log_scan(t, complaints[i], &found);
      t_check(t, scanned && !found, "G: the driver never said \"%s\"%s",
              complaints[i], scanned ? "" : " (the log could not be read)");
   }

out_surface:
   fw.wsi.vkDestroySurfaceKHR(fw.instance, surface, NULL);
out:
   vkfw_finish(&fw);
   return rv;
}
