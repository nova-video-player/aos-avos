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

/*
 * External ASS script that arrives AFTER the first render (the ordering that left "Custom"
 * settings ignored until the user touched a setting).
 *
 * An external .ass track is opened empty, the render thread starts polling immediately, and the
 * file is fed a moment later. sync_styles() caches its baseline against the (empty) track on that
 * first render; the late [Script Info] feed must invalidate that cache. The check is behavioural,
 * so it needs no knowledge of the cache's internals: render the same script and the same Custom
 * (forced font size) settings in two orderings and compare the ink.
 *
 *   control : open -> feed script -> first render
 *   late    : open -> first render (nothing to draw) -> feed script -> render
 *
 * With the bug, "late" keeps drawing with the script's own style, so its height differs from
 * "control". The unforced run proves the forced size is visibly different from the script's own,
 * i.e. the comparison can actually fail.
 */
#include "sub_engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } \
                           else { printf("ok:   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Ink bounding-box height of everything rendered at pts_ms; -1 if nothing was drawn. */
static int ink_height(SUB_FORMAT_BACKEND *be, int64_t pts_ms) {
    SUB_FRAME *frame = be->render_at(be, pts_ms);
    if (!frame) frame = be->render_at(be, pts_ms + 1);   /* "unchanged" is about content; nudge once */
    if (!frame) return -1;
    int y0 = 1 << 30, y1 = -1;
    for (SUB_EVENT *ev = frame->events; ev; ev = ev->next) {
        if (ev->kind != SUB_EVENT_BITMAP) continue;
        if (ev->y < y0) y0 = ev->y;
        if (ev->y + ev->h > y1) y1 = ev->y + ev->h;
    }
    be->free_frame(be, frame);
    return y1 < 0 ? -1 : y1 - y0;
}

/* One scenario: Custom settings = forced font size `font` with override mode `override_mode`. */
static int scenario(int render_before_script, int override_mode, float font) {
    SUB_USER_STYLE *style = sub_style_create();
    sub_style_set_font_size(style, font);
    sub_style_set_override_mode(style, override_mode);

    SUB_FORMAT_BACKEND *be = sub_format_ssa_create();
    SUB_FORMAT_OPEN_PARAMS params;
    memset(&params, 0, sizeof(params));
    params.video_w = 1280;  params.video_h = 720;
    params.user_style = style;
    params.is_plain_text_format = 0;               /* a real ASS script, like an external .ass file */
    if (be->open(be, &params) != 0) { fprintf(stderr, "open failed\n"); exit(1); }

    if (render_before_script) {
        /* The render thread's first poll: the track has no script yet. */
        SUB_FRAME *f = be->render_at(be, 0);
        if (f) be->free_frame(be, f);
    }
    be->feed(be, (const uint8_t *)SCRIPT, (int)strlen(SCRIPT), 0, 5000);   /* the file arrives */
    int h = ink_height(be, 1000);

    be->close(be);
    free(be);
    sub_style_destroy(style);
    return h;
}

int main(void) {
    const float CUSTOM = 90.0f;
    const int FORCE = 1;                            /* ASS_OVERRIDE_FORCE, as in ssa_style_cache_test */

    int control  = scenario(0, FORCE, CUSTOM);
    int late     = scenario(1, FORCE, CUSTOM);
    int unforced = scenario(0, 0, CUSTOM);
    printf("   ink height: control %d, late %d, unforced %d\n", control, late, unforced);

    CHECK(control > 0 && late > 0 && unforced > 0, "every ordering drew text (control %d, late %d, unforced %d)", control, late, unforced);
    CHECK(abs(control - unforced) > 1,
          "forced Custom size is visibly different from the script's own (%d vs %d) -- the comparison below can fail", control, unforced);
    CHECK(abs(late - control) <= 1,
          "script loaded after the first render: Custom settings applied without touching a setting (late %d, control %d)", late, control);
    CHECK(abs(late - unforced) > 1,
          "late-loaded script is NOT stuck on the script's own style (late %d, unforced %d)", late, unforced);

    printf("%s\n", fails ? "FAILED" : "ALL PASSED");
    return fails != 0;
}
