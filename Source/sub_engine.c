#include "sub_engine.h"
#include "sub_render_gl.h"
#include "sub_format.h"
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <android/log.h>

#define LOG_TAG "SubEngine"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

extern SUB_FORMAT_BACKEND *sub_format_ssa_create(void);
extern SUB_FORMAT_BACKEND *sub_format_srt_create(void);
extern SUB_FORMAT_BACKEND *sub_format_gfx_create(void);
extern void sub_render_gl_set_engine(SUB_RENDERER *r, void *engine);

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
    sub_render_gl_set_engine(eng->renderer, eng);
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
void sub_engine_surface_resized(SUB_ENGINE *eng, int width, int height) {
    if (!eng) return;
    sub_render_gl_resize(eng->renderer, width, height);
    sub_engine_resize_video(eng, width, height); // <--- Tells Libass to wrap text to the new 3D box!
}

int sub_engine_open_track(SUB_ENGINE *eng, SUB_FORMAT_ID format_id, int video_w, int video_h, const uint8_t *codec_private, int codec_private_size) {
    if (!eng) return -1;

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

    // Atomically swap the new backend in for whatever was active, in a single
    // lock hold from "read what's currently active" to "install the new one".
    // This used to be sub_engine_close_track(eng) called up front, followed
    // *later* (after the possibly-slow backend->open() above) by a separate
    // lock/assign. That gap let two threads calling open_track() close
    // together race: e.g. a just-closing video's subtitle-decode thread
    // finishing its last loop iteration right as the next video's
    // subtitle-decode thread opens its own first track. Whichever thread's
    // create+open finished last would stomp active_backend without closing
    // what the other thread had just installed -- leaking a backend and
    // silently leaving the WRONG one (e.g. the previous video's) active, with
    // nothing left to trigger a correction. Doing the swap itself as one
    // lock-protected step closes that gap, the same way close_track() does.
    pthread_mutex_lock(&eng->lock);
    SUB_FORMAT_BACKEND *old_backend = eng->active_backend;
    eng->active_backend = backend;
    pthread_mutex_unlock(&eng->lock);

    if (old_backend) {
        old_backend->close(old_backend);
        free(old_backend);
    }

    // Drop any cached frame from the previous track
    sub_render_gl_clear(eng->renderer);

    return 0;
}

void sub_engine_close_track(SUB_ENGINE *eng) {
    if (!eng) return;
    // Close + free the backend while STILL holding eng->lock. Every other
    // function that touches active_backend (feed/flush/feed_bitmap/poll_frame,
    // below) now also holds this same lock for the entire duration of its call
    // into the backend, so a backend can never be freed here while another
    // thread — in practice the EGL render thread, continuously polling — is
    // still mid-call into that exact pointer. That was a real use-after-free
    // race before: this function used to copy the pointer, unlock, and only
    // then close()+free() it, while poll_frame()/feed()/etc. could already be
    // running on that same backend on another thread.
    pthread_mutex_lock(&eng->lock);
    SUB_FORMAT_BACKEND *backend = eng->active_backend;
    eng->active_backend = NULL;
    if (backend) {
        backend->close(backend);
        free(backend);
    }
    pthread_mutex_unlock(&eng->lock);

    // Command the GL thread to dump memory and clear the screen safely
    sub_render_gl_clear(eng->renderer);
}

int sub_engine_feed(SUB_ENGINE *eng, const uint8_t *data, int size, int64_t pts_ms, int64_t duration_ms) {
    if (!eng) return 0;
    pthread_mutex_lock(&eng->lock);
    SUB_FORMAT_BACKEND *backend = eng->active_backend;
    int ret = 0;
    if (backend && backend->feed) {
        // Held across the call so close_track() can't free this backend
        // out from under us mid-feed (see sub_engine_close_track).
        ret = backend->feed(backend, data, size, pts_ms, duration_ms);
    }
    pthread_mutex_unlock(&eng->lock);
    return ret;
}

void sub_engine_flush(SUB_ENGINE *eng) {
    if (!eng) return;
    pthread_mutex_lock(&eng->lock);
    SUB_FORMAT_BACKEND *backend = eng->active_backend;
    if (backend && backend->flush) backend->flush(backend);
    pthread_mutex_unlock(&eng->lock);
}

void sub_engine_resize_video(SUB_ENGINE *eng, int video_w, int video_h) {
    if (!eng) return;
    pthread_mutex_lock(&eng->lock);
    if (eng->active_backend && eng->active_backend->resize) {
        // Direct passthrough: Libass will wrap natively to whatever size Java tells it
        eng->active_backend->resize(eng->active_backend, video_w, video_h);
    }
    pthread_mutex_unlock(&eng->lock);
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
    // render_at() now runs with eng->lock still held (see sub_engine_close_track)
    // instead of releasing the lock first and calling through a copied pointer —
    // this was the main use-after-free window: close_track() on another thread
    // (e.g. the next video opening) could free the backend in between.
    SUB_FRAME *frame = eng->active_backend->render_at(eng->active_backend, pts);
    pthread_mutex_unlock(&eng->lock);

    return frame;
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
    int ret = -1;
    if (backend && backend->feed_bitmap) {
        ret = backend->feed_bitmap(backend, pixels, width, height, pitch, colorspace, x_offset, y_offset, pts_ms, duration_ms);
    }
    pthread_mutex_unlock(&eng->lock);
    return ret;
}

void sub_engine_set_ui_mode(SUB_ENGINE *eng, int mode) {
    if (!eng || !eng->renderer) return;
    sub_render_gl_set_ui_mode(eng->renderer, mode);
}

int sub_engine_fill_bitmap(SUB_ENGINE *eng, void* pixels, int w, int h, int stride) {
    if (!eng || !eng->renderer) return 0;
    return sub_render_gl_fill_bitmap(eng->renderer, pixels, w, h, stride);
}
