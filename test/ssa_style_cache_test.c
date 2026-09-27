/*
 * Regression test for ssa_apply_geometry()'s style-cache invalidation.
 *
 * sync_styles() bakes forced FontSize (and Outline/Shadow) from
 * ssa_geom_short_side(canvas_w, canvas_h) / ctx->content_h, but only
 * re-runs when the style serial changes. ssa_apply_geometry() is
 * responsible for bumping that serial (via last_serial = 0) whenever
 * EITHER input moves, since a resize/box change can move one without
 * the other:
 *
 *   - toggling "render in black bars" off keeps the video's displayed
 *     height steady (content_h unchanged) while the canvas shrinks to
 *     match the video -- changing short_side alone.
 *   - a resize that keeps the video's aspect fit while growing the
 *     window can move content_h without changing which side is
 *     shorter.
 *
 * This test drives the REAL production path -- sub_format_ssa_create()'s
 * open()/resize()/render_at() -- across exactly that first transition
 * (bar-extended canvas -> video-only canvas, content_h held fixed) and
 * measures the rendered glyph bounding box, the same way
 * ssa_geometry_test.c measures ass_render_frame() output directly. If
 * the cache invalidation regresses, the forced FontSize from the OLD
 * short_side stays in effect and the measured text height won't move
 * even though geometry says it must.
 */
