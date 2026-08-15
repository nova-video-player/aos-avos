#pragma once

#include "sub_types.h"
#include "sub_style.h"
#include "sub_format.h"
#include <android/native_window.h>
#include <stdint.h>

typedef struct SUB_ENGINE SUB_ENGINE;

SUB_ENGINE *sub_engine_create(void);
void        sub_engine_destroy(SUB_ENGINE *eng);

// `embedded_fonts`/`embedded_fonts_count` are fonts extracted from container
// attachments (e.g. MKV AVMEDIA_TYPE_ATTACHMENT streams -- see av.h's
// ATTACHED_FONT and stream_parser_ffmpeg.c's harvesting of them). Pass
// NULL/0 if the container has none, or fonts aren't relevant to this open
// (e.g. bitmap subtitle tracks). Same synchronous-only lifetime contract as
// codec_private above: only needs to stay valid for the duration of this
// call -- see SUB_EMBEDDED_FONT's doc comment in sub_format.h.
//
// `out_generation`, if non-NULL, is filled -- on success (return 0) -- with
// the track-generation token this newly-opened track now has, i.e. exactly
// what sub_engine_get_track_generation() would return if called immediately
// after this returns, but captured atomically as part of the same open/swap
// instead of by a second, separate, later call. Callers about to hand this
// track off to a long-lived, checkpointed feed job (see the
// TRACK-GENERATION TOKEN section below) should capture it HERE and store it
// with the job itself, rather than calling sub_engine_get_track_generation()
// afterwards from wherever that job actually runs -- a separate later call
// can race a concurrent track switch and pick up a NEWER generation than
// the one this open actually produced, silently reattaching the job to the
// wrong track. Pass NULL if the caller has no such job (e.g. internal
// embedded tracks fed synchronously, single-threaded, off
// stream_sub_dec_thread -- see the token section's own note on this).
// Left unfilled (untouched) if `eng` is NULL; set to 0 on any other failure.
int sub_engine_open_track(SUB_ENGINE *eng, SUB_FMT_ID format_id, int video_w, int video_h,
                           const uint8_t *codec_private, int codec_private_size,
                           const SUB_EMBEDDED_FONT *embedded_fonts, int embedded_fonts_count,
                           uint64_t *out_generation);
void sub_engine_close_track(SUB_ENGINE *eng);
int sub_engine_feed(SUB_ENGINE *eng, const uint8_t *data, int size, int64_t pts_ms, int64_t duration_ms);
void sub_engine_flush(SUB_ENGINE *eng);
void sub_engine_resize_video(SUB_ENGINE *eng, int video_w, int video_h);

// --- TRACK-GENERATION TOKEN ---
// open_track()/close_track() each bump an internal counter. Meant for feed
// sources that run asynchronously and across multiple calls relative to the
// currently-open track -- concretely, stream_sub_ext.c's SRT/VTT parse-worker
// pool, which streams a file's cues (or replays a cached list) cue-by-cue on
// a background thread, checkpointing only periodically. Between checkpoints,
// the user can switch tracks: open_track() swaps in a new backend, but the
// old worker -- unaware -- can keep calling sub_engine_feed()/flush(), which
// would silently land in the NEW backend (feed) or wipe out cues the NEW
// worker already fed it (flush), since both simply operate on "whatever
// backend is currently active" with no notion of which track a given call
// was FOR.
//
// Usage: capture the token once, then pass that same fixed value to every
// _gen() call made during that pass. Once open_track()/close_track() moves
// the engine on to a different track, the token goes stale and every _gen()
// call using it becomes a silent no-op instead of touching the new backend.
//
// Preferred capture point: sub_engine_open_track()'s `out_generation`
// out-param, read by whichever thread actually selects/opens the track, and
// stored with the job (e.g. on the SRT/VTT parse-worker's job struct) right
// then -- BEFORE that job can possibly be enqueued/observed by another
// thread. A later, separate sub_engine_get_track_generation() call made
// from wherever the job actually runs (e.g. a background worker thread) has
// a gap: the very track switch this token exists to detect can land between
// "this job is still the selected one" being checked and that separate call
// actually reading the generation, handing the job a NEWER token than the
// track it was really opened against and defeating the whole mechanism.
// sub_engine_get_track_generation() below remains available for callers
// with no such gap to worry about.
//
// Plain sub_engine_feed()/flush() above are unaffected and remain the right
// choice for synchronous, single-threaded callers (e.g. internal/embedded
// tracks fed directly off stream_sub_dec_thread) that can't overlap a track
// switch this way in the first place.
uint64_t sub_engine_get_track_generation(SUB_ENGINE *eng);
int  sub_engine_feed_gen(SUB_ENGINE *eng, uint64_t token, const uint8_t *data, int size, int64_t pts_ms, int64_t duration_ms);
void sub_engine_flush_gen(SUB_ENGINE *eng, uint64_t token);

