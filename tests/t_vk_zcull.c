/*
 * Zcull — is the depth culling this driver switched on actually correct?
 *
 * WHY THE QUESTION EXISTS. The physical device has advertised
 * has_zcull_info since the geometry query landed, and NVK acts on it:
 * nvk_CmdBeginRendering programs SET_ZCULL_REGION_*, the subregion
 * allocation and CLEAR_ZCULL_REGION whenever that flag is true and a
 * depth attachment is being cleared. Until the patch this test was
 * written for, every channel was created with bind_zcull false, so the
 * on-chip Zcull state had no context-switch save area.
 *
 * THE FAILURE MODE IS SILENT, and that is what shapes the whole test.
 * Zcull only ever *rejects*: it is a conservative early-out in front of
 * the depth test. Wrong Zcull state therefore does not fault, does not
 * set an error notifier and does not corrupt a value — it makes
 * fragments that should have been drawn simply not be there. A test that
 * only asked "did anything go wrong" would pass on a broken driver.
 *
 * SO THIS TEST ASKS THE SAME WORKLOAD TWICE, AND COMPARES.
 * NVK_HORIZON_ZCULL=0 makes the physical device withhold has_zcull_info,
 * which takes NVK's else branch (SET_ACTIVE_ZCULL_REGION 0x3f) and
 * leaves the channel with no Zcull context. Section A renders with that;
 * section B tears the whole instance down, sets the variable back, and
 * renders the identical workload with Zcull on. Depth culling is not
 * allowed to change what is drawn, so:
 *
 *     the two colour images must be identical, byte for byte,
 *     and so must the two depth images.
 *
 * That comparison is what makes the answer attributable. A difference is
 * Zcull rejecting something it must not, and there is nothing else it
 * could be — the two runs differ in one environment variable.
 *
 * IT ALSO CHECKS AN ABSOLUTE ANSWER, because a fault that affects both
 * halves equally would compare equal. The workload is built so the final
 * image is analytic: in every pass, each band of the target is drawn
 * three times at three depths, and the nearest of the three wins whether
 * the three are issued back-to-front or front-to-back. So the last
 * pass's near colour is what every pixel must hold, and 0.25 is what
 * every depth texel must hold.
 *
 * FRONT-TO-BACK IS THE ORDER ZCULL IS FOR. Odd passes issue the near
 * draw first and then two draws the depth test must reject; that is the
 * case a Zcull region exists to reject early, and the case where a wrong
 * region rejects the near draw of the *next* pass instead.
 *
 * WHAT IT CANNOT ARRANGE, said rather than implied: a context switch
 * caused by another process. One application cannot make the scheduler
 * switch its channel out on demand. What it does instead is make the
 * work as churned as a single process can — a separate submit per pass,
 * so the channel goes idle and is rescheduled twelve times, and one
 * throwaway pipeline creation in the middle, which uploads a shader
 * through NVK's upload queue and therefore through a *different*
 * channel on the same address space. If Zcull ever needs a cross-process
 * switch to break, this test will not see it, and the ledger says so.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>    /* snprintf */
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include "common/vkfw.h"

#include "depth_vert_z.spv.h"
#include "depth_frag_pc.spv.h"
/* Only ever compiled into a pipeline and destroyed again — the upload
 * that pipeline creation performs is the point, not the shader. */
#include "comp_write_id.spv.h"

const char *const test_name = "t_vk_zcull";
/* No display: main() starts a console and reports through it. */
const bool test_uses_display = false;

/* 256x256 rather than the 64x64 the other render tests use. Zcull
 * allocates in aliquots covering a square of pixels — the geometry query
 * reports pixel_squares_by_aliquots — and a target smaller than a few of
 * them would put the whole image inside one region, where a per-region
 * mistake has nothing to show. */
#define W            256u
#define H            256u
#define TEXELS       (W * H)
#define IMAGE_B      (TEXELS * 4u)
#define TAIL_WORDS   256u
#define READBACK_B   (IMAGE_B + TAIL_WORDS * 4u)
#define POISON       0xdeadbeefu

#define BANDS        16u
#define BAND_W       (W / BANDS)

/* Twelve passes, each its own submit. Enough that a Zcull region left
 * over from one pass has eleven chances to be wrong in the next. */
