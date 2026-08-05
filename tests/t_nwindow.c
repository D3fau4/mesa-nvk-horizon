/*
 * Phase 6 test — t_nwindow: the compositor, with no Vulkan in the way.
 *
 * WHY THIS EXISTS AND WHY IT COMES FIRST. Phase 6 puts a VK_KHR_swapchain
 * on top of Horizon's VI compositor. Between the two sit four questions
 * that Vulkan cannot answer and that would otherwise be answered for the
 * first time inside a swapchain, where every failure is ambiguous:
 *
 *   1. Can this project register its own scanout buffers on the default
 *      NWindow — an NvMap of our own, an NvGraphicBuffer we filled in —
 *      rather than going through libnx's framebufferCreate, which owns
 *      the window's slots itself and would leave a Vulkan swapchain
 *      nothing to own?
 *
 *   2. HOW MANY SLOTS CAN BE DEQUEUED AT ONCE? This is the load-bearing
 *      one. libnx's nwindowDequeueBuffer keeps a single `cur_slot` and
 *      refuses a second dequeue before the first is queued or cancelled
 *      (native_window.c: `if (!nw->slots_configured || nw->cur_slot >= 0)`).
 *      Vulkan does not allow that restriction: with
 *      VkSurfaceCapabilitiesKHR::minImageCount = m and n images, an
 *      application may hold n - m + 1 images acquired, and blocking
 *      indefinitely for them is *valid usage*. Offering m = 2 so double
 *      buffering exists at all therefore makes two acquired images legal
 *      on a three-image swapchain. If the BufferQueue underneath libnx
 *      hands out two slots at once, the WSI can drive the producer API
 *      directly and track ownership itself; if it cannot, the buffer
 *      count design in docs/wsi.md § 2.3 has to change. A number is
 *      measured here rather than assumed anywhere.
 *
 *   3. Does anything actually reach the compositor, and at what rate?
 *      "No errors" is not evidence. Three independent things are
 *      recorded instead, none of which this process can produce on its
 *      own: the release fence a dequeue comes back with (a syncpoint
 *      *another* process signals), the consumer's pending-buffer count
 *      reported by every queue, and the frame cadence — which can only
 *      lock to the display's refresh if something is consuming at that
 *      rate.
 *
 *   4. Is double buffering measurably different from triple? Phase 6's
 *      second exit criterion asks for that in numbers. With nothing to
 *      render both simply track the display, so the difference is made
 *      visible the way it appears in a real application: a load that
 *      alternates between cheap and expensive frames, averaging under
 *      one refresh but peaking over it. Two buffers must round every
 *      overrun up to a whole extra refresh; three can absorb it. The
 *      load is derived from the measured cost of a frame rather than
 *      guessed — see nw_choose_load().
 *
 * WHAT IT DRAWS. Solid colours, rotating. Solid is not laziness: a
 * single repeating 32-bit pixel is the same image under any block-linear
 * swizzle, so the buffer can be filled with plain stores and the test
 * needs no swizzle of its own to put a *correct* image on screen. The
 * operator sees the colour sweep, which is the fourth, human piece of
 * evidence.
 *
 * GEOMETRY. Every constant below is libnx's framebufferCreate, which is
 * the one known-working producer on this platform, re-derived here
 * rather than copied: 16Bx2 sector ordering, block height 16 GOBs,
 * 64-byte wide GOBs, an NvMap of NvKind_Pitch aligned to 0x20000 with
 * the layout described per surface instead. The WSI backend in
 * mesa-patches/ builds the same structure from the same facts; the two
 * are deliberately separate code, because one lives in this repository
 * and the other in a patch against Mesa.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include "horizon_gpu/device.h"
#include "horizon_gpu/memory.h"
#include "common/testfw.h"

const char *const test_name = "t_nwindow";

/* This test owns the display: no console, and the SD-card log is the
 * whole record (testfw.h). */
const bool test_uses_display = true;

/* ------------------------------------------------------------------ */
/* Geometry — every number with the source that fixes it.              */
/* ------------------------------------------------------------------ */

/* PIXEL_FORMAT_RGBA_8888 <-> NvColorFormat_A8B8G8R8. libnx's
 * g_nvColorFmtTable (framebuffer.c) is the mapping; the low byte of an
 * NvColorFormat encodes the bit depth, so bytes-per-pixel is
 * ((fmt >> 3) & 0x1f) = 4 here. Spelled out rather than computed,
 * because this test offers exactly one format. */
#define NW_PIXEL_FORMAT   PIXEL_FORMAT_RGBA_8888
#define NW_COLOR_FORMAT   NvColorFormat_A8B8G8R8
#define NW_BYTES_PER_PIXEL 4u

/* GOBs are 64 bytes wide and 8 rows tall (Tegra X1 TRM ch. 20.1); a
 * block is 2^block_height_log2 GOBs tall. 4 is what the TRM calls the
 * optimal value (SIXTEEN_GOBS) and what libnx uses. */
#define NW_GOB_WIDTH_B        64u
#define NW_GOB_HEIGHT_ROWS    8u
#define NW_BLOCK_HEIGHT_LOG2  4u

/* nvmap alignment for a buffer the display block will scan out. 0x20000
 * is what libnx passes to nvMapCreate for its framebuffers. */
#define NW_NVMAP_ALIGN    UINT64_C(0x20000)

/* Marshalling constants of NvGraphicBuffer, from libnx's
 * framebufferCreate. `magic` and `pid` are what the compositor expects
 * to find; they are not ours to choose. */
