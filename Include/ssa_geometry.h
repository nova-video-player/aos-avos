// Copyright 2026 Courville Software
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

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
    int layout_h;                                // height libass scales ORDINARY dialogue against
                                                 // (its "fit" height). == content_h except when the
                                                 // box overflows the frame vertically, where it is
                                                 // the frame height. \pos events still use content_h.
    int storage_w, storage_h;                    // ass_set_storage_size (0 = leave unset)
} SSA_GEOM;

/*
 * canvas_w/h        : size the subtitle view was laid out at (0 = not known yet)
 * box_x/y/w/h       : video's on-screen box within the canvas (w or h == 0 = not known yet)
 * real_w/h          : coded video size, used as libass "storage size" (0 = not known)
 *
 * Margins are never negative EXCEPT for one deliberate shape: the box overflows the canvas
 * (see below). Negative margins mean "crop" to libass, and the canvas and the box arrive
 * through two separate callbacks (resize / set_video_box), so a momentarily inconsistent pair
 * is possible. Anything else that doesn't fit falls back to "the video fills the canvas",
 * which is exactly what the backend did before boxes existed -- a safe, bounded,
 * self-correcting state.
 *
 * Overflow: when the video is scaled past the screen and cropped ("Fit width" on a wide phone
 * overflows vertically; Fit / Auto in portrait overflows horizontally), the player clips the
 * canvas to the visible screen and reports a box that is larger than it on that axis, centered,
 * with a negative x or y. Mapping that to negative libass margins makes libass scale PlayRes
 * and place \pos / \move against the video (the picture keeps its alignment with signs and
 * typesetting), while ordinary dialogue, which use_margins places against the frame, stays
 * inside the visible area. Per axis, the box must either lie inside the canvas, or COVER it
 * (start at or before the edge, end at or after the other), be centered on it, and be at most
 * SSA_GEOM_MAX_OVERFLOW times its length, so a stale box from a previous layout (for example
 * one from before a rotation) can't slip through.
 */
#define SSA_GEOM_MAX_OVERFLOW 8

static inline int ssa_geom_axis_ok(int pos, int size, int frame) {
    if (pos >= 0 && pos + size <= frame) return 1;                       // inside
    // Overflow: the box covers the canvas and is CENTERED on it (the player always centres the
    // video in the subtitle canvas), within a couple of pixels of integer rounding. The
    // symmetry is what rejects a stale box from before a rotation.
    int off = 2 * pos + size - frame;
    return size > frame && size <= frame * SSA_GEOM_MAX_OVERFLOW &&
           pos <= 0 && pos + size >= frame && off >= -2 && off <= 2;
}

static inline SSA_GEOM ssa_geom_compute(int canvas_w, int canvas_h,
                                        int box_x, int box_y, int box_w, int box_h,
                                        int real_w, int real_h) {
    SSA_GEOM g;
    g.frame_w = canvas_w > 0 ? canvas_w : 1920;
    g.frame_h = canvas_h > 0 ? canvas_h : 1080;

    int fits = box_w > 0 && box_h > 0 &&
               ssa_geom_axis_ok(box_x, box_w, g.frame_w) &&
               ssa_geom_axis_ok(box_y, box_h, g.frame_h);
    if (!fits) { box_x = 0; box_y = 0; box_w = g.frame_w; box_h = g.frame_h; }

    g.margin_l = box_x;
    g.margin_t = box_y;
    g.margin_r = g.frame_w - box_x - box_w;
    g.margin_b = g.frame_h - box_y - box_h;
    g.content_w = box_w;
    g.content_h = box_h;
    // Mirror of ass_reconfigure()'s fit_height: regular (non-\pos) events are scaled to the box
    // fitted inside the frame, letterboxed or pillarboxed. For the usual letterbox case this is
    // exactly box_h; box taller than the frame (vertical overflow) -> frame height; box wider
    // than the frame (horizontal overflow, portrait) -> the box height scaled down to the
    // frame width.
    {
        long long cw = (long long)box_w;
        g.layout_h = (cw * g.frame_h <= (long long)box_h * g.frame_w)
                         ? g.frame_h
                         : (int)((double)box_h * g.frame_w / box_w + 0.5);
    }

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

// The screen (canvas) dimension text sizing scales off: the shorter of the two canvas sides.
// Pulled out to its own function because it's needed in two places -- ssa_apply_geometry()
// (to know when to invalidate the forced-style cache) and sync_styles() (to compute
// font_res_scale) -- and those two MUST agree on the exact same value or the cache
// invalidation check silently stops catching the case it exists for. min(w, h) rather than h
// alone because it's the one canvas dimension that survives a device rotation unchanged
// (canvas_w and canvas_h swap, but whichever one was smaller still is), so text sized off it
// doesn't jump between portrait and landscape on the same physical screen.
static inline int ssa_geom_short_side(int canvas_w, int canvas_h) {
    if (canvas_w <= 0 || canvas_h <= 0) return 0;
    return canvas_w < canvas_h ? canvas_w : canvas_h;
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