#define PASSES       12u
/* The one pass that LOADs depth instead of clearing it. NVK takes the
 * non-Zcull branch for it (use_zcull wants loadOp == CLEAR when the
 * image has no Zcull plane, and upstream disables those), so this is the
 * transition out of and back into Zcull mid-sequence. */
#define LOAD_PASS    5u
/* Two passes render to a sub-rectangle, which moves
 * SET_ZCULL_REGION_PIXEL_OFFSET and the region size. Neither is the last
 * pass, so the analytic answer is unaffected. */
#define SMALL_PASS_A 3u
#define SMALL_PASS_B 8u
#define SMALL_INSET  32u

#define DEPTH_FORMAT VK_FORMAT_D32_SFLOAT
#define COLOUR_FORMAT VK_FORMAT_R8G8B8A8_UNORM

/* Depths for an ordinary pass. All exact in binary, so the depth
 * readback is an equality and not a tolerance. */
#define Z_NEAR       0.25f
#define Z_MID        0.5f
#define Z_FAR        0.75f
#define Z_CLEAR      1.0f
/* And for the LOAD pass, which inherits 0.25 from the pass before it and
 * therefore has to draw in front of that. */
#define Z_LOAD_NEAR  0.0625f
#define Z_LOAD_MID   0.125f
#define Z_LOAD_FAR   0.1875f

#define CLEAR_RGBA   0x05u, 0x06u, 0x07u, 0x08u

/* One render of the whole workload: what came back and how long it
 * took. The images are copies on the host heap because the two halves
 * of this test do not have a device open at the same time — B's is
 * created only after A's is destroyed. */
struct run {
   uint32_t *colour;   /* TEXELS words */
   uint32_t *depth;    /* TEXELS words */
   uint64_t ns;
   bool valid;
};

static uint32_t texel(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
   return (a << 24) | (b << 16) | (g << 8) | r;
}

static VkClearColorValue unorm_colour(uint32_t r, uint32_t g, uint32_t b,
                                      uint32_t a)
{
   VkClearColorValue c;
   c.float32[0] = (float)r / 255.0f;
   c.float32[1] = (float)g / 255.0f;
   c.float32[2] = (float)b / 255.0f;
   c.float32[3] = (float)a / 255.0f;
   return c;
}

static uint32_t float_bits(float f)
{
   uint32_t bits;
   memcpy(&bits, &f, sizeof(bits));
   return bits;
}

/* The three colours a (pass, band) draws, `slot` 0 = near, 1 = mid,
 * 2 = far. Every one of the 12 * 16 * 3 is distinct, so a pixel holding
 * the wrong one names the pass, the band and the depth it came from
 * rather than just "wrong". Each channel is a byte, and k/255.0f
 * round-trips to exactly k through an R8G8B8A8_UNORM attachment. */
static uint32_t draw_texel(uint32_t pass, uint32_t band, uint32_t slot)
{
   return texel(0x10u + pass * 4u, 0x10u + band * 8u,
                0x30u + slot * 0x50u + pass, 0xffu);
}

/* The push constant block both shaders declare: colour at offset 0,
 * depth at offset 16. */
struct push_data {
   float colour[4];
   float z;
};

static struct push_data push_for(uint32_t rgba, float z)
{
   struct push_data p;
   p.colour[0] = (float)(rgba & 0xffu) / 255.0f;
   p.colour[1] = (float)((rgba >> 8) & 0xffu) / 255.0f;
   p.colour[2] = (float)((rgba >> 16) & 0xffu) / 255.0f;
   p.colour[3] = (float)((rgba >> 24) & 0xffu) / 255.0f;
   p.z = z;
   return p;
}

static void barrier(vkfw *fw, VkCommandBuffer cb, VkImage img,
                    VkImageAspectFlags aspect,
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
      .image = img,
      .subresourceRange = {
         .aspectMask = aspect,
         .levelCount = 1,
         .layerCount = 1,
      },
   };
   fw->vk.vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0,
                               0, NULL, 0, NULL, 1, &b);
}

/* The rectangle pass `p` renders to. Two of the passes use a sub-
 * rectangle; every scissor below is clamped to whatever this returns,
 * because a scissor reaching outside the render area is invalid usage
 * that nothing here would report. */
