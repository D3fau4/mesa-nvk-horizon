/*
 * What the driver claims, measured against what the backend can do —
 * and the one operation nothing had ever executed.
 *
 * TWO THINGS THAT HAD NEVER BEEN CHECKED ON HARDWARE, both of them
 * consequences of patches this project wrote.
 *
 * PART A — the extension gating (patch 0028). NVK advertised
 * VK_KHR_external_memory_fd, VK_EXT_external_memory_dma_buf and
 * VK_EXT_map_memory_placed unconditionally, and their entry points call
 * nvkmd operations this backend does not implement: an application that
 * used one would have jumped through a NULL function pointer. Patch
 * 0046 made each follow the nvkmd_info flag its implementation needs.
 * That patch has never run on a console.
 *
 * The gate is checked in BOTH directions, because a gate that only ever
 * says no is indistinguishable from an extension list that was never
 * built:
 *
 *   has_dma_buf   false  ->  KHR_external_memory_fd        ABSENT
 *   has_dma_buf   false  ->  EXT_external_memory_dma_buf   ABSENT
 *   has_map_fixed false  ->  EXT_map_memory_placed         ABSENT
 *   has_alloc_tiled TRUE ->  EXT_image_drm_format_modifier PRESENT
 *   has_sparse    false  ->  every sparse feature          FALSE
 *
 * The values on the left are nvkmd_horizon_pdev.c's, stated there with
 * a reason each. The last two rows are what make this a measurement
 * rather than a formality: one thing the gate must let through, and one
 * feature set patch 0022 made follow a capability instead of the 3D
 * class.
 *
 * PART B — nvkmd_dev_alloc_tiled_mem (patch 0027), executed at last.
 * The operation was a NULL function pointer until patch 0027
 * implemented it, and then nothing reached it: the obvious caller is
 * "render into a VK_IMAGE_TILING_LINEAR image", and NVK refuses that
 * outright — nvk_image.c:76 withholds COLOR_ATTACHMENT from every
 * linear format, upstream, on every chip.
 *
 * There is exactly one other path, and it is the one patch 0027 itself
 * opened. EXT_image_drm_format_modifier is advertised when
 * has_alloc_tiled is true, so setting that flag is what put the
 * extension in the list. An image created with
 * VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT and DRM_FORMAT_MOD_LINEAR is
 * not VK_IMAGE_TILING_LINEAR, so it may be a colour attachment; and
 * when the 3D engine is asked to render into one, NVK allocates a
 * *tiled shadow* for it through nvkmd_dev_alloc_tiled_mem
 * (nvk_cmd_draw.c:1241), renders into that, and copies the result back
 * into the linear image when the render pass ends.
 *
 * So part B renders a known colour into such an image and reads every
 * texel back. Passing means alloc_tiled_mem allocated a correctly
 * tiled, correctly aligned object, the engine rendered into it, and the
 * copy back landed in the linear layout — the whole shadow round trip,
 * which no earlier test could reach.
 *
 * WHY DRM MODIFIERS AT ALL ON A PLATFORM WITH NO DRM. The tokens are
 * named after DRM; the mechanism is not one. Nothing here calls a DRM
 * ioctl, imports a dma-buf or reimplements a kernel interface — NIL
 * knows the layouts by itself, and the extension is the only way in
 * Vulkan to ask for "linear layout, still usable as an attachment".
 * That is a distinction Phase 6 will need for nwindow buffers, so the
 * extension stays and is tested rather than gated away.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "common/vkfw.h"

#include "fmt_vert_fullscreen.spv.h"
#include "fmt_frag_float.spv.h"

const char *const test_name = "t_vk_caps";
/* No display: main() starts a console and reports through it. */
const bool test_uses_display = false;

#define W            64u
#define H            64u
#define TEXELS       (W * H)
#define IMAGE_B      (TEXELS * 4u)
#define TAIL_WORDS   256u
#define READBACK_B   (IMAGE_B + TAIL_WORDS * 4u)
#define POISON       0xdeadbeefu

/* drm_fourcc.h: DRM_FORMAT_MOD_LINEAR is ((__u64)0), the "no vendor,
 * no modifier" code point. Spelled out here because this tree has no
 * DRM headers and should not grow any. */
#define DRM_FORMAT_MOD_LINEAR_LOCAL   UINT64_C(0)

/* The colour, as bytes that round-trip exactly: k/255.0f * 255 lands
 * within a millionth of k. Same four values fmt_frag_float is fed
 * everywhere else. */
#define C_R 0x33u
#define C_G 0x66u
#define C_B 0x99u
#define C_A 0xffu

struct gate {
   const char *name;
   bool expected;
   const char *why;
};

