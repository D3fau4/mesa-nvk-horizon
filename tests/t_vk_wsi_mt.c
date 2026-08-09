/*
 * t_vk_wsi_mt — the swapchain under concurrency, and under length.
 *
 * WHY THIS IS A SECOND TEST AND NOT A SECTION OF t_vk_swapchain. That
 * one asks whether a swapchain presents; this one asks whether it keeps
 * presenting while something else legitimately happens at the same
 * time. STATUS.md listed "anything multi-threaded" as never verified,
 * and the WSI's own comments reason about concurrent creation and
 * eviction — reasoning nothing had ever executed.
 *
 * WHAT "VALID" MEANS HERE, because it is the whole design constraint.
 * Vulkan externally synchronises a swapchain in vkAcquireNextImageKHR,
 * vkQueuePresentKHR, vkReleaseSwapchainImagesEXT and
 * vkDestroySwapchainKHR; a surface in vkCreateSwapchainKHR; and a queue
 * in vkQueueSubmit and vkQueuePresentKHR. Every section below holds an
 * application-side mutex for exactly those, and nothing more — so a
 * failure is the driver's and not the test's. In particular:
 *
 *   - Two threads never touch one swapchain without the swapchain's own
 *     mutex, and never touch the queue without the queue's.
 *   - vkDestroySwapchainKHR(old) beside vkAcquireNextImageKHR(new) is
 *     NOT synchronised, on purpose: they name different objects, so the
 *     specification does not require it and an application may do it.
 *     That pairing is section B, and it is where the defect this test
 *     was written for lives.
 *   - Object creation, memory allocation and the surface queries take
 *     no lock at all, because vkCreateBuffer, vkAllocateMemory and
 *     vkGetPhysicalDeviceSurface*KHR require none.
 *
 * THE SECTIONS:
 *
 *   A. Recreate, then destroy the old one — single-threaded, on both
 *      present paths. The ordinary swapchain-recreation sequence: create
 *      the new swapchain, watch the old one start refusing, destroy the
 *      old one, keep presenting on the new one. On the copy fallback
 *      this used to end the new swapchain's life, because libnx's
 *      framebufferClose calls nwindowReleaseBuffers on the window —
 *      whoever owns it by then. Deterministic, and the reason this file
 *      exists.
 *
 *   B. The same, concurrently. A present loop on the new swapchain runs
 *      on one thread while another destroys the old one underneath it.
 *      Legal, unsynchronised by the specification, and the concurrent
 *      shape of section A.
 *
 *   C. A render thread and a present thread. The pattern every engine
 *      uses: one thread acquires, records and submits; another presents.
 *      The swapchain mutex is the application's, the queue mutex is the
 *      application's, and the WSI has to be free of any state that
 *      assumes one thread.
 *
 *   D. Recreation churn with a reaper. Generation n+1 is created with
 *      generation n as oldSwapchain and starts presenting immediately,
 *      while a second thread destroys generation n. Thirty generations,
 *      alternating image counts.
 *
 *   E. Unrelated device work beside a present loop. Buffers allocated
 *      and freed, images created and destroyed, and the surface
 *      queried, all from a second thread that never touches the
 *      swapchain. Nothing here is externally synchronised by the
 *      specification, so anything that breaks is shared state the WSI
 *      or the driver should not have had.
 *
 *   F. Length. Three thousand presents with a recreation every two
 *      hundred, both paths, while E's thread keeps running. Frame
 *      pacing is reported; a slot leaked once per generation shows up
 *      here as an acquire that stops returning.
 *
 * THREADS ARE PINNED where the kernel allows it, and the test says
 * which cores it got. Two threads timesliced on one core interleave;
 * two on different cores race. Only the second finds the memory
 * ordering bugs, and whether it happened is not something to assume.
 *
 * REPORTING IS SINGLE-THREADED. testfw's t_check and t_note write to a
 * shared FILE and shared counters; no worker calls either. Workers fill
 * in a result struct and the main thread reads it after the join.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include "common/vkfw.h"

const char *const test_name = "t_vk_wsi_mt";

/* This test owns the display: no console, the SD-card log is the whole
 * record (testfw.h). */
const bool test_uses_display = true;

#define MT_MAX_IMAGES 4u

#define MT_REFRESH_NS UINT64_C(16666666)

/* A wait long enough to be a hang detector and short enough that a hang
 * does not cost the operator the session. Every acquire and every fence
 * wait in this file carries it: an infinite timeout in a multi-threaded
 * test turns a defect into a lock-up with no verdict, and this test
 * exists to produce verdicts. t_vk_swapchain covers the infinite case
 * on one thread. */
#define MT_WAIT_NS UINT64_C(2000000000)

/* Section frame counts. Long enough to be more than a smoke test,
 * short enough that the whole file is a couple of minutes. */
#define MT_A_FRAMES     30u
#define MT_B_GENS       20u
#define MT_B_FRAMES     20u
#define MT_C_FRAMES    600u
#define MT_D_GENS       30u
#define MT_D_FRAMES     30u
#define MT_E_FRAMES    300u
#define MT_F_FRAMES   3000u
#define MT_F_GEN_EVERY 200u

/* ------------------------------------------------------------------ */
/* Core pinning                                                        */
/* ------------------------------------------------------------------ */

/* Moves the calling thread onto `core` when the process is allowed it.
 * Returns the core it ended up asking for, or -1 when it asked for
 * nothing.
 *
 * A pthread on this platform inherits the creating thread's core, so
 * without this every worker in the file runs where main() runs and the
 * concurrency is timeslicing rather than parallelism. That is still a
 * test, and it is a weaker one; which of the two happened is recorded
 * rather than assumed. */
static int mt_pin_self(int core)
{
   u64 allowed = 0;
   Result rc = svcGetInfo(&allowed, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
   if (R_FAILED(rc))
      return -1;
   if (core < 0 || core > 63 || !(allowed & (UINT64_C(1) << core)))
      return -1;
   rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, core,
                             (u32)(UINT64_C(1) << core));
   return R_FAILED(rc) ? -1 : core;
}

/* ------------------------------------------------------------------ */
/* The fixture the sections share                                      */
/* ------------------------------------------------------------------ */

typedef struct mt_ctx {
   vkfw *fw;
   VkSurfaceKHR surface;
   VkExtent2D extent;
   VkFormat format;

   /* VkQueue is externally synchronised in vkQueueSubmit and
    * vkQueuePresentKHR, and this device has one queue. */
   pthread_mutex_t queue_lock;

   /* The first teardown that could not get the device quiet again, from
    * whichever thread hit it. VK_SUCCESS until it is not, and read once
    * by the main thread at the end — a worker may not call t_check. */
   atomic_int quiesce_error;
} mt_ctx;

