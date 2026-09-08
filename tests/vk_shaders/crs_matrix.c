/*
 * The convergence stack and the register count, varied one at a time.
 *
 * WHY THIS CASE EXISTS. Godot's Forward+ scene fragment shader takes the
 * GM20B's graphics channel down — one draw, `fault notification 8 (fifo
 * idle timeout)`, every submit after it VK_ERROR_DEVICE_LOST — and every
 * attempt to name the cause so far has moved one knob that turns two
 * things at once. The list of what it is NOT is long and is in
 * docs/MEASURED-ON-HARDWARE.md: not the executed code, not the
 * instruction count, not the program size, not shader local memory, not
 * a wrong descriptor read, not the size of the convergence stack, and —
 * since Mobile was measured rendering a 64-register fragment shader —
 * not the register count either.
 *
 * What has never been separated is the pair. Every shader that hangs has
 * both a convergence stack the hardware has to spill to memory and a
 * register count near the top of what a warp can hold; every shader that
 * renders has at most one of the two. This case makes those two
 * independent:
 *
 *      variant   nest    values live across it
 *      A         4       2          neither
 *      B         4       24         some registers, no reservation
 *      C         20      2          a reservation, few registers
 *      D         20      24         both, moderately
 *      E         4       56         many registers, no reservation
 *      F         20      56         both, at the register count of the
 *                                   Mobile shader that renders
 *      G         4       104        the register count of the shader
 *                                   that hangs, without its reservation
 *      H         20      104        both, at the register count of the
 *                                   shader that hangs
 *
 * and the six shaders are generated from one template by
 * scripts/gen-crs-matrix.py, so they differ in those two numbers and in
 * nothing else. If a deep variant hangs where the shallow one with the
 * same register count does not, the pair is the answer and it is
 * reproducible in a binary that rebuilds in a minute instead of a 76 MB
 * Godot export. If nothing hangs, the pair is not sufficient either,
 * and that is worth knowing before anybody patches a register limit or
 * a subtiling knob.
 *
 * THE REGISTER COLUMN IS A MEASUREMENT, NOT A REQUEST. 24 live values
 * came out of this compiler as 32 registers, 56 as 64, and 104 as
 * whatever the log says; the shaders this is about are at 64 (Mobile's
 * heaviest, which renders) and 112 (Godot's scene shader, which hangs).
 * A pair that does not hang at 32 says nothing about 64, and one that
 * does not hang at 64 says nothing about 112, which is why there are
 * four steps rather than two. A variant that SPILLS has stopped
 * measuring registers and started measuring local memory; the report
 * says which.
 *
 * THE NEST IS NOT THE DEPTH, AND THE DEPTH IS NOT WHAT A WARP WALKS.
 * These three get confused every time this is discussed, so this case
 * keeps them apart:
 *
 *   the nest        `levels` above, a property of the assembly, and the
 *                   only one a reader can count;
 *   the depth       what from_nir's convergence stack reached, which is
 *                   larger — the loop pushes too. Measured, not
 *                   derived: NAK_CRS_INFO=1 makes the compiler print it,
 *                   and `crs_frag_deep`'s twelve levels came out as
 *                   depth 15 on this compiler;
 *   the reservation what sm50's crs_size() makes of that depth: zero at
 *                   or below sixteen, 1024 above it. It is what the
 *                   shader program header carries and the queue programs
 *                   per warp;
 *   what is walked  a property of the DATA. `mode` decides it: 0 nobody
 *                   enters the nest, 1 every lane walks to the bottom,
 *                   2 the two lanes of every quad take different depths.
 *                   A shader compiled with a reservation and never
 *                   entered is a different question from the same binary
 *                   entered divergently, and both are asked here.
 *
 * AND THE COMPILER HAS TO BE THIS ONE. Every crsinfo number recorded
 * before 2026-09-07 came from a working tree that doubles the depth
 * before reserving — nouveau counts two slots per nesting level where
 * sm50 counts one — and also carries codegen changes of its own. Read
 * against the pinned tree's crs_size(), those numbers say the opposite
 * of what they meant: twelve levels "reserve nothing" in one compiler
 * and 1024 bytes in the other. Whatever this log says is what THIS
 * driver did, which is the whole reason the numbers are printed beside
 * the pixels.
 *
 * WHAT IT CHECKS BESIDES NOT HANGING. Every texel of every draw is
 * compared against the same arithmetic in C, so a variant that renders
 * something wrong is a failure and not a pass: a deep nest whose lanes
 * reconverge at the wrong place would corrupt the accumulator, and the
 * live band is carried through the loop precisely so that a register
 * allocator that lost one of them shows up as a wrong sum rather than as
 * a number nobody reads.
 *
 * OCCUPANCY IS THE THIRD DIMENSION, AND IT IS WHY THIS FILE CHANGED.
 * Measured 2026-09-07 at 16x16, every variant rendered — including H at
 * 112 registers with a 1024-byte reservation walked divergently, the
 * compiled profile of the shader that hangs. But 16x16 is 256 pixels,
 * which is eight warps; the draw that hangs is 1280x720, which is
 * nearly thirty thousand. Per-warp state — the convergence stack, the
 * slice of the register file each warp holds — is a resource that
 * scales with exactly that number and with nothing else this case
 * varies, so a matrix that renders at eight warps says nothing about
 * the one question left standing about the pair.
 *
 * So the whole matrix runs twice: once at 16x16, where every texel is
 * compared, and once at 1280x720, where the same draw puts as many
 * warps in flight as the failing one does. The second pass compares a
 * dense block at the origin plus a prime-strided sample across the rest
 * — about a thousand texels, chosen so that x, y and (x^y)&1 all vary
 * along the walk — because comparing nine hundred thousand of them
 * against 104 live values each costs minutes of CPU and answers nothing
 * the sample does not.
 *
 * WHAT IS STILL NOT VARIED, which is the list the next single variable
 * comes off:
 *
 *   size           1006 instructions here against 3932 there.
 *   what it reads  a push constant, and nothing else. The shader that
 *                  hangs reads uniform buffers, storage buffers and
 *                  textures, and discards.
 *   the frame      one draw in one render pass, against a depth
 *                  prepass, a shadow atlas and a dozen pipelines.
 *
 * None of those is varied here on purpose: this case exists because the
 * last four attempts moved two things at once.
 *
 * ORDER MATTERS HERE. The small extent runs first, all eight variants,
 * and only then does any of them draw at 1280x720: the small pass is
 * the shape that has passed on hardware, so a run that dies in the big
 * pass still carries a full result for it. Within a pass the variants
 * run A to H — least to most suspected — and the log is flushed per
 * line, so if one of them takes the channel down the file already holds
 * everything the run established. A device lost between variants stops
 * the case rather than reporting more failures with one cause.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "common/vkfw.h"

#include "fmt_vert_fullscreen.spv.h"
#include "crs_mx_a.spv.h"
#include "crs_mx_b.spv.h"
#include "crs_mx_c.spv.h"
#include "crs_mx_d.spv.h"
#include "crs_mx_e.spv.h"
#include "crs_mx_f.spv.h"
#include "crs_mx_g.spv.h"
#include "crs_mx_h.spv.h"

#define SMALL_W      16u
#define SMALL_H      16u
/* The extent of the draw that hangs, which is the whole point of the
 * second pass. */
