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

// GFX (PGS/VobSub) placement, gfx_map_to_canvas() in gfx_layout.h.
//
// What must hold for a bitmap cue, whatever the video does:
//   - it lies fully inside the subtitle viewport, clear of the controls / system bars;
//   - it keeps its proportions (one uniform scale) and is never enlarged beyond the plain
//     "stream frame fits the viewport" scale;
//   - the video's own box (zoom, crop, Fit / Auto, forced aspect ratio) has NO influence;
//   - a cue that already fits stays where the authored position puts it, and returns there when
//     the controls hide;
//   - all rectangles of one cue share one scale and one translation.
//
// The viewport is the only thing a video mode changes, so the matrix below is over the viewport
// sizes SurfaceController produces for each mode (listed with each entry), in portrait and
// landscape, crossed with stream frames, cue shapes and the reserved bottom space (controls
// hidden / shown / system bar only).
#include "gfx_layout.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int fails = 0, checks = 0;
static void check(const char *what, int ok) {
    checks++;
    if (!ok) { fails++; if (fails < 40) printf("      FAIL %s\n", what); }
}
static int near(float a, float b, float tol) { return fabsf(a - b) <= tol; }

typedef struct { float x, y, w, h, sx, sy; } R;

static SUB_FRAME make_frame(int cw, int ch, int sw, int sh, int inset) {
    SUB_FRAME f; memset(&f, 0, sizeof f);
    f.video_w = cw; f.video_h = ch; f.gfx_stream_w = sw; f.gfx_stream_h = sh;
    f.video_box_x = 0; f.video_box_y = 0; f.video_box_w = cw; f.video_box_h = ch;
    f.gfx_inset_bottom = inset;
    return f;
}
static SUB_EVENT make_event(float fx, float fy, float fw, float fh, int sw, int sh) {   // fractions of the stream frame
    SUB_EVENT e; memset(&e, 0, sizeof e);
    e.kind = SUB_EVENT_BITMAP;
    e.x = (int)(fx * sw); e.y = (int)(fy * sh); e.w = (int)(fw * sw); e.h = (int)(fh * sh);
    return e;
}
static R map(const SUB_FRAME *f, const SUB_EVENT *e) {
    R r; gfx_map_to_canvas(f, e, &r.x, &r.y, &r.w, &r.h, &r.sx, &r.sy); return r;
}

typedef struct { const char *name; int w, h; } VIEWPORT;
typedef struct { const char *name; int w, h; } PLANE;
typedef struct { const char *name; float x, y, w, h; } CUE;

static const VIEWPORT VP[] = {
    // landscape 2404x1080 phone, 16:9 video
    { "landscape  Original            (video box, pillarbox)",        1920, 1080 },
    { "landscape  Fit width / Auto    (canvas clipped to screen)",    2404, 1080 },
    { "landscape  forced 2.39 / bars  (canvas extended to screen)",   2404, 1080 },
    { "landscape  forced 4:3          (pillarbox)",                   1440, 1080 },
    // portrait 1080x2400 phone, 16:9 video
    { "portrait   Original / forced   (canvas extended to screen)",   1080, 2400 },
    { "portrait   Original, bars off  (canvas = video box)",          1080,  607 },
    { "portrait   Fit / Auto          (canvas clipped to screen)",    1080, 2400 },
    { "portrait   forced 2.39         (canvas extended to screen)",   1080, 2400 },
};
static const PLANE PL[] = {
    { "PGS 1920x1080",  1920, 1080 },
    { "PGS 3840x2160",  3840, 2160 },
    { "VobSub 720x576",  720,  576 },
};
static const CUE CU[] = {
    { "bottom two-line cue",   0.20f, 0.82f, 0.60f, 0.10f },
    { "cue at the bottom edge",0.25f, 0.94f, 0.50f, 0.06f },
    { "top cue (signs)",       0.30f, 0.03f, 0.40f, 0.09f },
    { "near full-width cue",   0.01f, 0.85f, 0.98f, 0.10f },
    { "tall cue",              0.40f, 0.40f, 0.20f, 0.55f },
    { "small cue, mid-left",   0.05f, 0.45f, 0.10f, 0.05f },
    { "full-width cue (shrunk to the margins)", 0.00f, 0.84f, 1.00f, 0.10f },
    { "full-width tall cue (shrunk, high top)", 0.00f, 0.08f, 1.00f, 0.84f },
};
static const int INSET_PORTRAIT_FRAC[] = { 0, 1, 2 };    // 0 hidden, 1 controls, 2 system bar only