typedef struct mt_sc {
   VkSwapchainKHR handle;
   uint32_t image_count;
   VkExtent2D extent;

   VkImage images[MT_MAX_IMAGES];
   /* ITS OWN COMMAND POOL, and that is external synchronisation rather
    * than tidiness. Vulkan externally synchronises a VkCommandPool in
    * vkAllocateCommandBuffers, in vkFreeCommandBuffers, and in every
    * vkBeginCommandBuffer and vkCmd* on a buffer allocated from it. A
    * reaper thread destroying generation n therefore may not share a
    * pool with the thread recording generation n+1 — which is what the
    * fixture's single fw->pool would have been. One pool per swapchain,
    * destroyed with it, and each is touched by one thread. */
   VkCommandPool pool;
   VkCommandBuffer cb[MT_MAX_IMAGES];
   VkSemaphore render_done[MT_MAX_IMAGES];
   VkFence in_flight[MT_MAX_IMAGES];
   bool in_flight_valid[MT_MAX_IMAGES];
   VkFence acquire_fence;

   /* The application's external synchronisation of this VkSwapchainKHR.
    * Held across vkAcquireNextImageKHR, vkQueuePresentKHR and
    * vkDestroySwapchainKHR — the three the specification names. */
   pthread_mutex_t lock;
   /* Whether `lock` has been initialised and not yet destroyed. A
    * swapchain handed to a reaper thread comes back zeroed, and
    * pthread_mutex_destroy on a zeroed mutex is undefined rather than
    * merely useless. */
   bool live;

   bool zero_copy;   /* what the driver's decision message said */
} mt_sc;

/* LOCK ORDER, stated once and obeyed everywhere: swapchain before
 * queue. vkQueuePresentKHR needs both and is the only place they are
 * held together, so one order is enough to make a deadlock
 * impossible — but only if it is the same order every time. */
static void mt_lock_sc(mt_sc *sc)     { pthread_mutex_lock(&sc->lock); }
static void mt_unlock_sc(mt_sc *sc)   { pthread_mutex_unlock(&sc->lock); }
static void mt_lock_q(mt_ctx *c)      { pthread_mutex_lock(&c->queue_lock); }
static void mt_unlock_q(mt_ctx *c)    { pthread_mutex_unlock(&c->queue_lock); }

static void mt_frame_colour(uint32_t frame, VkClearColorValue *out)
{
   out->float32[0] = (float)((frame * 3u) & 0xffu) / 255.0f;
   out->float32[1] = (float)((frame * 5u + 85u) & 0xffu) / 255.0f;
   out->float32[2] = (float)((frame * 7u + 170u) & 0xffu) / 255.0f;
   out->float32[3] = 1.0f;
}

static void mt_sc_destroy_locked(mt_ctx *c, mt_sc *sc)
{
   vkfw *fw = c->fw;

   for (uint32_t i = 0; i < sc->image_count; i++) {
      if (sc->render_done[i] != VK_NULL_HANDLE)
         fw->wsi.vkDestroySemaphore(fw->dev, sc->render_done[i], NULL);
      if (sc->in_flight[i] != VK_NULL_HANDLE)
         fw->vk.vkDestroyFence(fw->dev, sc->in_flight[i], NULL);
   }
   /* The pool takes its command buffers with it; freeing them first
    * would be a second externally-synchronised use of it for nothing. */
   if (sc->pool != VK_NULL_HANDLE)
      fw->vk.vkDestroyCommandPool(fw->dev, sc->pool, NULL);
   if (sc->acquire_fence != VK_NULL_HANDLE)
      fw->vk.vkDestroyFence(fw->dev, sc->acquire_fence, NULL);
   if (sc->handle != VK_NULL_HANDLE)
      fw->wsi.vkDestroySwapchainKHR(fw->dev, sc->handle, NULL);

   sc->handle = VK_NULL_HANDLE;
   sc->image_count = 0;
   sc->pool = VK_NULL_HANDLE;
   memset(sc->images, 0, sizeof(sc->images));
   memset(sc->cb, 0, sizeof(sc->cb));
   memset(sc->render_done, 0, sizeof(sc->render_done));
   memset(sc->in_flight, 0, sizeof(sc->in_flight));
   memset(sc->in_flight_valid, 0, sizeof(sc->in_flight_valid));
   sc->acquire_fence = VK_NULL_HANDLE;
}

/* Keeps the first quiesce failure of the run, whoever hit it. */
static void mt_record_quiesce_failure(mt_ctx *c, VkResult r)
{
   int expected = (int)VK_SUCCESS;
   atomic_compare_exchange_strong(&c->quiesce_error, &expected, (int)r);
}

/* Everything the device might still be doing with this swapchain,
 * finished — before a single one of its objects is destroyed.
 *
 * TWO WAITS AND NOT ONE. The per-image fence says the render submission
 * retired. It says nothing about the vkQueuePresentKHR that waits on
 * render_done[i], because a fence signalled by an earlier submission is
 * no proof that a later queue operation has consumed the semaphore it
 * was waiting on. vkDestroySemaphore requires every batch referring to
 * the semaphore to have completed, so the present has to be waited for
 * too, and on a queue vkQueueWaitIdle is the only thing that says so.
 *
 * The fence waits stay unlocked and per image: they cost nobody else
 * anything and they name which image is stuck. The queue wait takes the
 * queue lock, because vkQueueWaitIdle externally synchronises the queue,
 * and that does briefly stall a thread presenting on a different
 * swapchain. It does not weaken sections B, D and F — the destroy itself
 * still lands against the survivor's acquire and present, which is the
 * unsynchronised pairing under test. */
static VkResult mt_sc_quiesce(mt_ctx *c, mt_sc *sc)
{
   vkfw *fw = c->fw;
   VkResult first = VK_SUCCESS;

   for (uint32_t i = 0; i < sc->image_count; i++) {
      if (!sc->in_flight_valid[i])
         continue;
      const VkResult r = fw->vk.vkWaitForFences(fw->dev, 1, &sc->in_flight[i],
                                                VK_TRUE, MT_WAIT_NS);
      if (r != VK_SUCCESS && first == VK_SUCCESS)
         first = r;
   }

   mt_lock_q(c);
   const VkResult r = fw->vk.vkQueueWaitIdle(fw->queue);
   mt_unlock_q(c);
   if (r != VK_SUCCESS && first == VK_SUCCESS)
      first = r;

   return first;
}

/* Destroys a swapchain the way an application does: holding the
 * swapchain's own mutex, and nothing else's. */
static void mt_sc_destroy(mt_ctx *c, mt_sc *sc)
{
   if (!sc->live)
      return;

   if (sc->handle == VK_NULL_HANDLE) {
      pthread_mutex_destroy(&sc->lock);
      memset(sc, 0, sizeof(*sc));
      return;
   }

   /* IF THE DEVICE DID NOT GO QUIET, NOTHING BELOW HAPPENS. A
    * vkDestroyFence, vkDestroyCommandPool or vkDestroySwapchainKHR with
    * work still referring to them is undefined behaviour, and undefined
    * behaviour here is a console that stops answering — the state least
    * likely to produce a readable verdict. A timeout at this point is
    * precisely the stall the two-second waits in this file exist to
    * catch, so it is recorded and reported and the handles are leaked on
    * purpose: the process is about to print a failure and exit. */
   const VkResult q = mt_sc_quiesce(c, sc);
   if (q != VK_SUCCESS) {
      mt_record_quiesce_failure(c, q);
      return;
   }

   mt_lock_sc(sc);
   mt_sc_destroy_locked(c, sc);
   mt_unlock_sc(sc);

   pthread_mutex_destroy(&sc->lock);
   memset(sc, 0, sizeof(*sc));
}

/* `err_out` receives the first failing VkResult, so a caller in a
 * worker thread can report without touching testfw. */