/* nvkmd_horizon_pdev.c is the authority for the right-hand column; each
 * entry names the flag it follows so a log line is readable without the
 * source next to it. */
static const struct gate GATES[] = {
   { "VK_KHR_external_memory_fd", false,
     "has_dma_buf is false: there is no dma-buf and no DRM to carry one" },
   { "VK_EXT_external_memory_dma_buf", false,
     "has_dma_buf is false" },
   { "VK_EXT_map_memory_placed", false,
     "has_map_fixed is false: NvMap hands back a CPU address of its own" },
   { "VK_EXT_image_drm_format_modifier", true,
     "has_alloc_tiled is true, and this is the gate letting one through" },
};
#define NUM_GATES (sizeof(GATES) / sizeof(GATES[0]))

static uint32_t texel(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
   return (a << 24) | (b << 16) | (g << 8) | r;
}

struct push_colour {
   float c[4];
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

/* PART A ---------------------------------------------------------- */

static void check_gates(vkfw *fw)
{
   test_ctx *t = fw->t;
   uint32_t count = 0;

   VkResult r = fw->vk.vkEnumerateDeviceExtensionProperties(fw->pdev, NULL,
                                                            &count, NULL);
   if (!t_check(t, r == VK_SUCCESS, "vkEnumerateDeviceExtensionProperties "
                "(count) -> %s", vkfw_result_str(r)))
      return;
   if (!t_check(t, count > 0, "the device advertises %u extension(s)", count))
      return;

   VkExtensionProperties *props =
      malloc((size_t)count * sizeof(*props));
   if (!t_check(t, props != NULL, "room for %u extension names", count))
      return;

   r = fw->vk.vkEnumerateDeviceExtensionProperties(fw->pdev, NULL, &count,
                                                   props);
   if (!t_check(t, r == VK_SUCCESS, "vkEnumerateDeviceExtensionProperties "
                "(list) -> %s", vkfw_result_str(r))) {
      free(props);
      return;
   }

   for (uint32_t g = 0; g < NUM_GATES; g++) {
      bool present = false;
      for (uint32_t i = 0; i < count; i++) {
         if (strcmp(props[i].extensionName, GATES[g].name) == 0) {
            present = true;
            break;
         }
      }
      t_check(t, present == GATES[g].expected,
              "%s is %s — %s", GATES[g].name,
              GATES[g].expected ? "advertised" : "not advertised",
              GATES[g].why);
      if (present != GATES[g].expected) {
         t_note(t, "%s: expected %s, found %s", GATES[g].name,
                GATES[g].expected ? "present" : "absent",
                present ? "present" : "absent");
      }
   }
   free(props);

   /* Patch 0029 made sparse follow nvkmd_info::has_sparse instead of
    * the 3D class, and this check has moved with the flag rather than
    * being deleted. has_sparse was false and this asserted that nothing
    * sparse was advertised; it is true since 2026-08-24 and this
    * asserts that ALL EIGHT are, because one flag is what gates them
    * (nvk_physical_device.c:395-402) and half of them appearing would
    * mean something else had got in between.
    *
    * What the flag rests on is two runs, not one: t_sparse asked the
    * address space directly — an unbound page inside a sparse
    * reservation swallows a write, and unbinding a bound block puts
    * that state back — and t_vk_sparse asked the same three questions
    * through vkQueueBindSparse and got the same answers, 74/74.
    *
    * ONLY TWO OF THE EIGHT HAVE BEEN EXERCISED: sparseBinding and
    * sparseResidencyBuffer, which are the two a buffer can reach.
    * That is recorded here rather than left for someone to assume from
    * a green run. */
   VkPhysicalDeviceFeatures feats;
   memset(&feats, 0, sizeof(feats));
   fw->vk.vkGetPhysicalDeviceFeatures(fw->pdev, &feats);
   const bool any_sparse =
      feats.sparseBinding || feats.sparseResidencyBuffer ||
      feats.sparseResidencyImage2D || feats.sparseResidencyImage3D ||
      feats.sparseResidency2Samples || feats.sparseResidency4Samples ||
      feats.sparseResidency8Samples || feats.sparseResidencyAliased;
   const bool all_sparse =
      feats.sparseBinding && feats.sparseResidencyBuffer &&
      feats.sparseResidencyImage2D && feats.sparseResidencyImage3D &&
      feats.sparseResidency2Samples && feats.sparseResidency4Samples &&
      feats.sparseResidency8Samples && feats.sparseResidencyAliased;
   t_check(t, all_sparse && any_sparse,
           "every sparse feature is advertised — has_sparse is true, and "
           "patch 0022 made these follow it rather than the 3D class");
}

/* PART B ---------------------------------------------------------- */

/* Reports which modifiers the driver offers for this format, and
 * whether the linear one can be a colour attachment. Returns true when
 * it can, which is the precondition for everything after. */
static bool linear_modifier_usable(vkfw *fw)
{
   test_ctx *t = fw->t;

   VkDrmFormatModifierPropertiesListEXT list = {
      .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
   };
   VkFormatProperties2 fp2 = {
      .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
      .pNext = &list,
   };
   fw->vk.vkGetPhysicalDeviceFormatProperties2(fw->pdev,
                                               VK_FORMAT_R8G8B8A8_UNORM,
                                               &fp2);
   if (!t_check(t, list.drmFormatModifierCount > 0,
                "the driver offers %u DRM format modifier(s) for "
                "R8G8B8A8_UNORM", list.drmFormatModifierCount))
      return false;

   VkDrmFormatModifierPropertiesEXT *mods =
      malloc((size_t)list.drmFormatModifierCount * sizeof(*mods));
   if (!t_check(t, mods != NULL, "room for %u modifier(s)",
                list.drmFormatModifierCount))
      return false;
   list.pDrmFormatModifierProperties = mods;
   fw->vk.vkGetPhysicalDeviceFormatProperties2(fw->pdev,
                                               VK_FORMAT_R8G8B8A8_UNORM,
                                               &fp2);

   bool linear_ok = false;
   for (uint32_t i = 0; i < list.drmFormatModifierCount; i++) {
      t_note(t, "modifier 0x%llx: %u plane(s), features 0x%08x",
             (unsigned long long)mods[i].drmFormatModifier,
             mods[i].drmFormatModifierPlaneCount,
             (unsigned)mods[i].drmFormatModifierTilingFeatures);
      if (mods[i].drmFormatModifier == DRM_FORMAT_MOD_LINEAR_LOCAL &&
          (mods[i].drmFormatModifierTilingFeatures &
           VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
         linear_ok = true;
   }
   free(mods);

   t_check(t, linear_ok,
           "DRM_FORMAT_MOD_LINEAR can be a colour attachment — the only "
           "path in Vulkan that reaches nvkmd_dev_alloc_tiled_mem");
   return linear_ok;
}

static void run_tiled_shadow(vkfw *fw, vkfw_buffer *dst)
{
   test_ctx *t = fw->t;

   const uint64_t modifier = DRM_FORMAT_MOD_LINEAR_LOCAL;
   const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

   /* The query first, with the modifier chained in: an image the
    * driver would refuse is a fact about the device, not a failure to
    * create one. */
   const VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod_query = {
      .sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
      .drmFormatModifier = modifier,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   const VkPhysicalDeviceImageFormatInfo2 fi = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
      .pNext = &mod_query,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .type = VK_IMAGE_TYPE_2D,
      .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
      .usage = usage,
   };
   VkImageFormatProperties2 ifp = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
   };
   VkResult r =
      fw->vk.vkGetPhysicalDeviceImageFormatProperties2(fw->pdev, &fi, &ifp);
   if (!t_check(t, r == VK_SUCCESS,
                "a linear-modifier colour attachment is supported -> %s",
                vkfw_result_str(r)))
      return;

   VkImage img = VK_NULL_HANDLE;
   VkDeviceMemory mem = VK_NULL_HANDLE;
   VkImageView view = VK_NULL_HANDLE;
   vkfw_gfx gfx = { 0 };

   const VkImageDrmFormatModifierListCreateInfoEXT mod_list = {
      .sType =
         VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
      .drmFormatModifierCount = 1,
      .pDrmFormatModifiers = &modifier,
   };
   const VkImageCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = &mod_list,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = { W, H, 1 },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   r = fw->vk.vkCreateImage(fw->dev, &ici, NULL, &img);
   if (!t_check(t, r == VK_SUCCESS,
                "vkCreateImage(DRM_FORMAT_MOD_LINEAR, colour attachment) "
                "-> %s", vkfw_result_str(r)))
      return;

   VkMemoryRequirements req;
   fw->vk.vkGetImageMemoryRequirements(fw->dev, img, &req);
   const uint32_t type =
      vkfw_memory_type(fw, req.memoryTypeBits,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (!t_check(t, type != UINT32_MAX,
                "a device-local memory type the image accepts"))
      goto out;
   t_note(t, "the linear image itself wants 0x%llx bytes, align 0x%llx",
          (unsigned long long)req.size, (unsigned long long)req.alignment);

   const VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size,
      .memoryTypeIndex = type,
   };
   r = fw->vk.vkAllocateMemory(fw->dev, &mai, NULL, &mem);
   if (!t_check(t, r == VK_SUCCESS, "vkAllocateMemory(linear image) -> %s",
                vkfw_result_str(r)))
      goto out;
   r = fw->vk.vkBindImageMemory(fw->dev, img, mem, 0);
   if (!t_check(t, r == VK_SUCCESS, "vkBindImageMemory -> %s",
                vkfw_result_str(r)))
      goto out;

   const VkImageViewCreateInfo ivci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = img,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .levelCount = 1, .layerCount = 1,
      },
   };
   r = fw->vk.vkCreateImageView(fw->dev, &ivci, NULL, &view);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateImageView -> %s",
                vkfw_result_str(r)))
      goto out;

   const vkfw_gfx_desc desc = {
      .vs_spv = fmt_vert_fullscreen_spv,
      .vs_B = sizeof(fmt_vert_fullscreen_spv),
      .fs_spv = fmt_frag_float_spv, .fs_B = sizeof(fmt_frag_float_spv),
      .colour_format = VK_FORMAT_R8G8B8A8_UNORM,
      .depth_format = VK_FORMAT_UNDEFINED,
      .push_constant_B = (uint32_t)sizeof(struct push_colour),
      .push_constant_stages = VK_SHADER_STAGE_FRAGMENT_BIT,
      .width = W, .height = H,
   };
   if (!vkfw_gfx_create(fw, "linear modifier", &desc, &gfx))
      goto out;

   if (!vkfw_buffer_poison(fw, dst, POISON))
      goto out;

   VkCommandBuffer cb;
   if (!vkfw_cmd_begin(fw, &cb))
      goto out;

   image_barrier(fw, cb, img,
                 VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

   /* DONT_CARE, not CLEAR: the draw covers every texel, so a clear
    * would give the attachment a second writer and a correct texel
    * could have come from either. */
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

   struct push_colour pc;
   pc.c[0] = (float)C_R / 255.0f;
   pc.c[1] = (float)C_G / 255.0f;
   pc.c[2] = (float)C_B / 255.0f;
   pc.c[3] = (float)C_A / 255.0f;

   fw->vk.vkCmdBeginRendering(cb, &ri);
   fw->vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            gfx.pipeline);
   fw->vk.vkCmdPushConstants(cb, gfx.layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                             0, (uint32_t)sizeof(pc), &pc);
   fw->vk.vkCmdDraw(cb, 3, 1, 0, 0);
   /* Ending the render pass is what copies the tiled shadow back into
    * the linear image. Everything this part measures happens between
    * vkCmdBeginRendering and here. */
   fw->vk.vkCmdEndRendering(cb);

   image_barrier(fw, cb, img,
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
      .imageExtent = { W, H, 1 },
   };
   fw->vk.vkCmdCopyImageToBuffer(cb, img,
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 dst->buf, 1, &region);

   if (!vkfw_submit_and_wait(fw, cb, "render into a linear-modifier image"))
      goto out;
   if (!vkfw_buffer_invalidate(fw, dst))
      goto out;

   vkfw_expect_words(fw, dst->map, texel(C_R, C_G, C_B, C_A), TEXELS,
                     "every texel of the linear image holds what the "
                     "tiled shadow rendered");
   vkfw_expect_words(fw, (const uint32_t *)dst->map + TEXELS, POISON,
                     TAIL_WORDS, "nothing was written past the image");

