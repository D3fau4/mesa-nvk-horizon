/*
 * What vkAllocateMemory costs, and whether the memory it hands back is
 * usable when nobody zeroed it.
 *
 * WHY THIS CASE EXISTS. On this platform every nvkmd allocation used to
 * be memset to zero before it was returned, because
 * horizon_gpu_mem_create does that and there was no way to say it was
 * not wanted. `gpu_memory/alloc` measured what the memset costs on a
 * console on 2026-09-07 — 21 us of a 64 KiB allocation, 182 us of a
 * 1 MiB one, 1403 us of an 8 MiB one — and that measurement is what
 * NVKMD_MEM_NO_ZERO_INIT was added for.
 *
 * TWO CONSUMERS TOOK THE FLAG, and this case is the validation aimed at
 * exactly those two and at nothing else:
 *
 *   vkAllocateMemory      Vulkan promises the application nothing about
 *                         the contents, and NVK already fills the one
 *                         case where zeroes ARE promised
 *                         (VK_MEMORY_ALLOCATE_ZERO_INITIALIZE_BIT_EXT,
 *                         NVK_DEBUG=zero_memory) itself and
 *                         synchronously. Sections A and B here write
 *                         before they read, from the CPU and from the
 *                         GPU, which is the promise the flag makes.
 *   command-buffer chunks NVK points the GPU at [addr, addr + used),
 *                         and nv_push writes every dword up to `used`.
 *                         Section C records far more commands than one
 *                         64 KiB chunk holds, so the recording runs off
 *                         the end of several of them, and then checks
 *                         every byte the GPU was asked to write.
 *
 * WHAT IT DOES NOT CLAIM. A case that passes does not prove no consumer
 * anywhere reads memory it never wrote; nothing short of running every
 * application would. It proves that the two consumers that took the
 * flag work when they are used the way NVK uses them, and it puts a
 * number on what the flag saves.
 *
 * THE A/B IS INSIDE ONE PROCESS, at one clock, against one cache.
 * NVK_HORIZON_ZERO_ALL_MEM=1 makes the Horizon backend ignore the flag
 * and fill anyway, which is the old shape exactly; the backend reads
 * that variable per allocation for this reason. Homebrew is launched
 * with no environment, so setenv on ourselves is the only way to do
 * this — the same shape as vk_present/drawn_frame's acquire A/B and
 * vk_core/submit_batching's.
 *
 * WHAT IS MEASURED AND WHAT IS NOT. This reports the cost of ONE
 * allocation, mean and max, at three sizes. It does NOT report a frame
 * time or a stutter: nothing here presents, and whether an application
 * stutters depends on whether it allocates inside a frame, which is the
 * application's behaviour and not the driver's. The per-allocation max
 * is the size of the spike such an application would take, and that is
 * the honest way to connect the two.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include "common/testfw.h"
#include "common/vkfw.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

/* The three sizes gpu_memory/alloc measured the fill at, so the numbers
 * here can be put beside that file's without converting anything. */
#define DM_SMALL_B   (64u * 1024u)
#define DM_MEDIUM_B  (1024u * 1024u)
#define DM_LARGE_B   (8u * 1024u * 1024u)

/* How many allocations go into one mean. Enough that a single outlier
 * does not carry it, few enough that eight megabytes twenty times over
 * is still a second of wall clock. */
#define DM_REPS      20u

/* Repetitions of the A/B pair, alternating which half goes first. */
#define DM_AB_PAIRS  3u

/* Section C: how many commands to record. One nv_push dword is four
 * bytes and NVK's chunk is 64 KiB, so a few thousand vkCmdFillBuffer
 * calls is several chunks however the encoding lands — and the check is
 * on the result, not on the count. */
#define DM_FILLS     3000u
#define DM_FILL_B    64u

typedef struct dm_timing {
   uint32_t allocations;
   uint64_t total_ns;
   uint64_t max_ns;
} dm_timing;

static uint64_t dm_mean_ns(const dm_timing *d)
{
   return d->allocations ? d->total_ns / d->allocations : 0;
}

