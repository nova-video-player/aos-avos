#include "sub_engine.h"
#include "sub_render_gl.h"
#include "sub_format.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <android/log.h>

#define LOG_TAG "SubEngine"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

extern SUB_FORMAT_BACKEND *sub_format_ssa_create(void);
extern SUB_FORMAT_BACKEND *sub_format_srt_create(void);
extern SUB_FORMAT_BACKEND *sub_format_gfx_create(void);

struct SUB_ENGINE {
    SUB_RENDERER         *renderer;
    SUB_USER_STYLE       *style;
    SUB_FORMAT_BACKEND   *active_backend;

    int is_paused;
    sub_engine_clock_fn clock_fn;
    void *clock_ctx;
    pthread_mutex_t lock;
};

SUB_ENGINE *sub_engine_create(void) {
    SUB_ENGINE *eng = calloc(1, sizeof(SUB_ENGINE));
    eng->renderer = sub_render_gl_create();
    eng->style    = sub_style_create();
    pthread_mutex_init(&eng->lock, NULL);
    return eng;
}

void sub_engine_destroy(SUB_ENGINE *eng) {
    if (!eng) return;
    sub_engine_stop(eng);
    sub_engine_close_track(eng);
    sub_render_gl_destroy(eng->renderer);
    sub_style_destroy(eng->style);
    pthread_mutex_destroy(&eng->lock);
    free(eng);
}

void sub_engine_attach_surface(SUB_ENGINE *eng, ANativeWindow *window) { sub_render_gl_attach_surface(eng->renderer, window); }
void sub_engine_detach_surface(SUB_ENGINE *eng) { sub_render_gl_detach_surface(eng->renderer); }
void sub_engine_surface_resized(SUB_ENGINE *eng, int width, int height) { sub_render_gl_resize(eng->renderer, width, height); }

int sub_engine_open_track(SUB_ENGINE *eng, SUB_FORMAT_ID format_id, int video_w, int video_h, const uint8_t *codec_private, int codec_private_size) {
    sub_engine_close_track(eng);

    SUB_FORMAT_BACKEND *backend;
    if (format_id == SUB_FMT_SSA) {
        backend = sub_format_ssa_create();
    } else if (format_id == SUB_FMT_SRT) {
        backend = sub_format_srt_create(); // Routes SRT to your dynamic ASS generator!
    } else if (format_id == SUB_FMT_GFX) {
        backend = sub_format_gfx_create(); // Routes Bitmaps to OpenGL Texture Uploader!
    } else {
        return -1;
    }

    SUB_FORMAT_OPEN_PARAMS params = {
        .video_w = video_w, .video_h = video_h,
        .codec_private = codec_private, .codec_private_size = codec_private_size,
        .user_style = eng->style,
        .is_plain_text_format = (format_id == SUB_FMT_SRT) // Tell backend to force styles!
    };

    int rc = backend->open(backend, &params);
    if (rc != 0) {
        free(backend);
        return rc;
    }

    pthread_mutex_lock(&eng->lock);
    eng->active_backend = backend;
    pthread_mutex_unlock(&eng->lock);

    return 0;
}

void sub_engine_close_track(SUB_ENGINE *eng) {
    pthread_mutex_lock(&eng->lock);
    SUB_FORMAT_BACKEND *backend = eng->active_backend;
    eng->active_backend = NULL;
    pthread_mutex_unlock(&eng->lock);

    if (backend) {
        backend->close(backend);
        free(backend);
    }
    // Command the GL thread to dump memory and clear the screen safely
    sub_render_gl_clear(eng->renderer);
}

int sub_engine_feed(SUB_ENGINE *eng, const uint8_t *data, int size, int64_t pts_ms, int64_t duration_ms) {
    pthread_mutex_lock(&eng->lock);
    SUB_FORMAT_BACKEND *backend = eng->active_backend;
    pthread_mutex_unlock(&eng->lock);

    if (backend && backend->feed) {
        return backend->feed(backend, data, size, pts_ms, duration_ms);
    }
    return 0;
}

