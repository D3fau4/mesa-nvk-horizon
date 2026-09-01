/*
 * t_vk_present_draw — a presented frame that a graphics pipeline drew,
 * and how long the GPU took to draw it.
 *
 * THE HOLE THIS FILLS, AND IT IS A HOLE IN THE EVIDENCE AND NOT IN THE
 * CODE. Two sets of tests in this suite were disjoint until this file:
 *
 *   draw       t_vk_caps, t_vk_depth, t_vk_format, t_vk_texture,
 *              t_vk_triangle — all into off-screen images, the largest
 *              of them 64x64
 *   present    t_vk_swapchain, t_vk_wsi_mt, t_vk_suboptimal,
 *              t_vk_immediate — every one of whose frames is a
 *              vkCmdClearColorImage, on purpose, because a test about
 *              pacing must not be slower than the compositor
 *
 * So "a VK_KHR_swapchain presents at 89 of 89 intervals inside 10% of a
 * refresh" is measured, true, and says nothing whatever about a frame
 * with a shader in it. Nothing in this project had ever put the 3D
 * pipeline's ROP into a scanout surface — block-linear, block height 32
 * GOBs, PTE kind 0xfe, display GOB ordering — until this file.
 *
 * WHAT PROVOKED IT. A Vulkan application presenting to the default
 * window through VK_NN_vi_surface reports 157 ms per frame at 1280x720
 * with three images on the zero-copy path: 139.6 ms of it inside
 * vkAcquireNextImageKHR under IMMEDIATE, the same 157 ms total under
 * FIFO with the wait moved into its first draw, and 183 ms with
 * MESA_VK_WSI_HORIZON_FORCE_COPY=1. Its own instrumentation puts 0.6 ms
 * in command recording, 0.1 ms in vkQueueSubmit and 0.3 ms in
 * vkQueuePresentKHR. The reporter read the stability of that total as an
 * external pacer releasing buffers at 6.4 Hz.
 *
 * That reading does not survive its own fourth measurement. Under an
 * independent 157 ms pacer the copy fallback's extra work — a row copy
 * and libnx's block-linear swizzle — lands inside a wait the loop was
 * going to spend anyway, and the frame time does not move. It moved, by
 * 26 ms, which is what a serial link in a dependency chain does and not
 * what a clock does. So the ~145 ms is *in* the frame, and the one
 * quantity nobody has measured is how long the GPU spends on it.
 *
 * WHAT THIS FILE MEASURES, AND WHY IT IS THREE THINGS AND NOT ONE.
 *
 *   B  submit-to-fence, per frame, on the real scanout image. Not an
 *      off-screen copy of one: the surface a swapchain image gets is not
 *      the surface vkfw_image_create would give, and the whole question
 *      is about that surface. The loop waits on the fence before it
 *      presents, so it is not measuring pacing and does not pretend to;
 *      it is measuring what the GPU costs.
 *
 *   C  frame pacing, pipelined, with no wait of its own — the ordinary
 *      loop, and the one the numbers above came from. CLEAR, then DRAW,
 *      then CLEAR again, because a DRAW run between two clear runs is a
 *      measurement and a DRAW run on its own is an anecdote.
 *
 *   D  the same DRAW loop under IMMEDIATE, which is the reporter's exact
 *      configuration.
 *
 * THE CHECK THAT CARRIES THE ARGUMENT is in E, and it is a relation
 * between B and C rather than a threshold on either:
 *
 *      presented interval  <=  max(refresh, GPU time) + one refresh
 *
 * A frame cannot arrive faster than the display shows it and cannot
 * arrive faster than the GPU draws it; anything beyond the larger of
 * those two, plus a refresh of slack for the queue, was spent somewhere
 * else — and "somewhere else" on this path is the presentation engine.
 * So a console where the GPU takes 1 ms and the frames arrive 157 ms
 * apart FAILS this and names the WSI. A console where the GPU takes 145
 * ms PASSES it, and section B's number is then the finding. Neither
 * outcome is assumed here and both are printed.
 *
 * THE CONTROL IS CHECKED FIRST. If the clear runs in C are not paced at
 * the refresh, this console is not doing what four other tests measured
 * it doing, and nothing else in the file is interpretable. That is
 * asserted before any comparison is drawn from it.
 *
 * WHAT THE FRAME IS. One fullscreen textured triangle sampling a
 * surface-sized RGBA8 texture uploaded once — the shape of a guest-output
 * blit, which is what the reporting application does. Drawn once and
 * three times, as two separate measurements: if one draw already costs
 * what three cost, the cost is per frame; if it scales, it is fill rate.
 * The reporting application draws three times.
 *
 * The screen alternates between a colour sweep (the clear runs) and a
 * static pattern (the draw runs), so an operator can see which section
 * is running.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include "common/vkfw.h"

#include "tex_vert_fullscreen.spv.h"
#include "tex_frag_sample.spv.h"

const char *const test_name = "t_vk_present_draw";

/* This test owns the display: no console, and the SD-card log is the
 * whole record (testfw.h). */
const bool test_uses_display = true;

/* The display's nominal refresh. Nothing is asserted to be exactly this;
 * it is the unit the thresholds are expressed in, and the same value
 * wsi_horizon.c, t_vk_swapchain.c and t_vk_immediate.c use. */
#define PD_REFRESH_NS UINT64_C(16666666)

/* The reporter's configuration: three images. t_vk_swapchain has already
 * measured three images presenting clears at a mean of 16666 us, so a
 * difference found here is not the image count. */
#define PD_IMAGES      3u
#define PD_MAX_IMAGES  4u

#define PD_WAIT_NS     UINT64_C(2000000000)

/* How many draws a DRAW frame issues. One isolates the mechanism; three
 * is what the reporting application does, and the pair of them says
 * whether the cost is per frame or per pixel. */
#define PD_DRAWS_ONE   1u
#define PD_DRAWS_APP   3u

