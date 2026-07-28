#include "sub_engine.h"
#include "sub_render_gl.h"
#include "sub_format.h"
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
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

    int surface_w;
    int surface_h;

    // Custom fonts folder (MX Player / mpv-android style third-party fonts
    // dir). Both are simple owned heap strings guarded by eng->lock, snapshot
    // into SUB_FORMAT_OPEN_PARAMS at open_track() time -- the SSA backend
    // reads them once at ass_renderer/ass_add_font time in ssa_open() and
    // does not need live updates mid-track (changing fonts mid-playback of
    // the SAME track isn't a supported use case; switching tracks/files
    // picks up the latest value naturally since open_track() re-reads it).
    char *fonts_dir;          // folder to scan for .ttf/.otf/.ttc, or NULL
    char *default_font_name;  // family name to use as fallback (e.g. for SRT), or NULL

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
    free(eng->fonts_dir);
    free(eng->default_font_name);
    pthread_mutex_destroy(&eng->lock);
    free(eng);
}

// Sets the folder to scan for extra .ttf/.otf/.ttc fonts (third-party fonts
// folder, à la MX Player / mpv-android). Takes effect on the NEXT
// open_track() call -- it does not touch whatever backend is already active,
// mirroring how style changes need sync_styles() to notice a serial bump
// rather than reaching into a live ASS_Renderer's font provider directly.
// Pass NULL or "" to clear (falls back to fontconfig-only resolution).
void sub_engine_set_fonts_dir(SUB_ENGINE *eng, const char *dir) {
    if (!eng) return;
    pthread_mutex_lock(&eng->lock);
    free(eng->fonts_dir);
    eng->fonts_dir = (dir && dir[0]) ? strdup(dir) : NULL;
    pthread_mutex_unlock(&eng->lock);
}

// Sets the fallback family name libass should use when nothing else names a
// font -- this is what makes plain SRT actually use a font from the custom
// folder instead of fontconfig's generic "sans-serif" alias. Should be a
// family name that's resolvable given the CURRENT fonts_dir (typically one
// of the files just scanned by sub_engine_set_fonts_dir()); takes effect on
// the next open_track() call, same as fonts_dir above.
void sub_engine_set_default_font_name(SUB_ENGINE *eng, const char *name) {
    if (!eng) return;
    pthread_mutex_lock(&eng->lock);
    free(eng->default_font_name);
    eng->default_font_name = (name && name[0]) ? strdup(name) : NULL;
    pthread_mutex_unlock(&eng->lock);
}

void sub_engine_attach_surface(SUB_ENGINE *eng, ANativeWindow *window) {
    if (!eng) return;
    // A new native window is a NEW surface. Any surface_w/h we cached from
    // whatever was attached before (e.g. the full-screen player.xml surface,
    // right before switching into floating_player.xml's separate
    // gl_subtitle_view) describes that old surface, not this one. If
    // open_track() runs before this new surface's own surface_resized()
    // callback arrives, it must fall back to the real video_w/h rather than
    // silently inheriting a foreign surface's size. So: invalidate on every
    // attach, not just on detach, since some callers attach a new window
    // without ever detaching the previous one first.
    pthread_mutex_lock(&eng->lock);
    eng->surface_w = 0;
    eng->surface_h = 0;
    pthread_mutex_unlock(&eng->lock);
    sub_render_gl_attach_surface(eng->renderer, window);
}
void sub_engine_detach_surface(SUB_ENGINE *eng) {
    if (!eng) return;
    // Mirror invalidation on detach too, so a gap between "surface gone" and
    // "next surface attached" can't leave a stale nonzero size sitting
    // around for open_track() to pick up in between.
    pthread_mutex_lock(&eng->lock);
    eng->surface_w = 0;
    eng->surface_h = 0;
    pthread_mutex_unlock(&eng->lock);
    sub_render_gl_detach_surface(eng->renderer);
}
void sub_engine_surface_resized(SUB_ENGINE *eng, int width, int height) {
    if (!eng) return;

    LOGD("SUB_SURFACE: Surface resized event received: %d x %d", width, height);

    pthread_mutex_lock(&eng->lock);
    eng->surface_w = width;
    eng->surface_h = height;
    pthread_mutex_unlock(&eng->lock);
    sub_render_gl_resize(eng->renderer, width, height);
    sub_engine_resize_video(eng, width, height); // <--- Tells Libass to wrap text to the new 3D box!
}

