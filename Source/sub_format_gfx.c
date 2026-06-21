#include "sub_engine.h"
#include "stream.h"   // for AV_IMAGE_BGRA_32
#include <stdlib.h>
#include <string.h>

typedef struct {
    SUB_FRAME *current_frame; // owned here — never freed by the GL renderer
    int        is_cleared;    // 1 = no subtitle currently visible (PGS clear signal received)
    int        is_dirty;
    int        delivered;     // 1 = current_frame has already been handed to the renderer (which
                              // now holds its own ref). Reset whenever current_frame is replaced.
    int        canvas_w, canvas_h;           // on-screen GL surface size. NOT the space
                                              // ev->x/y/w/h are expressed in -- see below.
    int        real_video_w, real_video_h;   // decoded video's own coded size -- fixed for
                                              // the track's lifetime. THIS is the space
                                              // codec_ffsub's x_offset/y_offset/width/height
                                              // are expressed in.
    int        video_box_x, video_box_y;     // where the video's own on-screen box sits
    int        video_box_w, video_box_h;     // within the canvas (post letterbox/pillarbox/
                                              // zoom-crop/stretch), as reported by
                                              // sub_engine_set_video_box().
} GFX_BACKEND;

// ---------------------------------------------------------------------------
// Shared pixel blocks
//
// A bitmap's pixels live in ONE malloc block laid out as [ atomic_int refs | pad | pixels ].
// SUB_EVENT::pixel_refs points at the counter, which is also the block's malloc base, and
// SUB_EVENT::data.bitmap.rgba points at the pixels inside it. sub_frame_unref() (sub_engine.c)
// only needs to know that: when pixel_refs is set it drops one reference and free()s
// pixel_refs when the last one goes -- the layout itself is private to this file.
//
// The pixels are never written once gfx_feed_bitmap() has finished filling them, so several
// frames (and threads) can safely share one block; only the counter changes, atomically.
// This is what lets gfx_clone_with_geometry() be O(1) instead of a bitmap-sized memcpy
// under eng->lock.
// ---------------------------------------------------------------------------
#define GFX_PIXEL_HDR 8   // counter + padding; keeps the pixels 8-byte aligned
_Static_assert(sizeof(atomic_int) <= GFX_PIXEL_HDR, "GFX_PIXEL_HDR too small for atomic_int");

// Allocates header + n bytes. Returns the pixel pointer (uninitialised) and stores the
// counter (refcount = 1) in *refs_out. NULL on failure.
static uint8_t *gfx_pixels_alloc(size_t n, atomic_int **refs_out) {
    if (n > SIZE_MAX - GFX_PIXEL_HDR) return NULL;
    uint8_t *block = (uint8_t *)malloc(GFX_PIXEL_HDR + n);
    if (!block) return NULL;
    atomic_int *refs = (atomic_int *)block;
    atomic_init(refs, 1);
    *refs_out = refs;
    return block + GFX_PIXEL_HDR;
}

// ---------------------------------------------------------------------------
// gfx_open
// ---------------------------------------------------------------------------
static int gfx_open(SUB_FORMAT_BACKEND *be, const SUB_FORMAT_OPEN_PARAMS *params) {
    GFX_BACKEND *ctx = calloc(1, sizeof(GFX_BACKEND));
    ctx->canvas_w     = params->video_w > 0 ? params->video_w : 1920;
    ctx->canvas_h     = params->video_h > 0 ? params->video_h : 1080;
    ctx->real_video_w = params->real_video_w > 0 ? params->real_video_w : ctx->canvas_w;
    ctx->real_video_h = params->real_video_h > 0 ? params->real_video_h : ctx->canvas_h;
    // Until sub_engine_set_video_box() has reported an actual box, assume the video fills
    // the canvas 1:1 -- this is today's (buggy) behavior, kept as the fallback so a track
    // opened before Java's first box report still renders (just not correctly positioned
    // until the first real report arrives, same as before this fix).
    ctx->video_box_x  = params->video_box_w > 0 ? params->video_box_x : 0;
    ctx->video_box_y  = params->video_box_h > 0 ? params->video_box_y : 0;
    ctx->video_box_w  = params->video_box_w > 0 ? params->video_box_w : ctx->canvas_w;
    ctx->video_box_h  = params->video_box_h > 0 ? params->video_box_h : ctx->canvas_h;
    ctx->is_cleared = 1; // nothing to show yet
    be->priv = ctx;
    return 0;
}

