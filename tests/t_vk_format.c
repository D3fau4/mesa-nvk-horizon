/*
 * Phase 5 item 8 — format coverage.
 *
 * Every earlier item in this phase used R8G8B8A8_UNORM and nothing
 * else, so everything they proved, they proved about one format. This
 * one renders a known value into a 16x16 target in each of twelve
 * formats and checks the bytes that come back — the actual encoding,
 * not "it did not crash".
 *
 * WHAT EACH CASE STATES. A format's entry carries the value the shader
 * writes and the sixteen bytes one texel must then hold, with the
 * conversion between them derived in its comment. Nothing is computed
 * from a format-description table, because a table is another thing
 * that can be wrong in the same direction as the driver; a literal that
 * disagrees with the hardware is a disagreement.
 *
 * WHY THE FLOAT VALUES ARE THE ONES THEY ARE. A UNORM channel of n bits
 * comes back as round(f * (2^n - 1)), so each value here is k/(2^n - 1)
 * for the k the entry expects. In float32 that product lands within
 * about a millionth of k — the largest miss in the whole table is 3e-7
 * — which no correctly rounded conversion can turn into k-1 or k+1.
 * Values like 0.5 were avoided wherever they would have produced a tie.
 *
 * A FORMAT THE DRIVER DOES NOT CLAIM IS NOT A FAILURE. Each case first
 * asks vkGetPhysicalDeviceFormatProperties whether the format can be a
 * colour attachment and a transfer source with optimal tiling. If it
 * cannot, the case says so and is skipped; if it can, it must work.
 * Which formats are claimed is itself reported, because "twelve
 * formats" and "twelve formats that this chip supports" are different
 * measurements and the log should say which one happened.
 *
 * THREE FRAGMENT SHADERS, NOT ONE. A fragment output's numeric type
 * must match its attachment's. Binding a float shader to a UINT
 * attachment is invalid usage, there are no validation layers here, and
 * the result would be a wrong answer rather than an error — so UINT and
 * SINT attachments get their own shaders and their own pipelines.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <string.h>

#include "common/vkfw.h"

#include "fmt_vert_fullscreen.spv.h"
#include "fmt_frag_float.spv.h"
#include "fmt_frag_uint.spv.h"
#include "fmt_frag_sint.spv.h"

const char *const test_name = "t_vk_format";
/* No display: main() starts a console and reports through it. */
const bool test_uses_display = false;

/* Small on purpose: a 16-byte-per-texel format at 64x64 would be 64 KiB
 * of readback for an answer that is the same in every texel. 16x16 is
 * still 256 independent texels, which is what catches a driver that
 * wrote only the first one. */
#define W            16u
#define H            16u
#define TEXELS       (W * H)
#define MAX_TEXEL_B  16u
#define IMAGE_B      (TEXELS * MAX_TEXEL_B)
#define TAIL_B       1024u
#define READBACK_B   (IMAGE_B + TAIL_B)
#define POISON       0xdeadbeefu

enum kind { KIND_FLOAT, KIND_UINT, KIND_SINT };

/* Sixteen bytes of push constant, read by whichever fmt_frag_* shader
 * matches the attachment. */
union push_data {
   float f[4];
   uint32_t u[4];
   int32_t i[4];
};

struct fmt_case {
   VkFormat format;
   const char *name;
   uint32_t texel_B;
   enum kind kind;
   union push_data push;
   uint8_t expect[MAX_TEXEL_B];
   /* Per byte, in counts. Zero everywhere except sRGB — see its
    * entry. */
   uint8_t tolerance;
};

/* The twelve. Ordered so the plain ones come first: if the whole test
 * is going to fail, it should fail on R8G8B8A8_UNORM, which every
 * earlier item already passed, rather than on something exotic. */
