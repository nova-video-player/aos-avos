/*
 * Minimal stand-in for the NDK's <EGL/egl.h>, just enough for sub_render_gl.c's render
 * thread to link and run. The stub bodies (android_stub.c) never produce a working GL
 * context: eglCreateWindowSurface() always returns EGL_NO_SURFACE, so the thread runs
 * "surfaceless" for the whole test -- the exact same path the 3D UI mode already takes in
 * production (no window attached), which is what every test in this directory exercises.
 * The actual GL draw calls (glDrawArrays and friends, stubbed in GLES2/gl2.h) are therefore
 * never reached; this file does NOT validate the real GPU blit path.
 */
#ifndef ANDROID_STUB_EGL_H
#define ANDROID_STUB_EGL_H

typedef void *EGLDisplay, *EGLConfig, *EGLContext, *EGLSurface;
typedef int   EGLint;

#define EGL_DEFAULT_DISPLAY ((void*)0)
#define EGL_NO_SURFACE       ((EGLSurface)0)
#define EGL_NO_CONTEXT       ((EGLContext)0)
enum {
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_BLUE_SIZE, EGL_GREEN_SIZE,
    EGL_RED_SIZE, EGL_ALPHA_SIZE, EGL_NONE, EGL_CONTEXT_CLIENT_VERSION,
    EGL_WIDTH, EGL_HEIGHT
};

EGLDisplay eglGetDisplay(EGLDisplay native_display);
int  eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor);
int  eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list, EGLConfig *configs, EGLint config_size, EGLint *num_config);
EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share_context, const EGLint *attrib_list);
EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, void *win, const EGLint *attrib_list);
int  eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx);
int  eglDestroySurface(EGLDisplay dpy, EGLSurface surface);
int  eglSwapInterval(EGLDisplay dpy, EGLint interval);
int  eglQuerySurface(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint *value);
int  eglSwapBuffers(EGLDisplay dpy, EGLSurface surface);
int  eglDestroyContext(EGLDisplay dpy, EGLContext ctx);
int  eglTerminate(EGLDisplay dpy);
#endif