void sub_engine_flush(SUB_ENGINE *eng) {
    pthread_mutex_lock(&eng->lock);
    SUB_FORMAT_BACKEND *backend = eng->active_backend;
    pthread_mutex_unlock(&eng->lock);
    if (backend && backend->flush) backend->flush(backend);
}

void sub_engine_resize_video(SUB_ENGINE *eng, int video_w, int video_h) {
    pthread_mutex_lock(&eng->lock);
    SUB_FORMAT_BACKEND *backend = eng->active_backend;
    pthread_mutex_unlock(&eng->lock);
    if (backend && backend->resize) backend->resize(backend, video_w, video_h);
}

SUB_USER_STYLE *sub_engine_get_style(SUB_ENGINE *eng) { return eng->style; }

void sub_engine_start(SUB_ENGINE *eng, sub_engine_clock_fn clock_fn, void *clock_ctx) {
    if (!eng) return;
    pthread_mutex_lock(&eng->lock);
    eng->clock_fn  = clock_fn;
    eng->clock_ctx = clock_ctx;
    eng->is_paused = 0;
    pthread_mutex_unlock(&eng->lock);
}

void sub_engine_stop(SUB_ENGINE *eng) {
    if (!eng) return;
    pthread_mutex_lock(&eng->lock);
    eng->clock_fn  = NULL;
    eng->clock_ctx = NULL;
    pthread_mutex_unlock(&eng->lock);
}

void sub_engine_set_paused(SUB_ENGINE *eng, int paused) {
    if (!eng) return;
    pthread_mutex_lock(&eng->lock);
    eng->is_paused = paused;
    pthread_mutex_unlock(&eng->lock);
}

void sub_engine_get_stats(const SUB_ENGINE *eng, SUB_ENGINE_STATS *out) {
    if (!out) return;
    out->frames_rendered = 0;
    out->frames_skipped_unchanged = 0;
    out->avg_render_us = 0;
    out->active_format = eng && eng->active_backend ? SUB_FMT_SSA : SUB_FMT_UNKNOWN;
}

// --- NEW: Safe Polling Implementation ---
SUB_FRAME *sub_engine_poll_frame(SUB_ENGINE *eng) {
    if (!eng) return NULL;
    pthread_mutex_lock(&eng->lock);
    if (!eng->active_backend || !eng->clock_fn || eng->is_paused) {
        pthread_mutex_unlock(&eng->lock);
        return NULL;
    }
    int64_t pts = eng->clock_fn(eng->clock_ctx);
    SUB_FORMAT_BACKEND *be = eng->active_backend;
    pthread_mutex_unlock(&eng->lock);

    return be->render_at(be, pts);
}

// A global free that doesn't rely on backends, preventing UAF during teardowns
void sub_engine_free_frame(SUB_FRAME *frame) {
    if (!frame) return;
    SUB_EVENT *ev = frame->events;
    while (ev) {
        SUB_EVENT *next = ev->next;
        if (ev->kind == SUB_EVENT_BITMAP && ev->data.bitmap.rgba) {
            free((void*)ev->data.bitmap.rgba);
        }
        free(ev);
        ev = next;
    }
    free(frame);
}

int sub_engine_feed_bitmap(SUB_ENGINE *eng, uint8_t *pixels, int width, int height, int pitch, int colorspace, int x_offset, int y_offset, int64_t pts_ms, int64_t duration_ms) {
    if (!eng || !pixels || width <= 0 || height <= 0) return -1;

    pthread_mutex_lock(&eng->lock);
    SUB_FORMAT_BACKEND *backend = eng->active_backend;
    pthread_mutex_unlock(&eng->lock);

    if (backend && backend->feed_bitmap) {
        return backend->feed_bitmap(backend, pixels, width, height, pitch, colorspace, x_offset, y_offset, pts_ms, duration_ms);
    }
    return -1;
}
