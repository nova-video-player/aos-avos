#include "sub_render_gl.h"
#include "sub_engine.h"
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include "debug.h"
#include <time.h>

#define DBG if(Debug[DBG_SUB])

struct SUB_RENDERER {
    ANativeWindow  *window;
    pthread_t       thread;
    pthread_mutex_t lock;
    int             running;
    int             surface_width;
    int             surface_height;
    int             ui_mode; // 0 = 2D, 1 = SBS, 2 = TB
    const SUB_FRAME *current_frame;
    int              pending_redraw; // unified "something changed, redraw regardless"
    uint64_t         applied_generation; // highest wakeup_generation this thread has finished a poll+store pass for
    uint64_t         frame_generation;   // identity of what current_frame currently means: bumped, under
                                         // lock, on EVERY visible change -- a new frame, a clear of
                                         // visible content (empty frame or track close/open). Never
                                         // bumped for "still the same thing". Always read together
                                         // with current_frame in one critical section; see
                                         // sub_render_gl_fill_bitmap().
    pthread_cond_t   frame_cond;         // broadcast whenever applied_generation advances
    GLuint           gl_program;
    GLuint           gl_texture;
    GLint            attrib_pos;
    GLint            attrib_tex;
    void            *engine;
    sub_render_change_cb change_cb;   // see sub_render_gl.h; both under r->lock
    void            *change_ctx;
};

static GLuint compile_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint status = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        char buf[512];
        glGetShaderInfoLog(shader, sizeof(buf), NULL, buf);
        serprintf("Shader compile error: %s\n", buf);
    }
    return shader;
}

static GLuint create_program(const char *vertex_src, const char *fragment_src) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment_src);
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    GLint status = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (!status) {
        char buf[512];
        glGetProgramInfoLog(program, sizeof(buf), NULL, buf);
        serprintf("Program link error: %s\n", buf);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return program;
}

// GFX (PGS/VobSub) placement: ev->x/y/w/h are in the subtitle coordinate frame
// (real_video_w/h); place them on the canvas.
//
// Where that frame comes from (codec_ffsub.c's open(), from the track's "size:" init data): PGS
// uses the plane size carried in its own PCS segment, independent of the video (usually
// 1920x1080 even when the video's letterbox bars were cropped to 1920x800, but 1280x720 /
// 3840x2160 etc. occur). VobSub uses the DVD subtitle canvas (idx "size:", e.g. 720x480). The
// bitmap x/y are FFmpeg's rect coords in that same space.
//
// One rule for both: the frame is scaled UNIFORMLY (aspect ratio always preserved, never
// stretched) to fit inside a target area, then centred on the video's on-screen box. Whatever the
// frame doesn't fill is left as margin on both sides: left/right for a frame narrower than the
// area (a 3:2 DVD frame on a 16:9 picture), top/bottom for one that is wider. The only thing that
// changes between modes is the target area:
//
//   * "Render in black bars" ON and bars exist (canvas taller than the video box -- the canvas
//     is what SurfaceController extends into the bars): the FULL canvas.
//       - PGS, landscape, 1920x1080 frame on a 1920x1080 screen with a 1920x800 picture: s = 1,
//         the frame fills the screen and the bottom band falls in the black bar.
//       - portrait: s is set by the screen width; the frame is centred on the picture, so the
//         part of it that extends past the picture renders just below/above the video.
//   * Option OFF (or no bars to use: canvas == box): just the VIDEO BOX. A 1920x1080 frame in a
//     1920x800 box scales down to s = 0.74 -- smaller, but not stretched.
//
// Consequence for anamorphic DVD VobSub: it is NOT stretched with the picture's pixel aspect, so
// on a 16:9 picture it is ~16% narrower than the picture-relative size, centred with side margins.
// The frame is assumed centred on the picture (a symmetric crop); the container/decoder give no
// crop offset, so an asymmetric crop would shift subs by half the difference.
//
// Left/right bars are never used: the canvas is always exactly as wide as the box.
// Outputs the destination rect and the src->dst scale on each axis (the CPU blender resamples
// with it). Shared by the GL path and the CPU blender (3D) so they can't drift apart.
static void gfx_map_to_canvas(const SUB_FRAME *f, const SUB_EVENT *ev,
                              float *out_x, float *out_y, float *out_w, float *out_h,
                              float *out_sx, float *out_sy) {
    const float ch = (float)f->video_h;                                   // canvas height
    const float fw = (float)f->real_video_w, fh = (float)f->real_video_h; // sub coordinate frame
    const float bx = (float)f->video_box_x,  by = (float)f->video_box_y;
    const float bw = (float)f->video_box_w,  bh = (float)f->video_box_h;  // video box in canvas

    const float bottom_bar = ch - (by + bh);
    const int   uses_bars  = by > 1.0f || bottom_bar > 1.0f;

    const float area_w = bw;                       // == canvas width whenever bars are used
    const float area_h = uses_bars ? ch : bh;      // full canvas vs. just the picture

    float s = area_w / fw;                         // contain
    if (area_h / fh < s) s = area_h / fh;

    // Centre the (scaled) frame on the video box.
    const float frame_w = fw * s, frame_h = fh * s;
    const float x0 = bx + (bw - frame_w) * 0.5f;
    float       y0 = by + (bh - frame_h) * 0.5f;
    if (uses_bars) {                               // keep it on the canvas (cutout asymmetry)
        if (y0 + frame_h > ch) y0 = ch - frame_h;
        if (y0 < 0.0f) y0 = 0.0f;
    }

    *out_x = x0 + ev->x * s;
    *out_y = y0 + ev->y * s;
    *out_w = ev->w * s;
    *out_h = ev->h * s;
    *out_sx = s; *out_sy = s;
}

