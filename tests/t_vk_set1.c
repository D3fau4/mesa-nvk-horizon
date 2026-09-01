/*
 * Does a fragment shader read descriptor set 1 correctly?
 *
 * WHY THIS EXISTS. Godot 4.1's Forward+ scene shader reads garbage out of
 * descriptor set 1 on GM20B, and it is the whole set rather than one
 * binding: the SSBO's contents are wrong, its runtime-array length is
 * wrong -- `cluster_buffer.data.length()` is not 176640, and that length
 * comes from the descriptor's own size field, so the descriptor is wrong
 * and not the index -- and the UBO at binding 1 is wrong too. The vertex
 * shader's view of the same set is fine, because its UBO is promoted to a
 * constant buffer whose address NVK resolves on the CPU: it never reads
 * set memory. That asymmetry is the shape of the bug.
 *
 * Everything about that shader's *control flow* is already excluded on
 * hardware -- t_vk_loop, t_vk_loop2, t_vk_crs and t_vk_kill all pass, so
 * a loop, a deep convergence stack, the fragment stage and a kill are all
 * fine on this chip (docs/MEASURED-ON-HARDWARE.md). What is left is the
 * descriptor read, and this asks it with nothing else in the shader.
 *
 * FOUR NUMBERS PER TEXEL, so one run says which part broke:
 *
 *   .x  data0[i]       set 0 binding 0, an SSBO -- the control
 *   .y  data1[i]       set 1 binding 0, an SSBO -- the suspect
 *   .z  data1.length() set 1's descriptor size field
 *   .w  ubo1.v.x       set 1 binding 1, a second descriptor type
 *
 * The two SSBOs carry distinct high bits (0x10000000 and 0x20000000) and
 * their own index in the low bits, so the failure names itself: a .y in
 * the 0x1 range means the shader read set 0 for set 1, a .y whose low
 * bits are not the texel's index means the base address is shifted, and
 * a wrong .z means the size field is wrong while the address may not be.
 * They are 64 KiB rather than a few words because Godot's is 1.5 MB and a
 * buffer small enough to sit inside one lucky page proves less.
 *
 * THREE BINDING SHAPES. The sets are identical in all three; only the
 * vkCmdBindDescriptorSets calls differ, because that is the one thing
 * Godot does that the existing tests do not:
 *
 *   A  both sets in one call (firstSet 0, count 2)
 *   B  two calls, set 0 then set 1 -- Godot's shape
 *   C  two calls, set 1 then set 0
 *
 * If A passes and B fails, the bug is in how a second call updates the
 * root descriptor table. If all three fail, it is the set address or the
 * memory behind it, and the binding order is not involved.
 *
 * TWO MORE, AND THEY ARE THE ONES GODOT ACTUALLY DOES. A, B and C read a
 * buffer the CPU wrote before the submit. Godot's cluster buffer is
 * written by the *GPU* and read by the fragment shader in the same
 * frame, and the two ways it was made to hold zeros both left the shader
 * reading nonzero:
 *
 *   D  vkCmdFillBuffer over set 1's SSBO, then the draw  (buffer_clear)
 *   E  vkCmdCopyBuffer into it from a staging buffer     (buffer_update)
 *
 * Both are transfer writes in the same command buffer as the draw, with
 * a buffer barrier from TRANSFER_WRITE to SHADER_READ at the fragment
 * stage in between -- so if the draw does not see them, what is missing
 * is on the *write* side. That direction is untested: NVK_DEBUG=inval_all
 * invalidates the reader's caches at every barrier and was excluded on
 * hardware, which says nothing about whether the write ever left.
 *
 * LOAD_OP_CLEAR so an unwritten texel is a value the shader cannot
 * produce, rather than undefined.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <string.h>

#include "common/vkfw.h"

#include "fmt_vert_fullscreen.spv.h"
#include "set1_frag.spv.h"

const char *const test_name = "t_vk_set1";
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

/* 64 KiB each: bigger than a page, and a length the shader reports. */
#define SSBO_WORDS   16384u
#define SSBO_B       (SSBO_WORDS * 4u)
#define DATA0_BASE   0x10000000u
#define DATA1_BASE   0x20000000u
#define UBO_WORDS    4u
#define UBO_B        (UBO_WORDS * 4u)
#define UBO_X        0xabcd0001u

