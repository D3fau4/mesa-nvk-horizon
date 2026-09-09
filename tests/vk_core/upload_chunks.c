/*
 * The regression test for `0073`: an upload chunk the CPU wrote into
 * and nobody cleaned out of its cache.
 *
 * THE DEFECT, in NVK's own words. `nvk_cmd_buffer_upload_alloc` has two
 * paths. The fast one carves a range out of `cmd->upload_mem` and
 * advances `used_B`, which is exactly what `flush_mem_list` cleans at
 * vkEndCommandBuffer:
 *
 *     if (mem->used_B > 0)
 *        nvkmd_mem_sync_map_to_gpu(mem->mem, 0, mem->used_B);
 *
 * The other path takes a brand-new 64 KiB chunk, hands the caller
 * offset 0 of it and — before `0073` — never touched `used_B`. A chunk
 * that then failed the "adopt it as cmd->upload_mem" test kept
 * `used_B == 0` for its whole life, so the clean was skipped: the bytes
 * the CPU memcpy'd stayed in the CPU's cache and the GPU read whatever
 * memory held. On this SoC the GPU is not coherent with the CPU's
 * caches for these mappings, which is why the maintenance exists at all
 * (`nvkmd_mem_sync_to_gpu` -> `util_flush_range`).
 *
 * HOW THIS CASE REACHES THAT PATH, deterministically rather than by
 * luck. Two facts do it:
 *
 *   - a chunk is `NVK_CMD_MEM_SIZE` = 64 KiB (nvk_cmd_pool.h), and
 *     `vkCmdUpdateBuffer` uploads its data with `alignment = 64`
 *     (nvk_cmd_copy.c). So an update of the largest size Vulkan allows
 *     — 65536 bytes, VUID-vkCmdUpdateBuffer-dataSize-00037 — cannot
 *     fit in a chunk that has anything in it at all, and always takes
 *     the new-chunk path;
 *   - the new chunk is adopted only `if (cmd->upload_mem == NULL ||
 *     size < cmd->upload_offset)`, and a 65536-byte upload is never
 *     smaller than an offset that is at most 65536. So it is never
 *     adopted either.
 *
 * A small update followed by a full-size one therefore leaves one chunk
 * holding 64 KiB the CPU wrote and nothing said was written. That is
 * the defect, and section A is that sequence.
 *
 * WHAT MAKES A FAILURE VISIBLE, which is the part that needs care. The
 * command pool's memory is still zero filled — `nvk_cmd_pool.c`
 * deliberately does NOT pass `NVKMD_MEM_NO_ZERO_INIT`, and the comment
 * there says why — and a chunk that comes back from the pool's free
 * list holds whatever the last command buffer put in it. Either could
 * hide a stale read by accident, so:
 *
 *   - no expected word is ever zero, so a zero-filled chunk arriving
 *     at the GPU cannot look like a pass;
 *   - every repetition uses a different pattern, so a recycled chunk
 *     still holding the previous repetition's bytes cannot either;
 *   - the destination is poisoned before every submit, so "the GPU
 *     wrote nothing" and "the GPU wrote the wrong thing" are different
 *     answers.
 *
 * WHAT IT CANNOT DO. It cannot see NVK's chunks, so it cannot assert
 * that the path was taken — it can only construct the sequence that
 * takes it and check the result. If `NVK_CMD_MEM_SIZE` ever grows past
 * 65536, the largest update Vulkan permits would fit beside a small one
 * and this case would quietly stop reaching the path it is named after.
 * Recorded here because the check for it does not exist.
 *
 * The failure is also a race with the CPU's own cache: a dirty line
 * that happens to be evicted before the GPU reads is a line the GPU
 * reads correctly. That is why there are repetitions rather than one
 * trip, and why the checks count wrong words rather than assert on the
 * first one.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <inttypes.h>
#include <string.h>

#include "common/vkfw.h"

/* The largest dataSize vkCmdUpdateBuffer takes, which is also NVK's
 * chunk size. Both halves of the coincidence matter — see the header. */