static void* egl_render_thread(void* arg) {
    SUB_RENDERER *r = (SUB_RENDERER*)arg;

    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(display, NULL, NULL);

    const EGLint attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_BLUE_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_RED_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig config;
    EGLint numConfigs;
    eglChooseConfig(display, attribs, &config, 1, &numConfigs);

    const EGLint context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attribs);

    EGLSurface surface = EGL_NO_SURFACE;
    // Last frame_generation announced through change_cb. UINT64_MAX = "never": generations
    // never reach it, so the first paused pass with a callback set always announces once.
    uint64_t notified_generation = UINT64_MAX;
    ANativeWindow *current_window = NULL;

    while (1) {
        // 1. Sample the generation counter BEFORE doing any work
        uint64_t loop_generation = 0;
        if (r->engine) {
            loop_generation = sub_engine_get_generation((SUB_ENGINE*)r->engine);
        }

        int needs_redraw = 0;

        pthread_mutex_lock(&r->lock);
        if (!r->running) {
            pthread_mutex_unlock(&r->lock);
            break;
        }

        // 2. Safely acquire our own strong reference to the window
        ANativeWindow *target_window = r->window;
        if (target_window) {
            ANativeWindow_acquire(target_window);
        }
        pthread_mutex_unlock(&r->lock);

        // --- CLEAN EGL SURFACE CREATION ---
        if (target_window != current_window) {
            if (surface != EGL_NO_SURFACE) {
                eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                eglDestroySurface(display, surface);
                surface = EGL_NO_SURFACE;
                pthread_mutex_lock(&r->lock);
                r->surface_width  = 0;
                r->surface_height = 0;
                pthread_mutex_unlock(&r->lock);
            }

            if (current_window) ANativeWindow_release(current_window);
            current_window = target_window; // Transfer ownership

            if (current_window) {
                surface = eglCreateWindowSurface(display, config, current_window, NULL);
                needs_redraw = 1; // <--- 2. FORCE REDRAW ON NEW SURFACE
                if (surface != EGL_NO_SURFACE) {
                    eglMakeCurrent(display, surface, surface, context);
                    eglSwapInterval(display, 1);

                    // Adopt this new surface's REAL pixel size right away, instead of
                    // waiting for a separate sub_render_gl_resize() call to arrive from
                    // the Java/JNI side. That call is driven by an independent callback
                    // (TextureView's onSurfaceTextureSizeChanged) whose timing relative
                    // to attach_surface() isn't guaranteed -- e.g. switching to the
                    // floating player's window here would otherwise keep drawing with
                    // r->surface_width/height still left over from whichever window
                    // (typically full-screen) was attached before, until that callback
                    // happens to land. Querying EGL directly makes this self-correcting.
                    EGLint real_w = 0, real_h = 0;
                    eglQuerySurface(display, surface, EGL_WIDTH, &real_w);
                    eglQuerySurface(display, surface, EGL_HEIGHT, &real_h);
                    if (real_w > 0 && real_h > 0) {
                        pthread_mutex_lock(&r->lock);
                        r->surface_width  = real_w;
                        r->surface_height = real_h;
                        pthread_mutex_unlock(&r->lock);
                        DBG serprintf("SUB_RENDER_GL: adopted real EGL surface size %d x %d on window attach\n", real_w, real_h);
                    }

                    if (r->gl_program == 0) {
                        const char* vs_src =
                        "attribute vec4 aPosition;\n"
                        "attribute vec2 aTexCoord;\n"
                        "varying vec2 vTexCoord;\n"
                        "void main() {\n"
                        "  gl_Position = aPosition;\n"
                        "  vTexCoord = aTexCoord;\n"
                        "}\n";

        const char* fs_src =
        "precision mediump float;\n"
        "varying vec2 vTexCoord;\n"
        "uniform sampler2D uTexture;\n"
        "void main() {\n"
        "  gl_FragColor = texture2D(uTexture, vTexCoord);\n"
        "}\n";

        r->gl_program = create_program(vs_src, fs_src);
        r->attrib_pos = glGetAttribLocation(r->gl_program, "aPosition");
        r->attrib_tex = glGetAttribLocation(r->gl_program, "aTexCoord");

        glGenTextures(1, &r->gl_texture);
        glBindTexture(GL_TEXTURE_2D, r->gl_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    }
                }
            }
        } else {
            // Unchanged, release the temporary check reference
            if (target_window) ANativeWindow_release(target_window);
        }

        // --- HYBRID FIX: ALWAYS POLL THE ENGINE ---
        // Even if the 3D Mode deactivated the GPU Surface, we MUST continue
        // to poll the clock so the memory frames update for the CPU Blender!
        //
        // Poll and publish are ONE step now, done inside the engine under the engine lock
        // (see sub_engine_poll_and_publish()): a frame rendered from track T can only ever be
        // installed while T is still the open track, because open_track()/close_track() swap
        // the backend and clear the renderer under that same lock. Nothing is handed back
        // across an unlocked gap for this thread to install later.
        int content_changed = 0;
        if (r->engine) {
            content_changed = sub_engine_poll_and_publish((SUB_ENGINE*)r->engine, r);
        }

        pthread_mutex_lock(&r->lock);
        if (r->pending_redraw) {
            r->pending_redraw = 0;
            needs_redraw = 1;
        }
        if (content_changed) {
            needs_redraw = 1; // <--- 3. FORCE REDRAW ON NEW FRAME / CLEAR
        }

        // This iteration's poll+publish (above) has now run and r->current_frame reflects
        // its result, so anything that was true when loop_generation was sampled at the top
        // of this iteration -- e.g. a style change whose force_wake() bump was already
        // visible at that point -- is guaranteed to be reflected here. Stamp and broadcast so
        // sub_render_gl_wait_for_generation() callers (the 3D pull path) can unblock.
        r->applied_generation = loop_generation;
        pthread_cond_broadcast(&r->frame_cond);

        // Snapshot for the content-changed notification below, taken in this same lock hold
        // as the poll's result so the generation announced is one this thread actually saw.
        uint64_t gen_now = r->frame_generation;
        sub_render_change_cb change_cb = r->change_cb;
        void *change_ctx = r->change_ctx;
        int mode_3d = (r->ui_mode != 0);

        int w = r->surface_width;
        int h = r->surface_height;
        const SUB_FRAME *frame_to_draw = r->current_frame;
        GLint attrib_pos = r->attrib_pos;
        GLint attrib_tex = r->attrib_tex;

        // We're about to use frame_to_draw's pixel data (glTexImage2D) after
        // unlocking below, in the branch where we actually draw. Take our own
        // reference to it now, under the lock, so another thread calling
        // sub_render_gl_clear()/close_track()/destroy() concurrently can't drop
        // this exact frame's refcount to zero and free() its rgba buffer out
        // from under us mid-upload. Matched by sub_engine_release_frame() right
        // after eglSwapBuffers() below, or immediately if we end up not drawing.
        int will_draw = (surface != EGL_NO_SURFACE) && needs_redraw;
        if (frame_to_draw && will_draw) {
            sub_frame_ref((SUB_FRAME *)frame_to_draw);
        }
        pthread_mutex_unlock(&r->lock);

        // --- CONTENT-CHANGED PUSH (3D path, paused only) -- see sub_render_gl.h ---
        // No lock held here. The paused check takes the engine lock, so it is only evaluated
        // once we already know there is something new to announce.
        if (change_cb && mode_3d && gen_now != notified_generation && r->engine &&
            sub_engine_is_paused((SUB_ENGINE*)r->engine)) {
            notified_generation = gen_now;
            change_cb(change_ctx);
        }

        // If EGL is offline (e.g. we are in 3D Canvas mode), sleep and skip drawing.
        // The Java onFrameAvailable() callback will extract frames via RAM instead.
        if (surface == EGL_NO_SURFACE) {
            // will_draw was false in this branch, so no ref was taken -- nothing to release.
            if (r->engine) {
                sub_engine_wait_event((SUB_ENGINE*)r->engine, loop_generation);
            } else {
                usleep(16000); // Fallback if engine isn't attached yet
            }
            continue;
        }

        // --- 4. THE GL BYPASS ---
        // If the surface didn't change and the frame didn't change, skip the GPU!
        if (!needs_redraw) {
            // will_draw was false in this branch too -- no ref was taken.
            if (r->engine) {
                // Pass the generation counter so we don't drop wakes
                sub_engine_wait_event((SUB_ENGINE*)r->engine, loop_generation);
            } else {
                usleep(16000); // Fallback if engine isn't attached yet
            }
            continue;
        }

        // --- NATIVE 2D OPENGL RENDERING ---
        glViewport(0, 0, w, h);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        if (frame_to_draw && frame_to_draw->events
            && r->gl_program != 0
            && attrib_pos != -1 && attrib_tex != -1) {

            glUseProgram(r->gl_program);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        SUB_EVENT *ev = frame_to_draw->events;
        while (ev) {
            if (ev->kind == SUB_EVENT_BITMAP) {
                glBindTexture(GL_TEXTURE_2D, r->gl_texture);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                             ev->w, ev->h, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE,
                             ev->data.bitmap.rgba);

                float x1, y1, x2, y2;
                if (frame_to_draw->real_video_w > 0 && frame_to_draw->real_video_h > 0) {
                    // GFX (PGS/VobSub) frame: placement (incl. black-bar usage) is decided by
                    // gfx_map_to_canvas().
                    DBG serprintf("SUB_GFX: real_video=%dx%d canvas=%dx%d box=(%d,%d %dx%d) ev=(%d,%d %dx%d)\n",
                                  frame_to_draw->real_video_w, frame_to_draw->real_video_h,
                                  frame_to_draw->video_w, frame_to_draw->video_h,
                                  frame_to_draw->video_box_x, frame_to_draw->video_box_y,
                                  frame_to_draw->video_box_w, frame_to_draw->video_box_h,
                                  ev->x, ev->y, ev->w, ev->h);
                    float box_x, box_y, box_w, box_h, unused_sx, unused_sy;
                    gfx_map_to_canvas(frame_to_draw, ev, &box_x, &box_y, &box_w, &box_h,
                                      &unused_sx, &unused_sy);

                    x1 = (box_x / (float)frame_to_draw->video_w) * 2.0f - 1.0f;
                    y1 = 1.0f - (box_y / (float)frame_to_draw->video_h) * 2.0f;
                    x2 = ((box_x + box_w) / (float)frame_to_draw->video_w) * 2.0f - 1.0f;
                    y2 = 1.0f - ((box_y + box_h) / (float)frame_to_draw->video_h) * 2.0f;
                } else {
                    // SSA/SRT frame: already positioned in the frame's own video_w x
                    // video_h (canvas) space by libass -- unchanged from before this fix.
                    x1 = (ev->x / (float)frame_to_draw->video_w) * 2.0f - 1.0f;
                    y1 = 1.0f - (ev->y / (float)frame_to_draw->video_h) * 2.0f;
                    x2 = ((ev->x + ev->w) / (float)frame_to_draw->video_w) * 2.0f - 1.0f;
                    y2 = 1.0f - ((ev->y + ev->h) / (float)frame_to_draw->video_h) * 2.0f;
                }

                GLfloat vertices[] = {
                    x1, y2, 0.0f,  0.0f, 1.0f,
                    x2, y2, 0.0f,  1.0f, 1.0f,
                    x1, y1, 0.0f,  0.0f, 0.0f,
                    x2, y1, 0.0f,  1.0f, 0.0f
                };

                glVertexAttribPointer(attrib_pos, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), vertices);
                glVertexAttribPointer(attrib_tex, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), vertices + 3);

                glEnableVertexAttribArray(attrib_pos);
                glEnableVertexAttribArray(attrib_tex);

                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

                glDisableVertexAttribArray(attrib_pos);
                glDisableVertexAttribArray(attrib_tex);
            }
            ev = ev->next;
        }
        glDisable(GL_BLEND);
            }

            eglSwapBuffers(display, surface);

        // Done reading frame_to_draw's pixels -- release the pin taken above.
        if (frame_to_draw && will_draw) {
            sub_engine_release_frame((SUB_ENGINE*)r->engine, (SUB_FRAME*)frame_to_draw);
        }

    }

    if (surface != EGL_NO_SURFACE) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(display, surface);
    }
    if (current_window) {
        ANativeWindow_release(current_window);   // NEW
    }
    if (r->gl_program != 0) {
        glDeleteProgram(r->gl_program);
        glDeleteTextures(1, &r->gl_texture);
    }
    eglDestroyContext(display, context);
    eglTerminate(display);
    return NULL;
}