/* Frames per measured run, and the wall-clock ceiling that stops a run
 * the moment it turns out to be slow.
 *
 * BOTH, AND THE BUDGET IS NOT A DETAIL. If the defect this file exists
 * to characterise reproduces, a frame costs 157 ms — so 180 of them is
 * 28 seconds, six such runs is nearly three minutes, and a test that
 * takes three minutes to say "it is slow" is one nobody runs twice. The
 * budget cuts every run at four seconds and the report prints how many
 * frames it actually got, which is the honest form: a mean over 25
 * intervals is still a mean, and the count is beside it. */
#define PD_FRAMES          180u
#define PD_RUN_BUDGET_NS   UINT64_C(4000000000)

/* The same, for the submit-to-fence runs in B. Fewer frames because
 * every one of them is a full CPU stall on the GPU and nothing is being
 * averaged out of jitter — the quantity is the cost itself. */
#define PD_GPU_FRAMES      60u
#define PD_GPU_BUDGET_NS   UINT64_C(3000000000)

/* Below this many intervals a mean is not worth drawing a verdict from,
 * and the file says so instead of computing one. Even at 157 ms a frame,
 * the four-second budget yields 25. */
#define PD_MIN_INTERVALS   8u

/* How many of a run's intervals must land within 10% of a refresh for
 * that run to count as paced. Same figure t_vk_immediate uses for the
 * FIFO reference it measures everything else against. */
#define PD_PACED_PCT       90u

/* Slack in the section E relation, in refreshes. One: the queue holds a
 * frame for up to a refresh beyond whichever of the display and the GPU
 * set the rate, and anything past that is not queueing. */
#define PD_SLACK_REFRESHES 1u

/* ------------------------------------------------------------------ */
/* What a frame contains.                                              */
/* ------------------------------------------------------------------ */

typedef enum pd_frame_kind {
   /* vkCmdClearColorImage — the control, and the frame every other
    * presenting test in this suite uses. */
   PD_FRAME_CLEAR,
   /* A render pass and `draws` fullscreen textured triangles. */
   PD_FRAME_DRAW,
} pd_frame_kind;

static const char *pd_kind_name(pd_frame_kind kind)
{
   return kind == PD_FRAME_CLEAR ? "clear" : "draw";
}

/* ------------------------------------------------------------------ */
/* The swapchain, and the per-image resources a frame needs.           */
/* ------------------------------------------------------------------ */

typedef struct pd_frame_res {
   VkCommandBuffer cb;
   VkSemaphore render_done;
   VkFence in_flight;
   bool in_flight_valid;
} pd_frame_res;

typedef struct pd_swapchain {
   VkSwapchainKHR handle;
   uint32_t image_count;
   VkImage images[PD_MAX_IMAGES];
   VkImageView views[PD_MAX_IMAGES];
   pd_frame_res res[PD_MAX_IMAGES];
   VkFence acquire_fence;

   VkExtent2D extent;
   VkFormat format;
   VkPresentModeKHR mode;

   bool zero_copy;
} pd_swapchain;

/* Everything the draw frames need, built once and shared by every
 * swapchain generation: the pipeline's colour format is the surface's
 * and does not change between them. */
typedef struct pd_content {
   vkfw_image texture;
   VkImageView tex_view;
   VkSampler sampler;
   VkDescriptorSetLayout set_layout;
   VkDescriptorPool pool;
   VkDescriptorSet set;
   vkfw_gfx gfx;
   bool ready;
} pd_content;

/* One measured run. `gpu_*` is filled only by the section B loop, which
 * is the only one that waits on the render before presenting. */
typedef struct pd_stats {
   const char *what;
   pd_frame_kind kind;
   uint32_t draws;
   VkPresentModeKHR mode;
   uint32_t image_count;

   uint32_t frames;
   uint64_t elapsed_ns;

   uint32_t intervals;
   uint64_t total_ns;
   uint64_t min_ns;
   uint64_t max_ns;
   uint32_t within_10pct;

   uint64_t acquire_total_ns;
   uint64_t acquire_max_ns;
   uint64_t present_total_ns;

   /* Submit to fence, section B only. */
   uint32_t gpu_samples;
   uint64_t gpu_total_ns;
   uint64_t gpu_min_ns;
   uint64_t gpu_max_ns;

   uint32_t suboptimal;
   uint32_t used_mask;

   bool failed;
   uint32_t fail_frame;
   VkResult fail_result;
   const char *fail_what;
} pd_stats;

static void pd_stats_init(pd_stats *s, const char *what, const pd_swapchain *sc,
                          pd_frame_kind kind, uint32_t draws)
{
   memset(s, 0, sizeof(*s));
   s->what = what;
   s->kind = kind;
   s->draws = draws;
   s->mode = sc->mode;
   s->image_count = sc->image_count;
   s->min_ns = UINT64_MAX;
   s->gpu_min_ns = UINT64_MAX;
}

static void pd_fail(pd_stats *s, uint32_t frame, VkResult r, const char *what)
{
   if (s->failed)
      return;
   s->failed = true;
   s->fail_frame = frame;
   s->fail_result = r;
   s->fail_what = what;
}

static uint64_t pd_mean_ns(const pd_stats *s)
{
   return s->intervals != 0 ? s->total_ns / s->intervals : 0;
}

static uint64_t pd_gpu_mean_ns(const pd_stats *s)
{
   return s->gpu_samples != 0 ? s->gpu_total_ns / s->gpu_samples : 0;
}

/* ------------------------------------------------------------------ */

static void pd_barrier(vkfw *fw, VkCommandBuffer cb, VkImage image,
                       VkImageLayout from, VkImageLayout to,
                       VkAccessFlags src_access, VkAccessFlags dst_access,
                       VkPipelineStageFlags src_stage,
                       VkPipelineStageFlags dst_stage)
{
   const VkImageMemoryBarrier b = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = src_access,
      .dstAccessMask = dst_access,
      .oldLayout = from,
      .newLayout = to,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .levelCount = 1,
         .layerCount = 1,
      },
   };
   fw->vk.vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, NULL, 0, NULL,
                               1, &b);
}

/* ------------------------------------------------------------------ */
/* The texture, the sampler, the descriptor and the pipeline.          */
/* ------------------------------------------------------------------ */

