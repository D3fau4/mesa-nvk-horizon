/*
 * Phase 5 item 6 — textures.
 *
 * An 8x8 source image with two mip levels is sampled by a fragment
 * shader into a 64x64 off-screen target, and every one of the 4096
 * result pixels is compared against what the sampler must have
 * produced.
 *
 * WHY THE NEAREST CASES ARE EXACT AND NOT APPROXIMATE. The full-target
 * triangle in tex_vert_fullscreen.spvasm makes uv = (fx/64, fy/64), so
 * a pixel centre samples at texel coordinate (px+0.5)/8 on the 8x8
 * level. That is never closer than 1/16 of a texel to a boundary, at
 * any of the 64 columns, so the texel a pixel lands in is decided by
 * arithmetic rather than by a rounding rule:
 *
 *     level 0:  output pixel (px, py)  <-  source texel (px/8,  py/8)
 *     level 1:  output pixel (px, py)  <-  source texel (px/16, py/16)
 *
 * with integer division. There is no tolerance in cases A and B; a
 * sampler that was off by half a texel would move an eighth of the
 * image and fail on hundreds of pixels.
 *
 * THE THREE CASES:
 *
 *   A  nearest, implicit LOD. The descriptor, the sampler, the
 *      addressing and the texel fetch, with the level chosen by the
 *      derivatives (du/dx is an eighth of a texel, so the computed LOD
 *      is -3 and clamps to 0 with three octaves of margin).
 *   B  nearest, explicit LOD 1. Mip selection as the measurement
 *      instead of a by-product: the view must really carry two levels
 *      and the descriptor must really describe them. Level 1 is 4x4 and
 *      holds a pattern that shares no texel with level 0 — checked
 *      below rather than asserted — so sampling the wrong level gives a
 *      different image, not a similar one.
 *   C  linear, implicit LOD. The filter unit. Its expected values are
 *      computed from the same four texels the hardware weighs.
 *
 * WHY C STILL GETS A TOLERANCE, AND A SMALL ONE. The fractional texel
 * position at every pixel centre is an odd sixteenth — 1/16, 3/16, ...
 * 15/16 — so the bilinear weights are exact in any subtexel fixed-point
 * format with four or more bits, and weight quantisation contributes
 * nothing. What is left is the final round to UNORM8, where the exact
 * answer can land on a tie. Two counts out of 255 covers that with room
 * to spare, and the largest deviation actually seen is reported, so a
 * filter that is merely nearly right does not hide inside the
 * tolerance.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <string.h>

#include "common/vkfw.h"

#include "tex_vert_fullscreen.spv.h"
#include "tex_frag_sample.spv.h"
#include "tex_frag_lod1.spv.h"

const char *const test_name = "t_vk_texture";

#define W            64u          /* render target */
#define H            64u
#define TEXELS       (W * H)
#define IMAGE_B      (TEXELS * 4u)
#define TAIL_WORDS   256u
#define READBACK_B   (IMAGE_B + TAIL_WORDS * 4u)
#define POISON       0xdeadbeefu

#define SRC_W        8u           /* source, level 0 */
#define SRC_H        8u
#define SRC1_W       4u           /* source, level 1 */
#define SRC1_H       4u
#define SRC_LEVELS   2u
#define SRC0_B       (SRC_W * SRC_H * 4u)
#define SRC1_B       (SRC1_W * SRC1_H * 4u)
#define STAGING_B    (SRC0_B + SRC1_B)

#define INTERP_TOLERANCE 2

/* R8G8B8A8_UNORM puts R at the lowest address. */
static uint32_t texel(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
   return (a << 24) | (b << 16) | (g << 8) | r;
}

/* The source data. Every texel of a level is distinct — r and g alone
 * identify (x, y) — and the two levels use disjoint red values, so no
 * texel of one can be mistaken for a texel of the other. Both
 * properties are checked before the GPU is asked anything. */
static uint32_t src_texel(uint32_t level, uint32_t x, uint32_t y)
{
   if (level == 0)
      return texel(8u + x * 30u, 8u + y * 30u, 8u + (x * SRC_W + y) * 3u,
                   0xffu);
   return texel(5u + x * 50u, 5u + y * 50u, 200u - (x * SRC1_W + y) * 4u,
                0xffu);
}

