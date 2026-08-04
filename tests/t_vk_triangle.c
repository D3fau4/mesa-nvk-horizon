/*
 * Phase 5 item 5 — a triangle into an off-screen image.
 *
 * The first draw call this project runs. Everything before it wrote to
 * memory with the copy engine (item 1), with a compute shader (item 2),
 * or with the 3D engine's clear path (items 3 and 4); none of them
 * touched vertex processing, rasterisation or the ROP.
 *
 * HOW THE ANSWER IS CHECKED. Not "a triangle appeared" — every one of
 * the 4096 pixels is compared against what it must hold, and the three
 * quantities that make a coverage failure readable are counted
 * separately: how many pixels hold the triangle's colour, how many hold
 * the clear colour, and how many hold neither. A draw that rendered the
 * wrong half, or filled the target, or produced nothing, differs in the
 * counts; a draw that covered the right *number* of pixels in the wrong
 * *places* differs only in the positional compare, which is why both
 * are here.
 *
 * WHY THE EXPECTED COUNT IS AN INTEGER AND NOT A TOLERANCE. The
 * triangle is stated in pixels — (4,4), (59.5,4), (4,59.5) — and its
 * three edges are x=4, y=4 and x+y=63.5. Single-sample rasterisation
 * tests the pixel centre, and no centre lies on any of those lines: the
 * nearest is half a pixel away, which is far outside anything the
 * subpixel grid could move. So the covered set is decided by geometry
 * alone, with no appeal to the top-left fill rule:
 *
 *     px >= 4 and py >= 4 and px + py <= 62   ->   exactly 1540 pixels
 *
 * The test does not hard-code that predicate. It evaluates the same
 * edge functions the rasteriser must, from the same three vertices, and
 * then checks that its own model still counts 1540 — so a mistake in
 * the model is caught by an arithmetic check rather than blamed on the
 * GPU.
 *
 * THE TWO CASES, AND WHY THEY DIFFER IN TWO WAYS ON PURPOSE:
 *
 *   A  gl_VertexIndex, constant colour, static viewport and scissor.
 *      The smallest draw that exists. If this fails, the failure is in
 *      rasterisation or in the ROP, because nothing else is involved.
 *
 *   B  the same triangle from a vertex buffer, with per-vertex colours
 *      interpolated, and a dynamic viewport and scissor. Three things
 *      case A cannot reach: attribute fetch, interpolation, and the
 *      two dynamic-state commands. B failing while A passes points at
 *      exactly those.
 *
 * B's expected colours are computed, not sampled: with w = 1 at every
 * vertex the perspective divide is the identity, so the colour at a
 * pixel is its screen-space barycentric coordinates against pure red,
 * green and blue. The comparison has a +/-2 tolerance per channel
 * because interpolation is the one thing here that is not exact; the
 * clear colour outside the triangle is compared exactly, and so is the
 * alpha, which is 1.0 at all three vertices and therefore interpolates
 * to exactly 1.0.
 *
 * The clear colour's alpha (0x40) differs from the triangle's (0xff) in
 * both cases, so no interpolated colour can be mistaken for the
 * background: "covered" and "not covered" are distinguishable in the
 * readback whatever the interpolation did.
 *
 * NO SHADER IS COMPILED BEFORE THE FIRST RENDER PASS anywhere else in
 * this suite — see t_vk_image's header — but here one must be, because
 * a draw needs a pipeline. That is not a workaround, it is the item.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "common/vkfw.h"

#include "tri_vert_index.spv.h"
#include "tri_frag_const.spv.h"
#include "tri_vert_attrib.spv.h"
#include "tri_frag_interp.spv.h"

const char *const test_name = "t_vk_triangle";

#define W            64u
#define H            64u
#define TEXELS       (W * H)
#define IMAGE_B      (TEXELS * 4u)
/* A tail past the image, so "the copy wrote outside the region it was
 * given" is measurable and not merely absent. Item 1's L2 defect had
 * exactly that shape and no whole-buffer comparison could see it. */
#define TAIL_WORDS   256u
#define READBACK_B   (IMAGE_B + TAIL_WORDS * 4u)
#define POISON       0xdeadbeefu

