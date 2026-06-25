#pragma once

#include "sub_types.h"
#include "sub_style.h"
#include <android/native_window.h>
#include <stdint.h>

typedef struct SUB_ENGINE SUB_ENGINE;

SUB_ENGINE *sub_engine_create(void);
void        sub_engine_destroy(SUB_ENGINE *eng);

int sub_engine_open_track(SUB_ENGINE *eng, SUB_FORMAT_ID format_id, int video_w, int video_h, const uint8_t *codec_private, int codec_private_size);
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

typedef struct {
    int64_t frames_rendered;
    int64_t frames_skipped_unchanged;
    int64_t avg_render_us;
    int     active_format;
} SUB_ENGINE_STATS;

void sub_engine_get_stats(const SUB_ENGINE *eng, SUB_ENGINE_STATS *out);

SUB_FRAME *sub_engine_poll_frame(SUB_ENGINE *eng);
void sub_engine_free_frame(SUB_FRAME *frame);
int sub_engine_feed_bitmap(SUB_ENGINE *eng, uint8_t *pixels, int width, int height, int pitch, int colorspace, int x_offset, int y_offset, int64_t pts_ms, int64_t duration_ms);

void sub_engine_set_ui_mode(SUB_ENGINE *eng, int mode);

// --- HYBRID 3D BRIDGE ---
int sub_engine_fill_bitmap(SUB_ENGINE *eng, void* pixels, int w, int h, int stride);
