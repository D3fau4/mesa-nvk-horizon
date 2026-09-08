/*
 * Host unit test — nvkmd_horizon's vk_sync over a simulated syncpoint.
 *
 * THE ONE HOST SUITE THAT COMPILES A MESA FILE, and why. mesa-patches/
 * 0081 fixes an interleaving: nvk_horizon_sync_wait snapshots the fence
 * it will wait on, waits with the mutex released, and used to mark the
 * object SIGNALLED with a signalled_value re-read from the object — so
 * a set_fence() that landed meanwhile (the upload stream's sync is
 * signalled again and again) made a wait for fence F1 report F2
 * complete, and nvk_mem_stream recycled a chunk the copy engine was
 * still reading. On a console that needs two threads and a full wait
 * ring at the same moment; here the syncpoint is a function this file
 * provides, the waiter is parked inside it on a condition variable,
 * and the interleaving is fixed by construction rather than by luck.
 * No sleeps anywhere: every step waits for the state it needs.
 *
 * What is compiled: nvkmd_horizon_sync.c itself, from mesa/ with the
 * series applied, against Mesa's own headers. Everything it calls into
 * that is not the code under test — the two fence waits, the meter,
 * the clock, vk_errorf — is defined here. scripts/run-host-tests.sh
 * builds this suite only when mesa/ and the generated headers of a
 * configured NVK build are present, and says so when they are not.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nvkmd_horizon.h"
#include "hostfw.h"

/* ------------------------------------------------------------------ */
/* The simulated syncpoint                                             */
/* ------------------------------------------------------------------ */

/* One counter, one channel. `reached` is the syncpoint's value; a wait
 * blocks until it is at or past the threshold. `waiters_inside` counts
 * threads parked in the blocking wait so the test can act only once
 * the waiter has committed to its snapshot. */
static pthread_mutex_t gpu_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gpu_cond = PTHREAD_COND_INITIALIZER;
static uint32_t gpu_reached;
static uint32_t gpu_waiters_inside;

static horizon_gpu_channel *const fake_chan = (horizon_gpu_channel *)0x1;

#define SYNCPT_ID 7u

static void gpu_complete(uint32_t threshold)
{
    pthread_mutex_lock(&gpu_mutex);
    if ((int32_t)(threshold - gpu_reached) > 0)
        gpu_reached = threshold;
    pthread_cond_broadcast(&gpu_cond);
    pthread_mutex_unlock(&gpu_mutex);
}

static void gpu_wait_for_waiter(void)
{
    pthread_mutex_lock(&gpu_mutex);
    while (gpu_waiters_inside == 0)
        pthread_cond_wait(&gpu_cond, &gpu_mutex);
    pthread_mutex_unlock(&gpu_mutex);
}

static void gpu_reset(void)
{
    pthread_mutex_lock(&gpu_mutex);
    gpu_reached = 0;
    gpu_waiters_inside = 0;
    pthread_mutex_unlock(&gpu_mutex);
}

horizon_gpu_result
horizon_gpu_channel_wait_fence(horizon_gpu_channel *chan,
                               horizon_gpu_fence fence, uint64_t timeout_ns)
{
    if (chan != fake_chan || fence.syncpt_id != SYNCPT_ID)
        return horizon_gpu_err(HORIZON_GPU_ERR_INVALID_ARG);

    pthread_mutex_lock(&gpu_mutex);
    if (timeout_ns == 0) {
        /* A poll, as get_value() uses it. */
        bool ok = horizon_gpu_syncpt_reached(gpu_reached, fence.threshold);
        pthread_mutex_unlock(&gpu_mutex);
        return ok ? horizon_gpu_ok() : horizon_gpu_err(HORIZON_GPU_ERR_TIMEOUT);
    }
    gpu_waiters_inside++;
    pthread_cond_broadcast(&gpu_cond);
    while (!horizon_gpu_syncpt_reached(gpu_reached, fence.threshold))
        pthread_cond_wait(&gpu_cond, &gpu_mutex);
    gpu_waiters_inside--;
    pthread_mutex_unlock(&gpu_mutex);
    return horizon_gpu_ok();
}

/* ------------------------------------------------------------------ */
/* Everything else the translation unit reaches for                    */
/* ------------------------------------------------------------------ */

horizon_gpu_result horizon_gpu_fence_wait(horizon_gpu_device *dev,
                                          horizon_gpu_fence fence,
                                          uint64_t timeout_ns)
{
    (void)dev; (void)fence; (void)timeout_ns;
    return horizon_gpu_err(HORIZON_GPU_ERR_UNSUPPORTED);
}