#define BIG_W        1280u
#define BIG_H        720u
#define TEXEL_B      16u                    /* R32G32B32A32_UINT */
#define TAIL_B       1024u
#define READBACK_B   ((BIG_W * BIG_H) * TEXEL_B + TAIL_B)
#define POISON       0xdeadbeefu
#define CLEAR_U      0xc1ea5eedu

/* How the big pass is sampled: every texel of the first block, then one
 * in every BIG_STRIDE. The stride is prime and coprime with 1280, so
 * the walk does not keep landing in the same column and (x^y)&1 — the
 * bit that decides which lanes enter the nest — varies along it. */
#define DENSE_TEXELS 4096u
#define BIG_STRIDE   1021u

/* The two passes. `dense` is what makes the small one a full
 * comparison and the big one a sample; nothing else differs. */
struct crs_extent {
   uint32_t w, h;
   bool dense;
};

static const struct crs_extent EXTENTS[] = {
   { SMALL_W, SMALL_H, true },
   { BIG_W,   BIG_H,   false },
};
#define NUM_EXTENTS (sizeof(EXTENTS) / sizeof(EXTENTS[0]))

/* The widest live band any variant has. */
#define MAX_LIVE     104u

/* THE SAME NUMBERS scripts/gen-crs-matrix.py WAS RUN WITH. They are
 * repeated here because this side has to compute what the shader should
 * produce, and a mismatch between the two would make every texel wrong
 * rather than silently pass — which is the failure mode worth having. */