#define NW_GB_MAGIC       UINT32_C(0xDAFFCAFF)
#define NW_GB_PID         UINT32_C(42)
#define NW_GB_USAGE       (GRALLOC_USAGE_HW_COMPOSER | \
                           GRALLOC_USAGE_HW_RENDER | \
                           GRALLOC_USAGE_HW_TEXTURE)

/* Slots this test will ever register. nwindowConfigureBuffer accepts up
 * to 64; four is what docs/wsi.md § 2.3 caps a swapchain at. */
#define NW_MAX_BUFFERS    4u

/* How long a single dequeue may take before the test calls it a
 * failure. At 60 Hz a buffer comes back in ~16.7 ms; a second is 60
 * refreshes, so this is a hang detector and not a deadline. */
#define NW_DEQUEUE_TIMEOUT_NS  UINT64_C(1000000000)

/* Frames per measured session. 90 at 60 Hz is a second and a half —
 * long enough that a mean is not noise, short enough that five sessions
 * plus teardown stay inside ten seconds of the operator's time. */
#define NW_FRAMES         90u

/* The display's nominal refresh. This is the expectation a measurement
 * is compared against, never an assumption: every interval is reported
 * whatever it turns out to be. */
#define NW_REFRESH_HZ     60u
#define NW_REFRESH_NS     (UINT64_C(1000000000) / NW_REFRESH_HZ)

/* ------------------------------------------------------------------ */

typedef struct nw_geometry {
    uint32_t width;             /* as configured, in pixels            */
    uint32_t height;
    uint32_t stride_px;         /* width rounded out to a whole GOB     */
    uint32_t pitch_B;           /* stride_px * bytes-per-pixel          */
    uint32_t height_aligned;    /* height rounded out to a whole block  */
    uint32_t buffer_size_B;     /* one buffer                           */
} nw_geometry;

/* Rounds `v` up to a multiple of `to` (a power of two), reporting
 * overflow rather than wrapping (CLAUDE.md: overflow-check every size
 * computation). */
static bool nw_align_up_u32(uint32_t v, uint32_t to, uint32_t *out)
{
    if (to == 0 || (to & (to - 1)) != 0)
        return false;
    if (v > UINT32_MAX - (to - 1))
        return false;
    *out = (v + (to - 1)) & ~(to - 1);
    return true;
}

static bool nw_geometry_init(nw_geometry *g, uint32_t width, uint32_t height)
{
    memset(g, 0, sizeof(*g));

    if (width == 0 || height == 0)
        return false;

    /* width * bpp only overflows for a width nobody will ask for; the
     * check is what makes every product below it safe. */
    if (width > UINT32_MAX / NW_BYTES_PER_PIXEL)
        return false;

    uint32_t pitch_B;
    if (!nw_align_up_u32(width * NW_BYTES_PER_PIXEL, NW_GOB_WIDTH_B, &pitch_B))
        return false;

    const uint32_t block_height_rows =
        NW_GOB_HEIGHT_ROWS << NW_BLOCK_HEIGHT_LOG2;
    uint32_t height_aligned;
    if (!nw_align_up_u32(height, block_height_rows, &height_aligned))
        return false;

    if (pitch_B == 0 || height_aligned > UINT32_MAX / pitch_B)
        return false;

    g->width = width;
    g->height = height;
    g->pitch_B = pitch_B;
    g->stride_px = pitch_B / NW_BYTES_PER_PIXEL;
    g->height_aligned = height_aligned;
    g->buffer_size_B = pitch_B * height_aligned;
    return true;
}

/* Fills in the NvGraphicBuffer for one slot. `offset_B` is where this
 * slot's image starts inside the single NvMap all of them share. */
static void nw_graphic_buffer_init(NvGraphicBuffer *gb, const nw_geometry *g,
                                   uint32_t nvmap_id, uint32_t offset_B)
{
    memset(gb, 0, sizeof(*gb));

    /* THE FIELD THAT DECIDES WHETHER ANY OF THIS WORKS. Left at zero,
     * bqSetPreallocatedBuffer returns success and marshals an empty
     * buffer, and the first dequeue then blocks forever
     * (docs/reference-analysis.md § 1, hardware-confirmed by the
     * reference). num_fds stays 0: the handle carries no file
     * descriptors, only the ints that follow the header. */
    gb->header.num_ints =
        (int)((sizeof(NvGraphicBuffer) - sizeof(NativeHandle)) / 4);
    gb->header.num_fds = 0;

    gb->unk0 = -1;
    gb->magic = NW_GB_MAGIC;
    gb->pid = NW_GB_PID;
    gb->usage = NW_GB_USAGE;
    gb->format = NW_PIXEL_FORMAT;
    gb->ext_format = NW_PIXEL_FORMAT;
    gb->num_planes = 1;

    gb->nvmap_id = (s32)nvmap_id;
    gb->stride = g->stride_px;      /* in pixels, per the header */
    gb->total_size = g->buffer_size_B;

    gb->planes[0].width = g->width;
    gb->planes[0].height = g->height;
    gb->planes[0].color_format = NW_COLOR_FORMAT;
    gb->planes[0].layout = NvLayout_BlockLinear;
    gb->planes[0].kind = NvKind_Generic_16BX2;
    gb->planes[0].block_height_log2 = NW_BLOCK_HEIGHT_LOG2;
    gb->planes[0].pitch = g->pitch_B;
    gb->planes[0].offset = offset_B;
    gb->planes[0].size = g->buffer_size_B;
}

