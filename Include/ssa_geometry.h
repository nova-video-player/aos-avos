#ifndef SSA_GEOMETRY_H
#define SSA_GEOMETRY_H
/*
 * Canvas / video-box geometry for the libass backend.
 *
 * Nova hands the backend two independent pieces of geometry:
 *   - the CANVAS: the subtitle view's pixel size, which (use_sub_margins) may extend
 *     into the top/bottom letterbox bars, and
 *   - the VIDEO BOX: where the video itself sits inside that canvas.
 *
 * libass wants exactly that split, not a single frame size:
 *   ass_set_frame_size()   -> the whole canvas (bars included)
 *   ass_set_margins()      -> the bars, i.e. everything that is NOT video
 *   ass_set_use_margins(1) -> let regular (unpositioned) events use the bars
 *
 * With that, libass derives PlayRes -> pixel scaling from the *video box* rather than the
 * whole canvas (so text is the same size in portrait as in landscape), places \pos / \move
 * events relative to the video box (authored PlayRes coordinates keep their meaning), and
 * still lets ordinary bottom/top-aligned dialogue sit in the bars.
 *
 * No Nova dependencies on purpose: this is pure arithmetic + libass calls so it can be
 * unit-tested on a host against a real libass build.
 */
#include <ass/ass.h>

typedef struct {
    int frame_w, frame_h;                        // ass_set_frame_size
    int margin_t, margin_b, margin_l, margin_r;  // ass_set_margins
    int content_w, content_h;                    // video box size == what PlayRes maps onto
    int storage_w, storage_h;                    // ass_set_storage_size (0 = leave unset)
} SSA_GEOM;

/*
 * canvas_w/h        : size the subtitle view was laid out at (0 = not known yet)
 * box_x/y/w/h       : video's on-screen box within the canvas (w or h == 0 = not known yet)
 * real_w/h          : coded video size, used as libass "storage size" (0 = not known)
 *
 * Never returns negative margins. Negative margins mean "crop" to libass, and the canvas and
 * the box arrive through two separate callbacks (resize / set_video_box), so a momentarily
 * inconsistent pair is possible. Anything that doesn't fit falls back to "the video fills
 * the canvas", which is exactly what the backend did before boxes existed -- a safe,
 * bounded, self-correcting state.
 */
static inline SSA_GEOM ssa_geom_compute(int canvas_w, int canvas_h,
                                        int box_x, int box_y, int box_w, int box_h,
                                        int real_w, int real_h) {
    SSA_GEOM g;
    g.frame_w = canvas_w > 0 ? canvas_w : 1920;
    g.frame_h = canvas_h > 0 ? canvas_h : 1080;

    int fits = box_w > 0 && box_h > 0 && box_x >= 0 && box_y >= 0 &&
               box_x + box_w <= g.frame_w && box_y + box_h <= g.frame_h;
    if (!fits) { box_x = 0; box_y = 0; box_w = g.frame_w; box_h = g.frame_h; }

    g.margin_l = box_x;
    g.margin_t = box_y;
    g.margin_r = g.frame_w - box_x - box_w;
    g.margin_b = g.frame_h - box_y - box_h;
    g.content_w = box_w;
    g.content_h = box_h;

    // Storage size only matters to libass for border/shadow/blur scaling of scripts with
    // "ScaledBorderAndShadow: no" (and, unless we pin PAR, for anamorphic text stretch).
    // Guard against a coded size whose orientation disagrees with the box (e.g. rotated
    // video reported un-rotated): a wrong-orientation storage size would make the border
    // scale wrong by ~2x, which is worse than not setting it.
    g.storage_w = g.storage_h = 0;
    if (real_w > 0 && real_h > 0 && ((real_w >= real_h) == (box_w >= box_h))) {
        g.storage_w = real_w;
        g.storage_h = real_h;
    }
    return g;
}

static inline void ssa_geom_apply(ASS_Renderer *r, const SSA_GEOM *g) {
    ass_set_frame_size(r, g->frame_w, g->frame_h);
    ass_set_margins(r, g->margin_t, g->margin_b, g->margin_l, g->margin_r);
    ass_set_use_margins(r, 1);          // with zero margins this is a no-op
    // Pin PAR to 1. libass otherwise infers PAR = (box aspect) / (storage aspect) as soon as
    // a storage size is set, which would squeeze/stretch glyph WIDTH whenever the user's
    // aspect-ratio mode makes the box differ from the coded aspect. Storage size is here
    // only for border/shadow scaling; text shape is left alone.
    ass_set_pixel_aspect(r, 1.0);
    if (g->storage_w > 0) ass_set_storage_size(r, g->storage_w, g->storage_h);
    else                  ass_set_storage_size(r, 0, 0);
}

#endif
