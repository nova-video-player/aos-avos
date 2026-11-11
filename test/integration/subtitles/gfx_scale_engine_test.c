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
 * The user's bitmap-subtitle scale through the REAL engine (sub_engine_set_gfx_scale()), with the
 * fake backend standing in for the decoders:
 *
 *   1. nobody called the setter: a track opened gets 100, not the calloc'd 0 that means "unset";
 *   2. a value reaches the backend through set_gfx_scale() on the next poll, and the same value
 *      again does not reach it twice;
 *   3. out-of-range values are clamped to 50..300 by the engine (0 and negatives included: a
 *      setter call is never "unset");
 *   4. it is applied while PAUSED (the setter wakes the render thread, like the inset and the
 *      position do);
 *   5. a track opened later starts with the stored value, and it is handed over once more by the
 *      first poll (a fresh backend needs it);
 *   6. the setters can be hammered from two threads while the render thread polls: no deadlock
 *      (watchdog), no data race (TSan), and the backend ends on the last value.
 */
#include "sub_engine.h"
#include "fake_backend.h"
#include "host_test_util.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static SUB_ENGINE *g_e;

static void settle(void) { usleep(120000); }

static void *hammer(void *arg) {
    const int base = (int)(long)arg;
    for (int i = 0; i < 2000; i++) {
        sub_engine_set_gfx_scale(g_e, base + (i % 100));
        if (i % 64 == 0) usleep(100);
    }
    return NULL;
}

int main(void) {
    t_watchdog(60, "gfx_scale_engine");
    fake_backend_set_mode(FAKE_EMIT_ON_DEMAND);
    fake_backend_reset_counters();
    g_e = sub_engine_create();
    sub_engine_start(g_e, fake_clock, NULL);

    /* ---- 1. default ---- */
    sub_engine_open_track(g_e, SUB_FMT_SSA, 1920, 1080, NULL, 0, NULL, 0, NULL);
    sub_engine_surface_resized(g_e, 1280, 720);
    settle();
    CHECK(fake_backend_open_gfx_scale() == 100, "a track opened before any setter call is given 100 (got %d)", fake_backend_open_gfx_scale());
    CHECK(fake_backend_last_gfx_scale() == 100, "a fresh backend is handed the value by the first poll (got %d)", fake_backend_last_gfx_scale());

    /* ---- 2. applied once ---- */
    int n0 = fake_backend_gfx_scale_sets();
    sub_engine_set_gfx_scale(g_e, 150);
    settle();
    CHECK(fake_backend_last_gfx_scale() == 150, "150 reached the backend (got %d)", fake_backend_last_gfx_scale());
    CHECK(fake_backend_gfx_scale_sets() == n0 + 1, "... through one call (%d -> %d)", n0, fake_backend_gfx_scale_sets());
    sub_engine_set_gfx_scale(g_e, 150);
    sub_engine_force_wake(g_e);
    settle();
    CHECK(fake_backend_gfx_scale_sets() == n0 + 1, "the same value again does not reach the backend again (%d)", fake_backend_gfx_scale_sets());
    sub_engine_set_gfx_scale(g_e, 100);
    settle();
    CHECK(fake_backend_last_gfx_scale() == 100 && fake_backend_gfx_scale_sets() == n0 + 2, "back to 100 is a change like any other (got %d)", fake_backend_last_gfx_scale());

    /* ---- 3. clamping ---- */
    static const struct { int given, want; } CLAMP[] = {
        { 7000, SUB_GFX_SCALE_MAX }, { 301, SUB_GFX_SCALE_MAX }, { 300, 300 },
        { 50, 50 }, { 49, 50 }, { 10, 50 }, { 80, 80 }, { 0, 50 }, { -7000, 50 },
    };
    for (unsigned i = 0; i < sizeof CLAMP / sizeof *CLAMP; i++) {
        sub_engine_set_gfx_scale(g_e, 200);               /* move off any clamp result so every call below is a change */
        settle();
        sub_engine_set_gfx_scale(g_e, CLAMP[i].given);
        settle();
        CHECK(fake_backend_last_gfx_scale() == CLAMP[i].want, "set %d -> the backend gets %d (got %d)", CLAMP[i].given, CLAMP[i].want, fake_backend_last_gfx_scale());
    }

    /* ---- 4. while paused ---- */
    sub_engine_set_paused(g_e, 1);
    settle();
    int p0 = fake_backend_gfx_scale_sets();
    sub_engine_set_gfx_scale(g_e, 125);
    settle();
    CHECK(fake_backend_last_gfx_scale() == 125 && fake_backend_gfx_scale_sets() == p0 + 1,
          "applied while paused (got %d, %d -> %d calls)", fake_backend_last_gfx_scale(), p0, fake_backend_gfx_scale_sets());
    CHECK(sub_engine_is_paused(g_e) == 1, "... and the engine is still paused");
    sub_engine_set_paused(g_e, 0);

    /* ---- 5. a track opened later ---- */
    sub_engine_close_track(g_e);
    fake_backend_reset_counters();
    sub_engine_open_track(g_e, SUB_FMT_SSA, 1920, 1080, NULL, 0, NULL, 0, NULL);
    settle();
    CHECK(fake_backend_open_gfx_scale() == 125, "a track opened later starts with the stored 125 (got %d)", fake_backend_open_gfx_scale());
    CHECK(fake_backend_last_gfx_scale() == 125 && fake_backend_gfx_scale_sets() >= 1, "... and the first poll hands it over as well (got %d)", fake_backend_last_gfx_scale());

    /* ---- 6. two threads ---- */
    pthread_t a, b;
    pthread_create(&a, NULL, hammer, (void *)60L);
    pthread_create(&b, NULL, hammer, (void *)200L);
    pthread_join(a, NULL); pthread_join(b, NULL);
    sub_engine_set_gfx_scale(g_e, 222);
    settle();
    CHECK(fake_backend_last_gfx_scale() == 222, "after the storm the backend ends on the last value (got %d)", fake_backend_last_gfx_scale());

    sub_engine_destroy(g_e);
    CHECK(fake_backend_live() == 0, "no backend leaked");
    return t_finish();
}
