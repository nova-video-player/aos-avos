/*
 * Minimal stand-in for the NDK's <android/native_window_jni.h>, alongside this directory's
 * existing native_window.h (which already covers plain ANativeWindow). jni_sub_engine.c's
 * nativeSurfaceCreated() calls ANativeWindow_fromSurface() to turn a Java Surface into an
 * ANativeWindow; on a host build there is no real Surface, so the stub body (android_stub.c)
 * always returns NULL, and sub_engine_attach_surface() must already treat a NULL window as
 * "no surface" (the render thread just stays surfaceless, as it does for the 3D UI mode) --
 * nothing here changes engine behavior versus what 3D mode already exercises.
 */
#ifndef ANDROID_STUB_NATIVE_WINDOW_JNI_H
#define ANDROID_STUB_NATIVE_WINDOW_JNI_H
#include <jni.h>
#include <android/native_window.h>
ANativeWindow *ANativeWindow_fromSurface(JNIEnv *env, jobject surface);
#endif
