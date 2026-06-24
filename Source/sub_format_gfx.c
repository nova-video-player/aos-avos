#include "sub_format.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    SUB_FRAME *current_frame;
} GFX_BACKEND;

static int gfx_open(SUB_FORMAT_BACKEND *be, const SUB_FORMAT_OPEN_PARAMS *params) {
    GFX_BACKEND *ctx = calloc(1, sizeof(GFX_BACKEND));
    be->priv = ctx;
    return 0;
}

static int gfx_feed_bitmap(SUB_FORMAT_BACKEND *be, uint8_t *pixels, int width, int height, int pitch, int colorspace, int x_offset, int y_offset, int64_t pts_ms, int64_t duration_ms) {
    GFX_BACKEND *ctx = (GFX_BACKEND *)be->priv;

    // Clean up previous frame if it exists
    if (ctx->current_frame) {
        be->free_frame(be, ctx->current_frame);
    }

    SUB_FRAME *frame = calloc(1, sizeof(SUB_FRAME));
    frame->pts_ms = pts_ms;
    frame->duration_ms = duration_ms;

    SUB_EVENT *ev = calloc(1, sizeof(SUB_EVENT));
    ev->kind = SUB_EVENT_BITMAP;
    ev->data.bitmap.x = x_offset;
    ev->data.bitmap.y = y_offset;
    ev->data.bitmap.w = width;
    ev->data.bitmap.h = height;
    ev->data.bitmap.pitch = pitch;

    // Allocate memory and copy the pixels so OpenGL can upload them asynchronously
    int pixel_bytes = pitch * height;
    ev->data.bitmap.rgba = malloc(pixel_bytes);
    memcpy((void*)ev->data.bitmap.rgba, pixels, pixel_bytes);

    frame->events = ev;
    ctx->current_frame = frame;

    return 0;
}

static SUB_FRAME *gfx_render_at(SUB_FORMAT_BACKEND *be, int64_t pts_ms) {
    GFX_BACKEND *ctx = (GFX_BACKEND *)be->priv;
    if (!ctx->current_frame) return NULL;

    // Check if the current frame is visible at this timestamp
    if (pts_ms >= ctx->current_frame->pts_ms && pts_ms <= (ctx->current_frame->pts_ms + ctx->current_frame->duration_ms)) {
        return ctx->current_frame;
    }
    return NULL;
}

static void gfx_free_frame(SUB_FORMAT_BACKEND *be, SUB_FRAME *frame) {
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

static int gfx_close(SUB_FORMAT_BACKEND *be) {
    GFX_BACKEND *ctx = (GFX_BACKEND *)be->priv;
    if (ctx->current_frame) {
        gfx_free_frame(be, ctx->current_frame);
    }
    free(ctx);
    return 0;
}

SUB_FORMAT_BACKEND *sub_format_gfx_create(void) {
    SUB_FORMAT_BACKEND *be = calloc(1, sizeof(SUB_FORMAT_BACKEND));
    be->open = gfx_open;
    be->feed_bitmap = gfx_feed_bitmap;
    be->render_at = gfx_render_at;
    be->free_frame = gfx_free_frame;
    be->close = gfx_close;
    return be;
}
