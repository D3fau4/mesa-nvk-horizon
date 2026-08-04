/*
 * Phase 5 item 6 — textures.
 *
 * A source image is uploaded, READ BACK AND VERIFIED, and then sampled
 * into a 64x64 target three ways, with every one of the 4096 result
 * pixels compared against what the sampler must have produced.
 *
 * WHY THE SOURCE IS VERIFIED FIRST, AND WHY THAT IS NOT PARANOIA. Run 1
 * of this test failed on hardware (2026-08-04, batch 5): rows 4 and 5
 * of an 8x8 source read as transparent black through the sampler while
 * rows 0-3 and 6-7 were right, and mip level 1 was entirely right. The
 * two failures were consistent to the byte — case C's first bad pixel
 * came back 7/92/16/239 where 8/92/16/239 is exactly what blending
 * 0.9375 of texel (0,3) with 0.0625 of a *zero* texel (0,4) gives.
 *
 * And the test could not say whether the upload had not landed or the
 * sampler was reading the wrong place, because it had never looked at
 * the source. Every expected value in it was built on the assumption
 * that the upload worked. So now the source comes back out through
 * vkCmdCopyImageToBuffer before anything samples it, and the answer is
 * attributed instead of guessed:
 *
 *   source readback wrong -> the upload, or the layout it wrote into
 *   source readback right, sampling wrong -> the texture descriptor
 *
 * THE THREE SOURCES, WHICH BRACKET THE OBVIOUS SUSPECT. A Maxwell GOB
 * is 64 bytes by 8 rows. An 8x8 RGBA8 image is 32 bytes by 8 rows —
 * half a GOB wide — and its rows 4 and 5 are exactly the third 64-byte
 * group of the GOB's swizzle. A 64x64 source is a full GOB wide and
 * eight GOBs tall, so if the fault is about surfaces narrower than a
 * GOB it appears in one and not the other. The linear source removes
 * tiling from the question altogether.
 *
 * THE THREE SAMPLING CASES, per source:
 *
 *   A  nearest, implicit LOD. The descriptor, the sampler, the
 *      addressing and the texel fetch, with the level chosen by the
 *      derivatives.
 *   B  nearest, explicit LOD 1. Mip selection as the measurement
 *      instead of a by-product. Skipped for a source with one level.
 *   C  linear, implicit LOD. The filter unit, against the same four
 *      texels it weighs.
 *
 * WHY THE NEAREST CASES ARE EXACT. The full-target triangle makes
 * uv = (fx/64, fy/64), so a pixel centre samples at texel coordinate
 * (px + 0.5) * Ws / 64 on a source Ws wide. With Ws dividing 64 that is
 * px / (64/Ws) by integer division and never lands within half a
 * sample of a texel boundary, so there is no tolerance in A or B.
 *
 * WHY C STILL GETS ONE, AND A SMALL ONE. The fractional texel position
 * at every pixel centre is an odd multiple of 1/(2*64/Ws), exact in any
 * subtexel fixed-point format with enough bits, so weight quantisation
 * contributes nothing. What is left is the final round to UNORM8, where
 * the exact answer can land on a tie — run 1 produced one, 7.5, which
 * this hardware rounds down and a naive model rounds up. Two counts
 * covers that; the largest deviation actually seen is reported either
 * way, so a filter that is merely nearly right cannot hide inside the
 * tolerance.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdio.h>
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

/* The largest source, so one staging buffer and one verify buffer
 * serve every source case: 64x64 plus its 32x32 level 1. */
#define MAX_SRC      64u
#define SRC_B        ((MAX_SRC * MAX_SRC + (MAX_SRC / 2) * (MAX_SRC / 2)) * 4u)
#define SRC_TAIL_W   256u
#define SRC_BUF_B    (SRC_B + SRC_TAIL_W * 4u)

#define MAX_LEVELS   2u
#define INTERP_TOLERANCE 2

static uint32_t texel(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
   return (a << 24) | (b << 16) | (g << 8) | r;
}

/* The source pattern.
 *
 * Multiplying by an odd number is a bijection modulo 256, so r
 * identifies x and g identifies y for any source up to 256 wide — one
 * texel names its own position, and a sampler reading the wrong texel
 * says which one it read. The blue channel's low bit carries the mip
 * level, so no texel of level 1 can equal one of level 0 whatever the
 * coordinates. Both properties are checked at run time rather than
 * asserted here. */
