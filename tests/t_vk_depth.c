/*
 * Phase 5 item 7 — depth.
 *
 * Four draws into one render pass, all of them the same full-target
 * triangle with the same shaders, differing only in the twenty bytes
 * pushed before each and in the scissor rectangle that carves the
 * region it may touch. Both the colour attachment and the depth
 * attachment are read back and checked, pixel by pixel.
 *
 * WHY SCISSOR AND NOT GEOMETRY. Item 5 measured rasterisation. Here the
 * question is only which fragments survive the depth test, so the
 * regions are cut with scissor rectangles: exact in pixel coordinates,
 * with no fill rule between the reader and the answer. If a region
 * boundary in this test moved, the fault would be in the scissor and
 * nowhere near depth.
 *
 * THE FOUR DRAWS, IN ORDER:
 *
 *   1  z 0.75  red     x in [0,48)   test LESS, write on
 *   2  z 0.25  green   x in [16,64)  test LESS, write on
 *   3  z 0.875 blue    x in [0,64)   test LESS, write on
 *   4  z 0.5   olive   x in [0,16)   test LESS, write OFF
 *
 * and what each one is for:
 *
 *   1 and 2 overlap in x [16,48). The nearer one is drawn second, so
 *     "green wins there" is the depth test accepting; the depth buffer
 *     must hold 0.25 across the whole of x >= 16 and 0.75 below it.
 *   3 is behind everything and covers everything. Every one of its
 *     fragments must be rejected. If the depth test were degenerate —
 *     always passing, or not enabled at all — draw 3 would paint the
 *     entire target and the failure would be total rather than subtle.
 *     Blue appearing at even one pixel is a failure, and it is counted
 *     separately so the log says so directly.
 *   4 is in front of the 0.75 region and passes there, but with
 *     depthWriteEnable off. Its colour must appear and the depth must
 *     NOT move to 0.5. That separates the two halves of the depth
 *     state: a driver that ignored depthWriteEnable passes every colour
 *     check in this test and fails on one depth value.
 *
 * So the answer is:
 *
 *   colour   x in [0,16): olive      x in [16,64): green
 *   depth    x in [0,16): 0.75       x in [16,64): 0.25
 *
 * The clear colour and the blue must appear at exactly zero pixels, and
 * red must too — draw 1's region is entirely overwritten by draws 2 and
 * 4. Three colours that must be absent is a stronger statement than one
 * colour that must be present.
 *
 * WHY D32_SFLOAT. Depth is compared bit for bit, so the format has to
 * represent 0.75 and 0.25 exactly and hand them back unchanged.
 * D32_SFLOAT does; D16_UNORM would put a rounding rule in the middle of
 * the one value being measured. If the device does not support it as a
 * depth attachment the test says so and skips rather than reporting a
 * pass it did not earn.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <string.h>

#include "common/vkfw.h"

#include "depth_vert_z.spv.h"
#include "depth_frag_pc.spv.h"

const char *const test_name = "t_vk_depth";

#define W            64u
#define H            64u
#define TEXELS       (W * H)
#define IMAGE_B      (TEXELS * 4u)
#define TAIL_WORDS   256u
#define READBACK_B   (IMAGE_B + TAIL_WORDS * 4u)
#define POISON       0xdeadbeefu

/* The boundary between the two answers, and the two scissor edges that
 * produce it. */
#define SPLIT_X      16u
#define FAR_RIGHT    48u

#define DEPTH_FORMAT VK_FORMAT_D32_SFLOAT

/* Colours as bytes; k/255.0f round-trips to exactly k. */
#define CLEAR_RGBA   0x05u, 0x06u, 0x07u, 0x08u
#define RED_RGBA     0xccu, 0x11u, 0x22u, 0xffu
#define GREEN_RGBA   0x22u, 0xccu, 0x33u, 0xffu
#define BLUE_RGBA    0x44u, 0x55u, 0xeeu, 0xffu
#define OLIVE_RGBA   0x88u, 0x99u, 0x11u, 0xffu

/* Depths. All three are exact in binary, so the readback comparison is
 * an equality and not a tolerance. */
#define Z_FAR        0.75f
#define Z_NEAR       0.25f
#define Z_BEHIND     0.875f
#define Z_NOWRITE    0.5f
#define Z_CLEAR      1.0f

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