static uint32_t level_w(uint32_t level) { return level == 0 ? SRC_W : SRC1_W; }
static uint32_t level_h(uint32_t level) { return level == 0 ? SRC_H : SRC1_H; }

static uint32_t unorm_byte(float v)
{
   if (v <= 0.0f)
      return 0;
   if (v >= 1.0f)
      return 255;
   return (uint32_t)(v * 255.0f + 0.5f);
}

static int32_t ifloor(float f)
{
   const int32_t i = (int32_t)f;
   return f < (float)i ? i - 1 : i;
}

static int32_t clampi(int32_t v, int32_t lo, int32_t hi)
{
   return v < lo ? lo : (v > hi ? hi : v);
}

static uint32_t chan(uint32_t t, uint32_t k)
{
   return (t >> (k * 8u)) & 0xffu;
}

/* What CLAMP_TO_EDGE bilinear filtering of level 0 must produce at a
 * pixel centre. The same four texels, the same two lerps, in the same
 * order the hardware does them. */
static void bilinear(uint32_t px, uint32_t py, uint32_t out[4])
{
   const float tu = ((float)px + 0.5f) / (float)W * (float)SRC_W - 0.5f;
   const float tv = ((float)py + 0.5f) / (float)H * (float)SRC_H - 0.5f;
   const int32_t i = ifloor(tu), j = ifloor(tv);
   const float fu = tu - (float)i, fv = tv - (float)j;

   const uint32_t x0 = (uint32_t)clampi(i, 0, (int32_t)SRC_W - 1);
   const uint32_t x1 = (uint32_t)clampi(i + 1, 0, (int32_t)SRC_W - 1);
   const uint32_t y0 = (uint32_t)clampi(j, 0, (int32_t)SRC_H - 1);
   const uint32_t y1 = (uint32_t)clampi(j + 1, 0, (int32_t)SRC_H - 1);

   const uint32_t t00 = src_texel(0, x0, y0), t10 = src_texel(0, x1, y0);
   const uint32_t t01 = src_texel(0, x0, y1), t11 = src_texel(0, x1, y1);

   for (uint32_t k = 0; k < 4; k++) {
      const float a = (float)chan(t00, k) / 255.0f;
      const float b = (float)chan(t10, k) / 255.0f;
      const float c = (float)chan(t01, k) / 255.0f;
      const float d = (float)chan(t11, k) / 255.0f;
      const float top = a + (b - a) * fu;
      const float bot = c + (d - c) * fu;
      out[k] = unorm_byte(top + (bot - top) * fv);
   }
}

static void image_barrier(vkfw *fw, VkCommandBuffer cb, VkImage img,
                          uint32_t levels,
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
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .levelCount = levels,
         .layerCount = 1,
      },
   };
   fw->vk.vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0,
                               0, NULL, 0, NULL, 1, &b);
}

/* One case: clear, draw the full-target triangle with `set` bound, copy
 * the result back. */
static bool render(vkfw *fw, const char *what, const vkfw_gfx *gfx,
                   VkDescriptorSet set, VkImage target, VkImageView view,
                   vkfw_buffer *dst)
{
   VkCommandBuffer cb;
   if (!vkfw_cmd_begin(fw, &cb))
      return false;

   image_barrier(fw, cb, target, 1,
                 VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

   /* The clear colour is never a possible sample result: every source
    * texel is opaque and this one is not, so a pixel the draw failed to
    * cover is distinguishable from one it covered. */
   VkClearValue clear;
   clear.color.float32[0] = 0.0f;
   clear.color.float32[1] = 0.0f;
   clear.color.float32[2] = 0.0f;
   clear.color.float32[3] = 0.0f;

   const VkRenderingAttachmentInfo colour = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = view,
      .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .clearValue = clear,
   };
   const VkRenderingInfo ri = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea = { .offset = { 0, 0 }, .extent = { W, H } },
      .layerCount = 1,
      .colorAttachmentCount = 1,
      .pColorAttachments = &colour,
   };

   fw->vk.vkCmdBeginRendering(cb, &ri);
   fw->vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            gfx->pipeline);
   fw->vk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  gfx->layout, 0, 1, &set, 0, NULL);
   fw->vk.vkCmdDraw(cb, 3, 1, 0, 0);
   fw->vk.vkCmdEndRendering(cb);

   image_barrier(fw, cb, target, 1,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_ACCESS_TRANSFER_READ_BIT,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT);

   const VkBufferImageCopy region = {
      .imageSubresource = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .mipLevel = 0,
         .baseArrayLayer = 0,
         .layerCount = 1,
      },
      .imageExtent = { W, H, 1 },
   };
   fw->vk.vkCmdCopyImageToBuffer(cb, target,
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 dst->buf, 1, &region);

   if (!vkfw_submit_and_wait(fw, cb, what))
      return false;
   return vkfw_buffer_invalidate(fw, dst);
}