static uint32_t src_texel(uint32_t level, uint32_t x, uint32_t y)
{
   const uint32_t r = (x * 7u + 13u) & 0xffu;
   const uint32_t g = (y * 11u + 29u) & 0xffu;
   const uint32_t b = (((x * 13u) ^ (y * 7u)) & 0xfeu) | (level & 1u);
   return texel(r, g, b, 0xffu);
}

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

/* A source configuration. The sampling cases are the same for all of
 * them; what changes is the surface the sampler has to address. */
struct source {
   const char *name;
   uint32_t w, h;
   uint32_t levels;
   VkImageTiling tiling;
};

static const struct source SOURCES[] = {
   /* The one that failed in run 1: half a GOB wide, exactly one GOB
    * tall. Its rows 4 and 5 are the third 64-byte group of the GOB
    * swizzle. */
   { "8x8 optimal", 8, 8, 2, VK_IMAGE_TILING_OPTIMAL },
   /* A full GOB wide and eight tall. If the fault is about surfaces
    * narrower than a GOB, this one is clean and the one above is not. */
   { "64x64 optimal", 64, 64, 2, VK_IMAGE_TILING_OPTIMAL },
   /* No tiling at all, and one level: LINEAR images are limited to one
    * mip on most implementations, so this source runs cases A and C
    * only. If this passes where the tiled 8x8 fails, the fault is in
    * the tiled layout and not in the sampler or the copy. */
   { "8x8 linear", 8, 8, 1, VK_IMAGE_TILING_LINEAR },
};
#define NUM_SOURCES (sizeof(SOURCES) / sizeof(SOURCES[0]))

static uint32_t level_w(const struct source *s, uint32_t l)
{
   return l == 0 ? s->w : s->w >> l;
}
static uint32_t level_h(const struct source *s, uint32_t l)
{
   return l == 0 ? s->h : s->h >> l;
}

/* Byte offset of a level inside the staging and verify buffers: levels
 * packed back to back, tightly, in order. */
static uint32_t level_offset_B(const struct source *s, uint32_t l)
{
   uint32_t off = 0;
   for (uint32_t i = 0; i < l; i++)
      off += level_w(s, i) * level_h(s, i) * 4u;
   return off;
}

/* The texel a nearest-filtered pixel centre lands in. See the header:
 * (px + 0.5) * Ws / 64 with Ws dividing 64 is px / (64/Ws), and never
 * on a boundary. */
static uint32_t nearest_texel_x(const struct source *s, uint32_t l,
                                uint32_t px)
{
   return px / (W / level_w(s, l));
}
static uint32_t nearest_texel_y(const struct source *s, uint32_t l,
                                uint32_t py)
{
   return py / (H / level_h(s, l));
}

/* CLAMP_TO_EDGE bilinear filtering of level 0 at a pixel centre: the
 * same four texels and the same two lerps the hardware does. */
static void bilinear(const struct source *s, uint32_t px, uint32_t py,
                     uint32_t out[4])
{
   const uint32_t sw = level_w(s, 0), sh = level_h(s, 0);
   const float tu = ((float)px + 0.5f) / (float)W * (float)sw - 0.5f;
   const float tv = ((float)py + 0.5f) / (float)H * (float)sh - 0.5f;
   const int32_t i = ifloor(tu), j = ifloor(tv);
   const float fu = tu - (float)i, fv = tv - (float)j;

   const uint32_t x0 = (uint32_t)clampi(i, 0, (int32_t)sw - 1);
   const uint32_t x1 = (uint32_t)clampi(i + 1, 0, (int32_t)sw - 1);
   const uint32_t y0 = (uint32_t)clampi(j, 0, (int32_t)sh - 1);
   const uint32_t y1 = (uint32_t)clampi(j + 1, 0, (int32_t)sh - 1);

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

/* Fills the copy regions for every level of `s`, packed. */
static void level_regions(const struct source *s, VkBufferImageCopy *out)
{
   for (uint32_t l = 0; l < s->levels; l++) {
      out[l] = (VkBufferImageCopy){
         .bufferOffset = level_offset_B(s, l),
         .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = l,
            .baseArrayLayer = 0,
            .layerCount = 1,
         },
         .imageExtent = { level_w(s, l), level_h(s, l), 1 },
      };
   }
}

/* THE CHECK RUN 1 DID NOT HAVE. Compares what came back out of the
 * source image against what was put in, level by level, and names the
 * first and last texel that differ — because "1024 wrong starting at
 * (0,32)" and "1024 wrong from (0,32) to (63,47)" are different
 * findings and only the second one identifies a band of rows. */
