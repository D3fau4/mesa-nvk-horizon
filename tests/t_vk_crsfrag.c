/*
 * Twelve levels of nesting around a loop, in a fragment shader.
 *
 * WHY THIS EXISTS, AND WHY t_vk_crs IS NOT IT. t_vk_crs asks the same
 * question of a *compute* shader and passes 35/35 on GM20B, which was
 * read as "the convergence stack works". It does not say that about the
 * graphics pipe: the two carry the stack through different state. The
 * compute path writes the size into the QMD (nak/qmd.rs, rounded up to a
 * multiple of 0x200) and programs SET_SHADER_LOCAL_MEMORY_NON_THROTTLED;
 * the graphics path writes it into the shader program header
 * (nak/sph.rs set_shader_local_memory_crs_size, not rounded) and programs
 * SET_SHADER_LOCAL_MEMORY_C/D/E. No fragment shader in this suite has
 * ever needed more than the sixteen on-chip slots, so the graphics half
 * of that pair has never been executed.
 *
 * WHAT MADE IT WORTH ASKING. Measured on GM20B on 2026-09-02, with
 * NAK_DEBUG=crsinfo and Godot 4.1 Forward+ on one cube: of the forty-two
 * shaders the process compiles, exactly one has a non-zero crs_size --
 * the scene fragment shader, instrs=3932 gprs=112 max_crs_depth=13
 * crs_size=1024, every other shader 0 -- and the draw that binds it is
 * the one the channel dies on. The same geometry's depth-prepass draw,
 * earlier in the same pushbuffer, completes; a GPU breadcrumb either side
 * of each 64-dword span puts the failure inside the colour draw, and
 * rendering the 3D pass at a quarter of each axis (sixteen times fewer
 * fragments) fails in the same span, so it is not how much the shader
 * runs.
 *
 * FOUR CASES.
 *
 *   mode 0, n 4   twelve levels compiled in, nobody enters   (control)
 *   mode 1, n 4   every lane goes all twelve levels deep
 *   mode 2, n 0   half the lanes twelve deep, loop not entered
 *   mode 2, n 4   half the lanes twelve deep, loop entered
 *
 * mode 2 is the one that matters: (x ^ y) & 1 is a checkerboard, so the
 * two lanes of every quad take different depths and the stack is pushed
 * and popped divergently -- the shape the scene shader has. Case 3
 * separates the nest from the loop, because t_vk_kill has already shown
 * that a fragment loop on its own is right.
 *
 * LOAD_OP_CLEAR: no texel is killed here, so every one must carry a
 * computed value and the clear must not survive anywhere. Checking
 * against the clear is what would catch a draw that silently did nothing.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <string.h>

#include "common/vkfw.h"

#include "fmt_vert_fullscreen.spv.h"
#include "crs_frag_deep.spv.h"

const char *const test_name = "t_vk_crsfrag";
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

/* Twelve nesting levels; the loop sits under all of them. */
#define LEVELS       12u
#define ALL_LEVELS   ((1u << LEVELS) - 1u)

struct crs_case {
   const char *name;
   uint32_t n;      /* loop bound */
   uint32_t mode;   /* 0 = nobody in, 1 = everybody in, 2 = half in */
};

static const struct crs_case CASES[] = {
   { "twelve levels compiled in, nobody enters", 4, 0 },
   { "every lane twelve levels deep, loop bound 4", 4, 1 },
   { "half the lanes twelve deep, loop never entered", 0, 2 },
   { "half the lanes twelve deep, loop bound 4", 4, 2 },
};
#define NUM_CASES (sizeof(CASES) / sizeof(CASES[0]))

/* uvec4(n, mode, 0, 0), matching crs_frag_deep.spvasm. */
struct push_data {
   uint32_t n, mode, pad0, pad1;
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

/* Exactly what the shader does, in C. */
static void expect_texel(const struct crs_case *c, uint32_t x, uint32_t y,
                         uint32_t out[4])
{
   uint32_t cond;
   if (c->mode == 0)
      cond = 0;
   else if (c->mode == 1)
      cond = ALL_LEVELS;
   else
      cond = (((x ^ y) & 1u) != 0u) ? ALL_LEVELS : 1u;

   uint32_t acc = 0, i = 0;
   if ((cond & ALL_LEVELS) == ALL_LEVELS) {
      for (i = 0; i < c->n; i++)
         acc += i + 1u;
   }
   out[0] = acc;
   out[1] = i;
   out[2] = cond;
   out[3] = c->n;
}

static bool run_case(vkfw *fw, const struct crs_case *c, vkfw_buffer *dst)
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
      .fs_spv = crs_frag_deep_spv,
      .fs_B = sizeof(crs_frag_deep_spv),
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

   const struct push_data push = { c->n, c->mode, 0, 0 };

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
      uint32_t wrong = 0, first = TEXELS, deep = 0;
      for (uint32_t i = 0; i < TEXELS; i++) {
         const uint32_t x = i % W, y = i / W;
         uint32_t want[4];
         expect_texel(c, x, y, want);
         deep += (want[2] == ALL_LEVELS);
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
      t_check(t, wrong == 0, "%s: %u/%u texels right (%u went twelve deep)",
              c->name, TEXELS - wrong, TEXELS, deep);
      if (wrong != 0) {
         uint32_t want[4];
         expect_texel(c, first % W, first / W, want);
         const uint32_t *got = base + (size_t)first * 4u;
         t_note(t, "%s: first wrong texel %u (%u,%u): got acc=%u i=%u "
                   "cond=0x%x n=%u, want acc=%u i=%u cond=0x%x n=%u",
                c->name, first, first % W, first / W,
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
