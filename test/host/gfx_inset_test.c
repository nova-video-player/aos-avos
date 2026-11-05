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

// The real GFX backend and the reserved bottom space (controls / system bars): the value is
// stamped on the frames it hands out, a change re-stamps the cue (so it moves when the controls
// appear and returns when they hide), an unchanged value does nothing, and a cue that is
// opened or fed later still gets the current value.
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

    printf("%s (%d checks, %d failed)\n", fails ? "FAILED" : "ALL PASSED", checks, fails);
    return fails;
}