/* Allocates and frees `DM_REPS` blocks of `size_B` one at a time,
 * timing each vkAllocateMemory on its own. One at a time rather than
 * all at once deliberately: an application asking for its next buffer
 * is what this is about, and holding twenty eight-megabyte blocks would
 * measure the heap filling up instead. */
static bool dm_time_allocs(vkfw *fw, uint32_t type_index, VkDeviceSize size_B,
                           dm_timing *out)
{
   test_ctx *t = fw->t;

   memset(out, 0, sizeof(*out));

   for (uint32_t i = 0; i < DM_REPS; i++) {
      const VkMemoryAllocateInfo ai = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = size_B,
         .memoryTypeIndex = type_index,
      };

      VkDeviceMemory mem = VK_NULL_HANDLE;
      const u64 start = armGetSystemTick();
      const VkResult r = fw->vk.vkAllocateMemory(fw->dev, &ai, NULL, &mem);
      const uint64_t ns = armTicksToNs(armGetSystemTick() - start);

      if (r != VK_SUCCESS) {
         t_check(t, false,
                 "vkAllocateMemory(%" PRIu64 " KiB, type %" PRIu32 ") -> %s "
                 "on repetition %" PRIu32,
                 (uint64_t)(size_B / 1024u), type_index, vkfw_result_str(r), i);
         return false;
      }

      out->allocations++;
      out->total_ns += ns;
      if (ns > out->max_ns)
         out->max_ns = ns;

      fw->vk.vkFreeMemory(fw->dev, mem, NULL);
   }

   return true;
}

/* One line of the A/B table. */
static void dm_note(test_ctx *t, const char *what, VkDeviceSize size_B,
                    const dm_timing *d)
{
   t_note(t, "%s at %" PRIu64 " KiB: mean %" PRIu64 " us, max %" PRIu64
             " us over %" PRIu32 " allocations",
          what, (uint64_t)(size_B / 1024u), dm_mean_ns(d) / 1000u,
          d->max_ns / 1000u, d->allocations);
}

