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

void sub_render_gl_clear(SUB_RENDERER *r);
void sub_render_gl_invalidate_cache(SUB_RENDERER *r);
void sub_render_gl_set_ui_mode(SUB_RENDERER *r, int mode);

// Monotonic counter, bumped only when the render thread swaps in a genuinely new,
// content-distinct SUB_FRAME (i.e. render_at() returned non-NULL and it differs from
// what was already current -- see egl_render_thread()'s "new_frame != r->current_frame"
// check). Unlike applied_generation (bumps every completed poll pass, content-changed
// or not) this only advances on an actual content change, mirroring the same
// change-detection every backend's render_at() already does internally (libass's
// ass_render_frame() &change flag for SSA/SRT, the is_dirty flag for GFX/PGS) --
// this just exposes that existing signal across the JNI boundary. Safe to read from
// any thread; always read/written under r->lock.
uint64_t sub_render_gl_get_frame_generation(SUB_RENDERER *r);

// --- HYBRID 3D BRIDGE ---
// Extracts the current frame into an Android CPU Bitmap for the Java 3D Shader.
// If out_generation is non-NULL, it is set (under the same lock as the blend itself,
// so it can't race a concurrent frame swap) to the frame_generation of the frame that
// was just blended -- callers can cache this and skip a future call entirely when it
// comes back unchanged, rather than re-clearing/re-blending/re-posting identical pixels.
int sub_render_gl_fill_bitmap(SUB_RENDERER *r, void* pixels, int dst_w, int dst_h, int dst_stride, uint64_t *out_generation);
void sub_render_gl_wait_for_generation(SUB_RENDERER *r, uint64_t target_generation, int timeout_ms);