out:
   vkfw_gfx_destroy(fw, &gfx);
   if (view != VK_NULL_HANDLE)
      fw->vk.vkDestroyImageView(fw->dev, view, NULL);
   if (img != VK_NULL_HANDLE)
      fw->vk.vkDestroyImage(fw->dev, img, NULL);
   if (mem != VK_NULL_HANDLE)
      fw->vk.vkFreeMemory(fw->dev, mem, NULL);
}

int run_test(test_ctx *t)
{
   VkPhysicalDeviceVulkan13Features features13 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
      .dynamicRendering = VK_TRUE,
   };

   /* Part B creates a VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT image, so
    * the extension has to be ENABLED on the device and not merely
    * advertised by it. Run 1 of this test enabled nothing and passed
    * anyway, which is what invalid usage looks like when there are no
    * validation layers: a wrong answer instead of an error. Found in
    * review of PR #7. */
   static const char *const device_exts[] = {
      "VK_EXT_image_drm_format_modifier",
   };

   vkfw fw;
   if (!vkfw_init_ext(&fw, t, &features13, device_exts, 1))
      return 1;

   vkfw_buffer dst = { 0 };

   check_gates(&fw);

   if (!vkfw_buffer_create(&fw, READBACK_B, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &dst))
      goto out;

   if (linear_modifier_usable(&fw))
      run_tiled_shadow(&fw, &dst);

out:
   vkfw_buffer_destroy(&fw, &dst);
   vkfw_finish(&fw);
   return 0;
}
