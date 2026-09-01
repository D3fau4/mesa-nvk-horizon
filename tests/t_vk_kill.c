/*
 * A loop in a fragment shader, with and without a kill in front of it.
 *
 * WHY THIS EXISTS. Godot 4.1's Forward+ scene shader hangs the channel on
 * GM20B -- "fault notification 8 (fifo idle timeout)" -- the moment one of
 * its cluster loops takes its back edge, and costs 2.77 ms a frame when
 * the same loop is compiled in but never entered. Measured 2026-08-29 by
 * taking the loop apart one piece at a time: with the bound forced to a
 * compile-time four, the body replaced by an increment, the inner loop
 * capped, the range read and the subgroup reduce deleted, and finally the
 * whole induction replaced by a plain counted loop, it still hung. Nothing
 * the loop reads or computes is involved. A compute shader with twelve
 * levels of nesting around a loop passes (t_vk_crs), so the question is
 * about the fragment stage, and the one thing the scene shader has that a
 * compute test cannot is a kill: it reports uses_kill.
 *
 * THREE CASES, AND THE FIRST TWO ARE THE CONTROL.
 *
 *   n = 0, kill_mode = 0   the loop is present and never entered
 *   n = 4, kill_mode = 0   the loop runs, nothing is killed
 *   n = 4, kill_mode = 1   the loop runs after half the lanes are gone
 *
 * Case 1 is the shape that stays fast in Godot, case 2 asks whether a
 * fragment loop works at all, and case 3 asks whether one works after a
 * kill. Whichever of the three stops being answerable is the one to fix.
 *
 * LOAD_OP_CLEAR, not DONT_CARE as in t_vk_format: case 3 kills half the
 * texels on purpose, and a killed texel has to be checked for holding the
 * clear rather than left undefined.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <string.h>

#include "common/vkfw.h"

#include "fmt_vert_fullscreen.spv.h"
#include "kill_frag_loop.spv.h"

const char *const test_name = "t_vk_kill";
const bool test_uses_display = false;

#define W            16u
#define H            16u
#define TEXELS       (W * H)
#define TEXEL_B      16u                    /* R32G32B32A32_UINT */
#define IMAGE_B      (TEXELS * TEXEL_B)
#define TAIL_B       1024u
#define READBACK_B   (IMAGE_B + TAIL_B)
#define POISON       0xdeadbeefu
#define CLEAR_U      0xc1ea5eedu

struct kill_case {
   const char *name;
   uint32_t n;           /* loop bound */
   uint32_t kill_mode;   /* 0 = nothing killed, 1 = half the lanes killed */
};

static const struct kill_case CASES[] = {
   { "bound 0, no kill (the loop is never entered)", 0, 0 },
   { "bound 4, no kill", 4, 0 },
   { "bound 4, half the lanes killed first", 4, 1 },
};
#define NUM_CASES (sizeof(CASES) / sizeof(CASES[0]))

/* uvec4(n, kill_mode, 0, 0), matching kill_frag_loop.spvasm. */
struct push_data {
   uint32_t n, kill_mode, pad0, pad1;
};

