#include "sub_engine.h"
#include "sub_render_gl.h"
#include "sub_kind.h"
#include "av.h"
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include "debug.h"
#include "util.h"   // rst_to_ts_delta()

#define DBG if(Debug[DBG_SUB])

// Wait tuning for sub_engine_wait_event() -- all WALL-clock milliseconds.
#define SUB_ANIM_TICK_MS      16   // cadence while a backend reports "animating"
#define SUB_WAIT_CAP_MS      250   // longest single wait while a deadline is pending. The rst->wall
                                   // conversion uses the speed in force when the wait starts; capping
                                   // bounds how long a speed change (incl. a deferred atempo commit)
                                   // can leave a stale wall deadline in place.
#define SUB_NO_CLOCK_POLL_MS  50   // no valid clock (seek/init): nothing broadcasts when it comes back

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

/* What the rest of the app is told about a track. Deliberately a pure function
 * of sub_fmt_from_format(): whichever backend native picks is, by construction,
 * the kind that is reported. Do not add a second switch here. */
SUB_KIND sub_kind_from_format(int fmt) {
    switch (sub_fmt_from_format(fmt)) {
    case SUB_FMT_SSA: return SUB_KIND_SSA;
    case SUB_FMT_SRT: return SUB_KIND_PLAIN_TEXT;
    case SUB_FMT_GFX: return SUB_KIND_GRAPHIC;
    default:          return SUB_KIND_UNSUPPORTED;
    }
}

// LOCK ORDER: eng->lock is always taken BEFORE the renderer's lock (r->lock), never after.
// Everything that changes what the renderer considers "current" -- publishing a polled frame,
// and clearing on open_track()/close_track() -- runs inside a single eng->lock hold together
// with the render_at()/backend swap it belongs to. That is what makes "this frame's track is
// still the open track" and "install this frame" one atomic step. Code holding r->lock must
// therefore never call anything that takes eng->lock (force_wake, get_generation, ...).
struct SUB_ENGINE {
    SUB_RENDERER         *renderer;
    SUB_USER_STYLE       *style;
    SUB_FORMAT_BACKEND   *active_backend;

    int canvas_w;    // RENAMED from surface_w -- this is the on-screen subtitle rendering
    int canvas_h;    // canvas's pixel size (letterbox bars included), not a generic "surface".

    // Video's own on-screen box within the canvas above, as last reported by
    // sub_engine_set_video_box() (driven by SurfaceController). Cached here, same pattern
    // as canvas_w/h, so a freshly (re)opened GFX track picks up correct geometry
    // immediately instead of waiting for the next box-change event. All zero until the
    // first call -- open_track()/gfx_open() treat that as "assume video fills canvas 1:1".
    int video_box_x, video_box_y;
    int video_box_w, video_box_h;

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

    // Set by sub_engine_surface_resized()/sub_engine_set_video_box() (and by open_track()
    // installing a fresh backend) whenever canvas_w/h or video_box_* changes; cleared by
    // sub_engine_poll_frame() once it has pushed BOTH to active_backend together. This is
    // what makes the backend immune to the order/timing the two setters are called in (see
    // item #3 in the geometry bug list: canvas size and box arrive from Java through two
    // genuinely different timing domains -- box synchronously out of updateSurface()'s own
    // layout math, canvas asynchronously from Android's TextureView size-changed callback,
    // which can fire frames later). Rather than each setter pushing into the backend
    // immediately (which is exactly what let the backend see a canvas resize paired with a
    // box computed for the OLD canvas, or vice versa, for however long the gap between the
    // two calls happens to be), they only update the cache below and set this flag;
    // poll_frame() is the single place that ever calls backend->resize()/set_video_box(),
    // and it always applies whatever is CURRENTLY cached as one atomic pair, immediately
    // before every render. That doesn't make Java's delivery atomic -- it makes the backend's
    // view of geometry atomic, which is what actually matters for what gets rendered.
    //
    // A bitmask of SUB_GEOM_* rather than a plain bool: backend->resize() is the expensive
    // direction (libass re-wraps every line), so it only runs when SUB_GEOM_CANVAS is set;
    // set_video_box() is cheap and idempotent, so it runs whenever ANY bit is set -- which
    // also means a canvas change always re-syncs the box against the new canvas.
    int geometry_dirty;
};