/* The bit pattern a D32_SFLOAT texel holds for `f`, without punning
 * through a pointer of the wrong type. */
static uint32_t float_bits(float f)
{
   uint32_t bits;
   memcpy(&bits, &f, sizeof(bits));
   return bits;
}

/* The push constant block both shaders declare: colour at offset 0,
 * depth at offset 16. */
struct push_data {
   float colour[4];
   float z;
};

static struct push_data push_for(uint32_t r, uint32_t g, uint32_t b,
                                 uint32_t a, float z)
{
   struct push_data p;
   p.colour[0] = (float)r / 255.0f;
   p.colour[1] = (float)g / 255.0f;
   p.colour[2] = (float)b / 255.0f;
   p.colour[3] = (float)a / 255.0f;
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

/* One of the four draws: push its twenty bytes, set its scissor, draw
 * the triangle. The viewport is the whole target every time; only the
 * scissor moves. */
struct draw {
   const char *name;
   struct push_data pc;
   uint32_t x0, x1;      /* scissor, [x0, x1) */
   bool depth_write;
};

static void record_draw(vkfw *fw, VkCommandBuffer cb, const vkfw_gfx *write,
                        const vkfw_gfx *nowrite, const struct draw *d)
{
   const vkfw_gfx *gfx = d->depth_write ? write : nowrite;

   const VkViewport viewport = {
      .x = 0.0f, .y = 0.0f,
      .width = (float)W, .height = (float)H,
      .minDepth = 0.0f, .maxDepth = 1.0f,
   };
   const VkRect2D scissor = {
      .offset = { (int32_t)d->x0, 0 },
      .extent = { d->x1 - d->x0, H },
   };

   fw->vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            gfx->pipeline);
   fw->vk.vkCmdSetViewport(cb, 0, 1, &viewport);
   fw->vk.vkCmdSetScissor(cb, 0, 1, &scissor);
   fw->vk.vkCmdPushConstants(cb, gfx->layout,
                             VK_SHADER_STAGE_VERTEX_BIT |
                                VK_SHADER_STAGE_FRAGMENT_BIT,
                             0, (uint32_t)sizeof(d->pc), &d->pc);
   fw->vk.vkCmdDraw(cb, 3, 1, 0, 0);
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

   vkfw_buffer colour_dst = { 0 }, depth_dst = { 0 };
   vkfw_image colour = { 0 }, depth = { 0 };
   VkImageView colour_view = VK_NULL_HANDLE, depth_view = VK_NULL_HANDLE;
   vkfw_gfx gfx_write = { 0 }, gfx_nowrite = { 0 };
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

   if (!vkfw_image_create(&fw, VK_FORMAT_R8G8B8A8_UNORM,
                          (VkExtent3D){ W, H, 1 }, 1, 1, colour_usage,
                          VK_IMAGE_TILING_OPTIMAL, &colour))
      goto out;
   if (!vkfw_image_create(&fw, DEPTH_FORMAT, (VkExtent3D){ W, H, 1 }, 1, 1,
                          depth_usage, VK_IMAGE_TILING_OPTIMAL, &depth))
      goto out;

   const VkImageViewCreateInfo colour_ivci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = colour.img,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .levelCount = 1, .layerCount = 1,
      },
   };
   r = fw.vk.vkCreateImageView(fw.dev, &colour_ivci, NULL, &colour_view);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateImageView(colour) -> %s",
                vkfw_result_str(r)))
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
   r = fw.vk.vkCreateImageView(fw.dev, &depth_ivci, NULL, &depth_view);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateImageView(depth) -> %s",
                vkfw_result_str(r)))
      goto out;

   if (!vkfw_buffer_create(&fw, READBACK_B, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &colour_dst))
      goto out;
   if (!vkfw_buffer_create(&fw, READBACK_B, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &depth_dst))
      goto out;

   /* Two pipelines that differ in one field, so that "depth write off"
    * is a pipeline and not a state the test could forget to set. */
   const vkfw_gfx_desc base = {
      .vs_spv = depth_vert_z_spv, .vs_B = sizeof(depth_vert_z_spv),
      .fs_spv = depth_frag_pc_spv, .fs_B = sizeof(depth_frag_pc_spv),
      .colour_format = VK_FORMAT_R8G8B8A8_UNORM,
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
   if (!vkfw_gfx_create(&fw, "depth write on", &base, &gfx_write))
      goto out;

   vkfw_gfx_desc no_write = base;
   no_write.depth_write = false;
   if (!vkfw_gfx_create(&fw, "depth write off", &no_write, &gfx_nowrite))
      goto out;

   const struct draw draws[4] = {
      { "1: far, red",       push_for(RED_RGBA, Z_FAR),
        0, FAR_RIGHT, true },
      { "2: near, green",    push_for(GREEN_RGBA, Z_NEAR),
        SPLIT_X, W, true },
      { "3: behind, blue",   push_for(BLUE_RGBA, Z_BEHIND),
        0, W, true },
      { "4: nearer, olive, no depth write",
        push_for(OLIVE_RGBA, Z_NOWRITE), 0, SPLIT_X, false },
   };

   if (!vkfw_buffer_poison(&fw, &colour_dst, POISON))
      goto out;
   if (!vkfw_buffer_poison(&fw, &depth_dst, POISON))
      goto out;

   VkCommandBuffer cb;
   if (!vkfw_cmd_begin(&fw, &cb))
      goto out;

   barrier(&fw, cb, colour.img, VK_IMAGE_ASPECT_COLOR_BIT,
           VK_IMAGE_LAYOUT_UNDEFINED,
           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
           0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
           VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
   barrier(&fw, cb, depth.img, VK_IMAGE_ASPECT_DEPTH_BIT,
           VK_IMAGE_LAYOUT_UNDEFINED,
           VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
           0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
           VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
           VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);

   /* Same four bytes the absence check below looks for, from one
    * macro, so the two cannot drift apart. */
   VkClearValue colour_clear;
   colour_clear.color = unorm_colour(CLEAR_RGBA);

   VkClearValue depth_clear;
   depth_clear.depthStencil.depth = Z_CLEAR;
   depth_clear.depthStencil.stencil = 0;

   const VkRenderingAttachmentInfo colour_att = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = colour_view,
      .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .clearValue = colour_clear,
   };
   const VkRenderingAttachmentInfo depth_att = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = depth_view,
      .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
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

   fw.vk.vkCmdBeginRendering(cb, &ri);
   for (uint32_t i = 0; i < 4; i++)
      record_draw(&fw, cb, &gfx_write, &gfx_nowrite, &draws[i]);
   fw.vk.vkCmdEndRendering(cb);

   barrier(&fw, cb, colour.img, VK_IMAGE_ASPECT_COLOR_BIT,
           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
           VK_PIPELINE_STAGE_TRANSFER_BIT);
   barrier(&fw, cb, depth.img, VK_IMAGE_ASPECT_DEPTH_BIT,
           VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
           VK_ACCESS_TRANSFER_READ_BIT,
           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
           VK_PIPELINE_STAGE_TRANSFER_BIT);

   VkBufferImageCopy region = {
      .imageSubresource = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1,
      },
      .imageExtent = { W, H, 1 },
   };
   fw.vk.vkCmdCopyImageToBuffer(cb, colour.img,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                colour_dst.buf, 1, &region);
   region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
   fw.vk.vkCmdCopyImageToBuffer(cb, depth.img,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                depth_dst.buf, 1, &region);

   if (!vkfw_submit_and_wait(&fw, cb, "four depth-tested draws"))
      goto out;
   if (!vkfw_buffer_invalidate(&fw, &colour_dst))
      goto out;
   if (!vkfw_buffer_invalidate(&fw, &depth_dst))
      goto out;

   /* ---- what must have come back -------------------------------- */
   const uint32_t *cols = (const uint32_t *)colour_dst.map;
   const uint32_t *deps = (const uint32_t *)depth_dst.map;

   const uint32_t c_clear = texel(CLEAR_RGBA);
   const uint32_t c_red = texel(RED_RGBA);
   const uint32_t c_green = texel(GREEN_RGBA);
   const uint32_t c_blue = texel(BLUE_RGBA);
   const uint32_t c_olive = texel(OLIVE_RGBA);

   const uint32_t d_far = float_bits(Z_FAR);
   const uint32_t d_near = float_bits(Z_NEAR);
   const uint32_t d_nowrite = float_bits(Z_NOWRITE);
   const uint32_t d_clear = float_bits(Z_CLEAR);

   uint32_t c_wrong = 0, c_first = 0;
   uint32_t d_wrong = 0, d_first = 0;
   uint32_t n_blue = 0, n_red = 0, n_clear = 0;
   uint32_t n_d_nowrite = 0, n_d_clear = 0;

   for (uint32_t py = 0; py < H; py++) {
      for (uint32_t px = 0; px < W; px++) {
         const uint32_t i = py * W + px;
         const uint32_t want_c = px < SPLIT_X ? c_olive : c_green;
         const uint32_t want_d = px < SPLIT_X ? d_far : d_near;

         if (cols[i] != want_c) {
            if (c_wrong == 0)
               c_first = i;
            c_wrong++;
         }
         if (deps[i] != want_d) {
            if (d_wrong == 0)
               d_first = i;
            d_wrong++;
         }

         if (cols[i] == c_blue)
            n_blue++;
         if (cols[i] == c_red)
            n_red++;
         if (cols[i] == c_clear)
            n_clear++;
         if (deps[i] == d_nowrite)
            n_d_nowrite++;
         if (deps[i] == d_clear)
            n_d_clear++;
      }
   }

   t_check(t, c_wrong == 0,
           "colour: %u/%u pixels hold what the depth test left there",
           TEXELS - c_wrong, TEXELS);
   if (c_wrong != 0) {
      t_note(t, "colour: first wrong at (%u,%u): got 0x%08x, want 0x%08x",
             c_first % W, c_first / W, cols[c_first],
             (c_first % W) < SPLIT_X ? c_olive : c_green);
   }

   t_check(t, d_wrong == 0,
           "depth: %u/%u pixels hold the value the surviving fragment "
           "wrote", TEXELS - d_wrong, TEXELS);
   if (d_wrong != 0) {
      t_note(t, "depth: first wrong at (%u,%u): got 0x%08x, want 0x%08x "
                "(0.75 is 0x%08x, 0.25 is 0x%08x, 1.0 is 0x%08x)",
             d_first % W, d_first / W, deps[d_first],
             (d_first % W) < SPLIT_X ? d_far : d_near,
             d_far, d_near, d_clear);
   }

   /* The three colours that must be absent, each counted on its own so
    * the log names which failure happened rather than only that one
    * did. */
   t_check(t, n_blue == 0,
           "the draw behind everything was rejected at every pixel "
           "(%u blue)", n_blue);
   t_check(t, n_red == 0,
           "nothing is left of the far draw (%u red)", n_red);
   t_check(t, n_clear == 0,
           "every pixel was drawn to (%u still the clear colour)", n_clear);

   /* depthWriteEnable off, stated as its own measurement: draw 4's
    * colour is everywhere it should be (checked above), and its depth
    * is nowhere. */
   t_check(t, n_d_nowrite == 0,
           "the draw with depth writes off left the depth buffer alone "
           "(%u pixels hold 0.5)", n_d_nowrite);
   t_check(t, n_d_clear == 0,
           "every pixel's depth was written by some draw (%u still 1.0)",
           n_d_clear);

   vkfw_expect_words(&fw, cols + TEXELS, POISON, TAIL_WORDS,
                     "nothing was written past the colour image");
   vkfw_expect_words(&fw, deps + TEXELS, POISON, TAIL_WORDS,
                     "nothing was written past the depth image");

out:
   vkfw_gfx_destroy(&fw, &gfx_nowrite);
   vkfw_gfx_destroy(&fw, &gfx_write);
   if (depth_view != VK_NULL_HANDLE)
      fw.vk.vkDestroyImageView(fw.dev, depth_view, NULL);
   if (colour_view != VK_NULL_HANDLE)
      fw.vk.vkDestroyImageView(fw.dev, colour_view, NULL);
   vkfw_image_destroy(&fw, &depth);
   vkfw_image_destroy(&fw, &colour);
   vkfw_buffer_destroy(&fw, &depth_dst);
   vkfw_buffer_destroy(&fw, &colour_dst);
   vkfw_finish(&fw);
   return 0;
}
