#include "sub_engine.h"
#include "sub_render_gl.h"
#include "av.h"
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include "debug.h"

#define DBG if(Debug[DBG_SUB])

extern SUB_FORMAT_BACKEND *sub_format_ssa_create(void);
extern SUB_FORMAT_BACKEND *sub_format_srt_create(void);
extern SUB_FORMAT_BACKEND *sub_format_gfx_create(void);

/* Canonical SUB_FORMAT_* (av.h, 12 values, codec/container identity) ->
 * SUB_FMT_ID (sub_types.h, 3 values, engine backend selector) mapping.
 * Declared in sub_format.h. This used to be re-derived independently at
 * three call sites (stream_subtitle.c's internal-track ternary,
 * stream_sub_ext.c's vobsub/is_pgs/is_ssa chain, codec_ffsub.c's
 * hardcoded SUB_FMT_GFX literal) -- collapsed here so adding a new
 * SUB_FORMAT_* value only requires updating one place. */
SUB_FMT_ID sub_fmt_from_format(int fmt) {
    switch (fmt) {
    case SUB_FORMAT_SSA:
    case SUB_FORMAT_ASS:
        return SUB_FMT_SSA;
    case SUB_FORMAT_PGS:
    case SUB_FORMAT_DVD_GFX:
        return SUB_FMT_GFX;
    case SUB_FORMAT_TEXT:
    case SUB_FORMAT_EXT:
    case SUB_FORMAT_WEBVTT:
    case SUB_FORMAT_MOV_TEXT:
        return SUB_FMT_SRT;
    default:
        return SUB_FMT_UNKNOWN;
    }
}

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
    pthread_cond_t  wake_cond;
    uint64_t wakeup_generation; // <--- NEW: Predicate counter
    uint64_t track_generation;  // bumped by open_track()/close_track(); see
                                 // sub_engine_get_track_generation() in sub_engine.h
};

// Internal helper for safe wakeups
static void broadcast_wake_locked(SUB_ENGINE *eng) {
    eng->wakeup_generation++;
    pthread_cond_broadcast(&eng->wake_cond);
}

SUB_ENGINE *sub_engine_create(void) {
    SUB_ENGINE *eng = calloc(1, sizeof(SUB_ENGINE));

    // FIX: Initialize mutex and cond BEFORE spawning the thread
    pthread_mutex_init(&eng->lock, NULL);
    pthread_cond_init(&eng->wake_cond, NULL);

    // Pass eng straight into create() so r->engine is set before the render
    // thread is spawned -- see sub_render_gl_create()'s doc comment. The old
    // create()-then-set_engine() sequence left a window where the freshly
    // created thread's first loop iterations could read r->engine as NULL
    // (or race the plain-pointer write) before this second call landed.
    eng->renderer = sub_render_gl_create(eng);
    eng->style    = sub_style_create();
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
    pthread_cond_destroy(&eng->wake_cond);
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
// folder instead of the locked internal default, SUB_DEFAULT_FONT_FAMILY
// (see sub_style.h; consumed by sub_style_create()'s factory default and
// ssa_open()'s default_font fallback). Should be a
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

    DBG serprintf("SUB_SURFACE: Surface resized event received: %d x %d\n", width, height);

    pthread_mutex_lock(&eng->lock);
    eng->surface_w = width;
    eng->surface_h = height;
    pthread_mutex_unlock(&eng->lock);
    sub_render_gl_resize(eng->renderer, width, height);
    sub_engine_resize_video(eng, width, height); // <--- Tells Libass to wrap text to the new 3D box!
}