#define SUB_GEOM_CANVAS 0x1  // canvas_w/h changed (or a fresh backend needs it)
#define SUB_GEOM_BOX    0x2  // video_box_* changed (or a fresh backend needs it)

// Internal helper for safe wakeups
static void broadcast_wake_locked(SUB_ENGINE *eng) {
    eng->wakeup_generation++;
    pthread_cond_broadcast(&eng->wake_cond);
}

SUB_ENGINE *sub_engine_create(void) {
    SUB_ENGINE *eng = calloc(1, sizeof(SUB_ENGINE));

    // FIX: Initialize mutex and cond BEFORE spawning the thread
    pthread_mutex_init(&eng->lock, NULL);
    // Timed waits are computed from a wall-clock DURATION, so wait on CLOCK_MONOTONIC: a
    // CLOCK_REALTIME deadline stretches or collapses when the system time is adjusted.
    pthread_condattr_t cattr;
    pthread_condattr_init(&cattr);
    pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
    pthread_cond_init(&eng->wake_cond, &cattr);
    pthread_condattr_destroy(&cattr);

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
    // A new native window is a NEW surface. Any canvas_w/h we cached from
    // whatever was attached before (e.g. the full-screen player.xml surface,
    // right before switching into floating_player.xml's separate
    // gl_subtitle_view) describes that old surface, not this one. If
    // open_track() runs before this new surface's own surface_resized()
    // callback arrives, it must fall back to the real video_w/h rather than
    // silently inheriting a foreign surface's size. So: invalidate on every
    // attach, not just on detach, since some callers attach a new window
    // without ever detaching the previous one first.
    pthread_mutex_lock(&eng->lock);
    eng->canvas_w = 0;
    eng->canvas_h = 0;
    // Mirror the canvas invalidation above for the video box: a box computed for whatever
    // surface was attached before describes THAT surface's geometry, not this new one's, and
    // open_track()'s box_x/y/w/h snapshot reads this cache directly. SurfaceController hasn't
    // had a chance to recompute and resend the real box for the new surface yet (see
    // SurfaceController.setVideoBoxListener()'s replay fix for the Java side of this same
    // handoff) -- until it does, falling back to "assume video fills canvas 1:1" (the same
    // convention zero already carries at this struct's video_box_x/y/w/h declaration) is safer
    // than silently keeping a stale box measured against a surface that's gone.
    eng->video_box_x = 0; eng->video_box_y = 0;
    eng->video_box_w = 0; eng->video_box_h = 0;
    // The cache above was just invalidated, but the LIVE backend still holds whatever box it
    // was last given. Mark the box dirty so poll_frame() pushes the (zeroed) cache into it;
    // otherwise the cache says "video fills canvas" while the backend keeps rendering with the
    // old surface's box until some later canvas change happens to re-sync it. The Java side
    // (SubtitleEngine.resendVideoBox()) replays the real box right after this call, which
    // just re-marks the same bit and coalesces into the same single apply. (poll_frame()
    // resolves a zero box to "fills canvas" and skips the push while the canvas is also
    // zero, as it is right now, so this never hands the backend a raw zero box.)
    eng->geometry_dirty |= SUB_GEOM_BOX;
    broadcast_wake_locked(eng);
    pthread_mutex_unlock(&eng->lock);
    sub_render_gl_attach_surface(eng->renderer, window);
}
void sub_engine_detach_surface(SUB_ENGINE *eng) {
    if (!eng) return;
    // Mirror invalidation on detach too, so a gap between "surface gone" and
    // "next surface attached" can't leave a stale nonzero size sitting
    // around for open_track() to pick up in between.
    pthread_mutex_lock(&eng->lock);
    eng->canvas_w = 0;
    eng->canvas_h = 0;
    // See the matching comment in sub_engine_attach_surface(): the box is invalidated on
    // detach for the same reason canvas_w/h already is -- so a gap between "surface gone" and
    // "next surface attached" can't leave a stale box sitting around for open_track() to pick
    // up in between.
    eng->video_box_x = 0; eng->video_box_y = 0;
    eng->video_box_w = 0; eng->video_box_h = 0;
    // Keep the live backend consistent with the invalidated cache -- same reason as in
    // sub_engine_attach_surface(). Callers that keep using the engine after a detach (the
    // 2D->3D switch) must replay the real box right after this call
    // (SubtitleEngine.resendVideoBox()); it coalesces with this bit into a single apply.
    eng->geometry_dirty |= SUB_GEOM_BOX;
    broadcast_wake_locked(eng);
    pthread_mutex_unlock(&eng->lock);
    sub_render_gl_detach_surface(eng->renderer);
}
void sub_engine_surface_resized(SUB_ENGINE *eng, int width, int height) {
    if (!eng) return;

    DBG serprintf("SUB_SURFACE: Surface resized event received: %d x %d\n", width, height);

    // Cache + mark dirty ONLY -- deliberately no backend->resize() here anymore. The format
    // backend (libass frame size / GFX canvas) is updated by sub_engine_poll_frame(), which
    // applies canvas and box to it together as one pair right before the next render. See
    // the geometry_dirty doc comment on struct SUB_ENGINE (item #3): this call and
    // sub_engine_set_video_box() arrive from Java through two different timing domains, so
    // pushing each into the backend on arrival is what let the backend see mismatched pairs.
    pthread_mutex_lock(&eng->lock);
    eng->canvas_w = width;
    eng->canvas_h = height;
    eng->geometry_dirty |= SUB_GEOM_CANVAS;
    broadcast_wake_locked(eng); // wake the render thread so poll_frame() applies it promptly
    pthread_mutex_unlock(&eng->lock);

    // The GL renderer's own framebuffer/surface resize stays eager: that's the physical
    // render target, not content layout, and has no pairing requirement with the box.
    sub_render_gl_resize(eng->renderer, width, height);
}

