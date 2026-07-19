#include "sub_render_gl.h"
#include "sub_engine.h"
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <android/log.h>
#include <time.h>

#define LOG_TAG "SubRenderGL"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

struct SUB_RENDERER {
    ANativeWindow  *window;
    pthread_t       thread;
    pthread_mutex_t lock;
    int             running;
    int             surface_width;
    int             surface_height;
    int             ui_mode; // 0 = 2D, 1 = SBS, 2 = TB
    const SUB_FRAME *current_frame;
    GLuint           gl_program;
    GLuint           gl_texture;
    GLint            attrib_pos;
    GLint            attrib_tex;
    void            *engine;
};

void sub_render_gl_set_engine(SUB_RENDERER *r, void *engine) {
    if (r) {
        pthread_mutex_lock(&r->lock);
        r->engine = engine;
        pthread_mutex_unlock(&r->lock);
    }
}

static GLuint compile_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint status = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        char buf[512];
        glGetShaderInfoLog(shader, sizeof(buf), NULL, buf);
        LOGE("Shader compile error: %s", buf);
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
        LOGE("Program link error: %s", buf);
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
        struct timespec t_loop_start;
        clock_gettime(CLOCK_MONOTONIC, &t_loop_start);

        pthread_mutex_lock(&r->lock);
        if (!r->running) {
            pthread_mutex_unlock(&r->lock);
            break;
        }

        ANativeWindow *target_window = r->window;
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

            current_window = target_window;

            if (current_window) {
                surface = eglCreateWindowSurface(display, config, current_window, NULL);
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
                        LOGD("SUB_RENDER_GL: adopted real EGL surface size %d x %d on window attach", real_w, real_h);
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
        }

        // --- HYBRID FIX: ALWAYS POLL THE ENGINE ---
        // Even if the 3D Mode deactivated the GPU Surface, we MUST continue
        // to poll the clock so the memory frames update for the CPU Blender!
        SUB_FRAME *new_frame = NULL;
        if (r->engine) {
            new_frame = sub_engine_poll_frame((SUB_ENGINE*)r->engine);
        }

        pthread_mutex_lock(&r->lock);
        if (new_frame != NULL) {
            if (r->current_frame) {
                sub_engine_release_frame((SUB_ENGINE*)r->engine, (SUB_FRAME*)r->current_frame);
            }
            r->current_frame = new_frame;
        }

        int w = r->surface_width;
        int h = r->surface_height;
        const SUB_FRAME *frame_to_draw = r->current_frame;
        GLint attrib_pos = r->attrib_pos;
        GLint attrib_tex = r->attrib_tex;
        pthread_mutex_unlock(&r->lock);

        // If EGL is offline (e.g. we are in 3D Canvas mode), sleep and skip drawing.
        // The Java onFrameAvailable() callback will extract frames via RAM instead.
        if (surface == EGL_NO_SURFACE) {
            usleep(16000);
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

            struct timespec t_render_end;
            clock_gettime(CLOCK_MONOTONIC, &t_render_end);

            double render_ms = (t_render_end.tv_sec  - t_loop_start.tv_sec)  * 1000.0 +
            (t_render_end.tv_nsec - t_loop_start.tv_nsec) / 1000000.0;

            double target_ms = 16.666;
            if (render_ms < target_ms) {
                long sleep_us = (long)((target_ms - render_ms) * 1000.0);
                usleep(sleep_us);
            }
    }

    if (surface != EGL_NO_SURFACE) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(display, surface);
    }
    if (r->gl_program != 0) {
        glDeleteProgram(r->gl_program);
        glDeleteTextures(1, &r->gl_texture);
    }
    eglDestroyContext(display, context);
    eglTerminate(display);
    return NULL;
}

SUB_RENDERER *sub_render_gl_create(void) {
    SUB_RENDERER *r = calloc(1, sizeof(SUB_RENDERER));
    pthread_mutex_init(&r->lock, NULL);
    r->running    = 1;
    r->attrib_pos = -1;
    r->attrib_tex = -1;
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
    pthread_join(r->thread, NULL);
    // The engine is already destroyed by this point (sub_engine_destroy calls
    // close_track then destroy_renderer), so use the bare global free —
    // no backend vtable to route through.
    if (leftover) sub_engine_free_frame(leftover);
    pthread_mutex_destroy(&r->lock);
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
}

void sub_render_gl_detach_surface(SUB_RENDERER *r) {
    sub_render_gl_attach_surface(r, NULL);
}

void sub_render_gl_resize(SUB_RENDERER *r, int width, int height) {
    pthread_mutex_lock(&r->lock);
    r->surface_width  = width;
    r->surface_height = height;
    pthread_mutex_unlock(&r->lock);
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
}

void sub_render_gl_invalidate_cache(SUB_RENDERER *r) {}

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