/* ------------------------------------------------------------------ */
/* The producer, driven one level below NWindow.                       */
/*                                                                     */
/* bqDequeueBuffer / bqQueueBuffer / bqCancelBuffer are libnx's public  */
/* IGraphicBufferProducer API and nw->bq is a public field of NWindow.  */
/* Only *reads* of NWindow's own bookkeeping happen here — width,       */
/* height, format, usage, crop, transform, the release event — and      */
/* nw->cur_slot is neither read nor written, so libnx's single-slot     */
/* invariant is not violated, it is simply not used. Registration and   */
/* teardown still go through nwindowConfigureBuffer and                 */
/* nwindowReleaseBuffers, which are the calls that own the window's     */
/* `slots_configured` and its connection.                              */
/* ------------------------------------------------------------------ */

typedef struct nw_producer {
    NWindow *win;
    uint32_t slots_requested;   /* bitmask: bqRequestBuffer already done */
} nw_producer;

/* One dequeued slot and the fence the compositor released it with. */
typedef struct nw_slot {
    s32 slot;
    NvMultiFence fence;
} nw_slot;

/* Non-blocking dequeue. Returns the raw Result so a caller can tell "no
 * buffer available yet" from a genuine failure — the distinction the
 * reference discards. */
static Result nw_try_dequeue(nw_producer *p, nw_slot *out)
{
    s32 slot = -1;
    NvMultiFence fence;
    memset(&fence, 0, sizeof(fence));

    Result rc = bqDequeueBuffer(&p->win->bq, true, p->win->width,
                                p->win->height, (s32)p->win->format,
                                p->win->usage, &slot, &fence);
    if (R_FAILED(rc))
        return rc;

    if (slot < 0 || slot >= (s32)NW_MAX_BUFFERS) {
        /* Out of range: hand it straight back rather than index with
         * it. The cancel's own result cannot be reported through this
         * signature, so it is folded into the failure the caller sees. */
        Result crc = bqCancelBuffer(&p->win->bq, slot, &fence);
        if (R_FAILED(crc))
            return crc;
        return MAKERESULT(Module_Libnx, LibnxError_BadGfxDequeueBuffer);
    }

    /* A slot is "requested" once, the first time it is dequeued; that is
     * what libnx does and what Android's producer contract asks for. */
    if (!(p->slots_requested & (1u << (uint32_t)slot))) {
        rc = bqRequestBuffer(&p->win->bq, slot, NULL);
        if (R_FAILED(rc)) {
            bqCancelBuffer(&p->win->bq, slot, &fence);
            return rc;
        }
        p->slots_requested |= 1u << (uint32_t)slot;
    }

    out->slot = slot;
    out->fence = fence;
    return 0;
}

/* Dequeue, waiting on the BufferQueue's release event until
 * `timeout_ns` elapses. The non-blocking attempt comes FIRST: at the
 * start of a session every buffer is free and nothing has been
 * released, so an event wait ahead of the first try would block for a
 * release that has no reason to happen. */
/* Which dequeue failures mean "come back when a buffer is free".
 *
 * MEASURED, AND THE FIRST VERSION OF THIS TEST WAS WRONG ABOUT IT. Only
 * WouldBlock was treated as retryable, because that is what libnx's own
 * loop in nwindowDequeueBuffer expects. On hardware, two registered
 * buffers with both of them queued gives 0x115d — LibnxBinderError_NoInit,
 * which is Android's NO_INIT (-ENODEV) through binderConvertErrorCode —
 * and every two-buffer session failed at frame 2
 * (docs/hw-logs/t_nwindow-run1-*.log). Three buffers never reached the
 * condition and never saw it.
 *
 * InvalidOperation is included for the same reason without having been
 * observed: Android's producer returns it when a dequeue would exceed
 * the allowance, which is the same condition under a different name.
 */
static bool nw_dequeue_would_block(Result rc)
{
    return R_VALUE(rc) ==
               MAKERESULT(Module_LibnxBinder, LibnxBinderError_WouldBlock) ||
           R_VALUE(rc) ==
               MAKERESULT(Module_LibnxBinder, LibnxBinderError_NoInit) ||
           R_VALUE(rc) ==
               MAKERESULT(Module_LibnxBinder,
                          LibnxBinderError_InvalidOperation);
}

static Result nw_dequeue(nw_producer *p, uint64_t timeout_ns, nw_slot *out)
{
    const u64 start = armGetSystemTick();

    for (;;) {
        Result rc = nw_try_dequeue(p, out);
        if (!nw_dequeue_would_block(rc))
            return rc;

        if (!eventActive(&p->win->event))
            return rc;  /* nothing to wait on; report WouldBlock */

        const u64 spent_ns = armTicksToNs(armGetSystemTick() - start);
        if (spent_ns >= timeout_ns)
            return MAKERESULT(Module_Libnx, LibnxError_Timeout);

        Result erc = eventWait(&p->win->event, timeout_ns - spent_ns);
        if (R_FAILED(erc))
            return erc;
    }
}