#include "sub_engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Defined in sub_format_ssa.c; not forward-declared in sub_format.h
 * (each backend .c is expected to be wired up via sub_format_register()
 * from init code we don't link here) -- declare it ourselves so the
 * test can drive the backend directly without pulling in sub_engine.c's
 * registration machinery. */
SUB_FORMAT_BACKEND *sub_format_ssa_create(void);

static const char *SCRIPT =
    "[Script Info]\n"
    "ScriptType: v4.00+\n"
    "PlayResX: 384\n"
    "PlayResY: 288\n"
    "ScaledBorderAndShadow: yes\n"
    "\n"
    "[V4+ Styles]\n"
    "Format: Name,Fontname,Fontsize,PrimaryColour,SecondaryColour,OutlineColour,BackColour,Bold,Italic,Underline,StrikeOut,ScaleX,ScaleY,Spacing,Angle,BorderStyle,Outline,Shadow,Alignment,MarginL,MarginR,MarginV,Encoding\n"
    "Style: Default,DejaVu Sans,18,&H00FFFFFF,&H00FFFFFF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,0,2,10,10,30,1\n"
    "\n"
    "[Events]\n"
    "Format: Layer,Start,End,Style,Name,MarginL,MarginR,MarginV,Effect,Text\n"
    "Dialogue: 0,0:00:00.00,0:00:05.00,Default,,0,0,0,,HHHH\n";

static int fails = 0;
static void check(const char *what, int got, int want, int tol) {
    if (got < want - tol || got > want + tol) {
        printf("   FAIL %-40s got %d, want %d (+/-%d)\n", what, got, want, tol);
        fails++;
    } else {
        printf("   ok   %-40s got %d (want %d +/-%d)\n", what, got, want, tol);
    }
}

/* Render at pts_ms and return the ink bounding-box height across every
 * SUB_EVENT_BITMAP event in the frame, 0 if nothing rendered. Mirrors
 * ssa_geometry_test.c's render()/BB approach, just measured off the
 * backend's own SUB_FRAME output instead of raw ASS_Image. */
static int rendered_ink_height(SUB_FORMAT_BACKEND *be, int64_t pts_ms) {
    SUB_FRAME *frame = be->render_at(be, pts_ms);
    if (!frame) {
        /* No change vs. the previous render at this backend. Force a
         * fresh measurement by nudging the pts slightly -- libass's
         * "change" flag is about content, and a resize alone doesn't
         * always trip it even though the CALLER (sub_engine.c) always
         * re-renders on the next poll anyway. */
        frame = be->render_at(be, pts_ms + 1);
    }
    if (!frame) return -1; /* genuinely nothing to measure */

    int y0 = 1 << 30, y1 = -1;
    for (SUB_EVENT *ev = frame->events; ev; ev = ev->next) {
        if (ev->kind != SUB_EVENT_BITMAP) continue;
        if (ev->y < y0) y0 = ev->y;
        if (ev->y + ev->h > y1) y1 = ev->y + ev->h;
    }
    be->free_frame(be, frame);
    if (y1 < 0) return -1;
    return y1 - y0;
}

int main(void) {
    /* --- Portrait phone, 16:9 video letterboxed into a taller canvas,
     * same geometry as ssa_geometry_test.c's first scenario: canvas
     * 1080x2400, video box (0,896) 1080x608 -- content_h = 608. --- */
    const int canvas_w = 1080, box_x = 0, box_y = 896, box_w = 1080, box_h = 608;
    const int bars_canvas_h = 2400;     /* bars included: short_side = 1080 (=canvas_w) */
    const int video_only_canvas_h = box_h; /* bars removed: canvas == box, short_side = 608 */

    SUB_USER_STYLE *style = sub_style_create();
    if (!style) { fprintf(stderr, "sub_style_create failed\n"); return 1; }
    /* is_plain_text_format below already forces force_all=1 regardless of
     * override_mode, but set an explicit font size so the forced value
     * (and therefore the bug, if reintroduced) is unambiguous. */
    sub_style_set_font_size(style, 55.0f);

    SUB_FORMAT_BACKEND *be = sub_format_ssa_create();
    if (!be) { fprintf(stderr, "sub_format_ssa_create failed\n"); return 1; }

    SUB_FORMAT_OPEN_PARAMS params;
    memset(&params, 0, sizeof(params));
    params.video_w = canvas_w;
    params.video_h = bars_canvas_h;
    params.video_box_x = box_x;
    params.video_box_y = box_y;
    params.video_box_w = box_w;
    params.video_box_h = box_h;
    params.codec_private = (const uint8_t *)SCRIPT;
    params.codec_private_size = (int)strlen(SCRIPT);
    params.user_style = style;
    params.is_plain_text_format = 0; /* real SSA script; force_all comes from override_mode==1 below */

    sub_style_set_override_mode(style, 1); /* ASS_OVERRIDE_FORCE -- exercise the force_all path */

    if (be->open(be, &params) != 0) { fprintf(stderr, "open failed\n"); return 1; }
    be->feed(be, (const uint8_t *)SCRIPT, (int)strlen(SCRIPT), 0, 5000);

    printf("== bar-extended canvas: %dx%d, box %dx%d @ (%d,%d)  content_h=%d short_side=%d\n",
           canvas_w, bars_canvas_h, box_w, box_h, box_x, box_y, box_h, canvas_w /* w < h here */);
    int h_before = rendered_ink_height(be, 0);
    check("ink height, bars-extended canvas", h_before, h_before, 0); /* just records it */

    /* --- The transition under test: canvas shrinks to exactly the video
     * box (bars removed), content_h held FIXED. This is the "toggle
     * render-in-black-bars off" case from the comment: content_h is
     * unchanged (still 608) but short_side moves from 1080 to 608. --- */
    be->resize(be, box_w, video_only_canvas_h);
    be->set_video_box(be, 0, 0, box_w, video_only_canvas_h);

    int h_after = rendered_ink_height(be, 0);

    printf("== video-only canvas:   %dx%d, box %dx%d @ (0,0)          content_h=%d short_side=%d\n",
           box_w, video_only_canvas_h, box_w, video_only_canvas_h, video_only_canvas_h, video_only_canvas_h);

    if (h_before <= 0 || h_after <= 0) {
        fprintf(stderr, "FAIL: expected on-screen text both before (%d) and after (%d) resize\n",
                h_before, h_after);
        return 1;
    }

    /* short_side went from 1080 -> 608, a ~1.776x shrink, so the forced
     * FontSize (which scales by short_side/content_h) must shrink by
     * roughly the same factor. Assert directional + rough magnitude
     * rather than an exact pixel count, to stay robust to font metrics:
     * the bug this guards against left h_after == h_before (no change
     * at all), which a >5% tolerance will not mask. */
    double ratio = (double)h_after / (double)h_before;
    double expected_ratio = (double)video_only_canvas_h / (double)canvas_w; /* 608/1080 */
    printf("   ink height: %d -> %d  (ratio %.3f, expected ~%.3f)\n",
           h_before, h_after, ratio, expected_ratio);

    if (h_after >= h_before) {
        printf("   FAIL ink height did not shrink after short_side changed "
               "(stale forced FontSize -- cache not invalidated)\n");
        fails++;
    } else {
        printf("   ok   ink height shrank after short_side changed\n");
    }
    check("ink height ratio tracks short_side ratio",
          (int)(ratio * 1000), (int)(expected_ratio * 1000), 250 /* generous: hinting/rounding */);

    /* --- Sanity check on the OTHER half of the fix: content_h moving
     * while short_side stays put must also invalidate. Grow the canvas
     * height (bars come back) while holding width -- and therefore
     * short_side, since width is still the shorter side -- fixed, but
     * change the box height so content_h moves. --- */
    be->resize(be, box_w, box_w * 3); /* short_side stays == box_w */
    be->set_video_box(be, 0, 0, box_w, box_w * 2); /* content_h changes: box_h -> box_w*2 */
    int h_content_only = rendered_ink_height(be, 0);
    printf("== content_h-only change: box_h %d -> %d, short_side fixed at %d\n",
           video_only_canvas_h, box_w * 2, box_w);
    if (h_content_only <= 0) {
        printf("   FAIL expected on-screen text after content_h-only change\n");
        fails++;
    } else if (h_content_only == h_after) {
        printf("   FAIL ink height unchanged when content_h moved alone "
               "(stale forced FontSize -- cache not invalidated)\n");
        fails++;
    } else {
        printf("   ok   ink height changed (%d -> %d) when content_h moved alone\n", h_after, h_content_only);
    }

    be->close(be);
    free(be);
    sub_style_destroy(style);

    printf("\n%s (%d failed checks)\n", fails ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED", fails);
    return fails != 0;
}
