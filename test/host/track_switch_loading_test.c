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
 * Rapidly switching between two large external SRT/VTT tracks while they are still loading.
 *
 * stream_sub_ext.c feeds a big cue list from a background worker, one sub_engine_feed_gen() per
 * cue, each stamped with the engine's track-generation token captured when the track was opened.
 * Switching tracks calls open_track() again, which bumps the generation, so every cue still in
 * flight from the previous track must be dropped -- never routed into the new track's backend and
 * never left on screen. The fake backend labels each backend with the first byte of its first cue
 * ('A...' / 'B...'), so a cue from the other track reaching it is counted as "foreign".
 *
 *   1. deterministic: a stale token is dropped (feed and flush), a current one is accepted;
 *   2. 300 rapid A/B switches (plus an occasional close_track) racing large feeders;
 *      invariant: every cue a feeder saw accepted reached exactly one backend (sum(accepted) ==
 *      backend feeds), and none reached a backend of the other track;
 *   3. the track opened last still loads completely, and nothing leaks.
 * Meant to run under ThreadSanitizer as well (see test_host_native.py).
 */
#include "sub_engine.h"
#include "fake_backend.h"
#include "host_test_util.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define BIG_CUES 40000     /* "large" track: more cues than one switch interval can feed */

typedef struct { SUB_ENGINE *e; uint64_t tok; char label; int cues; int accepted; int stopped_early; } FEED;

static void *feeder(void *arg) {
    FEED *f = arg;
    char buf[32];
    for (int i = 0; i < f->cues; i++) {
        int n = snprintf(buf, sizeof buf, "%c%d", f->label, i);
        if (!sub_engine_feed_gen(f->e, f->tok, (const uint8_t *)buf, n, (int64_t)i * 40, 2000)) {
            f->stopped_early = 1;          /* stale token: the track was switched or closed under us */
            break;
        }
        f->accepted++;
    }
    return NULL;
}

static uint64_t open_label(SUB_ENGINE *e, char label) {
    uint64_t tok = 0;
    /* A = SRT, anything else = SSA-wrapped (VTT/ASS): different backends, same engine, like a real switch. */
    int rc = sub_engine_open_track(e, label == 'A' ? SUB_FMT_SRT : SUB_FMT_SSA, 1920, 1080, NULL, 0, NULL, 0, &tok);
    if (rc != 0) CHECK(0, "open_track(%c) failed (rc=%d)", label, rc);   /* silent on success: called hundreds of times */
    return tok;
}

static int something_visible(SUB_ENGINE *e) {
    uint64_t gen; uint8_t px[64];
    return sub_engine_fill_bitmap(e, px, 4, 4, 16, (uint64_t)-1, /*force=*/1, &gen) == SUB_FILL_FRAME;
}

int main(int argc, char **argv) {
    t_watchdog(150, "track_switch_loading");
    int iters = argc > 1 ? atoi(argv[1]) : 300;
    fake_backend_set_mode(FAKE_EMIT_ON_DEMAND);
    fake_backend_reset_counters();
    SUB_ENGINE *e = sub_engine_create();
    sub_engine_start(e, fake_clock, NULL);

    /* ---- 1. deterministic stale-token behaviour ---- */
    uint64_t tokA = open_label(e, 'A');
    CHECK(sub_engine_feed_gen(e, tokA, (const uint8_t *)"A0", 2, 0, 1000) == 1, "current token: cue accepted");
    uint64_t tokB = open_label(e, 'B');
    CHECK(tokB != tokA && sub_engine_get_track_generation(e) == tokB, "switching tracks moves the generation token");
    int feeds0 = fake_backend_feeds(), flushes0 = fake_backend_flushes();
    CHECK(sub_engine_feed_gen(e, tokA, (const uint8_t *)"A1", 2, 40, 1000) == 0 && fake_backend_feeds() == feeds0,
          "stale token: cue dropped before it reaches the new backend");
    sub_engine_flush_gen(e, tokA);
    CHECK(fake_backend_flushes() == flushes0, "stale token: flush_gen() does not flush the new track");
    CHECK(sub_engine_feed_gen(e, tokB, (const uint8_t *)"B0", 2, 0, 1000) == 1 && fake_backend_foreign() == 0,
          "current token on the new track: accepted, new backend has seen no foreign cue");

    /* ---- 2. storm: large feeders racing rapid switches ---- */
    fake_backend_reset_counters();
    FEED *live = NULL; pthread_t live_th = 0;
    long accepted_total = 0; int stale_stops = 0, closes = 0;
    for (int i = 0; i < iters; i++) {
        if (i % 7 == 6) {
            /* subtitles turned off while a feed is running: nothing may remain visible afterwards */
            sub_engine_close_track(e);
            closes++;
            if (live) {
                pthread_join(live_th, NULL);
                accepted_total += live->accepted; stale_stops += live->stopped_early;
                free(live); live = NULL;
            }
            if (something_visible(e)) { printf("FAIL: iteration %d: content visible after close_track()\n", i); t_fails++; }
            continue;
        }
        FEED *nf = calloc(1, sizeof *nf);
        nf->e = e; nf->label = (i & 1) ? 'B' : 'A'; nf->cues = BIG_CUES;
        nf->tok = open_label(e, nf->label);      /* bumps the generation: `live` (if any) is stale from here */
        pthread_t nth;
        pthread_create(&nth, NULL, feeder, nf);
        if (live) {                              /* the stale feeder is still mid-list: it must stop on its own */
            pthread_join(live_th, NULL);
            accepted_total += live->accepted; stale_stops += live->stopped_early;
            free(live);
        }
        live = nf; live_th = nth;
        usleep((unsigned)(rand() % 1500));       /* let the new feeder get partway through its list */
    }

    /* ---- 3. the last track still loads completely ---- */
    uint64_t tokZ = open_label(e, 'Z');
    FEED z = { e, tokZ, 'Z', BIG_CUES, 0, 0 };
    pthread_t zth;
    pthread_create(&zth, NULL, feeder, &z);
    if (live) {                                  /* stale after the open above */
        pthread_join(live_th, NULL);
        accepted_total += live->accepted; stale_stops += live->stopped_early;
        free(live);
    }
    pthread_join(zth, NULL);
    accepted_total += z.accepted;

    CHECK(fake_backend_foreign() == 0, "no cue from another track ever reached a backend (foreign=%d)", fake_backend_foreign());
    CHECK(fake_backend_feeds() == accepted_total,
          "every accepted cue reached exactly one backend (feeders saw %ld accepted, backends saw %d)", accepted_total, fake_backend_feeds());
    CHECK(stale_stops > 0, "the race was exercised: %d feeders were cut off mid-list by a switch/close (%d closes)", stale_stops, closes);
    CHECK(z.accepted == BIG_CUES && !z.stopped_early && fake_backend_active_since_flush() == BIG_CUES,
          "final track loaded completely (%d of %d cues, backend holds %d)", z.accepted, BIG_CUES, fake_backend_active_since_flush());
    CHECK(fake_backend_live() == 1, "exactly one backend alive before destroy (%d)", fake_backend_live());

    fake_backend_emit(); sub_engine_force_wake(e); usleep(50000);
    CHECK(something_visible(e), "the finished track can render");
    sub_engine_destroy(e);
    CHECK(fake_backend_live() == 0, "no backend leaked after destroy (%d live)", fake_backend_live());
    return t_finish();
}
