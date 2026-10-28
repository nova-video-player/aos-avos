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

#pragma once
/*
 * A STREAM just real enough to run the REAL external-subtitle code on the host.
 *
 * Nothing in here re-implements stream_sub_ext.c, stream_subtitle.c, subtitle_formats.c or the
 * SRT/VTT parsers: those are linked and driven through their own entry points --
 *
 *     stream_sub_ext_check()          discovery of real .srt/.vtt files written by the test
 *     stream_sub_dec_thread()         the real subtitle decode thread -> _sub_decode() -> _get_next_ext_sub()
 *                                     -> sub_engine_open_track() / stream_sub_ext_set_track_generation() /
 *                                     stream_sub_ext_feed_engine() / stream_sub_ext_force_streaming_refeed()
 *     stream_set_subtitle_stream()    the call behind the app's setsubtitletrack()
 *     stream_sub_ext_close()          what stream_stop() calls
 *     the parse-worker pool, feed_SRT()/feed_VTT(), the cue cache and the SUBT_PARSE_* state machine
 *
 * What is stood in for (stream_host.c): the demuxer/parser thread (a loop that only acknowledges
 * thread-state requests, as the real one does between packets), pause/seek/clock plumbing that the
 * subtitle code merely calls out to, and the audio-speed lookup. The engine behind it is the real
 * sub_engine.c with fake_backend.c as its backend, so "how many cues does the engine hold" is
 * fake_backend_active_since_flush().
 */
#include <stdint.h>
#include "stream.h"
#include "sub_engine.h"

typedef struct HOST_STREAM {
    STREAM     *s;
    SUB_ENGINE *eng;
    char        dir[256];          /* temp directory holding the video placeholder and subtitle files */
    char        video[300];
    pthread_t   sub_th, parser_th;
    int         started;
    int         owns_engine;       /* 0: the engine came from the caller (e.g. the registry) and outlives this stream */
} HOST_STREAM;

/* Writes <dir>/movie.<tag>.srt (or .vtt) with n cues. Every cue's text starts with `label`, so the
 * fake backend can tell if a cue from another file ever reaches it (fake_backend_foreign()). Cue i
 * is shown from i*1000 ms for 900 ms. Call before hs_start(). Returns the file's path (static). */
const char *hs_write_srt(HOST_STREAM *h, const char *tag, char label, int n);
const char *hs_write_vtt(HOST_STREAM *h, const char *tag, char label, int n);

HOST_STREAM *hs_create(void);
/* Same, on an engine owned by the caller (typically acquired from the sub-engine registry, like
 * avos_mp_video_open() does). Starts the engine clock. Pair with hs_close_video(). */
HOST_STREAM *hs_create_on(SUB_ENGINE *eng);
/* Discovers the files (stream_sub_ext_check), selects track 0 and starts the subtitle thread and
 * the parser stand-in. Returns the number of external tracks found. */
int  hs_start(HOST_STREAM *h);
/* Index into s->av.sub[] of the external track backed by movie.<tag>.<ext>, or -1. */
int  hs_track(HOST_STREAM *h, const char *tag);
/* Stops the threads, closes the external subtitle state like stream_stop() does, frees everything. */
void hs_destroy(HOST_STREAM *h);

/* What avos_mp_video_close() does, in its order: stop the engine clock; stop the stream (subtitle thread,
 * parse workers via stream_sub_ext_close()); only then close the engine's track. The engine itself is NOT
 * destroyed (the caller releases its registry reference afterwards). Frees the stream. For a stream made
 * by hs_create_on(). */
void hs_close_video(HOST_STREAM *h);
/* True if the engine can currently produce a frame for the screen (something is visible). Emits a frame
 * from the fake backend first, so "visible" means "the engine has a live track and renders it". */
int  hs_visible(HOST_STREAM *h);

/* The app's "select this subtitle track" (stream_set_subtitle_stream). Returns its result (0 = ok). */
int  hs_select(HOST_STREAM *h, int idx);
/* Idle then resume the subtitle thread: what pause-for-reconfigure, speed change, a seek and a
 * subtitle rescan all do around their work (thread_state_set(&s->sub_tstate, ...)). `hold_us` is how
 * long it stays idle. */
void hs_idle_sub_thread(HOST_STREAM *h, int hold_us);

/* Like hs_idle_sub_thread(), but stays idle only until the running feed has NOTICED the request (the
 * parse worker checks every 200 cues, so a shorter pulse would be missed and nothing would be
 * interrupted): it waits for the engine's cue count to drop to 0 (the feed flushed), then lets the
 * subtitle thread resume after `extra_us` more. Returns 1 if the feed was seen to react within
 * timeout_ms, 0 if it never did (the pulse was wasted -- a test must not count it as an interruption). */
int  hs_interrupt_feed(HOST_STREAM *h, int extra_us, int timeout_ms);

/* Waits until the engine's current track holds exactly n cues. 1 if it did within timeout_ms. */
int  hs_wait_cues(int n, int timeout_ms);
/* After hs_wait_cues() succeeded: stays at n for settle_ms (no late flush, no duplicate feed). */
int  hs_stays_at(int n, int settle_ms);