/* The count the geometry below produces. Stated here so the test can
 * check its own model, not so the model can be skipped. */
#define EXPECTED_COVERED 1540u

/* Clear colour, and case A's triangle colour. Both are bytes, and both
 * round-trip through R8G8B8A8_UNORM exactly: k/255.0f is within a
 * millionth of k after the conversion back. Case A's four values are
 * the shader's 0.2/0.4/0.6/1.0 — see tri_frag_const.spvasm. */
#define CLEAR_R 0x10u
#define CLEAR_G 0x20u
#define CLEAR_B 0x30u
#define CLEAR_A 0x40u
#define TRI_R   0x33u
#define TRI_G   0x66u
#define TRI_B   0x99u
#define TRI_A   0xffu

/* Interpolation is the only inexact step in case B. Two counts out of
 * 255 is about 0.8%, far above the hardware's barycentric error and far
 * below anything a real interpolation bug would produce. */
#define INTERP_TOLERANCE 2

/* The triangle, in pixels, stated once. tri_vert_index.spvasm carries
 * the NDC form of the same three points; case B's vertex buffer is
 * converted from these at run time, so the two cases cannot silently
 * drift apart in position — only in how the position arrives. */
static const float TRI_PX[3][2] = {
   { 4.0f,  4.0f },
   { 59.5f, 4.0f },
   { 4.0f,  59.5f },
};

/* Twice the signed area of (a, b, s): positive when s is to the left of
 * a->b. The rasteriser's own primitive, and the only geometry this test
 * knows. */
static float edge(const float a[2], const float b[2], float sx, float sy)
{
   return (b[0] - a[0]) * (sy - a[1]) - (b[1] - a[1]) * (sx - a[0]);
}

static float tri_area2(void)
{
   return edge(TRI_PX[0], TRI_PX[1], TRI_PX[2][0], TRI_PX[2][1]);
}

/* Coverage by the rule single-sample rasterisation must follow: the
 * pixel centre inside all three edges. No centre lies on an edge here
 * (the vertex shader's header explains why), so > and >= agree and the
 * fill rule never has to be guessed. */
static bool covers(uint32_t px, uint32_t py)
{
   const float sx = (float)px + 0.5f, sy = (float)py + 0.5f;
   return edge(TRI_PX[0], TRI_PX[1], sx, sy) > 0.0f &&
          edge(TRI_PX[1], TRI_PX[2], sx, sy) > 0.0f &&
          edge(TRI_PX[2], TRI_PX[0], sx, sy) > 0.0f;
}

/* Barycentric coordinates of a pixel centre. lambda[i] belongs to
 * vertex i: the area of the sub-triangle opposite it over the whole. */
static void barycentric(uint32_t px, uint32_t py, float lambda[3])
{
   const float sx = (float)px + 0.5f, sy = (float)py + 0.5f;
   const float a2 = tri_area2();
   lambda[0] = edge(TRI_PX[1], TRI_PX[2], sx, sy) / a2;
   lambda[1] = edge(TRI_PX[2], TRI_PX[0], sx, sy) / a2;
   lambda[2] = edge(TRI_PX[0], TRI_PX[1], sx, sy) / a2;
}

static uint32_t unorm_byte(float v)
{
   if (v <= 0.0f)
      return 0;
   if (v >= 1.0f)
      return 255;
   return (uint32_t)(v * 255.0f + 0.5f);
}

/* R8G8B8A8_UNORM puts R at the lowest address, so a little-endian
 * 32-bit read of one texel is A:B:G:R from the top. */
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

/* Case B's vertex: NDC position, then colour. Laid out to match the
 * attribute descriptions below and nothing else. */
struct vertex {
   float x, y;
   float r, g, b, a;
};

static void image_barrier(vkfw *fw, VkCommandBuffer cb, VkImage img,
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
         .levelCount = 1,
         .layerCount = 1,
      },
   };
   fw->vk.vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0,
                               0, NULL, 0, NULL, 1, &b);
}

/* Records clear + draw + copy-to-buffer for one case, submits it and
 * waits. The wait is vkWaitForFences, which is where Vulkan puts a CPU
 * stall; nothing else here blocks. */
