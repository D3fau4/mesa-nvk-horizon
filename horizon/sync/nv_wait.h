/*
 * What nvFenceWait's Result means, in the one place both wait loops can
 * read it.
 *
 * Private to horizon/ (libnx types are fine below horizon/include/, and
 * check-layering.sh draws the line there). Included by
 * horizon/sync/syncpt.c and horizon/channel/channel.c, which run the
 * same loop over the same ioctl and disagreed about this once already.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef HORIZON_SYNC_NV_WAIT_H
#define HORIZON_SYNC_NV_WAIT_H

#include <stdbool.h>
#include <stdint.h>

#include <switch.h>

/* "The wait was armed and its deadline passed."
 *
 * MEASURED on a console 2026-08-24 (t_fence_wait_many): a genuinely
 * pending fence, waited on for 300 ms, comes back
 *
 *     Result 0x00000d5c (module 348, desc 6)
 *
 * after having blocked the whole 300 ms. Module 348 is
 * Module_LibnxNvidia and description 6 is LibnxNvidiaError_Timeout
 * (switch/result.h), so nvFenceWait reports a timeout as the *nv
 * service's* timeout — not as KERNELRESULT(TimedOut), which is what
 * both loops used to compare against and nothing else here produces.
 *
 * That mattered beyond a mislabelled log line. Both files had a branch
 * reading "if it is not KERNELRESULT(TimedOut) it is a failure, return
 * it", on the path taken when the syncpoint counter cannot be read: an
 * ordinary expired chunk would have been reported to the caller as an
 * error there.
 *
 * KERNELRESULT(TimedOut) is still accepted. libnx routes some waits
 * through svcWaitSynchronization, and a platform that answers with it is
 * saying the same thing.
 */
static inline bool horizon_nv_wait_timed_out(Result rc)
{
    return rc == KERNELRESULT(TimedOut) ||
           rc == MAKERESULT(Module_LibnxNvidia, LibnxNvidiaError_Timeout);
}

/* Upper bound on the pacing sleep both wait loops take.
 *
 * The point of the sleep is to stop a chunk that did not block turning
 * the loop into two ioctls back to back; a millisecond of it does that,
 * cutting the ioctl rate by three orders of magnitude. Sleeping out the
 * whole unspent chunk would do it better and would also mean that a
 * counter read lagging the kernel's own answer — measured happening,
 * 2026-08-24: nvFenceWait returned success at 1347 us of a 100 ms chunk
 * while SyncptRead still showed the old value — could add most of a
 * chunk to a fence that had in fact retired. A wait must not be slower
 * than the thing it is waiting for.
 *
 * AND IT IS OBSERVABLE. Pacing changes no verdict and no wall time —
 * a spinning wait and a paced wait both consume the caller's deadline
 * and both end in TIMEOUT — so the only difference a caller can see is
 * how many times the loop went round. Both loops count that into the
 * device's wait meter (horizon_gpu_device_wait_stats), which is what
 * lets gpu_submit/fence_wait_many part 2 assert this instead of
 * describing it.
 *
 * HERE BECAUSE IT WAS IN BOTH FILES. syncpt.c had SYNC_PACE_MAX_NS and
 * channel.c had CHANNEL_PACE_MAX_NS, same value, same paragraph, and
 * the second one said so — which is the shape this header exists to
 * end. The loops themselves still differ (only the channel's re-checks
 * the error notifier), so only the number and its reason moved. */
#define HORIZON_NV_WAIT_PACE_MAX_NS UINT64_C(1000000)

#endif /* HORIZON_SYNC_NV_WAIT_H */