// ---------------------------------------------------------------------------
// gfx_feed_bitmap
//
// Called by _feed_bitmap_to_engine() in stream_subtitle.c every time
// codec_ffsub produces a decoded frame — including the PGS zero-rect clear
// frame (width=1, height=1, duration=0).
//
// Colorspace:
//   codec_ffsub always produces AV_IMAGE_BGRA_32 (confirmed by the
//   av_image_alloc(AV_PIX_FMT_BGRA) call and frame->colorspace assignment).
//   The GL renderer expects RGBA. We swizzle R<->B during the pixel copy.
// ---------------------------------------------------------------------------
static int gfx_feed_bitmap(SUB_FORMAT_BACKEND *be,
                           uint8_t *pixels,
                           int width, int height, int pitch,
                           int colorspace,
                           int x_offset, int y_offset,
                           int64_t pts_ms, int64_t duration_ms)
{
    GFX_BACKEND *ctx = (GFX_BACKEND *)be->priv;

    // Retire the previous stored frame before replacing it
    if (ctx->current_frame) {
        sub_frame_unref(ctx->current_frame);
        ctx->current_frame = NULL;
    }
    ctx->delivered = 0;

    // PGS clear signal: codec_ffsub sends a 1x1 zero-rect frame with
    // duration=0 to signal "hide the current subtitle". Honour it.
    if (duration_ms == 0 || !pixels || width <= 0 || height <= 0) {
        ctx->is_cleared = 1;
        ctx->is_dirty = 1;
        return 0;
    }

    // Pixels first, so an allocation failure leaves the backend in a clean "nothing shown"
    // state instead of half-built.
    atomic_int *pixel_refs = NULL;
    uint8_t *rgba = gfx_pixels_alloc((size_t)width * (size_t)height * 4, &pixel_refs);
    if (!rgba) {
        ctx->is_cleared = 1;
        ctx->is_dirty = 1;
        return -1;
    }

    ctx->is_cleared = 0;
    ctx->is_dirty = 1;

    // Build the stored frame
    SUB_FRAME *frame  = calloc(1, sizeof(SUB_FRAME));
    atomic_init(&frame->refcount, 1);

    frame->pts_ms      = pts_ms;
    frame->duration_ms = duration_ms;
    frame->video_w        = ctx->canvas_w;
    frame->video_h        = ctx->canvas_h;
    frame->real_video_w   = ctx->real_video_w;
    frame->real_video_h   = ctx->real_video_h;
    frame->video_box_x    = ctx->video_box_x;
    frame->video_box_y    = ctx->video_box_y;
    frame->video_box_w    = ctx->video_box_w;
    frame->video_box_h    = ctx->video_box_h;

    SUB_EVENT *ev = calloc(1, sizeof(SUB_EVENT));
    ev->kind             = SUB_EVENT_BITMAP;
    ev->x                = x_offset;
    ev->y                = y_offset;
    ev->w                = width;
    ev->h                = height;
    ev->data.bitmap.stride = width * 4; // always RGBA after swizzle

    ev->data.bitmap.rgba = rgba;
    ev->pixel_refs       = pixel_refs;   // event owns the single initial reference

    const int is_bgra = (colorspace == AV_IMAGE_BGRA_32);

    for (int row = 0; row < height; row++) {
        const uint8_t *src = pixels + row * pitch;
        uint8_t       *dst = rgba   + row * (width * 4);
        for (int col = 0; col < width; col++) {
            if (is_bgra) {
                // BGRA -> RGBA: swap B(src[0]) and R(src[2])
                dst[0] = src[2]; // R
                dst[1] = src[1]; // G
                dst[2] = src[0]; // B
                dst[3] = src[3]; // A
            } else {
                // Already RGBA, just copy
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                dst[3] = src[3];
            }
            src += 4;
            dst += 4;
        }
    }

    frame->events      = ev;
    ctx->current_frame = frame;

    return 0;
}