/* A pattern with structure in both axes, so a frame that renders is
 * distinguishable on the screen from one that does not, and so that a
 * sampler reading the wrong place is visible rather than plausible. The
 * bytes are R, G, B, A in memory order, which is what
 * VK_FORMAT_R8G8B8A8_UNORM means. */
static uint32_t pd_texel(uint32_t x, uint32_t y)
{
   const uint32_t r = (x * 255u) / 1280u;
   const uint32_t g = (y * 255u) / 720u;
   const uint32_t b = ((x >> 5) ^ (y >> 5)) & 1u ? 0xc0u : 0x30u;
   return (0xffu << 24) | ((b & 0xffu) << 16) | ((g & 0xffu) << 8) |
          (r & 0xffu);
}

static void pd_content_destroy(vkfw *fw, pd_content *c)
{
   vkfw_gfx_destroy(fw, &c->gfx);
   if (c->pool != VK_NULL_HANDLE)
      fw->vk.vkDestroyDescriptorPool(fw->dev, c->pool, NULL);
   if (c->set_layout != VK_NULL_HANDLE)
      fw->vk.vkDestroyDescriptorSetLayout(fw->dev, c->set_layout, NULL);
   if (c->sampler != VK_NULL_HANDLE)
      fw->vk.vkDestroySampler(fw->dev, c->sampler, NULL);
   if (c->tex_view != VK_NULL_HANDLE)
      fw->vk.vkDestroyImageView(fw->dev, c->tex_view, NULL);
   vkfw_image_destroy(fw, &c->texture);
   memset(c, 0, sizeof(*c));
}

static bool pd_content_create(vkfw *fw, VkExtent2D extent, VkFormat colour,
                              pd_content *c)
{
   test_ctx *t = fw->t;
   memset(c, 0, sizeof(*c));

   const VkDeviceSize texels = (VkDeviceSize)extent.width * extent.height;
   const VkDeviceSize bytes = texels * 4u;

   if (!vkfw_image_create(fw, VK_FORMAT_R8G8B8A8_UNORM,
                          (VkExtent3D){ extent.width, extent.height, 1 }, 1, 1,
                          VK_IMAGE_USAGE_SAMPLED_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          VK_IMAGE_TILING_OPTIMAL, &c->texture))
      return false;

   /* Uploaded once. A guest-output blit re-uploads every frame; this
    * file is measuring the blit and not the upload, and an upload in the
    * loop would put a second unknown into every number below. */
   vkfw_buffer staging = { 0 };
   if (!vkfw_buffer_create(fw, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &staging))
      return false;
   if (!t_check(t, staging.map != NULL, "the texture's staging buffer is "
                                        "mapped")) {
      vkfw_buffer_destroy(fw, &staging);
      return false;
   }

   uint32_t *px = staging.map;
   for (uint32_t y = 0; y < extent.height; y++) {
      for (uint32_t x = 0; x < extent.width; x++)
         px[(size_t)y * extent.width + x] = pd_texel(x, y);
   }
   if (!vkfw_buffer_flush(fw, &staging)) {
      vkfw_buffer_destroy(fw, &staging);
      return false;
   }

   VkCommandBuffer cb;
   if (!vkfw_cmd_begin(fw, &cb)) {
      vkfw_buffer_destroy(fw, &staging);
      return false;
   }
   pd_barrier(fw, cb, c->texture.img, VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              0, VK_ACCESS_TRANSFER_WRITE_BIT,
              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT);
   const VkBufferImageCopy region = {
      .imageSubresource = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .layerCount = 1,
      },
      .imageExtent = { extent.width, extent.height, 1 },
   };
   fw->vk.vkCmdCopyBufferToImage(cb, staging.buf, c->texture.img,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 1, &region);
   pd_barrier(fw, cb, c->texture.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT,
              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
   const bool uploaded = vkfw_submit_and_wait(fw, cb, "uploading the texture");
   vkfw_buffer_destroy(fw, &staging);
   if (!uploaded)
      return false;

   const VkImageViewCreateInfo ivci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = c->texture.img,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .levelCount = 1,
         .layerCount = 1,
      },
   };
   VkResult r = fw->vk.vkCreateImageView(fw->dev, &ivci, NULL, &c->tex_view);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateImageView(texture) -> %s",
                vkfw_result_str(r)))
      return false;

   /* LINEAR, and it is deliberate rather than incidental: a guest-output
    * blit filters, and a nearest sampler over a 1:1 mapping would let
    * the texture unit take a path a real one does not. */
   const VkSamplerCreateInfo sci = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR,
      .minFilter = VK_FILTER_LINEAR,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
   };
   r = fw->vk.vkCreateSampler(fw->dev, &sci, NULL, &c->sampler);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateSampler(linear) -> %s",
                vkfw_result_str(r)))
      return false;

   const VkDescriptorSetLayoutBinding binding = {
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
   };
   const VkDescriptorSetLayoutCreateInfo dslci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = &binding,
   };
   r = fw->vk.vkCreateDescriptorSetLayout(fw->dev, &dslci, NULL,
                                          &c->set_layout);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateDescriptorSetLayout -> %s",
                vkfw_result_str(r)))
      return false;

   const VkDescriptorPoolSize psize = {
      .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .descriptorCount = 1,
   };
   const VkDescriptorPoolCreateInfo dpci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 1,
      .pPoolSizes = &psize,
   };
   r = fw->vk.vkCreateDescriptorPool(fw->dev, &dpci, NULL, &c->pool);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateDescriptorPool -> %s",
                vkfw_result_str(r)))
      return false;

   const VkDescriptorSetAllocateInfo dsai = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = c->pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &c->set_layout,
   };
   r = fw->vk.vkAllocateDescriptorSets(fw->dev, &dsai, &c->set);
   if (!t_check(t, r == VK_SUCCESS, "vkAllocateDescriptorSets -> %s",
                vkfw_result_str(r)))
      return false;

   /* Written once, before anything is submitted, and never again. A
    * descriptor set rewritten while a submit that uses it is in flight
    * is invalid usage nothing here would report. */
   const VkDescriptorImageInfo dii = {
      .sampler = c->sampler,
      .imageView = c->tex_view,
      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
   };
   const VkWriteDescriptorSet write = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = c->set,
      .dstBinding = 0,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .pImageInfo = &dii,
   };
   fw->vk.vkUpdateDescriptorSets(fw->dev, 1, &write, 0, NULL);

   const vkfw_gfx_desc desc = {
      .vs_spv = tex_vert_fullscreen_spv,
      .vs_B = sizeof(tex_vert_fullscreen_spv),
      .fs_spv = tex_frag_sample_spv,
      .fs_B = sizeof(tex_frag_sample_spv),
      .colour_format = colour,
      .depth_format = VK_FORMAT_UNDEFINED,
      .set_layout_count = 1,
      .set_layouts = &c->set_layout,
      .width = extent.width,
      .height = extent.height,
   };
   if (!vkfw_gfx_create(fw, "the fullscreen blit", &desc, &c->gfx))
      return false;

   c->ready = true;
   t_note(t, "the draw frames sample a %" PRIu32 "x%" PRIu32
             " RGBA8 texture (%" PRIu64 " KiB), uploaded once, through a "
             "linear sampler", extent.width, extent.height,
          (uint64_t)bytes / 1024u);
   return true;
}

