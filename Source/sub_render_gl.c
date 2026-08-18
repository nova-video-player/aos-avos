#include "sub_render_gl.h"
#include "sub_engine.h"
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
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
    pthread_cond_t   frame_cond;         // broadcast whenever applied_generation advances
    GLuint           gl_program;
    GLuint           gl_texture;
    GLint            attrib_pos;
    GLint            attrib_tex;
    void            *engine;
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

            current_window = target_window;

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
        SUB_FRAME *new_frame = NULL;
        if (r->engine) {
            new_frame = sub_engine_poll_frame((SUB_ENGINE*)r->engine);
        }

        pthread_mutex_lock(&r->lock);
        if (r->pending_redraw) {
            r->pending_redraw = 0;
            needs_redraw = 1;
        }
        if (new_frame != NULL && new_frame != r->current_frame) {
            if (r->current_frame) {
                sub_engine_release_frame((SUB_ENGINE*)r->engine, (SUB_FRAME*)r->current_frame);
            }
            r->current_frame = new_frame;
            needs_redraw = 1; // <--- 3. FORCE REDRAW ON NEW FRAME
        }

        // This iteration's poll_frame() (above) has now run and r->current_frame reflects
        // its result, so anything that was true when loop_generation was sampled at the top
        // of this iteration -- e.g. a style change whose force_wake() bump was already
        // visible at that point -- is guaranteed to be reflected here. Stamp and broadcast so
        // sub_render_gl_wait_for_generation() callers (the 3D pull path) can unblock.
        r->applied_generation = loop_generation;
        pthread_cond_broadcast(&r->frame_cond);

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

                float x1 = (ev->x / (float)frame_to_draw->video_w) * 2.0f - 1.0f;
                float y1 = 1.0f - (ev->y / (float)frame_to_draw->video_h) * 2.0f;
                float x2 = ((ev->x + ev->w) / (float)frame_to_draw->video_w) * 2.0f - 1.0f;
                float y2 = 1.0f - ((ev->y + ev->h) / (float)frame_to_draw->video_h) * 2.0f;

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
    pthread_mutex_unlock(&r->lock);
    sub_render_gl_invalidate_cache(r);
}

void sub_render_gl_set_ui_mode(SUB_RENDERER *r, int mode) {
    if (!r) return;
    pthread_mutex_lock(&r->lock);
    r->ui_mode = mode;
    pthread_mutex_unlock(&r->lock);
}

void sub_render_gl_clear(SUB_RENDERER *r) {
    if (!r) return;
    pthread_mutex_lock(&r->lock);
    SUB_FRAME *to_free = (SUB_FRAME*)r->current_frame;
    r->current_frame = NULL;
    pthread_mutex_unlock(&r->lock);

    if (to_free) {
        sub_engine_release_frame((SUB_ENGINE*)r->engine, to_free);
    }
    sub_render_gl_invalidate_cache(r);
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
int sub_render_gl_fill_bitmap(SUB_RENDERER *r, void* pixels, int dst_w, int dst_h, int dst_stride) {
    if (!r) return 0;
    int has_subs = 0;

    pthread_mutex_lock(&r->lock);
    const SUB_FRAME *frame = r->current_frame;

    if (frame && frame->events) {
        has_subs = 1;
        SUB_EVENT *ev = frame->events;

        while (ev) {
            if (ev->kind == SUB_EVENT_BITMAP && ev->data.bitmap.rgba) {
                const uint8_t *src_rgba = ev->data.bitmap.rgba;

                int src_w = ev->w;
                int src_h = ev->h;
                int src_x = ev->x;
                int src_y = ev->y;

                // 1:1 Pixel copy. No scaling, no rounding errors, no clipping!
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
            ev = ev->next;
        }
    }
    pthread_mutex_unlock(&r->lock);
    return has_subs;
}