static const struct fmt_case CASES[] = {
   /* 0.2/0.4/0.6/0.8 * 255 = 51.0000008 / 102.0000015 / 153.0000061 /
    * 204.0000030. The format every other item in this phase used. */
   { VK_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM", 4, KIND_FLOAT,
     { .f = { 0.2f, 0.4f, 0.6f, 0.8f } },
     { 51, 102, 153, 204 }, 0 },

   /* The same four channel values, stored B, G, R, A. Identical to the
    * entry above except for the byte order, which is the whole point:
    * a driver that ignored the swizzle passes one of these two. */
   { VK_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM", 4, KIND_FLOAT,
     { .f = { 0.2f, 0.4f, 0.6f, 0.8f } },
     { 153, 102, 51, 204 }, 0 },

   /* One channel and two channels. The shader still writes a vec4; the
    * components with no channel to land in must be dropped, not packed
    * forward, and the readback would show it if they were. */
   { VK_FORMAT_R8_UNORM, "R8_UNORM", 1, KIND_FLOAT,
     { .f = { 0.2f, 0.4f, 0.6f, 0.8f } },
     { 51 }, 0 },
   { VK_FORMAT_R8G8_UNORM, "R8G8_UNORM", 2, KIND_FLOAT,
     { .f = { 0.2f, 0.4f, 0.6f, 0.8f } },
     { 51, 102 }, 0 },

   /* IEEE 754 binary16, little endian: 0.5 = 0x3800, 0.25 = 0x3400,
    * 1.0 = 0x3c00, 1.5 = 0x3e00. All four are exact in half precision,
    * so this measures the conversion and not a rounding rule. */
   { VK_FORMAT_R16G16B16A16_SFLOAT, "R16G16B16A16_SFLOAT", 8, KIND_FLOAT,
     { .f = { 0.5f, 0.25f, 1.0f, 1.5f } },
     { 0x00, 0x38, 0x00, 0x34, 0x00, 0x3c, 0x00, 0x3e }, 0 },

   /* The same four values as binary32: 0x3f000000, 0x3e800000,
    * 0x3f800000, 0x3fc00000, little endian. Nothing is converted at
    * all here, which makes it the case that separates "the ROP wrote
    * the wrong bits" from "the ROP converted wrongly". */
   { VK_FORMAT_R32G32B32A32_SFLOAT, "R32G32B32A32_SFLOAT", 16, KIND_FLOAT,
     { .f = { 0.5f, 0.25f, 1.0f, 1.5f } },
     { 0x00, 0x00, 0x00, 0x3f, 0x00, 0x00, 0x80, 0x3e,
       0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0xc0, 0x3f }, 0 },

   /* A2B10G10R10_UNORM_PACK32: R in bits 0-9, G in 10-19, B in 20-29,
    * A in 30-31. 1/3 * 1023 = 341.00001, 2/3 * 1023 = 682.00002,
    * 1.0 * 1023 = 1023, 2/3 * 3 = 2.0000001, so the word is
    * 341 | (682 << 10) | (1023 << 20) | (2 << 30) = 0xbffaa955.
    * Three different bit widths in one word: a shift that is off by
    * one anywhere shows up here and nowhere else. */
   { VK_FORMAT_A2B10G10R10_UNORM_PACK32, "A2B10G10R10_UNORM_PACK32", 4,
     KIND_FLOAT,
     { .f = { 1.0f / 3.0f, 2.0f / 3.0f, 1.0f, 2.0f / 3.0f } },
     { 0x55, 0xa9, 0xfa, 0xbf }, 0 },

   /* R5G6B5_UNORM_PACK16: R in bits 11-15, G in 5-10, B in 0-4.
    * 10/31 * 31 = 9.9999997, 40/63 * 63 = 40.0000014,
    * 25/31 * 31 = 25.0000002, so the halfword is
    * (10 << 11) | (40 << 5) | 25 = 0x5519. There is no alpha channel;
    * the shader still writes one and it must go nowhere. */
   { VK_FORMAT_R5G6B5_UNORM_PACK16, "R5G6B5_UNORM_PACK16", 2, KIND_FLOAT,
     { .f = { 10.0f / 31.0f, 40.0f / 63.0f, 25.0f / 31.0f, 1.0f } },
     { 0x19, 0x55 }, 0 },

   /* sRGB. The shader writes linear values and the ROP encodes them
    * with 1.055 * c^(1/2.4) - 0.055 above the linear segment:
    *   0.1  -> 89.044     0.25 -> 136.960     0.3  -> 148.877
    * and alpha stays linear, 0.8 -> 204.0000030.
    *
    * The only entry with a tolerance, and one count. Vulkan requires
    * the conversion but leaves its precision loose enough that a
    * hardware encode table can differ by a last place; every value
    * here was picked at least a third of a count from a tie, so a
    * tolerance of one covers implementation precision without
    * covering an actual encoding error, which would be tens of
    * counts. */
   { VK_FORMAT_R8G8B8A8_SRGB, "R8G8B8A8_SRGB", 4, KIND_FLOAT,
     { .f = { 0.1f, 0.25f, 0.3f, 0.8f } },
     { 89, 137, 149, 204 }, 1 },

   /* Integers are stored, not converted: 1, 2, 250, 255 as bytes. */
   { VK_FORMAT_R8G8B8A8_UINT, "R8G8B8A8_UINT", 4, KIND_UINT,
     { .u = { 1u, 2u, 250u, 255u } },
     { 1, 2, 250, 255 }, 0 },

   /* 32-bit unsigned, including one value with every bit set and one
    * that does not fit in 16. */
   { VK_FORMAT_R32G32B32A32_UINT, "R32G32B32A32_UINT", 16, KIND_UINT,
     { .u = { 1u, 70000u, 0xdeadbeefu, 0xffffffffu } },
     { 0x01, 0x00, 0x00, 0x00, 0x70, 0x11, 0x01, 0x00,
       0xef, 0xbe, 0xad, 0xde, 0xff, 0xff, 0xff, 0xff }, 0 },

   /* Signed 16-bit, two's complement little endian, at both ends of
    * the range: -1, 2, -32768, 32767. A format path that treated the
    * value as unsigned differs on three of the four. */
   { VK_FORMAT_R16G16B16A16_SINT, "R16G16B16A16_SINT", 8, KIND_SINT,
     { .i = { -1, 2, -32768, 32767 } },
     { 0xff, 0xff, 0x02, 0x00, 0x00, 0x80, 0xff, 0x7f }, 0 },
};
#define NUM_CASES (sizeof(CASES) / sizeof(CASES[0]))

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

