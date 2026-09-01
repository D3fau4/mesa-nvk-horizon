/*
 * Draw volume — hundreds of draws, pipeline switches between them, and
 * blending as an answer that is computed rather than compared.
 *
 * WHAT WAS NOT COVERED BEFORE THIS. The largest render this suite had
 * was t_vk_depth's four draws through two pipelines in one render pass.
 * Nothing had ever asked for the shapes an application actually
 * produces: hundreds of draws, the pipeline changing between them, and
 * the same work split across many small render passes instead of one
 * large one. Those are different code paths in NVK — a pipeline bind
 * re-emits state into the push buffer, and a render pass boundary
 * re-emits all of it plus the attachment setup — and both of them are
 * how a command buffer gets long enough to cross a push-buffer chunk,
 * which is where nvk_mem_stream's chunk recycling lives.
 *
 * Blending was excluded from the fixture on purpose until now ("a blend
 * would make the destination part of the answer"). That is exactly why
 * it belongs here: this test wants the destination to be part of the
 * answer, because a blend chain is a computation the CPU can carry out
 * too, and a driver that drops, reorders or double-issues a draw lands
 * on a different number.
 *
 * THE SHAPE. The target is BANDS vertical bands. Every band receives the
 * same number of draws, and each draw is a full-target triangle
 * scissored to its band, so what is measured is never rasterisation —
 * t_vk_triangle owns that question — and always which draws happened, in
 * what order, through which pipeline.
 *
 *   A  DRAWS_PER_BAND draws per band in ONE render pass, cycling
 *      through four pipelines, ending on an opaque draw so the answer
 *      is exact.
 *   B  the same draws split over PASSES render passes, which must
 *      produce the identical image. State re-emitted at PASSES render
 *      pass boundaries is not allowed to change what was drawn.
 *   C  a blend chain with no opaque draw at the end of it, checked
 *      against the same chain evaluated in C.
 *
 * WHY C HAS A TOLERANCE AND A AND B DO NOT. A and B end each band with
 * an opaque draw, so their expected value is a stored colour and the
 * comparison is an equality. C's is the result of BLEND_STEPS rounds of
 *
 *     dst = src * src.a + dst * (1 - src.a)
 *
 * evaluated in fixed point by the ROP and in float by the C below, and
 * the spec pins neither the internal precision nor the rounding of the
 * last bit. The bound is arithmetic rather than a guess: if each round
 * costs at most half a least-significant bit and the round after it
 * scales what came before by (1 - src.a), the accumulated error settles
 * at 0.5 / min(src.a). The alphas here never go below 0x30/255 = 0.188,
 * so 0.5 / 0.188 = 2.7 -- BLEND_TOLERANCE is 3. The worst error actually
 * seen is reported either way, so a console run says how much of that
 * bound this hardware uses.
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

const char *const test_name = "t_vk_draws";
/* No display: main() starts a console and reports through it. */
const bool test_uses_display = false;

#define W              128u
#define H              128u
#define TEXELS         (W * H)
#define IMAGE_B        (TEXELS * 4u)
#define TAIL_WORDS     256u
#define READBACK_B     (IMAGE_B + TAIL_WORDS * 4u)
#define POISON         0xdeadbeefu

#define BANDS          16u
#define BAND_W         (W / BANDS)

/* Sections A and B: this many draws in each band, so 512 draws in all.
 * Chosen to be long enough that the push buffer for section A is not a
 * single small allocation and short enough that the whole test stays
 * inside one console run. */
#define DRAWS_PER_BAND 32u
#define TOTAL_DRAWS    (BANDS * DRAWS_PER_BAND)

/* Section B spreads the same draws over this many render passes. */
#define PASSES         64u
#define DRAWS_PER_PASS (TOTAL_DRAWS / PASSES)

/* Section C's chain length, per band, and how far its result may be
 * from the float evaluation. The 3 is derived in the header. */
#define BLEND_STEPS    12u
#define BLEND_TOLERANCE 3u

#define PIPELINE_COUNT 4u