static void barrier(vkfw *fw, VkCommandBuffer cb, VkImage img,
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

/* The arithmetic the shader does: acc = sum(1..n), i ends at n. */
static void expect_texel(const struct kill_case *c, uint32_t x, uint32_t y,
                         uint32_t out[4], bool *killed)
{
   *killed = c->kill_mode != 0 && (((x ^ y) & 1u) != 0u);
   if (*killed) {
      for (uint32_t k = 0; k < 4; k++)
         out[k] = CLEAR_U;
      return;
   }
   uint32_t acc = 0;
   for (uint32_t i = 0; i < c->n; i++)
      acc += i + 1u;
   out[0] = acc;
   out[1] = c->n;
   out[2] = c->kill_mode;
   out[3] = c->n;
}

static bool run_case(vkfw *fw, const struct kill_case *c, vkfw_buffer *dst)
{
   test_ctx *t = fw->t;
   const VkFormat format = VK_FORMAT_R32G32B32A32_UINT;

   const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
   vkfw_image img = { 0 };
   VkImageView view = VK_NULL_HANDLE;
   vkfw_gfx gfx = { 0 };
   VkCommandBuffer cb;

   if (!vkfw_image_create(fw, format, (VkExtent3D){ W, H, 1 }, 1, 1,
                          usage, VK_IMAGE_TILING_OPTIMAL, &img))
      return false;

   const VkImageViewCreateInfo ivci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = img.img,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = format,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .levelCount = 1, .layerCount = 1,
      },
   };
   VkResult r = fw->vk.vkCreateImageView(fw->dev, &ivci, NULL, &view);
   if (!t_check(t, r == VK_SUCCESS, "%s: vkCreateImageView -> %s",
                c->name, vkfw_result_str(r)))
      goto out;

   const vkfw_gfx_desc desc = {
      .vs_spv = fmt_vert_fullscreen_spv,
      .vs_B = sizeof(fmt_vert_fullscreen_spv),
      .fs_spv = kill_frag_loop_spv,
      .fs_B = sizeof(kill_frag_loop_spv),
      .colour_format = format,
      .depth_format = VK_FORMAT_UNDEFINED,
      .push_constant_B = (uint32_t)sizeof(struct push_data),
      .push_constant_stages = VK_SHADER_STAGE_FRAGMENT_BIT,
      .width = W, .height = H,
   };
   if (!vkfw_gfx_create(fw, c->name, &desc, &gfx))
      goto out;

   if (!vkfw_buffer_poison(fw, dst, POISON))
      goto out;
   if (!vkfw_cmd_begin(fw, &cb))
      goto out;

   barrier(fw, cb, img.img,
           VK_IMAGE_LAYOUT_UNDEFINED,
           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
           0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
           VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

   const VkRenderingAttachmentInfo att = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = view,
      .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .clearValue = { .color = { .uint32 = { CLEAR_U, CLEAR_U,
                                             CLEAR_U, CLEAR_U } } },
   };
   const VkRenderingInfo ri = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea = { .offset = { 0, 0 }, .extent = { W, H } },
      .layerCount = 1,
      .colorAttachmentCount = 1,
      .pColorAttachments = &att,
   };

   const struct push_data push = { c->n, c->kill_mode, 0, 0 };

   fw->vk.vkCmdBeginRendering(cb, &ri);
   fw->vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            gfx.pipeline);
   fw->vk.vkCmdPushConstants(cb, gfx.layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                             0, (uint32_t)sizeof(push), &push);
   fw->vk.vkCmdDraw(cb, 3, 1, 0, 0);
   fw->vk.vkCmdEndRendering(cb);

   barrier(fw, cb, img.img,
           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
           VK_PIPELINE_STAGE_TRANSFER_BIT);

   const VkBufferImageCopy region = {
      .imageSubresource = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1,
      },
      .imageExtent = { W, H, 1 },
   };
   fw->vk.vkCmdCopyImageToBuffer(cb, img.img,
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 dst->buf, 1, &region);

   if (!vkfw_submit_and_wait(fw, cb, c->name))
      goto out;
   if (!vkfw_buffer_invalidate(fw, dst))
      goto out;

   {
      const uint32_t *base = (const uint32_t *)dst->map;
      uint32_t wrong = 0, first = TEXELS, killed_n = 0;
      for (uint32_t i = 0; i < TEXELS; i++) {
         const uint32_t x = i % W, y = i / W;
         uint32_t want[4];
         bool killed;
         expect_texel(c, x, y, want, &killed);
         killed_n += killed;
         const uint32_t *got = base + (size_t)i * 4u;
         bool ok = true;
         for (uint32_t k = 0; k < 4; k++)
            ok = ok && got[k] == want[k];
         if (!ok) {
            if (first == TEXELS)
               first = i;
            wrong++;
         }
      }
      t_check(t, wrong == 0, "%s: %u/%u texels right (%u of them killed)",
              c->name, TEXELS - wrong, TEXELS, killed_n);
      if (wrong != 0) {
         uint32_t want[4];
         bool killed;
         expect_texel(c, first % W, first / W, want, &killed);
         const uint32_t *got = base + (size_t)first * 4u;
         t_note(t, "%s: first wrong texel %u (%u,%u)%s: got acc=%u i=%u "
                   "kill=%u n=%u, want acc=%u i=%u kill=%u n=%u",
                c->name, first, first % W, first / W,
                killed ? ", which should have been killed" : "",
                got[0], got[1], got[2], got[3],
                want[0], want[1], want[2], want[3]);
      }

      vkfw_expect_words(fw, (const uint8_t *)dst->map + IMAGE_B, POISON,
                        (READBACK_B - IMAGE_B) / 4u,
                        "nothing was written past the image");
   }

out:
   vkfw_gfx_destroy(fw, &gfx);
   if (view != VK_NULL_HANDLE)
      fw->vk.vkDestroyImageView(fw->dev, view, NULL);
   vkfw_image_destroy(fw, &img);
   return true;
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

   vkfw_buffer dst = { 0 };

   if (!vkfw_buffer_create(&fw, READBACK_B, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &dst))
      goto out;

   for (uint32_t i = 0; i < NUM_CASES; i++) {
      /* Once the channel is gone every later case fails the same way,
       * and the first one is the answer. */
      if (vkfw_device_lost(&fw)) {
         t_note(t, "device lost; %u case(s) from \"%s\" on not attempted",
                (unsigned)(NUM_CASES - i), CASES[i].name);
         break;
      }
      run_case(&fw, &CASES[i], &dst);
   }

out:
   vkfw_buffer_destroy(&fw, &dst);
   vkfw_finish(&fw);
   return 0;
}
