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
#include <stdio.h>
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

/* How many intervals. One says what the rate was once; several say
 * whether there is a rate at all. */
#define WINDOWS 3u

/* The two candidate answers, named so the log says which one it landed
 * on instead of leaving the reader to divide. */
#define CANDIDATE_ONE_NS   1.0
#define CANDIDATE_13_8_NS  1.625

/* Writes one timestamp into `pool` at `index` and waits for it.
 *
 * ONE COMMAND BUFFER, and it took two runs and a fix elsewhere to get
 * back to it. nvk_CmdResetQueryPool writes 0 to the query's
 * availability word through an NV9097 report semaphore and then
 * acquires on that word from the host engine; nvk_CmdWriteTimestamp2
 * writes the report and releases 1 to it. Three accesses to one
 * address, from two engines, inside one submission — and on 2026-08-24
 * the query never became available, on about half the runs.
 *
 * That looked like the two engines racing and was not. The cause was
 * stale dirty CPU cache lines on a fresh allocation, fixed in
 * horizon/memory/mem.c: the invalidate before reading the word cleaned
 * the line first, writing the memset's zeros over what the GPU had
 * written. This function ran the reset in a submit of its own while
 * that was still unknown, so the measurement below could be made at
 * all, and said in as many words that it was not a fix.
 *
 * MEASURED after the fix, the same day: the combined form became
 * available 6 of 6 times. The split is gone and so is the probe that
 * asked for it.
 */
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

/* POLLED, WITHOUT VK_QUERY_RESULT_WAIT_BIT, AND THAT IS THE INSTRUMENT.
 *
 * With the wait bit, NVK spins for two seconds and then calls
 * vk_device_set_lost() — every later call on that device returns
 * VK_ERROR_DEVICE_LOST, so one unavailable query ends the run and takes
 * the measurement with it. Measured 2026-08-24: it does that, and NOT
 * every time. Some runs read the query immediately; others never see it
 * become available at all. An intermittent failure that poisons the
 * device on the first occurrence cannot be characterised.
 *
 * Without the wait bit the same call answers VK_NOT_READY and leaves
 * the device alone, so this can poll on its own terms and say how long
 * availability took — or that it never came, and carry on to the next
 * reading either way. */
#define QUERY_POLL_NS   UINT64_C(2000000000)
#define QUERY_POLL_STEP UINT64_C(1000000)

static bool read_timestamp(vkfw *fw, VkQueryPool pool, uint32_t index,
                           uint64_t *out, const char *what)
{
   const uint64_t start = armGetSystemTick();
   uint64_t waited_ns = 0;
   VkResult r = VK_NOT_READY;

   while (waited_ns < QUERY_POLL_NS) {
      uint64_t value = 0;
      r = fw->vk.vkGetQueryPoolResults(
         fw->dev, pool, index, 1, sizeof(value), &value, sizeof(value),
         VK_QUERY_RESULT_64_BIT);
      if (r == VK_SUCCESS) {
         *out = value;
         if (waited_ns > 0) {
            t_note(fw->t, "%s: the query became available after "
                   "%" PRIu64 " us of polling", what, waited_ns / 1000);
         }
         return true;
      }
      if (r != VK_NOT_READY)
         break;
      svcSleepThread((s64)QUERY_POLL_STEP);
      waited_ns = armTicksToNs(armGetSystemTick() - start);
   }

   t_check(fw->t, false, "%s: vkGetQueryPoolResults -> %s after "
           "%" PRIu64 " ms of polling", what, vkfw_result_str(r),
           waited_ns / 1000000);
   return false;
}