/* What the GPU puts in set 1's SSBO in cases D and E. Neither can be
 * confused with the CPU-written contents or with the poison. */
#define FILL_U       0x5a5a5a5au
#define STAGE_BASE   0x30000000u

/* Who wrote set 1's SSBO before the draw reads it. */
enum write_mode {
   WRITE_CPU,      /* the CPU, before the submit -- cases A, B, C */
   WRITE_FILL,     /* vkCmdFillBuffer in this command buffer      */
   WRITE_COPY,     /* vkCmdCopyBuffer in this command buffer      */
};

struct bind_case {
   const char *name;
   bool split;     /* two vkCmdBindDescriptorSets calls, not one */
   bool reverse;   /* when split, bind set 1 before set 0        */
   enum write_mode wmode;
};

static const struct bind_case CASES[] = {
   { "both sets in one call", false, false, WRITE_CPU },
   { "two calls, set 0 then set 1 (Godot's shape)", true, false, WRITE_CPU },
   { "two calls, set 1 then set 0", true, true, WRITE_CPU },
   /* From here the GPU is the writer, which is Godot's shape and the
    * one thing A-C cannot ask. They run last because they overwrite
    * the buffer A-C read. */
   { "set 1's SSBO filled by the GPU, then read (buffer_clear)",
     false, false, WRITE_FILL },
   { "set 1's SSBO copied into by the GPU, then read (buffer_update)",
     false, false, WRITE_COPY },
};
#define NUM_CASES (sizeof(CASES) / sizeof(CASES[0]))

/* Everything the cases share, built once. */
struct fixture {
   VkDescriptorSetLayout l0, l1;
   VkDescriptorPool pool;
   VkDescriptorSet s0, s1;
   vkfw_buffer b0, b1, ubo, stage;
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

/* Exactly what the shader writes, in C. Only .y moves between cases:
 * the point of D and E is that everything else must stay right while
 * the contents of set 1's SSBO change under the draw. */
static void expect_texel(const struct bind_case *c, uint32_t i,
                         uint32_t out[4])
{
   out[0] = DATA0_BASE + i;
   switch (c->wmode) {
   case WRITE_FILL: out[1] = FILL_U;          break;
   case WRITE_COPY: out[1] = STAGE_BASE + i;  break;
   default:         out[1] = DATA1_BASE + i;  break;
   }
   out[2] = SSBO_WORDS;
   out[3] = UBO_X;
}

static bool fixture_create(vkfw *fw, struct fixture *f)
{
   test_ctx *t = fw->t;

   /* Set 0: one SSBO. Set 1: the same SSBO shape plus a UBO, so the
    * suspect set is asked about two descriptor types at once. */
   const VkDescriptorSetLayoutBinding b0[] = {
      { .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
   };
   const VkDescriptorSetLayoutBinding b1[] = {
      { .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
      { .binding = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
   };
   VkDescriptorSetLayoutCreateInfo lci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1, .pBindings = b0,
   };
   VkResult r = fw->vk.vkCreateDescriptorSetLayout(fw->dev, &lci, NULL,
                                                   &f->l0);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateDescriptorSetLayout(set 0) "
                "-> %s", vkfw_result_str(r)))
      return false;

   lci.bindingCount = 2;
   lci.pBindings = b1;
   r = fw->vk.vkCreateDescriptorSetLayout(fw->dev, &lci, NULL, &f->l1);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateDescriptorSetLayout(set 1) "
                "-> %s", vkfw_result_str(r)))
      return false;

   const VkDescriptorPoolSize sizes[] = {
      { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 },
      { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 },
   };
   const VkDescriptorPoolCreateInfo pci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 2,
      .poolSizeCount = 2,
      .pPoolSizes = sizes,
   };
   r = fw->vk.vkCreateDescriptorPool(fw->dev, &pci, NULL, &f->pool);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateDescriptorPool -> %s",
                vkfw_result_str(r)))
      return false;

   const VkDescriptorSetLayout layouts[2] = { f->l0, f->l1 };
   VkDescriptorSet sets[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
   const VkDescriptorSetAllocateInfo dsai = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = f->pool,
      .descriptorSetCount = 2,
      .pSetLayouts = layouts,
   };
   r = fw->vk.vkAllocateDescriptorSets(fw->dev, &dsai, sets);
   if (!t_check(t, r == VK_SUCCESS, "vkAllocateDescriptorSets(2) -> %s",
                vkfw_result_str(r)))
      return false;
   f->s0 = sets[0];
   f->s1 = sets[1];

   if (!vkfw_buffer_create(fw, SSBO_B, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &f->b0))
      return false;
   /* TRANSFER_DST as well: cases D and E have the GPU write it. */
   if (!vkfw_buffer_create(fw, SSBO_B,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &f->b1))
      return false;
   if (!vkfw_buffer_create(fw, SSBO_B, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &f->stage))
      return false;
   if (!vkfw_buffer_create(fw, UBO_B, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &f->ubo))
      return false;

   uint32_t *p0 = (uint32_t *)f->b0.map;
   uint32_t *p1 = (uint32_t *)f->b1.map;
   for (uint32_t i = 0; i < SSBO_WORDS; i++) {
      p0[i] = DATA0_BASE + i;
      p1[i] = DATA1_BASE + i;
   }
   uint32_t *ps = (uint32_t *)f->stage.map;
   for (uint32_t i = 0; i < SSBO_WORDS; i++)
      ps[i] = STAGE_BASE + i;
   uint32_t *pu = (uint32_t *)f->ubo.map;
   for (uint32_t i = 0; i < UBO_WORDS; i++)
      pu[i] = UBO_X + i;

   if (!vkfw_buffer_flush(fw, &f->b0) || !vkfw_buffer_flush(fw, &f->b1) ||
       !vkfw_buffer_flush(fw, &f->stage) || !vkfw_buffer_flush(fw, &f->ubo))
      return false;

   /* Explicit ranges rather than VK_WHOLE_SIZE: the runtime array's
    * length is one of the four numbers under test, so the range it is
    * derived from is stated here and not inferred. */
   const VkDescriptorBufferInfo i0 = { f->b0.buf, 0, SSBO_B };
   const VkDescriptorBufferInfo i1 = { f->b1.buf, 0, SSBO_B };
   const VkDescriptorBufferInfo iu = { f->ubo.buf, 0, UBO_B };
   const VkWriteDescriptorSet writes[] = {
      { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = f->s0, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &i0 },
      { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = f->s1, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &i1 },
      { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = f->s1, .dstBinding = 1, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .pBufferInfo = &iu },
   };
   fw->vk.vkUpdateDescriptorSets(fw->dev, 3, writes, 0, NULL);
   t_note(t, "sets written: set 0 SSBO 0x%x.., set 1 SSBO 0x%x.., "
             "set 1 UBO 0x%08x, both SSBOs %u words",
          DATA0_BASE >> 24, DATA1_BASE >> 24, UBO_X, SSBO_WORDS);
   return true;
}