TEST_CASE_DECL(vk_core, device_memory)
{
   vkfw fw;
   int rv = 0;

   if (!vkfw_init(&fw, t, NULL))
      return 1;

   /* ---------------------------------------------------------------- */
   /* A — host-visible memory the application writes before it reads.  */
   /* ---------------------------------------------------------------- */

   /* THE CLAIM BEING CHECKED is not "the memory is zero" — it is not,
    * and Vulkan never said it would be. It is that a mapping written
    * and flushed reads back exactly what was written, with no byte of
    * the allocation left holding whatever the heap had before. Every
    * byte of the size asked for is written and every one is compared.
    * The tail beyond it is not: the allocator rounds up to a page and
    * that padding is not something the application can see or has a
    * claim on. */
   {
      const uint32_t host_type =
         vkfw_memory_type(&fw, ~0u, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

      if (t_check(t, host_type != UINT32_MAX,
                  "A: a host-visible coherent memory type exists")) {
         const VkMemoryAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = DM_MEDIUM_B,
            .memoryTypeIndex = host_type,
         };
         VkDeviceMemory mem = VK_NULL_HANDLE;
         VkResult r = fw.vk.vkAllocateMemory(fw.dev, &ai, NULL, &mem);

         if (t_check(t, r == VK_SUCCESS,
                     "A: vkAllocateMemory(1 MiB, host visible) -> %s",
                     vkfw_result_str(r))) {
            void *map = NULL;
            r = fw.vk.vkMapMemory(fw.dev, mem, 0, VK_WHOLE_SIZE, 0, &map);
            if (t_check(t, r == VK_SUCCESS && map != NULL,
                        "A: vkMapMemory -> %s", vkfw_result_str(r))) {
               uint32_t *w = map;
               const uint32_t words = DM_MEDIUM_B / 4u;

               for (uint32_t i = 0; i < words; i++)
                  w[i] = i * 2654435761u;   /* Knuth's multiplicative hash */

               fw.vk.vkUnmapMemory(fw.dev, mem);

               /* Mapped again rather than read through the pointer that
                * wrote it: a second mapping is what an application that
                * comes back to its memory does, and it is the one that
                * would notice a driver having handed out a different
                * page. */
               map = NULL;
               r = fw.vk.vkMapMemory(fw.dev, mem, 0, VK_WHOLE_SIZE, 0, &map);
               if (t_check(t, r == VK_SUCCESS && map != NULL,
                           "A: the memory maps a second time -> %s",
                           vkfw_result_str(r))) {
                  const uint32_t *rd = map;
                  uint32_t wrong = 0;
                  for (uint32_t i = 0; i < words; i++) {
                     if (rd[i] != i * 2654435761u)
                        wrong++;
                  }
                  t_check(t, wrong == 0,
                          "MEASURED A: every one of %" PRIu32 " words written "
                          "through a mapping read back unchanged (%" PRIu32
                          " did not) — an allocation nobody zeroed is still "
                          "the application's to write and read",
                          words, wrong);
                  fw.vk.vkUnmapMemory(fw.dev, mem);
               }
            }
            fw.vk.vkFreeMemory(fw.dev, mem, NULL);
         }
      }
   }

   /* ---------------------------------------------------------------- */
   /* B — device-local memory the GPU writes before anybody reads.     */
   /* ---------------------------------------------------------------- */

   /* The other half of the promise: a buffer bound to memory the
    * allocator did not fill, written entirely by the GPU, copied back
    * and compared. The readback buffer is poisoned first, so a match
    * cannot have come from a destination that already held the answer.
    */
   {
      vkfw_buffer dst;
      vkfw_buffer host;
      const VkDeviceSize size_B = DM_MEDIUM_B;

      const bool have_dst =
         vkfw_buffer_create(&fw, size_B,
                            VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &dst);
      const bool have_host =
         have_dst &&
         vkfw_buffer_create(&fw, size_B, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &host);

      if (t_check(t, have_dst && have_host,
                  "B: a device-local buffer and a host-visible readback "
                  "buffer of 1 MiB each")) {
         vkfw_buffer_poison(&fw, &host, 0xDEADBEEFu);

         VkCommandBuffer cb = VK_NULL_HANDLE;
         if (vkfw_cmd_begin(&fw, &cb)) {
            fw.vk.vkCmdFillBuffer(cb, dst.buf, 0, size_B, 0xA5A5A5A5u);

            const VkMemoryBarrier mb = {
               .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
               .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
               .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            };
            fw.vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                       1, &mb, 0, NULL, 0, NULL);

            const VkBufferCopy copy = { 0, 0, size_B };
            fw.vk.vkCmdCopyBuffer(cb, dst.buf, host.buf, 1, &copy);

            if (vkfw_submit_and_wait(&fw, cb, "B: fill and copy back") &&
                vkfw_buffer_invalidate(&fw, &host)) {
               const uint32_t wrong =
                  vkfw_expect_words(&fw, host.map, 0xA5A5A5A5u,
                                    (uint32_t)(size_B / 4u), "B");
               t_check(t, wrong == 0,
                       "MEASURED B: the GPU wrote every one of %" PRIu32
                       " words of an allocation nobody zeroed, and every one "
                       "came back (%" PRIu32 " did not)",
                       (uint32_t)(size_B / 4u), wrong);
            }
         }
      }

      if (have_host)
         vkfw_buffer_destroy(&fw, &host);
      if (have_dst)
         vkfw_buffer_destroy(&fw, &dst);
   }

   /* ---------------------------------------------------------------- */
   /* C — a command buffer that runs off the end of several chunks.    */
   /* ---------------------------------------------------------------- */

   /* THE SECOND CONSUMER, and the one whose failure would not look like
    * a memory bug. A command-buffer chunk is 64 KiB and the GPU is
    * pointed at [addr, addr + used); if anything ever read past `used`
    * — a stale dword left in the chunk from its previous tenant — the
    * symptom would be a command nobody recorded. Three thousand fills
    * is several chunks' worth of pushbuffer whatever the encoding, and
    * every one of them is checked, so a chunk boundary that lost or
    * invented a command shows up as a region with the wrong value.
    *
    * Each fill writes its own index, so a region holding another
    * region's value is as visible as one holding stale memory. */
   {
      const VkDeviceSize size_B = (VkDeviceSize)DM_FILLS * DM_FILL_B;
      vkfw_buffer dst;
      vkfw_buffer host;

      const bool have_dst =
         vkfw_buffer_create(&fw, size_B,
                            VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &dst);
      const bool have_host =
         have_dst &&
         vkfw_buffer_create(&fw, size_B, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &host);

      if (t_check(t, have_dst && have_host,
                  "C: buffers for %" PRIu32 " fills of %" PRIu32 " bytes",
                  (uint32_t)DM_FILLS, (uint32_t)DM_FILL_B)) {
         vkfw_buffer_poison(&fw, &host, 0xDEADBEEFu);

         VkCommandBuffer cb = VK_NULL_HANDLE;
         const u64 rec_start = armGetSystemTick();
         if (vkfw_cmd_begin(&fw, &cb)) {
            for (uint32_t i = 0; i < DM_FILLS; i++) {
               fw.vk.vkCmdFillBuffer(cb, dst.buf,
                                     (VkDeviceSize)i * DM_FILL_B, DM_FILL_B,
                                     0x10000u + i);
            }

            const VkMemoryBarrier mb = {
               .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
               .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
               .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            };
            fw.vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                       1, &mb, 0, NULL, 0, NULL);

            const VkBufferCopy copy = { 0, 0, size_B };
            fw.vk.vkCmdCopyBuffer(cb, dst.buf, host.buf, 1, &copy);

            const uint64_t rec_ns =
               armTicksToNs(armGetSystemTick() - rec_start);

            if (vkfw_submit_and_wait(&fw, cb,
                                     "C: many fills across chunks") &&
                vkfw_buffer_invalidate(&fw, &host)) {
               const uint32_t *w = host.map;
               const uint32_t per_fill = DM_FILL_B / 4u;
               uint32_t wrong_regions = 0;
               uint32_t first_wrong = UINT32_MAX;

               for (uint32_t i = 0; i < DM_FILLS; i++) {
                  bool ok = true;
                  for (uint32_t j = 0; j < per_fill; j++) {
                     if (w[i * per_fill + j] != 0x10000u + i) {
                        ok = false;
                        break;
                     }
                  }
                  if (!ok) {
                     wrong_regions++;
                     if (first_wrong == UINT32_MAX)
                        first_wrong = i;
                  }
               }

               t_check(t, wrong_regions == 0,
                       "MEASURED C: all %" PRIu32 " commands recorded across "
                       "several command-buffer chunks reached the GPU and "
                       "wrote what they were asked to (%" PRIu32 " region(s) "
                       "wrong, first at %" PRIu32 ")",
                       (uint32_t)DM_FILLS, wrong_regions,
                       first_wrong == UINT32_MAX ? 0u : first_wrong);

               t_note(t, "C: recording %" PRIu32 " commands took %" PRIu64
                         " us, which includes every chunk the pool had to "
                         "allocate", (uint32_t)DM_FILLS, rec_ns / 1000u);
            }
         }
      }

      if (have_host)
         vkfw_buffer_destroy(&fw, &host);
      if (have_dst)
         vkfw_buffer_destroy(&fw, &dst);
   }

   /* ---------------------------------------------------------------- */
   /* D — what the flag saves, both shapes in this process.            */
   /* ---------------------------------------------------------------- */

   /* NVK_HORIZON_ZERO_ALL_MEM=1 makes the backend fill anyway, which is
    * this driver before the flag existed. Three pairs, alternating
    * which half runs first, so a console that drifts drifts into both.
    *
    * DEVICE-LOCAL MEMORY, because that is what an application's buffers
    * and images are made of and it is the path with no CPU mapping to
    * confuse the number. The mean is what an allocation costs; the max
    * is the size of the pause an application takes if it allocates
    * inside a frame — which this case does not do and does not claim
    * to have measured. */
   {
      const uint32_t local_type =
         vkfw_memory_type(&fw, ~0u, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

      if (t_check(t, local_type != UINT32_MAX,
                  "D: a device-local memory type exists")) {
         static const VkDeviceSize sizes[] = {
            DM_SMALL_B, DM_MEDIUM_B, DM_LARGE_B,
         };
         const uint32_t size_count = (uint32_t)(sizeof(sizes) /
                                                sizeof(sizes[0]));

         for (uint32_t s = 0; s < size_count; s++) {
            dm_timing skip = { 0 }, fill = { 0 };
            bool ok = true;

            /* A warm-up at this size, discarded: the first allocation
             * of a size pays for whatever the heap has to do to make
             * room, and that is not what is being compared. */
            dm_timing warm;
            setenv("NVK_HORIZON_ZERO_ALL_MEM", "0", 1);
            ok = dm_time_allocs(&fw, local_type, sizes[s], &warm);

            for (uint32_t p = 0; ok && p < DM_AB_PAIRS; p++) {
               const bool fill_first = (p & 1u) != 0;

               for (uint32_t half = 0; ok && half < 2u; half++) {
                  const bool zero_all = (half == 0) == fill_first;
                  dm_timing one;

                  setenv("NVK_HORIZON_ZERO_ALL_MEM", zero_all ? "1" : "0", 1);
                  ok = dm_time_allocs(&fw, local_type, sizes[s], &one);
                  if (!ok)
                     break;

                  dm_timing *acc = zero_all ? &fill : &skip;
                  acc->allocations += one.allocations;
                  acc->total_ns += one.total_ns;
                  if (one.max_ns > acc->max_ns)
                     acc->max_ns = one.max_ns;
               }
            }

            setenv("NVK_HORIZON_ZERO_ALL_MEM", "0", 1);

            if (!ok)
               break;

            dm_note(t, "D: allocator does not fill", sizes[s], &skip);
            dm_note(t, "D: allocator fills with zero", sizes[s], &fill);

            const uint64_t a = dm_mean_ns(&skip);
            const uint64_t b = dm_mean_ns(&fill);

            /* NO CHECK THAT IT IS FASTER, and 2026-09-07 is why.
             *
             * The first version of this asserted that not filling was
             * the cheaper of the two at a megabyte and above. A console
             * answered 360 us against 357 us at 1 MiB and 1475 against
             * 1490 at 8 MiB — a difference of nothing, in both
             * directions, on a fill that gpu_memory/alloc measures at
             * 1403 us for the same 8 MiB. Whatever vkAllocateMemory
             * spends on this path, the memset is not what dominates it.
             *
             * A threshold here would have been this test inventing a
             * number the hardware does not support, so the timings are
             * notes and the verdict says which way they came out. What
             * IS checked is that the mechanism was live at all, and
             * that is the log scan after this loop rather than a
             * difference in the times — because a difference of zero
             * cannot tell "the flag arrived and saved nothing" from
             * "the flag never arrived". */
            const bool cheaper = b > a;
            t_note(t, "D VERDICT at %" PRIu64 " KiB: not filling is %"
                      PRIu64 " us %s per allocation (%" PRIu64 "%%), and "
                      "the worst single allocation goes from %" PRIu64
                      " us to %" PRIu64 " us — which is the size of the "
                      "pause an application takes if it allocates this "
                      "inside a frame, not a frame time this case measured",
                   (uint64_t)(sizes[s] / 1024u),
                   (cheaper ? b - a : a - b) / 1000u,
                   cheaper ? "cheaper" : "DEARER",
                   b != 0 ? ((cheaper ? b - a : a - b) * 100u) / b : 0,
                   fill.max_ns / 1000u, skip.max_ns / 1000u);
         }
      }

      unsetenv("NVK_HORIZON_ZERO_ALL_MEM");

      /* THE ONE THING THE TIMES CANNOT SAY. nvkmd_horizon logs once,
       * the first time an allocation asks not to be filled and is not,
       * and that line is the only evidence that NVKMD_MEM_NO_ZERO_INIT
       * travelled from nvk_device_memory.c through nvkmd to the
       * backend. Without it a measured difference of zero is
       * indistinguishable from a flag that was dropped on the way. */
      bool skipped = false;
      if (t_log_scan(t, "asked not to be zero filled", &skipped)) {
         t_check(t, skipped,
                 "MEASURED D: the allocator reported skipping a zero fill, "
                 "so NVKMD_MEM_NO_ZERO_INIT reached it — the timings above "
                 "are two shapes and not one measured twice");
      } else {
         t_check(t, false,
                 "D: the log could not be read back, so whether the flag "
                 "reached the allocator was NOT checked");
      }
   }

   vkfw_finish(&fw);
   return rv;
}
