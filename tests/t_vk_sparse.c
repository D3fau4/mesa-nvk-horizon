/*
 * Sparse residency through Vulkan — D12, one level up from t_sparse.
 *
 * WHAT t_sparse ALREADY ESTABLISHED, on hardware, 2026-08-24: a sparse
 * reservation is accepted; a write to a page nothing was bound to is
 * swallowed rather than faulting; memory binds into the middle of one
 * and the payload arrives; and after unbinding that block, a write to
 * the same address is swallowed again and does not reach the memory
 * that was unbound. Partial residency is expressible on this chip.
 *
 * That was horizon_gpu talking to itself. This is the same three
 * questions asked through vkQueueBindSparse, which is the only way to
 * find out whether the layers between — nvkmd_horizon_va's sparse
 * reservation and its partial unbind, and NVK's own sparse paths —
 * carry them.
 *
 * WHY IT IS ONE FLAG'S WORTH OF RISK. nvkmd_info::has_sparse gates
 * eight VkPhysicalDeviceFeatures at once (nvk_physical_device.c:395-402):
 * sparseBinding, sparseResidencyBuffer, sparseResidencyImage2D and 3D,
 * three multisample variants and sparseResidencyAliased. Turning it on
 * claims all of them. This test exercises the two that a buffer can
 * reach — sparseBinding and sparseResidencyBuffer — and says so rather
 * than implying the rest.
 *
 * THE ALIGNMENT QUESTION UNDERNEATH IT. horizon_gpu_vm_map refuses to
 * map an object in pages larger than the object's own alignment,
 * because MapBufferEx accepts that pairing and the GPU's writes to it
 * go nowhere (measured 2026-08-24). vkAllocateMemory takes no
 * alignment and this backend gives an ordinary allocation 4 KiB. So a
 * sparse bind works only if the reservation binds in small pages —
 * which is what t_sparse's arm D asks directly, and what this test
 * asks by doing it.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#include <switch.h>

#include "common/vkfw.h"

const char *const test_name = "t_vk_sparse";
/* No display: main() starts a console and reports through it. */
const bool test_uses_display = false;

#define BLOCKS      4u
#define PATTERN_A   UINT32_C(0x5A5AA5A5)
#define PATTERN_B   UINT32_C(0xC3C33C3C)
#define WAIT_NS     UINT64_C(2000000000)

/* Binds `mem` (or nothing, when mem is VK_NULL_HANDLE) over
 * [offset, offset+size) of `buf`, and waits for the queue to finish.
 *
 * A bind is a queue operation like a submit, so it needs the same
 * discipline: the fence is what says it happened, and reading the
 * result of a bind that has not completed would be reading the state
 * before it. */
