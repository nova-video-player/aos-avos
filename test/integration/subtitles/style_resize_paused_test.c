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
 * Change subtitle style and resize the surface while PAUSED.
 *
 * While paused the render thread only wakes on an explicit broadcast (a style/resize setter, a
 * pause toggle, a feed). Each such change must still produce one fresh render at the frozen clock
 * -- otherwise a font-size change or a rotation is invisible until the user resumes -- without
 * moving the clock, un-pausing, or inventing content the backend did not produce.
 *
 *   1. a style change renders once, at the paused pts, and publishes nothing the backend did not emit;
 *   2. a surface resize + video box reach the backend (canvas once, box with it) and are applied
 *      as one pair, at the frozen pts;
 *   3. 100 rapid resize/box pairs coalesce: the backend ends on exactly the last pair;
 *   4. style setters and resize storms on two threads at once, still paused: no deadlock (watchdog),
 *      no data race (TSan), final geometry == last request, pause state untouched.
 */
#include "sub_engine.h"
#include "fake_backend.h"
#include "host_test_util.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

static SUB_ENGINE *g_e;
static atomic_int g_stop;

static void settle(void) { usleep(120000); }
static int fill(uint64_t last, uint64_t *gen, int force) {
    uint8_t px[64];
    return (int)sub_engine_fill_bitmap(g_e, px, 4, 4, 16, last, force, gen);
}

/* thread S: user dragging style sliders */
static void *style_thread(void *arg) {
    (void)arg;
    SUB_USER_STYLE *st = sub_engine_get_style(g_e);
    for (int i = 0; i < 3000; i++) {
        switch (i % 5) {
        case 0: sub_style_set_font_size(st, 20.0f + (float)(i % 40)); break;
        case 1: sub_style_set_outline_width(st, (float)(i % 9)); break;
        case 2: sub_style_set_text_color(st, 0xFF000000u | (uint32_t)(i * 2654435761u)); break;
        case 3: sub_style_set_bg_opacity(st, (uint8_t)(i % 256)); break;
        default: sub_style_set_margin_bottom(st, i % 200); break;
        }
        sub_engine_force_wake(g_e);
        if (i % 64 == 0) usleep(100);
    }
    return NULL;
}

/* thread R: window being resized / rotated; the box always matches the canvas width */
static int g_last_w, g_last_h;
static void *resize_thread(void *arg) {
    (void)arg;
    for (int i = 0; i < 3000; i++) {
        int w = 800 + (i % 50) * 16, h = 480 + (i % 37) * 9;
        sub_engine_surface_resized(g_e, w, h);
        sub_engine_set_video_box(g_e, 0, h / 8, w, h * 3 / 4);
        g_last_w = w; g_last_h = h;
        if (i % 64 == 0) usleep(100);
    }
    return NULL;
}

