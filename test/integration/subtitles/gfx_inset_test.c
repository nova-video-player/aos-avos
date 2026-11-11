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

// The real GFX backend and the three layout inputs it forwards: the reserved bottom space
// (controls / system bars), the user's vertical position and the user's scale. The value is stamped on the frames
// it hands out, a change re-stamps the cue (so it moves when the controls appear and returns
// when they hide), an unchanged value does nothing, and a cue that is opened or fed later still
// gets the current value.
#include "sub_format.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern SUB_FORMAT_BACKEND *sub_format_gfx_create(void);
// stubs.c's stand-in for sub_frame_unref() cannot free the shared pixel blocks the GFX backend
// uses, and the real one lives in sub_engine.c with the whole engine. These are the same
// implementations (sub_engine.c), overriding the weak stand-in.
void sub_frame_ref(SUB_FRAME *frame) { if (frame) atomic_fetch_add(&frame->refcount, 1); }
void sub_frame_unref(SUB_FRAME *frame) {
    if (!frame) return;
    if (atomic_fetch_sub(&frame->refcount, 1) == 1) {
        for (SUB_EVENT *ev = frame->events, *next; ev; ev = next) {
            next = ev->next;
            if (ev->pixel_refs) { if (atomic_fetch_sub(ev->pixel_refs, 1) == 1) free(ev->pixel_refs); }
            else if (ev->kind == SUB_EVENT_BITMAP && ev->data.bitmap.rgba) free((void *)ev->data.bitmap.rgba);
            free(ev);
        }
        free(frame);
    }
}

static int fails = 0, checks = 0;
#define CHECK(c, msg) do { checks++; if (!(c)) { printf("FAIL: %s\n", msg); fails++; } else printf("ok:   %s\n", msg); } while (0)

static SUB_FORMAT_BACKEND *open_backend(int inset) {
    SUB_FORMAT_BACKEND *be = sub_format_gfx_create();
    SUB_FORMAT_OPEN_PARAMS p; memset(&p, 0, sizeof p);
    p.video_w = 1080; p.video_h = 2400; p.real_video_w = 1920; p.real_video_h = 1080;
    p.bottom_inset = inset;
    if (!be || be->open(be, &p) != 0) { printf("FAIL: open\n"); exit(1); }
    return be;
}
static void feed(SUB_FORMAT_BACKEND *be) {
    uint8_t px[8 * 4 * 4]; memset(px, 0xFF, sizeof px);
    be->feed_bitmap(be, px, 8, 4, 8 * 4, 0 /* any colorspace: pixels are not inspected */, 100, 900, 0, -1);   // unbounded cue
}

