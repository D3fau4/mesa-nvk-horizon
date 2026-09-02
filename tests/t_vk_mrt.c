/*
 * A colour pass that declares more render targets than it writes.
 *
 * WHY THIS EXISTS. Dumping the whole pipeline state at the cube's draw in
 * Godot 4.1 under Forward+ and under Mobile (2026-08-23) left five
 * differences out of ~930 fields. Four have since been tested in
 * isolation and none of them is the fault: Zcull (excluded 2026-09-02 with
 * NVK_HORIZON_ZCULL=0, which turns the whole feature off and changes
 * nothing), the depth prepass and its DEPTH_FUNC EQUAL, the register
 * count, and the RGBA16F target. The fifth was never asked on its own:
 *
 *     SET_CT_SELECT.TARGET_COUNT = 3 under Forward+, 1 under Mobile,
 *     with SET_COLOR_TARGET_FORMAT(1) and (2) = DISABLED.
 *
 * That is a subpass declaring three colour attachments and writing one.
 * Mesa's render-pass runtime turns the two it does not write into a NULL
 * imageView and VK_FORMAT_UNDEFINED, and nvk_cmd_draw.c still counts them
 * in TARGET_COUNT. Whether GM20B's ROP is content to be told about two
 * targets that do not exist has never been measured.
 *
 * THREE CASES, and the middle one is the control that separates "three
 * targets" from "three targets, two of them disabled":
 *
 *   1 target                 what every other test in this suite renders
 *   3 targets, all real      three attachments, the shader writes one
 *   3 targets, 2 disabled    Godot's Forward+ shape
 *
 * The fragment shader is kill_frag_loop with its kill turned off, reused
 * on purpose: it writes exactly one colour output and it is already
 * proven right on this chip (t_vk_kill, 83/83), so the only thing that
 * varies across the three cases is the number of colour targets.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <string.h>

#include "common/vkfw.h"

#include "fmt_vert_fullscreen.spv.h"
#include "kill_frag_loop.spv.h"

const char *const test_name = "t_vk_mrt";
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
#define LOOP_N       4u

#define MAX_TARGETS  3u

struct mrt_case {
   const char *name;
   uint32_t targets;    /* colour attachments the pass declares */
   uint32_t real;       /* how many of them have an image view */
};