struct crs_variant {
   const char *name;
   const uint32_t *spv;
   uint32_t spv_B;
   uint32_t levels;
   uint32_t live;
};

static const struct crs_variant VARIANTS[] = {
   { "A shallow nest, 2 live",  crs_mx_a_spv, sizeof(crs_mx_a_spv),  4,  2 },
   { "B shallow nest, 24 live", crs_mx_b_spv, sizeof(crs_mx_b_spv),  4, 24 },
   { "C deep nest, 2 live",     crs_mx_c_spv, sizeof(crs_mx_c_spv), 20,  2 },
   { "D deep nest, 24 live",    crs_mx_d_spv, sizeof(crs_mx_d_spv), 20, 24 },
   { "E shallow nest, 56 live", crs_mx_e_spv, sizeof(crs_mx_e_spv),  4, 56 },
   { "F deep nest, 56 live",    crs_mx_f_spv, sizeof(crs_mx_f_spv), 20, 56 },
   { "G shallow nest, 104 live", crs_mx_g_spv, sizeof(crs_mx_g_spv), 4, 104 },
   { "H deep nest, 104 live",   crs_mx_h_spv, sizeof(crs_mx_h_spv), 20, 104 },
};
#define NUM_VARIANTS (sizeof(VARIANTS) / sizeof(VARIANTS[0]))

/* n is the loop bound; mode is which lanes enter the nest. n = 0 with
 * mode 2 is the case where the stack is walked divergently and the loop
 * under it never runs, which separates the nest from its body. */
struct crs_input {
   const char *what;
   uint32_t n;
   uint32_t mode;
};

static const struct crs_input INPUTS[] = {
   { "nobody enters",            4, 0 },
   { "every lane to the bottom", 4, 1 },
   { "lanes diverge, no loop",   0, 2 },
   { "lanes diverge, loop 4",    4, 2 },
};
#define NUM_INPUTS (sizeof(INPUTS) / sizeof(INPUTS[0]))

/* uvec4(n, mode, 0, 0), matching the generated shaders. */
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

/* Exactly what the generated shader does, in C. Unsigned arithmetic
 * wraps in both, which is what makes the comparison exact. */
static void expect_texel(const struct crs_variant *v,
                         const struct crs_input *in,
                         uint32_t x, uint32_t y, uint32_t out[4])
{
   const uint32_t all_bits = (v->levels >= 32u)
                             ? 0xffffffffu : ((1u << v->levels) - 1u);
   const uint32_t sel = (x ^ y) & 1u;
   uint32_t cond;

   if (in->mode == 0)
      cond = 0;
   else if (in->mode == 1)
      cond = all_bits;
   else
      cond = sel ? all_bits : 1u;

   uint32_t live[MAX_LIVE];
   for (uint32_t k = 0; k < v->live; k++)
      live[k] = (x * (2u * k + 1u)) ^ (y + k) ^ in->n;

   uint32_t acc = 0, i = 0;

   /* The loop sits under every level, so it runs only when every bit is
    * set — which is what "the lane reached the bottom" means. */
   if ((cond & all_bits) == all_bits) {
      for (i = 0; i < in->n; i++) {
         acc += i + 1u;
         for (uint32_t k = 0; k < v->live; k++)
            live[k] = live[k] * 3u + acc;
      }
   }

   uint32_t sum = 0;
   for (uint32_t k = 0; k < v->live; k++)
      sum += live[k];

   out[0] = acc;
   out[1] = i;
   out[2] = cond;
   out[3] = sum;
}

