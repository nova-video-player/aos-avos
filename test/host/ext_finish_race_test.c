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
 * A track reopened exactly as its feed job finishes must still end up fed  (aos-AVP issue #2004).
 *
 * Same real stack as ext_reselect_test.c (see stream_host.h). The window is a few instructions wide
 * -- on the worker, between "has the track generation moved?" and "write parse_state" -- so timing
 * alone would hit it once in millions of runs. This test opens it on purpose, without touching Source/:
 *
 *   - the fake backend reports every cue to a hook, which tells us when the LAST cue of the pass has
 *     been fed (the worker is then in the job's tail);
 *   - the build renames pthread_mutex_unlock to host_unlock_hook in every object of this test
 *     (-Dpthread_mutex_unlock=host_unlock_hook: a compile-time rename, so it needs no linker support -- Apple's ld has no
 *     --wrap -- and reaches the same call sites). On the worker thread, after
 *     the last cue, the first unlock of a mutex other than the engine lock is the job's parse_mutex
 *     unlock that ends the generation check (before the fix) or the combined check+state write (after
 *     it); we sleep there for INJECT_MS. Before the fix that sleep sits between the check and the state
 *     write; after it, the state is already committed.
 *   - while the worker sleeps, the test reopens the track through the real stream_set_subtitle_stream().
 *
 * Before the fix the old job then wrote DONE for a track whose engine had just been reopened and was
 * empty, and nothing asked for another feed: the track stayed empty. After it, the reopen is ordered
 * strictly before or after the completion and the track is re-fed either way. Both orders must end with
 * every cue in the engine. The test also checks that the sleep really was injected in every round, so it
 * cannot pass vacuously if the code under test is reorganised.
 */
#include "stream_host.h"
#include "fake_backend.h"
#include "host_test_util.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef pthread_mutex_unlock
#error "build with -Dpthread_mutex_unlock=host_unlock_hook (see test_host_native.py)"
#endif
#define CUES        300
#define INJECT_MS   40
#define ROUNDS      12
#define WAIT_MS     8000

/* ---- injection ----------------------------------------------------------------------------- */
static __thread int t_after_last;             /* this (worker) thread has just fed the last cue of the pass */
static __thread void *t_engine_lock;          /* the engine lock it was holding when it did */
static __thread int t_slept;
static atomic_int g_injected;                 /* rounds in which the sleep was injected (worker adds, main reads) */
static atomic_int g_arm;                      /* armed for the current round (main writes, the worker's feed hook reads) */

static void feed_hook(const uint8_t *data, int size)
{
    if (!atomic_load(&g_arm) || size < 2) return;
    if (atoi((const char *)data + 1) == CUES - 1) { t_after_last = 1; t_engine_lock = NULL; t_slept = 0; }
}

#undef pthread_mutex_unlock                  /* from here on, the real function; the rename is in force everywhere else */
extern int pthread_mutex_unlock(pthread_mutex_t *m);
int host_unlock_hook(pthread_mutex_t *m)
{
    int rc = pthread_mutex_unlock(m);
    if (t_after_last && !t_slept) {
        if (!t_engine_lock) {
            t_engine_lock = m;                /* the first unlock after the hook is the engine lock (feed_gen returning) */
        } else if (m != t_engine_lock) {
            t_slept = 1;
            atomic_fetch_add(&g_injected, 1);
            usleep(INJECT_MS * 1000);
            t_after_last = 0;
        }
    }
    return rc;
}

int main(int argc, char **argv)
{
    int vtt = argc > 1 && !strcmp(argv[1], "vtt");
    t_watchdog(200, vtt ? "ext_finish_race_vtt" : "ext_finish_race_srt");
    fake_backend_set_mode(FAKE_EMIT_ON_DEMAND);
    fake_backend_reset_counters();
    fake_backend_set_feed_hook(feed_hook);

    int failures = 0, wait_ms = WAIT_MS;
    for (int round = 0; round < ROUNDS; round++) {
        HOST_STREAM *h = hs_create();
        (vtt ? hs_write_vtt : hs_write_srt)(h, "01", 'L', CUES);
        hs_start(h);                          /* the only track: selected and fed by itself */
        int L = hs_track(h, "01");

        atomic_store(&g_arm, 1);
        int fed = hs_wait_cues(CUES, WAIT_MS);                /* the last cue is in; the worker is in the job's tail */
        int rc = hs_select(h, L);                             /* ...and the app reselects the track right then */
        atomic_store(&g_arm, 0);

        int ok = fed && rc == 0 && hs_wait_cues(CUES, wait_ms) && hs_stays_at(CUES, 60);
        int held = fake_backend_active_since_flush();
        CHECK(ok, "round %d: reopened while the feed job was finishing: engine holds %d of %d cues (first feed %s, select rc=%d)",
              round, held, CUES, fed ? "completed" : "NEVER completed", rc);
        if (!ok) {
            failures++;
            wait_ms = 800;
            printf("      ^ SILENT FAILURE: the reopen's feed was deferred (job in progress), the old job then finished as DONE with "
                   "an empty engine, and nothing asked for another feed (engine %d of %d cues)\n", held, CUES);
        }
        hs_destroy(h);
    }
    fake_backend_set_feed_hook(NULL);

    CHECK(atomic_load(&g_injected) == ROUNDS, "the race window was opened in every round (%d of %d) -- otherwise this test proves nothing", atomic_load(&g_injected), ROUNDS);
    CHECK(fake_backend_foreign() == 0, "no foreign cue (foreign=%d)", fake_backend_foreign());
    CHECK(fake_backend_live() == 0, "no engine backend leaked (%d live)", fake_backend_live());
    CHECK(failures == 0, "no silent failures (%d of %d rounds ended with an empty/short track)", failures, ROUNDS);
    return t_finish();
}