int main(void) {
    SUB_FORMAT_BACKEND *be = open_backend(0);
    feed(be);
    SUB_FRAME *f1 = be->render_at(be, 100);
    CHECK(f1 && f1->gfx_inset_bottom == 0 && f1->gfx_stream_w == 1920, "cue stamped with no reserved space");
    const uint8_t *pix1 = f1->events->data.bitmap.rgba;

    be->set_bottom_inset(be, 300);                       // controls appear
    SUB_FRAME *f2 = be->render_at(be, 100);
    CHECK(f2 && f2 != f1 && f2->gfx_inset_bottom == 300, "controls shown: cue re-stamped with the reserved space");
    CHECK(f1->gfx_inset_bottom == 0, "the frame the renderer already holds is untouched (copy on write)");
    CHECK(f2->events->data.bitmap.rgba == pix1, "pixels are shared, not copied");

    be->set_bottom_inset(be, 300);                       // same value again (layout pass)
    CHECK(be->render_at(be, 100) == NULL, "unchanged value: no redraw");

    be->set_bottom_inset(be, 0);                         // controls hide
    SUB_FRAME *f3 = be->render_at(be, 100);
    CHECK(f3 && f3->gfx_inset_bottom == 0, "controls hidden: cue back to no reserved space");

    be->set_bottom_inset(be, -40);                       // bogus value
    CHECK(be->render_at(be, 100) == NULL, "negative value is treated as 0 (no change from 0)");

    be->set_bottom_inset(be, 120);                       // system bar only
    SUB_FRAME *f4 = be->render_at(be, 100);
    CHECK(f4 && f4->gfx_inset_bottom == 120, "system bar value applied");

    feed(be);                                            // next cue while the value is set
    SUB_FRAME *f5 = be->render_at(be, 100);
    CHECK(f5 && f5->gfx_inset_bottom == 120, "a cue fed later gets the current value");

    for (SUB_FRAME *f = f1; f; f = (f == f1) ? f2 : (f == f2) ? f3 : (f == f3) ? f4 : (f == f4) ? f5 : NULL) sub_frame_unref(f);
    be->close(be); free(be);

    SUB_FORMAT_BACKEND *be2 = open_backend(250);         // track opened while the controls are up
    feed(be2);
    SUB_FRAME *g1 = be2->render_at(be2, 100);
    CHECK(g1 && g1->gfx_inset_bottom == 250, "a track opened with the controls up starts with the reserved space");
    sub_frame_unref(g1);
    be2->close(be2); free(be2);

    // The user's vertical position (set_gfx_position): same contract as the inset.
    SUB_FORMAT_BACKEND *be3 = open_backend(0);
    CHECK(be3->set_gfx_position != NULL, "GFX backend implements set_gfx_position");
    feed(be3);
    SUB_FRAME *v1 = be3->render_at(be3, 100);
    CHECK(v1 && v1->gfx_vpos == 0, "cue stamped with no user offset by default");
    const uint8_t *vpix = v1->events->data.bitmap.rgba;

    be3->set_gfx_position(be3, 40);                      // slider moved up
    SUB_FRAME *v2 = be3->render_at(be3, 100);
    CHECK(v2 && v2 != v1 && v2->gfx_vpos == 40, "slider moved: cue re-stamped with the offset");
    CHECK(v1->gfx_vpos == 0, "the frame the renderer already holds is untouched (copy on write)");
    CHECK(v2->events->data.bitmap.rgba == vpix, "pixels are shared, not copied");

    be3->set_gfx_position(be3, 40);                      // same value again (re-applied on a layout pass)
    CHECK(be3->render_at(be3, 100) == NULL, "unchanged offset: no redraw");

    be3->set_bottom_inset(be3, 300);                     // controls appear: the offset must survive
    SUB_FRAME *v3 = be3->render_at(be3, 100);
    CHECK(v3 && v3->gfx_inset_bottom == 300 && v3->gfx_vpos == 40,
          "controls shown: the user's offset is kept next to the reserved space");

    be3->set_gfx_position(be3, 7000);                    // bogus values are clamped
    SUB_FRAME *v4 = be3->render_at(be3, 100);
    CHECK(v4 && v4->gfx_vpos == 100, "out-of-range offset clamped to +100");
    be3->set_gfx_position(be3, -7000);
    SUB_FRAME *v5 = be3->render_at(be3, 100);
    CHECK(v5 && v5->gfx_vpos == -100, "offset below -100 clamped");

    be3->set_gfx_position(be3, -30);
    feed(be3);                                           // next cue while the offset is set
    SUB_FRAME *v6 = be3->render_at(be3, 100);
    CHECK(v6 && v6->gfx_vpos == -30 && v6->gfx_inset_bottom == 300,
          "a cue fed later gets the current offset");
    for (SUB_FRAME *f = v1; f; f = (f == v1) ? v2 : (f == v2) ? v3 : (f == v3) ? v4 : (f == v4) ? v5 : (f == v5) ? v6 : NULL) sub_frame_unref(f);
    be3->close(be3); free(be3);

    SUB_FORMAT_BACKEND *be4 = sub_format_gfx_create();   // track opened with the offset already set
    SUB_FORMAT_OPEN_PARAMS p4; memset(&p4, 0, sizeof p4);
    p4.video_w = 1080; p4.video_h = 2400; p4.real_video_w = 1920; p4.real_video_h = 1080;
    p4.gfx_vpos = -60;
    be4->open(be4, &p4);
    feed(be4);
    SUB_FRAME *w1 = be4->render_at(be4, 100);
    CHECK(w1 && w1->gfx_vpos == -60, "a track opened with an offset starts with it");
    sub_frame_unref(w1);
    be4->close(be4); free(be4);

    // The user's scale (set_gfx_scale): same contract again, a third independent input.
    SUB_FORMAT_BACKEND *be5 = open_backend(0);
    CHECK(be5->set_gfx_scale != NULL, "GFX backend implements set_gfx_scale");
    feed(be5);
    SUB_FRAME *s1 = be5->render_at(be5, 100);
    CHECK(s1 && s1->gfx_scale == 100, "cue stamped with 100% by default (params left unset)");
    const uint8_t *spix = s1->events->data.bitmap.rgba;

    be5->set_gfx_scale(be5, 150);                        // slider moved
    SUB_FRAME *s2 = be5->render_at(be5, 100);
    CHECK(s2 && s2 != s1 && s2->gfx_scale == 150, "slider moved: cue re-stamped with the scale");
    CHECK(s1->gfx_scale == 100, "the frame the renderer already holds is untouched (copy on write)");
    CHECK(s2->events->data.bitmap.rgba == spix, "pixels are shared, not copied");
    CHECK(s2->gfx_vpos == 0 && s2->gfx_inset_bottom == 0, "scale does not touch the position or the reserved space");

    be5->set_gfx_scale(be5, 150);                        // same value again (re-applied on a layout pass)
    CHECK(be5->render_at(be5, 100) == NULL, "unchanged scale: no redraw");

    be5->set_gfx_position(be5, -40);                     // the other two inputs must survive each other
    SUB_FRAME *s3 = be5->render_at(be5, 100);
    CHECK(s3 && s3->gfx_vpos == -40 && s3->gfx_scale == 150, "position changed: scale kept");
    be5->set_bottom_inset(be5, 300);
    SUB_FRAME *s4 = be5->render_at(be5, 100);
    CHECK(s4 && s4->gfx_inset_bottom == 300 && s4->gfx_vpos == -40 && s4->gfx_scale == 150,
          "controls shown: scale kept next to the position and the reserved space");
    be5->set_gfx_scale(be5, 60);
    SUB_FRAME *s5 = be5->render_at(be5, 100);
    CHECK(s5 && s5->gfx_scale == 60 && s5->gfx_vpos == -40 && s5->gfx_inset_bottom == 300,
          "scale changed: position and reserved space kept");

    be5->set_gfx_scale(be5, 7000);                       // bogus values are clamped
    SUB_FRAME *s6 = be5->render_at(be5, 100);
    CHECK(s6 && s6->gfx_scale == SUB_GFX_SCALE_MAX, "out-of-range scale clamped to 300");
    be5->set_gfx_scale(be5, 10);
    SUB_FRAME *s7 = be5->render_at(be5, 100);
    CHECK(s7 && s7->gfx_scale == SUB_GFX_SCALE_MIN, "scale below 50 clamped to 50");
    be5->set_gfx_scale(be5, 120);                        // away from the minimum, so the next clamp is a real change
    SUB_FRAME *s8a = be5->render_at(be5, 100);
    CHECK(s8a && s8a->gfx_scale == 120, "scale moved off the minimum");
    be5->set_gfx_scale(be5, -7000);
    SUB_FRAME *s8 = be5->render_at(be5, 100);
    CHECK(s8 && s8->gfx_scale == SUB_GFX_SCALE_MIN, "a negative scale set on the live backend clamps to 50 as well");
    be5->set_gfx_scale(be5, 0);
    CHECK(be5->render_at(be5, 100) == NULL, "0 set on the live backend is out of range, not 'unset': it clamps to 50 (unchanged, no redraw)");

    be5->set_gfx_scale(be5, 200);
    feed(be5);                                           // next cue while the scale is set
    SUB_FRAME *s9 = be5->render_at(be5, 100);
    CHECK(s9 && s9->gfx_scale == 200 && s9->gfx_vpos == -40 && s9->gfx_inset_bottom == 300,
          "a cue fed later gets the current scale");
    {
        SUB_FRAME *all[] = { s1, s2, s3, s4, s5, s6, s7, s8a, s8, s9 };
        for (unsigned i = 0; i < sizeof all / sizeof *all; i++) sub_frame_unref(all[i]);
    }
    be5->close(be5); free(be5);

    // a track opened with the scale already set starts with it; unset opens at 100, out of range is clamped
    static const struct { int given, want; const char *what; } OPEN[] = {
        { 75,   75,  "a track opened with a scale starts with it" },
        { 0,    100, "a track opened with no scale (unset) starts at 100" },
        { 9999, SUB_GFX_SCALE_MAX, "a track opened above the range is clamped to 300" },
        { 10,   SUB_GFX_SCALE_MIN, "a track opened below the range is clamped to 50" },
    };
    for (unsigned i = 0; i < sizeof OPEN / sizeof *OPEN; i++) {
        SUB_FORMAT_BACKEND *bo = sub_format_gfx_create();
        SUB_FORMAT_OPEN_PARAMS po; memset(&po, 0, sizeof po);
        po.video_w = 1080; po.video_h = 2400; po.real_video_w = 1920; po.real_video_h = 1080;
        po.gfx_scale = OPEN[i].given;
        bo->open(bo, &po);
        feed(bo);
        SUB_FRAME *fo = bo->render_at(bo, 100);
        CHECK(fo && fo->gfx_scale == OPEN[i].want, OPEN[i].what);
        sub_frame_unref(fo);
        bo->close(bo); free(bo);
    }

    printf("%s (%d checks, %d failed)\n", fails ? "FAILED" : "ALL PASSED", checks, fails);
    return fails;
}
