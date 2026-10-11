/*
 * host_stubs.h -- header-only stand-ins for the Android NDK headers the subtitle engine
 * includes, so Source/ files compile and link on a plain host build. Nothing in Source/ is
 * modified.
 *
 * test_host_native.py writes one-line forwarding headers into its temp build directory
 * (<jni.h>, <android/native_window.h>, <android/native_window_jni.h>, <android/bitmap.h>,
 * <android/log.h>, <EGL/egl.h>, <GLES2/gl2.h>), each of which just includes this file, so
 * the engine's `#include <EGL/egl.h>` etc. resolve here. Every function is a `static inline`
 * no-op or a "not available" failure -- there is no companion .c file.
 *
 * What this does NOT exercise:
 *   - The render thread's real GL path. eglCreateWindowSurface() always returns
 *     EGL_NO_SURFACE, so the thread runs surfaceless for the whole test -- the same path the
 *     3D UI mode takes in production (no window attached). The gl*() bodies are therefore
 *     never reached; none of this validates the GPU blend path.
 *   - Real Surfaces/Bitmaps. ANativeWindow_fromSurface() returns NULL and AndroidBitmap_*()
 *     fail, matching what happens off-Android. Tests drive sub_engine_fill_bitmap() /
 *     sub_render_gl_fill_bitmap() directly instead of nativeFillBitmap().
 *   - A real JVM. JNIEnv/JavaVM are pointers to const function tables (like the real
 *     headers); a test supplies its OWN table (see jni_listener_test.c's mock JavaVM) and
 *     fills in only the slots it cares about. This header just fixes the layout both sides
 *     agree on, and declares only the entries jni_sub_engine.c actually calls.
 *   - __android_log_print() discards its message: diagnostic only, never gates control flow
 *     in the files these tests link.
 */
#ifndef HOST_STUBS_H
#define HOST_STUBS_H

#include <stddef.h>
#include <stdint.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

/* ------------------------------------------------------------------ jni.h */

typedef int32_t  jint;
typedef int64_t  jlong;
typedef float    jfloat;
typedef unsigned char jboolean;
typedef void    *jobject, *jstring, *jlongArray, *jclass, *jmethodID;

#define JNI_TRUE  1
#define JNI_FALSE 0
#define JNI_OK        0
#define JNI_EDETACHED (-2)
#define JNI_VERSION_1_4 0x00010004
#define JNIEXPORT
#define JNICALL

struct JNINativeInterface_;
typedef const struct JNINativeInterface_ *JNIEnv;
struct JNIInvokeInterface_;
typedef const struct JNIInvokeInterface_ *JavaVM;

typedef struct { jint version; const char *name; jobject group; } JavaVMAttachArgs;

struct JNIInvokeInterface_ {
    jint (*GetEnv)(JavaVM *vm, void **penv, jint version);
    jint (*AttachCurrentThread)(JavaVM *vm, JNIEnv **penv, void *args);
    jint (*DetachCurrentThread)(JavaVM *vm);
};

struct JNINativeInterface_ {
    const char *(*GetStringUTFChars)(JNIEnv *env, jstring str, jboolean *isCopy);
    void        (*ReleaseStringUTFChars)(JNIEnv *env, jstring str, const char *utf);
    jint        (*GetArrayLength)(JNIEnv *env, jlongArray array);
    void        (*SetLongArrayRegion)(JNIEnv *env, jlongArray array, jint start, jint len, const jlong *buf);
    jint        (*GetJavaVM)(JNIEnv *env, JavaVM **vm);
    jclass      (*GetObjectClass)(JNIEnv *env, jobject obj);
    jmethodID   (*GetMethodID)(JNIEnv *env, jclass clazz, const char *name, const char *sig);
    void        (*DeleteLocalRef)(JNIEnv *env, jobject obj);
    jobject     (*NewGlobalRef)(JNIEnv *env, jobject obj);
    void        (*DeleteGlobalRef)(JNIEnv *env, jobject globalRef);
    void        (*CallVoidMethod)(JNIEnv *env, jobject obj, jmethodID methodID, ...);
    jboolean    (*ExceptionCheck)(JNIEnv *env);
    void        (*ExceptionDescribe)(JNIEnv *env);
    void        (*ExceptionClear)(JNIEnv *env);
};

/* ------------------------------------------- android/native_window{,_jni}.h */

typedef struct ANativeWindow ANativeWindow;

/* sub_render_gl.c calls these three; sub_format_ssa.c only ever sees ANativeWindow as an
 * opaque pointer. */
static inline void ANativeWindow_acquire(ANativeWindow *window) {}
static inline void ANativeWindow_release(ANativeWindow *window) {}
static inline int  ANativeWindow_setBuffersGeometry(ANativeWindow *window, int width, int height, int format) { return 0; }

/* jni_sub_engine.c's nativeSurfaceCreated() turns a Java Surface into an ANativeWindow. No
 * real Surface here, so NULL -- sub_engine_attach_surface() already treats a NULL window as
 * "no surface" (surfaceless render thread, as in 3D UI mode). */
static inline ANativeWindow *ANativeWindow_fromSurface(JNIEnv *env, jobject surface) { return NULL; }

/* ------------------------------------------------------------ android/bitmap.h */