static Result nw_queue(nw_producer *p, s32 slot, uint32_t swap_interval,
                       const NvMultiFence *fence, BqBufferOutput *out)
{
    BqBufferInput in;
    memset(&in, 0, sizeof(in));
    in.crop = p->win->crop;
    in.scalingMode = p->win->scaling_mode;
    in.transform = p->win->transform;
    in.stickyTransform = p->win->sticky_transform;
    in.swapInterval = swap_interval;
    if (fence != NULL)
        in.fence = *fence;

    return bqQueueBuffer(&p->win->bq, slot, &in, out);
}

/* ------------------------------------------------------------------ */
/* One measured session.                                               */
/* ------------------------------------------------------------------ */

typedef struct nw_stats {
    const char *what;

    uint32_t frames_presented;
    /* Intervals between consecutive presents; there is one fewer of
     * these than there are frames. */
    uint32_t intervals;
    uint64_t total_ns;
    uint64_t min_ns;
    uint64_t max_ns;
    uint32_t within_10pct;      /* intervals within +/-10% of a refresh  */
    uint32_t over_1p5_refresh;  /* intervals a whole extra refresh long  */

    uint64_t dequeue_total_ns;
    uint64_t dequeue_max_ns;
    uint64_t fill_total_ns;     /* fill + cache flush, the frame's cost  */

    uint32_t max_pending;       /* largest numPendingBuffers seen        */
    uint32_t release_fences;    /* dequeues that carried >= 1 fence      */
    uint32_t first_fence_syncpt;
    bool     first_fence_seen;

    /* First failure, if any. Recorded rather than asserted per frame:
     * 90 frames of passing checks would bury the one line that matters,
     * and a loop that aborts on the first failure makes every later
     * count unfalsifiable. */
    bool     failed;
    uint32_t fail_frame;
    Result   fail_rc;
    const char *fail_what;
    /* A failure that happens with a slot dequeued has to hand it back,
     * or the next session starts a buffer short. The cancel's own
     * result is kept and reported rather than discarded — a cleanup
     * that silently fails is how a first failure becomes three. */
    bool     fail_cancelled;
    Result   fail_cancel_rc;
} nw_stats;

static void nw_stats_init(nw_stats *s, const char *what)
{
    memset(s, 0, sizeof(*s));
    s->what = what;
    s->min_ns = UINT64_MAX;
}

static void nw_fail(nw_stats *s, uint32_t frame, Result rc, const char *what)
{
    if (s->failed)
        return;
    s->failed = true;
    s->fail_frame = frame;
    s->fail_rc = rc;
    s->fail_what = what;
}

/* Hands a dequeued slot back after a failure, keeping the result. */
static void nw_fail_cancel(nw_stats *s, nw_producer *p, const nw_slot *held)
{
    s->fail_cancelled = true;
    s->fail_cancel_rc = bqCancelBuffer(&p->win->bq, held->slot, &held->fence);
}

/* A visible colour that changes every frame: three phase-shifted ramps,
 * so the screen sweeps through hues rather than flickering. Packed as
 * PIXEL_FORMAT_RGBA_8888 wants it — red in the low byte. */
static uint32_t nw_frame_colour(uint32_t frame)
{
    const uint32_t r = (frame * 3u) & 0xffu;
    const uint32_t g = (frame * 5u + 85u) & 0xffu;
    const uint32_t b = (frame * 7u + 170u) & 0xffu;
    return r | (g << 8) | (b << 16) | 0xff000000u;
}

/* Fills one buffer with a single repeating pixel. Correct under any
 * block-linear swizzle precisely because every pixel is the same. */
static void nw_fill(void *base, uint32_t offset_B, uint32_t size_B,
                    uint32_t pixel)
{
    uint32_t *p = (uint32_t *)((char *)base + offset_B);
    const uint32_t words = size_B / 4u;
    for (uint32_t i = 0; i < words; i++)
        p[i] = pixel;
}

/* Burns `ns` of CPU. Reads the tick counter rather than looping a fixed
 * number of times, so the load is a duration and not a guess about how
 * fast this core is. `volatile` keeps the loop from being optimised
 * away — it has no other effect. */
static void nw_busy(uint64_t ns)
{
    if (ns == 0)
        return;
    const u64 start = armGetSystemTick();
    volatile uint32_t sink = 0;
    while (armTicksToNs(armGetSystemTick() - start) < ns)
        sink++;
    (void)sink;
}

typedef struct nw_session {
    test_ctx *t;
    nw_producer prod;
    horizon_gpu_mem *mem;
    void *cpu;
    nw_geometry geom;
    uint32_t num_buffers;
    uint32_t swap_interval;
    /* Extra CPU work on odd frames, to make a bursty producer. Zero for
     * the sessions that measure the display's own cadence. */
    uint64_t busy_ns_odd;
} nw_session;

/* Registers `num_buffers` slots. On any failure every slot already
 * registered is released before returning, so the window is never left
 * half-configured (docs/wsi.md § 5). */
