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

extern SUB_ENGINE *g_sub_engine;

struct SUB_RENDERER {
    ANativeWindow  *window;
    pthread_t       thread;
    pthread_mutex_t lock;
    int             running;
    int             surface_width;
    int             surface_height;
    const SUB_FRAME *current_frame;
    GLuint           gl_program;
    GLuint           gl_texture;
};

static GLuint compile_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    return shader;
}

static GLuint create_program(const char *vertex_src, const char *fragment_src) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment_src);
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
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

        if (r->window != current_window) {
            if (surface != EGL_NO_SURFACE) {
                eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                eglDestroySurface(display, surface);
                surface = EGL_NO_SURFACE;
            }
            current_window = r->window;
            if (current_window) {
                surface = eglCreateWindowSurface(display, config, current_window, NULL);
                eglMakeCurrent(display, surface, surface, context);
                eglSwapInterval(display, 1);

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
glGenTextures(1, &r->gl_texture);
glBindTexture(GL_TEXTURE_2D, r->gl_texture);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                }
            }
        }
        pthread_mutex_unlock(&r->lock);

        // --- NEW: Safe Memory Polling ---
        // Poll the engine outside the lock so we don't stall the Android UI thread
        SUB_FRAME *new_frame = NULL;
        if (g_sub_engine) {
            new_frame = sub_engine_poll_frame(g_sub_engine);
        }

        pthread_mutex_lock(&r->lock);
        if (new_frame != NULL) {
            if (r->current_frame) {
                sub_engine_free_frame((SUB_FRAME*)r->current_frame); // Free safely!
            }
            r->current_frame = new_frame;
        }

        int w = r->surface_width;
        int h = r->surface_height;
        const SUB_FRAME *frame_to_draw = r->current_frame;
        pthread_mutex_unlock(&r->lock);

        if (surface != EGL_NO_SURFACE) {
            glViewport(0, 0, w, h);
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            if (frame_to_draw && frame_to_draw->events) {
                glUseProgram(r->gl_program);
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

                SUB_EVENT *ev = frame_to_draw->events;
                while (ev) {
                    if (ev->kind == SUB_EVENT_BITMAP) {
                        glBindTexture(GL_TEXTURE_2D, r->gl_texture);
                        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ev->w, ev->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, ev->data.bitmap.rgba);

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

                        GLint posLoc = glGetAttribLocation(r->gl_program, "aPosition");
                        GLint texLoc = glGetAttribLocation(r->gl_program, "aTexCoord");

                        glVertexAttribPointer(posLoc, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), vertices);
                        glVertexAttribPointer(texLoc, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), vertices + 3);

                        glEnableVertexAttribArray(posLoc);
                        glEnableVertexAttribArray(texLoc);

                        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

                        glDisableVertexAttribArray(posLoc);
                        glDisableVertexAttribArray(texLoc);
                    }
                    ev = ev->next;
                }
                glDisable(GL_BLEND);
            }

            eglSwapBuffers(display, surface);
        }

        struct timespec t_render_end;
        clock_gettime(CLOCK_MONOTONIC, &t_render_end);

        double render_ms = (t_render_end.tv_sec - t_loop_start.tv_sec) * 1000.0 +
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
    r->running = 1;
    pthread_create(&r->thread, NULL, egl_render_thread, r);
    return r;
}

void sub_render_gl_destroy(SUB_RENDERER *r) {
    if (!r) return;
    pthread_mutex_lock(&r->lock);
    r->running = 0;
    pthread_mutex_unlock(&r->lock);
    pthread_join(r->thread, NULL);
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
    r->surface_width = width;
    r->surface_height = height;
    pthread_mutex_unlock(&r->lock);
}

void sub_render_gl_clear(SUB_RENDERER *r) {
    if (!r) return;
    pthread_mutex_lock(&r->lock);
    SUB_FRAME *to_free = (SUB_FRAME*)r->current_frame;
    r->current_frame = NULL;
    pthread_mutex_unlock(&r->lock);

    if (to_free) {
        sub_engine_free_frame(to_free);
    }
}

void sub_render_gl_invalidate_cache(SUB_RENDERER *r) {}
