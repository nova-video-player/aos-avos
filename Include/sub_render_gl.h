#pragma once

#include "sub_types.h"
#include <android/native_window.h>

typedef struct SUB_RENDERER SUB_RENDERER;

// `engine` is stored into r->engine BEFORE the render thread is created, so
// the thread's very first loop iteration already sees a valid pointer --
// there is no window where an unlocked r->engine read on the render thread
// can race a separate, later publication call. There is no setter for
// r->engine after creation; pass the real engine pointer here up front.
SUB_RENDERER *sub_render_gl_create(void *engine);
void          sub_render_gl_destroy(SUB_RENDERER *r);

void sub_render_gl_attach_surface(SUB_RENDERER *r, ANativeWindow *window);
void sub_render_gl_detach_surface(SUB_RENDERER *r);
void sub_render_gl_resize(SUB_RENDERER *r, int width, int height);

// Drops whatever is showing. This variant takes the engine lock (to wake the render thread),
// so it must NOT be called with the engine lock held -- use sub_render_gl_clear_nowake() there.
// --- CONTENT-CHANGED NOTIFICATION (native -> Java push for the 3D CPU-blend path) ---
// The 3D overlay is PULLED (fill_bitmap) on video frames and style changes, so while the
// player is paused nothing pulls a change that originates natively -- a track switch/close,
// or a new track's first cue arriving. This callback is that missing push.
//
// Invoked on the render thread, with NO renderer or engine lock held (so it may call back into
// the engine), when ALL of these hold: a callback is registered, ui_mode is a 3D mode, the
// engine is paused, and frame_generation differs from the last value announced. Comparing
// generations (rather than reacting to "a frame was published") means clears from
// open_track()/close_track() notify too, and a change that landed just before a pause is
// announced as soon as the pause is observed. While playing nothing is announced: the per-
// video-frame pull already covers it. It must be quick and non-blocking (it stalls the render
// thread), and is guaranteed not to be running once sub_render_gl_destroy() has returned.
typedef void (*sub_render_change_cb)(void *ctx);
void  sub_render_gl_set_change_callback(SUB_RENDERER *r, sub_render_change_cb cb, void *ctx);
void *sub_render_gl_get_change_ctx(SUB_RENDERER *r);

void sub_render_gl_clear(SUB_RENDERER *r);
// Same, for callers that ALREADY hold the engine lock (open_track()/close_track()): clears in
// the caller's lock hold and does not wake -- the caller broadcasts. See "LOCK ORDER" in
// sub_engine.c: engine lock is always taken before the renderer's lock, never after.
void sub_render_gl_clear_nowake(SUB_RENDERER *r);
// Installs the result of a backend render_at() call. Caller MUST hold the engine lock.
// Takes ownership of `frame` (may be NULL). Returns 1 if visible content changed (new frame,
// or a clear of something showing), 0 for unchanged. See sub_render_gl.c for the exact
// NULL / same-frame / empty-frame classification.
int  sub_render_gl_publish(SUB_RENDERER *r, SUB_FRAME *frame);
void sub_render_gl_invalidate_cache(SUB_RENDERER *r);
void sub_render_gl_set_ui_mode(SUB_RENDERER *r, int mode);

// Monotonic counter identifying what the renderer currently considers "on screen".
// Bumped, under r->lock, on every VISIBLE change: a new frame is published, or visible
// content is cleared (an empty frame from render_at(), or open_track()/close_track()).
// Never bumped for "same thing again", nor for clearing an already-clear screen. Unlike
// applied_generation (bumps every completed poll pass) this only advances on an actual
// content change, mirroring the change-detection every backend's render_at() already does
// (libass's &change flag for SSA/SRT, is_dirty for GFX/PGS).
//
// Prefer sub_render_gl_fill_bitmap() over reading this directly: it returns the generation
// belonging to the exact frame it decided on, atomically with the pixel copy. A separate
// read here can already describe a newer frame than pixels copied a moment earlier.
uint64_t sub_render_gl_get_frame_generation(SUB_RENDERER *r);

// --- HYBRID 3D BRIDGE ---
// What a fill_bitmap() pull decided the screen should show. Values are mirrored by
// SubtitleEngine.java's FILL_* constants -- keep in sync.
typedef enum {
    SUB_FILL_ERROR     = -1, // bad arguments; nothing decided, caller must not record *out_generation
    SUB_FILL_UNCHANGED =  0, // generation == last_generation (and !force): last post is still right; pixels untouched
    SUB_FILL_CLEAR     =  1, // nothing should be showing; pixels untouched (caller clears its own surface)
    SUB_FILL_FRAME     =  2  // pixels cleared and the current frame blended in
} SUB_FILL_RESULT;

// Extracts the current frame into an Android CPU Bitmap for the Java 3D Shader.
// *out_generation (required unless ERROR) is the generation of the exact frame -- or absence
// of one -- the returned result describes: frame pointer, generation and a pin on the frame
// are taken in one critical section, then the blend runs outside the lock on the pinned frame.
// last_generation: what the caller last posted; UINT64_MAX (Java: -1) means "nothing valid".
// force: never answer UNCHANGED (style-change redraws, where pixels differ though the frame
// identity does not).
SUB_FILL_RESULT sub_render_gl_fill_bitmap(SUB_RENDERER *r, void* pixels, int dst_w, int dst_h, int dst_stride,
                                          uint64_t last_generation, int force, uint64_t *out_generation);
void sub_render_gl_wait_for_generation(SUB_RENDERER *r, uint64_t target_generation, int timeout_ms);