/* True when the driver claims this format can be what the case needs
 * it to be. Passes no check: a format that is not claimed is a fact
 * about the device, and the caller reports it as a skip. */
static bool claimed(vkfw *fw, VkFormat format, VkFormatFeatureFlags *feats)
{
   VkFormatProperties props;
   fw->vk.vkGetPhysicalDeviceFormatProperties(fw->pdev, format, &props);
   *feats = props.optimalTilingFeatures;
   const VkFormatFeatureFlags need =
      VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
      VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
   return (props.optimalTilingFeatures & need) == need;
}

static void hexdump(char *out, size_t out_B, const uint8_t *b, uint32_t n)
{
   static const char hex[] = "0123456789abcdef";
   size_t o = 0;
   for (uint32_t i = 0; i < n && o + 3 < out_B; i++) {
      out[o++] = hex[b[i] >> 4];
      out[o++] = hex[b[i] & 0xf];
      if (i + 1 < n)
         out[o++] = ' ';
   }
   out[o < out_B ? o : out_B - 1] = '\0';
}


static const uint32_t *frag_for(enum kind k, size_t *size_B)
{
   switch (k) {
   case KIND_UINT:
      *size_B = sizeof(fmt_frag_uint_spv);
      return fmt_frag_uint_spv;
   case KIND_SINT:
      *size_B = sizeof(fmt_frag_sint_spv);
      return fmt_frag_sint_spv;
   case KIND_FLOAT:
   default:
      *size_B = sizeof(fmt_frag_float_spv);
      return fmt_frag_float_spv;
   }
}

/* Returns true when the case was actually run (as opposed to skipped
 * because the driver does not claim the format). */