static VkRect2D pass_area(uint32_t p)
{
   if (p == SMALL_PASS_A || p == SMALL_PASS_B) {
      return (VkRect2D){
         .offset = { (int32_t)SMALL_INSET, (int32_t)SMALL_INSET },
         .extent = { W - 2u * SMALL_INSET, H - 2u * SMALL_INSET },
      };
   }
   return (VkRect2D){ .offset = { 0, 0 }, .extent = { W, H } };
}

/* The three depths pass `p` draws at, nearest first. */
static void pass_depths(uint32_t p, float z[3])
{
   if (p == LOAD_PASS) {
      z[0] = Z_LOAD_NEAR; z[1] = Z_LOAD_MID; z[2] = Z_LOAD_FAR;
   } else {
      z[0] = Z_NEAR; z[1] = Z_MID; z[2] = Z_FAR;
   }
}

/* Records one pass into `cb`. Odd passes issue the near draw first and
 * then two the depth test must reject — the order Zcull exists for; even
 * passes issue back to front, where every draw passes. The winner is the
 * near one either way, which is what makes the final image analytic
 * without making the two orders interchangeable. */
static void record_pass(vkfw *fw, VkCommandBuffer cb, uint32_t p,
                        const vkfw_gfx *gfx, VkImageView colour_view,
                        VkImageView depth_view)
{
   const VkRect2D area = pass_area(p);
   const bool front_to_back = (p & 1u) != 0u;

   VkClearValue colour_clear;
   colour_clear.color = unorm_colour(CLEAR_RGBA);
   VkClearValue depth_clear;
   depth_clear.depthStencil.depth = Z_CLEAR;
   depth_clear.depthStencil.stencil = 0;

   const VkRenderingAttachmentInfo colour_att = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = colour_view,
      .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      /* Cleared once, then carried: the final image is what the last
       * pass wrote over everything the passes before it did, so a
       * fragment wrongly rejected in the last pass shows up as an
       * earlier pass's colour rather than as the clear. */
      .loadOp = (p == 0) ? VK_ATTACHMENT_LOAD_OP_CLEAR
                         : VK_ATTACHMENT_LOAD_OP_LOAD,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .clearValue = colour_clear,
   };
   const VkRenderingAttachmentInfo depth_att = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = depth_view,
      .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
      .loadOp = (p == LOAD_PASS) ? VK_ATTACHMENT_LOAD_OP_LOAD
                                 : VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .clearValue = depth_clear,
   };
   const VkRenderingInfo ri = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea = area,
      .layerCount = 1,
      .colorAttachmentCount = 1,
      .pColorAttachments = &colour_att,
      .pDepthAttachment = &depth_att,
   };

   float z[3];
   pass_depths(p, z);

   fw->vk.vkCmdBeginRendering(cb, &ri);

   const VkViewport viewport = {
      .x = 0.0f, .y = 0.0f,
      .width = (float)W, .height = (float)H,
      .minDepth = 0.0f, .maxDepth = 1.0f,
   };
   fw->vk.vkCmdSetViewport(cb, 0, 1, &viewport);
   fw->vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            gfx->pipeline);

   const uint32_t area_x0 = (uint32_t)area.offset.x;
   const uint32_t area_x1 = area_x0 + area.extent.width;

   for (uint32_t band = 0; band < BANDS; band++) {
      uint32_t x0 = band * BAND_W;
      uint32_t x1 = x0 + BAND_W;
      if (x0 < area_x0)
         x0 = area_x0;
      if (x1 > area_x1)
         x1 = area_x1;
      if (x0 >= x1)
         continue;   /* this band is outside the pass's render area */

      const VkRect2D scissor = {
         .offset = { (int32_t)x0, area.offset.y },
         .extent = { x1 - x0, area.extent.height },
      };
      fw->vk.vkCmdSetScissor(cb, 0, 1, &scissor);

      for (uint32_t n = 0; n < 3; n++) {
         const uint32_t slot = front_to_back ? n : (2u - n);
         const struct push_data pc =
            push_for(draw_texel(p, band, slot), z[slot]);
         fw->vk.vkCmdPushConstants(cb, gfx->layout,
                                   VK_SHADER_STAGE_VERTEX_BIT |
                                      VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, (uint32_t)sizeof(pc), &pc);
         fw->vk.vkCmdDraw(cb, 3, 1, 0, 0);
      }
   }

   fw->vk.vkCmdEndRendering(cb);
}

