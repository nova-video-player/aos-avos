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

#ifndef GFX_LAYOUT_H
#define GFX_LAYOUT_H
/*
 * GFX (PGS/VobSub) placement on the subtitle canvas. Pure arithmetic with no GL/EGL
 * dependency so the host tests can exercise it; shared by the GL path and the CPU blender
 * (3D) in sub_render_gl.c so they can't drift apart.
 *
 * ev->x/y/w/h are in the subtitle stream's own coordinate frame (gfx_stream_w x gfx_stream_h:
 * the PGS PCS plane / the VobSub DVD canvas). The canvas (video_w x video_h on the frame) is the
 * subtitle VIEWPORT: whatever area SurfaceController gave the subtitle view -- the visible
 * screen when the video is zoomed or cropped, the video box when subtitles may not use the
 * bars, the whole area when they may.
 *
 * Placement is decided by the viewport and the stream frame ONLY. The video's own box (zoom,
 * crop, Fit / Auto, forced aspect ratio) plays no part: a zoomed or stretched picture must not
 * enlarge a subtitle or push it off screen. Readability wins over exact alignment of the
 * authored position with the video (positioned ASS/SSA, which does follow the video, is a
 * different backend).
 *
 *   1. NOMINAL: the stream frame is scaled uniformly to fit inside the viewport (contain) and
 *      centred in it. The cue keeps its authored place in that frame and its authored size,
 *      scaled by that single factor -- so a cue is never stretched.
 *
 *   2. FIT (margins): the cue must lie inside the viewport minus a small margin on every side.
 *      A cue that does not fit is first moved (down from the top edge, in from the sides);
 *      only if it is larger than that area is it shrunk, uniformly, about its bottom centre
 *      (so it stays low, where subtitles are expected). A cue that already fits is left
 *      exactly where the authored position puts it. These are the cue's FITTED bounds with
 *      the controls hidden: what the user position is measured on.
 *
 *   3. USER POSITION: the user's vertical position (gfx_vpos, -100..100) shifts the whole
 *      fitted cue vertically; 0 changes nothing. It is always RELATIVE to the fitted place, so
 *      a positive value only ever moves the cue up and a negative one only ever down:
 *        +100 = up until the TOP of the cue reaches the top margin of the viewport,
 *        -100 = down until the BOTTOM of the cue reaches the bottom margin of the viewport,
 *        in between = that fraction of the distance available in that direction.
 *      The distance is measured on the fitted bounds, so a cue that was shrunk to fit the side
 *      margins still reaches the margin exactly (measuring the unshrunk cue would stop it
 *      short). "The cue" is the union of the frame's bitmap events, so an upper and a lower
 *      bitmap move together with their layout intact and the limit is set by whichever reaches
 *      the margin first. The distance is measured with the controls HIDDEN: the saved value
 *      means the same thing whether or not the controls are showing. Because the distance is
 *      what is left in each direction, a step up and a step down are generally not the same
 *      size, and a cue already at a margin has nothing left to move towards it. Everything is
 *      in viewport fractions: no pixel range comes from the player, so a resize cannot leave
 *      the two out of step.
 *
 *   4. CONTROL CLEARANCE: the cue, moved vertically by what steps 2 and 3 moved it, is fitted
 *      again, now also keeping gfx_inset_bottom (playback controls / system bars) clear at the
 *      bottom: it is moved up above the controls, and only if it is larger than the area left
 *      is it shrunk, again about its bottom centre. A cue that fits keeps exactly the place
 *      steps 2 and 3 gave it, and returns there when the controls hide. Near the bottom
 *      several user positions can therefore land on the same place while the controls are up.
 *      With the controls hidden the inset is 0 and this step changes nothing.
 *
 * codec_ffsub blends every rectangle of a cue into ONE bounding-box bitmap, so a cue is a
 * single event and its rectangles share one scale and translation by construction. The code
 * below still works on the union of the frame's bitmap events, so a frame with several events
 * (should a backend ever produce one) is moved and scaled as one unit with its relative
 * layout preserved.
 */
#include "sub_types.h"

// Fraction of the viewport kept clear at each edge.
#define GFX_EDGE_MARGIN 0.01f

// Cap on the reserved bottom space, as a fraction of the viewport height, so a bogus inset
// can never leave no room for the cue.
#define GFX_MAX_INSET 0.5f

// A cue's union (the frame's bitmap events together) and the event being mapped, as boxes in
// viewport pixels, plus the total uniform scale applied so far on top of the nominal one.
typedef struct { float ux0, uy0, ux1, uy1, ex, ey, ew, eh, k; } GFX_BOX;