int sub_engine_open_track(SUB_ENGINE *eng, SUB_FMT_ID format_id, int video_w, int video_h,
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

    DBG serprintf("SUB_SURFACE: Opening track (format=%d) with canvas dimensions: %d x %d (raw video dim: %d x %d)\n",
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
    eng->track_generation++; // invalidates every in-flight sub_engine_*_gen()
                              // token captured against the track we're replacing
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
    eng->track_generation++; // same reasoning as open_track() above
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
    broadcast_wake_locked(eng); // <--- WAKE THE GL THREAD
    pthread_mutex_unlock(&eng->lock);
    return ret;
}

void sub_engine_flush(SUB_ENGINE *eng) {
    if (!eng) return;
    pthread_mutex_lock(&eng->lock);
    SUB_FORMAT_BACKEND *backend = eng->active_backend;
    if (backend && backend->flush) backend->flush(backend);
    broadcast_wake_locked(eng); // <--- WAKE THE GL THREAD
    pthread_mutex_unlock(&eng->lock);
}

uint64_t sub_engine_get_track_generation(SUB_ENGINE *eng) {
    if (!eng) return 0;
    pthread_mutex_lock(&eng->lock);
    uint64_t gen = eng->track_generation;
    pthread_mutex_unlock(&eng->lock);
    return gen;
}

int sub_engine_feed_gen(SUB_ENGINE *eng, uint64_t token, const uint8_t *data, int size, int64_t pts_ms, int64_t duration_ms) {
    if (!eng) return 0;
    pthread_mutex_lock(&eng->lock);
    if (token != eng->track_generation) {
        // This call belongs to a track that isn't the one currently open on
        // this engine anymore (open_track()/close_track() moved on since the
        // caller captured `token`) -- drop it silently instead of routing a
        // stale worker's cues into whatever track has replaced it.
        pthread_mutex_unlock(&eng->lock);
        return 0;
    }
    SUB_FORMAT_BACKEND *backend = eng->active_backend;
    int ret = 0;
    if (backend && backend->feed) {
        ret = backend->feed(backend, data, size, pts_ms, duration_ms);
    }
    broadcast_wake_locked(eng);
    pthread_mutex_unlock(&eng->lock);
    return ret;
}

void sub_engine_flush_gen(SUB_ENGINE *eng, uint64_t token) {
    if (!eng) return;
    pthread_mutex_lock(&eng->lock);
    if (token != eng->track_generation) {
        pthread_mutex_unlock(&eng->lock);
        return;
    }
    SUB_FORMAT_BACKEND *backend = eng->active_backend;
    if (backend && backend->flush) backend->flush(backend);
    broadcast_wake_locked(eng);
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
    broadcast_wake_locked(eng); // NEW
    pthread_mutex_unlock(&eng->lock);
}

SUB_USER_STYLE *sub_engine_get_style(SUB_ENGINE *eng) { return eng->style; }

void sub_engine_start(SUB_ENGINE *eng, sub_engine_clock_fn clock_fn, void *clock_ctx) {
    if (!eng) return;
    pthread_mutex_lock(&eng->lock);
    eng->clock_fn  = clock_fn;
    eng->clock_ctx = clock_ctx;
    eng->is_paused = 0;
    broadcast_wake_locked(eng);
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
    broadcast_wake_locked(eng); // NEW — broadcast on both pause and unpause
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
//
// Deliberately does NOT bail out just because eng->is_paused is set.
// sub_engine_wait_event() already stops TIMED polling while paused (it
// hardcodes timeout_ms = -1 instead of consulting get_timeout_ms(), so the
// 16ms libass animation tick and PGS/VobSub's normal wake schedule never
// fire while paused) -- the ONLY way this thread wakes while paused is an
// explicit broadcast_wake_locked() call: a style/resize setter, a
// pause/unpause toggle, or a feed/flush. Every one of those is a genuine
// invalidation that should still produce one fresh frame at the current
// (frozen, since clock_fn is expected to return the same value while
// paused) pts -- e.g. so a font-size change while paused is visible
// immediately instead of only appearing after the user unpauses. Bailing
// out here unconditionally, as before, silently dropped that render.
SUB_FRAME *sub_engine_poll_frame(SUB_ENGINE *eng) {
    if (!eng) return NULL;
    pthread_mutex_lock(&eng->lock);
    if (!eng->active_backend || !eng->clock_fn) {
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
    sub_frame_unref(frame);
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
    // FIX: Remove backend routing. Frames use global atomic refcounting.
    sub_frame_unref(frame);
}

int sub_engine_feed_bitmap(SUB_ENGINE *eng, uint8_t *pixels, int width, int height, int pitch, int colorspace, int x_offset, int y_offset, int64_t pts_ms, int64_t duration_ms) {
    if (!eng || !pixels || width <= 0 || height <= 0) return -1;

    pthread_mutex_lock(&eng->lock);
    SUB_FORMAT_BACKEND *backend = eng->active_backend;
    int ret = -1;
    if (backend && backend->feed_bitmap) {
        ret = backend->feed_bitmap(backend, pixels, width, height, pitch, colorspace, x_offset, y_offset, pts_ms, duration_ms);
    }
    broadcast_wake_locked(eng); // <--- WAKE THE GL THREAD
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
    broadcast_wake_locked(eng); // <--- WAKE THE GL THREAD
    pthread_mutex_unlock(&eng->lock);
    return ret;
}

void sub_frame_ref(SUB_FRAME *frame) {
    if (frame) {
        atomic_fetch_add(&frame->refcount, 1);
    }
}

void sub_frame_unref(SUB_FRAME *frame) {
    if (!frame) return;

    // atomic_fetch_sub returns the value BEFORE the subtraction.
    // If it was 1, it is now 0, meaning we hold the final reference and must free.
    if (atomic_fetch_sub(&frame->refcount, 1) == 1) {
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
}

// --- ADD THE WAIT FUNCTION ---
void sub_engine_wait_event(SUB_ENGINE *eng, uint64_t last_generation) {
    if (!eng) return;

    pthread_mutex_lock(&eng->lock);

    // Predicate check: if the generation bumped before we locked, skip the sleep!
    if (eng->wakeup_generation != last_generation) {
        pthread_mutex_unlock(&eng->lock);
        return;
    }

    int timeout_ms = -1;
    // FIX: Hard sleep if paused. Completely bypasses Libass 16ms polling.
    if (eng->is_paused) {
        timeout_ms = -1;
    } else if (eng->active_backend && eng->active_backend->get_timeout_ms && eng->clock_fn) {
        int64_t pts_ms = eng->clock_fn(eng->clock_ctx);
        timeout_ms = eng->active_backend->get_timeout_ms(eng->active_backend, pts_ms);
    }

    if (timeout_ms < 0) {
        // Sleep indefinitely until a broadcast
        pthread_cond_wait(&eng->wake_cond, &eng->lock);
    } else if (timeout_ms > 0) {
        // Sleep until timeout OR a broadcast
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        long long nsec = ts.tv_nsec + ((long long)timeout_ms * 1000000LL);
        ts.tv_sec += nsec / 1000000000LL;
        ts.tv_nsec = nsec % 1000000000LL;
        pthread_cond_timedwait(&eng->wake_cond, &eng->lock, &ts);
    }

    pthread_mutex_unlock(&eng->lock);
}

void sub_engine_force_wake(SUB_ENGINE *eng) {
    if (!eng) return;
    pthread_mutex_lock(&eng->lock);
    broadcast_wake_locked(eng);
    pthread_mutex_unlock(&eng->lock);
}

// Add the getter for the render thread
uint64_t sub_engine_get_generation(SUB_ENGINE *eng) {
    if (!eng) return 0;
    pthread_mutex_lock(&eng->lock);
    uint64_t gen = eng->wakeup_generation;
    pthread_mutex_unlock(&eng->lock);
    return gen;
}