/* An attachment-to-attachment dependency between two submits. The passes
 * are separate command buffers on purpose (see the header), so the
 * write-after-write between them is stated rather than left to the
 * channel's ordering. */
static void self_barrier(vkfw *fw, VkCommandBuffer cb, VkImage colour,
                         VkImage depth)
{
   barrier(fw, cb, colour, VK_IMAGE_ASPECT_COLOR_BIT,
           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
           VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
   barrier(fw, cb, depth, VK_IMAGE_ASPECT_DEPTH_BIT,
           VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
           VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
           VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
              VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
           VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
              VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
}

/* Creates a compute pipeline and destroys it again, purely so NVK
 * uploads a shader — which goes through the upload queue, and therefore
 * through a channel that is not the one rendering.
 *
 * THE LAYOUT HAS TO MATCH THE SHADER even though nothing dispatches it.
 * comp_write_id declares an SSBO at set 0 binding 0, and pipeline-layout
 * consistency is required at pipeline *creation*, not only at dispatch:
 * a layout declaring no set makes vkCreateComputePipelines invalid
 * usage, and a driver that refuses it takes the upload — the whole point
 * of this function — with it. An earlier version did exactly that and
 * argued its way out of it in a comment; found by review on PR #22.
 *
 * A failure is not fatal — this is the test trying to disturb the
 * scheduler, not something it measures — but it IS reported, because a
 * poke that silently did not happen would leave section B claiming a
 * cross-channel disturbance it never caused. */
static void poke_upload_queue(vkfw *fw)
{
   VkShaderModule module = VK_NULL_HANDLE;
   VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
   VkPipelineLayout layout = VK_NULL_HANDLE;
   VkPipeline pipeline = VK_NULL_HANDLE;
   const char *failed = NULL;

   const VkShaderModuleCreateInfo smci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(comp_write_id_spv),
      .pCode = comp_write_id_spv,
   };
   if (fw->vk.vkCreateShaderModule(fw->dev, &smci, NULL, &module) !=
       VK_SUCCESS) {
      failed = "vkCreateShaderModule";
      goto out;
   }

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
   if (fw->vk.vkCreateDescriptorSetLayout(fw->dev, &dslci, NULL,
                                          &set_layout) != VK_SUCCESS) {
      failed = "vkCreateDescriptorSetLayout";
      goto out;
   }

   const VkPipelineLayoutCreateInfo plci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &set_layout,
   };
   if (fw->vk.vkCreatePipelineLayout(fw->dev, &plci, NULL, &layout) !=
       VK_SUCCESS) {
      failed = "vkCreatePipelineLayout";
      goto out;
   }

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
   if (fw->vk.vkCreateComputePipelines(fw->dev, VK_NULL_HANDLE, 1, &cpci,
                                       NULL, &pipeline) != VK_SUCCESS)
      failed = "vkCreateComputePipelines";

out:
   if (failed != NULL) {
      t_note(fw->t, "the upload-queue poke did not happen: %s failed — "
                    "no shader was uploaded between the passes, so this "
                    "run disturbed one channel less than it meant to",
             failed);
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

/* Renders the whole workload on an already-initialised fixture and
 * copies the two images onto the host heap. `label` names the half in
 * every check line. Returns false with `out->valid` false on the first
 * failure. */
static bool render_workload(vkfw *fw, const char *label, struct run *out)
{
   test_ctx *t = fw->t;
   bool ok = false;

   vkfw_buffer colour_dst = { 0 }, depth_dst = { 0 };
   vkfw_image colour = { 0 }, depth = { 0 };
   VkImageView colour_view = VK_NULL_HANDLE, depth_view = VK_NULL_HANDLE;
   vkfw_gfx gfx = { 0 };
   VkResult r;

   const VkImageUsageFlags colour_usage =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
   const VkImageUsageFlags depth_usage =
      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

   if (!vkfw_image_supported(fw, DEPTH_FORMAT, VK_IMAGE_TYPE_2D,
                             VK_IMAGE_TILING_OPTIMAL, depth_usage,
                             "D32_SFLOAT depth attachment")) {
      t_note(t, "%s: this device cannot use D32_SFLOAT as a depth "
                "attachment that can also be copied out; skipped, not "
                "failed", label);
      goto out;
   }

   if (!vkfw_image_create(fw, COLOUR_FORMAT, (VkExtent3D){ W, H, 1 }, 1, 1,
                          colour_usage, VK_IMAGE_TILING_OPTIMAL, &colour))
      goto out;
   if (!vkfw_image_create(fw, DEPTH_FORMAT, (VkExtent3D){ W, H, 1 }, 1, 1,
                          depth_usage, VK_IMAGE_TILING_OPTIMAL, &depth))
      goto out;

   const VkImageViewCreateInfo colour_ivci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = colour.img,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = COLOUR_FORMAT,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .levelCount = 1, .layerCount = 1,
      },
   };
   r = fw->vk.vkCreateImageView(fw->dev, &colour_ivci, NULL, &colour_view);
   if (!t_check(t, r == VK_SUCCESS, "%s: vkCreateImageView(colour) -> %s",
                label, vkfw_result_str(r)))
      goto out;

   const VkImageViewCreateInfo depth_ivci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = depth.img,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = DEPTH_FORMAT,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
         .levelCount = 1, .layerCount = 1,
      },
   };
   r = fw->vk.vkCreateImageView(fw->dev, &depth_ivci, NULL, &depth_view);
   if (!t_check(t, r == VK_SUCCESS, "%s: vkCreateImageView(depth) -> %s",
                label, vkfw_result_str(r)))
      goto out;

   if (!vkfw_buffer_create(fw, READBACK_B, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &colour_dst))
      goto out;
   if (!vkfw_buffer_create(fw, READBACK_B, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &depth_dst))
      goto out;

   const vkfw_gfx_desc desc = {
      .vs_spv = depth_vert_z_spv, .vs_B = sizeof(depth_vert_z_spv),
      .fs_spv = depth_frag_pc_spv, .fs_B = sizeof(depth_frag_pc_spv),
      .colour_format = COLOUR_FORMAT,
      .depth_format = DEPTH_FORMAT,
      .push_constant_B = (uint32_t)sizeof(struct push_data),
      .push_constant_stages = VK_SHADER_STAGE_VERTEX_BIT |
                              VK_SHADER_STAGE_FRAGMENT_BIT,
      .width = W, .height = H,
      .dynamic_viewport = true,
      .depth_test = true,
      .depth_write = true,
      .depth_compare = VK_COMPARE_OP_LESS,
   };
   if (!vkfw_gfx_create(fw, label, &desc, &gfx))
      goto out;

   if (!vkfw_buffer_poison(fw, &colour_dst, POISON) ||
       !vkfw_buffer_poison(fw, &depth_dst, POISON))
      goto out;

   const u64 t0 = armGetSystemTick();

   /* Both images into their attachment layouts, once, in a submit of
    * their own — so the per-pass command buffers below are nothing but
    * a barrier and a render pass. */
   {
      VkCommandBuffer cb;
      if (!vkfw_cmd_begin(fw, &cb))
         goto out;
      barrier(fw, cb, colour.img, VK_IMAGE_ASPECT_COLOR_BIT,
              VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
      barrier(fw, cb, depth.img, VK_IMAGE_ASPECT_DEPTH_BIT,
              VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
              0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);
      if (!vkfw_submit_and_wait(fw, cb, "the initial layout transitions"))
         goto out;
   }

   for (uint32_t p = 0; p < PASSES; p++) {
      VkCommandBuffer cb;
      if (!vkfw_cmd_begin(fw, &cb))
         goto out;
      if (p != 0)
         self_barrier(fw, cb, colour.img, depth.img);
      record_pass(fw, cb, p, &gfx, colour_view, depth_view);

      char what[64];
      snprintf(what, sizeof(what), "%s: pass %u (%s depth, %s)", label, p,
               p == LOAD_PASS ? "loaded" : "cleared",
               (p & 1u) ? "front to back" : "back to front");
      if (!vkfw_submit_and_wait(fw, cb, what))
         goto out;

      /* Halfway through, make another channel do something. */
      if (p == LOAD_PASS)
         poke_upload_queue(fw);
   }

   /* Copy both images out. */
   {
      VkCommandBuffer cb;
      if (!vkfw_cmd_begin(fw, &cb))
         goto out;
      barrier(fw, cb, colour.img, VK_IMAGE_ASPECT_COLOR_BIT,
              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
              VK_ACCESS_TRANSFER_READ_BIT,
              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT);
      /* Both fragment-test stages: nothing here writes gl_FragDepth or
       * discards, so the depth write is free to happen in the early
       * one. */
      barrier(fw, cb, depth.img, VK_IMAGE_ASPECT_DEPTH_BIT,
              VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
              VK_ACCESS_TRANSFER_READ_BIT,
              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                 VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT);

      VkBufferImageCopy region = {
         .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1,
         },
         .imageExtent = { W, H, 1 },
      };
      fw->vk.vkCmdCopyImageToBuffer(cb, colour.img,
                                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    colour_dst.buf, 1, &region);
      region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
      fw->vk.vkCmdCopyImageToBuffer(cb, depth.img,
                                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    depth_dst.buf, 1, &region);

      if (!vkfw_submit_and_wait(fw, cb, "copying both images out"))
         goto out;
   }

   out->ns = armTicksToNs(armGetSystemTick() - t0);

   if (!vkfw_buffer_invalidate(fw, &colour_dst) ||
       !vkfw_buffer_invalidate(fw, &depth_dst))
      goto out;

   out->colour = (uint32_t *)malloc(IMAGE_B);
   out->depth = (uint32_t *)malloc(IMAGE_B);
   if (!t_check(t, out->colour != NULL && out->depth != NULL,
                "%s: host copies of the two images", label))
      goto out;
   memcpy(out->colour, colour_dst.map, IMAGE_B);
   memcpy(out->depth, depth_dst.map, IMAGE_B);

   /* The tail past the image must still hold its poison: a copy that
    * wrote more than the image is a different bug from a render that
    * drew the wrong thing, and it would otherwise look like one. */
   vkfw_expect_words(fw, (const uint32_t *)colour_dst.map + TEXELS, POISON,
                     TAIL_WORDS, "nothing was written past the colour image");
   vkfw_expect_words(fw, (const uint32_t *)depth_dst.map + TEXELS, POISON,
                     TAIL_WORDS, "nothing was written past the depth image");

   out->valid = true;
   ok = true;