// See sub_engine.h for the full doc comment.
void sub_engine_set_video_box(SUB_ENGINE *eng, int x, int y, int w, int h) {
    if (!eng) return;

    DBG serprintf("SUB_SURFACE: Video box set: (%d,%d) %dx%d\n", x, y, w, h);

    // Cache + mark dirty only; sub_engine_poll_frame() pushes it to the active backend
    // together with the canvas size (see sub_engine_surface_resized() and the
    // geometry_dirty doc comment on struct SUB_ENGINE -- item #3). This also retires the
    // old use-after-free concern this function used to guard by calling into the backend
    // under the lock: it no longer touches the backend at all, so there's no pointer to
    // dangle -- poll_frame() reads active_backend under the same lock it renders under.
    pthread_mutex_lock(&eng->lock);
    eng->video_box_x = x; eng->video_box_y = y;
    eng->video_box_w = w; eng->video_box_h = h;
    eng->geometry_dirty |= SUB_GEOM_BOX;
    broadcast_wake_locked(eng); // same pattern feed()/flush()/etc. already use
    pthread_mutex_unlock(&eng->lock);
}

int sub_engine_open_track(SUB_ENGINE *eng, SUB_FMT_ID format_id, int video_w, int video_h,
                           const uint8_t *codec_private, int codec_private_size,
                           const SUB_EMBEDDED_FONT *embedded_fonts, int embedded_fonts_count,
                           uint64_t *out_generation) {
    if (!eng) return -1;
    if (out_generation) *out_generation = 0; // default until the swap below actually succeeds

    // Use the actual reported canvas size when known, for every format. This used to branch
    // per format_id (SRT/GFX got the surface size, SSA was locked to the raw video frame), but
    // Java no longer special-cases any category when sizing mSubtitleView -- the use_sub_margins
    // preference now applies uniformly (see SurfaceController.updateSurface()'s mSubtitleView
    // sizing block), so eng->canvas_w/h already reflects the correct canvas for every format,
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
    // target_w/target_h below is ONLY the canvas. video_w/video_h (this function's own params,
    // the decoded video size) are passed through separately as real_video_w/h. Exception: for
    // GFX the caller passes the subtitle stream's own coordinate frame in this slot (PGS/VobSub
    // bitmap coordinates are in that space, not the canvas's or the video's) -- see
    // sub_format_gfx.c's gfx_open(), which stamps it on frames as gfx_stream_w/h.
    //
    // Falls back to the raw video_w/video_h when no canvas size is known yet (e.g. before the
    // first onSurfaceTextureAvailable/onSurfaceTextureSizeChanged callback has fired).
    int target_w, target_h;
    int box_x, box_y, box_w, box_h;
    pthread_mutex_lock(&eng->lock);
    target_w = eng->canvas_w > 0 ? eng->canvas_w : video_w;
    target_h = eng->canvas_h > 0 ? eng->canvas_h : video_h;
    box_x = eng->video_box_x; box_y = eng->video_box_y;
    box_w = eng->video_box_w; box_h = eng->video_box_h;
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
        .real_video_w = video_w, .real_video_h = video_h,
        .video_box_x = box_x, .video_box_y = box_y,
        .video_box_w = box_w, .video_box_h = box_h,
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
    if (out_generation) *out_generation = eng->track_generation; // same lock
                              // hold as the bump above -- see this param's
                              // doc comment in sub_engine.h for why that
                              // atomicity is the whole point

    // Force the very next poll_frame() to push the CURRENT canvas/box into this fresh backend
    // before its first render (item #4). A surface_resized()/set_video_box() call landing
    // between the target_w/target_h/box_* snapshot taken above and this swap (backend->open()
    // can be slow: it scans a fonts folder) already updated the cache correctly, but had no
    // active backend to reach. Marking both bits dirty here reuses the same apply-at-render
    // mechanism as item #3 instead of a second, parallel push. poll_frame() skips the
    // resize() if no real canvas is known yet (canvas_w==0), in which case open() already got
    // the video_w/h fallback via target_w/target_h.
    eng->geometry_dirty |= (SUB_GEOM_CANVAS | SUB_GEOM_BOX);

    // Drop whatever the previous track had published, in THIS lock hold -- not after it.
    // poll_and_publish() renders and publishes under eng->lock, so once the backend is
    // swapped above no frame from the old track can be published afterwards, and no frame
    // from the NEW track can be published until we release the lock -- meaning this clear
    // can neither miss a stale frame nor discard a fresh one. (It used to run after the
    // unlock: the render thread could poll a frame from the old backend, lose the race to
    // this clear, and then install it -- a stale cue on screen for the new track.)
    sub_render_gl_clear_nowake(eng->renderer);
    broadcast_wake_locked(eng); // same pattern every other mutator in this file uses
    pthread_mutex_unlock(&eng->lock);

    if (old_backend) {
        old_backend->close(old_backend);
        free(old_backend);
    }

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
    // Clear in the same lock hold as the close -- see open_track() for why this must not be
    // a separate step after the unlock. No-wake variant: we hold eng->lock, so wake directly.
    sub_render_gl_clear_nowake(eng->renderer);
    broadcast_wake_locked(eng);
    pthread_mutex_unlock(&eng->lock);
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

// RENAMED from sub_engine_resize_video() -- see sub_engine.h's doc comment. NOTE: no longer
// called from sub_engine_surface_resized() -- that now only caches + marks geometry dirty,
// and sub_engine_poll_frame() applies backend->resize() paired with the box (item #3). This
// still resizes the backend immediately when called directly, so any other external caller
// keeps its old eager behavior; it bypasses the paired-apply path and clears nothing.
void sub_engine_resize_canvas(SUB_ENGINE *eng, int canvas_w, int canvas_h) {
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
        // No format-specific branching needed here at all. (For GFX, real
        // video size and the video's on-screen box are separate state --
        // see sub_engine_set_video_box() -- untouched by a canvas resize.)
        eng->active_backend->resize(eng->active_backend, canvas_w, canvas_h);
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

int sub_engine_is_paused(SUB_ENGINE *eng) {
    if (!eng) return 0;
    pthread_mutex_lock(&eng->lock);
    int paused = eng->is_paused;
    pthread_mutex_unlock(&eng->lock);
    return paused;
}

void sub_engine_set_change_callback(SUB_ENGINE *eng, sub_render_change_cb cb, void *ctx) {
    if (!eng) return;
    sub_render_gl_set_change_callback(eng->renderer, cb, ctx);
}

void *sub_engine_get_change_ctx(SUB_ENGINE *eng) {
    if (!eng) return NULL;
    return sub_render_gl_get_change_ctx(eng->renderer);
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
// hardcodes timeout_ms = -1 instead of consulting get_schedule(), so the
// 16ms libass animation tick and PGS/VobSub's normal wake schedule never
// fire while paused) -- the ONLY way this thread wakes while paused is an
// explicit broadcast_wake_locked() call: a style/resize setter, a
// pause/unpause toggle, or a feed/flush. Every one of those is a genuine
// invalidation that should still produce one fresh frame at the current
// (frozen, since clock_fn is expected to return the same value while
// paused) pts -- e.g. so a font-size change while paused is visible
// immediately instead of only appearing after the user unpauses. Bailing
// out here unconditionally, as before, silently dropped that render.
//
// Renders AND publishes to the renderer in one eng->lock hold (this used to return the frame
// and let the render thread install it later, across an unlocked gap in which close_track()/
// open_track() could complete -- see open_track()). Returns 1 if visible content changed
// (new frame, or a clear of something that was showing), 0 for "unchanged" -- including the
// no-track / no-clock cases, where nothing was polled at all. `r` is passed in by the render
// thread rather than read from eng->renderer: the thread is spawned inside
// sub_render_gl_create(), before sub_engine_create() has stored that pointer.
int sub_engine_poll_and_publish(SUB_ENGINE *eng, SUB_RENDERER *r) {
    if (!eng || !r) return 0;
    pthread_mutex_lock(&eng->lock);
    if (!eng->active_backend || !eng->clock_fn) {
        pthread_mutex_unlock(&eng->lock);
        return 0;
    }
    // Apply any pending canvas/box change to the backend as ONE pair, right before
    // rendering (item #3). This is the only place backend->resize()/set_video_box() are
    // called for live geometry now: whatever is CURRENTLY cached is applied together under
    // the same lock hold that renders, so the backend can never observe a canvas resize and
    // a box update as separate, independently-ordered mutations, no matter how far apart
    // Java's two calls landed. Rapid back-to-back calls also coalesce into a single apply.
    // (If the clock isn't running yet we returned above and the bits simply stay set.)
    if (eng->geometry_dirty) {
        SUB_FORMAT_BACKEND *be = eng->active_backend;
        if ((eng->geometry_dirty & SUB_GEOM_CANVAS) &&
                eng->canvas_w > 0 && eng->canvas_h > 0 && be->resize) {
            be->resize(be, eng->canvas_w, eng->canvas_h);
        }
        if (be->set_video_box) {
            int bx = eng->video_box_x, by = eng->video_box_y;
            int bw = eng->video_box_w, bh = eng->video_box_h;
            if (bw <= 0 || bh <= 0) {
                // No box known (never reported, or invalidated by attach/detach): by
                // contract that means "video fills canvas 1:1" -- the same fallback
                // open_track()/gfx_open() apply. Resolve it HERE rather than handing the
                // backend zeros: gfx_set_video_box() stores whatever it's given, so a raw
                // zero box would collapse every PGS/VobSub bitmap to zero size (invisible)
                // -- overriding the fallback gfx_open() had just installed.
                bx = 0; by = 0; bw = eng->canvas_w; bh = eng->canvas_h;
            }
            // Still unresolved (no canvas either): leave the backend on its open() fallback.
            if (bw > 0 && bh > 0) be->set_video_box(be, bx, by, bw, bh);
        }
        eng->geometry_dirty = 0;
    }
    int64_t pts = eng->clock_fn(eng->clock_ctx);
    if (pts < 0) {
        // No valid clock yet (seek/init: video_time is -1). Rendering at a made-up pts would
        // flash cues from the wrong position; the wait side re-polls until the clock returns.
        pthread_mutex_unlock(&eng->lock);
        return 0;
    }
    // render_at() now runs with eng->lock still held (see sub_engine_close_track)
    // instead of releasing the lock first and calling through a copied pointer —
    // this was the main use-after-free window: close_track() on another thread
    // (e.g. the next video opening) could free the backend in between.
    SUB_FRAME *frame = eng->active_backend->render_at(eng->active_backend, pts);
    int changed = sub_render_gl_publish(r, frame); // takes ownership of `frame`; lock order eng -> r
    pthread_mutex_unlock(&eng->lock);

    return changed;
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

SUB_FILL_RESULT sub_engine_fill_bitmap(SUB_ENGINE *eng, void* pixels, int w, int h, int stride,
                                       uint64_t last_generation, int force, uint64_t *out_generation) {
    if (!eng || !eng->renderer) return SUB_FILL_ERROR;
    return sub_render_gl_fill_bitmap(eng->renderer, pixels, w, h, stride, last_generation, force, out_generation);
}

uint64_t sub_engine_get_frame_generation(SUB_ENGINE *eng) {
    if (!eng || !eng->renderer) return 0;
    return sub_render_gl_get_frame_generation(eng->renderer);
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
            if (ev->pixel_refs) {
                // Shared pixel block (see sub_format_gfx.c): rgba points INTO it, and
                // pixel_refs is both its refcount and its malloc base. Drop this event's
                // reference; whoever drops the last one frees the whole block.
                if (atomic_fetch_sub(ev->pixel_refs, 1) == 1) free(ev->pixel_refs);
            } else if (ev->kind == SUB_EVENT_BITMAP && ev->data.bitmap.rgba) {
                free((void*)ev->data.bitmap.rgba);   // legacy: event owns a plain malloc
            }
            free(ev);
            ev = next;
        }
        free(frame);
    }
}

// Turns a backend's SUB_SCHEDULE (absolute rst deadline + animating flag) into a WALL-clock wait.
// Returns -1 = sleep until broadcast, otherwise >= 1. Never 0: a zero wait would skip the wait
// entirely and spin the render loop if a backend ever reported a deadline that is already due.
static int schedule_to_wall_ms(int64_t pts_rst, const SUB_SCHEDULE *sc) {
    int wall = -1;
    if (sc->next_rst_ms >= 0) {
        int64_t d = sc->next_rst_ms - pts_rst;
        // delta_wc == delta_ts (audio_speed_*_architecture.md), and rst_to_ts_delta() applies the
        // CURRENT slope of the timeline mapping -- so this is right at any speed, including one
        // whose commit was deferred (atempo).
        // Known, accepted inexactness: for an external text track with a subtitle ratio the clock
        // runs at d/n (0.959x / 1.043x) of rst, which this conversion does not model. Waking up to 4%
        // early is harmless (the loop just re-polls); up to 4% late is bounded by the wait cap to
        // ~10 ms, well under one video frame.
        double w = d > 0 ? ceil(rst_to_ts_delta((double)d)) : 1.0;
        if (w < 1.0) w = 1.0;
        if (w > SUB_WAIT_CAP_MS) w = SUB_WAIT_CAP_MS;
        wall = (int)w;
    }
    if (sc->animating && (wall < 0 || wall > SUB_ANIM_TICK_MS))
        wall = SUB_ANIM_TICK_MS;
    return wall;
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
    } else if (eng->active_backend && eng->clock_fn) {
        int64_t pts = eng->clock_fn(eng->clock_ctx);
        if (pts < 0) {
            timeout_ms = SUB_NO_CLOCK_POLL_MS;
        } else if (eng->active_backend->get_schedule) {
            SUB_SCHEDULE sc = { -1, 0 };
            eng->active_backend->get_schedule(eng->active_backend, pts, &sc);
            timeout_ms = schedule_to_wall_ms(pts, &sc);
        }
    }

    if (timeout_ms < 0) {
        // Sleep indefinitely until a broadcast
        pthread_cond_wait(&eng->wake_cond, &eng->lock);
    } else if (timeout_ms > 0) {
        // Sleep until timeout OR a broadcast
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);   // matches the condattr set in sub_engine_create()
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

// Like sub_engine_force_wake(), but atomically returns the generation the bump landed
// on, so the caller can hand that exact number to sub_engine_wait_for_render() and know
// unambiguously which generation it's waiting for -- doing the bump and the read as two
// separate locked calls would leave a window for another wake to sneak in between them.
uint64_t sub_engine_force_wake_and_get_generation(SUB_ENGINE *eng) {
    if (!eng) return 0;
    pthread_mutex_lock(&eng->lock);
    broadcast_wake_locked(eng);
    uint64_t gen = eng->wakeup_generation;
    pthread_mutex_unlock(&eng->lock);
    return gen;
}

// Blocks (bounded by timeout_ms) until the render thread has produced at least one
// frame reflecting every style/state change made before this call -- see
// sub_render_gl_wait_for_generation() for the actual mechanism. Safe to call from any
// thread, including a JNI call on the Java UI thread.
void sub_engine_wait_for_render(SUB_ENGINE *eng, uint64_t target_generation, int timeout_ms) {
    if (!eng || !eng->renderer) return;
    sub_render_gl_wait_for_generation(eng->renderer, target_generation, timeout_ms);
}

// Add the getter for the render thread
uint64_t sub_engine_get_generation(SUB_ENGINE *eng) {
    if (!eng) return 0;
    pthread_mutex_lock(&eng->lock);
    uint64_t gen = eng->wakeup_generation;
    pthread_mutex_unlock(&eng->lock);
    return gen;
}