static bool bind_range(vkfw *fw, VkBuffer buf, VkDeviceMemory mem,
                       VkDeviceSize resource_offset,
                       VkDeviceSize mem_offset, VkDeviceSize size,
                       const char *what)
{
   test_ctx *t = fw->t;

   const VkSparseMemoryBind bind = {
      .resourceOffset = resource_offset,
      .size = size,
      .memory = mem,
      .memoryOffset = mem_offset,
      .flags = 0,
   };
   const VkSparseBufferMemoryBindInfo buf_bind = {
      .buffer = buf,
      .bindCount = 1,
      .pBinds = &bind,
   };
   const VkBindSparseInfo info = {
      .sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO,
      .bufferBindCount = 1,
      .pBufferBinds = &buf_bind,
   };

   VkFence fence = VK_NULL_HANDLE;
   const VkFenceCreateInfo fci = { .sType =
      VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
   VkResult r = fw->vk.vkCreateFence(fw->dev, &fci, NULL, &fence);
   if (!t_check(t, r == VK_SUCCESS, "%s: vkCreateFence -> %s", what,
                vkfw_result_str(r)))
      return false;

   r = fw->vk.vkQueueBindSparse(fw->queue, 1, &info, fence);
   bool ok = t_check(t, r == VK_SUCCESS, "%s: vkQueueBindSparse -> %s",
                     what, vkfw_result_str(r));
   if (ok) {
      r = fw->vk.vkWaitForFences(fw->dev, 1, &fence, VK_TRUE, WAIT_NS);
      ok = t_check(t, r == VK_SUCCESS, "%s: the bind completed -> %s",
                   what, vkfw_result_str(r));
   }
   fw->vk.vkDestroyFence(fw->dev, fence, NULL);
   return ok;
}

/* Fills [offset, offset+size) of `buf` from the GPU and waits. Returns
 * false only when a Vulkan call refused; a fill that faults shows up as
 * a lost device, which the caller checks. */
static bool gpu_fill(vkfw *fw, VkBuffer buf, VkDeviceSize offset,
                     VkDeviceSize size, uint32_t pattern, const char *what)
{
   VkCommandBuffer cb;
   if (!vkfw_cmd_begin(fw, &cb))
      return false;
   fw->vk.vkCmdFillBuffer(cb, buf, offset, size, pattern);
   return vkfw_submit_and_wait(fw, cb, what);
}

int run_test(test_ctx *t)
{
   /* The features have to be asked for, not assumed: vkCreateDevice
    * fails if a feature is enabled that the physical device does not
    * offer, so this reads them first and says what it found. */
   vkfw probe;
   if (!vkfw_init(&probe, t, NULL))
      return 1;

   VkPhysicalDeviceFeatures avail;
   memset(&avail, 0, sizeof(avail));
   probe.vk.vkGetPhysicalDeviceFeatures(probe.pdev, &avail);
   const bool have_binding = avail.sparseBinding;
   const bool have_buffer = avail.sparseResidencyBuffer;
   t_note(t, "MEASURED: the driver offers sparseBinding=%s, "
          "sparseResidencyBuffer=%s, sparseResidencyImage2D=%s",
          have_binding ? "true" : "false",
          have_buffer ? "true" : "false",
          avail.sparseResidencyImage2D ? "true" : "false");
   vkfw_finish(&probe);

   if (!have_binding) {
      t_note(t, "MEASURED: sparseBinding is not offered, so nothing below "
             "can run. nvkmd_info::has_sparse is what decides this, and it "
             "is false — see docs/PENDING-VERIFICATION.md.");
      return 0;
   }

   VkPhysicalDeviceFeatures want;
   memset(&want, 0, sizeof(want));
   want.sparseBinding = VK_TRUE;
   want.sparseResidencyBuffer = have_buffer;
   const VkPhysicalDeviceFeatures2 features2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
      .features = want,
   };

   vkfw fw;
   if (!vkfw_init(&fw, t, &features2))
      return 1;

   int rv = 0;
   VkBuffer buf = VK_NULL_HANDLE;
   VkDeviceMemory block = VK_NULL_HANDLE;
   vkfw_buffer read_back;
   memset(&read_back, 0, sizeof(read_back));

   /* A buffer whose pages are the application's to place. SPARSE_
    * RESIDENCY on top of SPARSE_BINDING is what makes a partially-bound
    * buffer legal to use rather than only legal to build. */
   VkBufferCreateFlags flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT;
   if (have_buffer)
      flags |= VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT;

   VkMemoryRequirements req;
   memset(&req, 0, sizeof(req));

   /* Sized in blocks of the alignment the driver asks for, which is
    * only known after the buffer exists — so it is created once at a
    * guess, measured, and created again at the right size. */
   VkBufferCreateInfo bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .flags = flags,
      .size = 0x10000,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
               VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkResult r = fw.vk.vkCreateBuffer(fw.dev, &bci, NULL, &buf);
   if (!t_check(t, r == VK_SUCCESS,
                "MEASURED: a sparse buffer can be created -> %s",
                vkfw_result_str(r))) {
      rv = 1;
      goto out;
   }
   fw.vk.vkGetBufferMemoryRequirements(fw.dev, buf, &req);
   fw.vk.vkDestroyBuffer(fw.dev, buf, NULL);
   buf = VK_NULL_HANDLE;

   const VkDeviceSize blk = req.alignment;
   t_note(t, "MEASURED: the sparse block is 0x%" PRIx64 " bytes, and a "
          "0x10000-byte buffer wanted 0x%" PRIx64 " of memory",
          (uint64_t)blk, (uint64_t)req.size);
   if (!t_check(t, blk != 0 && (blk & (blk - 1)) == 0,
                "the sparse block is a power of two")) {
      rv = 1;
      goto out;
   }

   bci.size = blk * BLOCKS;
   r = fw.vk.vkCreateBuffer(fw.dev, &bci, NULL, &buf);
   if (!t_check(t, r == VK_SUCCESS, "a %u-block sparse buffer -> %s",
                BLOCKS, vkfw_result_str(r))) {
      rv = 1;
      goto out;
   }
   fw.vk.vkGetBufferMemoryRequirements(fw.dev, buf, &req);

   /* ONE block of memory for a FOUR block buffer. That is the whole
    * point of residency: the resource is bigger than its backing, and
    * the application says which part is real. */
   const uint32_t type =
      vkfw_memory_type(&fw, req.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
   if (!t_check(t, type != UINT32_MAX,
                "a host-visible memory type the sparse buffer accepts")) {
      rv = 1;
      goto out;
   }
   const VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = blk,
      .memoryTypeIndex = type,
   };
   r = fw.vk.vkAllocateMemory(fw.dev, &mai, NULL, &block);
   if (!t_check(t, r == VK_SUCCESS, "one block of memory -> %s",
                vkfw_result_str(r))) {
      rv = 1;
      goto out;
   }

   /* ---- A: a write to a block nothing was bound to ---------------- */
   if (!gpu_fill(&fw, buf, 0, blk, PATTERN_A,
                 "A: filling a block with nothing bound to it"))
      goto out;
   t_check(t, !vkfw_device_lost(&fw),
           "MEASURED A: writing to an unbound sparse block did not lose "
           "the device");
   if (vkfw_device_lost(&fw)) {
      t_note(t, "MEASURED A: it FAULTED, so residency cannot be built on "
             "this and the rest of the run cannot be believed");
      rv = 1;
      goto out;
   }

   /* ---- B: bind the middle block and write to it ------------------ */
   if (!bind_range(&fw, buf, block, blk, 0, blk, "B: binding block 1"))
      goto out;

   if (!gpu_fill(&fw, buf, blk, blk, PATTERN_B, "B: filling block 1"))
      goto out;
   if (!t_check(t, !vkfw_device_lost(&fw),
                "B: writing to the bound block did not lose the device")) {
      rv = 1;
      goto out;
   }

   /* Read it back through a copy, because the sparse buffer's own
    * memory is only partly there and mapping it is not a thing Vulkan
    * offers. */
   if (!vkfw_buffer_create(&fw, blk, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                           &read_back)) {
      rv = 1;
      goto out;
   }
   vkfw_buffer_poison(&fw, &read_back, ~PATTERN_B);

   {
      VkCommandBuffer cb;
      if (!vkfw_cmd_begin(&fw, &cb))
         goto out;
      const VkBufferCopy copy = { .srcOffset = blk, .dstOffset = 0,
                                  .size = blk };
      fw.vk.vkCmdCopyBuffer(cb, buf, read_back.buf, 1, &copy);
      if (!vkfw_submit_and_wait(&fw, cb, "B: copying the bound block out"))
         goto out;
   }
   vkfw_buffer_invalidate(&fw, &read_back);

   const uint32_t *rb = read_back.map;
   uint32_t wrong = 0;
   const uint32_t words = (uint32_t)(blk / 4);
   for (uint32_t i = 0; i < words; i++)
      if (rb[i] != PATTERN_B)
         wrong++;
   const bool b_landed =
      t_check(t, wrong == 0,
              "MEASURED B: the bound block holds what the GPU wrote "
              "(%" PRIu32 " of %" PRIu32 " words wrong, first 0x%08" PRIx32
              ")", wrong, words, rb[0]);

   /* ---- C: unbind it and write to the same range again ------------ */
   if (!bind_range(&fw, buf, VK_NULL_HANDLE, blk, 0, blk,
                   "C: unbinding block 1"))
      goto out;

   if (!gpu_fill(&fw, buf, blk, blk, PATTERN_A, "C: filling it unbound"))
      goto out;
   const bool c_survived = !vkfw_device_lost(&fw);
   t_check(t, c_survived,
           "MEASURED C: writing to the block after unbinding it did not "
           "lose the device");

   if (!b_landed) {
      t_note(t, "MEASURED C: not usable — B's control did not land, so "
             "this range was never shown to resolve before the unbind");
   } else {
      t_note(t, "MEASURED C — D12 THROUGH VULKAN: after vkQueueBindSparse "
             "unbound the block, a write to it %s", c_survived
             ? "was swallowed. Sparse residency works through the whole "
               "stack, not only in horizon_gpu."
             : "FAULTED. The layers above horizon_gpu do not carry what "
               "t_sparse measured, and has_sparse must go back to false.");
   }

   t_check(t, b_landed && c_survived,
           "MEASURED: sparseBinding and sparseResidencyBuffer both do what "
           "the driver says they do");

out:
   if (read_back.buf != VK_NULL_HANDLE)
      vkfw_buffer_destroy(&fw, &read_back);
   /* The buffer is destroyed before its memory, and every bind is
    * dropped by destroying the buffer — Vulkan says a sparse resource's
    * bindings die with it, so there is no unbind to make here. */
   if (buf != VK_NULL_HANDLE)
      fw.vk.vkDestroyBuffer(fw.dev, buf, NULL);
   if (block != VK_NULL_HANDLE)
      fw.vk.vkFreeMemory(fw.dev, block, NULL);
   vkfw_finish(&fw);
   return rv;
}