static bool render(vkfw *fw, const char *what, const vkfw_gfx *p,
                   VkImage img, VkImageView view, const vkfw_buffer *vbuf,
                   bool dynamic_viewport, vkfw_buffer *dst)
{
   VkCommandBuffer cb;
   if (!vkfw_cmd_begin(fw, &cb))
      return false;

   image_barrier(fw, cb, img,
                 VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

   VkClearValue clear;
   clear.color = unorm_colour(CLEAR_R, CLEAR_G, CLEAR_B, CLEAR_A);

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
   fw->vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipeline);

   if (dynamic_viewport) {
      const VkViewport viewport = {
         .x = 0.0f, .y = 0.0f,
         .width = (float)W, .height = (float)H,
         .minDepth = 0.0f, .maxDepth = 1.0f,
      };
      const VkRect2D scissor = { .offset = { 0, 0 }, .extent = { W, H } };
      fw->vk.vkCmdSetViewport(cb, 0, 1, &viewport);
      fw->vk.vkCmdSetScissor(cb, 0, 1, &scissor);
   }

   if (vbuf != NULL) {
      const VkDeviceSize offset = 0;
      fw->vk.vkCmdBindVertexBuffers(cb, 0, 1, &vbuf->buf, &offset);
   }

   fw->vk.vkCmdDraw(cb, 3, 1, 0, 0);
   fw->vk.vkCmdEndRendering(cb);

   image_barrier(fw, cb, img,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_ACCESS_TRANSFER_READ_BIT,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT);

   const VkBufferImageCopy region = {
      .bufferOffset = 0,
      .bufferRowLength = 0,
      .bufferImageHeight = 0,
      .imageSubresource = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .mipLevel = 0,
         .baseArrayLayer = 0,
         .layerCount = 1,
      },
      .imageOffset = { 0, 0, 0 },
      .imageExtent = { W, H, 1 },
   };
   fw->vk.vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 dst->buf, 1, &region);

   if (!vkfw_submit_and_wait(fw, cb, what))
      return false;
   return vkfw_buffer_invalidate(fw, dst);
}

/* The three named pixels that make a half-pixel error visible, and the
 * three that make an off-by-one on the two axis edges visible. Each is
 * one check, so a log says which boundary moved instead of only "pixel
 * 2017 differs". */
struct probe {
   uint32_t px, py;
   const char *what;
};
static const struct probe PROBES[] = {
   { 31, 31, "just inside the hypotenuse (31,31)" },
   { 32, 31, "just outside the hypotenuse (32,31)" },
   {  4,  4, "the corner pixel at the two axis edges (4,4)" },
   {  3,  4, "one pixel left of the left edge (3,4)" },
   {  4,  3, "one pixel above the top edge (4,3)" },
   { 10, 10, "well inside (10,10)" },
   { 60, 60, "well outside, past the hypotenuse (60,60)" },
};
#define NUM_PROBES (sizeof(PROBES) / sizeof(PROBES[0]))

