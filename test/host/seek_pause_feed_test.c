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
 * Seek and pause during the initial external-subtitle feed.
 *
 * The first bulk feed of an external track runs on a worker and can take a while. While it runs the
 * user may pause, resume and seek. A seek (stream_video.c _seek_init) flushes the engine and raises
 * subtitle_ext_needs_refeed AFTER the flush; the decode thread then re-runs the feed from the
 * start. This test models that protocol with a worker thread and drives it from another thread:
 *
 *   1. feed the whole list while PAUSED: feed() must not depend on the engine running;
 *   2. 600 random pause / resume / seek operations racing the feed;
 *   3. a final seek, then resume: the worker's last complete pass must leave the engine holding
 *      exactly one full copy of the list (no cues lost to a flush, none duplicated by a pass that
 *      kept feeding across the seek), and the engine's pause state must be what the user set.
 *
 * Any lost wake-up or lock-order problem between pause/seek and feed shows as the watchdog firing;
 * data races show under ThreadSanitizer.
 */
#include "sub_engine.h"
#include "fake_backend.h"
#include "host_test_util.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#define N_CUES 6000

static SUB_ENGINE *g_e;
static uint64_t g_tok;
static atomic_int g_refeed;        /* models s->subtitle_ext_needs_refeed */
static atomic_int g_stop, g_busy, g_passes;

/* The decode/parse worker: one pass = flush, then feed every cue; abandon the pass as soon as the
 * refeed flag is raised again (what _sub_feed_should_stop() does) and start over. */
static void *worker(void *arg) {
    (void)arg;
    char buf[24];
    while (!atomic_load(&g_stop)) {
        atomic_store(&g_busy, 1);
        if (!atomic_exchange(&g_refeed, 0)) { atomic_store(&g_busy, 0); usleep(100); continue; }
        sub_engine_flush_gen(g_e, g_tok);
        int interrupted = 0;
        for (int i = 0; i < N_CUES; i++) {
            if (atomic_load(&g_refeed)) { interrupted = 1; break; }
            int n = snprintf(buf, sizeof buf, "S%d", i);
            sub_engine_feed_gen(g_e, g_tok, (const uint8_t *)buf, n, (int64_t)i * 40, 2000);
        }
        if (!interrupted) atomic_fetch_add(&g_passes, 1);
        atomic_store(&g_busy, 0);
    }
    return NULL;
}

/* Same order as _seek_init(): flush first, THEN raise the flag. */
static void seek(void) { sub_engine_flush(g_e); atomic_store(&g_refeed, 1); }

/* "Nothing pending and nothing running". Read the flag BEFORE busy: the worker raises busy before it
 * consumes the flag, so if the flag is already clear the worker's busy=1 is guaranteed visible. */
static int idle(void) {
    int flag = atomic_load(&g_refeed);
    int busy = atomic_load(&g_busy);
    return !flag && !busy;
}
static int wait_for(int (*cond)(void), int ms) {
    for (int t = 0; t < ms; t++) { if (cond()) return 1; usleep(1000); }
    return cond();
}
static int g_passes_target;
static int passes_reached(void) { return atomic_load(&g_passes) >= g_passes_target; }
static int idle_and_done(void)  { return passes_reached() && idle(); }

static int something_visible(SUB_ENGINE *e) {
    uint64_t gen; uint8_t px[64];
    return sub_engine_fill_bitmap(e, px, 4, 4, 16, (uint64_t)-1, /*force=*/1, &gen) == SUB_FILL_FRAME;
}

int main(void) {
    t_watchdog(120, "seek_pause_feed");
    fake_backend_set_mode(FAKE_EMIT_ON_DEMAND);
    fake_backend_reset_counters();
    g_e = sub_engine_create();
    sub_engine_start(g_e, fake_clock, NULL);
    sub_engine_open_track(g_e, SUB_FMT_SRT, 1920, 1080, NULL, 0, NULL, 0, &g_tok);

    pthread_t wt;
    pthread_create(&wt, NULL, worker, NULL);

    /* ---- 1. initial feed while paused ---- */
    sub_engine_set_paused(g_e, 1);
    g_passes_target = 1;
    atomic_store(&g_refeed, 1);
    CHECK(wait_for(idle_and_done, 20000), "initial feed finished while the engine was paused");
    CHECK(fake_backend_active_since_flush() == N_CUES, "paused feed delivered the whole list (%d of %d)",
          fake_backend_active_since_flush(), N_CUES);
    CHECK(sub_engine_is_paused(g_e) == 1, "feeding did not change the pause state");

    /* ---- 2. random pause / resume / seek racing a re-feed ---- */
    int paused = 1, seeks = 0, pause_mismatch = 0;
    for (int i = 0; i < 600; i++) {
        switch (rand() % 4) {
        case 0: sub_engine_set_paused(g_e, 1); paused = 1; break;
        case 1: sub_engine_set_paused(g_e, 0); paused = 0; break;
        case 2: seek(); seeks++; break;
        default: usleep((unsigned)(rand() % 600)); break;
        }
        if (sub_engine_is_paused(g_e) != paused) pause_mismatch++;
        if (rand() % 8 == 0) usleep((unsigned)(rand() % 2000));
    }
    CHECK(pause_mismatch == 0, "pause state always matched the last request (%d mismatches over %d seeks)", pause_mismatch, seeks);

    /* ---- 3. final seek, then resume: exactly one complete copy of the list ---- */
    int base = atomic_load(&g_passes);
    seek();
    g_passes_target = base + 1;
    sub_engine_set_paused(g_e, 0);
    CHECK(wait_for(idle_and_done, 30000), "after the last seek the worker completed a pass and went idle");
    int held = fake_backend_active_since_flush();
    CHECK(held == N_CUES, "engine holds exactly one full copy of the list after the storm (%d of %d)", held, N_CUES);
    CHECK(sub_engine_is_paused(g_e) == 0, "resumed");

    fake_backend_emit(); sub_engine_force_wake(g_e); usleep(60000);
    CHECK(something_visible(g_e), "after resume the renderer produces content again");

    atomic_store(&g_stop, 1);
    pthread_join(wt, NULL);
    sub_engine_destroy(g_e);
    CHECK(fake_backend_live() == 0, "no backend leaked (%d live)", fake_backend_live());
    return t_finish();
}
