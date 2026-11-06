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
 *   1b. USER POSITION: the user's vertical position (gfx_vpos, -100..100) shifts the whole
 *      cue vertically from that nominal place; 0 changes nothing. It is always RELATIVE to the
 *      nominal place, so a positive value only ever moves the cue up and a negative one only
 *      ever down, whatever the nominal place is:
 *        +100 = up by gfx_vpos_range pixels (the player passes a third of the screen height),
 *        -100 = down until a typical bottom-placed cue (one authored GFX_TYPICAL_BOTTOM_MARGIN
 *               of the frame height above the frame's bottom edge) reaches the bottom limit of
 *               the viewport -- the room below the cue depends a lot on the orientation (large
 *               in portrait, small in landscape), so a fixed fraction would leave most of the
 *               negative range dead on one of them. The reference ignores the reserved bottom
 *               space on purpose: the controls clear the cue in step 2, they do not rescale
 *               the slider.
 *      Every cue of a track is shifted by the same amount, so authored relative positions
 *      (a top cue and a bottom cue) are preserved; step 2 then keeps each one on screen.
 *
 *   2. FIT: the cue must lie inside the AVAILABLE area: the viewport minus a small margin, and
 *      minus gfx_inset_bottom at the bottom (playback controls / system bars). A cue that
 *      does not fit is first moved (up above the controls, down from the top edge, in from the
 *      sides); only if it is larger than the area itself is it shrunk, uniformly, about its
 *      bottom centre (so it stays low, where subtitles are expected). A cue that already fits
 *      is left exactly where the authored position puts it. With the controls hidden the
 *      inset is 0 and the cue returns to its nominal place.
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

// Where a "typical" bottom-placed cue ends, as a fraction of the stream frame height above the
// frame's bottom edge: the reference a vpos of -100 moves to the bottom limit. A tunable
// assumption -- authoring tools place dialogue roughly 5-9% up -- not a measured value.
#define GFX_TYPICAL_BOTTOM_MARGIN 0.06f

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

    // 1b. the user's vertical position: one vertical shift for the whole cue (see above).
    float shift = 0.0f;
    {
        int v = f->gfx_vpos;
        if (v > 100) v = 100;
        if (v < -100) v = -100;
        if (v > 0) {
            float range = (float)f->gfx_vpos_range;
            if (range < 0.0f) range = 0.0f;
            if (range > ch * 2.0f) range = ch * 2.0f;        // a bogus range can't fling the cue
            shift = -(float)v * 0.01f * range;
        } else if (v < 0) {
            const float ref  = oy + fh * s0 * (1.0f - GFX_TYPICAL_BOTTOM_MARGIN);
            float room = ch - ch * GFX_EDGE_MARGIN - ref;    // down to the bottom limit, controls hidden
            if (room < 0.0f) room = 0.0f;
            shift = (float)(-v) * 0.01f * room;
        }
    }
    const float oy_s = oy + shift;

    // Union of the frame's bitmap events (always including ev), nominal placement + shift.
    float ux0 = ox + ev->x * s0,            uy0 = oy_s + ev->y * s0;
    float ux1 = ux0 + ev->w * s0,           uy1 = uy0 + ev->h * s0;
    for (const SUB_EVENT *e = f->events; e; e = e->next) {
        if (e->kind != SUB_EVENT_BITMAP) continue;
        const float x0 = ox + e->x * s0, y0 = oy_s + e->y * s0;
        if (x0 < ux0) ux0 = x0;
        if (y0 < uy0) uy0 = y0;
        if (x0 + e->w * s0 > ux1) ux1 = x0 + e->w * s0;
        if (y0 + e->h * s0 > uy1) uy1 = y0 + e->h * s0;
    }

    // 2. fit into the available area.
    float inset = (float)f->gfx_inset_bottom;
    if (inset < 0.0f) inset = 0.0f;
    if (inset > ch * GFX_MAX_INSET) inset = ch * GFX_MAX_INSET;
    const float ax0 = cw * GFX_EDGE_MARGIN, ax1 = cw - cw * GFX_EDGE_MARGIN;
    const float ay0 = ch * GFX_EDGE_MARGIN, ay1 = ch - inset - ch * GFX_EDGE_MARGIN;

    const float uw = ux1 - ux0, uh = uy1 - uy0;
    float k = 1.0f;                                  // uniform shrink, never an enlargement
    if (uw > ax1 - ax0) k = (ax1 - ax0) / uw;
    if (uh > ay1 - ay0 && (ay1 - ay0) / uh < k) k = (ay1 - ay0) / uh;

    const float anchor_x = (ux0 + ux1) * 0.5f, anchor_y = uy1;   // bottom centre of the union
    const float nx0 = anchor_x + (ux0 - anchor_x) * k, nx1 = anchor_x + (ux1 - anchor_x) * k;
    const float ny0 = anchor_y + (uy0 - anchor_y) * k, ny1 = anchor_y;

    float dx = 0.0f, dy = 0.0f;
    if (nx1 > ax1) dx = ax1 - nx1; else if (nx0 < ax0) dx = ax0 - nx0;
    if (ny1 > ay1) dy = ay1 - ny1; else if (ny0 < ay0) dy = ay0 - ny0;

    const float ex = ox + ev->x * s0, ey = oy_s + ev->y * s0;
    *out_x = anchor_x + (ex - anchor_x) * k + dx;
    *out_y = anchor_y + (ey - anchor_y) * k + dy;
    *out_w = ev->w * s0 * k;
    *out_h = ev->h * s0 * k;
    *out_sx = *out_sy = s0 * k;
}

#endif
