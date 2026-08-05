/*
 * Phase 5 items 3 and 4 — off-screen images, and clears.
 *
 * WHY THE TWO ITEMS ARE ONE TEST. "Off-screen image" is not observable
 * on its own: creating one and binding memory to it produces nothing a
 * CPU readback can look at, and writing to it with a copy is item 1.
 * What makes an image *off-screen* is being rendered to, and the
 * smallest thing that renders to one is a clear — which in NVK is not a
 * blit and not a shader but a render pass:
 * nvk_CmdClearColorImage builds a VkRenderingInfo with
 * LOAD_OP_CLEAR and calls nvk_CmdBeginRendering
 * (nvk_cmd_clear.c:298-325). So this test is the first time the 3D
 * engine is asked to bind a render target on this platform, and it asks
 * that without a shader in the picture — which is the point of doing it
 * before item 2.
 *
 * WHAT EACH CASE IS FOR:
 *
 *   optimal 64x64, two layers   the ordinary case, plus the bounds
 *                               check: layer 1 is cleared to a second
 *                               colour and layer 0 must still hold the
 *                               first. A clear that ignores its
 *                               subresource range fails here and
 *                               nowhere else.
 *   optimal 67x53, one layer    a non-power-of-two extent, so the
 *                               surface has padding NIL chose and the
 *                               copy back has to respect it. An extent
 *                               that is all powers of two would let a
 *                               stride bug pass.
 *   linear 64x64, one layer     VK_IMAGE_TILING_LINEAR as a colour
 *                               attachment, which NVK implements by
 *                               allocating a *tiled shadow* through
 *                               nvkmd_dev_alloc_tiled_mem
 *                               (nvk_cmd_draw.c:1055) — the operation
 *                               that was a NULL function pointer until
 *                               patch 0045. This case is the one that
 *                               executes it.
 *
 * Both colours are non-zero and different from each other, so a driver
 * that cleared everything to black, or ignored the second clear, is
 * caught by the value and not only by the position.
 *
 * WHY THIS TEST MUST NOT COMPILE ANYTHING BEFORE ITS FIRST RENDER PASS.
 * On 2026-08-04 the first render pass this project ever submitted
 * MMU-faulted: nothing was written, the fault surfaced one case later at
 * the next kickoff, and vkWaitForFences had already returned VK_SUCCESS
 * for it (fixed separately in horizon/channel — a faulted channel's
 * syncpoints are force-incremented by nvgpu's recovery, so a reached
 * threshold is not evidence the work ran).
 *
 * The cause was in the VA map that run printed:
 *
 *   map VA 0xa14a000+0x100000000 page 0x1000  (nothing bound)
 *
 * NVK's shader heap — 4 GiB of contiguous VA reserved at device
 * creation, whose first chunk was bound only when a shader was first
 * uploaded, and whose base is handed to the engines as
 * SET_PROGRAM_REGION. Nothing in this test compiled a shader, so the 3D
 * engine began a render pass with a program region pointing at unbacked
 * address space, and the instruction cache's pre-fetch of that address
 * is a read of nothing. t_vk_compute, which does compile a shader, ran
 * on the same console without faulting; run 3 of this test compiled a
 * throwaway one on purpose and passed 76/76, which is the controlled
 * experiment that named the cause. Patch 0048 fixes it in the driver:
 * the heap binds its first chunk in vkCreateDevice.
 *
 * That is why this test compiles no shader, creates no pipeline and no
 * descriptor set layout (which also uploads to the shader heap,
 * nvk_descriptor_set_layout.c:399) before the first clear. The
 * throwaway compute pipeline that used to stand here is gone. Adding
 * any of them back would make the driver bind that chunk as a side
 * effect and this test would stop being able to catch the regression —
 * which is the whole reason it is arranged this way.
 *
 * The cases are also ordered simplest-first. Run 1 started with the
 * two-layer image, which confounded "layered rendering" with "the first
 * render pass at all".
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <string.h>

#include "common/vkfw.h"

const char *const test_name = "t_vk_image";
/* No display: main() starts a console and reports through it. */
const bool test_uses_display = false;

#define POISON        0xdeadbeefu

/* Largest case: 64 x 64 x 2 layers of RGBA8. */
#define READBACK_B    (64u * 64u * 2u * 4u)

/* Clear colours as bytes. UNORM conversion is round-to-nearest of
 * f * 255, and k/255.0f round-trips to exactly k, so these are the
 * bytes that must come back. */
#define A_R 0x12u
#define A_G 0x34u
#define A_B 0x56u
#define A_A 0x78u
#define B_R 0x9au
#define B_G 0xbcu
#define B_B 0xdeu
#define B_A 0xf0u

/* RGBA8_UNORM puts R in the lowest address, so a little-endian 32-bit
 * read of one texel is A:B:G:R from the top. */
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

static void image_barrier(vkfw *fw, VkCommandBuffer cb, VkImage img,
                          uint32_t layers,
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
         .layerCount = layers,
      },
   };
   fw->vk.vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0,
                               0, NULL, 0, NULL, 1, &b);
}

struct image_case {
   const char *name;
   VkImageTiling tiling;
   uint32_t w, h, layers;
};