static bool nw_session_register(nw_session *s)
{
    Result rc = nwindowSetDimensions(s->prod.win, s->geom.width,
                                     s->geom.height);
    if (!t_check(s->t, R_SUCCEEDED(rc),
                 "%" PRIu32 " buffers: nwindowSetDimensions(%" PRIu32
                 ", %" PRIu32 ") -> 0x%08x",
                 s->num_buffers, s->geom.width, s->geom.height, rc))
        return false;

    const uint32_t nvmap_id = horizon_gpu_mem_get_id(s->mem);

    for (uint32_t i = 0; i < s->num_buffers; i++) {
        NvGraphicBuffer gb;
        nw_graphic_buffer_init(&gb, &s->geom, nvmap_id,
                               i * s->geom.buffer_size_B);

        rc = nwindowConfigureBuffer(s->prod.win, (s32)i, &gb);
        if (!t_check(s->t, R_SUCCEEDED(rc),
                     "%" PRIu32 " buffers: slot %" PRIu32
                     " registered at offset 0x%" PRIx32 " -> 0x%08x",
                     s->num_buffers, i, i * s->geom.buffer_size_B, rc)) {
            /* Unwind: what is registered comes off before we leave. */
            Result rrc = nwindowReleaseBuffers(s->prod.win);
            t_note(s->t, "unwound %" PRIu32 " registered slot(s) after the "
                         "failure: nwindowReleaseBuffers -> 0x%08x", i, rrc);
            return false;
        }
    }

    s->prod.slots_requested = 0;
    return true;
}

/* Measures how many slots the BufferQueue will hand out at once, then
 * hands them all back. Question 2 of this file's header, as a number. */
static uint32_t nw_measure_concurrent_dequeues(nw_session *s)
{
    nw_slot held[NW_MAX_BUFFERS];
    uint32_t n = 0;
    uint32_t seen_mask = 0;
    bool distinct = true;

    while (n < NW_MAX_BUFFERS) {
        nw_slot got;
        Result rc = nw_try_dequeue(&s->prod, &got);
        if (R_FAILED(rc)) {
            /* Report anything that is not simply "no buffer free": a
             * measurement that stops for an unrelated failure would
             * report a smaller number as though it were the limit. */
            t_check(s->t, nw_dequeue_would_block(rc),
                    "%" PRIu32 " buffers: the dequeue that ended the count "
                    "said no buffer was free rather than failing "
                    "(0x%08x)", s->num_buffers, rc);
            break;
        }
        if (seen_mask & (1u << (uint32_t)got.slot))
            distinct = false;
        seen_mask |= 1u << (uint32_t)got.slot;
        held[n++] = got;
    }

    /* Vacuous when n < 2, and said so rather than left to look like a
     * result: with one slot held there is nothing for it to collide
     * with. The count itself is what the caller asserts on. */
    t_check(s->t, distinct,
            "%" PRIu32 " buffers: the %" PRIu32 " concurrently dequeued "
            "slot(s) are all different (mask 0x%" PRIx32 ")",
            s->num_buffers, n, seen_mask);

    for (uint32_t i = 0; i < n; i++) {
        Result rc = bqCancelBuffer(&s->prod.win->bq, held[i].slot,
                                   &held[i].fence);
        t_check(s->t, R_SUCCEEDED(rc),
                "%" PRIu32 " buffers: slot %" PRId32 " cancelled back to the "
                "compositor -> 0x%08x", s->num_buffers, held[i].slot, rc);
    }

    return n;
}

static void nw_session_present(nw_session *s, nw_stats *st)
{
    u64 prev_tick = 0;
    bool have_prev = false;

    for (uint32_t frame = 0; frame < NW_FRAMES; frame++) {
        const u64 deq_start = armGetSystemTick();

        nw_slot got;
        Result rc = nw_dequeue(&s->prod, NW_DEQUEUE_TIMEOUT_NS, &got);
        if (R_FAILED(rc)) {
            nw_fail(st, frame, rc, "dequeue");
            return;
        }

        const uint64_t deq_ns = armTicksToNs(armGetSystemTick() - deq_start);
        st->dequeue_total_ns += deq_ns;
        if (deq_ns > st->dequeue_max_ns)
            st->dequeue_max_ns = deq_ns;

        if (got.fence.num_fences > 0) {
            st->release_fences++;
            if (!st->first_fence_seen) {
                st->first_fence_seen = true;
                st->first_fence_syncpt = got.fence.fences[0].id;
            }
            /* The CPU is about to write this buffer, so the compositor
             * must have finished reading it. This wait is required by
             * what follows it, not a blanket stall, and its result is
             * checked — the reference discards it, which turns a
             * timeout into "go ahead and overwrite the buffer that is
             * still being scanned out". */
            Result frc = nvMultiFenceWait(&got.fence, 1000000 /* us */);
            if (R_FAILED(frc)) {
                nw_fail(st, frame, frc, "release fence wait");
                nw_fail_cancel(st, &s->prod, &got);
                return;
            }
        }

        const u64 fill_start = armGetSystemTick();
        const uint32_t offset_B = (uint32_t)got.slot * s->geom.buffer_size_B;
        nw_fill(s->cpu, offset_B, s->geom.buffer_size_B,
                nw_frame_colour(frame));

        horizon_gpu_result mres =
            horizon_gpu_mem_flush(s->mem, offset_B, s->geom.buffer_size_B);
        if (mres.status != HORIZON_GPU_OK) {
            nw_fail(st, frame, mres.nv, "cache flush");
            nw_fail_cancel(st, &s->prod, &got);
            return;
        }
        st->fill_total_ns += armTicksToNs(armGetSystemTick() - fill_start);

        if ((frame & 1u) != 0)
            nw_busy(s->busy_ns_odd);

        BqBufferOutput out;
        memset(&out, 0, sizeof(out));
        rc = nw_queue(&s->prod, got.slot, s->swap_interval, NULL, &out);
        if (R_FAILED(rc)) {
            nw_fail(st, frame, rc, "queue");
            nw_fail_cancel(st, &s->prod, &got);
            return;
        }
        st->frames_presented++;

        if (out.numPendingBuffers > st->max_pending)
            st->max_pending = out.numPendingBuffers;

        const u64 now = armGetSystemTick();
        if (have_prev) {
            const uint64_t dt = armTicksToNs(now - prev_tick);
            st->total_ns += dt;
            if (dt < st->min_ns)
                st->min_ns = dt;
            if (dt > st->max_ns)
                st->max_ns = dt;
            if (dt >= NW_REFRESH_NS - NW_REFRESH_NS / 10 &&
                dt <= NW_REFRESH_NS + NW_REFRESH_NS / 10)
                st->within_10pct++;
            if (dt > NW_REFRESH_NS + NW_REFRESH_NS / 2)
                st->over_1p5_refresh++;
            st->intervals++;
        }
        prev_tick = now;
        have_prev = true;
    }
}

