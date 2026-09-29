/*
 * Link-time bodies for this directory's NDK header stand-ins (android/, EGL/, GLES2/,
 * jni.h's free functions). Every one of these is a no-op or a "not available" failure;
 * see the header each is declared in for why that's a faithful stand-in for a host build
 * and what it does NOT exercise (the render thread's real GL draw path, most notably).
 *
 * This is the one .c file in android_stub/ -- the existing android/native_window.h needed
 * no function bodies (ANativeWindow is only ever a pointer here), so there was nothing to
 * put one in until these headers needed real (if trivial) definitions.
 */
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/bitmap.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdarg.h>
#include <stddef.h>

/* android/native_window.h itself declares no functions (a bare ANativeWindow typedef), but
 * sub_render_gl.c calls these three against it. */
void ANativeWindow_acquire(ANativeWindow *window) { (void)window; }
void ANativeWindow_release(ANativeWindow *window) { (void)window; }
int  ANativeWindow_setBuffersGeometry(ANativeWindow *window, int w, int h, int format) {
    (void)window; (void)w; (void)h; (void)format; return 0;
}
ANativeWindow *ANativeWindow_fromSurface(JNIEnv *env, jobject surface) {
    (void)env; (void)surface; return NULL;
}

int AndroidBitmap_getInfo(JNIEnv *env, jobject jbitmap, AndroidBitmapInfo *info) {
    (void)env; (void)jbitmap; (void)info; return -1; /* no real jobject bitmap off-Android */
}
int AndroidBitmap_lockPixels(JNIEnv *env, jobject jbitmap, void **addrPtr) {
    (void)env; (void)jbitmap; (void)addrPtr; return -1;
}
int AndroidBitmap_unlockPixels(JNIEnv *env, jobject jbitmap) { (void)env; (void)jbitmap; return 0; }

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
    (void)prio; (void)tag; (void)fmt; return 0;
}

EGLDisplay eglGetDisplay(EGLDisplay d) { (void)d; return (EGLDisplay)1; }
int eglInitialize(EGLDisplay d, EGLint *a, EGLint *b) { (void)d; (void)a; (void)b; return 1; }
int eglChooseConfig(EGLDisplay d, const EGLint *a, EGLConfig *c, EGLint n, EGLint *m) {
    (void)d; (void)a; (void)c; (void)n; (void)m; return 1;
}
EGLContext eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext s, const EGLint *a) {
    (void)d; (void)c; (void)s; (void)a; return (EGLContext)1;
}
EGLSurface eglCreateWindowSurface(EGLDisplay d, EGLConfig c, void *w, const EGLint *a) {
    (void)d; (void)c; (void)w; (void)a; return EGL_NO_SURFACE; /* stays surfaceless -- see file header */
}
int eglMakeCurrent(EGLDisplay a, EGLSurface b, EGLSurface c, EGLContext d) { (void)a; (void)b; (void)c; (void)d; return 1; }
int eglDestroySurface(EGLDisplay a, EGLSurface b) { (void)a; (void)b; return 1; }
int eglSwapInterval(EGLDisplay a, EGLint b) { (void)a; (void)b; return 1; }
int eglQuerySurface(EGLDisplay a, EGLSurface b, EGLint c, EGLint *d) { (void)a; (void)b; (void)c; (void)d; return 1; }
int eglSwapBuffers(EGLDisplay a, EGLSurface b) { (void)a; (void)b; return 1; }
int eglDestroyContext(EGLDisplay a, EGLContext b) { (void)a; (void)b; return 1; }
int eglTerminate(EGLDisplay a) { (void)a; return 1; }

GLuint glCreateShader(GLenum a) { (void)a; return 1; }
void glShaderSource(GLuint a, GLsizei b, const char *const *c, const GLint *d) { (void)a; (void)b; (void)c; (void)d; }
void glCompileShader(GLuint a) { (void)a; }
void glGetShaderiv(GLuint a, GLenum b, GLint *c) { (void)a; (void)b; *c = 1; }
void glGetShaderInfoLog(GLuint a, GLsizei b, GLsizei *c, char *d) { (void)a; (void)b; (void)c; (void)d; }
GLuint glCreateProgram(void) { return 1; }
void glAttachShader(GLuint a, GLuint b) { (void)a; (void)b; }
void glLinkProgram(GLuint a) { (void)a; }
void glGetProgramiv(GLuint a, GLenum b, GLint *c) { (void)a; (void)b; *c = 1; }
void glGetProgramInfoLog(GLuint a, GLsizei b, GLsizei *c, char *d) { (void)a; (void)b; (void)c; (void)d; }
void glDeleteShader(GLuint a) { (void)a; }
void glDeleteProgram(GLuint a) { (void)a; }
GLint glGetAttribLocation(GLuint a, const char *b) { (void)a; (void)b; return 0; }
void glGenTextures(GLsizei a, GLuint *b) { (void)a; (void)b; }
void glBindTexture(GLenum a, GLuint b) { (void)a; (void)b; }
void glDeleteTextures(GLsizei a, const GLuint *b) { (void)a; (void)b; }
void glTexParameteri(GLenum a, GLenum b, GLint c) { (void)a; (void)b; (void)c; }
void glViewport(GLint a, GLint b, GLsizei c, GLsizei d) { (void)a; (void)b; (void)c; (void)d; }
void glClearColor(GLfloat a, GLfloat b, GLfloat c, GLfloat d) { (void)a; (void)b; (void)c; (void)d; }
void glClear(unsigned a) { (void)a; }
void glUseProgram(GLuint a) { (void)a; }
void glEnable(GLenum a) { (void)a; }
void glDisable(GLenum a) { (void)a; }
void glBlendFunc(GLenum a, GLenum b) { (void)a; (void)b; }
void glPixelStorei(GLenum a, GLint b) { (void)a; (void)b; }
void glTexImage2D(GLenum a, GLint b, GLint c, GLsizei d, GLsizei e, GLint f, GLenum g, GLenum h, const void *i) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i;
}
void glVertexAttribPointer(GLuint a, GLint b, GLenum c, int d, GLsizei e, const void *f) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
}
void glEnableVertexAttribArray(GLuint a) { (void)a; }
void glDisableVertexAttribArray(GLuint a) { (void)a; }
void glDrawArrays(GLenum a, GLint b, GLsizei c) { (void)a; (void)b; (void)c; }