// ---------------------------------------------------------------------------
// gfx_render_at
//
// The GL renderer polls this every frame.  For bitmap subtitles there is no
// per-frame re-render — we just hand back a clone of whatever codec_ffsub
// last gave us, until a clear signal arrives.
//
// We return a CLONE (not the stored pointer) because the GL renderer will
// call free_frame on the returned pointer.  ctx->current_frame must survive
// intact for the next poll.
//
// We do NOT do a time-window check here.  PGS subtitles don't carry reliable
// duration; the real end is signalled by the zero-rect clear packet handled
// in gfx_feed_bitmap above.  VobSub does have durations but they are also
// unreliable — keeping it simple: show until cleared.
// ---------------------------------------------------------------------------
// True if the cached frame's baked-in geometry no longer matches what the backend
// currently knows (canvas size / video box).
static int gfx_geometry_stale(const GFX_BACKEND *ctx, const SUB_FRAME *f) {
    return f->video_w     != ctx->canvas_w     || f->video_h     != ctx->canvas_h     ||
           f->video_box_x != ctx->video_box_x  || f->video_box_y != ctx->video_box_y  ||
           f->video_box_w != ctx->video_box_w  || f->video_box_h != ctx->video_box_h;
}

// Copy-on-write: build a brand-new frame stamped with the CURRENT geometry. The old frame
// is never mutated -- the render thread reads its fields without eng->lock, so in-place
// edits would be a data race and would also leave the frame's pointer unchanged, which the
// renderer treats as "nothing new, don't redraw".
//
// The pixels are NOT copied: the clone takes a reference on the same immutable pixel block,
// so this is two small callocs and an atomic increment even for a full-screen bitmap.
// (render_at runs with eng->lock held, so this matters -- a memcpy here would stall
// feed/resize/set_video_box during e.g. a floating-window drag-resize.)
// gfx_feed_bitmap only ever builds a single-event frame, so cloning one event is enough.
static SUB_FRAME *gfx_clone_with_geometry(const GFX_BACKEND *ctx, const SUB_FRAME *src) {
    const SUB_EVENT *sev = src->events;
    if (!sev || !sev->pixel_refs) return NULL;   // not built by gfx_feed_bitmap -- leave it alone

    SUB_FRAME *f  = calloc(1, sizeof(SUB_FRAME));
    SUB_EVENT *ev = calloc(1, sizeof(SUB_EVENT));
    if (!f || !ev) { free(f); free(ev); return NULL; }

    atomic_init(&f->refcount, 1);
    f->pts_ms       = src->pts_ms;
    f->duration_ms  = src->duration_ms;
    f->video_w      = ctx->canvas_w;       f->video_h      = ctx->canvas_h;
    f->real_video_w = ctx->real_video_w;   f->real_video_h = ctx->real_video_h;
    f->video_box_x  = ctx->video_box_x;    f->video_box_y  = ctx->video_box_y;
    f->video_box_w  = ctx->video_box_w;    f->video_box_h  = ctx->video_box_h;

    *ev = *sev;                       // kind, x, y, w, h, stride, rgba ptr, pixel_refs
    ev->next = NULL;
    atomic_fetch_add(ev->pixel_refs, 1);        // shared, immutable pixels
    f->events = ev;
    return f;
}

static SUB_FRAME *gfx_render_at(SUB_FORMAT_BACKEND *be, int64_t pts_ms) {
    GFX_BACKEND *ctx = (GFX_BACKEND *)be->priv;

    // 1. If nothing changed, return NULL (Triggers the GL Bypass)
    if (!ctx->is_dirty) return NULL;
    // Consume the dirty flag
    ctx->is_dirty = 0;
    // 2. If it changed to a CLEAR state, return an empty frame to wipe the screen
    if (ctx->is_cleared || !ctx->current_frame) {
        SUB_FRAME *empty_frame = calloc(1, sizeof(SUB_FRAME));
        atomic_init(&empty_frame->refcount, 1);
        return empty_frame; // No events attached = clear screen
    }

    // 3. Geometry changed since this bitmap was decoded (rotation, floating window,
    //    margins toggle, ...): swap in a re-stamped copy. The renderer keeps its own ref
    //    to the old frame, untouched, so this is race-free, and the pointer difference is
    //    what makes the renderer redraw and bump frame_generation.
    SUB_FRAME *cf = ctx->current_frame;
    if (gfx_geometry_stale(ctx, cf)) {
        SUB_FRAME *fresh = gfx_clone_with_geometry(ctx, cf);
        if (fresh) {
            sub_frame_unref(cf);
            ctx->current_frame = cf = fresh;
            ctx->delivered = 0;
        }
    }

    // 4. Renderer already holds this exact frame and nothing about it changed (e.g. a
    //    redundant set_video_box). Returning it again would hand out a ref the renderer
    //    never releases (it only unrefs on a pointer swap) -- so return NULL instead.
    if (ctx->delivered) return NULL;

    ctx->delivered = 1;
    sub_frame_ref(cf);
    return cf;
}