static void fixture_destroy(vkfw *fw, struct fixture *f)
{
   vkfw_buffer_destroy(fw, &f->ubo);
   vkfw_buffer_destroy(fw, &f->stage);
   vkfw_buffer_destroy(fw, &f->b1);
   vkfw_buffer_destroy(fw, &f->b0);
   if (f->pool != VK_NULL_HANDLE)
      fw->vk.vkDestroyDescriptorPool(fw->dev, f->pool, NULL);
   if (f->l1 != VK_NULL_HANDLE)
      fw->vk.vkDestroyDescriptorSetLayout(fw->dev, f->l1, NULL);
   if (f->l0 != VK_NULL_HANDLE)
      fw->vk.vkDestroyDescriptorSetLayout(fw->dev, f->l0, NULL);
}

static void bind_sets(vkfw *fw, VkCommandBuffer cb, VkPipelineLayout layout,
                      const struct fixture *f, const struct bind_case *c)
{
   if (!c->split) {
      const VkDescriptorSet both[2] = { f->s0, f->s1 };
      fw->vk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                     layout, 0, 2, both, 0, NULL);
      return;
   }
   /* Two calls. Each names its own firstSet, so the order they are
    * issued in is the only difference between B and C. */
   if (c->reverse) {
      fw->vk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                     layout, 1, 1, &f->s1, 0, NULL);
      fw->vk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                     layout, 0, 1, &f->s0, 0, NULL);
   } else {
      fw->vk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                     layout, 0, 1, &f->s0, 0, NULL);
      fw->vk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                     layout, 1, 1, &f->s1, 0, NULL);
   }
}