#define UC_BIG_B     65536u
/* Small enough to leave the chunk almost empty, aligned enough for the
 * 64-byte alignment the upload asks for. */
#define UC_SMALL_B   256u

/* Where each update lands in the destination. The big one starts at
 * 64 KiB so the two never overlap and a copy with the wrong length is
 * visible as poison left behind rather than as a second pattern. */
#define UC_SMALL_OFF 0u
#define UC_BIG_OFF   UC_BIG_B
#define UC_TAIL_OFF  (UC_BIG_OFF + UC_BIG_B)
#define UC_TAIL_B    UC_SMALL_B
#define UC_DST_B     (UC_TAIL_OFF + UC_TAIL_B)

/* Repetitions. Sixteen because the failure depends on a dirty cache
 * line surviving until the GPU reads, which is not certain for any one
 * of them; sixteen fresh command buffers, each with its own pattern,
 * make "no line ever survived" the only way to miss it. */
#define UC_REPS      16u

#define UC_POISON    0xdeadbeefu

/* The pattern. A function of the repetition as well as the index, and
 * never zero: a chunk that arrives zero filled or still holding an
 * earlier repetition must not be able to match. */
static uint32_t uc_word(uint32_t rep, uint32_t i)
{
   const uint32_t w = ((i * 0x01000193u) ^ (0xa5c3f00du + rep * 0x9e3779b9u));
   return w | 1u;
}

/* The data for one repetition, filled in place. Static rather than on
 * the stack: 64 KiB is more than a Horizon thread's stack should be
 * asked for, and this case is single-threaded. */
static uint32_t uc_big[UC_BIG_B / 4u];
static uint32_t uc_small[UC_SMALL_B / 4u];
static uint32_t uc_tail[UC_TAIL_B / 4u];

static void uc_fill(uint32_t rep)
{
   for (uint32_t i = 0; i < UC_BIG_B / 4u; i++)
      uc_big[i] = uc_word(rep, i);
   for (uint32_t i = 0; i < UC_SMALL_B / 4u; i++)
      uc_small[i] = uc_word(rep, 0x40000u + i);
   for (uint32_t i = 0; i < UC_TAIL_B / 4u; i++)
      uc_tail[i] = uc_word(rep, 0x80000u + i);
}

/* Counts the words of `got` that differ from `want`, and remembers the
 * first one so a failure says what arrived instead. */
typedef struct uc_diff {
   uint32_t wrong;
   bool have_first;
   uint32_t first_rep;
   uint32_t first_index;
   uint32_t first_got;
   uint32_t first_want;
} uc_diff;

static void uc_compare(uc_diff *d, uint32_t rep, const uint32_t *got,
                       const uint32_t *want, uint32_t words)
{
   for (uint32_t i = 0; i < words; i++) {
      if (got[i] == want[i])
         continue;

      d->wrong++;
      if (!d->have_first) {
         d->have_first = true;
         d->first_rep = rep;
         d->first_index = i;
         d->first_got = got[i];
         d->first_want = want[i];
      }
   }
}

static void uc_report(test_ctx *t, const uc_diff *d, const char *what,
                      uint32_t total_words)
{
   if (d->have_first) {
      t_note(t, "%s: first mismatch in repetition %" PRIu32 " at word "
                "%" PRIu32 ": got 0x%08x, want 0x%08x", what, d->first_rep,
             d->first_index, d->first_got, d->first_want);
   }

   t_check(t, d->wrong == 0,
           "%s: %" PRIu32 " of %" PRIu32 " words arrived as the CPU wrote "
           "them", what, total_words - d->wrong, total_words);
}