// ---------------------------------------------------------------------------
// gfx_free_frame
//
// Called by the GL renderer on frames returned by render_at.
// Those are clones — free them fully.
// ---------------------------------------------------------------------------
static void gfx_free_frame(SUB_FORMAT_BACKEND *be, SUB_FRAME *frame) {
    sub_frame_unref(frame);
}

// ---------------------------------------------------------------------------
// gfx_resize
// ---------------------------------------------------------------------------
static int gfx_resize(SUB_FORMAT_BACKEND *be, int canvas_w, int canvas_h) {
    GFX_BACKEND *ctx = (GFX_BACKEND *)be->priv;
    // Canvas resize only (rotation, surface recreate) -- real_video_w/h and the video's
    // own box don't change just because the GL surface did; those come from
    // gfx_set_video_box() below, driven independently by SurfaceController.
    ctx->canvas_w = canvas_w;
    ctx->canvas_h = canvas_h;
    ctx->is_dirty = 1;
    return 0;
}

// ---------------------------------------------------------------------------
// gfx_set_video_box
//
// Called whenever SurfaceController recomputes where the video itself sits
// on screen -- rotation, use_sub_margins toggling, a new video's aspect
// ratio changing the letterbox/pillarbox amount. Independent of gfx_resize:
// the canvas can resize without the video's box changing shape (e.g. a
// symmetric screen resize) and the box can change without the canvas
// resizing (e.g. margins preference flipped without rotating).
// ---------------------------------------------------------------------------
static int gfx_set_video_box(SUB_FORMAT_BACKEND *be, int x, int y, int w, int h) {
    GFX_BACKEND *ctx = (GFX_BACKEND *)be->priv;
    ctx->video_box_x = x;
    ctx->video_box_y = y;
    ctx->video_box_w = w;
    ctx->video_box_h = h;
    ctx->is_dirty = 1; // force a redraw with corrected geometry even if the bitmap itself is unchanged
    return 0;
}

// ---------------------------------------------------------------------------
// gfx_flush
//
// Called on seek — clear the stored frame so stale bitmaps don't reappear.
// ---------------------------------------------------------------------------
static int gfx_flush(SUB_FORMAT_BACKEND *be) {
    GFX_BACKEND *ctx = (GFX_BACKEND *)be->priv;
    if (ctx->current_frame) {
        sub_frame_unref(ctx->current_frame);
        ctx->current_frame = NULL;
    }
    ctx->is_cleared = 1;
    ctx->delivered = 0;
    ctx->is_dirty = 1;
    return 0;
}

// ---------------------------------------------------------------------------
// gfx_close
// ---------------------------------------------------------------------------
static int gfx_close(SUB_FORMAT_BACKEND *be) {
    GFX_BACKEND *ctx = (GFX_BACKEND *)be->priv;
    if (ctx->current_frame) {
        sub_frame_unref(ctx->current_frame);
    }
    free(ctx);
    return 0;
}

// ---------------------------------------------------------------------------
// gfx_get_timeout_ms
// ---------------------------------------------------------------------------
static int gfx_get_timeout_ms(SUB_FORMAT_BACKEND *be, int64_t pts_ms) {
    return -1; // Infinite sleep. Only wakes when a feed/clear signals the engine.
}

// ---------------------------------------------------------------------------
// sub_format_gfx_create
// ---------------------------------------------------------------------------
SUB_FORMAT_BACKEND *sub_format_gfx_create(void) {
    SUB_FORMAT_BACKEND *be = calloc(1, sizeof(SUB_FORMAT_BACKEND));
    be->open        = gfx_open;
    be->feed_bitmap = gfx_feed_bitmap;
    be->render_at   = gfx_render_at;
    be->free_frame  = gfx_free_frame;
    be->resize      = gfx_resize;
    be->set_video_box = gfx_set_video_box;
    be->flush       = gfx_flush;
    be->close       = gfx_close;
    be->get_timeout_ms = gfx_get_timeout_ms;
    return be;
}
