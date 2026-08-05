/*
 * Phase 5 item 1 — transfers: buffer↔buffer and buffer↔image.
 *
 * WHAT THIS TEST IS FOR. Every later item reads its result back through
 * a copy, so a copy that is quietly wrong would make every later
 * measurement wrong in the same direction and none of them would say so.
 * This is the item that has nothing behind it: the CPU writes a pattern,
 * the GPU moves it, the CPU reads it back, and the two are compared word
 * for word.
 *
 * FIVE COPIES, NOT ONE, because they are different machinery:
 *
 *   A  buffer → buffer, whole
 *   B  buffer → buffer, a region in the middle — and the check that the
 *      words *outside* the region still hold their poison, which is the
 *      only way to catch a copy that ignores its own bounds
 *   C  vkCmdUpdateBuffer, where the data travels inside the command
 *      stream instead of in memory the GPU reads
 *   D  buffer → image → buffer with VK_IMAGE_TILING_OPTIMAL, so the
 *      block-linear address swizzle has to be an exact inverse of
 *      itself; a wrong tiling comes back as shuffled bytes, not as an
 *      error
 *   E  the same round trip with VK_IMAGE_TILING_LINEAR, which is the
 *      pitch path and a different set of PTE kinds
 *
 * D is the one that earns the test. It is the first thing in this
 * project that asks the GPU to interpret a surface layout, and the
 * round trip is self-checking: whatever the layout is, reading it back
 * the same way has to return what went in.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <string.h>

#include "common/vkfw.h"

const char *const test_name = "t_vk_transfer";
/* No display: main() starts a console and reports through it. */
const bool test_uses_display = false;

/* 64 x 64 RGBA8 is 16 KiB, and so is the buffer. The two sizes are the
 * same on purpose: with tightly packed rows the image round trip is an
 * identity over the whole buffer, so it can be compared against the
 * source word for word with no layout arithmetic in the test. */
#define IMG_W        64u
#define IMG_H        64u
#define BUF_WORDS    (IMG_W * IMG_H)          /* one RGBA8 texel per word */
#define BUF_BYTES    (BUF_WORDS * 4u)

/* Poison: what the destination holds before the GPU touches it. Not
 * zero, and not the pattern — a test that passes on either proves
 * nothing (Phase 4). */
#define POISON       0xdeadbeefu

/* The source pattern. A function of the index, so a copy that shifted
 * everything by one word, or repeated one word, fails on the value as
 * well as on the position. */
static uint32_t pattern_word(uint32_t i)
{
   return (i * 0x01000193u) ^ 0xa5c3f00du;
}

/* B's region: word-aligned, in the middle, and not a multiple of
 * anything convenient. */
#define REGION_OFF_W  257u
#define REGION_LEN_W  613u

/* C's inline update. vkCmdUpdateBuffer takes at most 65536 bytes and
 * wants a multiple of 4; 256 bytes is well inside both and large enough
 * that a partial write shows up. */
#define UPDATE_OFF_B  128u
#define UPDATE_LEN_B  256u

static void fill_source(uint32_t *w)
{
   for (uint32_t i = 0; i < BUF_WORDS; i++)
      w[i] = pattern_word(i);
}

/* One image memory barrier, which is the whole of what these copies need
 * for synchronisation: every submit here is waited on before the next is
 * recorded, so the only ordering that is not already implied is the
 * layout transition inside a single command buffer. */
static void image_barrier(vkfw *fw, VkCommandBuffer cb, VkImage img,
                          VkImageLayout from, VkImageLayout to,
                          VkAccessFlags src_access, VkAccessFlags dst_access)
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
   fw->vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                               0, NULL, 0, NULL, 1, &b);
}

/* The buffer↔image round trip, for one tiling. Returns true when the
 * bytes came back unchanged. */