static bool check_source(vkfw *fw, const struct source *s,
                         const uint32_t *back)
{
   test_ctx *t = fw->t;
   bool all_ok = true;

   for (uint32_t l = 0; l < s->levels; l++) {
      const uint32_t lw = level_w(s, l), lh = level_h(s, l);
      const uint32_t *lv = back + level_offset_B(s, l) / 4u;
      uint32_t wrong = 0, first = 0, last = 0, zeroes = 0;

      for (uint32_t y = 0; y < lh; y++) {
         for (uint32_t x = 0; x < lw; x++) {
            const uint32_t got = lv[y * lw + x];
            if (got == src_texel(l, x, y))
               continue;
            if (wrong == 0)
               first = y * lw + x;
            last = y * lw + x;
            if (got == 0)
               zeroes++;
            wrong++;
         }
      }

      const bool ok = t_check(t, wrong == 0,
                              "%s: level %u came back as it went in "
                              "(%u/%u texels)", s->name, l,
                              lw * lh - wrong, lw * lh);
      all_ok = all_ok && ok;
      if (wrong != 0) {
         t_note(t, "%s: level %u wrong from (%u,%u) to (%u,%u), %u of %u "
                   "are zero; first got 0x%08x want 0x%08x",
                s->name, l, first % lw, first / lw, last % lw, last / lw,
                zeroes, wrong, lv[first],
                src_texel(l, first % lw, first / lw));
      }
   }
   return all_ok;
}

/* One sampling case: clear, draw the full-target triangle with `set`
 * bound, copy the result back. */
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

   /* Transparent black, which no source texel can be: every one of them
    * is opaque. A pixel the draw failed to cover is distinguishable
    * from one it covered, and so is a texel that read as nothing. */
   VkClearValue clear;
   memset(&clear, 0, sizeof(clear));

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
         .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1,
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
static void check_nearest(vkfw *fw, const struct source *s, uint32_t level,
                          const uint32_t *got, const char *what)
{
   uint32_t wrong = 0, first = 0, last = 0;

   for (uint32_t py = 0; py < H; py++) {
      for (uint32_t px = 0; px < W; px++) {
         const uint32_t want = src_texel(level, nearest_texel_x(s, level, px),
                                         nearest_texel_y(s, level, py));
         if (got[py * W + px] != want) {
            if (wrong == 0)
               first = py * W + px;
            last = py * W + px;
            wrong++;
         }
      }
   }

   t_check(fw->t, wrong == 0, "%s: %u/%u pixels hold the texel they sampled",
           what, TEXELS - wrong, TEXELS);
   if (wrong != 0) {
      const uint32_t px = first % W, py = first / W;
      t_note(fw->t, "%s: wrong from (%u,%u) to (%u,%u); first got 0x%08x, "
                    "want 0x%08x (texel %u,%u of level %u)",
             what, px, py, last % W, last / W, got[first],
             src_texel(level, nearest_texel_x(s, level, px),
                       nearest_texel_y(s, level, py)),
             nearest_texel_x(s, level, px), nearest_texel_y(s, level, py),
             level);
   }
}

/* Case C: per channel, within the tolerance, worst deviation reported
 * whatever the verdict. */
