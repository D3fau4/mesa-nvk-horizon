/*
 * The two Maxwell-B privileged register writes, asked for on a console.
 *
 * WHY THIS CASE EXISTS. `nvk_push_draw_state_init` issues two
 * privileged graphics-register writes while the queue's context is
 * being initialised, and this backend has refused them since Phase 4:
 * `nvkmd_horizon_pdev.c` publishes `has_priv_reg_writes = false`
 * because known-risk R18 says Horizon rejects such a write from an
 * unprivileged channel and resets the channel, after which every submit
 * on it times out.
 *
 * R18 WAS INHERITED FROM THE REFERENCE PORTS AND NEVER MEASURED HERE,
 * and two things argue with it. deko3d makes masked writes to
 * 0x00418800, 0x00419a08, 0x00419f78, 0x00404468 and 0x00419a04 from an
 * ordinary Horizon application, and 0x00419f78 is the first of the two
 * registers NVK wants — so at least one Switch renderer does this
 * routinely. And NVK does not need a privileged path to try: the write
 * is an MME macro that hands the register number to `SET_FALCON04` and
 * then spins on a scratch handshake, which is a method any channel can
 * push. Whether the handshake completes is a question about this
 * console's FECS microcode and nobody has asked it.
 *
 * WHAT IS AT STAKE, in NVK's own words. The first write clears bit 3 of
 * `gr_gpcs_tpcs_sm_disp_ctrl`, which enables FP helper-invocation
 * memory loads. The second clears bit 14 of
 * `gr_gpcs_tpcs_sms_hww_warp_esr_report_mask`, which disables Out Of
 * Range Address exceptions — and those, NVK says, otherwise "kill the
 * context", for an empty fragment shader and for array overruns on I/O
 * arrays.
 *
 * AND THE CONSOLE HAS NOW ANSWERED THE TWO DIFFERENTLY. The first
 * version of this case asked for both at once, on 2026-09-08, and the
 * queue context's first submit died with
 *
 *   channel: fault notification 31 (MMU fault) — marking lost
 *   error: notifier=31 error_type=2 (graphics exception)
 *   intr=0x00080000 addr=0x00102310 data=0x0000000000419e44
 *
 * `gr_intr` bit 19 is the FECS-error bit, and the trapped method data
 * is 0x419e44 — the warp-ESR report mask, which is the SECOND write.
 * The first register, 0x419f78, goes out before it in the same push and
 * appears in no trap record, so FECS took that one and refused this
 * one. R18 is right about half of what it claimed and wrong about the
 * other half, which is why the option is now a mask.
 *
 * AND THE ACCEPTED WRITE IS THE FORWARD+ FIX, which is why this case is
 * now guarding a default rather than exploring an option. Measured the
 * same day on one Godot binary with one variable — `GODOT_NO_PSO_CACHE=1`
 * and `MESA_SHADER_CACHE_DISABLE=true` on both runs, so neither could be
 * reading the other's compiled shaders: with the write, all eight bench
 * phases render, twice; without it, `3d_cubes_200` takes the channel
 * down with `fault notification 8 (fifo idle timeout)`, which is the
 * failure this project has had six runs out of six of since
 * 2026-08-23. `NVK_HORIZON_PRIV_REG` now defaults to 1 for that reason.
 * The table is in `docs/MEASURED-ON-HARDWARE.md`.
 *
 * The bit enables FP helper-invocation memory loads, and Godot's scene
 * fragment shader is the first thing this project has run that both
 * discards and reads memory — so it is the first with helper lanes
 * issuing loads. Nothing in tests/ pairs those: `crs_matrix` reads a
 * push constant, `descriptor_set1` reads without discarding,
 * `fragment_kill` discards without reading.
 *
 * WHAT THIS CASE ASKS. Not "is Forward+ fixed" — it draws no Godot
 * shader and cannot say. It asks the two things that have to hold for
 * the default to be safe:
 *
 *   1. does a device whose queue context wrote gr_gpcs_tpcs_sm_disp_ctrl
 *      come up, submit and retire, run after run;
 *   2. does an ordinary draw through it produce the same image as the
 *      same draw without it.
 *
 * It does NOT ask for the refused write. Doing so costs a channel and
 * then the process — the launch that measured it ended in the crash
 * dialog after RESULT — and a deliberate fault every run is what
 * `gpu_fault` is a separate `.nro` for. The refusal is recorded in
 * `docs/MEASURED-ON-HARDWARE.md` and in `nvkmd_horizon_pdev.c`, where
 * the mask is documented; `NVK_HORIZON_PRIV_REG=3` reproduces it in one
 * launch for anyone who needs to see it again.
 *
 * A IS WRITTEN AS 0 AND NOT LEFT UNSET, and since the default became 1
 * that is load-bearing rather than tidy: unset now means "make the
 * write", and the control half would stop being a control.
 *
 * So the case runs one draw twice — mask 0, then mask 1 — and compares
 * the two readbacks texel for texel. Comparing against a control rather
 * than against arithmetic in C is deliberate: the shader's own
 * correctness is `crs_matrix`'s job, and what is asked here is only
 * whether the register write changed anything, which a self-comparison
 * answers without a second model to keep in step.
 *
 * The image also has to be non-trivial in both halves. A run where the
 * draw silently did nothing would compare equal to another run where it
 * did nothing, so both halves are checked against the clear value
 * before they are checked against each other.
 *
 * THE FLAG HAS TO BE SEEN TO ARRIVE. `debug_get_bool_option` reading a
 * variable this case sets on itself is the only way a homebrew process
 * can turn a driver option on, and a typo in the name would read as
 * "the writes were made and nothing happened" — the most misleading
 * result available. `nvkmd_horizon` therefore logs once, at device
 * creation, when it publishes the capability, and this case scans the
 * log for that line. No line, no verdict.
 *
 * WHY IT IS LAST IN THE SUITE. R18's failure mode is a channel that
 * stops retiring, and half of R18 turned out to be real. Every case
 * here builds its own instance and device, so the blast radius should
 * be this case's own channel — but "should be" is why this one runs
 * after the others rather than before them, and why the log is flushed
 * per line: when the console did go the way R18 says, everything the
 * suite had established was already on the card, and the case still
 * reported its own verdict before the process died.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "common/vkfw.h"

#include "fmt_vert_fullscreen.spv.h"
#include "crs_mx_g.spv.h"

#define PR_W         256u
#define PR_H         256u
#define PR_TEXELS    (PR_W * PR_H)
#define PR_TEXEL_B   16u                    /* R32G32B32A32_UINT */
#define PR_IMAGE_B   (PR_TEXELS * PR_TEXEL_B)
#define PR_WORDS     (PR_IMAGE_B / 4u)
#define PR_CLEAR_U   0xc1ea5eedu