static bool image_round_trip(vkfw *fw, VkImageTiling tiling,
                             const vkfw_buffer *src, vkfw_buffer *dst,
                             const char *what)
{
   test_ctx *t = fw->t;

   vkfw_image img;
   if (!vkfw_image_create(fw, VK_FORMAT_R8G8B8A8_UNORM,
                          (VkExtent3D){ IMG_W, IMG_H, 1 }, 1, 1,
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                          tiling, &img))
      return false;

   bool ok = false;
   if (!vkfw_buffer_poison(fw, dst, POISON))
      goto out;

   VkCommandBuffer cb;
   if (!vkfw_cmd_begin(fw, &cb))
      goto out;

   image_barrier(fw, cb, img.img, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 0, VK_ACCESS_TRANSFER_WRITE_BIT);

   /* bufferRowLength and bufferImageHeight of 0 mean "tightly packed to
    * the image extent", which is what makes the buffer's 16 KiB and the
    * image's 64x64 the same bytes in the same order. */
   const VkBufferImageCopy region = {
      .bufferOffset = 0,
      .bufferRowLength = 0,
      .bufferImageHeight = 0,
      .imageSubresource = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .layerCount = 1,
      },
      .imageOffset = { 0, 0, 0 },
      .imageExtent = { IMG_W, IMG_H, 1 },
   };
   fw->vk.vkCmdCopyBufferToImage(cb, src->buf, img.img,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 1, &region);

   image_barrier(fw, cb, img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);

   fw->vk.vkCmdCopyImageToBuffer(cb, img.img,
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 dst->buf, 1, &region);

   if (!vkfw_submit_and_wait(fw, cb, what))
      goto out;
   if (!vkfw_buffer_invalidate(fw, dst))
      goto out;

   ok = vkfw_expect_words_array(fw, dst->map, (const uint32_t *)src->map,
                                BUF_WORDS, what) == 0;

out:
   vkfw_image_destroy(fw, &img);
   (void)t;
   return ok;
}

/* F — the sweep that found the L2 bug, kept as the regression test for it.
 *
 * It started as "measure the copy engine's transfer granularity", after
 * case B asked for [1028, 3480) and [1024, 3488) changed. That framing
 * was wrong twice over. NVK programs the exact byte address and length
 * (nvk_cmd_copy.c:373-415) and the engine honours them; what moved the
 * edges was this project's own missing L2 invalidate before a submit.
 *
 * What made it findable was running nine regions instead of one and
 * scanning byte by byte for the first and last byte that actually
 * changed. Every spill turned out to be the distance back to a 32-byte
 * boundary, and to happen exactly when the previous submit had touched
 * that line — eleven data points, one model, and this GPU's L2 line at
 * 32 bytes. Fixed in horizon/submit; PASS 202/202 on hardware,
 * 2026-08-04.
 *
 * It stays because a partial write picking up cached data instead of
 * memory is invisible to any test that writes whole buffers, which is
 * every other case in this file.
 *
 * The check per probe is the one Vulkan requires and which nothing about
 * the hardware excuses: a copy writes its region and nothing else.
 *
 * RUN 2 MADE THIS SWEEP THE THING THAT NEEDED FIXING. Probe [1028, 3480)
 * is case B's region exactly, and in the same process the two disagreed:
 * B saw four bytes before and eight after carrying the source's pattern,
 * F saw a copy that landed exactly where it was asked. A copy engine
 * does not behave two ways for one request, so at least one of the two
 * was measuring something other than the copy — and the only shared
 * assumption underneath both is that the poison reached memory before
 * the GPU ran. Nothing checked it. vkfw_buffer_poison now does, so a
 * probe whose foundation did not hold says so instead of reporting an
 * overrun that never happened.
 *
 * The batch-1 conclusion drawn from case B alone — "the copy engine's
 * transfer granularity is 16 bytes" — is therefore withdrawn. It rested
 * on one unverified data point.
 */
struct copy_probe {
   uint32_t off_B;
   uint32_t size_B;
   const char *why;
};

static uint8_t poison_byte(uint32_t i)
{
   return (uint8_t)(POISON >> (8u * (i & 3u)));
}

