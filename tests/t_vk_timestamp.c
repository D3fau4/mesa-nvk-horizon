/*
 * What a GPU timestamp tick is worth, measured rather than assumed.
 *
 * WHAT IS ASSUMED TODAY. nvk_physical_device.c publishes
 * VkPhysicalDeviceLimits::timestampPeriod = 1.0f — one nanosecond per
 * tick — for every device NVK drives. On the GPUs nouveau was written
 * for that is the truth, because their PTIMER counts nanoseconds. It
 * was never checked here, and timestampPeriod is not a detail: it is the
 * only thing that turns the number vkGetQueryPoolResults returns into a
 * duration. Every profiler, every frame-time overlay and
 * VK_EXT_calibrated_timestamps all scale by it, so if it is wrong they
 * are all wrong by the same factor and none of them can tell.
 *
 * THERE ARE TWO CLOCKS AND THEY MUST NOT BE CONFLATED. This test
 * measures both, separately, because they are read through completely
 * different paths and nothing has ever shown they agree:
 *
 *   the QUERY clock  — what vkCmdWriteTimestamp records. NVK emits
 *                      NV9097_SET_REPORT_SEMAPHORE and the GPU writes
 *                      its own timestamp beside the payload. This is
 *                      the clock timestampPeriod describes.
 *
 *   the DEVICE clock — what horizon_gpu_device_get_timestamp returns,
 *                      through the nv driver's GET_GPU_TIME ioctl. This
 *                      is what nvkmd_horizon hands to NVK's
 *                      get_gpu_timestamp, and therefore what
 *                      VK_EXT_calibrated_timestamps would pair with a
 *                      CPU clock.
 *
 * VK_EXT_calibrated_timestamps is only correct if those two are the same
 * domain. Asking one question would have hidden that.
 *
 * HOW EACH IS MEASURED. Against the CPU, which is the one clock here
 * whose unit is not in doubt: armGetSystemTick / armTicksToNs is the
 * system counter, and every other test in this suite already reports
 * durations through it.
 *
 *   1. Read both GPU clocks and the CPU clock.
 *   2. Sleep for a known wall-clock interval.
 *   3. Read all three again.
 *   4. ns per tick = elapsed CPU ns / elapsed GPU ticks.
 *
 * The query clock cannot be read without submitting, so each of its two
 * reads is a vkCmdWriteTimestamp in its own submit, waited to
 * completion. That puts submit latency inside the interval — which is
 * exactly why the interval is long: at half a second, an 85 microsecond
 * submit is under a part in five thousand, far below the difference
 * between the two candidate answers.
 *
 * WHAT THE ANSWER LOOKS LIKE. A ratio near 1.0 says timestampPeriod is
 * already right. A ratio near 1.625 — thirteen eighths — says a tick is
 * not a nanosecond and every duration this driver reports is off by
 * that factor. The test PRINTS the measured ratio and does not enforce
 * either: it exists to find out. It fails only on things that are
 * broken whatever the ratio turns out to be — a timestamp that does not
 * advance, a query that returns nothing, a period that is not positive.
 *
 * NOTHING IS CHANGED BY THIS TEST. timestampPeriod stays at 1.0f until
 * this has run on a console, because changing it on a prediction is how
 * the wrong constant gets a measurement's authority.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#include <switch.h>

#include "horizon_gpu/device.h"
#include "common/vkfw.h"

const char *const test_name = "t_vk_timestamp";
/* No display: main() starts a console and reports through it. */
const bool test_uses_display = false;

/* Long enough that submit latency and scheduler noise are noise, short
 * enough that the whole test is a few seconds. */
#define INTERVAL_NS UINT64_C(500000000)

/* The two candidate answers, named so the log says which one it landed
 * on instead of leaving the reader to divide. */
#define CANDIDATE_ONE_NS   1.0
#define CANDIDATE_13_8_NS  1.625

