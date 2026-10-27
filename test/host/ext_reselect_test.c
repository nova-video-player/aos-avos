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
 * Re-selecting an external SRT/VTT track must show its cues every time  (aos-AVP issue #2004).
 *
 * Runs the REAL external-subtitle stack (see stream_host.h): stream_set_subtitle_stream() -> the real
 * subtitle decode thread -> _get_next_ext_sub() -> sub_engine_open_track() +
 * stream_sub_ext_set_track_generation() + stream_sub_ext_feed_engine() -> the real parse-worker pool
 * -> the real feed_SRT()/feed_VTT() -> the real engine. Only the engine's backend is fake, and it
 * counts the cues its current track holds -- the "n_events" of the bug report.
 *
 * The bug: a streaming track's SUBT_PARSE_DONE was read as "the engine holds this track's cues",
 * but every selection closes and reopens the engine track (new generation, empty). A track whose
 * first feed had finished was therefore never fed again, and a feed that was still running when the
 * track was reopened finished as DONE although the engine had dropped all of its cues (stale token).
 *
 *   1. the exact sequence from the report: a long and a short file, select 02, 01, 02, 01 ... each
 *      selection must end with exactly that file's cue count in the engine;
 *   2. the double open at first play (setsubtitletrack(0) twice within milliseconds);
 *   3. reselect the track while its own feed is still running (the engine drops the old pass's cues);
 *   4. a storm of back-to-back selections of both tracks, ending on one: that track must be complete,
 *      with no cue from the other file and no duplicates.
 *
 * argv[1] = srt | vtt.
 */
#include "stream_host.h"
#include "fake_backend.h"
#include "host_test_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LONG_CUES   1000
#define SHORT_CUES  150
#define WAIT_MS     8000

static HOST_STREAM *g_h;
static int g_long, g_short;                     /* indexes into s->av.sub[] */

/* Selects `idx` and expects the engine to end up holding exactly `want` cues of that file. */
static int select_and_expect(int idx, int want, const char *what)
{
    int rc = hs_select(g_h, idx);
    int ok = rc == 0 && hs_wait_cues(want, WAIT_MS) && hs_stays_at(want, 60);
    CHECK(ok, "%s: engine holds %d of %d cues (select rc=%d)", what, fake_backend_active_since_flush(), want, rc);
    return ok;
}

int main(int argc, char **argv)
{
    const char *fmt = argc > 1 ? argv[1] : "srt";
    int vtt = !strcmp(fmt, "vtt");
    t_watchdog(180, vtt ? "ext_reselect_vtt" : "ext_reselect_srt");
    fake_backend_set_mode(FAKE_EMIT_ON_DEMAND);
    fake_backend_reset_counters();

    g_h = hs_create();
    (vtt ? hs_write_vtt : hs_write_srt)(g_h, "01", 'L', LONG_CUES);
    (vtt ? hs_write_vtt : hs_write_srt)(g_h, "02", 'S', SHORT_CUES);
    int n = hs_start(g_h);
    g_long = hs_track(g_h, "01");
    g_short = hs_track(g_h, "02");
    CHECK(n == 2 && g_long >= 0 && g_short >= 0, "discovered both %s files as external tracks (%d found)", fmt, n);
    if (n != 2 || g_long < 0 || g_short < 0) { hs_destroy(g_h); return t_finish(); }

    /* ---- first play: whichever track is the default must load by itself ---- */
    int def_want = g_h->s->av.subs == g_long ? LONG_CUES : SHORT_CUES;
    CHECK(hs_wait_cues(def_want, WAIT_MS) && hs_stays_at(def_want, 60),
          "first play: the default track loads without being touched (%d of %d cues)", fake_backend_active_since_flush(), def_want);

    /* ---- 1. the sequence from the report ---- */
    for (int round = 0; round < 4; round++) {
        char what[64];
        snprintf(what, sizeof what, "round %d: select 02 (short)", round);
        select_and_expect(g_short, SHORT_CUES, what);
        snprintf(what, sizeof what, "round %d: select 01 (long)", round);
        select_and_expect(g_long, LONG_CUES, what);
    }
    CHECK(fake_backend_foreign() == 0, "no cue of one file reached the other file's track (foreign=%d)", fake_backend_foreign());

    /* ---- 2. the app sends the same selection twice in a row ---- */
    hs_select(g_h, g_short);
    hs_select(g_h, g_short);
    CHECK(hs_wait_cues(SHORT_CUES, WAIT_MS) && hs_stays_at(SHORT_CUES, 60),
          "double select of the same track: complete, once (%d of %d)", fake_backend_active_since_flush(), SHORT_CUES);

    /* ---- 3. reselect while the track's own feed is still running ---- */
    fake_backend_set_feed_delay_us(150);          /* the long feed now takes ~150 ms: a window to reopen in */
    for (int i = 0; i < 6; i++) {
        hs_select(g_h, g_short);                  /* short and settled... */
        hs_wait_cues(SHORT_CUES, WAIT_MS);
        hs_select(g_h, g_long);                   /* ...long starts feeding... */
        for (int t = 0; t < 2000 && fake_backend_active_since_flush() < 80; t++) usleep(100);
        int mid = fake_backend_active_since_flush();
        hs_select(g_h, g_long);                   /* ...and is reopened half way: the old pass's cues are dropped */
        int ok = hs_wait_cues(LONG_CUES, WAIT_MS) && hs_stays_at(LONG_CUES, 60);
        CHECK(ok, "reopened mid-feed (engine held %d): long track still completes (%d of %d)",
              mid, fake_backend_active_since_flush(), LONG_CUES);
    }
    fake_backend_set_feed_delay_us(0);

    /* ---- 4. storm of selections, no waiting between them ---- */
    fake_backend_set_feed_delay_us(20);
    int last = g_long;
    for (int i = 0; i < 120; i++) {
        last = (rand() & 1) ? g_long : g_short;
        hs_select(g_h, last);
        if (rand() % 3 == 0) usleep((unsigned)(rand() % 4000));
    }
    fake_backend_set_feed_delay_us(0);
    int want = last == g_long ? LONG_CUES : SHORT_CUES;
    int ok = hs_wait_cues(want, WAIT_MS) && hs_stays_at(want, 100);
    CHECK(ok, "after 120 back-to-back selections the last one (%s) is complete: %d of %d cues",
          last == g_long ? "long" : "short", fake_backend_active_since_flush(), want);
    CHECK(fake_backend_foreign() == 0, "still no foreign cue after the storm (foreign=%d)", fake_backend_foreign());

    hs_destroy(g_h);
    CHECK(fake_backend_live() == 0, "no engine backend leaked (%d live)", fake_backend_live());
    return t_finish();
}