enum { ANDROID_BITMAP_FORMAT_RGBA_8888 = 1 };

typedef struct {
    uint32_t width, height, stride;
    int32_t  format;
    uint32_t flags;
} AndroidBitmapInfo;

/* jni_sub_engine.c's nativeFillBitmap() needs these to link. They always fail, as off-Android
 * with no real jobject bitmap. */
static inline int AndroidBitmap_getInfo(JNIEnv *env, jobject jbitmap, AndroidBitmapInfo *info) { return -1; }
static inline int AndroidBitmap_lockPixels(JNIEnv *env, jobject jbitmap, void **addrPtr) { return -1; }
static inline int AndroidBitmap_unlockPixels(JNIEnv *env, jobject jbitmap) { return 0; }

/* -------------------------------------------------------------- android/log.h */

enum { ANDROID_LOG_WARN = 5 };
static inline int __android_log_print(int prio, const char *tag, const char *fmt, ...) { return 0; }

/* ------------------------------------------------------------------ EGL/egl.h */

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

static inline EGLDisplay eglGetDisplay(EGLDisplay d) { return (EGLDisplay)1; }
static inline int eglInitialize(EGLDisplay d, EGLint *major, EGLint *minor) { return 1; }
static inline int eglChooseConfig(EGLDisplay d, const EGLint *attribs, EGLConfig *configs, EGLint size, EGLint *num) { return 1; }
static inline EGLContext eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext share, const EGLint *attribs) { return (EGLContext)1; }
static inline EGLSurface eglCreateWindowSurface(EGLDisplay d, EGLConfig c, void *win, const EGLint *attribs) { return EGL_NO_SURFACE; }
static inline int eglMakeCurrent(EGLDisplay d, EGLSurface draw, EGLSurface read, EGLContext ctx) { return 1; }
static inline int eglDestroySurface(EGLDisplay d, EGLSurface s) { return 1; }
static inline int eglSwapInterval(EGLDisplay d, EGLint interval) { return 1; }
static inline int eglQuerySurface(EGLDisplay d, EGLSurface s, EGLint attribute, EGLint *value) { return 1; }
static inline int eglSwapBuffers(EGLDisplay d, EGLSurface s) { return 1; }
static inline int eglDestroyContext(EGLDisplay d, EGLContext ctx) { return 1; }
static inline int eglTerminate(EGLDisplay d) { return 1; }

/* ---------------------------------------------------------------- GLES2/gl2.h */

typedef unsigned int GLuint, GLenum;
typedef int          GLint, GLsizei;
typedef float        GLfloat;

enum {
    GL_VERTEX_SHADER, GL_FRAGMENT_SHADER, GL_COMPILE_STATUS, GL_LINK_STATUS,
    GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_TEXTURE_MAG_FILTER, GL_LINEAR,
    GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE, GL_COLOR_BUFFER_BIT,
    GL_BLEND, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_UNPACK_ALIGNMENT,
    GL_RGBA, GL_UNSIGNED_BYTE, GL_FLOAT, GL_FALSE, GL_TRIANGLE_STRIP
};

static inline GLuint glCreateShader(GLenum type) { return 1; }
static inline void   glShaderSource(GLuint shader, GLsizei count, const char *const *string, const GLint *length) {}
static inline void   glCompileShader(GLuint shader) {}
static inline void   glGetShaderiv(GLuint shader, GLenum pname, GLint *params) { *params = 1; }
static inline void   glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei *length, char *infoLog) {}
static inline GLuint glCreateProgram(void) { return 1; }
static inline void   glAttachShader(GLuint program, GLuint shader) {}
static inline void   glLinkProgram(GLuint program) {}
static inline void   glGetProgramiv(GLuint program, GLenum pname, GLint *params) { *params = 1; }
static inline void   glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei *length, char *infoLog) {}
static inline void   glDeleteShader(GLuint shader) {}
static inline void   glDeleteProgram(GLuint program) {}
static inline GLint  glGetAttribLocation(GLuint program, const char *name) { return 0; }
static inline void   glGenTextures(GLsizei n, GLuint *textures) {}
static inline void   glBindTexture(GLenum target, GLuint texture) {}
static inline void   glDeleteTextures(GLsizei n, const GLuint *textures) {}
static inline void   glTexParameteri(GLenum target, GLenum pname, GLint param) {}
static inline void   glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {}
static inline void   glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {}
static inline void   glClear(unsigned mask) {}
static inline void   glUseProgram(GLuint program) {}
static inline void   glEnable(GLenum cap) {}
static inline void   glDisable(GLenum cap) {}
static inline void   glBlendFunc(GLenum sfactor, GLenum dfactor) {}
static inline void   glPixelStorei(GLenum pname, GLint param) {}
static inline void   glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                                  GLint border, GLenum format, GLenum type, const void *pixels) {}
static inline void   glVertexAttribPointer(GLuint index, GLint size, GLenum type, int normalized, GLsizei stride, const void *pointer) {}
static inline void   glEnableVertexAttribArray(GLuint index) {}
static inline void   glDisableVertexAttribArray(GLuint index) {}
static inline void   glDrawArrays(GLenum mode, GLint first, GLsizei count) {}

#pragma GCC diagnostic pop

#endif /* HOST_STUBS_H */