static void check_linear(vkfw *fw, const struct source *s,
                         const uint32_t *got, const char *what)
{
   uint32_t wrong = 0, first = 0, last = 0, worst = 0;

   for (uint32_t py = 0; py < H; py++) {
      for (uint32_t px = 0; px < W; px++) {
         uint32_t want[4];
         bilinear(s, px, py, want);
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
            last = py * W + px;
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
      bilinear(s, first % W, first / W, want);
      t_note(fw->t, "%s: outside tolerance from (%u,%u) to (%u,%u); first "
                    "got %u %u %u %u, want %u %u %u %u",
             what, first % W, first / W, last % W, last / W,
             chan(got[first], 0), chan(got[first], 1),
             chan(got[first], 2), chan(got[first], 3),
             want[0], want[1], want[2], want[3]);
   }
}

/* The two properties every expected value below rests on, checked on
 * the CPU before the GPU is involved. */
static bool check_pattern(vkfw *fw)
{
   test_ctx *t = fw->t;
   uint32_t dup = 0, cross = 0;

   /* Unique within a level, over the largest source any case uses. */
   for (uint32_t y = 0; y < MAX_SRC; y++)
      for (uint32_t x = 0; x < MAX_SRC; x++)
         for (uint32_t x2 = x + 1; x2 < MAX_SRC; x2++)
            if (src_texel(0, x, y) == src_texel(0, x2, y))
               dup++;
   for (uint32_t x = 0; x < MAX_SRC; x++)
      for (uint32_t y = 0; y < MAX_SRC; y++)
         for (uint32_t y2 = y + 1; y2 < MAX_SRC; y2++)
            if (src_texel(0, x, y) == src_texel(0, x, y2))
               dup++;
   if (!t_check(t, dup == 0,
                "a source texel names its own position (%u collisions)",
                dup))
      return false;

   /* And no texel of level 1 can be mistaken for one of level 0. */
   for (uint32_t y = 0; y < MAX_SRC; y++)
      for (uint32_t x = 0; x < MAX_SRC; x++)
         for (uint32_t y1 = 0; y1 < MAX_SRC / 2; y1++)
            for (uint32_t x1 = 0; x1 < MAX_SRC / 2; x1++)
               if (src_texel(0, x, y) == src_texel(1, x1, y1))
                  cross++;
   return t_check(t, cross == 0,
                  "no texel of level 1 equals one of level 0 (%u do)",
                  cross);
}

struct sampling {
   vkfw_gfx gfx_sample, gfx_lod1;
   VkDescriptorSetLayout set_layout;
   VkDescriptorPool pool;
   VkSampler nearest, linear;
};

static void run_source(vkfw *fw, const struct source *s,
                       const struct sampling *smp, vkfw_buffer *staging,
                       vkfw_buffer *src_back, vkfw_buffer *dst,
                       VkImage target, VkImageView target_view)
{
   test_ctx *t = fw->t;
   const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

   if (!vkfw_image_supported(fw, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
                             s->tiling, usage, s->name)) {
      t_note(t, "%s: the driver does not support this source "
                "configuration; skipped, not failed", s->name);
      return;
   }

   vkfw_image src = { 0 };
   VkImageView view = VK_NULL_HANDLE;
   VkDescriptorSet sets[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };

   if (!vkfw_image_create(fw, VK_FORMAT_R8G8B8A8_UNORM,
                          (VkExtent3D){ s->w, s->h, 1 }, s->levels, 1,
                          usage, s->tiling, &src))
      return;

   /* ---- fill the staging buffer and poison the readback ---------- */
   uint32_t *stage = (uint32_t *)staging->map;
   for (uint32_t l = 0; l < s->levels; l++) {
      uint32_t *lv = stage + level_offset_B(s, l) / 4u;
      for (uint32_t y = 0; y < level_h(s, l); y++)
         for (uint32_t x = 0; x < level_w(s, l); x++)
            lv[y * level_w(s, l) + x] = src_texel(l, x, y);
   }
   if (!vkfw_buffer_flush(fw, staging))
      goto out;
   if (!vkfw_buffer_poison(fw, src_back, POISON))
      goto out;

   /* ---- upload, and read straight back out ----------------------- */
   {
      VkCommandBuffer cb;
      if (!vkfw_cmd_begin(fw, &cb))
         goto out;

      image_barrier(fw, cb, src.img, s->levels,
                    VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);

      VkBufferImageCopy regions[MAX_LEVELS];
      level_regions(s, regions);
      fw->vk.vkCmdCopyBufferToImage(cb, staging->buf, src.img,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    s->levels, regions);

      image_barrier(fw, cb, src.img, s->levels,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);

      fw->vk.vkCmdCopyImageToBuffer(cb, src.img,
                                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    src_back->buf, s->levels, regions);

      image_barrier(fw, cb, src.img, s->levels,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

      if (!vkfw_submit_and_wait(fw, cb, "upload and read back the source"))
         goto out;
      if (!vkfw_buffer_invalidate(fw, src_back))
         goto out;
   }

   const bool source_intact = check_source(fw, s, (const uint32_t *)
                                           src_back->map);
   if (!source_intact) {
      /* The sampling cases would now fail on the source's contents and
       * say nothing about sampling. Reporting three more failures for
       * one cause is noise. */
      t_note(t, "%s: the source did not survive the round trip, so the "
                "sampling cases below would be measuring the upload; "
                "skipped", s->name);
      goto out;
   }

   /* ---- descriptors --------------------------------------------- */
   const VkImageViewCreateInfo ivci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = src.img,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .levelCount = s->levels, .layerCount = 1,
      },
   };
   VkResult r = fw->vk.vkCreateImageView(fw->dev, &ivci, NULL, &view);
   if (!t_check(t, r == VK_SUCCESS, "%s: vkCreateImageView(%u level(s)) "
                "-> %s", s->name, s->levels, vkfw_result_str(r)))
      goto out;

   const VkDescriptorSetLayout layouts[2] = { smp->set_layout,
                                              smp->set_layout };
   const VkDescriptorSetAllocateInfo dsai = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = smp->pool,
      .descriptorSetCount = 2,
      .pSetLayouts = layouts,
   };
   r = fw->vk.vkAllocateDescriptorSets(fw->dev, &dsai, sets);
   if (!t_check(t, r == VK_SUCCESS, "%s: vkAllocateDescriptorSets(2) -> %s",
                s->name, vkfw_result_str(r)))
      goto out;

   const VkDescriptorImageInfo dii[2] = {
      { .sampler = smp->nearest, .imageView = view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
      { .sampler = smp->linear, .imageView = view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
   };
   const VkWriteDescriptorSet writes[2] = {
      { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = sets[0], .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &dii[0] },
      { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = sets[1], .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &dii[1] },
   };
   fw->vk.vkUpdateDescriptorSets(fw->dev, 2, writes, 0, NULL);

   char what[80];

   /* ---- A: nearest, level 0 -------------------------------------- */
   snprintf(what, sizeof(what), "%s A: nearest, implicit LOD", s->name);
   if (!vkfw_buffer_poison(fw, dst, POISON))
      goto out;
   if (render(fw, what, &smp->gfx_sample, sets[0], target, target_view,
              dst)) {
      check_nearest(fw, s, 0, (const uint32_t *)dst->map, what);
      vkfw_expect_words(fw, (const uint32_t *)dst->map + TEXELS, POISON,
                        TAIL_WORDS, "nothing was written past the image");
   }

   /* ---- B: nearest, level 1 -------------------------------------- */
   if (s->levels > 1 && !vkfw_device_lost(fw)) {
      snprintf(what, sizeof(what), "%s B: nearest, explicit LOD 1", s->name);
      if (!vkfw_buffer_poison(fw, dst, POISON))
         goto out;
      if (render(fw, what, &smp->gfx_lod1, sets[0], target, target_view,
                 dst)) {
         check_nearest(fw, s, 1, (const uint32_t *)dst->map, what);
         vkfw_expect_words(fw, (const uint32_t *)dst->map + TEXELS, POISON,
                           TAIL_WORDS, "nothing was written past the image");
      }
   } else if (s->levels == 1) {
      t_note(t, "%s: one mip level, so there is no explicit-LOD case",
             s->name);
   }

   /* ---- C: linear, level 0 --------------------------------------- */
   if (!vkfw_device_lost(fw)) {
      snprintf(what, sizeof(what), "%s C: linear, implicit LOD", s->name);
      if (!vkfw_buffer_poison(fw, dst, POISON))
         goto out;
      if (render(fw, what, &smp->gfx_sample, sets[1], target, target_view,
                 dst)) {
         check_linear(fw, s, (const uint32_t *)dst->map, what);
         vkfw_expect_words(fw, (const uint32_t *)dst->map + TEXELS, POISON,
                           TAIL_WORDS, "nothing was written past the image");
      }
   }

out:
   if (view != VK_NULL_HANDLE)
      fw->vk.vkDestroyImageView(fw->dev, view, NULL);
   vkfw_image_destroy(fw, &src);
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

   vkfw_buffer dst = { 0 }, staging = { 0 }, src_back = { 0 };
   vkfw_image target = { 0 };
   VkImageView target_view = VK_NULL_HANDLE;
   struct sampling smp = { 0 };
   VkResult r;

   if (!check_pattern(&fw))
      goto out;

   /* ---- the render target ---------------------------------------- */
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
         .levelCount = 1, .layerCount = 1,
      },
   };
   r = fw.vk.vkCreateImageView(fw.dev, &tgt_ivci, NULL, &target_view);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateImageView(target) -> %s",
                vkfw_result_str(r)))
      goto out;

   if (!vkfw_buffer_create(&fw, READBACK_B, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &dst))
      goto out;
   if (!vkfw_buffer_create(&fw, SRC_BUF_B, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &staging))
      goto out;
   if (!t_check(t, staging.map != NULL, "the staging buffer is mapped"))
      goto out;
   if (!vkfw_buffer_create(&fw, SRC_BUF_B, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &src_back))
      goto out;

   /* ---- samplers, layout, pool, pipelines ------------------------ */
   VkSamplerCreateInfo sci = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_NEAREST,
      .minFilter = VK_FILTER_NEAREST,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .maxLod = (float)(MAX_LEVELS - 1),
      .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
   };
   r = fw.vk.vkCreateSampler(fw.dev, &sci, NULL, &smp.nearest);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateSampler(nearest) -> %s",
                vkfw_result_str(r)))
      goto out;

   sci.magFilter = VK_FILTER_LINEAR;
   sci.minFilter = VK_FILTER_LINEAR;
   r = fw.vk.vkCreateSampler(fw.dev, &sci, NULL, &smp.linear);
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
   r = fw.vk.vkCreateDescriptorSetLayout(fw.dev, &dslci, NULL,
                                         &smp.set_layout);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateDescriptorSetLayout -> %s",
                vkfw_result_str(r)))
      goto out;

   /* Two sets per source, and sources are not torn down between cases,
    * so the pool holds two for each. A descriptor set must not be
    * rewritten while a submit that uses it is in flight, and separate
    * sets make that impossible to get wrong. */
   const VkDescriptorPoolSize psize = {
      .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .descriptorCount = 2u * NUM_SOURCES,
   };
   const VkDescriptorPoolCreateInfo dpci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 2u * NUM_SOURCES,
      .poolSizeCount = 1,
      .pPoolSizes = &psize,
   };
   r = fw.vk.vkCreateDescriptorPool(fw.dev, &dpci, NULL, &smp.pool);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateDescriptorPool -> %s",
                vkfw_result_str(r)))
      goto out;

   const vkfw_gfx_desc desc_sample = {
      .vs_spv = tex_vert_fullscreen_spv,
      .vs_B = sizeof(tex_vert_fullscreen_spv),
      .fs_spv = tex_frag_sample_spv, .fs_B = sizeof(tex_frag_sample_spv),
      .colour_format = VK_FORMAT_R8G8B8A8_UNORM,
      .depth_format = VK_FORMAT_UNDEFINED,
      .set_layout = smp.set_layout,
      .width = W, .height = H,
   };
   if (!vkfw_gfx_create(&fw, "implicit LOD", &desc_sample, &smp.gfx_sample))
      goto out;

   vkfw_gfx_desc desc_lod1 = desc_sample;
   desc_lod1.fs_spv = tex_frag_lod1_spv;
   desc_lod1.fs_B = sizeof(tex_frag_lod1_spv);
   if (!vkfw_gfx_create(&fw, "explicit LOD 1", &desc_lod1, &smp.gfx_lod1))
      goto out;

   for (uint32_t i = 0; i < NUM_SOURCES; i++) {
      if (vkfw_device_lost(&fw)) {
         t_note(t, "device lost; %u source(s) from \"%s\" on not attempted",
                (unsigned)(NUM_SOURCES - i), SOURCES[i].name);
         break;
      }
      run_source(&fw, &SOURCES[i], &smp, &staging, &src_back, &dst,
                 target.img, target_view);
   }

out:
   vkfw_gfx_destroy(&fw, &smp.gfx_lod1);
   vkfw_gfx_destroy(&fw, &smp.gfx_sample);
   if (smp.pool != VK_NULL_HANDLE)
      fw.vk.vkDestroyDescriptorPool(fw.dev, smp.pool, NULL);
   if (smp.set_layout != VK_NULL_HANDLE)
      fw.vk.vkDestroyDescriptorSetLayout(fw.dev, smp.set_layout, NULL);
   if (smp.linear != VK_NULL_HANDLE)
      fw.vk.vkDestroySampler(fw.dev, smp.linear, NULL);
   if (smp.nearest != VK_NULL_HANDLE)
      fw.vk.vkDestroySampler(fw.dev, smp.nearest, NULL);
   if (target_view != VK_NULL_HANDLE)
      fw.vk.vkDestroyImageView(fw.dev, target_view, NULL);
   vkfw_image_destroy(&fw, &target);
   vkfw_buffer_destroy(&fw, &src_back);
   vkfw_buffer_destroy(&fw, &staging);
   vkfw_buffer_destroy(&fw, &dst);
   vkfw_finish(&fw);
   return 0;
}