void nvkmd_horizon_meter_fence_wait(struct nvkmd_horizon_meter *m,
                                    uint64_t total_ns, uint64_t before_ns,
                                    uint64_t age_ns)
{
    (void)m; (void)total_ns; (void)before_ns; (void)age_ns;
}

VkResult nvkmd_horizon_result(horizon_gpu_result res)
{
    (void)res;
    return VK_ERROR_DEVICE_LOST;
}

int64_t os_time_get_nano(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* Mesa's c11 headers route timespec_get here; answer it from the clock
 * rather than through the name, which is this function again. */
int c23_timespec_get(struct timespec *ts, int base)
{
    if (base != TIME_UTC || clock_gettime(CLOCK_REALTIME, ts) != 0)
        return 0;
    return base;
}

VkResult vk_sync_wait(struct vk_device *device, struct vk_sync *sync,
                      uint64_t wait_value, enum vk_sync_wait_flags wait_flags,
                      uint64_t abs_timeout_ns)
{
    (void)device; (void)sync; (void)wait_value; (void)wait_flags;
    (void)abs_timeout_ns;
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

VkResult __vk_errorf(const void *_obj, VkResult error, const char *file,
                     int line, const char *format, ...)
{
    (void)_obj; (void)file; (void)line; (void)format;
    return error;
}

/* ------------------------------------------------------------------ */
/* The waiter thread                                                   */
/* ------------------------------------------------------------------ */

typedef struct waiter {
    struct vk_sync *sync;
    uint64_t wait_value;
    VkResult result;
} waiter;

static void *waiter_main(void *arg)
{
    waiter *w = arg;
    w->result = nvk_horizon_sync_type.wait(NULL, w->sync, w->wait_value,
                                           VK_SYNC_WAIT_COMPLETE,
                                           UINT64_MAX);
    return NULL;
}

static struct nvk_horizon_sync *sync_new(void)
{
    struct nvk_horizon_sync *s = calloc(1, sizeof(*s));
    if (s == NULL)
        return NULL;
    s->base.type = &nvk_horizon_sync_type;
    s->base.flags = VK_SYNC_IS_TIMELINE;
    if (nvk_horizon_sync_type.init(NULL, &s->base, 0) != VK_SUCCESS) {
        free(s);
        return NULL;
    }
    return s;
}

static void sync_free(struct nvk_horizon_sync *s)
{
    nvk_horizon_sync_type.finish(NULL, &s->base);
    free(s);
}

static uint64_t value_of(struct nvk_horizon_sync *s)
{
    uint64_t v = ~UINT64_C(0);
    if (nvk_horizon_sync_type.get_value(NULL, &s->base, &v) != VK_SUCCESS)
        return ~UINT64_C(0);
    return v;
}

int main(void)
{
    const horizon_gpu_fence f1 = { .syncpt_id = SYNCPT_ID, .threshold = 1 };
    const horizon_gpu_fence f2 = { .syncpt_id = SYNCPT_ID, .threshold = 2 };

    /* ---------------------------------------------------------------
     * A. Nothing interferes. The control: a wait on F1 completes the
     * object with the value F1 carried.
     * --------------------------------------------------------------- */
    {
        gpu_reset();
        struct nvk_horizon_sync *s = sync_new();
        H_CHECK(s != NULL, "A: a sync initialises");
        nvk_horizon_sync_set_fence(&s->base, fake_chan, NULL, f1, 1);
        H_CHECK(s->state == NVK_HORIZON_SYNC_PENDING, "A: PENDING on F1");

        waiter w = { .sync = &s->base, .wait_value = 1 };
        pthread_t th;
        H_CHECK(pthread_create(&th, NULL, waiter_main, &w) == 0,
                "A: the waiter starts");
        gpu_wait_for_waiter();
        gpu_complete(1);
        pthread_join(th, NULL);

        H_CHECK(w.result == VK_SUCCESS, "A: the wait returned success");
        H_CHECK(s->state == NVK_HORIZON_SYNC_SIGNALLED, "A: SIGNALLED");
        H_CHECK(s->passed_value == 1, "A: passed_value is F1's value");
        H_CHECK(value_of(s) == 1, "A: get_value() says 1");
        sync_free(s);
    }

    /* ---------------------------------------------------------------
     * B. The interleaving 0081 exists for. The waiter is parked inside
     * the syncpoint wait for F1; a second submit attaches F2 with value
     * 2; then F1 alone completes. The wait returns — F1 is done — but
     * the object must stay PENDING on F2, and no reader may be told
     * that 2 has passed while the counter sits at 1. Before 0081 the
     * wait marked the object SIGNALLED with the re-read value 2.
     * --------------------------------------------------------------- */
    {
        gpu_reset();
        struct nvk_horizon_sync *s = sync_new();
        H_CHECK(s != NULL, "B: a sync initialises");
        nvk_horizon_sync_set_fence(&s->base, fake_chan, NULL, f1, 1);

        waiter w = { .sync = &s->base, .wait_value = 1 };
        pthread_t th;
        H_CHECK(pthread_create(&th, NULL, waiter_main, &w) == 0,
                "B: the waiter starts");
        gpu_wait_for_waiter();

        /* The second submit, while the waiter holds its snapshot. */
        nvk_horizon_sync_set_fence(&s->base, fake_chan, NULL, f2, 2);
        H_CHECK(value_of(s) == 0, "B: nothing has passed yet");

        gpu_complete(1);
        pthread_join(th, NULL);

        H_CHECK(w.result == VK_SUCCESS, "B: the wait for F1 returned");
        H_CHECK(s->state == NVK_HORIZON_SYNC_PENDING,
                "B: the object is still PENDING, on F2");
        H_CHECK(s->fence.threshold == 2, "B: and the fence it holds is F2");
        H_CHECK(s->passed_value == 1,
                "B: passed_value is 1 — what the wait established");
        H_CHECK(value_of(s) == 1,
                "B: get_value() polls F2, finds it unreached, says 1");
        H_CHECK(s->state == NVK_HORIZON_SYNC_PENDING,
                "B: the poll left it PENDING");

        gpu_complete(2);
        H_CHECK(value_of(s) == 2, "B: once F2 completes, get_value() says 2");
        H_CHECK(s->state == NVK_HORIZON_SYNC_SIGNALLED,
                "B: and the object is SIGNALLED");
        sync_free(s);
    }

    /* ---------------------------------------------------------------
     * C. A CPU signal during the wait is the case the re-read used to
     * serve, and it must still be served: the object is SIGNALLED by
     * the signal itself, with the signal's value, and the wait's return
     * must not undo any of that.
     * --------------------------------------------------------------- */
    {
        gpu_reset();
        struct nvk_horizon_sync *s = sync_new();
        H_CHECK(s != NULL, "C: a sync initialises");
        nvk_horizon_sync_set_fence(&s->base, fake_chan, NULL, f1, 1);

        waiter w = { .sync = &s->base, .wait_value = 1 };
        pthread_t th;
        H_CHECK(pthread_create(&th, NULL, waiter_main, &w) == 0,
                "C: the waiter starts");
        gpu_wait_for_waiter();

        H_CHECK(nvk_horizon_sync_type.signal(NULL, &s->base, 3) == VK_SUCCESS,
                "C: a CPU signal with value 3 while the wait is blocked");
        gpu_complete(1);
        pthread_join(th, NULL);

        H_CHECK(w.result == VK_SUCCESS, "C: the wait returned");
        H_CHECK(s->state == NVK_HORIZON_SYNC_SIGNALLED,
                "C: SIGNALLED, as the signal made it");
        H_CHECK(s->passed_value == 3, "C: passed_value is the signal's 3");
        H_CHECK(value_of(s) == 3, "C: get_value() says 3");
        sync_free(s);
    }

    /* ---------------------------------------------------------------
     * D. The wait that finds the work already done. A poll before the
     * wait (get_value) moves the object to SIGNALLED; the wait must
     * return at once and change nothing.
     * --------------------------------------------------------------- */
    {
        gpu_reset();
        struct nvk_horizon_sync *s = sync_new();
        H_CHECK(s != NULL, "D: a sync initialises");
        nvk_horizon_sync_set_fence(&s->base, fake_chan, NULL, f2, 2);
        gpu_complete(2);
        H_CHECK(value_of(s) == 2, "D: the poll sees F2 reached");
        H_CHECK(nvk_horizon_sync_type.wait(NULL, &s->base, 2,
                                           VK_SYNC_WAIT_COMPLETE, 0)
                    == VK_SUCCESS,
                "D: a wait with no time to spare succeeds anyway");
        H_CHECK(s->passed_value == 2, "D: passed_value stays 2");
        sync_free(s);
    }

    return h_summary("h_nvkmd_sync");
}