static uint64_t nw_mean_ns(const nw_stats *st)
{
    return st->intervals ? st->total_ns / st->intervals : 0;
}

/* One check that the session ran at all, then the numbers as notes. The
 * check is falsifiable because the loop records its failure instead of
 * aborting the test. */
static bool nw_report(test_ctx *t, const nw_stats *st)
{
    const bool ok = t_check(t, !st->failed && st->frames_presented == NW_FRAMES,
                            "%s: %" PRIu32 " of %u frames presented%s",
                            st->what, st->frames_presented, NW_FRAMES,
                            st->failed ? " (see the note below)" : "");
    if (st->failed) {
        t_note(t, "%s: %s failed at frame %" PRIu32 " -> 0x%08x",
               st->what, st->fail_what, st->fail_frame, st->fail_rc);
        if (st->fail_cancelled)
            t_note(t, "%s: the dequeued slot was cancelled back -> 0x%08x",
                   st->what, st->fail_cancel_rc);
    }

    if (st->intervals == 0) {
        t_note(t, "%s: no intervals recorded", st->what);
        return ok;
    }

    t_note(t, "%s: %" PRIu32 " intervals, mean %" PRIu64 " us, min %" PRIu64
              " us, max %" PRIu64 " us; %" PRIu32 " within 10%% of %" PRIu64
              " us; %" PRIu32 " longer than 1.5 refreshes",
           st->what, st->intervals, nw_mean_ns(st) / 1000, st->min_ns / 1000,
           st->max_ns / 1000, st->within_10pct, NW_REFRESH_NS / 1000,
           st->over_1p5_refresh);
    t_note(t, "%s: dequeue mean %" PRIu64 " us / max %" PRIu64 " us; "
              "fill+flush mean %" PRIu64 " us; largest pending-buffer count "
              "%" PRIu32 "; %" PRIu32 " dequeue(s) carried a release fence",
           st->what, (st->dequeue_total_ns / st->frames_presented) / 1000,
           st->dequeue_max_ns / 1000,
           (st->fill_total_ns / st->frames_presented) / 1000,
           st->max_pending, st->release_fences);
    return ok;
}

/* ------------------------------------------------------------------ */

/* Chooses the odd-frame load for the pacing comparison from what a
 * frame already costs, rather than from a guess.
 *
 * The shape wanted is: average frame cost comfortably below one
 * refresh, peak cost above it. Then a two-buffer producer has to round
 * every peak up to a whole extra refresh — it has no spare buffer to
 * get ahead with — while a three-buffer one can absorb it into the
 * cheap frame that follows. `fill_ns` is the cost this test cannot
 * avoid: writing 3.75 MB and flushing it out of the cache.
 *
 * Returns 0 when no such load exists, which happens if a frame already
 * costs more than half a refresh. The caller then says so and does not
 * pretend to have measured a difference.
 */
static uint64_t nw_choose_load(uint64_t fill_ns)
{
    /* Target: mean cost = 3/4 of a refresh. Odd frames carry twice the
     * average extra work and even frames carry none, so the extra on an
     * odd frame is 2 * (target - fill). */
    const uint64_t target = (NW_REFRESH_NS * 3u) / 4u;
    if (fill_ns >= target)
        return 0;

    const uint64_t busy = 2u * (target - fill_ns);

    /* The peak has to exceed one refresh or there is nothing for double
     * buffering to round up. */
    if (fill_ns + busy <= NW_REFRESH_NS)
        return 0;

    return busy;
}