#define DEPTH_FORMAT   VK_FORMAT_D32_SFLOAT
#define COLOUR_FORMAT  VK_FORMAT_R8G8B8A8_UNORM

#define CLEAR_RGBA     0x05u, 0x06u, 0x07u, 0x08u

/* Depths for sections A and B. Every draw but the last in a band is
 * behind the one before it and therefore rejected; the last is in front
 * of all of them and decides the band. Exact in binary. */
#define Z_WINNER       0.125f
#define Z_FIRST        0.5f
#define Z_STEP         0.015625f   /* 1/64, exact */
#define Z_CLEAR        1.0f

/* Section C draws every step at the same depth with the test off, so
 * the chain is decided by the blend and not by depth. */
#define Z_FLAT         0.5f

struct push_data {
   float colour[4];
   float z;
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

/* Sections A and B: the colour of draw `d` in band `b`. Distinct for
 * every (band, draw), so a pixel holding the wrong one names which draw
 * survived that should not have. */
static uint32_t ab_texel(uint32_t band, uint32_t d)
{
   return texel(0x20u + band * 0xdu, 0x20u + d * 0x7u,
                0x40u + band + d, 0xffu);
}

/* Section C: the source colour of blend step `k` in band `b`, as four
 * bytes. Alpha varies with the step so the chain is not a repetition of
 * one weight — a driver that issued a step twice, or dropped one, would
 * otherwise still land on the same fixed point. */
static void c_source(uint32_t band, uint32_t k, uint32_t rgba[4])
{
   rgba[0] = 0x18u + band * 0xbu + k * 3u;
   rgba[1] = 0x30u + k * 0x9u;
   rgba[2] = 0x50u + band * 5u;
   /* 0x30..0xc0, never 0 or 255: a step that contributed nothing or
    * everything would hide a mistake in the step before it. */
   rgba[3] = 0x30u + ((k * 0x1fu) % 0x91u);
}

static struct push_data push_rgba(const uint32_t rgba[4], float z)
{
   struct push_data p;
   for (uint32_t i = 0; i < 4; i++)
      p.colour[i] = (float)rgba[i] / 255.0f;
   p.z = z;
   return p;
}

static struct push_data push_texel(uint32_t t, float z)
{
   const uint32_t rgba[4] = { t & 0xffu, (t >> 8) & 0xffu,
                              (t >> 16) & 0xffu, (t >> 24) & 0xffu };
   return push_rgba(rgba, z);
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

/* Everything the three sections share. */
struct fixture {
   vkfw_image colour, depth;
   VkImageView colour_view, depth_view;
   vkfw_buffer colour_dst, depth_dst;
   /* Four pipelines that differ only in state the driver has to
    * re-emit when it binds one: two depth-write settings and two
    * compare ops that are equivalent for this geometry. Section A
    * cycles through them so that a bind is not a no-op the driver may
    * legitimately skip. */
   vkfw_gfx gfx[PIPELINE_COUNT];
   /* Section C: blending on, depth test off. */
   vkfw_gfx blend_gfx;
};

/* One draw: its scissor is band `band`, and only the twenty pushed bytes
 * differ between calls. */
static void draw_band(vkfw *fw, VkCommandBuffer cb, const vkfw_gfx *gfx,
                      uint32_t band, const struct push_data *pc)
{
   const VkRect2D scissor = {
      .offset = { (int32_t)(band * BAND_W), 0 },
      .extent = { BAND_W, H },
   };
   fw->vk.vkCmdSetScissor(cb, 0, 1, &scissor);
   fw->vk.vkCmdPushConstants(cb, gfx->layout,
                             VK_SHADER_STAGE_VERTEX_BIT |
                                VK_SHADER_STAGE_FRAGMENT_BIT,
                             0, (uint32_t)sizeof(*pc), pc);
   fw->vk.vkCmdDraw(cb, 3, 1, 0, 0);
}

static void set_viewport(vkfw *fw, VkCommandBuffer cb)
{
   const VkViewport viewport = {
      .x = 0.0f, .y = 0.0f,
      .width = (float)W, .height = (float)H,
      .minDepth = 0.0f, .maxDepth = 1.0f,
   };
   fw->vk.vkCmdSetViewport(cb, 0, 1, &viewport);
}

/* Begins a render pass over the whole target. `clear` says whether the
 * attachments are cleared or loaded, which is what lets section B chain
 * PASSES of them into one image. */
static void begin_pass(vkfw *fw, VkCommandBuffer cb,
                       const struct fixture *fx, bool clear)
{
   VkClearValue colour_clear;
   colour_clear.color = unorm_colour(CLEAR_RGBA);
   VkClearValue depth_clear;
   depth_clear.depthStencil.depth = Z_CLEAR;
   depth_clear.depthStencil.stencil = 0;

   const VkRenderingAttachmentInfo colour_att = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = fx->colour_view,
      .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR
                      : VK_ATTACHMENT_LOAD_OP_LOAD,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .clearValue = colour_clear,
   };
   const VkRenderingAttachmentInfo depth_att = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = fx->depth_view,
      .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
      .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR
                      : VK_ATTACHMENT_LOAD_OP_LOAD,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .clearValue = depth_clear,
   };
   const VkRenderingInfo ri = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea = { .offset = { 0, 0 }, .extent = { W, H } },
      .layerCount = 1,
      .colorAttachmentCount = 1,
      .pColorAttachments = &colour_att,
      .pDepthAttachment = &depth_att,
   };
   fw->vk.vkCmdBeginRendering(cb, &ri);
   set_viewport(fw, cb);
}