/* ------------------------------------------------------------------ */
/* The swapchain.                                                      */
/* ------------------------------------------------------------------ */

static void pd_destroy(vkfw *fw, pd_swapchain *sc)
{
   for (uint32_t i = 0; i < sc->image_count; i++) {
      if (sc->views[i] != VK_NULL_HANDLE)
         fw->vk.vkDestroyImageView(fw->dev, sc->views[i], NULL);
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

/* The in-flight fence says the render retired and says nothing about a
 * vkQueuePresentKHR still waiting on render_done[i], which
 * vkDestroySemaphore requires to have completed. */
static VkResult pd_quiesce(vkfw *fw)
{
   const VkResult r = fw->vk.vkQueueWaitIdle(fw->queue);
   if (r != VK_SUCCESS)
      return r;
   return fw->vk.vkDeviceWaitIdle(fw->dev);
}

static bool pd_create(vkfw *fw, VkSurfaceKHR surface, VkFormat format,
                      VkExtent2D extent, VkPresentModeKHR mode,
                      const char *what, pd_swapchain *sc)
{
   test_ctx *t = fw->t;
   memset(sc, 0, sizeof(*sc));
   sc->extent = extent;
   sc->format = format;
   sc->mode = mode;

   vkfw_forget_messages(fw);

   /* COLOR_ATTACHMENT because these images are rendered into, which is
    * the point of this file, and TRANSFER_DST because the clear runs
    * still clear them. */
   const VkSwapchainCreateInfoKHR ci = {
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .surface = surface,
      .minImageCount = PD_IMAGES,
      .imageFormat = format,
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
   };

   VkResult r = fw->wsi.vkCreateSwapchainKHR(fw->dev, &ci, NULL, &sc->handle);
   if (!t_check(t, r == VK_SUCCESS, "%s: vkCreateSwapchainKHR -> %s", what,
                vkfw_result_str(r)))
      return false;

   sc->zero_copy = !vkfw_saw_message(fw, "copy fallback: the rendered image",
                                     NULL);

   uint32_t count = 0;
   r = fw->wsi.vkGetSwapchainImagesKHR(fw->dev, sc->handle, &count, NULL);
   if (!t_check(t, r == VK_SUCCESS && count >= 2 && count <= PD_MAX_IMAGES,
                "%s: the swapchain has %" PRIu32 " image(s) -> %s", what,
                count, vkfw_result_str(r)))
      goto fail;
   r = fw->wsi.vkGetSwapchainImagesKHR(fw->dev, sc->handle, &count, sc->images);
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
      const VkImageViewCreateInfo ivci = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = sc->images[i],
         .viewType = VK_IMAGE_VIEW_TYPE_2D,
         .format = format,
         .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
         },
      };
      r = fw->vk.vkCreateImageView(fw->dev, &ivci, NULL, &sc->views[i]);
      if (!t_check(t, r == VK_SUCCESS, "%s: image view %" PRIu32 " -> %s",
                   what, i, vkfw_result_str(r)))
         goto fail;

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
      if (!t_check(t, r == VK_SUCCESS, "%s: semaphore %" PRIu32 " -> %s", what,
                   i, vkfw_result_str(r)))
         goto fail;

      r = fw->vk.vkCreateFence(fw->dev, &fci, NULL, &sc->res[i].in_flight);
      if (!t_check(t, r == VK_SUCCESS, "%s: fence %" PRIu32 " -> %s", what, i,
                   vkfw_result_str(r)))
         goto fail;
   }

   t_note(t, "%s: %" PRIu32 " image(s), %" PRIu32 "x%" PRIu32 ", %s, %s", what,
          sc->image_count, extent.width, extent.height,
          mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE" : "FIFO",
          sc->zero_copy ? "zero-copy" : "copy fallback");
   return true;

fail:
   pd_destroy(fw, sc);
   return false;
}

/* ------------------------------------------------------------------ */
/* One frame.                                                          */
/* ------------------------------------------------------------------ */

static void pd_clear_colour(uint32_t frame, VkClearColorValue *out)
{
   out->float32[0] = (float)((frame * 3u) & 0xffu) / 255.0f;
   out->float32[1] = (float)((frame * 5u + 85u) & 0xffu) / 255.0f;
   out->float32[2] = (float)((frame * 7u + 170u) & 0xffu) / 255.0f;
   out->float32[3] = 1.0f;
}