TEST_CASE_DECL(vk_core, upload_chunks)
{
   vkfw fw;
   int rv = 0;

   if (!vkfw_init(&fw, t, NULL))
      return 1;

   vkfw_buffer dst;
   memset(&dst, 0, sizeof(dst));

   if (!vkfw_buffer_create(&fw, UC_DST_B,
                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &dst)) {
      rv = 1;
      goto out;
   }

   t_note(t, "destination: %" PRIu32 " bytes, %s memory; %" PRIu32
             " repetitions of a %" PRIu32 "-byte update followed by a "
             "%" PRIu32 "-byte one", (uint32_t)UC_DST_B,
          dst.coherent ? "coherent" : "cached", (uint32_t)UC_REPS,
          (uint32_t)UC_SMALL_B, (uint32_t)UC_BIG_B);

   /* ---------------------------------------------------------------- */
   /* A — the sequence that leaves a chunk unflushed.                  */
   /* ---------------------------------------------------------------- */

   /* Small first, so the command buffer has an upload chunk with a low
    * offset in it; then the largest update Vulkan allows, which cannot
    * fit beside it and cannot be adopted in its place. Before `0073`
    * the second chunk's 64 KiB were never cleaned. */
   {
      uc_diff big_d, small_d;
      memset(&big_d, 0, sizeof(big_d));
      memset(&small_d, 0, sizeof(small_d));
      uint32_t reps_run = 0;

      for (uint32_t rep = 0; rep < UC_REPS; rep++) {
         uc_fill(rep);

         if (!vkfw_buffer_poison(&fw, &dst, UC_POISON))
            break;

         VkCommandBuffer cb;
         if (!vkfw_cmd_begin(&fw, &cb))
            break;

         fw.vk.vkCmdUpdateBuffer(cb, dst.buf, UC_SMALL_OFF, UC_SMALL_B,
                                 uc_small);
         fw.vk.vkCmdUpdateBuffer(cb, dst.buf, UC_BIG_OFF, UC_BIG_B, uc_big);

         if (!vkfw_submit_and_wait(&fw, cb, "A: small then full-size"))
            break;
         if (!vkfw_buffer_invalidate(&fw, &dst))
            break;

         const uint32_t *got = dst.map;
         uc_compare(&small_d, rep, got + UC_SMALL_OFF / 4u, uc_small,
                    UC_SMALL_B / 4u);
         uc_compare(&big_d, rep, got + UC_BIG_OFF / 4u, uc_big,
                    UC_BIG_B / 4u);
         reps_run++;
      }

      t_check(t, reps_run == UC_REPS,
              "A: %" PRIu32 " of %" PRIu32 " repetitions ran", reps_run,
              (uint32_t)UC_REPS);

      uc_report(t, &small_d, "MEASURED A: the small update",
                reps_run * (UC_SMALL_B / 4u));
      uc_report(t, &big_d, "MEASURED A: the full-size update, whose chunk "
                "is the one `0073` is about",
                reps_run * (UC_BIG_B / 4u));
   }

   /* ---------------------------------------------------------------- */
   /* B — and it is not simply "the last upload is not flushed".       */
   /* ---------------------------------------------------------------- */

   /* A third update after the big one goes back to the chunk the small
    * one is in — it fits there — so it advances THAT chunk's used_B and
    * not the big one's. A driver that cleaned only the chunk it
    * finished on would pass section A's shape by accident and fail
    * here; one that cleans what was written passes both. */
   {
      uc_diff big_d, small_d, tail_d;
      memset(&big_d, 0, sizeof(big_d));
      memset(&small_d, 0, sizeof(small_d));
      memset(&tail_d, 0, sizeof(tail_d));
      uint32_t reps_run = 0;

      for (uint32_t rep = 0; rep < UC_REPS; rep++) {
         /* Offset by UC_REPS so no repetition of B repeats a pattern A
          * already put in a chunk. */
         uc_fill(UC_REPS + rep);

         if (!vkfw_buffer_poison(&fw, &dst, UC_POISON))
            break;

         VkCommandBuffer cb;
         if (!vkfw_cmd_begin(&fw, &cb))
            break;

         fw.vk.vkCmdUpdateBuffer(cb, dst.buf, UC_SMALL_OFF, UC_SMALL_B,
                                 uc_small);
         fw.vk.vkCmdUpdateBuffer(cb, dst.buf, UC_BIG_OFF, UC_BIG_B, uc_big);
         fw.vk.vkCmdUpdateBuffer(cb, dst.buf, UC_TAIL_OFF, UC_TAIL_B,
                                 uc_tail);

         if (!vkfw_submit_and_wait(&fw, cb, "B: small, full-size, small"))
            break;
         if (!vkfw_buffer_invalidate(&fw, &dst))
            break;

         const uint32_t *got = dst.map;
         uc_compare(&small_d, rep, got + UC_SMALL_OFF / 4u, uc_small,
                    UC_SMALL_B / 4u);
         uc_compare(&big_d, rep, got + UC_BIG_OFF / 4u, uc_big,
                    UC_BIG_B / 4u);
         uc_compare(&tail_d, rep, got + UC_TAIL_OFF / 4u, uc_tail,
                    UC_TAIL_B / 4u);
         reps_run++;
      }

      t_check(t, reps_run == UC_REPS,
              "B: %" PRIu32 " of %" PRIu32 " repetitions ran", reps_run,
              (uint32_t)UC_REPS);

      uc_report(t, &small_d, "MEASURED B: the first small update",
                reps_run * (UC_SMALL_B / 4u));
      uc_report(t, &big_d, "MEASURED B: the full-size update between them",
                reps_run * (UC_BIG_B / 4u));
      uc_report(t, &tail_d, "MEASURED B: the small update after it",
                reps_run * (UC_TAIL_B / 4u));
   }

   /* ---------------------------------------------------------------- */
   /* C — two unflushed chunks in one command buffer.                  */
   /* ---------------------------------------------------------------- */

   /* Two full-size updates after a small one take a chunk each, and
    * neither is adopted, so both depend on the same missing line. One
    * repetition per pattern is enough here: what C adds is that the
    * defect is per chunk and not per command buffer. */
   {
      uc_diff first_d, second_d;
      memset(&first_d, 0, sizeof(first_d));
      memset(&second_d, 0, sizeof(second_d));
      uint32_t reps_run = 0;

      for (uint32_t rep = 0; rep < UC_REPS; rep++) {
         uc_fill(2u * UC_REPS + rep);

         if (!vkfw_buffer_poison(&fw, &dst, UC_POISON))
            break;

         VkCommandBuffer cb;
         if (!vkfw_cmd_begin(&fw, &cb))
            break;

         fw.vk.vkCmdUpdateBuffer(cb, dst.buf, UC_TAIL_OFF, UC_TAIL_B,
                                 uc_tail);
         /* The same 64 KiB written twice, to two different places, so
          * the two chunks hold identical bytes and a mismatch cannot be
          * explained by one of them having got the other's data. */
         fw.vk.vkCmdUpdateBuffer(cb, dst.buf, UC_SMALL_OFF, UC_BIG_B,
                                 uc_big);
         fw.vk.vkCmdUpdateBuffer(cb, dst.buf, UC_BIG_OFF, UC_BIG_B, uc_big);

         if (!vkfw_submit_and_wait(&fw, cb, "C: two full-size updates"))
            break;
         if (!vkfw_buffer_invalidate(&fw, &dst))
            break;

         const uint32_t *got = dst.map;
         uc_compare(&first_d, rep, got + UC_SMALL_OFF / 4u, uc_big,
                    UC_BIG_B / 4u);
         uc_compare(&second_d, rep, got + UC_BIG_OFF / 4u, uc_big,
                    UC_BIG_B / 4u);
         reps_run++;
      }

      t_check(t, reps_run == UC_REPS,
              "C: %" PRIu32 " of %" PRIu32 " repetitions ran", reps_run,
              (uint32_t)UC_REPS);

      uc_report(t, &first_d, "MEASURED C: the first of two chunks",
                reps_run * (UC_BIG_B / 4u));
      uc_report(t, &second_d, "MEASURED C: the second of two chunks",
                reps_run * (UC_BIG_B / 4u));
   }

out:
   vkfw_buffer_destroy(&fw, &dst);
   vkfw_finish(&fw);
   return rv;
}