/* uvec4(n, mode, 0, 0), the push constants crs_mx_g takes. mode 2 is
 * the divergent walk — the most work the shader can be asked for, so
 * the draw is not a case the hardware could shortcut. */
struct pr_push {
   uint32_t n, mode, pad0, pad1;
};

static void pr_barrier(vkfw *fw, VkCommandBuffer cb, VkImage img,
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
         .levelCount = 1, .layerCount = 1,
      },
   };
   fw->vk.vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0,
                               0, NULL, 0, NULL, 1, &b);
}

/* One whole device, one draw, one readback into `out`. Its own instance
 * and device, because the option this case moves is read where the
 * physical device is created and nowhere later. Returns false when the
 * half did not produce an image, having said why. */
static bool pr_draw_once(test_ctx *t, const char *half, uint32_t *out)
{
   const VkFormat format = VK_FORMAT_R32G32B32A32_UINT;
   VkPhysicalDeviceVulkan13Features features13 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
      .dynamicRendering = VK_TRUE,
   };
   vkfw fw;
   vkfw_buffer dst = { 0 };
   vkfw_image img = { 0 };
   VkImageView view = VK_NULL_HANDLE;
   vkfw_gfx gfx = { 0 };
   bool ok = false;

   if (!vkfw_init(&fw, t, &features13))
      return false;

   if (!vkfw_buffer_create(&fw, PR_IMAGE_B, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &dst))
      goto out;

   if (!vkfw_image_create(&fw, format, (VkExtent3D){ PR_W, PR_H, 1 }, 1, 1,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                          VK_IMAGE_TILING_OPTIMAL, &img))
      goto out;

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
   VkResult r = fw.vk.vkCreateImageView(fw.dev, &ivci, NULL, &view);
   if (!t_check(t, r == VK_SUCCESS, "%s: vkCreateImageView -> %s",
                half, vkfw_result_str(r)))
      goto out;

   const vkfw_gfx_desc desc = {
      .vs_spv = fmt_vert_fullscreen_spv,
      .vs_B = sizeof(fmt_vert_fullscreen_spv),
      .fs_spv = crs_mx_g_spv,
      .fs_B = sizeof(crs_mx_g_spv),
      .colour_format = format,
      .depth_format = VK_FORMAT_UNDEFINED,
      .push_constant_B = (uint32_t)sizeof(struct pr_push),
      .push_constant_stages = VK_SHADER_STAGE_FRAGMENT_BIT,
      .width = PR_W, .height = PR_H,
   };
   if (!vkfw_gfx_create(&fw, half, &desc, &gfx))
      goto out;

   VkCommandBuffer cb;
   if (!vkfw_cmd_begin(&fw, &cb))
      goto out;

   pr_barrier(&fw, cb, img.img,
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
      .clearValue = { .color = { .uint32 = { PR_CLEAR_U, PR_CLEAR_U,
                                             PR_CLEAR_U, PR_CLEAR_U } } },
   };
   const VkRenderingInfo ri = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea = { .offset = { 0, 0 }, .extent = { PR_W, PR_H } },
      .layerCount = 1,
      .colorAttachmentCount = 1,
      .pColorAttachments = &att,
   };
   const struct pr_push push = { 4u, 2u, 0u, 0u };

   fw.vk.vkCmdBeginRendering(cb, &ri);
   fw.vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gfx.pipeline);
   fw.vk.vkCmdPushConstants(cb, gfx.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                            (uint32_t)sizeof(push), &push);
   fw.vk.vkCmdDraw(cb, 3, 1, 0, 0);
   fw.vk.vkCmdEndRendering(cb);

   pr_barrier(&fw, cb, img.img,
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
      .imageExtent = { PR_W, PR_H, 1 },
   };
   fw.vk.vkCmdCopyImageToBuffer(cb, img.img,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                dst.buf, 1, &region);

   /* Said before the submit: if this is the submit that never retires,
    * this line is the last thing in the log and it names which half. */
   t_note(t, "%s: about to submit the draw", half);

   if (!vkfw_submit_and_wait(&fw, cb, half))
      goto out;
   if (!vkfw_buffer_invalidate(&fw, &dst))
      goto out;

   memcpy(out, dst.map, PR_IMAGE_B);
   ok = true;