/* The depth of draw `d` in sections A and B: behind its predecessor,
 * except the last, which is in front of everything. */
static float ab_depth(uint32_t d)
{
   if (d == DRAWS_PER_BAND - 1u)
      return Z_WINNER;
   return Z_FIRST + (float)d * Z_STEP;
}

/* The order sections A and B issue (band, draw) pairs in. Section A
 * walks draws outermost so consecutive draws are in different bands,
 * which is what makes the scissor and the push constant change on every
 * single draw rather than every DRAWS_PER_BAND of them. */
static void ab_pair(uint32_t n, uint32_t *band, uint32_t *d)
{
   *d = n / BANDS;
   *band = n % BANDS;
}

/* Every image copy this test makes: colour and depth, in one command
 * buffer, from the attachment layouts back to them. */
static bool copy_out(vkfw *fw, struct fixture *fx, const char *what)
{
   VkCommandBuffer cb;
   if (!vkfw_cmd_begin(fw, &cb))
      return false;

   barrier(fw, cb, fx->colour.img, VK_IMAGE_ASPECT_COLOR_BIT,
           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
           VK_PIPELINE_STAGE_TRANSFER_BIT);
   /* Both fragment-test stages: nothing here writes gl_FragDepth or
    * discards, so the depth write may happen in the early one. */
   barrier(fw, cb, fx->depth.img, VK_IMAGE_ASPECT_DEPTH_BIT,
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
   fw->vk.vkCmdCopyImageToBuffer(cb, fx->colour.img,
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 fx->colour_dst.buf, 1, &region);
   region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
   fw->vk.vkCmdCopyImageToBuffer(cb, fx->depth.img,
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 fx->depth_dst.buf, 1, &region);

   /* Back to the attachment layouts, so the next section starts where
    * the last one did rather than from UNDEFINED, which would discard
    * the contents this test is about to overwrite anyway but would also
    * hide a layout mistake. */
   barrier(fw, cb, fx->colour.img, VK_IMAGE_ASPECT_COLOR_BIT,
           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
           VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
           VK_PIPELINE_STAGE_TRANSFER_BIT,
           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
   barrier(fw, cb, fx->depth.img, VK_IMAGE_ASPECT_DEPTH_BIT,
           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
           VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
           VK_ACCESS_TRANSFER_READ_BIT,
           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
           VK_PIPELINE_STAGE_TRANSFER_BIT,
           VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);

   if (!vkfw_submit_and_wait(fw, cb, what))
      return false;
   return vkfw_buffer_invalidate(fw, &fx->colour_dst) &&
          vkfw_buffer_invalidate(fw, &fx->depth_dst);
}

/* Sections A and B expect the same thing: each band holds the colour of
 * its last draw, and its depth holds that draw's z. */
static void check_ab(test_ctx *t, const struct fixture *fx,
                     const char *label)
{
   const uint32_t *cols = (const uint32_t *)fx->colour_dst.map;
   const uint32_t *deps = (const uint32_t *)fx->depth_dst.map;
   const uint32_t d_want = float_bits(Z_WINNER);

   uint32_t c_wrong = 0, c_i = 0, c_got = 0, c_want = 0;
   uint32_t d_wrong = 0, d_i = 0, d_got = 0;
   for (uint32_t i = 0; i < TEXELS; i++) {
      const uint32_t band = (i % W) / BAND_W;
      const uint32_t want = ab_texel(band, DRAWS_PER_BAND - 1u);
      if (cols[i] != want) {
         if (c_wrong == 0) { c_i = i; c_got = cols[i]; c_want = want; }
         c_wrong++;
      }
      if (deps[i] != d_want) {
         if (d_wrong == 0) { d_i = i; d_got = deps[i]; }
         d_wrong++;
      }
   }

   t_check(t, c_wrong == 0,
           "%s: %u draws, every band holds its last draw's colour "
           "(%u of %u wrong; first at (%u,%u) got 0x%08x want 0x%08x)",
           label, TOTAL_DRAWS, c_wrong, TEXELS, c_i % W, c_i / W, c_got,
           c_want);
   t_check(t, d_wrong == 0,
           "%s: every depth texel is the winning draw's z "
           "(%u of %u wrong; first at (%u,%u) holds 0x%08x)",
           label, d_wrong, TEXELS, d_i % W, d_i / W, d_got);
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

   struct fixture fx;
   memset(&fx, 0, sizeof(fx));
   VkResult r;

   const VkImageUsageFlags colour_usage =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
   const VkImageUsageFlags depth_usage =
      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

   if (!vkfw_image_supported(&fw, DEPTH_FORMAT, VK_IMAGE_TYPE_2D,
                             VK_IMAGE_TILING_OPTIMAL, depth_usage,
                             "D32_SFLOAT depth attachment")) {
      t_note(t, "this device cannot use D32_SFLOAT as a depth attachment "
                "that can also be copied out; skipped, not failed");
      goto out;
   }

   if (!vkfw_image_create(&fw, COLOUR_FORMAT, (VkExtent3D){ W, H, 1 }, 1, 1,
                          colour_usage, VK_IMAGE_TILING_OPTIMAL, &fx.colour))
      goto out;
   if (!vkfw_image_create(&fw, DEPTH_FORMAT, (VkExtent3D){ W, H, 1 }, 1, 1,
                          depth_usage, VK_IMAGE_TILING_OPTIMAL, &fx.depth))
      goto out;

   const VkImageViewCreateInfo colour_ivci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = fx.colour.img,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = COLOUR_FORMAT,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .levelCount = 1, .layerCount = 1,
      },
   };
   r = fw.vk.vkCreateImageView(fw.dev, &colour_ivci, NULL, &fx.colour_view);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateImageView(colour) -> %s",
                vkfw_result_str(r)))
      goto out;

   const VkImageViewCreateInfo depth_ivci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = fx.depth.img,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = DEPTH_FORMAT,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
         .levelCount = 1, .layerCount = 1,
      },
   };
   r = fw.vk.vkCreateImageView(fw.dev, &depth_ivci, NULL, &fx.depth_view);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateImageView(depth) -> %s",
                vkfw_result_str(r)))
      goto out;

   if (!vkfw_buffer_create(&fw, READBACK_B, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                           &fx.colour_dst))
      goto out;
   if (!vkfw_buffer_create(&fw, READBACK_B, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                           &fx.depth_dst))
      goto out;

   const vkfw_gfx_desc base = {
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

   /* Four pipelines whose differences do not change this geometry's
    * answer, so cycling through them is a test of the bind and not a
    * second variable. Two reasons, and both are properties of the
    * depths chosen above rather than of luck:
    *
    *   LESS vs LESS_OR_EQUAL   they differ only on equality, and no
    *       draw's z is ever exactly the depth already stored. The
    *       stored depth is either the clear (1.0, and every z here is
    *       below it) or 0.5, written by a band's first draw, and every
    *       later z is either strictly greater (0.515625 upwards) or
    *       Z_WINNER.
    *   depth write on vs off   a band's pipeline is (band % 4), because
    *       ab_pair() steps n by BANDS and BANDS is a multiple of
    *       PIPELINE_COUNT, so a band uses one pipeline throughout. With
    *       writes on, the first draw stores 0.5 and every later draw
    *       but the winner is rejected; with writes off, the depth stays
    *       at the clear and every draw passes. Either way the last draw
    *       is the nearest, passes, and goes through pipeline 0 — which
    *       writes — so the colour and the depth both end up the same.
    */
   {
      static const char *const names[PIPELINE_COUNT] = {
         "LESS, write on", "LESS_OR_EQUAL, write on",
         "LESS, write off", "LESS_OR_EQUAL, write off",
      };
      for (uint32_t i = 0; i < PIPELINE_COUNT; i++) {
         vkfw_gfx_desc d = base;
         d.depth_compare = (i & 1u) ? VK_COMPARE_OP_LESS_OR_EQUAL
                                    : VK_COMPARE_OP_LESS;
         /* The last draw of a band must write depth, and it is the one
          * issued through pipeline (DRAWS_PER_BAND - 1) % PIPELINE_COUNT
          * — see the loop below, which forces that pipeline. So a
          * write-off pipeline here can never be the deciding one. */
         d.depth_write = (i < 2u);
         if (!vkfw_gfx_create(&fw, names[i], &d, &fx.gfx[i]))
            goto out;
      }
   }

   /* Section C's pipeline: the blend the header describes, and the depth
    * test off so the chain is decided by the blend alone. */
   {
      const VkPipelineColorBlendAttachmentState blend = {
         .blendEnable = VK_TRUE,
         .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
         .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
         .colorBlendOp = VK_BLEND_OP_ADD,
         .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
         .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
         .alphaBlendOp = VK_BLEND_OP_ADD,
         .colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                           VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT |
                           VK_COLOR_COMPONENT_A_BIT,
      };
      vkfw_gfx_desc d = base;
      d.depth_test = false;
      d.depth_write = false;
      d.blend = &blend;
      if (!vkfw_gfx_create(&fw, "blend, depth test off", &d, &fx.blend_gfx))
         goto out;
   }

   /* --- A: every draw in one render pass ---------------------------- */
   {
      if (!vkfw_buffer_poison(&fw, &fx.colour_dst, POISON) ||
          !vkfw_buffer_poison(&fw, &fx.depth_dst, POISON))
         goto out;

      VkCommandBuffer cb;
      if (!vkfw_cmd_begin(&fw, &cb))
         goto out;
      barrier(&fw, cb, fx.colour.img, VK_IMAGE_ASPECT_COLOR_BIT,
              VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
      barrier(&fw, cb, fx.depth.img, VK_IMAGE_ASPECT_DEPTH_BIT,
              VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
              0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);

      const u64 t0 = armGetSystemTick();
      begin_pass(&fw, cb, &fx, true);
      uint32_t binds = 0;
      for (uint32_t n = 0; n < TOTAL_DRAWS; n++) {
         uint32_t band, d;
         ab_pair(n, &band, &d);
         /* The deciding draw always goes through a write-enabled
          * pipeline, so the final depth is its z whatever the rest
          * did; every other draw takes its band's pipeline. */
         const uint32_t p = (d == DRAWS_PER_BAND - 1u)
                               ? 0u : (n % PIPELINE_COUNT);
         fw.vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 fx.gfx[p].pipeline);
         binds++;
         const struct push_data pc =
            push_texel(ab_texel(band, d), ab_depth(d));
         draw_band(&fw, cb, &fx.gfx[p], band, &pc);
      }
      fw.vk.vkCmdEndRendering(cb);
      const uint64_t record_ns = armTicksToNs(armGetSystemTick() - t0);

      const u64 t1 = armGetSystemTick();
      if (!vkfw_submit_and_wait(&fw, cb, "A: one pass, every draw"))
         goto out;
      const uint64_t gpu_ns = armTicksToNs(armGetSystemTick() - t1);

      if (!copy_out(&fw, &fx, "A: copying both images out"))
         goto out;
      check_ab(t, &fx, "A");
      t_note(t, "A: %u draws and %u pipeline binds in one render pass — "
                "%" PRIu64 " us to record, %" PRIu64 " us submitted to "
                "retired", TOTAL_DRAWS, binds, record_ns / 1000u,
             gpu_ns / 1000u);

      vkfw_expect_words(&fw, (const uint32_t *)fx.colour_dst.map + TEXELS,
                        POISON, TAIL_WORDS,
                        "A: nothing was written past the colour image");
   }

   /* --- B: the same draws, PASSES render passes --------------------- */
   {
      if (!vkfw_buffer_poison(&fw, &fx.colour_dst, POISON) ||
          !vkfw_buffer_poison(&fw, &fx.depth_dst, POISON))
         goto out;

      VkCommandBuffer cb;
      if (!vkfw_cmd_begin(&fw, &cb))
         goto out;

      const u64 t0 = armGetSystemTick();
      uint32_t n = 0;
      for (uint32_t pass = 0; pass < PASSES; pass++) {
         if (pass != 0) {
            const VkMemoryBarrier mb = {
               .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
               .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
               .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            };
            fw.vk.vkCmdPipelineBarrier(
               cb,
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
               0, 1, &mb, 0, NULL, 0, NULL);
         }
         begin_pass(&fw, cb, &fx, pass == 0);
         for (uint32_t k = 0; k < DRAWS_PER_PASS; k++, n++) {
            uint32_t band, d;
            ab_pair(n, &band, &d);
            const uint32_t p = (d == DRAWS_PER_BAND - 1u)
                                  ? 0u : (n % PIPELINE_COUNT);
            fw.vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    fx.gfx[p].pipeline);
            const struct push_data pc =
               push_texel(ab_texel(band, d), ab_depth(d));
            draw_band(&fw, cb, &fx.gfx[p], band, &pc);
         }
         fw.vk.vkCmdEndRendering(cb);
      }
      const uint64_t record_ns = armTicksToNs(armGetSystemTick() - t0);

      const u64 t1 = armGetSystemTick();
      if (!vkfw_submit_and_wait(&fw, cb, "B: the same draws, many passes"))
         goto out;
      const uint64_t gpu_ns = armTicksToNs(armGetSystemTick() - t1);

      if (!copy_out(&fw, &fx, "B: copying both images out"))
         goto out;
      check_ab(t, &fx, "B");
      t_note(t, "B: the same %u draws over %u render passes — %" PRIu64
                " us to record, %" PRIu64 " us submitted to retired",
             TOTAL_DRAWS, PASSES, record_ns / 1000u, gpu_ns / 1000u);
   }

   /* --- C: a blend chain, checked against the same chain in C ------- */
   {
      if (!vkfw_buffer_poison(&fw, &fx.colour_dst, POISON) ||
          !vkfw_buffer_poison(&fw, &fx.depth_dst, POISON))
         goto out;

      VkCommandBuffer cb;
      if (!vkfw_cmd_begin(&fw, &cb))
         goto out;
      begin_pass(&fw, cb, &fx, true);
      fw.vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              fx.blend_gfx.pipeline);
      for (uint32_t band = 0; band < BANDS; band++) {
         for (uint32_t k = 0; k < BLEND_STEPS; k++) {
            uint32_t rgba[4];
            c_source(band, k, rgba);
            const struct push_data pc = push_rgba(rgba, Z_FLAT);
            draw_band(&fw, cb, &fx.blend_gfx, band, &pc);
         }
      }
      fw.vk.vkCmdEndRendering(cb);
      if (!vkfw_submit_and_wait(&fw, cb, "C: the blend chain"))
         goto out;
      if (!copy_out(&fw, &fx, "C: copying both images out"))
         goto out;

      /* The same chain in float. The clear is the first destination;
       * alpha uses ONE/ZERO, so the destination alpha is simply the
       * last source's. */
      const uint32_t clear[4] = { CLEAR_RGBA };
      uint32_t want[BANDS];
      for (uint32_t band = 0; band < BANDS; band++) {
         float dst[4];
         for (uint32_t c = 0; c < 4; c++)
            dst[c] = (float)clear[c] / 255.0f;
         for (uint32_t k = 0; k < BLEND_STEPS; k++) {
            uint32_t rgba[4];
            c_source(band, k, rgba);
            const float sa = (float)rgba[3] / 255.0f;
            for (uint32_t c = 0; c < 3; c++) {
               const float src = (float)rgba[c] / 255.0f;
               dst[c] = src * sa + dst[c] * (1.0f - sa);
            }
            dst[3] = sa;
         }
         uint32_t b8[4];
         for (uint32_t c = 0; c < 4; c++) {
            float v = dst[c] * 255.0f + 0.5f;
            if (v < 0.0f)
               v = 0.0f;
            if (v > 255.0f)
               v = 255.0f;
            b8[c] = (uint32_t)v;
         }
         want[band] = texel(b8[0], b8[1], b8[2], b8[3]);
      }

      const uint32_t *cols = (const uint32_t *)fx.colour_dst.map;
      uint32_t wrong = 0, first_i = 0, first_got = 0, first_want = 0;
      uint32_t worst = 0;
      for (uint32_t i = 0; i < TEXELS; i++) {
         const uint32_t band = (i % W) / BAND_W;
         const uint32_t got = cols[i], w = want[band];
         uint32_t d_max = 0;
         for (uint32_t c = 0; c < 4; c++) {
            const int32_t a = (int32_t)((got >> (c * 8)) & 0xffu);
            const int32_t b = (int32_t)((w >> (c * 8)) & 0xffu);
            const uint32_t d = (uint32_t)(a > b ? a - b : b - a);
            if (d > d_max)
               d_max = d;
         }
         if (d_max > worst)
            worst = d_max;
         if (d_max > BLEND_TOLERANCE) {
            if (wrong == 0) { first_i = i; first_got = got; first_want = w; }
            wrong++;
         }
      }
      t_check(t, wrong == 0,
              "C: %u blend steps per band land within %u/255 per channel "
              "(%u of %u pixels outside it, worst channel error %u; first "
              "at (%u,%u) got 0x%08x want 0x%08x)",
              BLEND_STEPS, BLEND_TOLERANCE, wrong, TEXELS, worst,
              first_i % W, first_i / W, first_got, first_want);
   }

out:
   vkfw_gfx_destroy(&fw, &fx.blend_gfx);
   for (uint32_t i = 0; i < PIPELINE_COUNT; i++)
      vkfw_gfx_destroy(&fw, &fx.gfx[i]);
   if (fx.depth_view != VK_NULL_HANDLE)
      fw.vk.vkDestroyImageView(fw.dev, fx.depth_view, NULL);
   if (fx.colour_view != VK_NULL_HANDLE)
      fw.vk.vkDestroyImageView(fw.dev, fx.colour_view, NULL);
   vkfw_buffer_destroy(&fw, &fx.depth_dst);
   vkfw_buffer_destroy(&fw, &fx.colour_dst);
   vkfw_image_destroy(&fw, &fx.depth);
   vkfw_image_destroy(&fw, &fx.colour);
   vkfw_finish(&fw);
   return 0;
}
