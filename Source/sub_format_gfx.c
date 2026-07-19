#include "sub_format.h"
#include "stream.h"   // for AV_IMAGE_BGRA_32
#include <stdlib.h>
#include <string.h>

typedef struct {
    SUB_FRAME *current_frame; // owned here — never freed by the GL renderer
    int        is_cleared;    // 1 = no subtitle currently visible (PGS clear signal received)
    int        video_w;
    int        video_h;
} GFX_BACKEND;

// ---------------------------------------------------------------------------
// _gfx_free_frame_internal
//
// Internal free that always destroys the frame and its pixel data.
// Called only by gfx_feed_bitmap (replacing old frame) and gfx_close.
// ---------------------------------------------------------------------------
static void _gfx_free_frame_internal(SUB_FRAME *frame) {
    if (!frame) return;
    SUB_EVENT *ev = frame->events;
    while (ev) {
        SUB_EVENT *next = ev->next;
        if (ev->kind == SUB_EVENT_BITMAP && ev->data.bitmap.rgba) {
            free((void*)ev->data.bitmap.rgba);
        }
        free(ev);
        ev = next;
    }
    free(frame);
}

// ---------------------------------------------------------------------------
// _gfx_clone_frame
//
// Returns a shallow clone of current_frame with a fresh pixel copy.
// The GL renderer calls free_frame on whatever render_at returns, so we must
// hand it a separate allocation — ctx->current_frame must remain intact for
// the next render_at call.
// ---------------------------------------------------------------------------
static SUB_FRAME *_gfx_clone_frame(const SUB_FRAME *src) {
    if (!src) return NULL;

    SUB_FRAME *dst = calloc(1, sizeof(SUB_FRAME));
    dst->pts_ms      = src->pts_ms;
    dst->duration_ms = src->duration_ms;
    dst->video_w     = src->video_w;
    dst->video_h     = src->video_h;

    SUB_EVENT *last = NULL;
    for (const SUB_EVENT *ev = src->events; ev; ev = ev->next) {
        SUB_EVENT *copy = calloc(1, sizeof(SUB_EVENT));
        copy->kind = ev->kind;
        copy->x    = ev->x;
        copy->y    = ev->y;
        copy->w    = ev->w;
        copy->h    = ev->h;

        if (ev->kind == SUB_EVENT_BITMAP && ev->data.bitmap.rgba) {
            int bytes = ev->data.bitmap.stride * ev->h;
            copy->data.bitmap.stride = ev->data.bitmap.stride;
            copy->data.bitmap.rgba   = malloc(bytes);
            memcpy((void*)copy->data.bitmap.rgba, ev->data.bitmap.rgba, bytes);
        }

        if (!dst->events) dst->events = copy;
        else              last->next  = copy;
        last = copy;
    }

    return dst;
}

// ---------------------------------------------------------------------------
// gfx_open
// ---------------------------------------------------------------------------
static int gfx_open(SUB_FORMAT_BACKEND *be, const SUB_FORMAT_OPEN_PARAMS *params) {
    GFX_BACKEND *ctx = calloc(1, sizeof(GFX_BACKEND));
    ctx->video_w   = params->video_w > 0 ? params->video_w : 1920;
    ctx->video_h   = params->video_h > 0 ? params->video_h : 1080;
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
        _gfx_free_frame_internal(ctx->current_frame);
        ctx->current_frame = NULL;
    }

    // PGS clear signal: codec_ffsub sends a 1x1 zero-rect frame with
    // duration=0 to signal "hide the current subtitle". Honour it.
    if (duration_ms == 0 || !pixels || width <= 0 || height <= 0) {
        ctx->is_cleared = 1;
        return 0;
    }

    ctx->is_cleared = 0;

    // Build the stored frame
    SUB_FRAME *frame  = calloc(1, sizeof(SUB_FRAME));
    frame->pts_ms      = pts_ms;
    frame->duration_ms = duration_ms;
    frame->video_w     = ctx->video_w;
    frame->video_h     = ctx->video_h;

    SUB_EVENT *ev = calloc(1, sizeof(SUB_EVENT));
    ev->kind             = SUB_EVENT_BITMAP;
    ev->x                = x_offset;
    ev->y                = y_offset;
    ev->w                = width;
    ev->h                = height;
    ev->data.bitmap.stride = width * 4; // always RGBA after swizzle

    uint8_t *rgba = malloc(width * height * 4);
    ev->data.bitmap.rgba = rgba;

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
static SUB_FRAME *gfx_render_at(SUB_FORMAT_BACKEND *be, int64_t pts_ms) {
    GFX_BACKEND *ctx = (GFX_BACKEND *)be->priv;

    if (ctx->is_cleared || !ctx->current_frame) return NULL;

    return _gfx_clone_frame(ctx->current_frame);
}

// ---------------------------------------------------------------------------
// gfx_free_frame
//
// Called by the GL renderer on frames returned by render_at.
// Those are clones — free them fully.
// ---------------------------------------------------------------------------
static void gfx_free_frame(SUB_FORMAT_BACKEND *be, SUB_FRAME *frame) {
    _gfx_free_frame_internal(frame);
}

// ---------------------------------------------------------------------------
// gfx_resize
// ---------------------------------------------------------------------------
static int gfx_resize(SUB_FORMAT_BACKEND *be, int video_w, int video_h) {
    GFX_BACKEND *ctx = (GFX_BACKEND *)be->priv;
    ctx->video_w = video_w;
    ctx->video_h = video_h;
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
        _gfx_free_frame_internal(ctx->current_frame);
        ctx->current_frame = NULL;
    }
    ctx->is_cleared = 1;
    return 0;
}

// ---------------------------------------------------------------------------
// gfx_close
// ---------------------------------------------------------------------------
static int gfx_close(SUB_FORMAT_BACKEND *be) {
    GFX_BACKEND *ctx = (GFX_BACKEND *)be->priv;
    if (ctx->current_frame) {
        _gfx_free_frame_internal(ctx->current_frame);
    }
    free(ctx);
    return 0;
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
    be->flush       = gfx_flush;
    be->close       = gfx_close;
    return be;
}