void sub_render_gl_set_change_callback(SUB_RENDERER *r, sub_render_change_cb cb, void *ctx) {
    if (!r) return;
    pthread_mutex_lock(&r->lock);
    r->change_cb  = cb;
    r->change_ctx = ctx;
    pthread_mutex_unlock(&r->lock);
}

void *sub_render_gl_get_change_ctx(SUB_RENDERER *r) {
    if (!r) return NULL;
    pthread_mutex_lock(&r->lock);
    void *ctx = r->change_ctx;
    pthread_mutex_unlock(&r->lock);
    return ctx;
}

SUB_RENDERER *sub_render_gl_create(void *engine) {
    SUB_RENDERER *r = calloc(1, sizeof(SUB_RENDERER));
    pthread_mutex_init(&r->lock, NULL);
    pthread_cond_init(&r->frame_cond, NULL);
    r->running    = 1;
    r->attrib_pos = -1;
    r->attrib_tex = -1;
    // Assign BEFORE pthread_create(): the render thread's loop reads
    // r->engine without the lock (it's only ever unlocked-read there, and
    // this is the only write, so this ordering is what actually makes that
    // safe). Passing it in up front means the thread's first iteration
    // already observes the real pointer instead of racing a later write.
    r->engine = engine;
    pthread_create(&r->thread, NULL, egl_render_thread, r);
    return r;
}