static bool mt_sc_create(mt_ctx *c, uint32_t want_images, VkPresentModeKHR mode,
                         VkSwapchainKHR old, mt_sc *sc, VkResult *err_out)
{
   vkfw *fw = c->fw;
   memset(sc, 0, sizeof(*sc));
   sc->extent = c->extent;
   pthread_mutex_init(&sc->lock, NULL);
   sc->live = true;
   *err_out = VK_SUCCESS;

   const VkSwapchainCreateInfoKHR ci = {
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .surface = c->surface,
      .minImageCount = want_images,
      .imageFormat = c->format,
      .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
      .imageExtent = c->extent,
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
   if (r != VK_SUCCESS) {
      *err_out = r;
      sc->handle = VK_NULL_HANDLE;
      goto fail;
   }

   uint32_t count = 0;
   r = fw->wsi.vkGetSwapchainImagesKHR(fw->dev, sc->handle, &count, NULL);
   if (r != VK_SUCCESS || count < 2 || count > MT_MAX_IMAGES) {
      *err_out = (r == VK_SUCCESS) ? VK_ERROR_UNKNOWN : r;
      goto fail;
   }
   r = fw->wsi.vkGetSwapchainImagesKHR(fw->dev, sc->handle, &count, sc->images);
   if (r != VK_SUCCESS) {
      *err_out = r;
      goto fail;
   }
   sc->image_count = count;

   const VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
   r = fw->vk.vkCreateFence(fw->dev, &fci, NULL, &sc->acquire_fence);
   if (r != VK_SUCCESS) {
      *err_out = r;
      goto fail;
   }

   const VkCommandPoolCreateInfo cpci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = fw->queue_family,
   };
   r = fw->vk.vkCreateCommandPool(fw->dev, &cpci, NULL, &sc->pool);
   if (r != VK_SUCCESS) {
      *err_out = r;
      goto fail;
   }

   for (uint32_t i = 0; i < sc->image_count; i++) {
      const VkCommandBufferAllocateInfo cbai = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = sc->pool,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 1,
      };
      r = fw->vk.vkAllocateCommandBuffers(fw->dev, &cbai, &sc->cb[i]);
      if (r != VK_SUCCESS) {
         *err_out = r;
         goto fail;
      }
      const VkSemaphoreCreateInfo sci = {
         .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
      };
      r = fw->wsi.vkCreateSemaphore(fw->dev, &sci, NULL, &sc->render_done[i]);
      if (r != VK_SUCCESS) {
         *err_out = r;
         goto fail;
      }
      r = fw->vk.vkCreateFence(fw->dev, &fci, NULL, &sc->in_flight[i]);
      if (r != VK_SUCCESS) {
         *err_out = r;
         goto fail;
      }
   }

   sc->zero_copy = vkfw_saw_message(fw, "wsi_horizon: zero-copy:", NULL);
   return true;

fail:
   mt_sc_destroy_locked(c, sc);
   pthread_mutex_destroy(&sc->lock);
   memset(sc, 0, sizeof(*sc));
   return false;
}

/* One acquire, taking the swapchain's mutex for exactly the call the
 * specification says needs it, and the acquire fence's wait outside it. */
static VkResult mt_acquire(mt_ctx *c, mt_sc *sc, uint32_t *index_out)
{
   vkfw *fw = c->fw;

   mt_lock_sc(sc);
   VkResult r = fw->wsi.vkAcquireNextImageKHR(fw->dev, sc->handle, MT_WAIT_NS,
                                              VK_NULL_HANDLE,
                                              sc->acquire_fence, index_out);
   mt_unlock_sc(sc);

   if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
      return r;
   if (*index_out >= sc->image_count)
      return VK_ERROR_UNKNOWN;

   /* vkWaitForFences is not externally synchronised; vkResetFences is,
    * and only the acquiring thread touches this fence. */
   VkResult wr = fw->vk.vkWaitForFences(fw->dev, 1, &sc->acquire_fence,
                                        VK_TRUE, MT_WAIT_NS);
   if (wr != VK_SUCCESS)
      return wr;
   wr = fw->vk.vkResetFences(fw->dev, 1, &sc->acquire_fence);
   if (wr != VK_SUCCESS)
      return wr;

   return r;
}

/* Records a clear into the acquired image and submits it. The command
 * buffer and the fences belong to whichever thread calls this, and only
 * one thread ever does per swapchain — VkCommandPool is externally
 * synchronised and this is what keeps that true. */
static VkResult mt_record_submit(mt_ctx *c, mt_sc *sc, uint32_t index,
                                 uint32_t frame)
{
   vkfw *fw = c->fw;
   VkResult r;

   if (sc->in_flight_valid[index]) {
      r = fw->vk.vkWaitForFences(fw->dev, 1, &sc->in_flight[index], VK_TRUE,
                                 MT_WAIT_NS);
      if (r != VK_SUCCESS)
         return r;
   }
   r = fw->vk.vkResetFences(fw->dev, 1, &sc->in_flight[index]);
   if (r != VK_SUCCESS)
      return r;

   const VkCommandBufferBeginInfo bi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   r = fw->vk.vkBeginCommandBuffer(sc->cb[index], &bi);
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
   fw->vk.vkCmdPipelineBarrier(sc->cb[index], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                               0, NULL, 0, NULL, 1, &bar);

   VkClearColorValue colour;
   mt_frame_colour(frame, &colour);
   fw->vk.vkCmdClearColorImage(sc->cb[index], sc->images[index],
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               &colour, 1, &range);

   bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
   bar.dstAccessMask = 0;
   bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   bar.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
   fw->vk.vkCmdPipelineBarrier(sc->cb[index], VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                               0, NULL, 0, NULL, 1, &bar);

   r = fw->vk.vkEndCommandBuffer(sc->cb[index]);
   if (r != VK_SUCCESS)
      return r;

   const VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &sc->cb[index],
      .signalSemaphoreCount = 1,
      .pSignalSemaphores = &sc->render_done[index],
   };

   mt_lock_q(c);
   r = fw->vk.vkQueueSubmit(fw->queue, 1, &si, sc->in_flight[index]);
   mt_unlock_q(c);

   if (r == VK_SUCCESS)
      sc->in_flight_valid[index] = true;
   return r;
}

/* One present, holding the swapchain's mutex and the queue's, in that
 * order. */
static VkResult mt_present(mt_ctx *c, mt_sc *sc, uint32_t index)
{
   vkfw *fw = c->fw;
   const VkPresentInfoKHR pi = {
      .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &sc->render_done[index],
      .swapchainCount = 1,
      .pSwapchains = &sc->handle,
      .pImageIndices = &index,
   };

   mt_lock_sc(sc);
   mt_lock_q(c);
   VkResult r = fw->wsi.vkQueuePresentKHR(fw->queue, &pi);
   mt_unlock_q(c);
   mt_unlock_sc(sc);

   return r;
}

/* The whole frame on one thread: acquire, render, present. Returns the
 * first non-success result, VK_SUBOPTIMAL_KHR excepted. */
static VkResult mt_frame(mt_ctx *c, mt_sc *sc, uint32_t frame)
{
   uint32_t index = 0;
   VkResult r = mt_acquire(c, sc, &index);
   if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
      return r;

   r = mt_record_submit(c, sc, index, frame);
   if (r != VK_SUCCESS)
      return r;

   r = mt_present(c, sc, index);
   if (r == VK_SUBOPTIMAL_KHR)
      r = VK_SUCCESS;
   return r;
}

/* `frames` frames on one thread. Stops at the first failure and reports
 * how far it got. */
