# Copyright 2026 Courville Software
# SPDX-License-Identifier: Apache-2.0

"""Replay the production subtitle draw loop against a stateful GL/EGL recorder.

The original client-memory vertex path is replayed with the retained #1981
diagnostics. Checks cover cue geometry, pending GL errors, bitmap rejection and
context lifetime, EGL failures/recovery, delayed buffer resizing and per-event
texture ownership through frame submission and replacement-window binding. They do not emulate
PowerVR or establish that #1981 is resolved.
"""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from native_replay import ROOT, extract


PREAMBLE = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include "host_stubs.h"
#include "sub_render_gl.h"
#include "sub_engine.h"
#include "gfx_layout.h"
#include "debug.h"

int Debug[DBG_MAX_ENTRIES];
static int egl_logs;
int serprintf(const char *fmt, ...) {
    if (strstr(fmt, "NovaSubGL: EGL ")) egl_logs++;
    assert(!strstr(fmt, "draw begin") && !strstr(fmt, "draw end") && !strstr(fmt, "swap begin"));
    return 0;
}
#define DBG if (Debug[DBG_SUB])
static int current, deleted_programs, deleted_textures, draws, vertices_set, swaps, refs;
static int replace_surface, detach_surface;
static const char *scenario;
static int polls, viewports, queries, binds, surface_creates;
static int failure_seen, suspended, offline_waits, failed_swaps;
static EGLint pending_egl_error;
static GLuint bound_texture, next_texture = 1;
static int generated_textures, context_reclaimed;
static int live[128], uploaded[128], drawn[128], parameters[128], created_at_swap[128];
static int is(const char *name) { return strcmp(scenario, name) == 0; }
static int paused_resize(void) {
    return is("paused_grow") || is("paused_shrink") || is("unstable_resize");
}
static int buffer_size(EGLint attribute) {
    if (is("unstable_resize")) return 8 + 4 * swaps;
    if (is("paused_shrink") && swaps > 0) return 4;
    if ((is("resize") && swaps >= 2) || (is("paused_grow") && swaps > 0))
        return attribute == EGL_WIDTH ? 16 : 12;
    return 8;
}
static int recovery(void) { return strstr(scenario, "_recovery") != NULL; }
static int fatal_replacement(void) { return is("swap_lost") || is("query_lost") || is("swap_other_error"); }
static int fail_egl(EGLint error) {
    pending_egl_error = error;
    failure_seen = suspended = 1;
    return EGL_FALSE;
}
static void *recorded_calloc(size_t count, size_t size) {
    return is("list_alloc_failure") ? NULL : calloc(count, size);
}
static EGLDisplay get_display(EGLDisplay d) { return is("display_failure") ? EGL_NO_DISPLAY : (EGLDisplay)1; }
static int initialize(EGLDisplay d, EGLint *a, EGLint *b) { return !is("init_failure"); }
static EGLContext create_context(EGLDisplay d, EGLConfig c, EGLContext shared, const EGLint *attrs) {
    return is("context_failure") ? EGL_NO_CONTEXT : (EGLContext)1;
}
static EGLint egl_error(void) {
    EGLint error = pending_egl_error ? pending_egl_error : EGL_SUCCESS;
    pending_egl_error = 0;
    return error;
}
static void viewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    assert(current);
    assert(w == buffer_size(EGL_WIDTH));
    assert(h == buffer_size(EGL_HEIGHT));
    viewports++;
}
static int forbidden_geometry(ANativeWindow *window, int w, int h, int format) { abort(); }