static void copy_bounds_sweep(vkfw *fw, const vkfw_buffer *src,
                              vkfw_buffer *dst)
{
   test_ctx *t = fw->t;

   static const struct copy_probe probes[] = {
      {    0,  256, "aligned both ends — the control" },
      {    4,  256, "start 4, size 256" },
      {    8,  256, "start 8, size 256" },
      {   16,  256, "start 16, size 256" },
      {   32,  256, "start 32, size 256" },
      {    0,    4, "one word at zero" },
      {    0,   12, "three words at zero" },
      {   64,  260, "aligned start, unaligned size" },
      { 1028, 2452, "exactly case B, so the two must agree" },
   };

   uint32_t unaligned_spill = 0;

   for (uint32_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
      const struct copy_probe *pr = &probes[i];

      if (vkfw_device_lost(fw))
         return;
      if (!vkfw_buffer_poison(fw, dst, POISON))
         return;

      VkCommandBuffer cb;
      if (!vkfw_cmd_begin(fw, &cb))
         return;
      const VkBufferCopy copy = {
         .srcOffset = pr->off_B,
         .dstOffset = pr->off_B,
         .size = pr->size_B,
      };
      fw->vk.vkCmdCopyBuffer(cb, src->buf, dst->buf, 1, &copy);
      if (!vkfw_submit_and_wait(fw, cb, "F copy probe"))
         return;
      if (!vkfw_buffer_invalidate(fw, dst))
         return;

      /* Byte-level, because the granularity being measured may be finer
       * than a word and a word-level scan would round the answer to the
       * very thing it is trying to measure. */
      const uint8_t *got = (const uint8_t *)dst->map;
      int64_t first = -1, last = -1;
      for (uint32_t b = 0; b < BUF_BYTES; b++) {
         if (got[b] != poison_byte(b)) {
            if (first < 0)
               first = b;
            last = b;
         }
      }

      if (first < 0) {
         t_check(t, false, "F [%u, %u): %s — nothing changed at all",
                 pr->off_B, pr->off_B + pr->size_B, pr->why);
         continue;
      }

      const uint32_t want_end = pr->off_B + pr->size_B;
      const bool exact = (uint64_t)first == pr->off_B &&
                         (uint64_t)last + 1 == want_end;
      t_check(t, exact, "F [%u, %u): %s", pr->off_B, want_end, pr->why);
      if (!exact) {
         t_note(t, "F [%u, %u): bytes [%lld, %lld) changed — %lld before, "
                "%lld after", pr->off_B, want_end, (long long)first,
                (long long)last + 1, (long long)pr->off_B - first,
                (long long)last + 1 - want_end);
         unaligned_spill++;
      }
   }

   /* The sentence that used to stand here called a spill "the copy
    * engine's transfer granularity". It was wrong — the engine programs
    * and honours the exact byte address and length, and the spill was
    * this project's own missing L2 invalidate before a submit
    * (horizon/submit). Left as a live summary rather than a conclusion:
    * it says what was measured and what a non-zero count would mean, and
    * nothing about why. */
   t_note(t, "F: %u of %u probes wrote outside their region. A non-zero "
             "count means a partial write to an L2 line picked up data "
             "the GPU had cached rather than what is in memory — the "
             "spill is always the distance to a 32-byte boundary.",
          unaligned_spill,
          (unsigned)(sizeof(probes) / sizeof(probes[0])));
}