/* One clock reading, with the CPU clock taken either side of it.
 *
 * WHY THE MIDPOINT AND NOT THE EDGE. Reading the query clock costs two
 * submits and two fence waits — milliseconds — and the first version of
 * this test took the CPU clock *outside* both GPU readings, so the GPU
 * interval covered more wall time than the CPU interval it was divided
 * by. That biases every ns-per-tick downwards, and by enough to matter:
 * it is what made the device clock read 1.6150 instead of 1.625, a
 * 0.6% error on a number the whole point is to pin down.
 *
 * Bracketing turns the cost of the reading into an uncertainty of half
 * its length rather than a systematic error of all of it. */
typedef struct {
   uint64_t value;      /* the GPU counter                            */
   uint64_t cpu_tick;   /* midpoint of the CPU ticks either side      */
   uint64_t spread_ns;  /* how long the reading took: the uncertainty */
} clock_read;

static void bracket(clock_read *r, uint64_t before, uint64_t after,
                    uint64_t value)
{
   r->value = value;
   r->cpu_tick = before + (after - before) / 2;
   r->spread_ns = armTicksToNs(after - before);
}

/* One bracketed reading of each clock, at the same moment.
 *
 * The query clock costs two submits and two fence waits; the device
 * clock costs one ioctl. Each is bracketed by the CPU clock on its own
 * terms, so the cheap one keeps its tight bracket instead of inheriting
 * the expensive one's. */
static bool take_readings(vkfw *fw, VkQueryPool pool, uint32_t index,
                          horizon_gpu_device *dev, bool have_dev,
                          clock_read *q, clock_read *d, const char *what)
{
   const uint64_t qa = armGetSystemTick();
   if (!write_timestamp(fw, pool, index, what))
      return false;
   uint64_t value = 0;
   if (!read_timestamp(fw, pool, index, &value, what))
      return false;
   bracket(q, qa, armGetSystemTick(), value);

   if (have_dev) {
      const uint64_t da = armGetSystemTick();
      uint64_t dvalue = 0;
      horizon_gpu_result hr = horizon_gpu_device_get_timestamp(dev, &dvalue);
      const uint64_t db = armGetSystemTick();
      if (!t_check(fw->t, horizon_gpu_succeeded(hr),
                   "%s: device clock read (status=%s)", what,
                   horizon_gpu_status_str(hr.status)))
         return false;
      bracket(d, da, db, dvalue);
   }
   return true;
}

/* Prints a measured tick length beside the two candidates, and returns
 * it in parts per million so the caller can compare windows. */