static int fail_texture, stale_error, small_max_texture, textures;
static GLenum pending_error;
static GLsizei texture_w;
static GLfloat data[20];
static const GLfloat expected[2][20] = {
    {-.5f, 0, 0, 0, 1, .5f, 0, 0, 1, 1,
     -.5f, .5f, 0, 0, 0, .5f, .5f, 0, 1, 0},
    {-1, .5f, 0, 0, 1, -.5f, .5f, 0, 1, 1,
     -1, 1, 0, 0, 0, -.5f, 1, 0, 1, 0}
};
static void finish_frame(void);
static int finish_offline(useconds_t delay);
static EGLSurface create_surface(EGLDisplay d, EGLConfig c, void *win, const EGLint *a) {
    surface_creates++;
    if (is("surface_failure") || (is("create_recovery") && !failure_seen)) {
        fail_egl(EGL_BAD_NATIVE_WINDOW);
        return EGL_NO_SURFACE;
    }
    return (EGLSurface)win;
}
static int choose_config(EGLDisplay d, const EGLint *a, EGLConfig *c, EGLint size, EGLint *n) {
    *c = (EGLConfig)1; *n = is("no_configs") ? 0 : 1; return !is("config_failure");
}
static int make_current(EGLDisplay d, EGLSurface draw, EGLSurface read, EGLContext ctx) {
    if (ctx != EGL_NO_CONTEXT) {
        binds++;
        if (is("bind_failure") || (is("rebind_failure") && binds == 2)
            || (is("bind_recovery") && !failure_seen)) {
            current = 0; return fail_egl(EGL_BAD_CURRENT_SURFACE);
        }
        suspended = 0;
    }
    current = ctx != EGL_NO_CONTEXT; return 1;
}
static int query_surface(EGLDisplay d, EGLSurface s, EGLint a, EGLint *value) {
    queries++;
    if (swaps == 1 && a == EGL_HEIGHT &&
        (is("query_failure") || ((is("query_recovery") || is("query_lost")) && !failure_seen)))
        return fail_egl(is("query_lost") ? EGL_CONTEXT_LOST : EGL_BAD_SURFACE);
    *value = buffer_size(a);
    if (is("empty_buffer")) *value = 0;
    return 1;
}
static int swap_buffers(EGLDisplay d, EGLSurface s) {
    assert(current && !suspended);
    finish_frame();
    if (is("swap_failure") || (!failure_seen && (is("swap_recovery") || is("detach_recovery") ||
        is("swap_lost") || is("swap_other_error")))) {
        failed_swaps++;
        return fail_egl(is("swap_lost") ? EGL_CONTEXT_LOST :
                        is("swap_other_error") ? EGL_BAD_ALLOC : EGL_BAD_SURFACE);
    }
    return EGL_TRUE;
}
static int destroy_context(EGLDisplay d, EGLContext c) {
    assert(!current);
    int clean = is("normal") || is("texture_failure") || is("stale_error") ||
                is("oversized_bitmap") || is("replace_surface") || is("resize") ||
                is("attribute_failure") || is("clear_only") || is("empty_buffer") ||
                is("texture_alloc_failure") || is("texture_name_error") ||
                is("list_alloc_failure") || is("equal_sizes") || is("many_events") || recovery() || paused_resize();
    assert(deleted_programs == clean);
    for (GLuint i = 1; i < next_texture; i++) {
        if (live[i]) {
            assert(is("swap_failure") || fatal_replacement());  /* No GL deletion after failed submission. */
            live[i] = 0;
            context_reclaimed++;
        }
    }
    return 1;
}
static GLint attrib(GLuint program, const char *name) {
    return strcmp(name, "aPosition") == 0 ? 0 : 1;
}
static GLenum recorded_error(void) {
    GLenum error = pending_error;
    pending_error = GL_NO_ERROR;
    return error;
}
static void get_integer(GLenum pname, GLint *value) {
    assert(current && pname == GL_MAX_TEXTURE_SIZE);
    *value = small_max_texture ? 3 : 4096;
    if (stale_error) pending_error = 0x0502;  /* Left behind by setup, before any draw. */
}
static void gen_textures(GLsizei n, GLuint *ids) {
    assert(current && !suspended && n == 1);
    if (is("texture_alloc_failure")) {
        *ids = 0;
        pending_error = 0x0505;
        return;
    }
    assert(next_texture < 128);
    *ids = next_texture++;
    live[*ids] = 1;
    created_at_swap[*ids] = swaps;
    generated_textures++;
    if (is("texture_name_error")) pending_error = 0x0505;
}
static void bind_texture(GLenum target, GLuint id) {
    assert(current && target == GL_TEXTURE_2D);
    assert(id == 0 || (id < next_texture && live[id]));
    bound_texture = id;
}
static void texture_parameter(GLenum target, GLenum name, GLint value) {
    assert(current && target == GL_TEXTURE_2D && bound_texture && live[bound_texture]);
    int bit = 0;
    switch (name) {
        case GL_TEXTURE_MIN_FILTER: assert(value == GL_LINEAR); bit = 1; break;
        case GL_TEXTURE_MAG_FILTER: assert(value == GL_LINEAR); bit = 2; break;
        case GL_TEXTURE_WRAP_S: assert(value == GL_CLAMP_TO_EDGE); bit = 4; break;
        case GL_TEXTURE_WRAP_T: assert(value == GL_CLAMP_TO_EDGE); bit = 8; break;
        default: abort();
    }
    parameters[bound_texture] |= bit;
}
static void tex_image(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h,
                      GLint border, GLenum format, GLenum type, const void *pixels) {
    assert(current && pending_error == GL_NO_ERROR);  /* Older errors were drained first. */
    assert(pixels && w > 0 && h > 0 && w <= (small_max_texture ? 3 : 4096));
    assert(bound_texture && live[bound_texture] && parameters[bound_texture] == 15);
    assert(!uploaded[bound_texture] && !drawn[bound_texture]);
    uploaded[bound_texture] = 1;  /* Exactly one image definition per object lifetime. */
    texture_w = w; textures++;
    if (fail_texture) pending_error = 0x0501;
}
static void vertex_pointer(GLuint index, GLint size, GLenum type, int normalized,
                           GLsizei stride, const void *p) {
    assert(current);
    assert(type == GL_FLOAT && normalized == GL_FALSE && stride == 5 * sizeof(GLfloat));
    assert(index < 2 && size == (index == 0 ? 3 : 2));
    static const GLfloat *base;
    assert(p);
    if (index == 0) { base = p; memcpy(data, p, sizeof(data)); vertices_set++; }
    else assert((const GLfloat *)p == base + 3);
    if (is("attribute_failure")) pending_error = 0x0502;
}
static void draw(GLenum mode, GLint first, GLsizei count) {
    assert(current && !suspended && vertices_set > draws);
    assert(mode == GL_TRIANGLE_STRIP && first == 0 && count == 4);
    assert(memcmp(data, expected[texture_w == 4 ? 0 : 1], sizeof(data)) == 0);
    assert(bound_texture && live[bound_texture] && uploaded[bound_texture]);
    assert(!drawn[bound_texture]);  /* Each event must use a distinct texture object. */
    drawn[bound_texture] = 1;
    draws++;
}
static void delete_program(GLuint p) { assert(current && !suspended); deleted_programs++; }
static void delete_texture(GLsizei n, const GLuint *p) {
    assert(current && !suspended && !is("swap_failure"));
    for (int i = 0; i < n; i++) {
        GLuint id = p[i];
        assert(id && id < next_texture && live[id]);
        assert(swaps > created_at_swap[id]);  /* No deletion before submission. */
        live[id] = 0;
        deleted_textures++;
    }
}