static bool run_case(vkfw *fw, const struct fmt_case *c, vkfw_buffer *dst)
{
   test_ctx *t = fw->t;
   VkFormatFeatureFlags feats = 0;

   if (!claimed(fw, c->format, &feats)) {
      t_note(t, "%s: not claimed as a colour attachment that can be copied "
                "out (optimalTilingFeatures 0x%08x); skipped, not failed",
             c->name, (unsigned)feats);
      return false;
   }

   const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
   vkfw_image img = { 0 };
   VkImageView view = VK_NULL_HANDLE;
   vkfw_gfx gfx = { 0 };
   VkCommandBuffer cb;

   if (!vkfw_image_create(fw, c->format, (VkExtent3D){ W, H, 1 }, 1, 1,
                          usage, VK_IMAGE_TILING_OPTIMAL, &img))
      return true;

   const VkImageViewCreateInfo ivci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = img.img,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = c->format,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .levelCount = 1, .layerCount = 1,
      },
   };
   VkResult r = fw->vk.vkCreateImageView(fw->dev, &ivci, NULL, &view);
   if (!t_check(t, r == VK_SUCCESS, "%s: vkCreateImageView -> %s",
                c->name, vkfw_result_str(r)))
      goto out;

   /* One pipeline per format: the attachment format is baked into
    * VkPipelineRenderingCreateInfo, so even two formats that share a
    * fragment shader cannot share a pipeline. */
   size_t fs_B = 0;
   const uint32_t *fs = frag_for(c->kind, &fs_B);
   const vkfw_gfx_desc desc = {
      .vs_spv = fmt_vert_fullscreen_spv,
      .vs_B = sizeof(fmt_vert_fullscreen_spv),
      .fs_spv = fs, .fs_B = fs_B,
      .colour_format = c->format,
      .depth_format = VK_FORMAT_UNDEFINED,
      .push_constant_B = (uint32_t)sizeof(union push_data),
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

   /* LOAD_OP_DONT_CARE, not CLEAR. The draw covers every texel, so a
    * clear would give the target a second writer and a texel that came
    * out right could have come from either. With DONT_CARE the only
    * thing that can have written a correct value is the draw. */
   const VkRenderingAttachmentInfo att = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = view,
      .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
   };
   const VkRenderingInfo ri = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea = { .offset = { 0, 0 }, .extent = { W, H } },
      .layerCount = 1,
      .colorAttachmentCount = 1,
      .pColorAttachments = &att,
   };

   fw->vk.vkCmdBeginRendering(cb, &ri);
   fw->vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            gfx.pipeline);
   fw->vk.vkCmdPushConstants(cb, gfx.layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                             0, (uint32_t)sizeof(c->push), &c->push);
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

   /* Every texel, not the first one: a driver that wrote one texel and
    * left the rest, or that got the stride wrong, differs here and
    * nowhere else. */
   const uint8_t *base = (const uint8_t *)dst->map;
   uint32_t wrong = 0, first = 0;
   for (uint32_t i = 0; i < TEXELS; i++) {
      const uint8_t *got = base + (size_t)i * c->texel_B;
      bool ok = true;
      for (uint32_t b = 0; b < c->texel_B; b++) {
         const int32_t d = (int32_t)got[b] - (int32_t)c->expect[b];
         if (d > (int32_t)c->tolerance || d < -(int32_t)c->tolerance)
            ok = false;
      }
      if (!ok) {
         if (wrong == 0)
            first = i;
         wrong++;
      }
   }

   t_check(t, wrong == 0, "%s: %u/%u texels hold the encoded value%s",
           c->name, TEXELS - wrong, TEXELS,
           c->tolerance ? " (within one count)" : "");
   if (wrong != 0) {
      char got_hex[MAX_TEXEL_B * 3 + 1], want_hex[MAX_TEXEL_B * 3 + 1];
      hexdump(got_hex, sizeof(got_hex), base + (size_t)first * c->texel_B,
              c->texel_B);
      hexdump(want_hex, sizeof(want_hex), c->expect, c->texel_B);
      t_note(t, "%s: first wrong texel %u (%u,%u): got %s, want %s",
             c->name, first, first % W, first / W, got_hex, want_hex);
   }

   /* Nothing past the image. The copy asked for W*H texels of this
    * format's size; a driver using the wrong texel size would overrun
    * here even when every texel it did write was correct. */
   const uint32_t used_B = TEXELS * c->texel_B;
   vkfw_expect_words(fw, base + used_B, POISON,
                     (READBACK_B - used_B) / 4u,
                     "nothing was written past the image");

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
   uint32_t ran = 0;

   if (!vkfw_buffer_create(&fw, READBACK_B, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &dst))
      goto out;

   for (uint32_t i = 0; i < NUM_CASES; i++) {
      /* Once the channel is gone every later case fails the same way,
       * and N identical failures hide which one was first. */
      if (vkfw_device_lost(&fw)) {
         t_note(t, "device lost; %u format(s) from \"%s\" on not attempted",
                (unsigned)(NUM_CASES - i), CASES[i].name);
         break;
      }
      if (run_case(&fw, &CASES[i], &dst))
         ran++;
   }

   /* "Twelve formats passed" and "the four this chip claims passed" are
    * different statements, and only one of them is true. */
   t_note(t, "%u of %u formats were claimed by the driver and tested",
          ran, (unsigned)NUM_CASES);
   t_check(t, ran > 0, "at least one format was testable");

out:
   vkfw_buffer_destroy(&fw, &dst);
   vkfw_finish(&fw);
   return 0;
}
