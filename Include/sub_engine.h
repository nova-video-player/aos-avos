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
int sub_engine_open_track(SUB_ENGINE *eng, SUB_FMT_ID format_id, int video_w, int video_h,
                           const uint8_t *codec_private, int codec_private_size,
                           const SUB_EMBEDDED_FONT *embedded_fonts, int embedded_fonts_count);
void sub_engine_close_track(SUB_ENGINE *eng);
int sub_engine_feed(SUB_ENGINE *eng, const uint8_t *data, int size, int64_t pts_ms, int64_t duration_ms);
void sub_engine_flush(SUB_ENGINE *eng);
void sub_engine_resize_video(SUB_ENGINE *eng, int video_w, int video_h);

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
