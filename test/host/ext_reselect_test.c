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
 *      with no cue from the other file and no duplicates;
 *   5. selecting and feeding while the ENGINE is paused (avos_mp_video_pause() -> sub_engine_set_paused):
 *      nothing may depend on the engine running, and the pause state must not change;
 *   6. subtitles switched off in the middle of a feed with stream_disable_subtitles() (what
 *      avos_mp_video_set_sub_track(<0) calls: a sticky "none" that also stops the subtitle thread from
 *      reopening the track): nothing may be visible afterwards, no cue of the still-running feed may land,
 *      the track must stay closed; switching the same track back on must then load it completely;
 *   7. the finished track renders.
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

/* A track that is not fed within WAIT_MS is the bug (select returned 0, nothing was ever fed -- the
 * report's `n_events=0`). Once one scenario has shown that, later waits are shortened so a broken
 * build fails in seconds with every symptom listed, rather than minutes with none. */
static int g_wait_ms = WAIT_MS;
static int g_silent_failures;

static HOST_STREAM *g_h;
static int g_long, g_short;                     /* indexes into s->av.sub[] */

static void note_result(int ok, int rc, int want, const char *what)
{
    int held = fake_backend_active_since_flush();
    CHECK(ok, "%s: engine holds %d of %d cues (select rc=%d)", what, held, want, rc);
    if (!ok && rc == 0 && held < want) {
        g_silent_failures++;
        g_wait_ms = 800;
        printf("      ^ SILENT FAILURE: the selection was accepted (rc=0) but the track was never (fully) fed -- "
               "the engine shows %d of %d cues, as in the bug report's n_events=0\n", held, want);
    }
}

/* Selects `idx` and expects the engine to end up holding exactly `want` cues of that file. */
static int select_and_expect(int idx, int want, const char *what)
{
    int rc = hs_select(g_h, idx);
    int ok = rc == 0 && hs_wait_cues(want, g_wait_ms) && hs_stays_at(want, 60);
    note_result(ok, rc, want, what);
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
    note_result(hs_wait_cues(def_want, g_wait_ms) && hs_stays_at(def_want, 60), 0, def_want,
                "first play: the default track loads without being touched");

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
    note_result(hs_wait_cues(SHORT_CUES, g_wait_ms) && hs_stays_at(SHORT_CUES, 60), 0, SHORT_CUES,
                "double select of the same track: complete, once");

    /* ---- 3. reselect while the track's own feed is still running ---- */
    int mid_feed = 0;
    fake_backend_set_feed_delay_us(150);          /* the long feed now takes ~150 ms: a window to reopen in */
    for (int i = 0; i < 6; i++) {
        hs_select(g_h, g_short);                  /* short and settled... */
        hs_wait_cues(SHORT_CUES, g_wait_ms);
        hs_select(g_h, g_long);                   /* ...long starts feeding... */
        for (int t = 0; t < 2000 && fake_backend_active_since_flush() < 80; t++) usleep(100);
        int mid = fake_backend_active_since_flush();
        hs_select(g_h, g_long);                   /* ...and is reopened half way: the old pass's cues are dropped */
        int ok = hs_wait_cues(LONG_CUES, g_wait_ms) && hs_stays_at(LONG_CUES, 60);
        char what[64];
        snprintf(what, sizeof what, "reopened mid-feed (engine held %d): long track still completes", mid);
        note_result(ok, 0, LONG_CUES, what);
        mid_feed += mid > 0 && mid < LONG_CUES;
    }
    fake_backend_set_feed_delay_us(0);
    CHECK(mid_feed == 6, "every reopen really landed in the middle of the feed (%d of 6) -- otherwise this proves nothing", mid_feed);

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
    int ok = hs_wait_cues(want, g_wait_ms) && hs_stays_at(want, 100);
    note_result(ok, 0, want, last == g_long ? "after 120 back-to-back selections the last one (long) is complete"
                                             : "after 120 back-to-back selections the last one (short) is complete");
    CHECK(fake_backend_foreign() == 0, "still no foreign cue after the storm (foreign=%d)", fake_backend_foreign());

    /* ---- 5. the engine is paused (user paused / 3D UI): loading and selecting must not depend on it running ---- */
    sub_engine_set_paused(g_h->eng, 1);
    select_and_expect(g_short, SHORT_CUES, "engine paused: select short");
    select_and_expect(g_long, LONG_CUES, "engine paused: select long");
    select_and_expect(g_short, SHORT_CUES, "engine paused: select short again");
    CHECK(sub_engine_is_paused(g_h->eng) == 1, "selecting and feeding did not change the engine's pause state");
    sub_engine_set_paused(g_h->eng, 0);
    CHECK(sub_engine_is_paused(g_h->eng) == 0, "resumed");

    /* ---- 6. subtitles switched off while a feed is running, then switched back on ---- */
    int off_visible = 0, off_landed = 0, off_cut = 0, off_reopened = 0;
    for (int i = 0; i < 8; i++) {
        fake_backend_set_feed_delay_us(100);
        hs_select(g_h, g_long);
        for (int t = 0; t < 2000 && fake_backend_active_since_flush() < 40; t++) usleep(100);
        int mid = fake_backend_active_since_flush();
        stream_disable_subtitles(g_h->s);                 /* what avos_mp_video_set_sub_track(<0) does: sticky "none" */
        off_cut += mid > 0 && mid < LONG_CUES;
        if (hs_visible(g_h)) off_visible++;
        for (int t = 0; t < 150; t++) {                   /* the worker is still feeding: nothing may land, nothing reopens */
            if (fake_backend_active_since_flush() != -1) { off_landed++; break; }
            usleep(1000);
        }
        if (fake_backend_live() != 0) off_reopened++;     /* the sticky flag keeps the subtitle thread from reopening the track */
        fake_backend_set_feed_delay_us(0);
        char what[80];
        snprintf(what, sizeof what, "switched back on after being off mid-feed (engine held %d): long track completes", mid);
        select_and_expect(g_long, LONG_CUES, what);
    }
    CHECK(off_cut == 8, "every switch-off really landed mid-feed (%d of 8)", off_cut);
    CHECK(off_visible == 0, "nothing was visible after the track was closed (%d of 8 times something was)", off_visible);
    CHECK(off_landed == 0, "no cue of the still-running feed landed in the engine after the track was closed (%d of 8 times one did)", off_landed);
    CHECK(off_reopened == 0, "the subtitle thread did not reopen the track while subtitles were off (%d of 8 times it did)", off_reopened);

    /* ---- 7. the finished track renders ---- */
    CHECK(hs_visible(g_h), "the finished track can render (the engine produces a frame)");

    hs_destroy(g_h);
    CHECK(fake_backend_live() == 0, "no engine backend leaked (%d live)", fake_backend_live());
    CHECK(g_silent_failures == 0, "no silent failures: every accepted selection ended with its cues in the engine (%d did not)", g_silent_failures);
    return t_finish();
}
