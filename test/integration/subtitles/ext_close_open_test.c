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
 * Close one video and immediately open another while its external-subtitle parse worker is active, with
 * the Java side rebuilding the SubtitleEngine underneath -- through the REAL external-subtitle stack
 * (see stream_host.h). Replaces the old close_open_parsing test, which modelled the worker with a thread.
 *
 * Each "video" mirrors avos_mp_video_open()/avos_mp_video_close(): acquire the published engine from the
 * registry, start the clock, a real STREAM discovers a real .srt/.vtt file, the real subtitle thread opens
 * the engine track and the real parse-worker pool starts feeding it. Close is avos_mp_video_close()'s
 * order: stop the engine clock, stop the stream (subtitle thread, then stream_sub_ext_close() with the
 * pool possibly mid-feed), only then close the engine's track, then release the registry reference.
 * A "Java" thread meanwhile rebuilds the engine (create + publish new, retract old, in either order). A
 * video that still holds the old engine keeps it alive and the last release destroys it; the next video
 * gets the new one.
 *
 * Checks: every engine created is destroyed exactly once and never while a video holds it; no cue from a
 * closed video reaches another video's backend; no backend leaks; no deadlock or hang while the pool is
 * torn down mid-feed. Run it under AddressSanitizer (use-after-free of an engine or STREAM) and
 * ThreadSanitizer too. argv[1] = srt | vtt, argv[2] = number of videos.
 */
#include "stream_host.h"
#include "sub_engine_registry.h"
#include "fake_backend.h"
#include "host_test_util.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CUES 3000

static atomic_int g_created, g_destroyed, g_stop_java, g_destroyed_while_held;

/* How many videos currently hold each engine, so destroy can assert it never runs under a holder. */
typedef struct { SUB_ENGINE *e; atomic_int holders; } SLOT;
static SLOT g_slots[2048];
static int g_nslots;
static pthread_mutex_t g_slots_m = PTHREAD_MUTEX_INITIALIZER;
static atomic_int *holders_of(SUB_ENGINE *e) {
    pthread_mutex_lock(&g_slots_m);
    atomic_int *h = NULL;
    for (int i = 0; i < g_nslots && !h; i++) if (g_slots[i].e == e) h = &g_slots[i].holders;
    if (!h && g_nslots < 2048) { g_slots[g_nslots].e = e; h = &g_slots[g_nslots++].holders; }
    pthread_mutex_unlock(&g_slots_m);
    return h;
}
static void destroy_cb(SUB_ENGINE *e) {
    atomic_int *h = holders_of(e);
    if (h && atomic_load(h) != 0) atomic_fetch_add(&g_destroyed_while_held, 1);
    sub_engine_destroy(e);
    atomic_fetch_add(&g_destroyed, 1);
}
static SUB_ENGINE *new_engine(void) { SUB_ENGINE *e = sub_engine_create(); atomic_fetch_add(&g_created, 1); return e; }

/* ---- the Java side: rebuilds the engine at random times ---- */
static void *java_thread(void *arg) {
    SUB_ENGINE *cur = arg;
    while (!atomic_load(&g_stop_java)) {
        usleep((unsigned)(500 + rand() % 3000));
        SUB_ENGINE *next = new_engine();
        if (rand() & 1) {                         /* new first: both engines exist, newest is published */
            sub_engine_registry_publish(next);
            sub_engine_registry_retract(cur, destroy_cb);
        } else {                                  /* old first: a video opening now may see no engine at all */
            sub_engine_registry_retract(cur, destroy_cb);
            usleep((unsigned)(rand() % 300));
            sub_engine_registry_publish(next);
        }
        cur = next;
    }
    return cur;                                   /* the engine that is still published */
}

/* ---- one "video" ---- */
typedef struct { HOST_STREAM *h; SUB_ENGINE *e; } VIDEO;

static VIDEO *video_open(int vtt, char label) {
    SUB_ENGINE *e = sub_engine_registry_acquire();
    if (!e) return NULL;                          /* Java is between engines: this video has no subtitles */
    atomic_fetch_add(holders_of(e), 1);
    VIDEO *v = calloc(1, sizeof *v);
    v->e = e;
    v->h = hs_create_on(e);                       /* starts the engine clock, like avos_mp_video_open() */
    (vtt ? hs_write_vtt : hs_write_srt)(v->h, "01", label, CUES);
    hs_start(v->h);                               /* discovery, subtitle thread -> engine track -> parse worker feed */
    return v;
}

static void video_close(VIDEO *v) {
    SUB_ENGINE *e = v->e;
    hs_close_video(v->h);                         /* avos_mp_video_close(): clock, stream (+ pool), then close_track */
    atomic_fetch_sub(holders_of(e), 1);
    sub_engine_registry_release(e);               /* after the last use of the engine */
    free(v);
}

int main(int argc, char **argv) {
    int vtt = argc > 1 && !strcmp(argv[1], "vtt");
    int iters = argc > 2 ? atoi(argv[2]) : 120;
    t_watchdog(200, vtt ? "ext_close_open_vtt" : "ext_close_open_srt");
    fake_backend_set_mode(FAKE_EMIT_ON_DEMAND);
    fake_backend_reset_counters();
    fake_backend_set_feed_delay_us(8);            /* a feed takes ~25 ms: videos are closed while it runs */

    SUB_ENGINE *first = new_engine();
    sub_engine_registry_publish(first);
    pthread_t jt;
    pthread_create(&jt, NULL, java_thread, first);

    int no_engine = 0, videos = 0, mid_feed = 0;
    for (int i = 0; i < iters; i++) {
        VIDEO *v = video_open(vtt, (i & 1) ? 'B' : 'A');
        if (!v) { no_engine++; usleep(200); continue; }
        videos++;
        usleep((unsigned)(rand() % 20000));       /* parsing is (usually) still active when we close */
        int held = fake_backend_active_since_flush();
        mid_feed += held >= 0 && held < CUES;
        video_close(v);                           /* ...and straight into the next open: no pause between the two */
    }
    fake_backend_set_feed_delay_us(0);

    atomic_store(&g_stop_java, 1);
    void *cur_ret = NULL;
    pthread_join(jt, &cur_ret);
    SUB_ENGINE *cur = cur_ret;

    CHECK(videos > iters / 2, "most videos found an engine (%d of %d; %d hit a rebuild gap)", videos, iters, no_engine);
    CHECK(mid_feed > videos / 4, "videos really were closed while their feed was running (%d of %d)", mid_feed, videos);
    CHECK(fake_backend_foreign() == 0, "no cue from a closed video reached another video's backend (foreign=%d)", fake_backend_foreign());
    CHECK(fake_backend_live() == 0, "every track backend was closed (%d live)", fake_backend_live());

    int r = sub_engine_registry_retract(cur, destroy_cb);
    CHECK(r == 1, "the last engine had no holders left: destroyed immediately");
    CHECK(atomic_load(&g_destroyed_while_held) == 0, "no engine was destroyed while a video still held it");
    CHECK(atomic_load(&g_destroyed) == atomic_load(&g_created),
          "every engine created was destroyed exactly once (%d created, %d destroyed)", atomic_load(&g_created), atomic_load(&g_destroyed));
    return t_finish();
}