out:
   if (!ok && vkfw_device_lost(&fw))
      t_note(t, "%s: the device was lost, which is what R18 predicts "
                "when a channel is reset under it", half);

   vkfw_gfx_destroy(&fw, &gfx);
   if (view != VK_NULL_HANDLE)
      fw.vk.vkDestroyImageView(fw.dev, view, NULL);
   vkfw_image_destroy(&fw, &img);
   vkfw_buffer_destroy(&fw, &dst);
   vkfw_finish(&fw);
   return ok;
}

/* How many of `n` words are the clear value. A draw that did nothing
 * leaves all of them, and two of those compare equal to each other. */
static uint32_t pr_count_clear(const uint32_t *w, uint32_t n)
{
   uint32_t c = 0;
   for (uint32_t i = 0; i < n; i++)
      c += (w[i] == PR_CLEAR_U);
   return c;
}

TEST_CASE_DECL(vk_shaders, priv_reg)
{
   /* Both halves compile their shaders in-process. A shader that came
    * back from disk_cache in the second half would be the same binary
    * either way, which is fine — but the compile is also where a
    * difference would show first, and a cached one hides it. */
   setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);

   uint32_t *off = malloc(PR_IMAGE_B);
   uint32_t *on = malloc(PR_IMAGE_B);
   if (!t_check(t, off != NULL && on != NULL,
                "two %u KiB readback copies fit on the heap",
                PR_IMAGE_B / 1024u)) {
      free(off);
      free(on);
      return 1;
   }

   /* A: the control. Written as 0 rather than left unset, because the
    * process may have been launched with anything. */
   setenv("NVK_HORIZON_PRIV_REG", "0", 1);
   const bool a_ok = pr_draw_once(t, "A no priv reg write", off);
   t_check(t, a_ok, "A: the control device came up, drew and retired");

   /* B: the same draw, with the queue context writing the one register
    * this console's FECS accepts. */
   setenv("NVK_HORIZON_PRIV_REG", "1", 1);
   const bool b_ok = pr_draw_once(t, "B sm_disp_ctrl written", on);
   unsetenv("NVK_HORIZON_PRIV_REG");

   t_check(t, b_ok,
           "B: a device whose queue context wrote "
           "gr_gpcs_tpcs_sm_disp_ctrl (0x419f78) came up, drew and "
           "retired — R18 said the channel is reset instead, and for "
           "this register it is not");

   /* The flag has to be seen to have arrived, or B proves nothing. */
   bool announced = false;
   if (t_log_scan(t, "NVK_HORIZON_PRIV_REG=1 —", &announced)) {
      t_check(t, announced,
              "the driver announced the capability, so the option "
              "reached nvkmd_horizon and B really is the other shape");
   } else {
      t_check(t, false,
              "the log could not be read back, so whether the option "
              "reached the driver at all is NOT established");
   }

   if (a_ok && b_ok) {
      const uint32_t a_clear = pr_count_clear(off, PR_WORDS);
      const uint32_t b_clear = pr_count_clear(on, PR_WORDS);

      t_check(t, a_clear == 0 && b_clear == 0,
              "both halves drew over the whole attachment (%u and %u "
              "of %u words still hold the clear value)",
              a_clear, b_clear, PR_WORDS);

      uint32_t wrong = 0, first = PR_WORDS;
      for (uint32_t i = 0; i < PR_WORDS; i++) {
         if (off[i] != on[i]) {
            if (first == PR_WORDS)
               first = i;
            wrong++;
         }
      }

      t_check(t, wrong == 0,
              "the two images are identical: %u/%u words agree",
              PR_WORDS - wrong, PR_WORDS);
      if (wrong != 0)
         t_note(t, "first disagreement at word %u (texel %u, component "
                   "%u): control 0x%08x, writes made 0x%08x",
                first, first / 4u, first % 4u, off[first], on[first]);
   } else {
      t_note(t, "the comparison was not attempted: %s did not produce "
                "an image", a_ok ? "B" : "A");
   }

   free(off);
   free(on);
   return 0;
}
