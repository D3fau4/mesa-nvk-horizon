/*
 * The first test that owns the display, and it draws nothing.
 *
 * WHAT IT IS FOR. Phase 6 is the Horizon swapchain, and its first
 * problem is not Vulkan: main() calls consoleInit(NULL), which
 * configures the default nwindow with the console's own framebuffers
 * and holds them. A swapchain configures that same nwindow. Both cannot
 * have it, so a presenting test has to run without a console — and then
 * the only record of the run is the file on the SD card.
 *
 * That switch is `test_uses_display` (tests/common/testfw.h), and this
 * test exists to fire it before anything depends on it. A mechanism
 * whose first execution is inside the first swapchain test would make
 * every swapchain failure ambiguous: the driver, or the harness that
 * was supposed to be reporting it?
 *
 * WHAT IT ASSERTS. Not much, on purpose — the interesting outcomes are
 * all outside the checks:
 *
 *   - it links at all, which is the weak default being overridden;
 *   - it produces a log with a RESULT line in it, which is the whole
 *     reporting path working with no console behind it;
 *   - it says so in the log, so a reader is never left deciding
 *     whether a console started and printed nothing;
 *   - and it exits cleanly, which is the part that would strand a
 *     console-less applet on a black screen if it did not.
 *
 * If this run produces a log, the harness half of Phase 6 is done. If
 * it produces nothing, the answer is nxlink over the network instead,
 * and better to learn that here than three tests into a swapchain.
 *
 * The screen during the run holds whatever was on it before. That is
 * not a defect; it is the framebuffer nobody has taken, which is
 * precisely the state a swapchain wants to find.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdbool.h>

#include <switch.h>

#include "common/testfw.h"

const char *const test_name = "t_display";

/* The one test that answers true. */
const bool test_uses_display = true;

int run_test(test_ctx *t)
{
   t_check(t, test_uses_display,
           "this test claims the display, so main() started no console");

   /* The default window exists and is ours to ask about. Nothing is
    * configured or dequeued here: Phase 6 does that, and doing it now
    * would put an untested thing inside the test that exists to make
    * the tested thing unambiguous. */
   NWindow *win = nwindowGetDefault();
   t_check(t, win != NULL, "nwindowGetDefault() returned a window");

   if (win != NULL) {
      u64 w = 0, h = 0;
      Result rc = nwindowGetDimensions(win, (u32 *)&w, (u32 *)&h);
      t_check(t, R_SUCCEEDED(rc),
              "nwindowGetDimensions -> 0x%08x (%llu x %llu)", rc,
              (unsigned long long)w, (unsigned long long)h);
      t_note(t, "the default window is %llu x %llu",
             (unsigned long long)w, (unsigned long long)h);
   }

   t_note(t, "if you are reading this line, a console-less run reported "
             "itself and Phase 6 can report through this file");
   return 0;
}