int main(void) {
    printf("\n== matrix: viewport x stream frame x cue x reserved space (controls hidden / shown / system bar)\n");
    for (unsigned v = 0; v < sizeof VP / sizeof *VP; v++) {
        const VIEWPORT *vp = &VP[v];
        int failed_before = fails;
        for (unsigned p = 0; p < sizeof PL / sizeof *PL; p++) {
            for (unsigned c = 0; c < sizeof CU / sizeof *CU; c++) {
                for (unsigned m = 0; m < 3; m++) {
                    const int mode = INSET_PORTRAIT_FRAC[m];
                    // controls ~ 17% of the viewport height (clamped), system bar ~ 6%
                    int inset = mode == 0 ? 0 : mode == 1 ? (int)(vp->h * 0.17f) : (int)(vp->h * 0.06f);
                    const PLANE *pl = &PL[p]; const CUE *cu = &CU[c];
                    char tag[160];
                    snprintf(tag, sizeof tag, "[%s | %s | %s | inset %d]", vp->name, pl->name, cu->name, inset);

                    SUB_FRAME f = make_frame(vp->w, vp->h, pl->w, pl->h, inset);
                    SUB_EVENT e = make_event(cu->x, cu->y, cu->w, cu->h, pl->w, pl->h);
                    f.events = &e;
                    R r = map(&f, &e);

                    float s0 = (float)vp->w / pl->w; if ((float)vp->h / pl->h < s0) s0 = (float)vp->h / pl->h;
                    const float mx = vp->w * GFX_EDGE_MARGIN, my = vp->h * GFX_EDGE_MARGIN, eps = 0.51f;
                    char msg[240];

                    // fully inside the viewport, clear of the reserved bottom space
                    snprintf(msg, sizeof msg, "%s inside viewport/clear of controls: x %.1f..%.1f y %.1f..%.1f", tag, r.x, r.x + r.w, r.y, r.y + r.h);
                    check(msg, r.x >= mx - eps && r.x + r.w <= vp->w - mx + eps &&
                               r.y >= my - eps && r.y + r.h <= vp->h - inset - my + eps);
                    // proportions preserved, one uniform scale, never enlarged
                    snprintf(msg, sizeof msg, "%s proportions: %.2fx%.2f from %dx%d", tag, r.w, r.h, e.w, e.h);
                    check(msg, near(r.sx, r.sy, 1e-6f) && near(r.w, e.w * r.sx, 0.01f) && near(r.h, e.h * r.sy, 0.01f));
                    snprintf(msg, sizeof msg, "%s never larger than the contain scale (%.4f > %.4f)", tag, r.sx, s0);
                    check(msg, r.sx <= s0 + 1e-5f);

                    // the video's own box has no influence, whatever it is
                    static const int BOXES[][4] = { {0,0,1,1}, {-1593,0,4267,2400}, {0,-136,2404,1352}, {500,500,10,10}, {-9999,-9999,99999,99999} };
                    for (unsigned b = 0; b < sizeof BOXES / sizeof *BOXES; b++) {
                        SUB_FRAME g = f;
                        g.video_box_x = BOXES[b][0]; g.video_box_y = BOXES[b][1];
                        g.video_box_w = BOXES[b][2]; g.video_box_h = BOXES[b][3];
                        R q = map(&g, &e);
                        snprintf(msg, sizeof msg, "%s independent of the video box #%u", tag, b);
                        check(msg, near(q.x, r.x, 1e-4f) && near(q.y, r.y, 1e-4f) && near(q.w, r.w, 1e-4f) && near(q.h, r.h, 1e-4f));
                    }

                    // a cue that fits where it was authored is not touched; hiding the controls
                    // brings every cue back to its authored place unless it must shrink/move for the viewport itself
                    SUB_FRAME h = f; h.gfx_inset_bottom = 0;
                    R n = map(&h, &e);
                    const float ox = (vp->w - pl->w * s0) * 0.5f, oy = (vp->h - pl->h * s0) * 0.5f;
                    const float nx = ox + e.x * s0, ny = oy + e.y * s0;
                    int nominal_fits = nx >= mx && nx + e.w * s0 <= vp->w - mx && ny >= my && ny + e.h * s0 <= vp->h - my;
                    if (nominal_fits) {
                        snprintf(msg, sizeof msg, "%s controls hidden: authored place kept (%.1f,%.1f vs %.1f,%.1f)", tag, n.x, n.y, nx, ny);
                        check(msg, near(n.x, nx, 0.01f) && near(n.y, ny, 0.01f) && near(n.sx, s0, 1e-6f));
                        if (inset > 0) {
                            int nominal_clear = ny + e.h * s0 <= vp->h - inset - my;
                            if (nominal_clear) {
                                snprintf(msg, sizeof msg, "%s controls shown but cue already clear: not moved", tag);
                                check(msg, near(r.x, nx, 0.01f) && near(r.y, ny, 0.01f));
                            } else if (e.h * s0 <= vp->h - inset - 2 * my) {
                                snprintf(msg, sizeof msg, "%s controls shown: moved up, not shrunk", tag);
                                check(msg, near(r.sx, s0, 1e-5f) && r.y < ny && near(r.x, nx, 0.01f));
                            }
                        }
                    }
                }
            }
        }
        printf("   %-62s %s\n", vp->name, fails == failed_before ? "ok" : "FAILED");
    }

    // one cue, several rectangles: same scale and translation, relative layout preserved
    printf("\n== cue with several bitmap events moves and scales as one unit\n");
    for (unsigned v = 0; v < sizeof VP / sizeof *VP; v++) {
        for (int inset = 0; inset <= 400; inset += 200) {
            const int eff = inset > VP[v].h * GFX_MAX_INSET ? (int)(VP[v].h * GFX_MAX_INSET) : inset;   // capped, see gfx_layout.h
            SUB_FRAME f = make_frame(VP[v].w, VP[v].h, 1920, 1080, inset);
            SUB_EVENT a = make_event(0.30f, 0.80f, 0.40f, 0.05f, 1920, 1080);
            SUB_EVENT b = make_event(0.20f, 0.90f, 0.60f, 0.07f, 1920, 1080);
            a.next = &b; f.events = &a;
            R ra = map(&f, &a), rb = map(&f, &b);
            char msg[200]; snprintf(msg, sizeof msg, "[%s inset %d] same scale", VP[v].name, inset);
            check(msg, near(ra.sx, rb.sx, 1e-6f));
            snprintf(msg, sizeof msg, "[%s inset %d] relative offset scaled uniformly", VP[v].name, inset);
            check(msg, near(rb.x - ra.x, (b.x - a.x) * ra.sx, 0.02f) && near(rb.y - ra.y, (b.y - a.y) * ra.sx, 0.02f));
            snprintf(msg, sizeof msg, "[%s inset %d] both inside", VP[v].name, inset);
            check(msg, ra.y >= 0 && rb.y + rb.h <= VP[v].h - eff + 0.51f && ra.x >= 0 && rb.x + rb.w <= VP[v].w + 0.51f);
        }
    }

    // The user's vertical position (gfx_vpos): relative to the nominal place, positive up, negative
    // down. +100 puts the TOP of the cue at the top margin and -100 its BOTTOM at the bottom
    // margin (controls hidden); in between is that fraction of the distance available. The
    // controls then clear the cue afterwards without redefining the value.
    printf("\n== user vertical position: viewport x stream frame x cue x reserved space x vpos\n");
    {
        static const int VPOS[] = { -100, -75, -50, -25, -10, -1, 0, 1, 10, 25, 50, 75, 100 };
        const int NV = (int)(sizeof VPOS / sizeof *VPOS);
        for (unsigned v = 0; v < sizeof VP / sizeof *VP; v++) {
            const VIEWPORT *vp = &VP[v];
            int failed_before = fails;
            for (unsigned p = 0; p < sizeof PL / sizeof *PL; p++) {
                for (unsigned c = 0; c < sizeof CU / sizeof *CU; c++) {
                    for (unsigned m = 0; m < 3; m++) {
                        int inset = m == 0 ? 0 : m == 1 ? (int)(vp->h * 0.17f) : (int)(vp->h * 0.06f);
                        const PLANE *pl = &PL[p]; const CUE *cu = &CU[c];
                        char tag[200], msg[300];
                        snprintf(tag, sizeof tag, "[%s | %s | %s | inset %d]", vp->name, pl->name, cu->name, inset);
                        SUB_EVENT e = make_event(cu->x, cu->y, cu->w, cu->h, pl->w, pl->h);
                        SUB_FRAME base = make_frame(vp->w, vp->h, pl->w, pl->h, inset);
                        base.events = &e;
                        R r0 = map(&base, &e);                       // vpos 0
                        const float my = vp->h * GFX_EDGE_MARGIN, mx = vp->w * GFX_EDGE_MARGIN, eps = 0.51f;

                        // what the cue does with the controls hidden: the reference for the exact reach
                        SUB_FRAME hid0 = base; hid0.gfx_inset_bottom = 0;
                        R h0 = map(&hid0, &e);
                        // the exact formula holds on the cue's FITTED bounds with the controls hidden (h0), whether
                        // or not the fit moved or shrunk it (a wide cue is shrunk to the side margins)
                        const int exact = 1;
                        const float top_room = h0.y - my, bot_room = (vp->h - my) - (h0.y + h0.h);

                        float prev_y = 1e9f;
                        for (int i = 0; i < NV; i++) {
                            SUB_FRAME f = base; f.gfx_vpos = VPOS[i];
                            R r = map(&f, &e);
                            snprintf(msg, sizeof msg, "%s vpos %d inside viewport / clear of controls (x %.1f..%.1f y %.1f..%.1f)",
                                     tag, VPOS[i], r.x, r.x + r.w, r.y, r.y + r.h);
                            check(msg, r.x >= mx - eps && r.x + r.w <= vp->w - mx + eps &&
                                       r.y >= my - eps && r.y + r.h <= vp->h - inset - my + eps);
                            snprintf(msg, sizeof msg, "%s vpos %d size/scale unchanged by the offset", tag, VPOS[i]);
                            check(msg, near(r.sx, r0.sx, 1e-6f) && near(r.sy, r0.sy, 1e-6f) && near(r.w, r0.w, 1e-3f) && near(r.h, r0.h, 1e-3f));
                            snprintf(msg, sizeof msg, "%s vpos %d horizontal position unchanged", tag, VPOS[i]);
                            check(msg, near(r.x, r0.x, 1e-3f));
                            snprintf(msg, sizeof msg, "%s vpos %d monotonic (y %.2f after %.2f)", tag, VPOS[i], r.y, prev_y);
                            check(msg, r.y <= prev_y + 1e-3f);
                            prev_y = r.y;
                            if (VPOS[i] > 0) { snprintf(msg, sizeof msg, "%s vpos %d never lower than 0 (%.2f vs %.2f)", tag, VPOS[i], r.y, r0.y); check(msg, r.y <= r0.y + 1e-3f); }
                            if (VPOS[i] < 0) { snprintf(msg, sizeof msg, "%s vpos %d never higher than 0 (%.2f vs %.2f)", tag, VPOS[i], r.y, r0.y); check(msg, r.y >= r0.y - 1e-3f); }
                            if (VPOS[i] == 0) { snprintf(msg, sizeof msg, "%s vpos 0 is the plain placement", tag); check(msg, near(r.y, r0.y, 1e-6f)); }

                            // controls hidden: the exact edge-relative rule, both ends reach their margin
                            SUB_FRAME hid = f; hid.gfx_inset_bottom = 0;
                            R rh = map(&hid, &e);
                            if (exact) {
                                float want = VPOS[i] > 0 ? h0.y - VPOS[i] * 0.01f * (top_room > 0 ? top_room : 0)
                                           : VPOS[i] < 0 ? h0.y + (-VPOS[i]) * 0.01f * (bot_room > 0 ? bot_room : 0) : h0.y;
                                snprintf(msg, sizeof msg, "%s vpos %d controls hidden: %.2f of the room (y %.2f, want %.2f)", tag, VPOS[i], VPOS[i] / 100.0f, rh.y, want);
                                check(msg, near(rh.y, want, 0.02f));
                                snprintf(msg, sizeof msg, "%s vpos %d controls hidden: the fitted size is kept", tag, VPOS[i]);
                                check(msg, near(rh.sx, h0.sx, 1e-6f) && near(rh.h, h0.h, 1e-3f));
                                if (VPOS[i] == 100 && top_room > 0.05f) { snprintf(msg, sizeof msg, "%s +100 puts the top at the top margin (%.2f vs %.2f)", tag, rh.y, my); check(msg, near(rh.y, my, 0.02f)); }
                                if (VPOS[i] == -100 && bot_room > 0.05f) { snprintf(msg, sizeof msg, "%s -100 puts the bottom at the bottom margin (%.2f vs %.2f)", tag, rh.y + rh.h, vp->h - my); check(msg, near(rh.y + rh.h, vp->h - my, 0.02f)); }
                            }
                            // controls shown = the same cue cleared afterwards: wherever the controls hidden put it,
                            // moved up only as far as needed (never shrunk here unless it cannot fit at all)
                            if (inset > 0 && near(r.sx, rh.sx, 1e-6f)) {
                                float limit = vp->h - inset - my - rh.h;           // lowest top that is still clear
                                float want = rh.y < limit ? rh.y : limit;
                                snprintf(msg, sizeof msg, "%s vpos %d controls shown: hidden place, cleared afterwards (y %.2f, want %.2f)", tag, VPOS[i], r.y, want);
                                check(msg, near(r.y, want, 0.02f) && near(r.x, rh.x, 0.01f));
                            }
                        }
                    }
                }
            }
            printf("   %-62s %s\n", vp->name, fails == failed_before ? "ok" : "FAILED");
        }

        // exact reach, with concrete numbers
        printf("\n   reach: +100 / -100 take the cue to the top / bottom margin\n");
        {
            char msg[200];
            // portrait 1080x2400, PGS 1920x1080
            SUB_FRAME f = make_frame(1080, 2400, 1920, 1080, 0);
            SUB_EVENT e = make_event(0.25f, 0.88f, 0.50f, 0.06f, 1920, 1080);
            f.events = &e;
            R r0 = map(&f, &e);
            f.gfx_vpos = 100;  R up = map(&f, &e);
            f.gfx_vpos = -100; R dn = map(&f, &e);
            snprintf(msg, sizeof msg, "portrait +100: top margin (%.2f)", up.y);   check(msg, near(up.y, 2400 * GFX_EDGE_MARGIN, 0.02f));
            snprintf(msg, sizeof msg, "portrait -100: bottom margin (%.2f)", dn.y + dn.h); check(msg, near(dn.y + dn.h, 2400 * (1.0f - GFX_EDGE_MARGIN), 0.02f));
            f.gfx_vpos = 50; R h_up = map(&f, &e); f.gfx_vpos = -50; R h_dn = map(&f, &e);
            snprintf(msg, sizeof msg, "+50 / -50 are half of each side's distance (%.2f, %.2f)", h_up.y, h_dn.y);
            check(msg, near(h_up.y, (r0.y + up.y) * 0.5f, 0.02f) && near(h_dn.y, (r0.y + dn.y) * 0.5f, 0.02f));
            snprintf(msg, sizeof msg, "the two sides have different distances (up %.1f, down %.1f)", r0.y - up.y, dn.y - r0.y);
            check(msg, (r0.y - up.y) > (dn.y - r0.y) + 100.0f);
            // landscape 2404x1080: a bottom cue has little room below, a lot above
            f = make_frame(2404, 1080, 1920, 1080, 0); f.events = &e;
            r0 = map(&f, &e); f.gfx_vpos = -100; dn = map(&f, &e);
            printf("   landscape: -100 moves the cue down by %.1f px (all the room there is)\n", dn.y - r0.y);
            check("landscape -100 reaches the bottom margin", near(dn.y + dn.h, 1080 * (1.0f - GFX_EDGE_MARGIN), 0.02f));
            // a cue already at the bottom margin cannot go further down; one at the top cannot go further up
            SUB_EVENT low = make_event(0.25f, 0.94f, 0.50f, 0.06f, 1920, 1080); low.h = 1080 - low.y;  // reaches the frame edge
            f.events = &low; f.gfx_vpos = 0; r0 = map(&f, &low); f.gfx_vpos = -100; dn = map(&f, &low);
            check("a cue already at the bottom margin does not move down", near(dn.y, r0.y, 1e-3f));
            SUB_EVENT top = make_event(0.25f, 0.0f, 0.50f, 0.06f, 1920, 1080);
            f.events = &top; f.gfx_vpos = 0; r0 = map(&f, &top); f.gfx_vpos = 100; up = map(&f, &top);
            check("a cue already at the top margin does not move up", near(up.y, r0.y, 1e-3f));
            // a cue above a third of the screen height (portrait Fit / Auto): +vpos still moves it up, relative to where it is
            f = make_frame(1080, 2404, 1920, 1080, 0);
            SUB_EVENT hi = make_event(0.20f, 0.88f, 0.60f, 0.06f, 1920, 1080); f.events = &hi;
            r0 = map(&f, &hi); f.gfx_vpos = 100; up = map(&f, &hi);
            check("starts above a third of the screen height", 2404 - (r0.y + r0.h) > 2404 / 3.0f);
            check("+100 is higher still, at the top margin", up.y < r0.y - 700.0f && near(up.y, 2404 * GFX_EDGE_MARGIN, 0.02f));
        }

        // a wide cue that is SHRUNK to fit the side margins: the range is measured on the fitted
        // (shrunk) bounds, so +100 still reaches the top margin and -100 the bottom one
        printf("   wide cue shrunk to the side margins: +100 / -100 still reach the margins\n");
        {
            char msg[240];
            // 1920x1080 viewport and stream: the frame fills the viewport, a full-width cue is wider than the
            // area (98%), so it is shrunk about its bottom centre and its top drops from 100 to about 118
            SUB_FRAME f = make_frame(1920, 1080, 1920, 1080, 0);
            SUB_EVENT w = make_event(0.0f, 100.0f / 1080.0f, 1.0f, 900.0f / 1080.0f, 1920, 1080);
            f.events = &w;
            R r0 = map(&f, &w);
            printf("   fitted top at 0: %.2f px (unshrunk top would be 100), scale %.4f\n", r0.y, r0.sx);
            check("the cue is shrunk by the side margins", r0.sx < 0.99f && r0.y > 105.0f);
            f.gfx_vpos = 100;  R up = map(&f, &w);
            f.gfx_vpos = -100; R dn = map(&f, &w);
            snprintf(msg, sizeof msg, "+100: top at the top margin (%.2f, want %.2f)", up.y, 1080 * GFX_EDGE_MARGIN);
            check(msg, near(up.y, 1080 * GFX_EDGE_MARGIN, 0.02f));
            snprintf(msg, sizeof msg, "-100: bottom at the bottom margin (%.2f, want %.2f)", dn.y + dn.h, 1080 * (1.0f - GFX_EDGE_MARGIN));
            check(msg, near(dn.y + dn.h, 1080 * (1.0f - GFX_EDGE_MARGIN), 0.02f));
            check("shrunk size unchanged by the position", near(up.sx, r0.sx, 1e-6f) && near(dn.sx, r0.sx, 1e-6f) && near(up.h, r0.h, 1e-3f));
            f.gfx_vpos = 50; R half = map(&f, &w);
            check("+50 is half of the distance from the FITTED top", near(half.y, (r0.y + up.y) * 0.5f, 0.02f));
            // controls shown: the same saved value, cleared afterwards
            for (int v = -100; v <= 100; v += 50) {
                f.gfx_vpos = v; f.gfx_inset_bottom = 0;   R h = map(&f, &w);
                f.gfx_inset_bottom = 180;                 R c = map(&f, &w);
                float lim = 1080 - 180 - 1080 * GFX_EDGE_MARGIN;
                snprintf(msg, sizeof msg, "controls shown, vpos %d: the hidden place cleared afterwards (%.2f vs hidden %.2f)", v, c.y + c.h, h.y + h.h);
                // shrunk further only because it no longer fits above the controls; otherwise moved up only as far as needed
                check(msg, c.y >= 1080 * GFX_EDGE_MARGIN - 0.51f && c.y + c.h <= lim + 0.51f && c.sx <= h.sx + 1e-6f);
            }
            // portrait: a full-width cue is the same story (frame width = viewport width)
            f = make_frame(1080, 2400, 1920, 1080, 0);
            SUB_EVENT pw = make_event(0.0f, 0.60f, 1.0f, 0.30f, 1920, 1080); f.events = &pw;
            f.gfx_vpos = 100; up = map(&f, &pw); f.gfx_vpos = -100; dn = map(&f, &pw);
            check("portrait wide cue: +100 reaches the top margin", near(up.y, 2400 * GFX_EDGE_MARGIN, 0.02f));
            check("portrait wide cue: -100 reaches the bottom margin", near(dn.y + dn.h, 2400 * (1.0f - GFX_EDGE_MARGIN), 0.02f));
            // an upper and a lower bitmap, the union wide: one shrunk group, bounded as a whole
            f = make_frame(1920, 1080, 1920, 1080, 0);
            SUB_EVENT ua = make_event(0.00f, 0.10f, 0.50f, 0.06f, 1920, 1080), ub = make_event(0.50f, 0.85f, 0.50f, 0.10f, 1920, 1080);
            ua.next = &ub; f.events = &ua;
            f.gfx_vpos = 100;  R a1 = map(&f, &ua), b1 = map(&f, &ub);
            f.gfx_vpos = -100; R a2 = map(&f, &ua), b2 = map(&f, &ub);
            check("wide group: +100 puts the upper bitmap on the top margin", near(a1.y, 1080 * GFX_EDGE_MARGIN, 0.02f));
            check("wide group: -100 puts the lower bitmap on the bottom margin", near(b2.y + b2.h, 1080 * (1.0f - GFX_EDGE_MARGIN), 0.02f));
            check("wide group: spacing kept", near(b1.y - a1.y, b2.y - a2.y, 0.02f));
        }

        // a cue with an upper AND a lower bitmap (signs + dialogue): they move as one group, the
        // whole group is bounded, so the slider can run out before its end -- and the controls clear it
        printf("   cue with an upper and a lower bitmap: one group, bounded as a whole, with and without controls\n");
        {
            static const struct { const char *name; float uy; } UP[] = { {"upper well below the top", 0.20f}, {"upper near the top", 0.02f}, {"upper at the top", 0.0f} };
            for (unsigned v = 0; v < sizeof VP / sizeof *VP; v++) {
                const VIEWPORT *vp = &VP[v];
                for (unsigned p = 0; p < sizeof PL / sizeof *PL; p++) {
                    for (unsigned u = 0; u < sizeof UP / sizeof *UP; u++) {
                        for (unsigned m = 0; m < 3; m++) {
                            const PLANE *pl = &PL[p];
                            const int inset = m == 0 ? 0 : m == 1 ? (int)(vp->h * 0.17f) : (int)(vp->h * 0.06f);
                            const float my = vp->h * GFX_EDGE_MARGIN, mx = vp->w * GFX_EDGE_MARGIN, eps = 0.51f;
                            SUB_EVENT a = make_event(0.30f, UP[u].uy, 0.40f, 0.06f, pl->w, pl->h);    // upper
                            SUB_EVENT b = make_event(0.20f, 0.88f, 0.60f, 0.08f, pl->w, pl->h);       // lower
                            a.next = &b;
                            SUB_FRAME base = make_frame(vp->w, vp->h, pl->w, pl->h, inset); base.events = &a;
                            SUB_FRAME hid0 = base; hid0.gfx_inset_bottom = 0;
                            R ha0 = map(&hid0, &a), hb0 = map(&hid0, &b);              // nominal place, controls hidden
                            float s0 = (float)vp->w / pl->w; if ((float)vp->h / pl->h < s0) s0 = (float)vp->h / pl->h;
                            const int fits = 1;                                         // the formula is on the fitted bounds, shrunk or not
                            char tag[200], msg[300];
                            snprintf(tag, sizeof tag, "[%s | %s | %s | inset %d]", vp->name, pl->name, UP[u].name, inset);
                            const float top_room = ha0.y - my, bot_room = (vp->h - my) - (hb0.y + hb0.h);
                            for (int vpos = -100; vpos <= 100; vpos += 25) {
                                SUB_FRAME f = base; f.gfx_vpos = vpos;
                                R ra = map(&f, &a), rb = map(&f, &b);
                                snprintf(msg, sizeof msg, "%s vpos %d: one scale, layout kept", tag, vpos);
                                check(msg, near(ra.sx, rb.sx, 1e-6f) && near(rb.x - ra.x, (b.x - a.x) * ra.sx, 0.02f) && near(rb.y - ra.y, (b.y - a.y) * ra.sx, 0.02f));
                                snprintf(msg, sizeof msg, "%s vpos %d: BOTH bitmaps inside, clear of the controls (a %.1f..%.1f, b %.1f..%.1f)", tag, vpos, ra.y, ra.y + ra.h, rb.y, rb.y + rb.h);
                                check(msg, ra.y >= my - eps && ra.x >= mx - eps && rb.y + rb.h <= vp->h - inset - my + eps && rb.x + rb.w <= vp->w - mx + eps &&
                                           rb.y >= my - eps && ra.y + ra.h <= vp->h - inset - my + eps);
                                SUB_FRAME hid = f; hid.gfx_inset_bottom = 0;
                                R rha = map(&hid, &a), rhb = map(&hid, &b);
                                if (fits && vpos != 0) {
                                    // controls hidden: the group's top / bottom is what limits it
                                    float want = vpos > 0 ? ha0.y - vpos * 0.01f * (top_room > 0 ? top_room : 0)
                                                          : ha0.y + (-vpos) * 0.01f * (bot_room > 0 ? bot_room : 0);
                                    snprintf(msg, sizeof msg, "%s vpos %d hidden: group moved %.0f%% of the room (upper y %.2f, want %.2f)", tag, vpos, vpos / 1.0f, rha.y, want);
                                    check(msg, near(rha.y, want, 0.03f));
                                    check("group keeps its spacing while moving", near(rhb.y - rha.y, hb0.y - ha0.y, 0.03f));
                                    if (vpos == 100 && top_room > 0.05f) { snprintf(msg, sizeof msg, "%s +100: the UPPER bitmap sits on the top margin (%.2f)", tag, rha.y); check(msg, near(rha.y, my, 0.03f)); }
                                    if (vpos == -100 && bot_room > 0.05f) { snprintf(msg, sizeof msg, "%s -100: the LOWER bitmap sits on the bottom margin (%.2f)", tag, rhb.y + rhb.h); check(msg, near(rhb.y + rhb.h, vp->h - my, 0.03f)); }
                                    // controls shown: the same group, cleared afterwards (moved up only as far as needed, together)
                                    if (inset > 0 && near(ra.sx, rha.sx, 1e-6f)) {
                                        float lim = vp->h - inset - my - (rhb.y + rhb.h - rha.y) - rha.y;   // how far its top may drop
                                        float dy = lim < 0 ? lim : 0;
                                        snprintf(msg, sizeof msg, "%s vpos %d controls shown: group cleared afterwards (a %.2f, want %.2f)", tag, vpos, ra.y, rha.y + dy);
                                        check(msg, near(ra.y, rha.y + dy, 0.03f) && near(rb.y - ra.y, rhb.y - rha.y, 0.03f));
                                    }
                                }
                            }
                        }
                    }
                }
            }
            // concrete limit case: landscape, upper bitmap close to the top: the whole group has only a few pixels
            SUB_FRAME f = make_frame(2404, 1080, 1920, 1080, 0);
            SUB_EVENT a = make_event(0.30f, 0.02f, 0.40f, 0.06f, 1920, 1080), b = make_event(0.20f, 0.88f, 0.60f, 0.08f, 1920, 1080);
            a.next = &b; f.events = &a;
            R a0 = map(&f, &a), b0 = map(&f, &b);
            f.gfx_vpos = 100; R a1 = map(&f, &a), b1 = map(&f, &b);
            printf("   landscape, upper bitmap 2%% from the top: +100 moves the group %.1f px (the lower one, with lots of room, follows)\n", b0.y - b1.y);
            check("the upper bitmap stops at the top margin", near(a1.y, 1080 * GFX_EDGE_MARGIN, 0.02f));
            check("the lower bitmap moved exactly as far as the upper one could", near(b0.y - b1.y, a0.y - a1.y, 0.02f));
            // and with the controls shown the saved value is the same: only the clearance differs
            f.gfx_inset_bottom = 180; f.gfx_vpos = -100; R as = map(&f, &a), bs = map(&f, &b);
            check("controls shown, -100: group sits above the controls", bs.y + bs.h <= 1080 - 180 + 0.51f && as.y >= 1080 * GFX_EDGE_MARGIN - 0.51f);
        }

        // the video's box still has no influence, with an offset applied
        {
            SUB_EVENT e = make_event(0.25f, 0.88f, 0.50f, 0.06f, 1920, 1080);
            static const int BOXES[][4] = { {0,0,1,1}, {-1593,0,4267,2400}, {0,-136,2404,1352}, {-9999,-9999,99999,99999} };
            for (int v = -100; v <= 100; v += 100) {
                SUB_FRAME f = make_frame(1080, 2400, 1920, 1080, 300);
                f.events = &e; f.gfx_vpos = v;
                R r = map(&f, &e);
                for (unsigned b = 0; b < sizeof BOXES / sizeof *BOXES; b++) {
                    SUB_FRAME g = f; g.video_box_x = BOXES[b][0]; g.video_box_y = BOXES[b][1];
                    g.video_box_w = BOXES[b][2]; g.video_box_h = BOXES[b][3];
                    R q = map(&g, &e);
                    check("vpos: independent of the video box", near(q.x, r.x, 1e-4f) && near(q.y, r.y, 1e-4f) && near(q.w, r.w, 1e-4f));
                }
            }
        }

        // degenerate inputs
        {
            SUB_EVENT e = make_event(0.25f, 0.88f, 0.50f, 0.06f, 1920, 1080);
            SUB_FRAME f = make_frame(1080, 2400, 1920, 1080, 0); f.events = &e;
            f.gfx_vpos = 100;  R a = map(&f, &e);  f.gfx_vpos = 5000;  R b = map(&f, &e);
            check("vpos above 100 behaves as 100", near(a.y, b.y, 1e-4f));
            f.gfx_vpos = -100; a = map(&f, &e);  f.gfx_vpos = -5000; b = map(&f, &e);
            check("vpos below -100 behaves as -100", near(a.y, b.y, 1e-4f));
            SUB_EVENT huge = make_event(0.0f, 0.0f, 1.0f, 1.0f, 1920, 1080);                 // the whole frame, which fills the viewport: no room either way
            f = make_frame(1920, 1080, 1920, 1080, 0);
            f.events = &huge; f.gfx_vpos = 0; a = map(&f, &huge);
            f.gfx_vpos = 100; b = map(&f, &huge);  check("a cue filling the viewport has nothing to move up into", near(a.y, b.y, 1e-3f) && !isnan(b.y));
            f.gfx_vpos = -100; b = map(&f, &huge); check("... or down into", near(a.y, b.y, 1e-3f) && !isnan(b.y));
            SUB_FRAME u = make_frame(0, 0, 0, 0, 0); u.events = &e; u.gfx_vpos = 100;
            R id = map(&u, &e);
            check("unknown canvas/stream: identity even with an offset", near(id.x, e.x, 1e-4f) && near(id.y, e.y, 1e-4f));
        }
    }

    // the reported case: 3840x2160 file with baked-in bars, Fit width on a 2404x1080 phone
    printf("\n== reported case: PGS 3840x2160, bottom-bar cue, landscape Fit width (viewport 2404x1080)\n");
    {
        SUB_FRAME f = make_frame(2404, 1080, 3840, 2160, 0);
        SUB_EVENT e = make_event(800.0f / 3840, 1950.0f / 2160, 2240.0f / 3840, 120.0f / 2160, 3840, 2160);
        f.events = &e;
        R r = map(&f, &e);
        printf("   cue y %.1f..%.1f of 1080 (was 1084..1160 off screen before), x %.1f..%.1f, scale %.3f\n", r.y, r.y + r.h, r.x, r.x + r.w, r.sx);
        check("reported case fully visible", r.y >= 0 && r.y + r.h <= 1080);
        f.gfx_inset_bottom = 180;
        r = map(&f, &e);
        printf("   with controls (180px): cue y %.1f..%.1f (clear of %d)\n", r.y, r.y + r.h, 1080 - 180);
        check("reported case clear of controls", r.y + r.h <= 1080 - 180);
    }

    // shrink only when the cue cannot fit
    printf("\n== shrink: cue larger than the space left above the controls\n");
    {
        SUB_FRAME f = make_frame(2404, 1080, 1920, 1080, 500);          // ~560px left
        SUB_EVENT e = make_event(0.10f, 0.20f, 0.80f, 0.70f, 1920, 1080);   // 756px tall nominal
        f.events = &e;
        R r = map(&f, &e);
        printf("   scale %.3f (contain %.3f), y %.1f..%.1f, bottom limit %.1f\n", r.sx, 1.0f, r.y, r.y + r.h, 1080 - 500 - 1080 * GFX_EDGE_MARGIN);
        check("shrunk below nominal", r.sx < 1.0f);
        check("proportions kept", near(r.w / r.h, (float)e.w / e.h, 0.01f));
        check("fits above the controls", r.y + r.h <= 1080 - 500 + 0.51f && r.y >= 0);
        check("horizontally centred about its own centre", near(r.x + r.w * 0.5f, 2404 * 0.5f, 1.0f));
    }

    // degenerate inputs: no NaN, no crash
    printf("\n== degenerate inputs\n");
    {
        SUB_FRAME f = make_frame(0, 0, 0, 0, 50);
        SUB_EVENT e = make_event(0.1f, 0.1f, 0.2f, 0.2f, 100, 100);
        f.events = &e;
        R r = map(&f, &e);
        check("unknown canvas/stream: identity", near(r.x, e.x, 1e-4f) && near(r.sx, 1.0f, 1e-6f) && !isnan(r.y));
        f = make_frame(1080, 2400, 1920, 1080, 100000);                   // absurd inset
        f.events = &e;
        r = map(&f, &e);
        check("absurd inset stays on screen", r.y >= 0 && r.y + r.h <= 2400 && !isnan(r.y));
        check("absurd inset is capped to half the viewport", r.y + r.h <= 2400 * (1.0f - GFX_MAX_INSET) + 0.51f);
        f = make_frame(1080, 2400, 1920, 1080, -50);                      // negative inset
        f.events = &e;
        r = map(&f, &e);
        check("negative inset treated as 0", r.y >= 0 && !isnan(r.y));
    }

    printf("\n%s (%d checks, %d failed)\n", fails ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED", checks, fails);
    return fails != 0;
}