int main(void) {
    t_watchdog(120, "style_resize_paused");
    fake_backend_set_mode(FAKE_EMIT_ON_DEMAND);
    fake_backend_reset_counters();
    g_e = sub_engine_create();
    sub_engine_start(g_e, fake_clock, NULL);                    /* fake_clock is frozen at 1000 ms */
    sub_engine_open_track(g_e, SUB_FMT_SSA, 1920, 1080, NULL, 0, NULL, 0, NULL);
    sub_engine_surface_resized(g_e, 1280, 720);
    sub_engine_set_video_box(g_e, 0, 90, 1280, 540);
    fake_backend_emit(); sub_engine_force_wake(g_e); settle();

    uint64_t gen = 0;
    CHECK(fill((uint64_t)-1, &gen, 1) == SUB_FILL_FRAME, "setup: a cue is on screen");
    sub_engine_set_paused(g_e, 1);
    settle();

    /* ---- 1. style change while paused ---- */
    int r0 = fake_backend_renders();
    sub_style_set_font_size(sub_engine_get_style(g_e), 48.0f);
    sub_engine_force_wake(g_e);
    settle();
    CHECK(fake_backend_renders() > r0, "style change while paused triggers a fresh render (%d -> %d)", r0, fake_backend_renders());
    CHECK(fake_backend_last_pts() == 1000, "...at the frozen clock (pts %lld)", (long long)fake_backend_last_pts());
    CHECK(sub_engine_is_paused(g_e) == 1, "...and the engine is still paused");
    CHECK(fill(gen, &gen, 0) == SUB_FILL_UNCHANGED, "backend answered 'unchanged': nothing new was published");

    /* ---- 2. surface resize + video box while paused ---- */
    int rs0 = fake_backend_resizes(), bx0 = fake_backend_box_sets();
    sub_engine_surface_resized(g_e, 1920, 1080);
    sub_engine_set_video_box(g_e, 0, 0, 1920, 1080);
    settle();
    int cw, ch, bx, by, bw, bh;
    fake_backend_last_canvas(&cw, &ch);
    fake_backend_last_box(&bx, &by, &bw, &bh);
    CHECK(cw == 1920 && ch == 1080, "paused resize reached the backend (canvas %dx%d)", cw, ch);
    CHECK(bx == 0 && by == 0 && bw == 1920 && bh == 1080, "paused video box reached the backend (%d,%d %dx%d)", bx, by, bw, bh);
    CHECK(fake_backend_resizes() == rs0 + 1, "the canvas was applied once (%d -> %d)", rs0, fake_backend_resizes());
    CHECK(fake_backend_box_sets() >= bx0 + 1, "the box was applied (%d -> %d)", bx0, fake_backend_box_sets());
    CHECK(fake_backend_last_pts() == 1000 && sub_engine_is_paused(g_e) == 1, "...at the frozen clock, still paused");

    /* ---- 3. rapid pairs coalesce onto the last one ---- */
    int rs1 = fake_backend_resizes();
    for (int i = 0; i < 100; i++) {
        int w = 1000 + i * 7, h = 600 + i * 3;
        sub_engine_surface_resized(g_e, w, h);
        sub_engine_set_video_box(g_e, 0, h / 10, w, h * 4 / 5);
    }
    settle();
    int lw = 1000 + 99 * 7, lh = 600 + 99 * 3;
    fake_backend_last_canvas(&cw, &ch);
    fake_backend_last_box(&bx, &by, &bw, &bh);
    CHECK(cw == lw && ch == lh, "after 100 rapid resizes the backend is on the last canvas (%dx%d, want %dx%d)", cw, ch, lw, lh);
    CHECK(bx == 0 && by == lh / 10 && bw == lw && bh == lh * 4 / 5, "...and the last box (%d,%d %dx%d)", bx, by, bw, bh);
    CHECK(fake_backend_resizes() - rs1 <= 100, "never more backend resizes than requests (%d for 100)", fake_backend_resizes() - rs1);

    /* ---- 4. style + resize storms at once, still paused ---- */
    pthread_t st, rt;
    pthread_create(&st, NULL, style_thread, NULL);
    pthread_create(&rt, NULL, resize_thread, NULL);
    pthread_join(st, NULL);
    pthread_join(rt, NULL);
    sub_engine_force_wake(g_e);
    settle();
    fake_backend_last_canvas(&cw, &ch);
    fake_backend_last_box(&bx, &by, &bw, &bh);
    CHECK(cw == g_last_w && ch == g_last_h, "storm: backend ended on the last requested canvas (%dx%d, want %dx%d)", cw, ch, g_last_w, g_last_h);
    CHECK(bw == g_last_w && bh == g_last_h * 3 / 4 && by == g_last_h / 8,
          "storm: ...and the matching box (%d,%d %dx%d)", bx, by, bw, bh);
    CHECK(sub_engine_is_paused(g_e) == 1 && fake_backend_last_pts() == 1000, "storm: still paused, clock never moved");

    /* ---- resume: the engine still works and picks up new content ---- */
    sub_engine_set_paused(g_e, 0);
    fake_backend_emit(); sub_engine_force_wake(g_e); settle();
    CHECK(fill(gen, &gen, 0) == SUB_FILL_FRAME, "after resume a new cue is published normally");

    atomic_store(&g_stop, 1);
    sub_engine_destroy(g_e);
    CHECK(fake_backend_live() == 0, "no backend leaked (%d live)", fake_backend_live());
    return t_finish();
}