static void run_case(vkfw *fw, const struct image_case *c, vkfw_buffer *dst)
{
   test_ctx *t = fw->t;
   const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT;

   if (!vkfw_image_supported(fw, VK_FORMAT_R8G8B8A8_UNORM,
                             VK_IMAGE_TYPE_2D, c->tiling, usage, c->name)) {
      t_note(t, "%s: the driver does not support this image "
                "configuration; skipped, not failed", c->name);
      return;
   }

   vkfw_image img;
   if (!vkfw_image_create(fw, VK_FORMAT_R8G8B8A8_UNORM,
                          (VkExtent3D){ c->w, c->h, 1 }, 1, c->layers,
                          usage, c->tiling, &img))
      return;

   /* Item 3's own observable: what the driver asked for. The alignment
    * is the interesting half — a block-linear image must be bound at
    * the address space's big-page granularity, and this is where that
    * shows. */
   t_note(t, "%s: %ux%u x%u layer(s), memory 0x%llx bytes, align 0x%llx",
          c->name, c->w, c->h, c->layers,
          (unsigned long long)img.alloc_B,
          (unsigned long long)img.align_B);

   const uint32_t texels_per_layer = c->w * c->h;
   const uint32_t total_texels = texels_per_layer * c->layers;

   if (!vkfw_buffer_poison(fw, dst, POISON))
      goto out;

   VkCommandBuffer cb;
   if (!vkfw_cmd_begin(fw, &cb))
      goto out;

   image_barrier(fw, cb, img.img, c->layers,
                 VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT);

   const VkClearColorValue colour_a = unorm_colour(A_R, A_G, A_B, A_A);
   const VkImageSubresourceRange all = {
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .baseMipLevel = 0, .levelCount = 1,
      .baseArrayLayer = 0, .layerCount = c->layers,
   };
   fw->vk.vkCmdClearColorImage(cb, img.img,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               &colour_a, 1, &all);

   if (c->layers > 1) {
      /* Between two clears of the same image: the second must not start
       * before the first has finished, and nothing else orders them. */
      image_barrier(fw, cb, img.img, c->layers,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);

      const VkClearColorValue colour_b = unorm_colour(B_R, B_G, B_B, B_A);
      const VkImageSubresourceRange layer1 = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .baseMipLevel = 0, .levelCount = 1,
         .baseArrayLayer = 1, .layerCount = 1,
      };
      fw->vk.vkCmdClearColorImage(cb, img.img,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  &colour_b, 1, &layer1);
   }

   image_barrier(fw, cb, img.img, c->layers,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT);

   /* One region per layer, packed back to back, so the readback buffer
    * is layer 0 then layer 1 with nothing between them. */
   VkBufferImageCopy regions[2];
   for (uint32_t l = 0; l < c->layers; l++) {
      regions[l] = (VkBufferImageCopy){
         .bufferOffset = (VkDeviceSize)l * texels_per_layer * 4u,
         .bufferRowLength = 0,
         .bufferImageHeight = 0,
         .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = l,
            .layerCount = 1,
         },
         .imageOffset = { 0, 0, 0 },
         .imageExtent = { c->w, c->h, 1 },
      };
   }
   fw->vk.vkCmdCopyImageToBuffer(cb, img.img,
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 dst->buf, c->layers, regions);

   if (!vkfw_submit_and_wait(fw, cb, c->name))
      goto out;
   if (!vkfw_buffer_invalidate(fw, dst))
      goto out;

   const uint32_t *got = (const uint32_t *)dst->map;
   vkfw_expect_words(fw, got, texel(A_R, A_G, A_B, A_A), texels_per_layer,
                     c->name);
   if (c->layers > 1) {
      vkfw_expect_words(fw, got + texels_per_layer,
                        texel(B_R, B_G, B_B, B_A), texels_per_layer,
                        "layer 1 holds the second colour");
   }

   /* Nothing wrote past the last texel the copy asked for. A copy that
    * used the image's *padded* stride instead of the packed one would
    * still fill the region and would also spill past it. */
   if (total_texels * 4u < READBACK_B) {
      vkfw_expect_words(fw, got + total_texels, POISON,
                        (uint32_t)(READBACK_B / 4u) - total_texels,
                        "the readback buffer past the image is untouched");
   }

out:
   vkfw_image_destroy(fw, &img);
}

int run_test(test_ctx *t)
{
   vkfw fw;
   if (!vkfw_init(&fw, t, NULL))
      return 1;

   vkfw_buffer dst = { 0 };
   if (!vkfw_buffer_create(&fw, READBACK_B, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &dst))
      goto out;

   t_note(t, "no shader, pipeline or descriptor set layout has been created "
             "on this device: the first render pass below reaches the 3D "
             "engine with a program region the driver had to make valid on "
             "its own (patch 0048)");

   /* Simplest first. Run 1 began with the two-layer image, so a failure
    * there could not distinguish layered rendering from the first render
    * pass of any kind. */
   static const struct image_case cases[] = {
      { "optimal 64x64",             VK_IMAGE_TILING_OPTIMAL, 64, 64, 1 },
      { "optimal 67x53",             VK_IMAGE_TILING_OPTIMAL, 67, 53, 1 },
      { "optimal 64x64, two layers", VK_IMAGE_TILING_OPTIMAL, 64, 64, 2 },
      { "linear 64x64",              VK_IMAGE_TILING_LINEAR,  64, 64, 1 },
   };
   for (uint32_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      /* Once the channel is gone every later case fails the same way, and
       * N identical failures bury which one was first. */
      if (vkfw_device_lost(&fw)) {
         t_note(t, "device lost; %u case(s) after \"%s\" not attempted",
                (unsigned)(sizeof(cases) / sizeof(cases[0]) - i),
                cases[i - 1].name);
         break;
      }
      run_case(&fw, &cases[i], &dst);
   }

out:
   vkfw_buffer_destroy(&fw, &dst);
   vkfw_finish(&fw);
   return 0;
}