int sub_engine_open_track(SUB_ENGINE *eng, SUB_FORMAT_ID format_id, int video_w, int video_h,
                           const uint8_t *codec_private, int codec_private_size,
                           const SUB_EMBEDDED_FONT *embedded_fonts, int embedded_fonts_count) {
    if (!eng) return -1;

    // Use the actual reported surface size when known, for every format. This used to branch
    // per format_id (SRT/GFX got the surface size, SSA was locked to the raw video frame), but
    // Java no longer special-cases any category when sizing mSubtitleView -- the use_sub_margins
    // preference now applies uniformly (see SurfaceController.updateSurface()'s mSubtitleView
    // sizing block), so eng->surface_w/h already reflects the correct canvas for every format,
    // margins included.
    //
    // For SSA specifically: this does NOT distort the track's authored layout. PlayResX/
    // PlayResY is a property of the track itself (parsed from [Script Info]), not of
    // ass_set_frame_size() -- libass always scales that authored coordinate space to fit
    // whatever physical frame size it's given. Handing it a taller frame (top/bottom bars
    // included) just changes what physical canvas that same authored layout gets mapped onto,
    // which is exactly the intended mpv-style "use the margins" behavior -- it does not change
    // the proportions of anything the author actually authored.
    //
    // Falls back to the raw video_w/video_h when no surface size is known yet (e.g. before the
    // first onSurfaceTextureAvailable/onSurfaceTextureSizeChanged callback has fired).
    int target_w, target_h;
    pthread_mutex_lock(&eng->lock);
    target_w = eng->surface_w > 0 ? eng->surface_w : video_w;
    target_h = eng->surface_h > 0 ? eng->surface_h : video_h;
    pthread_mutex_unlock(&eng->lock);

    LOGD("SUB_SURFACE: Opening track (format=%d) with canvas dimensions: %d x %d (raw video dim: %d x %d)",
         format_id, target_w, target_h, video_w, video_h);

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

    // Snapshot the fonts-dir settings under lock as OWNED COPIES, not raw
    // pointers into eng->fonts_dir/eng->default_font_name. open_track() runs
    // backend->open() (potentially slow: it scans a directory and reads
    // every font file in it) entirely AFTER this unlock, so a concurrent
    // sub_engine_set_fonts_dir()/sub_engine_set_default_font_name() call on
    // another thread could free() the live buffer out from under a held raw
    // pointer -- the same use-after-free shape sub_style_snapshot() already
    // guards against for font_family, for the same reason. These locals are
    // freed below once backend->open() returns.
    pthread_mutex_lock(&eng->lock);
    char *fonts_dir_snapshot = eng->fonts_dir ? strdup(eng->fonts_dir) : NULL;
    char *default_font_snapshot = eng->default_font_name ? strdup(eng->default_font_name) : NULL;
    pthread_mutex_unlock(&eng->lock);

    SUB_FORMAT_OPEN_PARAMS params = {
        .video_w = target_w, .video_h = target_h,
        .codec_private = codec_private, .codec_private_size = codec_private_size,
        .user_style = eng->style,
        .is_plain_text_format = (format_id == SUB_FMT_SRT), // Tell backend to force styles!
        .fonts_dir = fonts_dir_snapshot,
        .default_font_name = default_font_snapshot,
        // Passed straight through, same as codec_private above -- no
        // snapshot/copy needed since backend->open() (load_embedded_fonts()
        // in sub_format_ssa.c) only reads embedded_fonts[i].data
        // synchronously during this call, handing each blob straight to
        // ass_add_font() (which copies internally) before returning.
        .embedded_fonts = embedded_fonts,
        .embedded_fonts_count = embedded_fonts_count
    };

    int rc = backend->open(backend, &params);
    // backend->open() (ssa_open() in particular) only reads params.fonts_dir /
    // params.default_font_name synchronously during this call -- to scan the
    // directory and register fonts -- and does not retain either pointer, so
    // it's safe to free our local copies unconditionally now, regardless of
    // which branch below runs next.
    free(fonts_dir_snapshot);
    free(default_font_snapshot);

    if (rc != 0) {
        // backend->open() may have partially constructed a private ctx (e.g.
        // calloc'd SSA_BACKEND and initialised the mutex before failing on
        // ass_renderer_init). Call close() first so the backend can drain
        // whatever it managed to allocate before we free the shell itself.
        if (backend->close) backend->close(backend);
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
        // Direct passthrough: whatever size the caller reports IS both the
        // libass canvas and where it's drawn -- for the 2D TextureView path
        // that's now correct by construction (SurfaceController sizes
        // mSubtitleView itself: full-screen for plain text, tethered to the
        // video's own box for embedded ASS/SSA -- see updateSurface()), and
        // for the 3D hybrid CPU-blend path (draw3DSubtitles) it's always the
        // full physical screen regardless of format, same as it always was.
        // No format-specific branching needed here at all.
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

// sub_engine_release_frame
//
// Backend-aware frame release. sub_render_gl.c must call this instead of the
// bare sub_engine_free_frame() so that backends which pool or reuse memory
// (e.g. a future hardware-buffer backend) can override the teardown path via
// their own free_frame() vtable entry.
//
// Falls back to sub_engine_free_frame() when the engine or backend is gone
// (e.g. called during teardown after close_track).
void sub_engine_release_frame(SUB_ENGINE *eng, SUB_FRAME *frame) {
    if (!frame) return;
    if (eng && eng->active_backend && eng->active_backend->free_frame) {
        eng->active_backend->free_frame(eng->active_backend, frame);
    } else {
        // Backend already closed or never set — fall back to global free
        sub_engine_free_frame(frame);
    }
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

// sub_engine_feed_raw
//
// Feeds a complete raw ASS/SSA script buffer directly to the active backend.
// Used by stream_sub_ext_feed_engine() for external .ass/.ssa files — the
// entire file contents go in one call so Libass processes the full [Script
// Info], [V4+ Styles], and all [Events] in one shot, identical to how
// internal embedded SSA tracks are handled via ass_process_codec_private +
// ass_process_data in ssa_open / ssa_feed.
//
// pts_ms and duration_ms are 0: the timing is encoded inside the ASS data.
int sub_engine_feed_raw(SUB_ENGINE *eng, const uint8_t *data, int size) {
    if (!eng || !data || size <= 0) return 0;
    pthread_mutex_lock(&eng->lock);
    SUB_FORMAT_BACKEND *backend = eng->active_backend;
    int ret = 0;
    if (backend && backend->feed) {
        // Pass pts_ms=0, duration_ms=0 — timing is embedded in the ASS data
        ret = backend->feed(backend, data, size, 0, 0);
    }
    pthread_mutex_unlock(&eng->lock);
    return ret;
}