/* Writes one timestamp into `pool` at `index` and waits for it. */
static bool write_timestamp(vkfw *fw, VkQueryPool pool, uint32_t index,
                            const char *what)
{
   VkCommandBuffer cb;
   if (!vkfw_cmd_begin(fw, &cb))
      return false;

   fw->vk.vkCmdResetQueryPool(cb, pool, index, 1);
   fw->vk.vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                              pool, index);

   return vkfw_submit_and_wait(fw, cb, what);
}

static bool read_timestamp(vkfw *fw, VkQueryPool pool, uint32_t index,
                           uint64_t *out, const char *what)
{
   uint64_t value = 0;
   VkResult r = fw->vk.vkGetQueryPoolResults(
      fw->dev, pool, index, 1, sizeof(value), &value, sizeof(value),
      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
   if (!t_check(fw->t, r == VK_SUCCESS, "%s: vkGetQueryPoolResults -> %s",
                what, vkfw_result_str(r)))
      return false;
   *out = value;
   return true;
}

/* Prints a measured tick length beside the two candidates. */
static void report(test_ctx *t, const char *clock_name, uint64_t ticks,
                   uint64_t cpu_ns)
{
   if (ticks == 0) {
      t_note(t, "MEASURED %s: the counter did not advance at all over "
             "%" PRIu64 " ms of wall clock", clock_name,
             cpu_ns / 1000000);
      return;
   }

   /* Integer parts per million, so the log carries the number without a
    * float format the console's printf may not have. */
   const uint64_t ns_per_tick_ppm = (cpu_ns * UINT64_C(1000000)) / ticks;

   t_note(t, "MEASURED %s: %" PRIu64 " ticks in %" PRIu64 " ns "
          "= %" PRIu64 ".%06" PRIu64 " ns per tick", clock_name, ticks,
          cpu_ns, ns_per_tick_ppm / 1000000, ns_per_tick_ppm % 1000000);

   const uint64_t one = (uint64_t)(CANDIDATE_ONE_NS * 1000000.0);
   const uint64_t thirteen_eighths = (uint64_t)(CANDIDATE_13_8_NS * 1000000.0);
   /* One per cent either way; the two candidates are 62 per cent
    * apart, so the window cannot match both. */
   const uint64_t tol = 10000;

   if (ns_per_tick_ppm + tol >= one && ns_per_tick_ppm <= one + tol) {
      t_note(t, "MEASURED %s: this is 1 ns per tick, so a "
             "timestampPeriod of 1.0f is right for it", clock_name);
   } else if (ns_per_tick_ppm + tol >= thirteen_eighths &&
              ns_per_tick_ppm <= thirteen_eighths + tol) {
      t_note(t, "MEASURED %s: this is 13/8 = 1.625 ns per tick. A "
             "timestampPeriod of 1.0f understates every duration read "
             "from this clock by that factor.", clock_name);
   } else {
      t_note(t, "MEASURED %s: neither 1.0 nor 1.625. Whatever it is, it "
             "is what timestampPeriod has to say.", clock_name);
   }
}

int run_test(test_ctx *t)
{
   vkfw fw;
   if (!vkfw_init(&fw, t, NULL))
      return 1;

   t_note(t, "the driver publishes timestampPeriod = %u.%06u ns and "
          "timestampComputeAndGraphics = %s",
          (unsigned)fw.props.limits.timestampPeriod,
          (unsigned)((fw.props.limits.timestampPeriod -
                      (float)(unsigned)fw.props.limits.timestampPeriod) *
                     1000000.0f),
          fw.props.limits.timestampComputeAndGraphics ? "true" : "false");

   t_check(t, fw.props.limits.timestampPeriod > 0.0f,
           "timestampPeriod is positive");

   /* The device clock is horizon_gpu's, not Vulkan's, so it needs its
    * own device handle. Opening a second one alongside NVK's is what
    * t_sysinfo already does. */
   horizon_gpu_device *dev = NULL;
   horizon_gpu_result hres = horizon_gpu_device_create(NULL, &dev);
   const bool have_dev_clock = horizon_gpu_succeeded(hres);
   if (!have_dev_clock) {
      t_note(t, "no second horizon_gpu device (status=%s nv=0x%08x); the "
             "device clock cannot be measured in this run",
             horizon_gpu_status_str(hres.status), hres.nv);
   }

   VkQueryPool qpool = VK_NULL_HANDLE;
   const VkQueryPoolCreateInfo qpci = {
      .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
      .queryType = VK_QUERY_TYPE_TIMESTAMP,
      .queryCount = 2,
   };
   VkResult r = fw.vk.vkCreateQueryPool(fw.dev, &qpci, NULL, &qpool);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateQueryPool(TIMESTAMP, 2) -> %s",
                vkfw_result_str(r)))
      goto out;

   /* ---- first reading of all three clocks ------------------------- */
   if (!write_timestamp(&fw, qpool, 0, "first timestamp"))
      goto out;
   uint64_t q0 = 0;
   if (!read_timestamp(&fw, qpool, 0, &q0, "first timestamp"))
      goto out;

   uint64_t d0 = 0;
   if (have_dev_clock) {
      hres = horizon_gpu_device_get_timestamp(dev, &d0);
      t_check(t, horizon_gpu_succeeded(hres),
              "device clock read (status=%s)",
              horizon_gpu_status_str(hres.status));
   }
   const uint64_t c0 = armGetSystemTick();

   svcSleepThread((s64)INTERVAL_NS);

   /* ---- second reading -------------------------------------------- */
   const uint64_t cpu_ns = armTicksToNs(armGetSystemTick() - c0);
   uint64_t d1 = 0;
   if (have_dev_clock) {
      hres = horizon_gpu_device_get_timestamp(dev, &d1);
      t_check(t, horizon_gpu_succeeded(hres),
              "device clock read again (status=%s)",
              horizon_gpu_status_str(hres.status));
   }

   if (!write_timestamp(&fw, qpool, 1, "second timestamp"))
      goto out;
   uint64_t q1 = 0;
   if (!read_timestamp(&fw, qpool, 1, &q1, "second timestamp"))
      goto out;

   t_note(t, "wall clock across the interval: %" PRIu64 " ns", cpu_ns);

   /* ---- the query clock ------------------------------------------- */
   t_check(t, q1 != q0, "the query timestamp advanced (0x%016" PRIx64
           " -> 0x%016" PRIx64 ")", q0, q1);
   t_check(t, q1 > q0, "the query timestamp advanced forwards");
   if (q1 > q0)
      report(t, "QUERY clock (vkCmdWriteTimestamp)", q1 - q0, cpu_ns);

   /* ---- the device clock ------------------------------------------ */
   if (have_dev_clock) {
      t_check(t, d1 > d0, "the device timestamp advanced forwards "
              "(0x%016" PRIx64 " -> 0x%016" PRIx64 ")", d0, d1);
      if (d1 > d0)
         report(t, "DEVICE clock (GET_GPU_TIME)", d1 - d0, cpu_ns);

      /* THE QUESTION VK_EXT_calibrated_timestamps TURNS ON. If the two
       * clocks tick at different rates they are different domains, and
       * pairing one of them with a CPU clock says nothing about the
       * other. */
      if (q1 > q0 && d1 > d0) {
         const uint64_t ratio_ppm =
            ((q1 - q0) * UINT64_C(1000000)) / (d1 - d0);
         t_note(t, "MEASURED: query ticks per device tick = "
                "%" PRIu64 ".%06" PRIu64 ". Equal rates mean one domain "
                "and calibrated timestamps can pair them; unequal means "
                "they cannot.", ratio_ppm / 1000000, ratio_ppm % 1000000);
      }
   }

out:
   if (qpool != VK_NULL_HANDLE)
      fw.vk.vkDestroyQueryPool(fw.dev, qpool, NULL);
   if (have_dev_clock)
      horizon_gpu_device_destroy(dev);
   vkfw_finish(&fw);
   return 0;
}