typedef struct mt_run {
   uint32_t presented;
   VkResult first_error;
   uint32_t error_frame;
   uint64_t total_ns;
} mt_run;

static void mt_run_frames(mt_ctx *c, mt_sc *sc, uint32_t frames, uint32_t seed,
                          mt_run *out)
{
   memset(out, 0, sizeof(*out));
   out->first_error = VK_SUCCESS;

   const u64 start = armGetSystemTick();
   for (uint32_t i = 0; i < frames; i++) {
      VkResult r = mt_frame(c, sc, seed + i);
      if (r != VK_SUCCESS) {
         out->first_error = r;
         out->error_frame = i;
         break;
      }
      out->presented++;
   }
   out->total_ns = armTicksToNs(armGetSystemTick() - start);
}

/* ------------------------------------------------------------------ */
/* Worker threads                                                      */
/* ------------------------------------------------------------------ */

/* A thread that destroys one swapchain, after a delay, while somebody
 * else is presenting on another. */
typedef struct mt_reaper_job {
   mt_ctx *c;
   mt_sc *sc;
   uint64_t delay_ns;
   int core;
   int core_got;
   bool done;
} mt_reaper_job;

static void *mt_reaper_thread(void *arg)
{
   mt_reaper_job *job = arg;
   job->core_got = mt_pin_self(job->core);
   if (job->delay_ns)
      svcSleepThread(job->delay_ns);
   mt_sc_destroy(job->c, job->sc);
   job->done = true;
   return NULL;
}

/* A thread doing ordinary device work and surface queries, none of
 * which Vulkan externally synchronises against a present. */
typedef struct mt_churn_job {
   mt_ctx *c;
   /* Written by the main thread, read by the worker, so it is atomic and
    * not volatile: volatile orders nothing and makes nothing indivisible,
    * which leaves a plain volatile flag shared across threads a data race
    * in C11 whatever it happens to compile to. t_threads.c and t_ostime.c
    * in this same suite already use atomics for exactly this. */
   atomic_bool stop;
   int core;
   int core_got;

   uint32_t buffers;
   uint32_t images;
   uint32_t queries;
   VkResult first_error;
   const char *first_error_what;
} mt_churn_job;

static void *mt_churn_thread(void *arg)
{
   mt_churn_job *job = arg;
   mt_ctx *c = job->c;
   vkfw *fw = c->fw;
   job->core_got = mt_pin_self(job->core);
   job->first_error = VK_SUCCESS;

   while (!atomic_load(&job->stop)) {
      /* A buffer with memory bound and freed again. vkCreateBuffer,
       * vkAllocateMemory, vkFreeMemory and vkDestroyBuffer are all
       * internally synchronised.
       *
       * SPELLED OUT RATHER THAN vkfw_buffer_create, because that helper
       * reports each step through t_check — a shared FILE and a shared
       * counter, from a thread that must not touch either, and several
       * hundred extra checks in the summary besides. vkfw_memory_type
       * is a pure lookup and is safe to call. */
      const VkBufferCreateInfo bci = {
         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
         .size = 64 * 1024,
         .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      };
      VkBuffer buf = VK_NULL_HANDLE;
      VkResult r = fw->vk.vkCreateBuffer(fw->dev, &bci, NULL, &buf);
      if (r != VK_SUCCESS) {
         if (job->first_error == VK_SUCCESS) {
            job->first_error = r;
            job->first_error_what = "vkCreateBuffer";
         }
         break;
      }

      VkMemoryRequirements breq;
      fw->vk.vkGetBufferMemoryRequirements(fw->dev, buf, &breq);
      const uint32_t type =
         vkfw_memory_type(fw, breq.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
      VkDeviceMemory bmem = VK_NULL_HANDLE;
      if (type != UINT32_MAX) {
         const VkMemoryAllocateInfo mai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = breq.size,
            .memoryTypeIndex = type,
         };
         r = fw->vk.vkAllocateMemory(fw->dev, &mai, NULL, &bmem);
         if (r == VK_SUCCESS)
            r = fw->vk.vkBindBufferMemory(fw->dev, buf, bmem, 0);
      } else {
         r = VK_ERROR_FORMAT_NOT_SUPPORTED;
      }
      if (bmem != VK_NULL_HANDLE)
         fw->vk.vkFreeMemory(fw->dev, bmem, NULL);
      fw->vk.vkDestroyBuffer(fw->dev, buf, NULL);
      if (r != VK_SUCCESS) {
         if (job->first_error == VK_SUCCESS) {
            job->first_error = r;
            job->first_error_what = "allocating and binding a buffer";
         }
         break;
      }
      job->buffers++;

      /* An image, because image creation reaches NIL and the memory
       * heaps a swapchain image also comes from. */
      const VkImageCreateInfo ici = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = VK_FORMAT_R8G8B8A8_UNORM,
         .extent = { 64, 64, 1 },
         .mipLevels = 1,
         .arrayLayers = 1,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .tiling = VK_IMAGE_TILING_OPTIMAL,
         .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                  VK_IMAGE_USAGE_SAMPLED_BIT,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      };
      VkImage img = VK_NULL_HANDLE;
      r = fw->vk.vkCreateImage(fw->dev, &ici, NULL, &img);
      if (r != VK_SUCCESS) {
         if (job->first_error == VK_SUCCESS) {
            job->first_error = r;
            job->first_error_what = "vkCreateImage";
         }
         break;
      }
      VkMemoryRequirements req;
      fw->vk.vkGetImageMemoryRequirements(fw->dev, img, &req);
      fw->vk.vkDestroyImage(fw->dev, img, NULL);
      job->images++;

      /* The surface queries. vkGetPhysicalDeviceSurfaceCapabilitiesKHR
       * and friends require no external synchronisation, so an
       * application may ask while another thread presents — and on this
       * backend those calls read the NWindow the present is writing. */
      VkSurfaceCapabilitiesKHR caps;
      memset(&caps, 0, sizeof(caps));
      r = fw->wsi.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(fw->pdev,
                                                            c->surface, &caps);
      if (r != VK_SUCCESS) {
         if (job->first_error == VK_SUCCESS) {
            job->first_error = r;
            job->first_error_what =
               "vkGetPhysicalDeviceSurfaceCapabilitiesKHR";
         }
         break;
      }
      if (caps.currentExtent.width != c->extent.width ||
          caps.currentExtent.height != c->extent.height) {
         /* Not a failure of this test's own making: the display mode
          * changed under it (docking). Recorded so the section's other
          * results can be read in that light. */
         if (job->first_error == VK_SUCCESS) {
            job->first_error = VK_ERROR_OUT_OF_DATE_KHR;
            job->first_error_what = "the surface extent changed mid-run";
         }
         break;
      }

      uint32_t n = 0;
      r = fw->wsi.vkGetPhysicalDeviceSurfacePresentModesKHR(fw->pdev,
                                                            c->surface,
                                                            &n, NULL);
      if (r != VK_SUCCESS || n == 0) {
         if (job->first_error == VK_SUCCESS) {
            job->first_error = (r == VK_SUCCESS) ? VK_ERROR_UNKNOWN : r;
            job->first_error_what =
               "vkGetPhysicalDeviceSurfacePresentModesKHR";
         }
         break;
      }
      job->queries++;

      /* Somewhere under a frame, so this thread contends rather than
       * saturates. */
      svcSleepThread(MT_REFRESH_NS / 4);
   }

   return NULL;
}