out:
   vkfw_gfx_destroy(fw, &gfx);
   if (depth_view != VK_NULL_HANDLE)
      fw->vk.vkDestroyImageView(fw->dev, depth_view, NULL);
   if (colour_view != VK_NULL_HANDLE)
      fw->vk.vkDestroyImageView(fw->dev, colour_view, NULL);
   vkfw_buffer_destroy(fw, &depth_dst);
   vkfw_buffer_destroy(fw, &colour_dst);
   vkfw_image_destroy(fw, &depth);
   vkfw_image_destroy(fw, &colour);
   return ok;
}

/* The analytic answer, checked on one half. Two statements, counted
 * separately because they fail for different reasons: every pixel holds
 * the last pass's near colour for its band, and every depth texel holds
 * that pass's near depth. */
static void check_absolute(test_ctx *t, const char *label,
                           const struct run *r)
{
   const uint32_t last = PASSES - 1u;
   const uint32_t d_want = float_bits(Z_NEAR);

   uint32_t c_wrong = 0, first_i = 0, first_got = 0, first_want = 0;
   for (uint32_t i = 0; i < TEXELS; i++) {
      const uint32_t band = (i % W) / BAND_W;
      const uint32_t want = draw_texel(last, band, 0);
      if (r->colour[i] != want) {
         if (c_wrong == 0) {
            first_i = i;
            first_got = r->colour[i];
            first_want = want;
         }
         c_wrong++;
      }
   }
   t_check(t, c_wrong == 0,
           "%s: every pixel holds the last pass's near colour "
           "(%u of %u wrong; first at (%u,%u) got 0x%08x want 0x%08x)",
           label, c_wrong, TEXELS, first_i % W, first_i / W, first_got,
           first_want);

   uint32_t d_wrong = 0, d_first_i = 0, d_first_got = 0;
   for (uint32_t i = 0; i < TEXELS; i++) {
      if (r->depth[i] != d_want) {
         if (d_wrong == 0) {
            d_first_i = i;
            d_first_got = r->depth[i];
         }
         d_wrong++;
      }
   }
   t_check(t, d_wrong == 0,
           "%s: every depth texel is %g (%u of %u wrong; first at (%u,%u) "
           "holds 0x%08x)",
           label, (double)Z_NEAR, d_wrong, TEXELS, d_first_i % W,
           d_first_i / W, d_first_got);
}