#define eglGetDisplay get_display
#define eglInitialize initialize
#define eglCreateContext create_context
#define eglGetError egl_error
#define glViewport viewport
#define ANativeWindow_setBuffersGeometry forbidden_geometry
#define eglChooseConfig choose_config
#define eglCreateWindowSurface create_surface
#define eglMakeCurrent make_current
#define eglQuerySurface query_surface
#define eglSwapBuffers swap_buffers
#define eglDestroyContext destroy_context
#define glGetAttribLocation attrib
#define glGetError recorded_error
#define glGetIntegerv get_integer
#define glGenTextures gen_textures
#define glBindTexture bind_texture
#define glTexParameteri texture_parameter
#define glTexImage2D tex_image
#define calloc recorded_calloc
#define glVertexAttribPointer vertex_pointer
#define glDrawArrays draw
#define glDeleteProgram delete_program
#define glDeleteTextures delete_texture
#define usleep finish_offline

uint64_t sub_engine_get_generation(SUB_ENGINE *e) { return 0; }
int sub_engine_poll_and_publish(SUB_ENGINE *e, SUB_RENDERER *r) { assert(++polls < 20); return 0; }
int sub_engine_is_paused(SUB_ENGINE *e) { return 0; }
void sub_engine_wait_event(SUB_ENGINE *e, uint64_t g) { finish_offline(0); }
void sub_frame_ref(SUB_FRAME *frame) { refs++; }
void sub_engine_release_frame(SUB_ENGINE *e, SUB_FRAME *frame) { refs--; }
'''

MAIN = r'''
static SUB_RENDERER *renderer;
void sub_render_gl_invalidate_cache(SUB_RENDERER *r) { r->pending_redraw = 1; }
static void finish_frame(void) {
    for (GLuint i = 1; i < next_texture; i++) {
        if (created_at_swap[i] == swaps)
            assert(live[i]);  /* Every allocated object survives through the swap. */
    }
    if (++swaps == (is("resize") ? 3 : 2) && !paused_resize()) renderer->running = 0;
    if (is("resize") && swaps == 1) sub_render_gl_resize(renderer, 16, 12);
    if (stale_error && swaps == 1) pending_error = 0x0502;  /* Left pending between frames. */
    if (!paused_resize()) renderer->pending_redraw = 1;
    if (replace_surface || is("rebind_failure")) renderer->window = (ANativeWindow *)2;
    if (detach_surface) renderer->window = NULL;
}
static int finish_offline(useconds_t delay) {
    if (paused_resize()) {
        assert(swaps == 2 && viewports == 2 && refs == 0);
        renderer->running = 0;
        return 0;
    }
    if (recovery() && failure_seen) {
        // Two waits on the failing window must not retry EGL or allocate again.
        assert(surface_creates == 1);
        if (++offline_waits == 3) renderer->window = (ANativeWindow *)2;
        else if (is("detach_recovery") && offline_waits == 2) renderer->window = NULL;
        return 0;
    }
    if (fatal_replacement()) {
        assert(surface_creates == 1 && binds == 1);
        if (++offline_waits == 1) { renderer->window = (ANativeWindow *)2; return 0; }
        renderer->running = 0;
        return 0;
    }
    assert(detach_surface || is("display_failure") || is("init_failure") ||
           is("config_failure") || is("no_configs") || is("context_failure") ||
           is("surface_failure") || is("bind_failure") || is("rebind_failure") ||
           is("query_failure") || is("swap_failure") || is("empty_buffer") || is("list_alloc_failure"));
    renderer->running = 0;
    return 0;
}
int main(int argc, char **argv) {
    assert(argc == 3);
    scenario = argv[1];
    Debug[DBG_SUB] = atoi(argv[2]);
    replace_surface = strcmp(argv[1], "replace_surface") == 0;
    detach_surface = strcmp(argv[1], "detach_surface") == 0;
    fail_texture = strcmp(argv[1], "texture_failure") == 0;
    stale_error = strcmp(argv[1], "stale_error") == 0;
    small_max_texture = strcmp(argv[1], "oversized_bitmap") == 0;
    static uint8_t pixels[4 * 2 * 4];
    SUB_EVENT empty = {.kind = SUB_EVENT_BITMAP, .x = 0, .y = 0, .w = 0, .h = 2};
    SUB_EVENT second = {.kind = SUB_EVENT_BITMAP, .x = 0, .y = 0, .w = 2, .h = 2, .next = &empty};
    SUB_EVENT first = {.kind = SUB_EVENT_BITMAP, .x = 2, .y = 2, .w = 4, .h = 2, .next = &second};
    first.data.bitmap.rgba = pixels;
    second.data.bitmap.rgba = pixels;
    empty.data.bitmap.rgba = pixels;  /* Zero width: rejected before any GL call. */
    if (is("equal_sizes")) { first.x = 0; first.y = 0; first.w = 2; }
    SUB_EVENT extra[8];
    if (is("many_events")) {
        for (int i = 0; i < 8; i++) {
            extra[i] = second;
            extra[i].next = i == 7 ? &empty : &extra[i + 1];
        }
        second.next = &extra[0];
    }
    SUB_FRAME frame = {.events = is("clear_only") ? NULL : &first, .video_w = 8, .video_h = 8};
    SUB_RENDERER r = {0};
    renderer = &r;
    pthread_mutex_init(&r.lock, NULL);
    pthread_cond_init(&r.frame_cond, NULL);
    r.running = 1; r.current_frame = &frame; r.window = (ANativeWindow *)1;
    r.engine = (void *)1;
    sub_render_gl_resize(&r, 10, 6);  /* Requested size differs from the EGL buffer. */
    egl_render_thread(&r);
    assert(polls > 0);  /* EGL failures must not kill CPU frame processing. */
    if (paused_resize()) assert(polls == 3);  /* One retry, then sleep without new content. */
    /* Two cues per frame, two frames unless the surface goes away after one. */
    int frames = is("resize") ? 3 : 2;
    if (detach_surface || is("rebind_failure") || is("query_failure") || is("swap_failure") || fatal_replacement()) frames = 1;
    if (is("display_failure") || is("init_failure") || is("config_failure") ||
        is("no_configs") || is("context_failure") || is("surface_failure") || is("bind_failure") ||
        is("empty_buffer")) frames = 0;
    if (is("list_alloc_failure")) frames = 1;
    assert(viewports == frames);
    int drawable = is("clear_only") ? 0 : small_max_texture ? 1 : 2;  /* The 4-wide cue exceeds a max of 3. */
    if (is("many_events")) drawable = 10;
    if (is("texture_alloc_failure") || is("texture_name_error") || is("list_alloc_failure")) drawable = 0;
    assert(textures == frames * drawable);
    assert(vertices_set == (fail_texture ? 0 : frames * drawable));
    assert(draws == (fail_texture || is("attribute_failure") ? 0 : frames * drawable));
    assert(refs == 0 && pending_error == GL_NO_ERROR);
    assert(generated_textures == deleted_textures + context_reclaimed);
    if (recovery()) {
        assert(offline_waits == 3 && surface_creates == 2);
        assert(binds == (is("create_recovery") ? 1 : 2));
        assert(context_reclaimed == 0 && generated_textures == deleted_textures);
        assert(failure_seen && egl_logs == 1 && pending_egl_error == 0);
    }
    if (fatal_replacement()) assert(offline_waits == 2 && egl_logs == 1);
    if (is("display_failure") || is("init_failure") || is("config_failure") ||
        is("no_configs") || is("context_failure") || is("swap_failure"))
        assert(egl_logs == 1);  /* Critical failures remain visible with DBG_SUB=0. */
    if (is("normal")) assert(egl_logs == 0);  /* Success logs stay quiet even at DBG_SUB=3. */
    for (GLuint i = 1; i < next_texture; i++) assert(!live[i]);
    pthread_mutex_destroy(&r.lock);
    pthread_cond_destroy(&r.frame_cond);
    return 0;
}
'''


class SubtitleGLDiagnosticsTest(unittest.TestCase):
    def test_draws_failures_and_surface_lifetime(self):
        source = (ROOT / 'Source/sub_render_gl.c').read_text()
        code = PREAMBLE + extract(source, 'struct SUB_RENDERER {') + ';\n'
        for marker in ('static GLuint compile_shader(', 'static GLuint create_program(',
                       'static GLenum subgl_take_errors(', 'static int subgl_check_egl(',
                       'static void subgl_release_textures(',
                       'void sub_render_gl_resize(',
                       'static void* egl_render_thread('):
            code += extract(source, marker) + '\n'
        code += MAIN
        with tempfile.TemporaryDirectory(prefix='subtitle-gl-diagnostics-') as directory:
            path = Path(directory) / 'replay.c'
            binary = path.with_suffix('')
            path.write_text(code)
            subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                            '-Werror', '-Wno-unused-parameter', '-fsanitize=undefined', '-pthread',
                            '-Wno-unused-function',
                            '-I' + str(ROOT / 'Include/host_compat'), '-I' + str(ROOT / 'Include'),
                            str(path), '-o', str(binary)], check=True)
            cases = ['normal', 'texture_failure', 'stale_error', 'oversized_bitmap',
                     'replace_surface', 'detach_surface', 'display_failure', 'init_failure',
                     'config_failure', 'no_configs', 'context_failure', 'surface_failure',
                     'bind_failure', 'rebind_failure', 'query_failure', 'swap_failure',
                     'resize', 'attribute_failure', 'clear_only', 'empty_buffer',
                     'texture_alloc_failure', 'texture_name_error', 'list_alloc_failure',
                     'equal_sizes', 'many_events', 'swap_recovery', 'query_recovery',
                     'create_recovery', 'bind_recovery', 'detach_recovery',
                     'swap_lost', 'query_lost', 'swap_other_error',
                     'paused_grow', 'paused_shrink', 'unstable_resize']
            for case in cases:
                for debug_level in ('0', '3'):
                    with self.subTest(case=case, debug_level=debug_level):
                        subprocess.run([str(binary), case, debug_level], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