/* pthread_create with a stack big enough for the driver's call depth.
 * newlib's default here is small, and a swapchain create inside a
 * worker reaches a long way down. */
static bool mt_thread_start(pthread_t *th, void *(*fn)(void *), void *arg)
{
   pthread_attr_t attr;
   if (pthread_attr_init(&attr) != 0)
      return false;
   pthread_attr_setstacksize(&attr, 256 * 1024);
   const int rc = pthread_create(th, &attr, fn, arg);
   pthread_attr_destroy(&attr);
   return rc == 0;
}

/* ------------------------------------------------------------------ */
/* Section C: a render thread and a present thread                     */
/* ------------------------------------------------------------------ */

/* The hand-off between them. A ring of image indices, bounded so the
 * render thread can never hold more images than Vulkan allows it to:
 * VkSurfaceCapabilitiesKHR::minImageCount is 2 here, so the application
 * may hold imageCount - 2 + 1 at once, and the ring is that. Exceeding
 * it is not a driver bug to find, it is invalid usage to avoid. */
typedef struct mt_pipe {
   pthread_mutex_t lock;
   pthread_cond_t not_empty;
   pthread_cond_t not_full;

   uint32_t slots[MT_MAX_IMAGES];
   uint32_t head, tail, count, capacity;

   bool render_done;      /* the render thread has produced its last */
   bool abort;            /* somebody failed; the other side must stop */
} mt_pipe;

static void mt_pipe_init(mt_pipe *p, uint32_t capacity)
{
   memset(p, 0, sizeof(*p));
   pthread_mutex_init(&p->lock, NULL);
   pthread_cond_init(&p->not_empty, NULL);
   pthread_cond_init(&p->not_full, NULL);
   p->capacity = capacity;
}

static void mt_pipe_finish(mt_pipe *p)
{
   pthread_cond_destroy(&p->not_empty);
   pthread_cond_destroy(&p->not_full);
   pthread_mutex_destroy(&p->lock);
}

/* False when the consumer has given up, so the producer stops rather
 * than acquiring images nobody will present — which would exhaust the
 * swapchain and report a timeout instead of the failure that caused it. */
static bool mt_pipe_push(mt_pipe *p, uint32_t index)
{
   pthread_mutex_lock(&p->lock);
   while (p->count == p->capacity && !p->abort)
      pthread_cond_wait(&p->not_full, &p->lock);
   const bool pushed = !p->abort;
   if (pushed) {
      p->slots[p->tail] = index;
      p->tail = (p->tail + 1u) % MT_MAX_IMAGES;
      p->count++;
   }
   pthread_cond_signal(&p->not_empty);
   pthread_mutex_unlock(&p->lock);
   return pushed;
}

/* False when there will be no more: either the producer finished or
 * somebody aborted. */
static bool mt_pipe_pop(mt_pipe *p, uint32_t *index_out)
{
   pthread_mutex_lock(&p->lock);
   while (p->count == 0 && !p->render_done && !p->abort)
      pthread_cond_wait(&p->not_empty, &p->lock);
   const bool have = p->count > 0 && !p->abort;
   if (have) {
      *index_out = p->slots[p->head];
      p->head = (p->head + 1u) % MT_MAX_IMAGES;
      p->count--;
   }
   pthread_cond_signal(&p->not_full);
   pthread_mutex_unlock(&p->lock);
   return have;
}

static void mt_pipe_close(mt_pipe *p, bool aborted)
{
   pthread_mutex_lock(&p->lock);
   p->render_done = true;
   if (aborted)
      p->abort = true;
   pthread_cond_broadcast(&p->not_empty);
   pthread_cond_broadcast(&p->not_full);
   pthread_mutex_unlock(&p->lock);
}

typedef struct mt_cd_job {
   mt_ctx *c;
   mt_sc *sc;
   mt_pipe *pipe;
   uint32_t frames;
   int core;
   int core_got;

   uint32_t done;
   VkResult first_error;
   uint32_t error_frame;
   const char *first_error_what;
} mt_cd_job;

static void *mt_render_thread(void *arg)
{
   mt_cd_job *job = arg;
   job->core_got = mt_pin_self(job->core);
   job->first_error = VK_SUCCESS;

   for (uint32_t i = 0; i < job->frames; i++) {
      uint32_t index = 0;
      VkResult r = mt_acquire(job->c, job->sc, &index);
      if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
         job->first_error = r;
         job->error_frame = i;
         job->first_error_what = "vkAcquireNextImageKHR";
         break;
      }
      r = mt_record_submit(job->c, job->sc, index, i);
      if (r != VK_SUCCESS) {
         job->first_error = r;
         job->error_frame = i;
         job->first_error_what = "record and submit";
         break;
      }
      if (!mt_pipe_push(job->pipe, index))
         break;   /* the present thread failed; it will say why */
      job->done++;
   }

   mt_pipe_close(job->pipe, job->first_error != VK_SUCCESS);
   return NULL;
}

static void *mt_presenter_thread(void *arg)
{
   mt_cd_job *job = arg;
   job->core_got = mt_pin_self(job->core);
   job->first_error = VK_SUCCESS;

   uint32_t index = 0;
   while (mt_pipe_pop(job->pipe, &index)) {
      VkResult r = mt_present(job->c, job->sc, index);
      if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
         job->first_error = r;
         job->error_frame = job->done;
         job->first_error_what = "vkQueuePresentKHR";
         mt_pipe_close(job->pipe, true);
         break;
      }
      job->done++;
   }

   return NULL;
}

/* ------------------------------------------------------------------ */
/* The test                                                            */
/* ------------------------------------------------------------------ */

static void mt_force_copy(bool on)
{
   setenv("MESA_VK_WSI_HORIZON_FORCE_COPY", on ? "1" : "0", 1);
}

/* Section A, run once per present path.
 *
 * create S1 -> present -> create S2 with oldSwapchain = S1 -> S1 must
 * refuse -> destroy S1 -> S2 must keep presenting.
 *
 * The last step is the check. Destroying a superseded copy-fallback
 * swapchain used to disconnect the window's producer, and the swapchain
 * that had taken the window was the one holding it. */