static VkResult pd_record_and_submit(vkfw *fw, pd_swapchain *sc,
                                     const pd_content *content,
                                     pd_frame_kind kind, uint32_t draws,
                                     uint32_t index, uint32_t frame)
{
   pd_frame_res *res = &sc->res[index];
   VkResult r;

   if (res->in_flight_valid) {
      /* This slot's previous submit has to be finished before its
       * command buffer is recorded again. The application's own
       * throttle, and the one CPU wait Vulkan puts here. */
      r = fw->vk.vkWaitForFences(fw->dev, 1, &res->in_flight, VK_TRUE,
                                 PD_WAIT_NS);
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

   /* UNDEFINED as the old layout: every pixel is about to be written, so
    * there is nothing in the image worth preserving. */
   if (kind == PD_FRAME_CLEAR) {
      pd_barrier(fw, res->cb, sc->images[index], VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT);

      VkClearColorValue colour;
      pd_clear_colour(frame, &colour);
      const VkImageSubresourceRange range = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .levelCount = 1,
         .layerCount = 1,
      };
      fw->vk.vkCmdClearColorImage(res->cb, sc->images[index],
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  &colour, 1, &range);

      pd_barrier(fw, res->cb, sc->images[index],
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                 VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
   } else {
      pd_barrier(fw, res->cb, sc->images[index], VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

      /* LOAD_OP_CLEAR and not DONT_CARE: the draw covers the target
       * completely, so the clear is invisible in the result — and it is
       * what a real frame does, which is the shape being measured. */
      VkClearValue clear;
      memset(&clear, 0, sizeof(clear));
      clear.color.float32[3] = 1.0f;

      const VkRenderingAttachmentInfo colour = {
         .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
         .imageView = sc->views[index],
         .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .clearValue = clear,
      };
      const VkRenderingInfo ri = {
         .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
         .renderArea = { .offset = { 0, 0 }, .extent = sc->extent },
         .layerCount = 1,
         .colorAttachmentCount = 1,
         .pColorAttachments = &colour,
      };

      fw->vk.vkCmdBeginRendering(res->cb, &ri);
      fw->vk.vkCmdBindPipeline(res->cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                               content->gfx.pipeline);
      fw->vk.vkCmdBindDescriptorSets(res->cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                     content->gfx.layout, 0, 1, &content->set,
                                     0, NULL);
      for (uint32_t d = 0; d < draws; d++)
         fw->vk.vkCmdDraw(res->cb, 3, 1, 0, 0);
      fw->vk.vkCmdEndRendering(res->cb);

      pd_barrier(fw, res->cb, sc->images[index],
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
   }

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
/* The loop.                                                           */
/* ------------------------------------------------------------------ */

/* `measure_gpu` turns this from a pacing loop into a cost loop: it waits
 * on the render's own fence between the submit and the present, which
 * destroys the overlap the pipelined loop depends on and is exactly why
 * the two are never the same run. */
static void pd_run(vkfw *fw, pd_swapchain *sc, const pd_content *content,
                   pd_frame_kind kind, uint32_t draws, uint32_t frames,
                   uint64_t budget_ns, bool measure_gpu, pd_stats *st)
{
   const u64 run_start = armGetSystemTick();
   u64 prev_tick = 0;
   bool have_prev = false;

   for (uint32_t frame = 0; frame < frames; frame++) {
      st->elapsed_ns = armTicksToNs(armGetSystemTick() - run_start);
      if (st->elapsed_ns >= budget_ns)
         break;

      const u64 acq_start = armGetSystemTick();
      uint32_t index = 0;
      VkResult r = fw->wsi.vkAcquireNextImageKHR(fw->dev, sc->handle,
                                                 PD_WAIT_NS, VK_NULL_HANDLE,
                                                 sc->acquire_fence, &index);
      if (r == VK_SUBOPTIMAL_KHR) {
         st->suboptimal++;
      } else if (r != VK_SUCCESS) {
         pd_fail(st, frame, r, "vkAcquireNextImageKHR");
         return;
      }

      /* The acquire took a reference to the fence on both success codes,
       * so every path out of this iteration has to have waited on it and
       * reset it — including the index check, which is why that comes
       * after. */
      r = fw->vk.vkWaitForFences(fw->dev, 1, &sc->acquire_fence, VK_TRUE,
                                 PD_WAIT_NS);
      if (r != VK_SUCCESS) {
         pd_fail(st, frame, r, "waiting for the acquire fence");
         return;
      }
      r = fw->vk.vkResetFences(fw->dev, 1, &sc->acquire_fence);
      if (r != VK_SUCCESS) {
         pd_fail(st, frame, r, "resetting the acquire fence");
         return;
      }

      const uint64_t acq_ns = armTicksToNs(armGetSystemTick() - acq_start);
      st->acquire_total_ns += acq_ns;
      if (acq_ns > st->acquire_max_ns)
         st->acquire_max_ns = acq_ns;

      if (index >= sc->image_count) {
         pd_fail(st, frame, VK_ERROR_UNKNOWN, "an image index out of range");
         return;
      }
      st->used_mask |= 1u << index;

      const u64 submit_start = armGetSystemTick();
      r = pd_record_and_submit(fw, sc, content, kind, draws, index, frame);
      if (r != VK_SUCCESS) {
         pd_fail(st, frame, r, "recording and submitting the frame");
         return;
      }

      if (measure_gpu) {
         r = fw->vk.vkWaitForFences(fw->dev, 1, &sc->res[index].in_flight,
                                    VK_TRUE, PD_WAIT_NS);
         if (r != VK_SUCCESS) {
            pd_fail(st, frame, r, "waiting for the render fence");
            return;
         }
         const uint64_t gpu_ns =
            armTicksToNs(armGetSystemTick() - submit_start);
         st->gpu_samples++;
         st->gpu_total_ns += gpu_ns;
         if (gpu_ns < st->gpu_min_ns)
            st->gpu_min_ns = gpu_ns;
         if (gpu_ns > st->gpu_max_ns)
            st->gpu_max_ns = gpu_ns;
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
      st->present_total_ns += armTicksToNs(armGetSystemTick() - pres_start);
      if (r == VK_SUBOPTIMAL_KHR) {
         st->suboptimal++;
      } else if (r != VK_SUCCESS) {
         pd_fail(st, frame, r, "vkQueuePresentKHR");
         return;
      }
      st->frames++;

      const u64 now = armGetSystemTick();
      if (have_prev) {
         const uint64_t dt = armTicksToNs(now - prev_tick);
         st->total_ns += dt;
         if (dt < st->min_ns)
            st->min_ns = dt;
         if (dt > st->max_ns)
            st->max_ns = dt;
         if (dt >= PD_REFRESH_NS - PD_REFRESH_NS / 10 &&
             dt <= PD_REFRESH_NS + PD_REFRESH_NS / 10)
            st->within_10pct++;
         st->intervals++;
      }
      prev_tick = now;
      have_prev = true;
   }
   st->elapsed_ns = armTicksToNs(armGetSystemTick() - run_start);
}

/* The numbers, always. A verdict is a separate call, because a run that
 * did not complete has nothing worth judging. */
static bool pd_report(test_ctx *t, const pd_stats *s)
{
   bool ok = t_check(t, !s->failed && s->frames > 0,
                     "%s: %" PRIu32 " frame(s) presented in %" PRIu64 " ms%s",
                     s->what, s->frames, s->elapsed_ns / 1000000u,
                     s->failed ? " (see the note below)" : "");
   if (s->failed) {
      t_note(t, "%s: %s at frame %" PRIu32 " -> %s", s->what, s->fail_what,
             s->fail_frame, vkfw_result_str(s->fail_result));
   }

   t_note(t, "%s [%s x%" PRIu32 ", %s, %" PRIu32 " images]: interval mean %"
             PRIu64 " us over %" PRIu32 " (min %" PRIu64 ", max %" PRIu64
             "), %" PRIu32 " within 10%% of a refresh; acquire mean %" PRIu64
             " us (max %" PRIu64 "), present mean %" PRIu64 " us; %" PRIu32
             " SUBOPTIMAL",
          s->what, pd_kind_name(s->kind), s->draws,
          s->mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE" : "FIFO",
          s->image_count, pd_mean_ns(s) / 1000u, s->intervals,
          s->min_ns == UINT64_MAX ? 0 : s->min_ns / 1000u, s->max_ns / 1000u,
          s->within_10pct,
          s->frames != 0 ? s->acquire_total_ns / s->frames / 1000u : 0,
          s->acquire_max_ns / 1000u,
          s->frames != 0 ? s->present_total_ns / s->frames / 1000u : 0,
          s->suboptimal);

   if (s->gpu_samples != 0) {
      t_note(t, "%s: submit to fence, mean %" PRIu64 " us over %" PRIu32
                " (min %" PRIu64 ", max %" PRIu64 ")", s->what,
             pd_gpu_mean_ns(s) / 1000u, s->gpu_samples,
             s->gpu_min_ns == UINT64_MAX ? 0 : s->gpu_min_ns / 1000u,
             s->gpu_max_ns / 1000u);
   }

   /* Every image must have been used. A driver that had retained one
    * would present through fewer slots and nothing else would say so. */
   if (s->frames >= s->image_count) {
      const uint32_t all = (1u << s->image_count) - 1u;
      ok = t_check(t, s->used_mask == all,
                   "%s: every one of the %" PRIu32 " images was acquired "
                   "(mask 0x%x)", s->what, s->image_count, s->used_mask) && ok;
   }
   return ok;
}

/* Runs one measured loop on a swapchain of its own, so no run inherits
 * the queue state of the one before it. */
static bool pd_measure(vkfw *fw, VkSurfaceKHR surface, const pd_content *content,
                       VkFormat format, VkExtent2D extent,
                       VkPresentModeKHR mode, pd_frame_kind kind,
                       uint32_t draws, uint32_t frames, uint64_t budget_ns,
                       bool measure_gpu, const char *what, pd_stats *out)
{
   pd_swapchain sc;
   if (!pd_create(fw, surface, format, extent, mode, what, &sc))
      return false;

   pd_stats_init(out, what, &sc, kind, draws);
   pd_run(fw, &sc, content, kind, draws, frames, budget_ns, measure_gpu, out);
   const bool ok = pd_report(fw->t, out);

   const VkResult q = pd_quiesce(fw);
   t_check(fw->t, q == VK_SUCCESS, "%s: the device is idle before teardown "
                                   "-> %s", what, vkfw_result_str(q));
   pd_destroy(fw, &sc);
   return ok;
}

/* ------------------------------------------------------------------ */

int run_test(test_ctx *t)
{
   VkPhysicalDeviceVulkan13Features features13 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
      .dynamicRendering = VK_TRUE,
   };

   vkfw fw;
   int rv = 0;
   VkSurfaceKHR surface = VK_NULL_HANDLE;
   pd_content content = { 0 };

   const char *const instance_exts[] = {
      VK_KHR_SURFACE_EXTENSION_NAME,
      VK_NN_VI_SURFACE_EXTENSION_NAME,
   };
   const char *const device_exts[] = {
      VK_KHR_SWAPCHAIN_EXTENSION_NAME,
   };

   if (!vkfw_init_full(&fw, t, &features13, instance_exts, 2, device_exts, 1)) {
      t_note(t, "if this failed with EXTENSION_NOT_PRESENT, the build has no "
                "VI surface: -Dplatforms=vi is what creates it");
      return 1;
   }

   if (!vkfw_wsi_load(&fw)) {
      rv = 1;
      goto out;
   }

   /* Whatever an earlier test left set must not decide this one's path. */
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

   /* ---------------------------------------------------------------- */
   /* A — the surface, the format and the content.                     */
   /* ---------------------------------------------------------------- */

   VkSurfaceCapabilitiesKHR caps;
   memset(&caps, 0, sizeof(caps));
   r = fw.wsi.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(fw.pdev, surface,
                                                        &caps);
   if (!t_check(t, r == VK_SUCCESS, "A: surface capabilities -> %s",
                vkfw_result_str(r))) {
      rv = 1;
      goto out_surface;
   }
   const VkExtent2D extent = caps.currentExtent;
   t_note(t, "A: the surface is %" PRIu32 "x%" PRIu32 ", image count %" PRIu32
             "..%" PRIu32 "; operation mode %d (0 handheld, 1 docked)",
          extent.width, extent.height, caps.minImageCount, caps.maxImageCount,
          (int)appletGetOperationMode());
   if (!t_check(t, extent.width > 0 && extent.height > 0 &&
                extent.width != UINT32_MAX,
                "A: the surface's currentExtent is a real size")) {
      rv = 1;
      goto out_surface;
   }

   /* B8G8R8A8_UNORM by preference, because that is the format the
    * reporting application uses (VkFormat 44) and matching it is the
    * point; R8G8B8A8_UNORM is the fallback every other test here uses,
    * and which one was taken is printed rather than assumed. */
   uint32_t fmt_count = 0;
   r = fw.wsi.vkGetPhysicalDeviceSurfaceFormatsKHR(fw.pdev, surface,
                                                   &fmt_count, NULL);
   VkSurfaceFormatKHR formats[8];
   uint32_t got = fmt_count < 8 ? fmt_count : 8;
   if (r == VK_SUCCESS && got > 0) {
      r = fw.wsi.vkGetPhysicalDeviceSurfaceFormatsKHR(fw.pdev, surface, &got,
                                                      formats);
   } else {
      got = 0;
   }
   if (!t_check(t, (r == VK_SUCCESS || r == VK_INCOMPLETE) && got > 0,
                "A: the surface offers %" PRIu32 " format(s) -> %s", got,
                vkfw_result_str(r))) {
      rv = 1;
      goto out_surface;
   }

   VkFormat format = formats[0].format;
   for (uint32_t i = 0; i < got; i++) {
      if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
         format = VK_FORMAT_B8G8R8A8_UNORM;
         break;
      }
   }
   t_note(t, "A: presenting in VkFormat %d (%s)", (int)format,
          format == VK_FORMAT_B8G8R8A8_UNORM ? "B8G8R8A8_UNORM, the one the "
                                               "report was taken in"
                                             : "not B8G8R8A8_UNORM");

   if (!pd_content_create(&fw, extent, format, &content)) {
      t_check(t, false, "A: the texture and the blit pipeline were built");
      rv = 1;
      goto out_content;
   }
   t_check(t, content.ready, "A: the texture and the blit pipeline were built");

   /* ---------------------------------------------------------------- */
   /* B — what the GPU costs, on the scanout image itself.             */
   /* ---------------------------------------------------------------- */

   pd_stats gpu_clear, gpu_draw1, gpu_draw3;
   pd_measure(&fw, surface, &content, format, extent, VK_PRESENT_MODE_FIFO_KHR,
              PD_FRAME_CLEAR, 0, PD_GPU_FRAMES, PD_GPU_BUDGET_NS, true,
              "B1 GPU cost, clear", &gpu_clear);
   pd_measure(&fw, surface, &content, format, extent, VK_PRESENT_MODE_FIFO_KHR,
              PD_FRAME_DRAW, PD_DRAWS_ONE, PD_GPU_FRAMES, PD_GPU_BUDGET_NS,
              true, "B2 GPU cost, one draw", &gpu_draw1);
   pd_measure(&fw, surface, &content, format, extent, VK_PRESENT_MODE_FIFO_KHR,
              PD_FRAME_DRAW, PD_DRAWS_APP, PD_GPU_FRAMES, PD_GPU_BUDGET_NS,
              true, "B3 GPU cost, three draws", &gpu_draw3);

   /* HOW MUCH OF A DRAWN FRAME IS THE DRAWING, and the comparison has to
    * be against the clear or it answers the wrong question. Run 26 is
    * why this is spelled out: clear 1193 us, one draw 1814 us, three
    * draws 3022 us. Comparing 3022 against 1814 says "it barely
    * doubled", which is arithmetically true and means nothing — the
    * fixed part of a frame is in both numbers. Subtract it and the
    * drawing is 621 us and 1829 us, i.e. almost exactly three times one
    * draw, and the frame is 1193 us of fixed cost plus 610 us a draw.
    *
    * So the per-draw cost is what is reported, and the baseline it was
    * taken against is printed beside it. Nothing is asserted: this file
    * does not know what the right number is, and a threshold invented
    * here would be the wrong way to find out. */
   if (gpu_draw1.gpu_samples != 0 && gpu_draw3.gpu_samples != 0 &&
       gpu_clear.gpu_samples != 0) {
      const uint64_t base = pd_gpu_mean_ns(&gpu_clear);
      const uint64_t one = pd_gpu_mean_ns(&gpu_draw1);
      const uint64_t three = pd_gpu_mean_ns(&gpu_draw3);
      /* A draw cannot cost less than nothing; a clear that came out
       * dearer than a drawn frame means the two runs are not comparable
       * and the subtraction is not performed on that reading. */
      const uint64_t one_draw = one > base ? one - base : 0;
      const uint64_t three_draws = three > base ? three - base : 0;
      t_note(t, "B: over a %" PRIu64 " us clear, one draw adds %" PRIu64
                " us and three add %" PRIu64 " us — %s", base / 1000u,
             one_draw / 1000u, three_draws / 1000u,
             one_draw != 0 && three_draws >= one_draw * 2u
                ? "the drawing scales with the fill, and what is left is "
                  "the frame's fixed cost"
                : "the drawing does not scale, so the cost is fixed per "
                  "frame and is not in the drawing");
   }

   /* ---------------------------------------------------------------- */
   /* C — pacing, pipelined, clear then draw then clear.               */
   /* ---------------------------------------------------------------- */

   pd_stats pace_clear_a, pace_draw, pace_clear_b;
   pd_measure(&fw, surface, &content, format, extent, VK_PRESENT_MODE_FIFO_KHR,
              PD_FRAME_CLEAR, 0, PD_FRAMES, PD_RUN_BUDGET_NS, false,
              "C1 pacing, clear", &pace_clear_a);
   pd_measure(&fw, surface, &content, format, extent, VK_PRESENT_MODE_FIFO_KHR,
              PD_FRAME_DRAW, PD_DRAWS_APP, PD_FRAMES, PD_RUN_BUDGET_NS, false,
              "C2 pacing, three draws", &pace_draw);
   pd_measure(&fw, surface, &content, format, extent, VK_PRESENT_MODE_FIFO_KHR,
              PD_FRAME_CLEAR, 0, PD_FRAMES, PD_RUN_BUDGET_NS, false,
              "C3 pacing, clear again", &pace_clear_b);

   /* THE CONTROL, AND IT IS CHECKED BEFORE ANYTHING IS CONCLUDED FROM
    * IT. Four other tests measure a clear frame pacing at the refresh on
    * this hardware. If it does not here, this console is not in the
    * state those measurements were taken in, and the comparison below is
    * against a number that was never the refresh. */
   for (uint32_t i = 0; i < 2; i++) {
      const pd_stats *s = i == 0 ? &pace_clear_a : &pace_clear_b;
      if (s->intervals < PD_MIN_INTERVALS) {
         t_check(t, false, "C: %s produced %" PRIu32 " interval(s), too few "
                           "to judge", s->what, s->intervals);
         continue;
      }
      t_check(t, s->within_10pct * 100u >= s->intervals * PD_PACED_PCT,
              "C: %s is paced at the refresh — %" PRIu32 " of %" PRIu32
              " intervals within 10%% of %" PRIu64 " us (>= %u%% needed)",
              s->what, s->within_10pct, s->intervals, PD_REFRESH_NS / 1000u,
              PD_PACED_PCT);
   }

   /* ---------------------------------------------------------------- */
   /* D — the reporter's configuration: three draws under IMMEDIATE.   */
   /* ---------------------------------------------------------------- */

   /* THE METER IS TURNED ON HERE, AND FROM INSIDE THE PROCESS BECAUSE
    * THERE IS NOWHERE ELSE. Nothing launches homebrew with an
    * environment, so MESA_VK_WSI_HORIZON_ACQUIRE_STATS can only be set
    * by the application itself — and it has to be set before
    * vkCreateSwapchainKHR, which is where the swapchain reads it.
    *
    * What it prints goes to stderr, which testfw's main() has dup2'd
    * onto this test's log, so the split lands in the same file as the
    * numbers below it and can be read against them. On for this section
    * only: the other runs are measurements of a driver that is not
    * logging once a second. */
   setenv("MESA_VK_WSI_HORIZON_ACQUIRE_STATS", "1", 1);
   t_note(t, "D: MESA_VK_WSI_HORIZON_ACQUIRE_STATS=1 for this run — the "
             "\"wsi_horizon: acquire over ...\" lines below split the "
             "acquire's wait into the dequeue and the release fence");

   pd_stats imm_draw;
   pd_measure(&fw, surface, &content, format, extent,
              VK_PRESENT_MODE_IMMEDIATE_KHR, PD_FRAME_DRAW, PD_DRAWS_APP,
              PD_FRAMES, PD_RUN_BUDGET_NS, false,
              "D pacing, three draws, IMMEDIATE", &imm_draw);

   unsetenv("MESA_VK_WSI_HORIZON_ACQUIRE_STATS");

   /* ---------------------------------------------------------------- */
   /* E — the relation the whole file exists for.                      */
   /* ---------------------------------------------------------------- */

   if (pace_draw.intervals >= PD_MIN_INTERVALS && gpu_draw3.gpu_samples != 0) {
      const uint64_t gpu = pd_gpu_mean_ns(&gpu_draw3);
      const uint64_t floor_ns = gpu > PD_REFRESH_NS ? gpu : PD_REFRESH_NS;
      const uint64_t bound_ns = floor_ns + PD_SLACK_REFRESHES * PD_REFRESH_NS;
      const uint64_t got_ns = pd_mean_ns(&pace_draw);

      t_check(t, got_ns <= bound_ns,
              "E: a drawn frame arrives no later than the slower of the "
              "display and the GPU allows — %" PRIu64 " us against a bound of "
              "%" PRIu64 " us (GPU %" PRIu64 " us, refresh %" PRIu64 " us, "
              "one refresh of slack)", got_ns / 1000u, bound_ns / 1000u,
              gpu / 1000u, PD_REFRESH_NS / 1000u);

      if (got_ns > bound_ns) {
         t_note(t, "E: %" PRIu64 " us of every drawn frame is spent neither "
                   "on the GPU nor waiting for the display. On this path "
                   "that is the presentation engine, and "
                   "MESA_VK_WSI_HORIZON_ACQUIRE_STATS=1 splits the acquire's "
                   "share of it into the dequeue and the release fence",
                (got_ns - bound_ns) / 1000u);
      } else if (gpu > PD_REFRESH_NS) {
         t_note(t, "E: the frame rate is the GPU's — a drawn frame costs %"
                   PRIu64 " us of GPU against a %" PRIu64 " us refresh, so "
                   "the presentation engine is not what is pacing this",
                gpu / 1000u, PD_REFRESH_NS / 1000u);
      }
   } else {
      t_check(t, false, "E: not enough of C2 or B3 completed to relate them "
                        "(%" PRIu32 " intervals, %" PRIu32 " GPU samples)",
              pace_draw.intervals, gpu_draw3.gpu_samples);
   }

   /* The clear runs, side by side with the drawn one, as the single line
    * a reader of the log needs. */
   t_note(t, "SUMMARY: clear %" PRIu64 "/%" PRIu64 " us before/after, three "
             "draws %" PRIu64 " us FIFO and %" PRIu64 " us IMMEDIATE; GPU "
             "cost clear %" PRIu64 " us, one draw %" PRIu64 " us, three "
             "draws %" PRIu64 " us",
          pd_mean_ns(&pace_clear_a) / 1000u, pd_mean_ns(&pace_clear_b) / 1000u,
          pd_mean_ns(&pace_draw) / 1000u, pd_mean_ns(&imm_draw) / 1000u,
          pd_gpu_mean_ns(&gpu_clear) / 1000u,
          pd_gpu_mean_ns(&gpu_draw1) / 1000u,
          pd_gpu_mean_ns(&gpu_draw3) / 1000u);

out_content:
   pd_content_destroy(&fw, &content);
out_surface:
   if (surface != VK_NULL_HANDLE)
      fw.wsi.vkDestroySurfaceKHR(fw.instance, surface, NULL);
out:
   vkfw_finish(&fw);
   return rv;
}
