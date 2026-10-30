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
 * A pause / seek / rescan that lands in the middle of an external SRT/VTT feed must not leave the track
 * empty or truncated  (aos-AVP issue #2004, "pause/seek mid-feed").
 *
 * Same real stack as ext_reselect_test.c (see stream_host.h). What interrupts the feed here is what
 * the app really does around a pause-for-reconfigure, speed change, seek or subtitle rescan: it idles
 * the subtitle thread (thread_state_set(&s->sub_tstate, THREAD_IDLE)) and resumes it. The parse worker
 * notices at its next checkpoint (every 200 cues, so only files longer than that are exposed),
 * flushes the engine and asks for a refeed.
 *
 * The bug: the worker raised the shared subtitle_ext_needs_refeed flag while its job was still
 * IN_PROGRESS. The subtitle thread could consume that flag before the job finished; its refeed only
 * resets a DONE job, so it did nothing, and the job then finished DONE with a flushed (empty) engine
 * and nothing left to ask for another feed. The same flag also decided whether the cue list was
 * cached, so a concurrent clear stored a truncated list that then cut the end of the file off every
 * later refeed.
 *
 * Each round uses a fresh stream (so the first feed is the uncached slow path, the only one exposed)
 * with a long file fed slowly, interrupts it late -- the cue list built so far is big, which is how
 * long the job spends finishing after it raised the flag -- and requires:
 *   1. the track ends up complete (all cues, once), whatever the pause lasted;
 *   2. the cached list is complete: selecting another track and back still yields every cue.
 * Then a storm of pauses during one feed, and (what the old modelled seek_pause_feed test checked, now through
 * the real stack) a storm of engine pause / resume / seek / subtitle-thread pauses racing one feed: no
 * deadlock (watchdog), the engine's pause state always exactly what was last requested, exactly one full
 * copy of the list afterwards, and it renders; also the documented refeed protocol (engine flushed + the
 * refeed flag raised) on a loaded track.
 */
#include "stream_host.h"
#include "fake_backend.h"
#include "host_test_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CUES        12000
#define FEED_US     5           /* per cue (spun): the whole feed takes ~60-100 ms */
#define LATE        11000       /* interrupt once this many cues are in: the list the job still has to free is big */
#define WAIT_MS     8000
#define ROUNDS      40

static int g_wait_ms = WAIT_MS;
static int g_empty_or_short;

static void result(int ok, const char *what, int want)
{
    int held = fake_backend_active_since_flush();
    CHECK(ok, "%s: engine holds %d of %d cues", what, held, want);
    if (!ok) {
        g_empty_or_short++;
        g_wait_ms = 800;           /* fail fast: the remaining rounds only need to list their symptoms */
        printf("      ^ SILENT FAILURE: nothing is left to feed this track again (engine %d of %d cues)\n", held, want);
    }
}

int main(int argc, char **argv)
{
    int vtt = argc > 1 && !strcmp(argv[1], "vtt");
    t_watchdog(240, vtt ? "ext_interrupt_vtt" : "ext_interrupt_srt");
    fake_backend_set_mode(FAKE_EMIT_ON_DEMAND);
    fake_backend_reset_counters();
    srand(12345);
    static const int holds_us[] = { 0, 20, 50, 100, 200, 400 };      /* extra idle time after the feed has reacted */
    int interrupted_rounds = 0;

    for (int round = 0; round < ROUNDS; round++) {
        int hold = holds_us[round % (int)(sizeof holds_us / sizeof holds_us[0])];
        HOST_STREAM *h = hs_create();
        (vtt ? hs_write_vtt : hs_write_srt)(h, "01", 'L', CUES);
        (vtt ? hs_write_vtt : hs_write_srt)(h, "02", 'S', 150);
        fake_backend_set_feed_delay_us(FEED_US);
        hs_start(h);                              /* the default track (which one depends on directory order) */
        int L = hs_track(h, "01"), S = hs_track(h, "02");
        if (h->s->av.subs != L) { hs_select(h, L); }      /* make sure the long one is the one loading */

        for (int t = 0; t < 20000 && fake_backend_active_since_flush() < LATE; t++) usleep(100);
        int at = fake_backend_active_since_flush();
        int noticed = hs_interrupt_feed(h, hold, 1000);   /* pause/seek/rescan lands late in the feed */
        interrupted_rounds += noticed;
        fake_backend_set_feed_delay_us(0);

        char what[96];
        snprintf(what, sizeof what, "round %d (interrupted at ~%d cues, paused %d us)", round, at, hold);
        result(hs_wait_cues(CUES, g_wait_ms) && hs_stays_at(CUES, 40), what, CUES);

        /* the cue list kept from that pass must be the whole file: re-selecting replays it */
        hs_select(h, S);
        hs_wait_cues(150, g_wait_ms);
        hs_select(h, L);
        snprintf(what, sizeof what, "round %d: reselect after the interrupted pass (cached list)", round);
        result(hs_wait_cues(CUES, g_wait_ms) && hs_stays_at(CUES, 40), what, CUES);

        hs_destroy(h);
    }
    CHECK(interrupted_rounds >= ROUNDS * 3 / 4,
          "the interruption really reached the feed (%d of %d rounds) -- otherwise this test proves nothing", interrupted_rounds, ROUNDS);

    /* ---- a storm of pauses during one feed ---- */
    {
        HOST_STREAM *h = hs_create();
        (vtt ? hs_write_vtt : hs_write_srt)(h, "01", 'L', CUES);
        hs_start(h);
        fake_backend_set_feed_delay_us(FEED_US);
        hs_select(h, hs_track(h, "01"));
        for (int i = 0; i < 40; i++) {
            hs_interrupt_feed(h, rand() % 600, 20);
            usleep((unsigned)(rand() % 3000));
        }
        fake_backend_set_feed_delay_us(0);
        result(hs_wait_cues(CUES, g_wait_ms) && hs_stays_at(CUES, 60), "after 40 pauses during one feed", CUES);
        hs_destroy(h);
    }

    /* ---- engine pause / resume / seek / subtitle-thread pause, all racing one feed ---- */
    {
        HOST_STREAM *h = hs_create();
        (vtt ? hs_write_vtt : hs_write_srt)(h, "01", 'L', CUES);
        hs_start(h);
        int L = hs_track(h, "01");
        sub_engine_set_paused(h->eng, 1);                         /* the first feed runs while the engine is PAUSED */
        fake_backend_set_feed_delay_us(FEED_US);
        hs_select(h, L);
        int paused = 1, mismatch = 0, seeks = 0, rescans = 0;
        for (int i = 0; i < 600; i++) {
            switch (rand() % 5) {
            case 0: sub_engine_set_paused(h->eng, 1); paused = 1; break;
            case 1: sub_engine_set_paused(h->eng, 0); paused = 0; break;
            case 2: sub_engine_force_wake(h->eng); seeks++; break;  /* a seek on an external text track: _seek_init() only wakes the render thread */
            case 3: rescans += hs_interrupt_feed(h, rand() % 300, 3); break;
            default: usleep((unsigned)(rand() % 800)); break;
            }
            if (sub_engine_is_paused(h->eng) != paused) mismatch++;
        }
        fake_backend_set_feed_delay_us(0);
        CHECK(mismatch == 0, "pause state always matched the last request (%d mismatches over %d seeks, %d feed interruptions)", mismatch, seeks, rescans);
        sub_engine_set_paused(h->eng, 0);
        result(hs_wait_cues(CUES, g_wait_ms) && hs_stays_at(CUES, 60), "after the pause/seek storm: exactly one full copy", CUES);
        CHECK(sub_engine_is_paused(h->eng) == 0, "resumed");
        CHECK(hs_visible(h), "after resume the engine renders again");

        /* the refeed protocol of _get_next_ext_sub(): engine flushed + subtitle_ext_needs_refeed raised, on a
         * LOADED track (no feed running), must bring back one full copy. (Not while a feed is running: nothing
         * flushes an external text track mid-feed today -- _seek_init() skips the flush for them.) */
        for (int i = 0; i < 5; i++) {
            sub_engine_flush(h->eng);
            __atomic_store_n(&h->s->subtitle_ext_needs_refeed, 1, __ATOMIC_RELEASE);
            char what[64];
            snprintf(what, sizeof what, "flush + refeed flag on a loaded track, round %d", i);
            result(hs_wait_cues(CUES, g_wait_ms) && hs_stays_at(CUES, 60), what, CUES);
        }
        hs_destroy(h);
    }

    CHECK(fake_backend_foreign() == 0, "no foreign cue (foreign=%d)", fake_backend_foreign());
    CHECK(fake_backend_live() == 0, "no engine backend leaked (%d live)", fake_backend_live());
    CHECK(g_empty_or_short == 0, "no silent failures: every interrupted feed ended complete (%d did not)", g_empty_or_short);
    return t_finish();
}