static void mt_section_a(mt_ctx *c, bool force_copy, const char *what)
{
   test_ctx *t = c->fw->t;
   mt_sc s1, s2;
   VkResult err = VK_SUCCESS;

   mt_force_copy(force_copy);
   vkfw_forget_messages(c->fw);

   /* Every exit from here on restores the override. Sections C and E
    * never set it themselves, so whatever this one leaves behind is what
    * they would run under. */
   if (!t_check(t, mt_sc_create(c, 3, VK_PRESENT_MODE_FIFO_KHR,
                                VK_NULL_HANDLE, &s1, &err),
                "A/%s: the first swapchain was created -> %s", what,
                vkfw_result_str(err))) {
      mt_force_copy(false);
      return;
   }
   t_check(t, s1.zero_copy != force_copy,
           "A/%s: it took the %s path, which is the one this case is "
           "about", what, force_copy ? "copy" : "zero-copy");

   mt_run r1;
   mt_run_frames(c, &s1, MT_A_FRAMES, 0, &r1);
   t_check(t, r1.presented == MT_A_FRAMES,
           "A/%s: the first swapchain presented %" PRIu32 " of %u frames "
           "-> %s", what, r1.presented, MT_A_FRAMES,
           vkfw_result_str(r1.first_error));

   /* The recreate, the way Vulkan describes it: the new swapchain names
    * the old one and is created before it is destroyed. */
   vkfw_forget_messages(c->fw);
   if (!t_check(t, mt_sc_create(c, 3, VK_PRESENT_MODE_FIFO_KHR, s1.handle,
                                &s2, &err),
                "A/%s: the second swapchain was created with the first as "
                "oldSwapchain -> %s", what, vkfw_result_str(err))) {
      mt_sc_destroy(c, &s1);
      mt_force_copy(false);
      return;
   }

   /* The retired one must refuse. Its images are still valid handles
    * until it is destroyed, so this is a present that the driver has to
    * turn down rather than one the application may not make. */
   uint32_t idx = 0;
   VkResult ra = mt_acquire(c, &s1, &idx);
   t_check(t, ra == VK_ERROR_OUT_OF_DATE_KHR,
           "A/%s: an acquire on the retired swapchain -> %s (expected "
           "VK_ERROR_OUT_OF_DATE_KHR)", what, vkfw_result_str(ra));

   /* THE STEP THAT USED TO BREAK THE SURVIVOR. */
   mt_sc_destroy(c, &s1);

   mt_run r2;
   mt_run_frames(c, &s2, MT_A_FRAMES, 1000, &r2);
   t_check(t, r2.presented == MT_A_FRAMES,
           "A/%s: after the retired swapchain was destroyed the survivor "
           "presented %" PRIu32 " of %u frames -> %s", what, r2.presented,
           MT_A_FRAMES, vkfw_result_str(r2.first_error));

   mt_sc_destroy(c, &s2);
   mt_force_copy(false);
}

/* Section B: the same sequence with the destroy on its own thread,
 * running while the survivor presents. */
static void mt_section_b(mt_ctx *c, bool force_copy, const char *what)
{
   test_ctx *t = c->fw->t;
   uint32_t generations_ok = 0;
   uint32_t frames_ok = 0;
   VkResult first_error = VK_SUCCESS;
   uint32_t reaper_core = 0;
   bool reaper_pinned = false;

   mt_force_copy(force_copy);

   mt_sc *cur = calloc(1, sizeof(*cur));
   if (!t_check(t, cur != NULL, "B/%s: allocation", what)) {
      mt_force_copy(false);
      return;
   }

   VkResult err = VK_SUCCESS;
   if (!t_check(t, mt_sc_create(c, 3, VK_PRESENT_MODE_FIFO_KHR, VK_NULL_HANDLE,
                                cur, &err),
                "B/%s: the first swapchain -> %s", what,
                vkfw_result_str(err))) {
      free(cur);
      mt_force_copy(false);
      return;
   }

   for (uint32_t gen = 0; gen < MT_B_GENS; gen++) {
      mt_sc *next = calloc(1, sizeof(*next));
      if (next == NULL) {
         first_error = VK_ERROR_OUT_OF_HOST_MEMORY;
         break;
      }

      if (!mt_sc_create(c, 3, VK_PRESENT_MODE_FIFO_KHR, cur->handle, next,
                        &err)) {
         first_error = err;
         free(next);
         break;
      }

      /* The old one is destroyed by another thread while this one
       * presents on the new one. vkDestroySwapchainKHR externally
       * synchronises only the swapchain it is given, and these are two
       * different swapchains, so no lock connects them — which is
       * exactly the claim being tested. */
      mt_reaper_job reap = {
         .c = c, .sc = cur,
         /* Half a frame in, so the destroy lands inside the present
          * loop rather than before it starts. */
         .delay_ns = MT_REFRESH_NS / 2,
         .core = 2,
         .core_got = -1,
      };
      pthread_t th;
      if (!mt_thread_start(&th, mt_reaper_thread, &reap)) {
         first_error = VK_ERROR_INITIALIZATION_FAILED;
         mt_sc_destroy(c, next);
         free(next);
         break;
      }

      mt_run run;
      mt_run_frames(c, next, MT_B_FRAMES, gen * 100u, &run);
      pthread_join(th, NULL);

      if (reap.core_got >= 0) {
         reaper_core = (uint32_t)reap.core_got;
         reaper_pinned = true;
      }

      free(cur);
      cur = next;

      frames_ok += run.presented;
      if (run.presented != MT_B_FRAMES) {
         first_error = run.first_error;
         break;
      }
      generations_ok++;
   }

   mt_sc_destroy(c, cur);
   free(cur);
   mt_force_copy(false);

   t_check(t, generations_ok == MT_B_GENS && first_error == VK_SUCCESS,
           "B/%s: %" PRIu32 " of %u generations presented %" PRIu32 " of %u "
           "frames each while the retired swapchain was destroyed on "
           "another thread -> %s", what, generations_ok, MT_B_GENS,
           frames_ok, MT_B_GENS * MT_B_FRAMES, vkfw_result_str(first_error));
   if (reaper_pinned)
      t_note(t, "B/%s: the destroying thread ran on core %" PRIu32, what,
             reaper_core);
   else
      t_note(t, "B/%s: the destroying thread could not be pinned; it shared "
                "a core with the presenting one", what);
}

/* Section C: a render thread and a present thread on one swapchain. */
static void mt_section_c(mt_ctx *c)
{
   test_ctx *t = c->fw->t;
   mt_sc sc;
   VkResult err = VK_SUCCESS;

   if (!t_check(t, mt_sc_create(c, 3, VK_PRESENT_MODE_FIFO_KHR, VK_NULL_HANDLE,
                                &sc, &err),
                "C: the swapchain -> %s", vkfw_result_str(err)))
      return;

   /* minImageCount is 2, so the application may hold imageCount - 1
    * images at once. The ring is that, and it is what keeps the render
    * thread inside valid usage rather than discovering the acquire's
    * deadlock detector. */
   mt_pipe pipe;
   mt_pipe_init(&pipe, sc.image_count - 1u);

   mt_cd_job render = { .c = c, .sc = &sc, .pipe = &pipe,
                        .frames = MT_C_FRAMES, .core = 1, .core_got = -1 };
   mt_cd_job present = { .c = c, .sc = &sc, .pipe = &pipe,
                         .frames = MT_C_FRAMES, .core = 2, .core_got = -1 };

   const u64 start = armGetSystemTick();

   pthread_t th_r, th_p;
   bool started_r = mt_thread_start(&th_r, mt_render_thread, &render);
   bool started_p = false;
   if (started_r)
      started_p = mt_thread_start(&th_p, mt_presenter_thread, &present);

   if (!started_p && started_r)
      mt_pipe_close(&pipe, true);

   if (started_r)
      pthread_join(th_r, NULL);
   if (started_p)
      pthread_join(th_p, NULL);

   const uint64_t elapsed_ns = armTicksToNs(armGetSystemTick() - start);

   if (!t_check(t, started_r && started_p,
                "C: a render thread and a present thread were started")) {
      mt_pipe_finish(&pipe);
      mt_sc_destroy(c, &sc);
      return;
   }

   t_check(t, render.first_error == VK_SUCCESS &&
           render.done == MT_C_FRAMES,
           "C: the render thread acquired and submitted %" PRIu32 " of %u "
           "frames -> %s%s%s", render.done, MT_C_FRAMES,
           vkfw_result_str(render.first_error),
           render.first_error_what ? " at " : "",
           render.first_error_what ? render.first_error_what : "");

   t_check(t, present.first_error == VK_SUCCESS &&
           present.done == MT_C_FRAMES,
           "C: the present thread presented %" PRIu32 " of %u frames -> %s",
           present.done, MT_C_FRAMES, vkfw_result_str(present.first_error));

   if (present.done > 1) {
      const uint64_t mean_ns = elapsed_ns / present.done;
      t_note(t, "C: %" PRIu32 " frames in %" PRIu64 " us, mean %" PRIu64
                " us per frame (a refresh is %" PRIu64 " us)",
             present.done, elapsed_ns / 1000u, mean_ns / 1000u,
             MT_REFRESH_NS / 1000u);
      /* Two threads must not be slower than one: the pipeline exists to
       * overlap the render with the present, and a mean far over a
       * refresh would mean they are serialising on something. Generous,
       * because this is a correctness test and the console is doing
       * other things. */
      t_check(t, mean_ns < 3u * MT_REFRESH_NS,
              "C: the split-thread pipeline keeps up with the display "
              "(mean %" PRIu64 " us against a %" PRIu64 " us refresh)",
              mean_ns / 1000u, MT_REFRESH_NS / 1000u);
   }

   t_note(t, "C: render thread on core %d, present thread on core %d "
             "(-1 means it could not be pinned and shares main's)",
          render.core_got, present.core_got);

   mt_pipe_finish(&pipe);
   mt_sc_destroy(c, &sc);
}