/* Case A: every pixel is one of exactly two values. */
static void check_case_a(vkfw *fw, const uint32_t *got)
{
   test_ctx *t = fw->t;
   const uint32_t tri = texel(TRI_R, TRI_G, TRI_B, TRI_A);
   const uint32_t bg = texel(CLEAR_R, CLEAR_G, CLEAR_B, CLEAR_A);

   uint32_t n_tri = 0, n_bg = 0, n_other = 0, first_other = 0;
   for (uint32_t i = 0; i < TEXELS; i++) {
      if (got[i] == tri)
         n_tri++;
      else if (got[i] == bg)
         n_bg++;
      else {
         if (n_other == 0)
            first_other = i;
         n_other++;
      }
   }

   t_check(t, n_other == 0,
           "A: every pixel is either the triangle or the clear colour "
           "(%u are neither; first at (%u,%u) = 0x%08x)",
           n_other, n_other ? first_other % W : 0,
           n_other ? first_other / W : 0,
           n_other ? got[first_other] : 0);
   t_check(t, n_tri == EXPECTED_COVERED,
           "A: %u pixels hold the triangle colour, expected %u",
           n_tri, EXPECTED_COVERED);
   t_check(t, n_bg == TEXELS - EXPECTED_COVERED,
           "A: %u pixels hold the clear colour, expected %u",
           n_bg, TEXELS - EXPECTED_COVERED);

   /* Counts can be right with every pixel in the wrong place. */
   uint32_t wrong = 0, first_wrong = 0;
   for (uint32_t py = 0; py < H; py++) {
      for (uint32_t px = 0; px < W; px++) {
         const uint32_t want = covers(px, py) ? tri : bg;
         if (got[py * W + px] != want) {
            if (wrong == 0)
               first_wrong = py * W + px;
            wrong++;
         }
      }
   }
   t_check(t, wrong == 0,
           "A: every pixel is where the geometry puts it (%u differ; "
           "first at (%u,%u): got 0x%08x)",
           wrong, first_wrong % W, first_wrong / W,
           wrong ? got[first_wrong] : 0);

   for (uint32_t i = 0; i < NUM_PROBES; i++) {
      const struct probe *p = &PROBES[i];
      const uint32_t want = covers(p->px, p->py) ? tri : bg;
      const uint32_t have = got[p->py * W + p->px];
      t_check(t, have == want, "A: %s is %s (0x%08x)",
              p->what, covers(p->px, p->py) ? "covered" : "background",
              have);
   }
}

/* Case B: the covered pixels carry interpolated colour, so the compare
 * is per channel with a tolerance; everything else is exact. */
static void check_case_b(vkfw *fw, const uint32_t *got)
{
   test_ctx *t = fw->t;
   const uint32_t bg = texel(CLEAR_R, CLEAR_G, CLEAR_B, CLEAR_A);

   uint32_t bad_bg = 0, first_bad_bg = 0;
   uint32_t bad_tri = 0, first_bad_tri = 0;
   uint32_t worst = 0;
   /* Counted from the readback, not from the model: the clear colour's
    * alpha is 0x40 and all three vertices carry alpha 1.0, so an opaque
    * pixel is one the draw produced whatever its colour turned out to
    * be. That makes coverage measurable in case B without depending on
    * interpolation being right. */
   uint32_t n_opaque = 0;

   for (uint32_t py = 0; py < H; py++) {
      for (uint32_t px = 0; px < W; px++) {
         const uint32_t have = got[py * W + px];

         if ((have >> 24) == 0xffu)
            n_opaque++;

         if (!covers(px, py)) {
            if (have != bg) {
               if (bad_bg == 0)
                  first_bad_bg = py * W + px;
               bad_bg++;
            }
            continue;
         }

         float lambda[3];
         barycentric(px, py, lambda);
         const uint32_t want_c[4] = {
            unorm_byte(lambda[0]), unorm_byte(lambda[1]),
            unorm_byte(lambda[2]), 255u,
         };
         const uint32_t have_c[4] = {
            have & 0xffu, (have >> 8) & 0xffu,
            (have >> 16) & 0xffu, (have >> 24) & 0xffu,
         };

         bool ok = true;
         for (uint32_t k = 0; k < 4; k++) {
            const uint32_t d = have_c[k] > want_c[k]
                                  ? have_c[k] - want_c[k]
                                  : want_c[k] - have_c[k];
            if (d > worst)
               worst = d;
            /* Alpha is 1.0 at all three vertices, so it interpolates to
             * exactly 1.0 and gets no tolerance at all. */
            if (d > (k == 3 ? 0u : (uint32_t)INTERP_TOLERANCE))
               ok = false;
         }
         if (!ok) {
            if (bad_tri == 0)
               first_bad_tri = py * W + px;
            bad_tri++;
         }
      }
   }

   t_check(t, n_opaque == EXPECTED_COVERED,
           "B: %u pixels came back opaque, expected %u",
           n_opaque, EXPECTED_COVERED);
   t_check(t, bad_bg == 0,
           "B: every pixel outside the triangle is exactly the clear "
           "colour (%u differ; first at (%u,%u) = 0x%08x)",
           bad_bg, first_bad_bg % W, first_bad_bg / W,
           bad_bg ? got[first_bad_bg] : 0);
   t_check(t, bad_tri == 0,
           "B: every covered pixel holds its interpolated colour within "
           "%d (%u differ; first at (%u,%u) = 0x%08x)",
           INTERP_TOLERANCE, bad_tri, first_bad_tri % W, first_bad_tri / W,
           bad_tri ? got[first_bad_tri] : 0);
   t_note(t, "B: largest deviation from the computed colour: %u of 255",
          worst);

   for (uint32_t i = 0; i < NUM_PROBES; i++) {
      const struct probe *p = &PROBES[i];
      const uint32_t have = got[p->py * W + p->px];
      if (!covers(p->px, p->py)) {
         t_check(t, have == bg, "B: %s is background (0x%08x)",
                 p->what, have);
         continue;
      }
      float lambda[3];
      barycentric(p->px, p->py, lambda);
      const uint32_t want_c[4] = {
         unorm_byte(lambda[0]), unorm_byte(lambda[1]),
         unorm_byte(lambda[2]), 255u,
      };
      const uint32_t have_c[4] = {
         have & 0xffu, (have >> 8) & 0xffu,
         (have >> 16) & 0xffu, (have >> 24) & 0xffu,
      };
      bool ok = true;
      for (uint32_t k = 0; k < 4; k++) {
         const uint32_t d = have_c[k] > want_c[k] ? have_c[k] - want_c[k]
                                                  : want_c[k] - have_c[k];
         if (d > (k == 3 ? 0u : (uint32_t)INTERP_TOLERANCE))
            ok = false;
      }
      t_check(t, ok,
              "B: %s interpolates to R=%u G=%u B=%u A=%u "
              "(expected %u %u %u %u)",
              p->what, have_c[0], have_c[1], have_c[2], have_c[3],
              want_c[0], want_c[1], want_c[2], want_c[3]);
   }
}

