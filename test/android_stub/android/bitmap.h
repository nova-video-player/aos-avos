/*
 * Minimal stand-in for the NDK's <android/bitmap.h>. jni_sub_engine.c's nativeFillBitmap()
 * needs this to link; the stub body (android_stub.c) always fails AndroidBitmap_getInfo()/
 * lockPixels(), matching what happens off-Android with no real jobject bitmap. Tests that
 * exercise nativeFillBitmap()'s pixel/generation contract (fill_contract_test.c,
 * race_harness_test.c) call sub_engine_fill_bitmap()/sub_render_gl_fill_bitmap() directly
 * instead of going through the JNI entry point, so this stub is link-only for them; only
 * jni_listener_lifecycle_test.c links jni_sub_engine.c itself, and it never calls
 * nativeFillBitmap().
 */
#ifndef ANDROID_STUB_BITMAP_H
#define ANDROID_STUB_BITMAP_H
#include <jni.h>
#include <stdint.h>

enum { ANDROID_BITMAP_FORMAT_RGBA_8888 = 1 };

typedef struct {
    uint32_t width, height, stride;
    int32_t  format;
    uint32_t flags;
} AndroidBitmapInfo;

int AndroidBitmap_getInfo(JNIEnv *env, jobject jbitmap, AndroidBitmapInfo *info);
int AndroidBitmap_lockPixels(JNIEnv *env, jobject jbitmap, void **addrPtr);
int AndroidBitmap_unlockPixels(JNIEnv *env, jobject jbitmap);
#endif