/* Cases A and B: every pixel is exactly the texel it sampled. */
static void check_nearest(vkfw *fw, const uint32_t *got, uint32_t level,
                          const char *what)
{
   const uint32_t shift = level == 0 ? 3u : 4u;   /* 64/8 and 64/4 */
   uint32_t wrong = 0, first = 0;

   for (uint32_t py = 0; py < H; py++) {
      for (uint32_t px = 0; px < W; px++) {
         const uint32_t want = src_texel(level, px >> shift, py >> shift);
         if (got[py * W + px] != want) {
            if (wrong == 0)
               first = py * W + px;
            wrong++;
         }
      }
   }

   t_check(fw->t, wrong == 0,
           "%s: %u/%u pixels hold the texel they sampled",
           what, TEXELS - wrong, TEXELS);
   if (wrong != 0) {
      const uint32_t px = first % W, py = first / W;
      t_note(fw->t, "%s: first wrong pixel (%u,%u): got 0x%08x, want 0x%08x "
                    "(texel %u,%u of level %u)",
             what, px, py, got[first],
             src_texel(level, px >> shift, py >> shift),
             px >> shift, py >> shift, level);
   }
}

/* Case C: per channel, within the tolerance, and the worst deviation
 * reported whatever the verdict. */
static void check_linear(vkfw *fw, const uint32_t *got, const char *what)
{
   uint32_t wrong = 0, first = 0, worst = 0;

   for (uint32_t py = 0; py < H; py++) {
      for (uint32_t px = 0; px < W; px++) {
         uint32_t want[4];
         bilinear(px, py, want);
         const uint32_t have = got[py * W + px];

         bool ok = true;
         for (uint32_t k = 0; k < 4; k++) {
            const uint32_t h = chan(have, k);
            const uint32_t d = h > want[k] ? h - want[k] : want[k] - h;
            if (d > worst)
               worst = d;
            if (d > (uint32_t)INTERP_TOLERANCE)
               ok = false;
         }
         if (!ok) {
            if (wrong == 0)
               first = py * W + px;
            wrong++;
         }
      }
   }

   t_check(fw->t, wrong == 0,
           "%s: %u/%u pixels are within %d of the computed filter result",
           what, TEXELS - wrong, TEXELS, INTERP_TOLERANCE);
   t_note(fw->t, "%s: largest deviation %u of 255", what, worst);
   if (wrong != 0) {
      uint32_t want[4];
      bilinear(first % W, first / W, want);
      t_note(fw->t, "%s: first outside tolerance at (%u,%u): got "
                    "%u %u %u %u, want %u %u %u %u",
             what, first % W, first / W,
             chan(got[first], 0), chan(got[first], 1),
             chan(got[first], 2), chan(got[first], 3),
             want[0], want[1], want[2], want[3]);
   }
}

/* The two properties the expected results rest on, checked on the CPU
 * before the GPU is involved: a texel identifies its position, and no
 * texel of level 1 equals one of level 0. If either stopped being true
 * the cases below would still pass while measuring less. */