/* One variant at one extent, all four inputs, one image and one
 * pipeline. Returns false when the device is gone and there is no point
 * continuing. */
static bool run_variant(vkfw *fw, const struct crs_variant *v,
                        const struct crs_extent *e, vkfw_buffer *dst)
{
   test_ctx *t = fw->t;
   const uint32_t w = e->w, h = e->h;
   const uint32_t texels = w * h;
   const uint32_t image_B = texels * TEXEL_B;
   const VkFormat format = VK_FORMAT_R32G32B32A32_UINT;
   const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
   vkfw_image img = { 0 };
   VkImageView view = VK_NULL_HANDLE;
   vkfw_gfx gfx = { 0 };
   bool alive = true;

   if (!vkfw_image_create(fw, format, (VkExtent3D){ w, h, 1 }, 1, 1,
                          usage, VK_IMAGE_TILING_OPTIMAL, &img))
      return true;

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
                v->name, vkfw_result_str(r)))
      goto out;

   /* The pipeline is where the shader is compiled, so the `NAK crs:`
    * line for this variant appears in the log here — before any draw
    * that could take the channel down. */
   t_note(t, "%s @%ux%u: compiling (%u nesting levels, %u values live)",
          v->name, w, h, v->levels, v->live);

   const vkfw_gfx_desc desc = {
      .vs_spv = fmt_vert_fullscreen_spv,
      .vs_B = sizeof(fmt_vert_fullscreen_spv),
      .fs_spv = v->spv,
      .fs_B = v->spv_B,
      .colour_format = format,
      .depth_format = VK_FORMAT_UNDEFINED,
      .push_constant_B = (uint32_t)sizeof(struct push_data),
      .push_constant_stages = VK_SHADER_STAGE_FRAGMENT_BIT,
      .width = w, .height = h,
   };
   char pipe_name[128];
   snprintf(pipe_name, sizeof(pipe_name), "%s @%ux%u", v->name, w, h);
   if (!vkfw_gfx_create(fw, pipe_name, &desc, &gfx))
      goto out;

   for (uint32_t ii = 0; ii < NUM_INPUTS; ii++) {
      const struct crs_input *in = &INPUTS[ii];
      VkCommandBuffer cb;

      if (vkfw_device_lost(fw)) {
         t_note(t, "%s @%ux%u: device lost; \"%s\" and what follows not "
                   "attempted", v->name, w, h, in->what);
         alive = false;
         goto out;
      }

      /* Said before the submit rather than after it: if this draw is
       * the one that never retires, this line is the last thing in the
       * log and it names exactly which shader and which input. */
      t_note(t, "%s @%ux%u: about to draw \"%s\" (n=%u mode=%u), "
                "%u pixels", v->name, w, h, in->what, in->n, in->mode,
             texels);

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
         .renderArea = { .offset = { 0, 0 }, .extent = { w, h } },
         .layerCount = 1,
         .colorAttachmentCount = 1,
         .pColorAttachments = &att,
      };

      const struct push_data push = { in->n, in->mode, 0, 0 };

      fw->vk.vkCmdBeginRendering(cb, &ri);
      fw->vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                               gfx.pipeline);
      fw->vk.vkCmdPushConstants(cb, gfx.layout,
                                VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                (uint32_t)sizeof(push), &push);
      fw->vk.vkCmdDraw(cb, 3, 1, 0, 0);
      fw->vk.vkCmdEndRendering(cb);

      barrier(fw, cb, img.img,
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
         .imageExtent = { w, h, 1 },
      };
      fw->vk.vkCmdCopyImageToBuffer(cb, img.img,
                                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    dst->buf, 1, &region);

      char what[160];
      snprintf(what, sizeof(what), "%s @%ux%u: %s", v->name, w, h,
               in->what);

      if (!vkfw_submit_and_wait(fw, cb, what))
         continue;
      if (!vkfw_buffer_invalidate(fw, dst))
         continue;

      const uint32_t *base = (const uint32_t *)dst->map;
      uint32_t wrong = 0, first = texels, deep = 0, looked = 0;
      for (uint32_t p = 0; p < texels; p++) {
         /* The big pass looks at the dense block and then every
          * BIG_STRIDE'th texel; the small one at all of them. */
         if (!e->dense && p >= DENSE_TEXELS && (p % BIG_STRIDE) != 0u)
            continue;

         const uint32_t x = p % w, y = p / w;
         uint32_t want[4];
         expect_texel(v, in, x, y, want);
         deep += (want[1] == in->n && in->n != 0);
         looked++;
         const uint32_t *got = base + (size_t)p * 4u;
         bool ok = true;
         for (uint32_t k = 0; k < 4; k++)
            ok = ok && got[k] == want[k];
         if (!ok) {
            if (first == texels)
               first = p;
            wrong++;
         }
      }

      t_check(t, wrong == 0,
              "%s: %u/%u compared texels right, of %u drawn (%u lanes "
              "reached the loop)", what, looked - wrong, looked, texels,
              deep);
      if (wrong != 0) {
         uint32_t want[4];
         expect_texel(v, in, first % w, first / w, want);
         const uint32_t *got = base + (size_t)first * 4u;
         t_note(t, "%s: first wrong texel %u (%u,%u): got acc=%u i=%u "
                   "cond=0x%x sum=0x%08x, want acc=%u i=%u cond=0x%x "
                   "sum=0x%08x", what, first, first % w, first / w,
                got[0], got[1], got[2], got[3],
                want[0], want[1], want[2], want[3]);
      }

      vkfw_expect_words(fw, (const uint8_t *)dst->map + image_B, POISON,
                        (READBACK_B - image_B) / 4u,
                        "nothing was written past the image");
   }

