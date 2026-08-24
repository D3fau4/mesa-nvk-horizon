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

#endif /* HORIZON_SYNC_NV_WAIT_H */