// Fit the union into the area ax0..ax1 x ay0..ay1: move it, and shrink it uniformly about its
// bottom centre only if it is larger than the area. The event is carried along.
static inline void gfx_fit_box(GFX_BOX *b, float ax0, float ay0, float ax1, float ay1) {
    const float uw = b->ux1 - b->ux0, uh = b->uy1 - b->uy0;
    float k = 1.0f;                                  // uniform shrink, never an enlargement
    if (uw > ax1 - ax0) k = (ax1 - ax0) / uw;
    if (uh > ay1 - ay0 && (ay1 - ay0) / uh < k) k = (ay1 - ay0) / uh;

    const float anchor_x = (b->ux0 + b->ux1) * 0.5f, anchor_y = b->uy1;   // bottom centre of the union
    const float nx0 = anchor_x + (b->ux0 - anchor_x) * k, nx1 = anchor_x + (b->ux1 - anchor_x) * k;
    const float ny0 = anchor_y + (b->uy0 - anchor_y) * k, ny1 = anchor_y;

    float dx = 0.0f, dy = 0.0f;
    if (nx1 > ax1) dx = ax1 - nx1; else if (nx0 < ax0) dx = ax0 - nx0;
    if (ny1 > ay1) dy = ay1 - ny1; else if (ny0 < ay0) dy = ay0 - ny0;

    b->ex = anchor_x + (b->ex - anchor_x) * k + dx;
    b->ey = anchor_y + (b->ey - anchor_y) * k + dy;
    b->ew *= k; b->eh *= k;
    b->ux0 = nx0 + dx; b->ux1 = nx1 + dx; b->uy0 = ny0 + dy; b->uy1 = ny1 + dy;
    b->k *= k;
}

static inline void gfx_map_to_canvas(const SUB_FRAME *f, const SUB_EVENT *ev,
                                     float *out_x, float *out_y, float *out_w, float *out_h,
                                     float *out_sx, float *out_sy) {
    const float cw = (float)f->video_w, ch = (float)f->video_h;                // viewport
    const float fw = (float)f->gfx_stream_w, fh = (float)f->gfx_stream_h;     // stream frame

    if (cw <= 0.0f || ch <= 0.0f || fw <= 0.0f || fh <= 0.0f) {                // nothing known
        *out_x = (float)ev->x; *out_y = (float)ev->y;
        *out_w = (float)ev->w; *out_h = (float)ev->h;
        *out_sx = *out_sy = 1.0f;
        return;
    }

    // 1. nominal placement: contain-fit the stream frame in the viewport, centred.
    float s0 = cw / fw;
    if (ch / fh < s0) s0 = ch / fh;
    const float ox = (cw - fw * s0) * 0.5f, oy = (ch - fh * s0) * 0.5f;

    // Union of the frame's bitmap events (always including ev), nominal placement.
    GFX_BOX b;
    b.ex = ox + ev->x * s0; b.ey = oy + ev->y * s0; b.ew = ev->w * s0; b.eh = ev->h * s0; b.k = 1.0f;
    b.ux0 = b.ex;           b.uy0 = b.ey;           b.ux1 = b.ex + b.ew; b.uy1 = b.ey + b.eh;
    for (const SUB_EVENT *e = f->events; e; e = e->next) {
        if (e->kind != SUB_EVENT_BITMAP) continue;
        const float x0 = ox + e->x * s0, y0 = oy + e->y * s0;
        if (x0 < b.ux0) b.ux0 = x0;
        if (y0 < b.uy0) b.uy0 = y0;
        if (x0 + e->w * s0 > b.ux1) b.ux1 = x0 + e->w * s0;
        if (y0 + e->h * s0 > b.uy1) b.uy1 = y0 + e->h * s0;
    }

    // 2. the cue's fitted bounds with the controls hidden: the nominal cue fitted into the
    // viewport minus the margins. Measured on a copy; it only decides how far the cue may move.
    const float ax0 = cw * GFX_EDGE_MARGIN, ax1 = cw - cw * GFX_EDGE_MARGIN;
    const float my  = ch * GFX_EDGE_MARGIN;
    GFX_BOX hid = b;
    gfx_fit_box(&hid, ax0, my, ax1, ch - my);

    // 3. the user's vertical position: one vertical shift for the whole union, a fraction of the
    // distance left, from the fitted bounds, to the viewport's top / bottom margin.
    float shift = 0.0f;
    {
        int v = f->gfx_vpos;
        if (v > 100) v = 100;
        if (v < -100) v = -100;
        if (v > 0) {
            float room = hid.uy0 - my;                                 // up to the top margin
            if (room < 0.0f) room = 0.0f;
            shift = -(float)v * 0.01f * room;
        } else if (v < 0) {
            float room = (ch - my) - hid.uy1;                          // down to the bottom margin
            if (room < 0.0f) room = 0.0f;
            shift = (float)(-v) * 0.01f * room;
        }
    }
    // The nominal cue goes where the hidden fit put it (a vertical move, if the fit made one)
    // plus the user's shift. With the controls hidden the fit below then changes nothing, so the
    // result is the fitted cue plus the shift exactly; and at 0 it is the plain placement.
    shift += hid.uy1 - b.uy1;
    b.uy0 += shift; b.uy1 += shift; b.ey += shift;

    // 4. control clearance: fit with the reserved bottom space taken off the bottom.
    float inset = (float)f->gfx_inset_bottom;
    if (inset < 0.0f) inset = 0.0f;
    if (inset > ch * GFX_MAX_INSET) inset = ch * GFX_MAX_INSET;
    gfx_fit_box(&b, ax0, my, ax1, ch - inset - my);

    *out_x = b.ex;
    *out_y = b.ey;
    *out_w = b.ew;
    *out_h = b.eh;
    *out_sx = *out_sy = s0 * b.k;
}

#endif