void sub_render_gl_destroy(SUB_RENDERER *r) {
    if (!r) return;
    pthread_mutex_lock(&r->lock);
    r->running = 0;
    // Grab and null any frame that poll_frame() may have installed between
    // the last sub_render_gl_clear() call and now. The render thread is still
    // running at this point (pthread_join not called yet), so we must hold
    // the lock while stealing the pointer. After join the thread is dead and
    // cannot install a new frame, so no further lock is needed for the free.
    SUB_FRAME *leftover = (SUB_FRAME *)r->current_frame;
    r->current_frame = NULL;
    pthread_mutex_unlock(&r->lock);
    // WAKE THE THREAD SO IT CAN EXIT!
    if (r->engine) {
        sub_engine_force_wake((SUB_ENGINE*)r->engine);
    }
    pthread_join(r->thread, NULL);
    // The engine is already destroyed by this point (sub_engine_destroy calls
    // close_track then destroy_renderer), so use the bare global free —
    // no backend vtable to route through.
    if (leftover) sub_engine_free_frame(leftover);
    pthread_mutex_destroy(&r->lock);
    pthread_cond_destroy(&r->frame_cond);
    free(r);
}

void sub_render_gl_attach_surface(SUB_RENDERER *r, ANativeWindow *window) {
    pthread_mutex_lock(&r->lock);
    if (r->window) {
        ANativeWindow_release(r->window);
    }
    r->window = window;
    if (r->window) {
        ANativeWindow_acquire(r->window);
    }
    pthread_mutex_unlock(&r->lock);
    if (r->engine) {
        sub_engine_force_wake((SUB_ENGINE*)r->engine);   // NEW
    }
}

