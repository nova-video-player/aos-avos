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
 * Close one video and immediately open another while subtitle parsing is still active, with the
 * Java side rebuilding the SubtitleEngine underneath.
 *
 * Each "video" mirrors avos_mp_video_open()/close(): acquire the published engine from the
 * registry, start the clock, open a track and start a parse worker feeding a large cue list with
 * the track's generation token; close = stop the clock, stop and join the worker, close the track,
 * release the engine. Two close orders are exercised:
 *
 *   orderly  : stop worker, join, close_track, release      (what stream_delete() guarantees)
 *   hostile  : close_track while the worker is still mid-list, join afterwards; proves that even a
 *              worker that has not noticed the cancellation yet cannot leak cues into whatever
 *              is opened next (the token check) and cannot touch freed memory.
 *
 * A "Java" thread meanwhile rebuilds the engine (create+publish new, retract old, in either
 * order). A video that still holds the old engine keeps it alive; the next video gets the new one.
 * Checks: every engine created is destroyed exactly once, never while a video holds it; no cue from
 * a closed video reaches the next video's backend; no backend leaks. Run it under AddressSanitizer
 * (use-after-free on a destroyed engine) and ThreadSanitizer (see test_host_native.py).
 */
#include "sub_engine.h"
#include "sub_engine_registry.h"
#include "fake_backend.h"
#include "host_test_util.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#define BIG_CUES 30000

static atomic_int g_created, g_destroyed, g_stop_java, g_destroyed_while_held;

/* How many videos currently hold each engine, so destroy can assert it never runs under a holder.
 * (ASan would catch the resulting use-after-free too, but only when it is built in.) */
typedef struct { SUB_ENGINE *e; atomic_int holders; } SLOT;
static SLOT g_slots[1024];
static int g_nslots;
static pthread_mutex_t g_slots_m = PTHREAD_MUTEX_INITIALIZER;
static atomic_int *holders_of(SUB_ENGINE *e) {
    pthread_mutex_lock(&g_slots_m);
    atomic_int *h = NULL;
    for (int i = 0; i < g_nslots && !h; i++) if (g_slots[i].e == e) h = &g_slots[i].holders;
    if (!h && g_nslots < 1024) { g_slots[g_nslots].e = e; h = &g_slots[g_nslots++].holders; }
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
        } else {                                  /* old first: a stream opening now may see no engine at all */
            sub_engine_registry_retract(cur, destroy_cb);
            usleep((unsigned)(rand() % 300));
            sub_engine_registry_publish(next);
        }
        cur = next;
    }
    return cur;                                   /* the engine that is still published */
}

/* ---- one "video" ---- */
typedef struct { SUB_ENGINE *e; uint64_t tok; char label; int accepted, stopped_early; atomic_int stop; pthread_t th; } VIDEO;

static void *parse_worker(void *arg) {
    VIDEO *v = arg;
    char buf[24];
    for (int i = 0; i < BIG_CUES && !atomic_load(&v->stop); i++) {
        int n = snprintf(buf, sizeof buf, "%c%d", v->label, i);
        if (!sub_engine_feed_gen(v->e, v->tok, (const uint8_t *)buf, n, (int64_t)i * 40, 2000)) { v->stopped_early = 1; break; }
        v->accepted++;
    }
    return NULL;
}

static VIDEO *video_open(char label) {
    SUB_ENGINE *e = sub_engine_registry_acquire();
    if (!e) return NULL;                          /* Java is between engines: this video has no subtitles */
    VIDEO *v = calloc(1, sizeof *v);
    v->e = e; v->label = label;
    atomic_fetch_add(holders_of(e), 1);
    sub_engine_start(e, fake_clock, NULL);
    sub_engine_open_track(e, SUB_FMT_SRT, 1920, 1080, NULL, 0, NULL, 0, &v->tok);
    pthread_create(&v->th, NULL, parse_worker, v);
    return v;
}

/* Returns how many cues this video's worker had accepted. */
static int video_close(VIDEO *v, int hostile) {
    sub_engine_stop(v->e);
    if (hostile) {
        sub_engine_close_track(v->e);             /* worker still mid-list */
        pthread_join(v->th, NULL);
    } else {
        atomic_store(&v->stop, 1);
        pthread_join(v->th, NULL);
        sub_engine_close_track(v->e);
    }
    int accepted = v->accepted;
    atomic_fetch_sub(holders_of(v->e), 1);
    sub_engine_registry_release(v->e);            /* after the last use of v->e */
    free(v);
    return accepted;
}

int main(int argc, char **argv) {
    t_watchdog(150, "close_open_parsing");
    int iters = argc > 1 ? atoi(argv[1]) : 150;
    fake_backend_set_mode(FAKE_EMIT_ON_DEMAND);
    fake_backend_reset_counters();

    SUB_ENGINE *first = new_engine();
    sub_engine_registry_publish(first);
    pthread_t jt;
    pthread_create(&jt, NULL, java_thread, first);

    long accepted_total = 0; int no_engine = 0, videos = 0;
    for (int i = 0; i < iters; i++) {
        VIDEO *v = video_open((i & 1) ? 'B' : 'A');
        if (!v) { no_engine++; usleep(200); continue; }
        videos++;
        usleep((unsigned)(rand() % 1500));        /* parsing is active when we close */
        /* close, then loop straight into the next open: no pause between the two */
        accepted_total += video_close(v, (i % 3) == 2);
    }

    atomic_store(&g_stop_java, 1);
    void *cur_ret = NULL;
    pthread_join(jt, &cur_ret);
    SUB_ENGINE *cur = cur_ret;

    CHECK(videos > iters / 2, "most videos found an engine (%d of %d; %d hit a rebuild gap)", videos, iters, no_engine);
    CHECK(fake_backend_foreign() == 0, "no cue from a closed video reached another video's backend (foreign=%d)", fake_backend_foreign());
    CHECK(fake_backend_feeds() == accepted_total,
          "every cue a worker saw accepted reached exactly one backend, none were duplicated or lost (%ld vs %d)", accepted_total, fake_backend_feeds());
    CHECK(fake_backend_live() == 0, "every track backend was closed (%d live)", fake_backend_live());

    int r = sub_engine_registry_retract(cur, destroy_cb);
    CHECK(r == 1, "the last engine had no holders left: destroyed immediately");
    CHECK(atomic_load(&g_destroyed_while_held) == 0, "no engine was destroyed while a video still held it");
    CHECK(atomic_load(&g_destroyed) == atomic_load(&g_created),
          "every engine created was destroyed exactly once (%d created, %d destroyed)", atomic_load(&g_created), atomic_load(&g_destroyed));
    return t_finish();
}
