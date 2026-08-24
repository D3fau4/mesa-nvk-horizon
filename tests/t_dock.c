/*
 * Does this process learn that the console was docked, and what does the
 * window become when it does?
 *
 * WHY IT NEEDS A TEST OF ITS OWN. The WSI backend's whole notion of
 * "this swapchain is suboptimal" rests on a display mode change
 * reaching the process: patch 0040 turned a dock into
 * VK_SUBOPTIMAL_KHR, and patch 0054 made a swapchain's own extent
 * independent of the layer's, which means the only thing left that can
 * make a swapchain suboptimal IS a mode change. Two runs of
 * t_vk_swapchain's section G, with the console genuinely docked and
 * undocked several times by hand, reported
 *
 *   note G: ... appletGetOperationMode() went 0 -> 0 (0 handheld, 1 docked)
 *
 * both times — the second of them with the wait loop pumping
 * appletMainLoop(), which is what libnx says updates that state.
 *
 * That is either a platform fact or a mistake in a two-minute test with
 * a ninety-second window at the end of it, and those are very different
 * things to write down. This is the smallest instrument that can tell
 * them apart: a console the operator can watch, a hook, and nothing
 * else in the way.
 *
 * IT ASKS THREE QUESTIONS AT ONCE, because they can disagree:
 *
 *   - appletGetOperationMode(), the applet's cached answer, which libnx
 *     updates from AppletMessage_OperationModeChanged inside
 *     appletMainLoop().
 *   - AppletHookType_OnOperationMode, the callback for that same
 *     message. If the hook fires and the mode does not move, the
 *     message arrived and something else is wrong.
 *   - nwindowGetDimensions() and NWindow::default_*, which is what the
 *     WSI actually reads. The mode changing and the layer resizing are
 *     not the same event, and a backend that watches the wrong one
 *     works until it does not.
 *
 * There is no pass condition on the operator doing anything. A run
 * where nobody touches the console records that, and says so.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#include <switch.h>

#include "common/testfw.h"

const char *const test_name = "t_dock";
/* A console, and it is the point: the operator has to see this happen
 * while they are holding the console. */
const bool test_uses_display = false;

/* Long enough to dock and undock without hurrying, short enough that a
 * run where nobody is there ends by itself. */
#define DOCK_WATCH_NS  UINT64_C(60000000000)
#define DOCK_POLL_NS   UINT64_C(50000000)

/* Set by the hook, read by the loop. The hook runs on this thread from
 * inside appletMainLoop(), so a plain counter is enough. */
static volatile uint32_t g_hook_calls;
static volatile uint32_t g_hook_mode_calls;

static void dock_hook(AppletHookType hook, void *param)
{
    (void)param;
    g_hook_calls++;
    if (hook == AppletHookType_OnOperationMode)
        g_hook_mode_calls++;
}

int run_test(test_ctx *t)
{
    NWindow *win = nwindowGetDefault();
    if (!t_check(t, win != NULL && nwindowIsValid(win),
                 "nwindowGetDefault() returned a valid window"))
        return 1;

    AppletHookCookie cookie;
    appletHook(&cookie, dock_hook, NULL);

    int mode = (int)appletGetOperationMode();
    u32 w = 0, h = 0;
    Result rc = nwindowGetDimensions(win, &w, &h);
    t_check(t, R_SUCCEEDED(rc), "nwindowGetDimensions -> 0x%08x", rc);

    u32 dw = win->default_width, dh = win->default_height;

    t_note(t, "AT START: appletGetOperationMode()=%d (0 handheld, "
           "1 docked), nwindowGetDimensions=%" PRIu32 "x%" PRIu32
           ", NWindow::default_=%" PRIu32 "x%" PRIu32, mode, w, h, dw, dh);
    t_note(t, "DOCK AND UNDOCK THE CONSOLE NOW. Watching for %" PRIu64
           " s; every change is printed as it happens.",
           DOCK_WATCH_NS / UINT64_C(1000000000));

    PadState pad;
    padInitializeDefault(&pad);

    const u64 start = armGetSystemTick();
    uint32_t changes = 0;
    int lowest = mode, highest = mode;

    while (appletMainLoop() &&
           armTicksToNs(armGetSystemTick() - start) < DOCK_WATCH_NS) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_B)
            break;

        const int now = (int)appletGetOperationMode();
        u32 nw_w = 0, nw_h = 0;
        nwindowGetDimensions(win, &nw_w, &nw_h);
        const u32 ndw = win->default_width, ndh = win->default_height;

        if (now != mode || nw_w != w || nw_h != h || ndw != dw ||
            ndh != dh) {
            changes++;
            t_note(t, "CHANGE %" PRIu32 " at %" PRIu64 " ms: mode %d -> "
                   "%d, dimensions %" PRIu32 "x%" PRIu32 " -> %" PRIu32
                   "x%" PRIu32 ", default_ %" PRIu32 "x%" PRIu32 " -> "
                   "%" PRIu32 "x%" PRIu32 ", hook fired %" PRIu32
                   " time(s), %" PRIu32 " of them for the mode",
                   changes,
                   armTicksToNs(armGetSystemTick() - start) / 1000000,
                   mode, now, w, h, nw_w, nw_h, dw, dh, ndw, ndh,
                   g_hook_calls, g_hook_mode_calls);
            mode = now;
            w = nw_w; h = nw_h;
            dw = ndw; dh = ndh;
            if (mode < lowest)
                lowest = mode;
            if (mode > highest)
                highest = mode;
        }

        svcSleepThread((s64)DOCK_POLL_NS);
    }

    appletUnhook(&cookie);

    t_note(t, "MEASURED: %" PRIu32 " change(s) seen; the applet hook fired "
           "%" PRIu32 " time(s), %" PRIu32 " of them for "
           "AppletHookType_OnOperationMode", changes, g_hook_calls,
           g_hook_mode_calls);

    if (highest != lowest) {
        t_note(t, "MEASURED: the operation mode DID move (%d and %d were "
               "both seen), so a dock reaches this process and the WSI "
               "can be built on it", lowest, highest);
    } else if (g_hook_mode_calls != 0) {
        t_note(t, "MEASURED: the hook fired for the operation mode but "
               "appletGetOperationMode() never moved off %d. The message "
               "arrives and the state does not follow it.", mode);
    } else if (changes != 0) {
        t_note(t, "MEASURED: something moved but the operation mode did "
               "not and its hook never fired. Whatever the WSI watches, "
               "it cannot be that mode.");
    } else {
        t_note(t, "MEASURED: NOTHING moved. Either nobody touched the "
               "console, or a dock does not reach a process launched this "
               "way — and this run cannot tell those apart. Run it again "
               "and dock while it is watching.");
    }

    /* The only thing asserted is that the instrument worked: the window
     * stayed valid and the applet loop kept running. What the operator
     * did, or did not do, is reported and never failed on. */
    t_check(t, nwindowIsValid(win),
            "the window is still valid after the watch");
    return 0;
}