void sub_render_gl_detach_surface(SUB_RENDERER *r) {
    sub_render_gl_attach_surface(r, NULL);
}

void sub_render_gl_resize(SUB_RENDERER *r, int width, int height) {
    pthread_mutex_lock(&r->lock);
    r->surface_width  = width;
    r->surface_height = height;
    // Force the native buffer to this size synchronously. Without this,
    // glViewport() below assumes a buffer size that SurfaceFlinger hasn't
    // necessarily allocated yet -- buffer resize isn't guaranteed to be
    // synchronous with this JNI call, so subs briefly draw stretched into
    // the old buffer until a later redraw happens to land after it catches
    // up.
    if (r->window) {
        ANativeWindow_setBuffersGeometry(r->window, width, height, 0);
    }
    pthread_mutex_unlock(&r->lock);
    sub_render_gl_invalidate_cache(r);
}

void sub_render_gl_set_ui_mode(SUB_RENDERER *r, int mode) {
    if (!r) return;
    pthread_mutex_lock(&r->lock);
    r->ui_mode = mode;
    pthread_mutex_unlock(&r->lock);
}

uint64_t sub_render_gl_get_frame_generation(SUB_RENDERER *r) {
    if (!r) return 0;
    pthread_mutex_lock(&r->lock);
    uint64_t gen = r->frame_generation;
    pthread_mutex_unlock(&r->lock);
    return gen;
}