/* Section D: recreation churn, generation n destroyed by a reaper while
 * generation n+1 presents, image count alternating. */
static void mt_section_d(mt_ctx *c)
{
   test_ctx *t = c->fw->t;
   uint32_t generations_ok = 0;
   uint32_t frames = 0;
   VkResult first_error = VK_SUCCESS;
   const char *first_what = NULL;

   mt_sc *cur = calloc(1, sizeof(*cur));
   VkResult err = VK_SUCCESS;
   if (!t_check(t, cur != NULL && mt_sc_create(c, 2, VK_PRESENT_MODE_FIFO_KHR,
                                               VK_NULL_HANDLE, cur, &err),
                "D: the first generation -> %s", vkfw_result_str(err))) {
      free(cur);
      return;
   }

   for (uint32_t gen = 0; gen < MT_D_GENS; gen++) {
      /* Alternating between two and three images, and between the two
       * present paths, so the churn covers both registrations. */
      const uint32_t want = (gen & 1u) ? 3u : 2u;
      mt_force_copy((gen % 4u) >= 2u);

      mt_sc *next = calloc(1, sizeof(*next));
      if (next == NULL) {
         first_error = VK_ERROR_OUT_OF_HOST_MEMORY;
         first_what = "calloc";
         break;
      }
      if (!mt_sc_create(c, want, VK_PRESENT_MODE_FIFO_KHR, cur->handle, next,
                        &err)) {
         first_error = err;
         first_what = "vkCreateSwapchainKHR";
         free(next);
         break;
      }

      mt_reaper_job reap = { .c = c, .sc = cur, .delay_ns = 0,
                             .core = 2, .core_got = -1 };
      pthread_t th;
      if (!mt_thread_start(&th, mt_reaper_thread, &reap)) {
         first_error = VK_ERROR_INITIALIZATION_FAILED;
         first_what = "pthread_create";
         mt_sc_destroy(c, next);
         free(next);
         break;
      }

      mt_run run;
      mt_run_frames(c, next, MT_D_FRAMES, gen * 37u, &run);
      pthread_join(th, NULL);

      free(cur);
      cur = next;
      frames += run.presented;

      if (run.presented != MT_D_FRAMES) {
         first_error = run.first_error;
         first_what = "a frame in the new generation";
         break;
      }
      generations_ok++;
   }

   mt_sc_destroy(c, cur);
   free(cur);
   mt_force_copy(false);

   t_check(t, generations_ok == MT_D_GENS && first_error == VK_SUCCESS,
           "D: %" PRIu32 " of %u swapchain generations, %" PRIu32 " frames, "
           "each generation's predecessor destroyed on another thread while "
           "it presented -> %s%s%s", generations_ok, MT_D_GENS, frames,
           vkfw_result_str(first_error), first_what ? " at " : "",
           first_what ? first_what : "");
}

/* Section E: a present loop with unrelated device work beside it. */
static void mt_section_e(mt_ctx *c)
{
   test_ctx *t = c->fw->t;
   mt_sc sc;
   VkResult err = VK_SUCCESS;

   if (!t_check(t, mt_sc_create(c, 3, VK_PRESENT_MODE_FIFO_KHR, VK_NULL_HANDLE,
                                &sc, &err),
                "E: the swapchain -> %s", vkfw_result_str(err)))
      return;

   mt_churn_job churn = { .c = c, .stop = false, .core = 2, .core_got = -1 };
   pthread_t th;
   const bool started = mt_thread_start(&th, mt_churn_thread, &churn);

   mt_run run;
   mt_run_frames(c, &sc, MT_E_FRAMES, 0, &run);

   atomic_store(&churn.stop, true);
   if (started)
      pthread_join(th, NULL);

   t_check(t, started, "E: the device-work thread was started");
   t_check(t, run.presented == MT_E_FRAMES,
           "E: %" PRIu32 " of %u frames presented while another thread "
           "allocated, created and queried -> %s", run.presented,
           MT_E_FRAMES, vkfw_result_str(run.first_error));
   t_check(t, churn.first_error == VK_SUCCESS,
           "E: the device-work thread did %" PRIu32 " buffers, %" PRIu32
           " images and %" PRIu32 " surface queries -> %s%s%s",
           churn.buffers, churn.images, churn.queries,
           vkfw_result_str(churn.first_error),
           churn.first_error_what ? " at " : "",
           churn.first_error_what ? churn.first_error_what : "");

   mt_sc_destroy(c, &sc);
}