int run_test(test_ctx *t)
{
   /* vkCmdBeginRendering is core 1.3 but the feature still has to be
    * asked for; the fixture enables nothing on its own. */
   VkPhysicalDeviceVulkan13Features features13 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
      .dynamicRendering = VK_TRUE,
   };

   vkfw fw;
   if (!vkfw_init(&fw, t, &features13))
      return 1;

   vkfw_gfx pa = { 0 }, pb = { 0 };
   vkfw_buffer dst = { 0 }, vbuf = { 0 };
   vkfw_image img = { 0 };
   VkImageView view = VK_NULL_HANDLE;

   const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

   if (!vkfw_buffer_create(&fw, READBACK_B, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &dst))
      goto out;

   if (!vkfw_image_supported(&fw, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
                             VK_IMAGE_TILING_OPTIMAL, usage, "target"))
      goto out;
   if (!vkfw_image_create(&fw, VK_FORMAT_R8G8B8A8_UNORM,
                          (VkExtent3D){ W, H, 1 }, 1, 1, usage,
                          VK_IMAGE_TILING_OPTIMAL, &img))
      goto out;

   const VkImageViewCreateInfo ivci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = img.img,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .levelCount = 1,
         .layerCount = 1,
      },
   };
   VkResult r = fw.vk.vkCreateImageView(fw.dev, &ivci, NULL, &view);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateImageView -> %s",
                vkfw_result_str(r)))
      goto out;

   /* The model, checked before anything is asked of the GPU: if this
    * count is not 1540 the geometry above was edited and the expected
    * results below no longer mean what the header says they do. */
   uint32_t model = 0;
   for (uint32_t py = 0; py < H; py++)
      for (uint32_t px = 0; px < W; px++)
         model += covers(px, py) ? 1u : 0u;
   if (!t_check(t, model == EXPECTED_COVERED,
                "the coverage model counts %u pixels, expected %u",
                model, EXPECTED_COVERED))
      goto out;

   /* --- case A --------------------------------------------------- */
   const vkfw_gfx_desc desc_a = {
      .vs_spv = tri_vert_index_spv, .vs_B = sizeof(tri_vert_index_spv),
      .fs_spv = tri_frag_const_spv, .fs_B = sizeof(tri_frag_const_spv),
      .colour_format = VK_FORMAT_R8G8B8A8_UNORM,
      .depth_format = VK_FORMAT_UNDEFINED,
      .width = W, .height = H,
      /* No vertex input and a static viewport: the two things case B
       * adds, absent here so that B failing alone names them. */
   };
   if (!vkfw_gfx_create(&fw, "A", &desc_a, &pa))
      goto out;

   if (!vkfw_buffer_poison(&fw, &dst, POISON))
      goto out;
   if (render(&fw, "A: gl_VertexIndex, constant colour", &pa, img.img, view,
              NULL, false, &dst)) {
      check_case_a(&fw, (const uint32_t *)dst.map);
      vkfw_expect_words(&fw, (const uint32_t *)dst.map + TEXELS, POISON,
                        TAIL_WORDS,
                        "A: nothing was written past the image");
   }

   /* --- case B --------------------------------------------------- */
   if (vkfw_device_lost(&fw)) {
      t_note(t, "device lost in case A; case B not attempted");
      goto out;
   }

   if (!vkfw_buffer_create(&fw, sizeof(struct vertex) * 3,
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &vbuf))
      goto out;
   if (!t_check(t, vbuf.map != NULL, "the vertex buffer is mapped"))
      goto out;

   /* Converted here from the same TRI_PX the coverage model uses, by
    * the transform the viewport applies in reverse. */
   const float colours[3][4] = {
      { 1.0f, 0.0f, 0.0f, 1.0f },
      { 0.0f, 1.0f, 0.0f, 1.0f },
      { 0.0f, 0.0f, 1.0f, 1.0f },
   };
   struct vertex verts[3];
   for (uint32_t i = 0; i < 3; i++) {
      verts[i].x = 2.0f * TRI_PX[i][0] / (float)W - 1.0f;
      verts[i].y = 2.0f * TRI_PX[i][1] / (float)H - 1.0f;
      verts[i].r = colours[i][0];
      verts[i].g = colours[i][1];
      verts[i].b = colours[i][2];
      verts[i].a = colours[i][3];
   }
   memcpy(vbuf.map, verts, sizeof(verts));
   if (!vkfw_buffer_flush(&fw, &vbuf))
      goto out;

   const VkVertexInputBindingDescription binding = {
      .binding = 0,
      .stride = sizeof(struct vertex),
      .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
   };
   const VkVertexInputAttributeDescription attrs[2] = {
      { .location = 0, .binding = 0,
        .format = VK_FORMAT_R32G32_SFLOAT,
        .offset = offsetof(struct vertex, x) },
      { .location = 1, .binding = 0,
        .format = VK_FORMAT_R32G32B32A32_SFLOAT,
        .offset = offsetof(struct vertex, r) },
   };
   const vkfw_gfx_desc desc_b = {
      .vs_spv = tri_vert_attrib_spv, .vs_B = sizeof(tri_vert_attrib_spv),
      .fs_spv = tri_frag_interp_spv, .fs_B = sizeof(tri_frag_interp_spv),
      .colour_format = VK_FORMAT_R8G8B8A8_UNORM,
      .depth_format = VK_FORMAT_UNDEFINED,
      .binding_count = 1, .bindings = &binding,
      .attr_count = 2, .attrs = attrs,
      .width = W, .height = H,
      .dynamic_viewport = true,
   };
   if (!vkfw_gfx_create(&fw, "B", &desc_b, &pb))
      goto out;

   if (!vkfw_buffer_poison(&fw, &dst, POISON))
      goto out;
   if (render(&fw, "B: vertex buffer, interpolated colour", &pb, img.img,
              view, &vbuf, true, &dst)) {
      check_case_b(&fw, (const uint32_t *)dst.map);
      vkfw_expect_words(&fw, (const uint32_t *)dst.map + TEXELS, POISON,
                        TAIL_WORDS,
                        "B: nothing was written past the image");
   }

out:
   vkfw_gfx_destroy(&fw, &pb);
   vkfw_gfx_destroy(&fw, &pa);
   if (view != VK_NULL_HANDLE)
      fw.vk.vkDestroyImageView(fw.dev, view, NULL);
   vkfw_image_destroy(&fw, &img);
   vkfw_buffer_destroy(&fw, &vbuf);
   vkfw_buffer_destroy(&fw, &dst);
   vkfw_finish(&fw);
   return 0;
}