// ---------------------------------------------------------------------------------------
// Publication. The ONLY two ways current_frame changes are sub_render_gl_publish() and
// sub_render_gl_clear_nowake() below, and both are called with the ENGINE lock held --
// the same lock hold that renders from / swaps / closes the backend. Lock order is always
// engine -> renderer; nothing that holds r->lock may ever take the engine lock.
// ---------------------------------------------------------------------------------------

// Takes ownership of `frame` (the reference render_at() handed back), which may be NULL.
// Returns 1 if visible content changed. Classifies the backend's answer explicitly:
//   NULL                          -> UNCHANGED: nothing new; current content stays valid.
//   same object as current        -> UNCHANGED (previous behaviour; see note below).
//   no events, nothing showing    -> UNCHANGED: "clear" of an already-clear screen is not news.
//   no events, something showing  -> CLEAR:     install the empty frame, bump generation.
//   anything else                 -> NEW FRAME: install it, bump generation.
// Frames displaced here are released AFTER r->lock is dropped (sub_frame_unref can free
// pixel buffers), but still under the engine lock -- brief, and nobody else can be waiting
// on r->lock for it: fill_bitmap() only holds r->lock long enough to pin+read.
//
// Note on the same-pointer case: if a backend's render_at() ever returns an already-owned
// cached frame WITH a fresh reference, that reference is dropped on the floor here exactly
// as it was before this change. That contract lives in the backends, not here.
int sub_render_gl_publish(SUB_RENDERER *r, SUB_FRAME *frame) {
    if (!r) return 0;
    SUB_FRAME *drop_old = NULL, *drop_new = NULL;
    int changed = 0;

    pthread_mutex_lock(&r->lock);
    SUB_FRAME *cur = (SUB_FRAME *)r->current_frame;
    if (!frame || frame == cur) {
        // UNCHANGED
    } else if (!frame->events && !(cur && cur->events)) {
        drop_new = frame;               // empty frame over an already-empty screen
    } else {
        drop_old = cur;                 // NEW FRAME, or CLEAR of visible content
        r->current_frame = frame;
        r->frame_generation++;
        changed = 1;
    }
    pthread_mutex_unlock(&r->lock);

    if (drop_old) sub_engine_release_frame((SUB_ENGINE*)r->engine, drop_old);
    if (drop_new) sub_engine_release_frame((SUB_ENGINE*)r->engine, drop_new);
    return changed;
}

// Drops whatever is showing. Caller MUST hold the engine lock, and is responsible for waking
// the render thread (broadcast_wake_locked) -- this deliberately does NOT call
// sub_engine_force_wake(), which would re-take the (non-recursive) engine lock.
// Bumps the generation iff something was actually showing, so a 3D-path puller sees
// "clear" as a real change, and a clear of an already-clear screen as nothing.
void sub_render_gl_clear_nowake(SUB_RENDERER *r) {
    if (!r) return;
    pthread_mutex_lock(&r->lock);
    SUB_FRAME *to_free = (SUB_FRAME*)r->current_frame;
    r->current_frame = NULL;
    if (to_free) r->frame_generation++;
    r->pending_redraw = 1;
    pthread_mutex_unlock(&r->lock);

    if (to_free) {
        sub_engine_release_frame((SUB_ENGINE*)r->engine, to_free);
    }
}

// Public variant for callers that do NOT hold the engine lock.
void sub_render_gl_clear(SUB_RENDERER *r) {
    if (!r) return;
    sub_render_gl_clear_nowake(r);
    if (r->engine) {
        sub_engine_force_wake((SUB_ENGINE*)r->engine);
    }
}

void sub_render_gl_invalidate_cache(SUB_RENDERER *r) {
    if (!r) return;
    pthread_mutex_lock(&r->lock);
    r->pending_redraw = 1;
    pthread_mutex_unlock(&r->lock);
    if (r->engine) {
        sub_engine_force_wake((SUB_ENGINE*)r->engine); // wake it if it's parked
    }
}