static bool check_source_model(vkfw *fw)
{
   test_ctx *t = fw->t;
   uint32_t dup = 0;

   for (uint32_t l = 0; l < SRC_LEVELS; l++) {
      const uint32_t n = level_w(l) * level_h(l);
      for (uint32_t a = 0; a < n; a++) {
         for (uint32_t b = a + 1; b < n; b++) {
            if (src_texel(l, a % level_w(l), a / level_w(l)) ==
                src_texel(l, b % level_w(l), b / level_w(l)))
               dup++;
         }
      }
   }
   if (!t_check(t, dup == 0,
                "every source texel of a level is unique (%u collisions)",
                dup))
      return false;

   uint32_t cross = 0;
   for (uint32_t y0 = 0; y0 < SRC_H; y0++)
      for (uint32_t x0 = 0; x0 < SRC_W; x0++)
         for (uint32_t y1 = 0; y1 < SRC1_H; y1++)
            for (uint32_t x1 = 0; x1 < SRC1_W; x1++)
               if (src_texel(0, x0, y0) == src_texel(1, x1, y1))
                  cross++;
   return t_check(t, cross == 0,
                  "no texel of level 1 equals one of level 0 (%u do)",
                  cross);
}

int run_test(test_ctx *t)
{
   VkPhysicalDeviceVulkan13Features features13 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
      .dynamicRendering = VK_TRUE,
   };

   vkfw fw;
   if (!vkfw_init(&fw, t, &features13))
      return 1;

   vkfw_buffer dst = { 0 }, staging = { 0 };
   vkfw_image src = { 0 }, target = { 0 };
   VkImageView src_view = VK_NULL_HANDLE, target_view = VK_NULL_HANDLE;
   VkSampler nearest = VK_NULL_HANDLE, linear = VK_NULL_HANDLE;
   VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
   VkDescriptorPool pool = VK_NULL_HANDLE;
   VkDescriptorSet set_nearest = VK_NULL_HANDLE, set_linear = VK_NULL_HANDLE;
   vkfw_gfx gfx_sample = { 0 }, gfx_lod1 = { 0 };
   VkResult r;

   if (!check_source_model(&fw))
      goto out;

   /* --- the source texture ---------------------------------------- */
   const VkImageUsageFlags src_usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_DST_BIT;
   if (!vkfw_image_supported(&fw, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
                             VK_IMAGE_TILING_OPTIMAL, src_usage, "source"))
      goto out;
   if (!vkfw_image_create(&fw, VK_FORMAT_R8G8B8A8_UNORM,
                          (VkExtent3D){ SRC_W, SRC_H, 1 }, SRC_LEVELS, 1,
                          src_usage, VK_IMAGE_TILING_OPTIMAL, &src))
      goto out;

   if (!vkfw_buffer_create(&fw, STAGING_B, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &staging))
      goto out;
   if (!t_check(t, staging.map != NULL, "the staging buffer is mapped"))
      goto out;

   uint32_t *stage = (uint32_t *)staging.map;
   for (uint32_t y = 0; y < SRC_H; y++)
      for (uint32_t x = 0; x < SRC_W; x++)
         stage[y * SRC_W + x] = src_texel(0, x, y);
   for (uint32_t y = 0; y < SRC1_H; y++)
      for (uint32_t x = 0; x < SRC1_W; x++)
         stage[SRC_W * SRC_H + y * SRC1_W + x] = src_texel(1, x, y);
   if (!vkfw_buffer_flush(&fw, &staging))
      goto out;

   {
      VkCommandBuffer cb;
      if (!vkfw_cmd_begin(&fw, &cb))
         goto out;

      image_barrier(&fw, cb, src.img, SRC_LEVELS,
                    VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);

      const VkBufferImageCopy up[SRC_LEVELS] = {
         {
            .bufferOffset = 0,
            .imageSubresource = {
               .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
               .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1,
            },
            .imageExtent = { SRC_W, SRC_H, 1 },
         },
         {
            .bufferOffset = SRC0_B,
            .imageSubresource = {
               .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
               .mipLevel = 1, .baseArrayLayer = 0, .layerCount = 1,
            },
            .imageExtent = { SRC1_W, SRC1_H, 1 },
         },
      };
      fw.vk.vkCmdCopyBufferToImage(cb, staging.buf, src.img,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   SRC_LEVELS, up);

      image_barrier(&fw, cb, src.img, SRC_LEVELS,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

      if (!vkfw_submit_and_wait(&fw, cb, "upload the source texture"))
         goto out;
   }

   const VkImageViewCreateInfo src_ivci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = src.img,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .levelCount = SRC_LEVELS,
         .layerCount = 1,
      },
   };
   r = fw.vk.vkCreateImageView(fw.dev, &src_ivci, NULL, &src_view);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateImageView(source, %u levels) "
                "-> %s", SRC_LEVELS, vkfw_result_str(r)))
      goto out;

   /* --- the render target ------------------------------------------ */
   const VkImageUsageFlags tgt_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
   if (!vkfw_image_create(&fw, VK_FORMAT_R8G8B8A8_UNORM,
                          (VkExtent3D){ W, H, 1 }, 1, 1, tgt_usage,
                          VK_IMAGE_TILING_OPTIMAL, &target))
      goto out;

   const VkImageViewCreateInfo tgt_ivci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = target.img,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .levelCount = 1,
         .layerCount = 1,
      },
   };
   r = fw.vk.vkCreateImageView(fw.dev, &tgt_ivci, NULL, &target_view);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateImageView(target) -> %s",
                vkfw_result_str(r)))
      goto out;

   if (!vkfw_buffer_create(&fw, READBACK_B, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &dst))
      goto out;

   /* --- samplers, descriptors, pipelines --------------------------- */
   VkSamplerCreateInfo sci = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_NEAREST,
      .minFilter = VK_FILTER_NEAREST,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .maxLod = (float)(SRC_LEVELS - 1),
      .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
   };
   r = fw.vk.vkCreateSampler(fw.dev, &sci, NULL, &nearest);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateSampler(nearest) -> %s",
                vkfw_result_str(r)))
      goto out;

   sci.magFilter = VK_FILTER_LINEAR;
   sci.minFilter = VK_FILTER_LINEAR;
   r = fw.vk.vkCreateSampler(fw.dev, &sci, NULL, &linear);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateSampler(linear) -> %s",
                vkfw_result_str(r)))
      goto out;

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
   r = fw.vk.vkCreateDescriptorSetLayout(fw.dev, &dslci, NULL, &set_layout);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateDescriptorSetLayout -> %s",
                vkfw_result_str(r)))
      goto out;

   const VkDescriptorPoolSize psize = {
      .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .descriptorCount = 2,
   };
   const VkDescriptorPoolCreateInfo dpci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 2,
      .poolSizeCount = 1,
      .pPoolSizes = &psize,
   };
   r = fw.vk.vkCreateDescriptorPool(fw.dev, &dpci, NULL, &pool);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateDescriptorPool -> %s",
                vkfw_result_str(r)))
      goto out;

   /* Two sets rather than one updated between cases: a descriptor set
    * must not be written while a submit that uses it is still in
    * flight, and two sets make that impossible to get wrong here. */
   const VkDescriptorSetLayout layouts[2] = { set_layout, set_layout };
   VkDescriptorSet sets[2];
   const VkDescriptorSetAllocateInfo dsai = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = pool,
      .descriptorSetCount = 2,
      .pSetLayouts = layouts,
   };
   r = fw.vk.vkAllocateDescriptorSets(fw.dev, &dsai, sets);
   if (!t_check(t, r == VK_SUCCESS, "vkAllocateDescriptorSets(2) -> %s",
                vkfw_result_str(r)))
      goto out;
   set_nearest = sets[0];
   set_linear = sets[1];

   const VkDescriptorImageInfo dii[2] = {
      { .sampler = nearest, .imageView = src_view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
      { .sampler = linear, .imageView = src_view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
   };
   const VkWriteDescriptorSet writes[2] = {
      { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set_nearest, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &dii[0] },
      { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set_linear, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &dii[1] },
   };
   /* Returns void and has nothing to check; the cases below are what
    * says whether the descriptors describe what this thinks they do. */
   fw.vk.vkUpdateDescriptorSets(fw.dev, 2, writes, 0, NULL);

   const vkfw_gfx_desc desc_sample = {
      .vs_spv = tex_vert_fullscreen_spv,
      .vs_B = sizeof(tex_vert_fullscreen_spv),
      .fs_spv = tex_frag_sample_spv, .fs_B = sizeof(tex_frag_sample_spv),
      .colour_format = VK_FORMAT_R8G8B8A8_UNORM,
      .depth_format = VK_FORMAT_UNDEFINED,
      .set_layout = set_layout,
      .width = W, .height = H,
   };
   if (!vkfw_gfx_create(&fw, "implicit LOD", &desc_sample, &gfx_sample))
      goto out;

   vkfw_gfx_desc desc_lod1 = desc_sample;
   desc_lod1.fs_spv = tex_frag_lod1_spv;
   desc_lod1.fs_B = sizeof(tex_frag_lod1_spv);
   if (!vkfw_gfx_create(&fw, "explicit LOD 1", &desc_lod1, &gfx_lod1))
      goto out;

   /* --- A: nearest, level 0 ---------------------------------------- */
   if (!vkfw_buffer_poison(&fw, &dst, POISON))
      goto out;
   if (render(&fw, "A: nearest, implicit LOD", &gfx_sample, set_nearest,
              target.img, target_view, &dst)) {
      check_nearest(&fw, (const uint32_t *)dst.map, 0,
                    "A: nearest, implicit LOD");
      vkfw_expect_words(&fw, (const uint32_t *)dst.map + TEXELS, POISON,
                        TAIL_WORDS, "A: nothing was written past the image");
   }

   /* --- B: nearest, level 1 ---------------------------------------- */
   if (vkfw_device_lost(&fw)) {
      t_note(t, "device lost in case A; B and C not attempted");
      goto out;
   }
   if (!vkfw_buffer_poison(&fw, &dst, POISON))
      goto out;
   if (render(&fw, "B: nearest, explicit LOD 1", &gfx_lod1, set_nearest,
              target.img, target_view, &dst)) {
      check_nearest(&fw, (const uint32_t *)dst.map, 1,
                    "B: nearest, explicit LOD 1");
      vkfw_expect_words(&fw, (const uint32_t *)dst.map + TEXELS, POISON,
                        TAIL_WORDS, "B: nothing was written past the image");
   }

   /* --- C: linear, level 0 ----------------------------------------- */
   if (vkfw_device_lost(&fw)) {
      t_note(t, "device lost in case B; C not attempted");
      goto out;
   }
   if (!vkfw_buffer_poison(&fw, &dst, POISON))
      goto out;
   if (render(&fw, "C: linear, implicit LOD", &gfx_sample, set_linear,
              target.img, target_view, &dst)) {
      check_linear(&fw, (const uint32_t *)dst.map, "C: linear, implicit LOD");
      vkfw_expect_words(&fw, (const uint32_t *)dst.map + TEXELS, POISON,
                        TAIL_WORDS, "C: nothing was written past the image");
   }

out:
   vkfw_gfx_destroy(&fw, &gfx_lod1);
   vkfw_gfx_destroy(&fw, &gfx_sample);
   if (pool != VK_NULL_HANDLE)
      fw.vk.vkDestroyDescriptorPool(fw.dev, pool, NULL);
   if (set_layout != VK_NULL_HANDLE)
      fw.vk.vkDestroyDescriptorSetLayout(fw.dev, set_layout, NULL);
   if (linear != VK_NULL_HANDLE)
      fw.vk.vkDestroySampler(fw.dev, linear, NULL);
   if (nearest != VK_NULL_HANDLE)
      fw.vk.vkDestroySampler(fw.dev, nearest, NULL);
   if (target_view != VK_NULL_HANDLE)
      fw.vk.vkDestroyImageView(fw.dev, target_view, NULL);
   if (src_view != VK_NULL_HANDLE)
      fw.vk.vkDestroyImageView(fw.dev, src_view, NULL);
   vkfw_image_destroy(&fw, &target);
   vkfw_image_destroy(&fw, &src);
   vkfw_buffer_destroy(&fw, &staging);
   vkfw_buffer_destroy(&fw, &dst);
   vkfw_finish(&fw);
   return 0;
}