out:
   vkfw_gfx_destroy(fw, &gfx);
   if (view != VK_NULL_HANDLE)
      fw->vk.vkDestroyImageView(fw->dev, view, NULL);
   vkfw_image_destroy(fw, &img);
   return alive;
}

TEST_CASE_DECL(vk_shaders, crs_matrix)
{
   /* Read per compilation by the driver, so a case in the middle of a
    * suite can still turn it on — NAK_DEBUG is latched in a static on
    * the first shader of the process. testfw restores the environment
    * around every case. */
   setenv("NAK_CRS_INFO", "1", 1);
   /* AND THE SHADER CACHE HAS TO BE OUT OF THE WAY, or the report is a
    * property of what a previous run left on the card: a shader that
    * comes back from disk_cache is never handed to NAK, so it prints
    * nothing and this case's own scan for the report fails on the
    * second launch of the same build. Measured that way on 2026-09-07,
    * by vk_shaders/nested_control_flow_frag doing exactly this. */
   setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);

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

   for (uint32_t xi = 0; xi < NUM_EXTENTS; xi++) {
      const struct crs_extent *e = &EXTENTS[xi];
      bool alive = true;

      t_note(t, "=== %ux%u, %u pixels, %s ===", e->w, e->h, e->w * e->h,
             e->dense ? "every texel compared"
                      : "a dense block plus a strided sample compared");

      for (uint32_t i = 0; i < NUM_VARIANTS; i++) {
         if (!run_variant(&fw, &VARIANTS[i], e, &dst)) {
            t_note(t, "%u variant(s) after \"%s\" at %ux%u not attempted",
                   (unsigned)(NUM_VARIANTS - i - 1), VARIANTS[i].name,
                   e->w, e->h);
            alive = false;
            break;
         }
      }

      if (!alive) {
         t_note(t, "%u extent(s) after %ux%u not attempted",
                (unsigned)(NUM_EXTENTS - xi - 1), e->w, e->h);
         break;
      }
   }

   /* The four `NAK crs:` lines are the other half of this case: without
    * them the four variants are four names, and which one carried a
    * reservation is a guess about a compiler. */
   bool reported = false;
   if (t_log_scan(t, "NAK crs: stage=fragment", &reported)) {
      t_check(t, reported,
              "the compiler reported what each variant compiled to — "
              "depth, reservation, registers and spills are on the "
              "`NAK crs:` lines above");
   } else {
      t_check(t, false,
              "the log could not be read back, so what the four "
              "variants compiled to was NOT recorded");
   }

out:
   vkfw_buffer_destroy(&fw, &dst);
   vkfw_finish(&fw);
   return 0;
}