// Blocks the calling thread -- typically a JNI call arriving on the Java UI thread --
// until the render thread has completed a poll+store pass stamped with a
// loop_generation >= target_generation, or until timeout_ms elapses, whichever comes
// first. This is what closes the race in the 3D hybrid pull path: force_wake() only
// guarantees the render thread will eventually notice a style change, not that it
// already has by the time fill_bitmap() runs on another thread. Never blocks
// indefinitely -- a wedged or slow render thread just means the caller falls through
// and draws whatever's currently cached, rather than hanging the UI thread.
void sub_render_gl_wait_for_generation(SUB_RENDERER *r, uint64_t target_generation, int timeout_ms) {
    if (!r) return;
    pthread_mutex_lock(&r->lock);
    if (r->applied_generation >= target_generation) {
        pthread_mutex_unlock(&r->lock);
        return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long long nsec = ts.tv_nsec + ((long long)timeout_ms * 1000000LL);
    ts.tv_sec += nsec / 1000000000LL;
    ts.tv_nsec = nsec % 1000000000LL;
    while (r->applied_generation < target_generation) {
        if (pthread_cond_timedwait(&r->frame_cond, &r->lock, &ts) != 0) {
            break; // timed out (or spurious wake past deadline) -- bail, don't hang the caller
        }
    }
    pthread_mutex_unlock(&r->lock);
}

// --- HYBRID 3D BRIDGE FAST CPU BLENDER ---

// Blends one frame's bitmap events into `pixels`. Pure function of its arguments: it reads
// only the frame, which the caller has pinned (sub_frame_ref), so it needs no renderer lock.
static void blend_frame(const SUB_FRAME *frame, void *pixels, int dst_w, int dst_h, int dst_stride) {
        SUB_EVENT *ev = frame->events;

        while (ev) {
            if (ev->kind == SUB_EVENT_BITMAP && ev->data.bitmap.rgba) {
                const uint8_t *src_rgba = ev->data.bitmap.rgba;

                int src_w = ev->w;
                int src_h = ev->h;
                int src_x = ev->x;
                int src_y = ev->y;

                if (frame->real_video_w > 0 && frame->real_video_h > 0) {
                    // GFX (PGS/VobSub) frame: src_x/y/w/h are in the decoded video's own
                    // pixel space, not this destination bitmap's (dst_w/h here is the
                    // on-screen canvas size -- SubtitleEngine.draw3DSubtitlesInternal()
                    // sizes mSoftBitmap to viewWidth x viewHeight and reports that same
                    // size via nativeSurfaceChanged() before this runs). Map through the
                    // video's own on-screen box first, same transform as the GL path
                    // above. Nearest-neighbor sampling is a deliberate first pass -- fine
                    // for subtitle bitmaps, much cheaper than a proper filter for a CPU
                    // blend path.
                    // Same placement (incl. black-bar usage) as the GL path.
                    float fx, fy, fw, fh, scale_x, scale_y;
                    gfx_map_to_canvas(frame, ev, &fx, &fy, &fw, &fh, &scale_x, &scale_y);
                    int box_x0 = (int)fx;
                    int box_y0 = (int)fy;
                    int box_w  = (int)fw;
                    int box_h  = (int)fh;

                    for (int dy = 0; dy < box_h; dy++) {
                        int py = box_y0 + dy;
                        if (py < 0 || py >= dst_h) continue;
                        int sy = (int)(dy / scale_y);
                        if (sy < 0 || sy >= src_h) continue;

                        uint8_t *dst_row = (uint8_t *)pixels + (py * dst_stride);
                        const uint8_t *src_row = src_rgba + (sy * ev->data.bitmap.stride);

                        for (int dx = 0; dx < box_w; dx++) {
                            int px = box_x0 + dx;
                            if (px < 0 || px >= dst_w) continue;
                            int sx = (int)(dx / scale_x);
                            if (sx < 0 || sx >= src_w) continue;

                            uint8_t *dst_px = dst_row + (px * 4);
                            const uint8_t *src_px = src_row + (sx * 4);

                            uint8_t sa = src_px[3];
                            if (sa == 0) continue;

                            if (sa == 255 || dst_px[3] == 0) {
                                dst_px[0] = src_px[0]; dst_px[1] = src_px[1]; dst_px[2] = src_px[2]; dst_px[3] = sa;
                            } else {
                                uint8_t dr = dst_px[0], dg = dst_px[1], db = dst_px[2], da = dst_px[3];
                                int inv_sa = 255 - sa;
                                dst_px[0] = (src_px[0] * sa + dr * inv_sa) >> 8;
                                dst_px[1] = (src_px[1] * sa + dg * inv_sa) >> 8;
                                dst_px[2] = (src_px[2] * sa + db * inv_sa) >> 8;
                                dst_px[3] = sa + ((da * inv_sa) >> 8);
                            }
                        }
                    }
                } else {
                    // SSA/SRT frame: already positioned in canvas space, and dst_w/h here
                    // IS the canvas -- 1:1 pixel copy, unchanged from before this fix.
                    for (int y = 0; y < src_h; y++) {
                        int dy = src_y + y;
                        if (dy < 0 || dy >= dst_h) continue;

                        uint8_t *dst_row = (uint8_t *)pixels + (dy * dst_stride);
                        const uint8_t *src_row = src_rgba + (y * ev->data.bitmap.stride);

                        for (int x = 0; x < src_w; x++) {
                            int dx = src_x + x;
                            if (dx < 0 || dx >= dst_w) continue;

                            uint8_t *dst_px = dst_row + (dx * 4);
                            const uint8_t *src_px = src_row + (x * 4);

                            uint8_t sa = src_px[3];
                            if (sa == 0) continue;

                            if (sa == 255 || dst_px[3] == 0) {
                                dst_px[0] = src_px[0]; dst_px[1] = src_px[1]; dst_px[2] = src_px[2]; dst_px[3] = sa;
                            } else {
                                uint8_t dr = dst_px[0], dg = dst_px[1], db = dst_px[2], da = dst_px[3];
                                int inv_sa = 255 - sa;
                                dst_px[0] = (src_px[0] * sa + dr * inv_sa) >> 8;
                                dst_px[1] = (src_px[1] * sa + dg * inv_sa) >> 8;
                                dst_px[2] = (src_px[2] * sa + db * inv_sa) >> 8;
                                dst_px[3] = sa + ((da * inv_sa) >> 8);
                            }
                        }
                    }
                }
            }
            ev = ev->next;
        }
}

// Hands the 3D path exactly one of four answers about "what should be on screen", together
// with the generation that answer belongs to:
//
//   SUB_FILL_UNCHANGED  generation == last_generation (and !force): what the caller last
//                       posted is still right. `pixels` is untouched.
//   SUB_FILL_CLEAR      nothing should be showing (no frame, or an empty one). `pixels` is
//                       untouched -- the caller clears its own surface; this is NOT an error
//                       and NOT "unchanged".
//   SUB_FILL_FRAME      `pixels` was cleared and the frame blended into it.
//   SUB_FILL_ERROR      bad arguments; nothing was decided, so the caller must not record
//                       *out_generation as posted.
//
// *out_generation is written for every non-ERROR result and is the generation of the exact
// frame (or absence of one) this call decided on: the frame pointer, its generation and the
// pin on it are all taken in ONE critical section. The blend then runs outside the lock on
// the pinned frame -- frames are immutable once published, which is the same assumption the
// GL thread already relies on for glTexImage2D -- so a long blend can't stall publication
// (which now happens under the engine lock and would otherwise stall feed()). A frame swap
// or track close landing mid-blend just means the NEXT call sees a newer generation and
// answers CLEAR/FRAME; the caller has recorded the generation of what it actually posted.
//
// last_generation: pass UINT64_MAX (Java: -1) for "I have posted nothing valid" -- a generation
// never reaches it. force: skip the UNCHANGED shortcut (style-change redraws, where pixels
// differ even if the frame identity does not).
SUB_FILL_RESULT sub_render_gl_fill_bitmap(SUB_RENDERER *r, void *pixels, int dst_w, int dst_h, int dst_stride,
                                          uint64_t last_generation, int force, uint64_t *out_generation) {
    if (!r || !pixels || !out_generation || dst_w <= 0 || dst_h <= 0) return SUB_FILL_ERROR;

    pthread_mutex_lock(&r->lock);
    uint64_t gen = r->frame_generation;
    if (!force && gen == last_generation) {
        *out_generation = gen;
        pthread_mutex_unlock(&r->lock);
        return SUB_FILL_UNCHANGED;
    }
    SUB_FRAME *frame = (SUB_FRAME *)r->current_frame;
    int has_subs = (frame && frame->events) ? 1 : 0;
    if (has_subs) sub_frame_ref(frame);
    *out_generation = gen;
    pthread_mutex_unlock(&r->lock);

    if (!has_subs) return SUB_FILL_CLEAR;

    memset(pixels, 0, (size_t)dst_stride * (size_t)dst_h);
    blend_frame(frame, pixels, dst_w, dst_h, dst_stride);
    sub_engine_release_frame((SUB_ENGINE*)r->engine, frame);
    return SUB_FILL_FRAME;
}