/* Names the component that went wrong, which is the whole point of
 * carrying four different numbers. */
static const char *const COMPONENT[4] = {
   "set 0 SSBO", "set 1 SSBO", "set 1 array length", "set 1 UBO",
};

static bool run_case(vkfw *fw, const struct bind_case *c,
                     const struct fixture *f, vkfw_buffer *dst)
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

   const VkDescriptorSetLayout layouts[2] = { f->l0, f->l1 };
   const vkfw_gfx_desc desc = {
      .vs_spv = fmt_vert_fullscreen_spv,
      .vs_B = sizeof(fmt_vert_fullscreen_spv),
      .fs_spv = set1_frag_spv,
      .fs_B = sizeof(set1_frag_spv),
      .colour_format = format,
      .depth_format = VK_FORMAT_UNDEFINED,
      .set_layout_count = 2,
      .set_layouts = layouts,
      .width = W, .height = H,
   };
   if (!vkfw_gfx_create(fw, c->name, &desc, &gfx))
      goto out;

   if (!vkfw_buffer_poison(fw, dst, POISON))
      goto out;
   if (!vkfw_cmd_begin(fw, &cb))
      goto out;

   /* Cases D and E: the GPU writes set 1's SSBO in this same command
    * buffer, then the draw reads it. The barrier is the whole point --
    * TRANSFER_WRITE to SHADER_READ at the fragment stage is exactly what
    * Godot's buffer_clear and buffer_update rely on. */
   if (c->wmode != WRITE_CPU) {
      if (c->wmode == WRITE_FILL) {
         fw->vk.vkCmdFillBuffer(cb, f->b1.buf, 0, SSBO_B, FILL_U);
      } else {
         const VkBufferCopy copy = { .srcOffset = 0, .dstOffset = 0,
                                     .size = SSBO_B };
         fw->vk.vkCmdCopyBuffer(cb, f->stage.buf, f->b1.buf, 1, &copy);
      }
      const VkBufferMemoryBarrier bb = {
         .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .buffer = f->b1.buf,
         .offset = 0,
         .size = SSBO_B,
      };
      fw->vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                  0, 0, NULL, 1, &bb, 0, NULL);
   }

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

   fw->vk.vkCmdBeginRendering(cb, &ri);
   fw->vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            gfx.pipeline);
   bind_sets(fw, cb, gfx.layout, f, c);
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
      uint32_t wrong[4] = { 0, 0, 0, 0 };
      uint32_t first = TEXELS;
      for (uint32_t i = 0; i < TEXELS; i++) {
         uint32_t want[4];
         expect_texel(c, i, want);
         const uint32_t *got = base + (size_t)i * 4u;
         for (uint32_t k = 0; k < 4; k++) {
            if (got[k] != want[k]) {
               wrong[k]++;
               if (first == TEXELS)
                  first = i;
            }
         }
      }

      /* One check line per component: which of the four broke is the
       * finding, and a single pass/fail would hide it. */
      for (uint32_t k = 0; k < 4; k++)
         t_check(t, wrong[k] == 0, "%s: %s right on %u/%u texels",
                 c->name, COMPONENT[k], TEXELS - wrong[k], TEXELS);

      if (first != TEXELS) {
         uint32_t want[4];
         expect_texel(c, first, want);
         const uint32_t *got = base + (size_t)first * 4u;
         t_note(t, "%s: first wrong texel %u (%u,%u): got "
                   "%08x %08x %u %08x, want %08x %08x %u %08x",
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

   struct fixture f = { 0 };
   vkfw_buffer dst = { 0 };

   if (!vkfw_buffer_create(&fw, READBACK_B, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &dst))
      goto out;
   if (!fixture_create(&fw, &f))
      goto out;

   for (uint32_t i = 0; i < NUM_CASES; i++) {
      /* Once the channel is gone every later case fails the same way,
       * and the first one is the answer. */
      if (vkfw_device_lost(&fw)) {
         t_note(t, "device lost; %u case(s) from \"%s\" on not attempted",
                (unsigned)(NUM_CASES - i), CASES[i].name);
         break;
      }
      run_case(&fw, &CASES[i], &f, &dst);
   }

out:
   fixture_destroy(&fw, &f);
   vkfw_buffer_destroy(&fw, &dst);
   vkfw_finish(&fw);
   return 0;
}