int run_test(test_ctx *t)
{
   struct run off = { 0 }, on = { 0 };

   VkPhysicalDeviceVulkan13Features features13 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
      .dynamicRendering = VK_TRUE,
   };

   /* --- A: the reference, with Zcull withheld ----------------------- */
   t_note(t, "A: NVK_HORIZON_ZCULL=0 — has_zcull_info is withheld, so NVK "
             "takes its non-Zcull branch and no channel binds a Zcull "
             "context");
   setenv("NVK_HORIZON_ZCULL", "0", 1);
   {
      vkfw fw;
      if (!vkfw_init(&fw, t, &features13)) {
         unsetenv("NVK_HORIZON_ZCULL");
         return 1;
      }
      render_workload(&fw, "A (Zcull off)", &off);
      vkfw_finish(&fw);
   }

   /* --- B: the same thing with Zcull on ----------------------------- */
   t_note(t, "B: NVK_HORIZON_ZCULL=1 — a fresh instance and device, the "
             "identical workload");
   setenv("NVK_HORIZON_ZCULL", "1", 1);
   {
      vkfw fw;
      if (!vkfw_init(&fw, t, &features13)) {
         unsetenv("NVK_HORIZON_ZCULL");
         goto done;
      }
      render_workload(&fw, "B (Zcull on)", &on);
      vkfw_finish(&fw);
   }
   unsetenv("NVK_HORIZON_ZCULL");

   /* --- the absolute answer, on each half separately ---------------- */
   if (off.valid)
      check_absolute(t, "A (Zcull off)", &off);
   if (on.valid)
      check_absolute(t, "B (Zcull on)", &on);

   /* --- and the comparison, which is the point ---------------------- */
   if (off.valid && on.valid) {
      uint32_t c_diff = 0, c_first = 0;
      for (uint32_t i = 0; i < TEXELS; i++) {
         if (off.colour[i] != on.colour[i]) {
            if (c_diff == 0)
               c_first = i;
            c_diff++;
         }
      }
      t_check(t, c_diff == 0,
              "Zcull changed no pixel (%u of %u differ; first at (%u,%u): "
              "off 0x%08x, on 0x%08x)",
              c_diff, TEXELS, c_first % W, c_first / W,
              off.colour[c_first], on.colour[c_first]);

      uint32_t d_diff = 0, d_first = 0;
      for (uint32_t i = 0; i < TEXELS; i++) {
         if (off.depth[i] != on.depth[i]) {
            if (d_diff == 0)
               d_first = i;
            d_diff++;
         }
      }
      t_check(t, d_diff == 0,
              "Zcull changed no depth texel (%u of %u differ; first at "
              "(%u,%u): off 0x%08x, on 0x%08x)",
              d_diff, TEXELS, d_first % W, d_first / W,
              off.depth[d_first], on.depth[d_first]);

      /* Reported, never asserted on. Whether Zcull is faster here
       * depends on the workload and on the clocks, and this test's
       * workload was built to be checkable rather than to be culled
       * well: a third of its draws are meant to be rejected, and Zcull
       * rejects them a little earlier than the depth test would. A
       * number that comes back the wrong way is a fact about this
       * workload, not a failure. */
      t_note(t, "wall time: %" PRIu64 " ms with Zcull off, %" PRIu64
                " ms with it on, over %u passes of %u draws",
             off.ns / 1000000u, on.ns / 1000000u, PASSES, BANDS * 3u);
   }

done:
   free(off.colour);
   free(off.depth);
   free(on.colour);
   free(on.depth);
   return 0;
}