static const struct mrt_case CASES[] = {
   { "one target", 1, 1 },
   { "three targets, all three real", 3, 3 },
   { "three targets, two disabled (Godot's Forward+ shape)", 3, 1 },
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

static bool run_case(vkfw *fw, const struct mrt_case *c, vkfw_buffer *dst)
{
   test_ctx *t = fw->t;
   const VkFormat format = VK_FORMAT_R32G32B32A32_UINT;

   const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
   vkfw_image img[MAX_TARGETS] = { { 0 } };
   VkImageView view[MAX_TARGETS] = { VK_NULL_HANDLE };
   vkfw_gfx gfx = { 0 };
   VkCommandBuffer cb;
   bool made[MAX_TARGETS] = { false };

   for (uint32_t i = 0; i < c->real; i++) {
      if (!vkfw_image_create(fw, format, (VkExtent3D){ W, H, 1 }, 1, 1,
                             usage, VK_IMAGE_TILING_OPTIMAL, &img[i]))
         goto out;
      made[i] = true;

      const VkImageViewCreateInfo ivci = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = img[i].img,
         .viewType = VK_IMAGE_VIEW_TYPE_2D,
         .format = format,
         .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1, .layerCount = 1,
         },
      };
      VkResult r = fw->vk.vkCreateImageView(fw->dev, &ivci, NULL, &view[i]);
      if (!t_check(t, r == VK_SUCCESS, "%s: vkCreateImageView(%u) -> %s",
                   c->name, i, vkfw_result_str(r)))
         goto out;
   }

   vkfw_gfx_desc desc = {
      .vs_spv = fmt_vert_fullscreen_spv,
      .vs_B = sizeof(fmt_vert_fullscreen_spv),
      .fs_spv = kill_frag_loop_spv,
      .fs_B = sizeof(kill_frag_loop_spv),
      .colour_format = format,
      .colour_extra_count = c->targets - 1u,
      .depth_format = VK_FORMAT_UNDEFINED,
      .push_constant_B = (uint32_t)sizeof(struct push_data),
      .push_constant_stages = VK_SHADER_STAGE_FRAGMENT_BIT,
      .width = W, .height = H,
   };
   /* A target with no image view is declared UNDEFINED in the pipeline
    * too, which is what a subpass that does not write it produces. */
   for (uint32_t i = 1; i < c->targets; i++)
      desc.colour_extra_formats[i - 1] =
         (i < c->real) ? format : VK_FORMAT_UNDEFINED;

   if (!vkfw_gfx_create(fw, c->name, &desc, &gfx))
      goto out;

   if (!vkfw_buffer_poison(fw, dst, POISON))
      goto out;
   if (!vkfw_cmd_begin(fw, &cb))
      goto out;

   for (uint32_t i = 0; i < c->real; i++)
      barrier(fw, cb, img[i].img,
              VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

   VkRenderingAttachmentInfo att[MAX_TARGETS];
   for (uint32_t i = 0; i < c->targets; i++) {
      att[i] = (VkRenderingAttachmentInfo){
         .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
         .imageView = (i < c->real) ? view[i] : VK_NULL_HANDLE,
         .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .clearValue = { .color = { .uint32 = { CLEAR_U, CLEAR_U,
                                                CLEAR_U, CLEAR_U } } },
      };
   }
   const VkRenderingInfo ri = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea = { .offset = { 0, 0 }, .extent = { W, H } },
      .layerCount = 1,
      .colorAttachmentCount = c->targets,
      .pColorAttachments = att,
   };

   const struct push_data push = { LOOP_N, 0, 0, 0 };

   fw->vk.vkCmdBeginRendering(cb, &ri);
   fw->vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            gfx.pipeline);
   fw->vk.vkCmdPushConstants(cb, gfx.layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                             0, (uint32_t)sizeof(push), &push);
   fw->vk.vkCmdDraw(cb, 3, 1, 0, 0);
   fw->vk.vkCmdEndRendering(cb);

   barrier(fw, cb, img[0].img,
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
   fw->vk.vkCmdCopyImageToBuffer(cb, img[0].img,
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 dst->buf, 1, &region);

   if (!vkfw_submit_and_wait(fw, cb, c->name))
      goto out;
   if (!vkfw_buffer_invalidate(fw, dst))
      goto out;

   {
      /* acc = sum(1..n), i ends at n, kill_mode 0, n. */
      uint32_t acc = 0;
      for (uint32_t i = 0; i < LOOP_N; i++)
         acc += i + 1u;
      const uint32_t want[4] = { acc, LOOP_N, 0u, LOOP_N };

      const uint32_t *base = (const uint32_t *)dst->map;
      uint32_t wrong = 0, first = TEXELS;
      for (uint32_t i = 0; i < TEXELS; i++) {
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
      t_check(t, wrong == 0, "%s: %u/%u texels right in target 0",
              c->name, TEXELS - wrong, TEXELS);
      if (wrong != 0) {
         const uint32_t *got = base + (size_t)first * 4u;
         t_note(t, "%s: first wrong texel %u (%u,%u): got %u %u %u %u, "
                   "want %u %u %u %u", c->name, first, first % W, first / W,
                got[0], got[1], got[2], got[3],
                want[0], want[1], want[2], want[3]);
      }

      vkfw_expect_words(fw, (const uint8_t *)dst->map + IMAGE_B, POISON,
                        (READBACK_B - IMAGE_B) / 4u,
                        "nothing was written past the image");
   }

out:
   vkfw_gfx_destroy(fw, &gfx);
   for (uint32_t i = 0; i < MAX_TARGETS; i++) {
      if (view[i] != VK_NULL_HANDLE)
         fw->vk.vkDestroyImageView(fw->dev, view[i], NULL);
      if (made[i])
         vkfw_image_destroy(fw, &img[i]);
   }
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