int run_test(test_ctx *t)
{
   vkfw fw;
   if (!vkfw_init(&fw, t, NULL))
      return 1;

   vkfw_buffer src = { 0 }, dst = { 0 };
   const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;

   if (!vkfw_buffer_create(&fw, BUF_BYTES,
                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           host, &src))
      goto out;
   if (!vkfw_buffer_create(&fw, BUF_BYTES,
                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           host, &dst))
      goto out;

   t_note(t, "buffers: 0x%llx requested, 0x%llx allocated, memory is %s",
          (unsigned long long)BUF_BYTES,
          (unsigned long long)src.alloc_B,
          src.coherent ? "coherent" : "cached (flush/invalidate in use)");

   fill_source((uint32_t *)src.map);
   if (!vkfw_buffer_flush(&fw, &src))
      goto out;

   /* --- A: whole buffer → buffer ------------------------------------ */
   {
      if (!vkfw_buffer_poison(&fw, &dst, POISON))
         goto out;

      VkCommandBuffer cb;
      if (!vkfw_cmd_begin(&fw, &cb))
         goto out;
      const VkBufferCopy copy = { .srcOffset = 0, .dstOffset = 0,
                                  .size = BUF_BYTES };
      fw.vk.vkCmdCopyBuffer(cb, src.buf, dst.buf, 1, &copy);
      if (!vkfw_submit_and_wait(&fw, cb, "A copy buffer→buffer"))
         goto out;
      if (!vkfw_buffer_invalidate(&fw, &dst))
         goto out;

      vkfw_expect_words_array(&fw, dst.map, (const uint32_t *)src.map,
                              BUF_WORDS, "A buffer→buffer");
   }

   /* --- B: a region, and the words around it ------------------------ */
   {
      if (!vkfw_buffer_poison(&fw, &dst, POISON))
         goto out;

      VkCommandBuffer cb;
      if (!vkfw_cmd_begin(&fw, &cb))
         goto out;
      const VkBufferCopy copy = {
         .srcOffset = REGION_OFF_W * 4u,
         .dstOffset = REGION_OFF_W * 4u,
         .size = REGION_LEN_W * 4u,
      };
      fw.vk.vkCmdCopyBuffer(cb, src.buf, dst.buf, 1, &copy);
      if (!vkfw_submit_and_wait(&fw, cb, "B copy region"))
         goto out;
      if (!vkfw_buffer_invalidate(&fw, &dst))
         goto out;

      const uint32_t *got = (const uint32_t *)dst.map;
      const uint32_t *want = (const uint32_t *)src.map;
      vkfw_expect_words_array(&fw, got + REGION_OFF_W, want + REGION_OFF_W,
                              REGION_LEN_W, "B region copied");
      /* The half that a copy with the wrong size would fail and a copy
       * with the right size cannot: everything outside the region still
       * holds the poison. */
      vkfw_expect_words(&fw, got, POISON, REGION_OFF_W,
                        "B words before the region untouched");
      vkfw_expect_words(&fw, got + REGION_OFF_W + REGION_LEN_W, POISON,
                        BUF_WORDS - REGION_OFF_W - REGION_LEN_W,
                        "B words after the region untouched");
   }

   /* --- C: data carried in the command stream ------------------------ */
   {
      if (!vkfw_buffer_poison(&fw, &dst, POISON))
         goto out;

      uint32_t inline_data[UPDATE_LEN_B / 4];
      for (uint32_t i = 0; i < UPDATE_LEN_B / 4; i++)
         inline_data[i] = pattern_word(0x1000u + i);

      VkCommandBuffer cb;
      if (!vkfw_cmd_begin(&fw, &cb))
         goto out;
      fw.vk.vkCmdUpdateBuffer(cb, dst.buf, UPDATE_OFF_B, UPDATE_LEN_B,
                              inline_data);
      if (!vkfw_submit_and_wait(&fw, cb, "C vkCmdUpdateBuffer"))
         goto out;
      if (!vkfw_buffer_invalidate(&fw, &dst))
         goto out;

      const uint32_t *got = (const uint32_t *)dst.map;
      vkfw_expect_words_array(&fw, got + UPDATE_OFF_B / 4, inline_data,
                              UPDATE_LEN_B / 4, "C inline update landed");
      vkfw_expect_words(&fw, got, POISON, UPDATE_OFF_B / 4,
                        "C words before the update untouched");
      /* And after it. The prefix alone would let an implementation
       * that writes past UPDATE_OFF_B + UPDATE_LEN_B pass, which is
       * precisely the bounds regression case B checks on the other
       * side — and precisely the shape of the L2 defect this suite
       * already found once. Found in review of PR #7. */
      vkfw_expect_words(&fw, got + (UPDATE_OFF_B + UPDATE_LEN_B) / 4,
                        POISON,
                        (BUF_BYTES - UPDATE_OFF_B - UPDATE_LEN_B) / 4u,
                        "C words after the update untouched");
   }

   /* --- D and E: through an image, both tilings ---------------------- */
   image_round_trip(&fw, VK_IMAGE_TILING_OPTIMAL, &src, &dst,
                    "D buffer→image→buffer, optimal");
   image_round_trip(&fw, VK_IMAGE_TILING_LINEAR, &src, &dst,
                    "E buffer→image→buffer, linear");

   /* --- F: how far outside its region does a copy actually write? ---- */
   copy_bounds_sweep(&fw, &src, &dst);

out:
   vkfw_buffer_destroy(&fw, &dst);
   vkfw_buffer_destroy(&fw, &src);
   vkfw_finish(&fw);
   return 0;
}