int run_test(test_ctx *t)
{
    horizon_gpu_device *dev = NULL;
    horizon_gpu_mem *mem = NULL;
    int rv = 0;

    t_note(t, "this test owns the display: no console was started, and "
              "this file is the whole record");

    /* The device is what brings up nv, nvfence and nvmap (device.c);
     * nvMultiFenceWait and nvMapCreate both need them. */
    horizon_gpu_result res = horizon_gpu_device_create(NULL, &dev);
    if (!t_check(t, res.status == HORIZON_GPU_OK,
                 "horizon_gpu_device_create (status %d, nv 0x%08" PRIx32 ")",
                 (int)res.status, res.nv))
        return 1;

    NWindow *win = nwindowGetDefault();
    if (!t_check(t, win != NULL && nwindowIsValid(win),
                 "nwindowGetDefault() returned a valid window")) {
        rv = 1;
        goto out_dev;
    }

    u32 w = 0, h = 0;
    Result rc = nwindowGetDimensions(win, &w, &h);
    if (!t_check(t, R_SUCCEEDED(rc),
                 "nwindowGetDimensions -> 0x%08x (%" PRIu32 " x %" PRIu32 ")",
                 rc, w, h)) {
        rv = 1;
        goto out_dev;
    }

    nw_geometry geom;
    if (!t_check(t, nw_geometry_init(&geom, w, h),
                 "the scanout geometry for %" PRIu32 "x%" PRIu32
                 " is representable", w, h)) {
        rv = 1;
        goto out_dev;
    }
    t_note(t, "geometry: pitch %" PRIu32 " B (stride %" PRIu32 " px), height "
              "aligned to %" PRIu32 " rows, %" PRIu32 " B per buffer",
           geom.pitch_B, geom.stride_px, geom.height_aligned,
           geom.buffer_size_B);

    /* One NvMap holding every buffer any session will use, so the
     * sessions differ only in how many slots they register. */
    const uint64_t total_B = (uint64_t)geom.buffer_size_B * NW_MAX_BUFFERS;
    res = horizon_gpu_mem_create(dev, total_B, NW_NVMAP_ALIGN,
                                 HORIZON_GPU_MEM_CACHED, &mem);
    if (!t_check(t, res.status == HORIZON_GPU_OK,
                 "one NvMap of %" PRIu64 " KiB for %u buffers "
                 "(status %d, nv 0x%08" PRIx32 ")",
                 total_B / 1024, NW_MAX_BUFFERS, (int)res.status, res.nv)) {
        rv = 1;
        goto out_dev;
    }

    const uint32_t nvmap_id = horizon_gpu_mem_get_id(mem);
    t_check(t, nvmap_id != 0,
            "the NvMap has an id the compositor can look up (%" PRIu32 ")",
            nvmap_id);

    void *cpu = horizon_gpu_mem_cpu_ptr(mem);
    if (!t_check(t, cpu != NULL, "the NvMap is mapped for the CPU")) {
        rv = 1;
        goto out_mem;
    }

    nw_session s = {
        .t = t,
        .prod = { .win = win, .slots_requested = 0 },
        .mem = mem,
        .cpu = cpu,
        .geom = geom,
        .num_buffers = 3,
        .swap_interval = 1,
        .busy_ns_odd = 0,
    };

    nw_stats st3, st2, st3_free, st3_load, st2_load;
    uint32_t concurrent3 = 0, concurrent2 = 0;
    bool ran3 = false, ran2 = false, ran3_free = false;
    bool ran3_load = false, ran2_load = false;

    /* --- session A: three buffers, interval 1, no extra load ------- */
    if (!nw_session_register(&s)) {
        rv = 1;
        goto out_mem;
    }

    concurrent3 = nw_measure_concurrent_dequeues(&s);
    t_note(t, "THE NUMBER: with 3 registered buffers, %" PRIu32
              " slot(s) could be dequeued at once", concurrent3);

    /* Vulkan's contract, restated as an assertion. Offering
     * minImageCount = 2 on a three-image swapchain makes two acquired
     * images valid usage, so anything less than two here means
     * docs/wsi.md § 2.3 cannot stand as written. */
    t_check(t, concurrent3 >= 2,
            "at least two slots can be held at once, which is what "
            "minImageCount = 2 on a 3-image swapchain requires");

    nw_stats_init(&st3, "3 buffers, interval 1");
    nw_session_present(&s, &st3);
    ran3 = nw_report(t, &st3);

    rc = nwindowReleaseBuffers(win);
    if (!t_check(t, R_SUCCEEDED(rc),
                 "3 buffers: released back to the compositor -> 0x%08x", rc)) {
        rv = 1;
        goto out_mem;
    }

    /* --- session B: two buffers, interval 1, no extra load --------- */
    s.num_buffers = 2;
    if (!nw_session_register(&s)) {
        rv = 1;
        goto out_mem;
    }
    concurrent2 = nw_measure_concurrent_dequeues(&s);
    t_note(t, "with 2 registered buffers, %" PRIu32
              " slot(s) could be dequeued at once", concurrent2);

    nw_stats_init(&st2, "2 buffers, interval 1");
    nw_session_present(&s, &st2);
    ran2 = nw_report(t, &st2);

    rc = nwindowReleaseBuffers(win);
    if (!t_check(t, R_SUCCEEDED(rc),
                 "2 buffers: released back to the compositor -> 0x%08x", rc)) {
        rv = 1;
        goto out_mem;
    }

    /* --- sessions C and D: the same bursty load, 3 buffers then 2 --- */
    const uint64_t fill_ns = st3.frames_presented ?
        st3.fill_total_ns / st3.frames_presented : 0;
    const uint64_t load_ns = nw_choose_load(fill_ns);

    t_note(t, "a frame's unavoidable cost (fill + flush) measured "
              "%" PRIu64 " us; the bursty load puts %" PRIu64
              " us more on every odd frame, for a mean of %" PRIu64
              " us against a %" PRIu64 " us refresh",
           fill_ns / 1000, load_ns / 1000, (fill_ns + load_ns / 2) / 1000,
           NW_REFRESH_NS / 1000);

    if (load_ns == 0) {
        t_note(t, "NO PACING COMPARISON WAS MADE: a frame already costs "
                  "%" PRIu64 " us of a %" PRIu64 " us refresh, so there is "
                  "no load that averages under one refresh and peaks over "
                  "it. The comparison is skipped rather than reported "
                  "from numbers that cannot show it",
               fill_ns / 1000, NW_REFRESH_NS / 1000);
    } else {
        s.num_buffers = 3;
        s.busy_ns_odd = load_ns;
        if (!nw_session_register(&s)) {
            rv = 1;
            goto out_mem;
        }
        nw_stats_init(&st3_load, "3 buffers, interval 1, bursty load");
        nw_session_present(&s, &st3_load);
        ran3_load = nw_report(t, &st3_load);

        rc = nwindowReleaseBuffers(win);
        if (!t_check(t, R_SUCCEEDED(rc),
                     "3 buffers (loaded): released -> 0x%08x", rc)) {
            rv = 1;
            goto out_mem;
        }

        s.num_buffers = 2;
        if (!nw_session_register(&s)) {
            rv = 1;
            goto out_mem;
        }
        nw_stats_init(&st2_load, "2 buffers, interval 1, bursty load");
        nw_session_present(&s, &st2_load);
        ran2_load = nw_report(t, &st2_load);

        rc = nwindowReleaseBuffers(win);
        if (!t_check(t, R_SUCCEEDED(rc),
                     "2 buffers (loaded): released -> 0x%08x", rc)) {
            rv = 1;
            goto out_mem;
        }
        s.busy_ns_odd = 0;
    }

    /* --- session E: three buffers, swap interval 0 ----------------- */
    /* libnx's own note on nwindowSetSwapInterval: 0 is honoured only
     * with three or more buffers configured; with fewer the system
     * enforces a minimum of 1. So this is the one configuration in
     * which the interval can be observed to do anything. */
    s.num_buffers = 3;
    s.swap_interval = 0;
    if (!nw_session_register(&s)) {
        rv = 1;
        goto out_mem;
    }
    nw_stats_init(&st3_free, "3 buffers, interval 0");
    nw_session_present(&s, &st3_free);
    ran3_free = nw_report(t, &st3_free);

    rc = nwindowReleaseBuffers(win);
    t_check(t, R_SUCCEEDED(rc),
            "3 buffers (interval 0): released back to the compositor "
            "-> 0x%08x", rc);

    /* --- what the numbers have to say ----------------------------- */

    /* Something outside this process consumed the buffers: with three
     * registered and 90 presented, at least 87 releases had to happen,
     * and this process performs none of them. */
    t_check(t, ran3 && st3.release_fences > 0,
            "the compositor handed back a release fence: %" PRIu32
            " of %u dequeues carried one, the first on syncpoint %" PRIu32,
            st3.release_fences, NW_FRAMES, st3.first_fence_syncpt);

    if (ran3)
        t_check(t, st3.within_10pct * 10 >= st3.intervals * 9,
                "3 buffers, interval 1: %" PRIu32 " of %" PRIu32
                " intervals are within 10%% of %" PRIu64 " us "
                "(>= 90%% needed)",
                st3.within_10pct, st3.intervals, NW_REFRESH_NS / 1000);

    if (ran2)
        t_check(t, st2.within_10pct * 10 >= st2.intervals * 9,
                "2 buffers, interval 1: %" PRIu32 " of %" PRIu32
                " intervals are within 10%% of %" PRIu64 " us "
                "(>= 90%% needed)",
                st2.within_10pct, st2.intervals, NW_REFRESH_NS / 1000);

    /* Double against triple, structurally: how deep the producer may
     * get ahead. This is the difference that makes the two configurations
     * different for Vulkan, and it has no timing in it at all. */
    t_check(t, concurrent3 > concurrent2,
            "three buffers let the producer hold more slots than two "
            "(%" PRIu32 " vs %" PRIu32 ")", concurrent3, concurrent2);

    /* Double against triple, in frame pacing. The same bursty load
     * through both: two buffers must round every overrun up to a whole
     * extra refresh, three can absorb it. 10%% is far below the 50%%
     * the arithmetic predicts and far above measurement noise. */
    if (ran3_load && ran2_load) {
        const uint64_t m3 = nw_mean_ns(&st3_load);
        const uint64_t m2 = nw_mean_ns(&st2_load);
        t_note(t, "bursty load: mean interval %" PRIu64 " us with 3 buffers, "
                  "%" PRIu64 " us with 2; intervals longer than 1.5 "
                  "refreshes: %" PRIu32 " with 3, %" PRIu32 " with 2",
               m3 / 1000, m2 / 1000, st3_load.over_1p5_refresh,
               st2_load.over_1p5_refresh);
        t_check(t, m2 * 10 > m3 * 11,
                "under the same bursty load two buffers pace at least 10%% "
                "slower than three (%" PRIu64 " us vs %" PRIu64 " us)",
                m2 / 1000, m3 / 1000);
    }

    if (ran3_free && ran3)
        t_note(t, "3 buffers at interval 0: mean %" PRIu64 " us against "
                  "%" PRIu64 " us at interval 1 — the swap interval is the "
                  "knob, and this is what it moved",
               nw_mean_ns(&st3_free) / 1000, nw_mean_ns(&st3) / 1000);

out_mem:
    if (mem != NULL) {
        horizon_gpu_result mres = horizon_gpu_mem_destroy(mem);
        t_check(t, mres.status == HORIZON_GPU_OK,
                "the scanout NvMap is destroyed (status %d)",
                (int)mres.status);
    }
out_dev:
    if (dev != NULL) {
        horizon_gpu_result dres = horizon_gpu_device_destroy(dev);
        t_check(t, dres.status == HORIZON_GPU_OK,
                "the device tears down after presenting (status %d)",
                (int)dres.status);
    }
    return rv;
}