typedef int64_t (*sub_engine_clock_fn)(void *ctx);
void sub_engine_start(SUB_ENGINE *eng, sub_engine_clock_fn clock_fn, void *clock_ctx);
void sub_engine_stop(SUB_ENGINE *eng);
void sub_engine_set_paused(SUB_ENGINE *eng, int paused);

void sub_engine_attach_surface(SUB_ENGINE *eng, ANativeWindow *window);
void sub_engine_detach_surface(SUB_ENGINE *eng);
void sub_engine_surface_resized(SUB_ENGINE *eng, int width, int height);

SUB_USER_STYLE *sub_engine_get_style(SUB_ENGINE *eng);

// --- CUSTOM FONTS FOLDER (MX Player / mpv-android style third-party fonts dir) ---
// Both are simple setters on the engine, snapshotted into SUB_FORMAT_OPEN_PARAMS the
// next time sub_engine_open_track() runs (see sub_engine.c) -- they do not reach into
// whatever backend is already active, so changing them mid-playback of the SAME track
// has no effect until the next open_track() (e.g. next video, or a track switch).
void sub_engine_set_fonts_dir(SUB_ENGINE *eng, const char *dir);          // NULL/"" disables
void sub_engine_set_default_font_name(SUB_ENGINE *eng, const char *name); // NULL/"" falls back to "sans-serif"

typedef struct {
    int64_t frames_rendered;
    int64_t frames_skipped_unchanged;
    int64_t avg_render_us;
    int     active_format;
} SUB_ENGINE_STATS;

void sub_engine_get_stats(const SUB_ENGINE *eng, SUB_ENGINE_STATS *out);

SUB_FRAME *sub_engine_poll_frame(SUB_ENGINE *eng);
void sub_engine_free_frame(SUB_FRAME *frame);                          // static global free — for internal use only
void sub_engine_release_frame(SUB_ENGINE *eng, SUB_FRAME *frame);      // backend-aware release — use this in sub_render_gl.c
int sub_engine_feed_bitmap(SUB_ENGINE *eng, uint8_t *pixels, int width, int height, int pitch, int colorspace, int x_offset, int y_offset, int64_t pts_ms, int64_t duration_ms);

void sub_engine_set_ui_mode(SUB_ENGINE *eng, int mode);

// --- HYBRID 3D BRIDGE ---
int sub_engine_fill_bitmap(SUB_ENGINE *eng, void* pixels, int w, int h, int stride);
int sub_engine_feed_raw(SUB_ENGINE *eng, const uint8_t *data, int size); // for external ASS/SSA raw file buffer

void sub_frame_ref(SUB_FRAME *frame);
void sub_frame_unref(SUB_FRAME *frame);
void sub_engine_wait_event(SUB_ENGINE *eng, uint64_t last_generation);
void sub_engine_force_wake(SUB_ENGINE *eng);
uint64_t sub_engine_get_generation(SUB_ENGINE *eng);