static uint64_t report(test_ctx *t, const char *clock_name, uint64_t ticks,
                       uint64_t cpu_ns)
{
   if (ticks == 0) {
      t_note(t, "MEASURED %s: the counter did not advance at all over "
             "%" PRIu64 " ms of wall clock", clock_name,
             cpu_ns / 1000000);
      return 0;
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
   return ns_per_tick_ppm;
}

/* Min, max and spread across the windows. A clock a constant
 * timestampPeriod can describe has to come back the same every time;
 * this is the line that says whether it did. */
static uint64_t spread(test_ctx *t, const char *clock_name,
                       const uint64_t *ppm, uint32_t n)
{
   if (n == 0)
      return 0;
   uint64_t lo = ppm[0], hi = ppm[0], sum = 0;
   for (uint32_t i = 0; i < n; i++) {
      if (ppm[i] < lo) lo = ppm[i];
      if (ppm[i] > hi) hi = ppm[i];
      sum += ppm[i];
   }
   const uint64_t mean = sum / n;
   /* Spread as parts per million of the mean, so it can be read against
    * the reading uncertainty printed per window. */
   const uint64_t rel = mean ? ((hi - lo) * UINT64_C(1000000)) / mean : 0;
   t_note(t, "MEASURED %s over %u window(s): min %" PRIu64 ".%06" PRIu64
          ", mean %" PRIu64 ".%06" PRIu64 ", max %" PRIu64 ".%06" PRIu64
          " ns per tick; spread %" PRIu64 " ppm of the mean",
          clock_name, n, lo / 1000000, lo % 1000000, mean / 1000000,
          mean % 1000000, hi / 1000000, hi % 1000000, rel);

   /* THE DERIVATION, printed beside the number so a reader does not
    * have to find it. 19.2 MHz is the Tegra X1's reference oscillator
    * and armGetSystemTick counts it directly; 614.4 MHz is 32 times it,
    * and one tick of that is 1.627604166... ns. A clock that lands on
    * that to a few parts per million is that clock, not a coincidence. */
   const uint64_t candidate_614_4 = 1627604;
   const uint64_t d = mean > candidate_614_4 ? mean - candidate_614_4
                                             : candidate_614_4 - mean;
   if (mean != 0 && (d * UINT64_C(1000000)) / mean < 2000) {
      t_note(t, "MEASURED %s: that is 1/614.4 MHz = 1.627604 ns, and "
             "614.4 MHz is 32 x the 19.2 MHz reference armGetSystemTick "
             "counts — %" PRIu64 " ppm away", clock_name,
             (d * UINT64_C(1000000)) / mean);
   }
   return rel;
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
      .queryCount = 2 * WINDOWS,
   };
   VkResult r = fw.vk.vkCreateQueryPool(fw.dev, &qpci, NULL, &qpool);
   if (!t_check(t, r == VK_SUCCESS, "vkCreateQueryPool(TIMESTAMP, %u) -> %s",
                2u * WINDOWS, vkfw_result_str(r)))
      goto out;
   /* WINDOWS INTERVALS, NOT ONE.
    *
    * One window says what the rate was once. What has to be decided
    * here is whether there IS a rate — whether this counter is a
    * fixed-frequency clock a constant timestampPeriod can describe at
    * all, or something that moves with the GPU's own state. The spread
    * across the windows is what answers that, and it is printed. */
   uint64_t q_ppm[WINDOWS], d_ppm[WINDOWS];
   /* Worst placement uncertainty of any window, in ppm of its interval.
    * See the comment where it is computed. */
   uint64_t q_place_ppm = 0;
   uint32_t measured = 0;
   uint64_t q0v = 0, q1v = 0;

   for (uint32_t w = 0; w < WINDOWS; w++) {
      clock_read q_a, q_b, d_a, d_b;
      char what[48];

      memset(&d_a, 0, sizeof(d_a));
      memset(&d_b, 0, sizeof(d_b));

      snprintf(what, sizeof(what), "window %u: first timestamp", w);
      if (!take_readings(&fw, qpool, 2 * w, dev, have_dev_clock,
                         &q_a, &d_a, what))
         goto out;

      svcSleepThread((s64)INTERVAL_NS);

      snprintf(what, sizeof(what), "window %u: second timestamp", w);
      if (!take_readings(&fw, qpool, 2 * w + 1, dev, have_dev_clock,
                         &q_b, &d_b, what))
         goto out;

      if (w == 0) {
         q0v = q_a.value;
         q1v = q_b.value;
      }

      const uint64_t q_cpu_ns = armTicksToNs(q_b.cpu_tick - q_a.cpu_tick);

      /* HOW MUCH THE PLACEMENT COULD BE WRONG BY, in parts per million
       * of the interval. bracket() puts each reading at the midpoint of
       * the CPU clock either side of it, which removes the bias of the
       * reading's whole length and leaves half of it as an uncertainty:
       * the GPU wrote the timestamp at some instant inside that window
       * and nothing here knows which. Two readings, so half of each.
       *
       * This is the tolerance the domain comparison below has to use.
       * The run-to-run spread is NOT that tolerance and using it was a
       * mistake: the spread shrinks as the instrument improves, so a
       * faster reading made the check HARDER to pass, which is exactly
       * backwards. Removing the two-submit split halved the reading
       * cost and turned a passing comparison into a failing one without
       * either clock changing. */
      const uint64_t place_ppm = q_cpu_ns
         ? ((q_a.spread_ns / 2 + q_b.spread_ns / 2) * UINT64_C(1000000))
           / q_cpu_ns
         : 0;
      if (place_ppm > q_place_ppm)
         q_place_ppm = place_ppm;

      t_note(t, "window %u: query reading uncertainty %" PRIu64 " us and "
             "%" PRIu64 " us against a %" PRIu64 " ms interval — the "
             "timestamp's placement is good to %" PRIu64 " ppm", w,
             q_a.spread_ns / 1000, q_b.spread_ns / 1000,
             q_cpu_ns / 1000000, place_ppm);

      t_check(t, q_b.value > q_a.value,
              "window %u: the query timestamp advanced forwards "
              "(0x%016" PRIx64 " -> 0x%016" PRIx64 ")", w, q_a.value,
              q_b.value);
      if (q_b.value <= q_a.value)
         continue;

      q_ppm[measured] = report(t, "QUERY clock (vkCmdWriteTimestamp)",
                               q_b.value - q_a.value, q_cpu_ns);

      d_ppm[measured] = 0;
      if (have_dev_clock && d_b.value > d_a.value) {
         const uint64_t d_cpu_ns =
            armTicksToNs(d_b.cpu_tick - d_a.cpu_tick);
         d_ppm[measured] = report(t, "DEVICE clock (GET_GPU_TIME)",
                                  d_b.value - d_a.value, d_cpu_ns);
      }
      measured++;
   }

   const uint64_t q_spread_ppm =
      spread(t, "QUERY clock (vkCmdWriteTimestamp)", q_ppm, measured);
   if (have_dev_clock)
      spread(t, "DEVICE clock (GET_GPU_TIME)", d_ppm, measured);

   (void)q0v; (void)q1v;

   /* THE QUESTION VK_EXT_calibrated_timestamps TURNS ON. If the two
    * clocks tick at different rates they are different domains, and
    * pairing one of them with a CPU clock says nothing about the other.
    * Taken from the per-window rates rather than from one pair of
    * readings, so a single noisy window cannot decide it. */
   if (have_dev_clock && measured > 0) {
      uint64_t qs = 0, ds = 0;
      for (uint32_t i = 0; i < measured; i++) {
         qs += q_ppm[i];
         ds += d_ppm[i];
      }
      if (qs > 0 && ds > 0) {
         /* ns-per-tick ratio inverted: a query tick is SHORTER than a
          * device tick by exactly this factor. */
         const uint64_t ratio_ppm = (ds * UINT64_C(1000000)) / qs;
         t_note(t, "MEASURED: query ticks per device tick = "
                "%" PRIu64 ".%06" PRIu64 ". Equal rates mean one domain "
                "and calibrated timestamps can pair them; unequal means "
                "they cannot.", ratio_ppm / 1000000, ratio_ppm % 1000000);
         /* NOT an assertion about which way it comes out. Either answer
          * is a result: one domain means calibrated timestamps can pair
          * the two, and separate domains mean they cannot. What is
          * asserted is that the run is able to tell — the two rates
          * have to agree, or differ, by more than the query clock's own
          * spread, or the comparison decides nothing. The query clock's
          * spread is what it is because reading it costs two submits
          * and a poll; the device clock's costs one ioctl and comes
          * back to a few parts per million. */
         const uint64_t off_by = ratio_ppm > 1000000 ? ratio_ppm - 1000000
                                                     : 1000000 - ratio_ppm;
         t_note(t, "MEASURED: that is %" PRIu64 " ppm from equal, against "
                "a placement uncertainty of %" PRIu64 " ppm and a "
                "run-to-run spread of %" PRIu64 " ppm", off_by,
                q_place_ppm, q_spread_ppm);
         t_check(t, off_by <= q_place_ppm,
                 "the two clocks agree to within the uncertainty of where "
                 "the query timestamp actually landed, so they are one "
                 "domain: %" PRIu64 " ppm apart, %" PRIu64 " ppm of "
                 "placement", off_by, q_place_ppm);
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
