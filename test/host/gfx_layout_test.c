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