/* Section F: length. Everything above, kept running. */
static void mt_section_f(mt_ctx *c)
{
   test_ctx *t = c->fw->t;
   uint32_t presented = 0;
   uint32_t generations = 0;
   VkResult first_error = VK_SUCCESS;
   uint32_t error_at = 0;

   mt_churn_job churn = { .c = c, .stop = false, .core = 3, .core_got = -1 };
   pthread_t churn_th;
   const bool churn_started = mt_thread_start(&churn_th, mt_churn_thread,
                                              &churn);

   mt_sc *cur = calloc(1, sizeof(*cur));
   VkResult err = VK_SUCCESS;
   if (!t_check(t, cur != NULL && mt_sc_create(c, 3, VK_PRESENT_MODE_FIFO_KHR,
                                               VK_NULL_HANDLE, cur, &err),
                "F: the first swapchain -> %s", vkfw_result_str(err))) {
      atomic_store(&churn.stop, true);
      if (churn_started)
         pthread_join(churn_th, NULL);
      free(cur);
      return;
   }

   const u64 start = armGetSystemTick();
   uint64_t min_ns = UINT64_MAX, max_ns = 0;
   u64 prev_tick = 0;
   bool have_prev = false;

   for (uint32_t frame = 0; frame < MT_F_FRAMES; frame++) {
      if (frame != 0 && (frame % MT_F_GEN_EVERY) == 0) {
         /* A generation boundary: create the next, hand the old one to
          * a reaper, keep going. Both paths, in turn. */
         mt_force_copy(((frame / MT_F_GEN_EVERY) & 1u) != 0u);

         mt_sc *next = calloc(1, sizeof(*next));
         if (next == NULL) {
            first_error = VK_ERROR_OUT_OF_HOST_MEMORY;
            error_at = frame;
            break;
         }
         if (!mt_sc_create(c, 3, VK_PRESENT_MODE_FIFO_KHR, cur->handle, next,
                           &err)) {
            first_error = err;
            error_at = frame;
            free(next);
            break;
         }

         mt_reaper_job reap = { .c = c, .sc = cur, .delay_ns = 0,
                                .core = 2, .core_got = -1 };
         pthread_t th;
         if (mt_thread_start(&th, mt_reaper_thread, &reap)) {
            /* Joined immediately after the next frame rather than here,
             * so the destroy really does overlap a present. */
            mt_run one;
            mt_run_frames(c, next, 1, frame, &one);
            pthread_join(th, NULL);
            presented += one.presented;
            if (one.presented != 1) {
               first_error = one.first_error;
               error_at = frame;
               free(cur);
               cur = next;
               break;
            }
         } else {
            mt_sc_destroy(c, cur);
         }

         free(cur);
         cur = next;
         generations++;
         have_prev = false;
         continue;
      }

      const u64 now = armGetSystemTick();
      if (have_prev) {
         const uint64_t d = armTicksToNs(now - prev_tick);
         if (d < min_ns) min_ns = d;
         if (d > max_ns) max_ns = d;
      }
      prev_tick = now;
      have_prev = true;

      VkResult r = mt_frame(c, cur, frame);
      if (r != VK_SUCCESS) {
         first_error = r;
         error_at = frame;
         break;
      }
      presented++;
   }

   const uint64_t elapsed_ns = armTicksToNs(armGetSystemTick() - start);

   atomic_store(&churn.stop, true);
   if (churn_started)
      pthread_join(churn_th, NULL);

   mt_sc_destroy(c, cur);
   free(cur);
   mt_force_copy(false);

   t_check(t, presented == MT_F_FRAMES && first_error == VK_SUCCESS,
           "F: %" PRIu32 " of %u frames over %" PRIu32 " swapchain "
           "generations, with a device-work thread running throughout "
           "-> %s (frame %" PRIu32 ")", presented, MT_F_FRAMES,
           generations, vkfw_result_str(first_error), error_at);
   if (presented > 1) {
      t_note(t, "F: %" PRIu64 " us total, mean %" PRIu64 " us per frame, "
                "shortest interval %" PRIu64 " us, longest %" PRIu64 " us",
             elapsed_ns / 1000u, (elapsed_ns / presented) / 1000u,
             min_ns == UINT64_MAX ? 0 : min_ns / 1000u, max_ns / 1000u);
   }
   /* Checked, and not merely used to guard the join: without it a
    * pthread_create that failed leaves first_error at VK_SUCCESS and the
    * check below passes on a soak that was quietly single-threaded. */
   t_check(t, churn_started, "F: the device-work thread was started");
   t_check(t, churn.first_error == VK_SUCCESS,
           "F: the device-work thread survived the soak (%" PRIu32 " buffers, "
           "%" PRIu32 " images, %" PRIu32 " queries) -> %s", churn.buffers,
           churn.images, churn.queries, vkfw_result_str(churn.first_error));
}

int run_test(test_ctx *t)
{
   vkfw fw;
   int rv = 0;
   mt_ctx ctx;
   memset(&ctx, 0, sizeof(ctx));

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

   ctx.fw = &fw;
   pthread_mutex_init(&ctx.queue_lock, NULL);
   atomic_init(&ctx.quiesce_error, (int)VK_SUCCESS);

   /* What the kernel will let this process do with cores, reported
    * rather than assumed: it decides whether the sections below are
    * parallel or merely interleaved. */
   {
      u64 allowed = 0;
      Result rc = svcGetInfo(&allowed, InfoType_CoreMask, CUR_PROCESS_HANDLE,
                             0);
      if (R_SUCCEEDED(rc)) {
         t_note(t, "the process may run on core mask 0x%" PRIx64
                   " (%d core(s))", (uint64_t)allowed,
                __builtin_popcountll(allowed));
      } else {
         t_note(t, "svcGetInfo(CoreMask) failed: 0x%08x — threads stay "
                   "wherever the scheduler puts them", rc);
      }
   }

   NWindow *win = nwindowGetDefault();
   if (!t_check(t, win != NULL && nwindowIsValid(win),
                "nwindowGetDefault() returned a valid window")) {
      rv = 1;
      goto out_lock;
   }

   const VkViSurfaceCreateInfoNN sci = {
      .sType = VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN,
      .window = win,
   };
   VkResult r = fw.wsi.vkCreateViSurfaceNN(fw.instance, &sci, NULL,
                                           &ctx.surface);
   if (!t_check(t, r == VK_SUCCESS && ctx.surface != VK_NULL_HANDLE,
                "vkCreateViSurfaceNN over the default window -> %s",
                vkfw_result_str(r))) {
      rv = 1;
      goto out_lock;
   }

   VkSurfaceCapabilitiesKHR caps;
   memset(&caps, 0, sizeof(caps));
   r = fw.wsi.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(fw.pdev, ctx.surface,
                                                        &caps);
   if (!t_check(t, r == VK_SUCCESS, "surface capabilities -> %s",
                vkfw_result_str(r))) {
      rv = 1;
      goto out_surface;
   }
   ctx.extent = caps.currentExtent;
   ctx.format = VK_FORMAT_R8G8B8A8_UNORM;

   t_note(t, "surface: %" PRIu32 "x%" PRIu32 ", %" PRIu32 " to %" PRIu32
             " images", ctx.extent.width, ctx.extent.height,
          caps.minImageCount, caps.maxImageCount);

   /* --- A: recreate then destroy the old one, both paths ----------- */
   mt_section_a(&ctx, false, "zero-copy");
   mt_section_a(&ctx, true,  "copy");

   /* --- B: the same, with the destroy on another thread ------------ */
   mt_section_b(&ctx, false, "zero-copy");
   mt_section_b(&ctx, true,  "copy");

   /* --- C: a render thread and a present thread -------------------- */
   mt_section_c(&ctx);

   /* --- D: recreation churn with a reaper -------------------------- */
   mt_section_d(&ctx);

   /* --- E: unrelated device work beside a present loop ------------- */
   mt_section_e(&ctx);

   /* --- F: length --------------------------------------------------- */
   mt_section_f(&ctx);

   /* Every teardown in the run had to get the device quiet before it
    * destroyed anything, and any that could not skipped the destruction
    * and left its result here. One check, because the failure is fatal
    * to the whole file rather than to one section. */
   {
      const VkResult q = (VkResult)atomic_load(&ctx.quiesce_error);
      t_check(t, q == VK_SUCCESS,
              "every swapchain teardown got the device quiet first -> %s",
              vkfw_result_str(q));
   }

   /* Nothing may be left holding the window: the surface is about to go
    * and the WSI asserts that no swapchain still owns it. */
   fw.vk.vkDeviceWaitIdle(fw.dev);

out_surface:
   if (ctx.surface != VK_NULL_HANDLE)
      fw.wsi.vkDestroySurfaceKHR(fw.instance, ctx.surface, NULL);
out_lock:
   pthread_mutex_destroy(&ctx.queue_lock);
out:
   vkfw_finish(&fw);
   return rv;
}
