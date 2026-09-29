#ifndef ANDROID_STUB_NATIVE_WINDOW_H
#define ANDROID_STUB_NATIVE_WINDOW_H
typedef struct ANativeWindow ANativeWindow;

/* Added for the frame-identity tests (test/run-race-harness.sh and friends): unlike
 * sub_format_ssa.c, which only ever sees ANativeWindow as an opaque pointer, sub_render_gl.c
 * calls these three. Bodies are the no-ops in android_stub.c. Harmless to the existing
 * ssa_geometry_test.c/ssa_style_cache_test.c, which never call them. */
void ANativeWindow_acquire(ANativeWindow *window);
void ANativeWindow_release(ANativeWindow *window);
int  ANativeWindow_setBuffersGeometry(ANativeWindow *window, int width, int height, int format);
#endif
