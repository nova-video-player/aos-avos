#include "jni_sub_engine.h"
#include "sub_engine.h"
#include "sub_style.h"
#include <android/native_window_jni.h>
#include <android/bitmap.h>
#include <stddef.h>
#include <string.h>

// Global pointer so the AVOS core demuxer can easily find the engine
SUB_ENGINE *g_sub_engine = NULL;

// Helper to extract the engine pointer
static SUB_ENGINE* get_engine(jlong handle) {
    return (SUB_ENGINE*)(intptr_t)handle;
}

JNIEXPORT jlong JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeCreate(JNIEnv *env, jobject thiz) {
    SUB_ENGINE *eng = sub_engine_create();
    g_sub_engine = eng;
    return (jlong)(intptr_t)eng;
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeDestroy(JNIEnv *env, jobject thiz, jlong handle) {
    SUB_ENGINE *eng = get_engine(handle);
    if (g_sub_engine == eng) {
        g_sub_engine = NULL; // Clear global before destroy to prevent dangling pointer
    }
    sub_engine_destroy(eng);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSurfaceCreated(JNIEnv *env, jobject thiz, jlong handle, jobject surface) {
    // BUG FIX: ANativeWindow_fromSurface() already increments the refcount by 1.
    // sub_render_gl_attach_surface() calls ANativeWindow_acquire() which increments it AGAIN.
    // We must NOT call ANativeWindow_release() here — let sub_render_gl manage the lifetime.
    // The renderer's lock-protected attach/detach owns the window from this point on.
    ANativeWindow *window = surface ? ANativeWindow_fromSurface(env, surface) : NULL;

    // Only attach the surface here. Sizing happens in nativeSurfaceChanged.
    // sub_render_gl_attach_surface acquires the window itself; we transfer ownership here.
    sub_engine_attach_surface(get_engine(handle), window);

    // BUG FIX: Do NOT call ANativeWindow_release(window) here.
    // sub_render_gl_attach_surface() took ownership via its own ANativeWindow_acquire().
    // ANativeWindow_fromSurface gave us refcount=1. acquire() makes it 2.
    // The renderer's detach will release() once (back to 1), and we release once here (to 0).
    // So we DO release our own reference from fromSurface(), but the renderer holds its own.
    if (window) {
        ANativeWindow_release(window); // Release only OUR fromSurface() ref; renderer has its own
    }
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSurfaceChanged(JNIEnv *env, jobject thiz, jlong handle, jint width, jint height) {
    sub_engine_surface_resized(get_engine(handle), width, height);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSurfaceDestroyed(JNIEnv *env, jobject thiz, jlong handle) {
    sub_engine_detach_surface(get_engine(handle));
}

// 3D Mode
JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetUIMode(JNIEnv *env, jobject thiz, jlong handle, jint mode) {
    // mode 0 = 2D, 1 = SBS, 2 = TB  (already mapped by Java SubtitleEngine.setUIMode)
    sub_engine_set_ui_mode(get_engine(handle), mode);
}

JNIEXPORT jboolean JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeFillBitmap(JNIEnv *env, jobject thiz, jlong handle, jobject jbitmap) {
    SUB_ENGINE *eng = get_engine(handle);
    if (!eng) return JNI_FALSE;

    AndroidBitmapInfo info;
    void* pixels;

    // Interrogate the Java Bitmap Object
    if (AndroidBitmap_getInfo(env, jbitmap, &info) < 0) return JNI_FALSE;
    if (info.format != ANDROID_BITMAP_FORMAT_RGBA_8888) return JNI_FALSE;

    // Lock the pixel buffer directly in RAM
    if (AndroidBitmap_lockPixels(env, jbitmap, &pixels) < 0) return JNI_FALSE;

    // Fast clear the frame to transparent
    memset(pixels, 0, info.stride * info.height);

    // Run the CPU Alpha Blender
    int has_subs = sub_engine_fill_bitmap(eng, pixels, info.width, info.height, info.stride);

    // Release the buffer back to Java
    AndroidBitmap_unlockPixels(env, jbitmap);

    return has_subs ? JNI_TRUE : JNI_FALSE;
}
// ====================================================================
// USER STYLE SETTERS (Java -> C Bridge)
// ====================================================================

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetFontSize(JNIEnv *env, jobject thiz, jlong handle, jfloat pt) {
    sub_style_set_font_size(sub_engine_get_style(get_engine(handle)), pt);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetFontScale(JNIEnv *env, jobject thiz, jlong handle, jfloat scale) {
    sub_style_set_font_scale(sub_engine_get_style(get_engine(handle)), scale);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetFontFamily(JNIEnv *env, jobject thiz, jlong handle, jstring familyName) {
    if (!familyName) return;
    const char *family_str = (*env)->GetStringUTFChars(env, familyName, NULL);
    sub_style_set_font_family(sub_engine_get_style(get_engine(handle)), family_str);
    (*env)->ReleaseStringUTFChars(env, familyName, family_str);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetBold(JNIEnv *env, jobject thiz, jlong handle, jboolean bold) {
    sub_style_set_bold(sub_engine_get_style(get_engine(handle)), bold);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetItalic(JNIEnv *env, jobject thiz, jlong handle, jboolean italic) {
    sub_style_set_italic(sub_engine_get_style(get_engine(handle)), italic);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetTextColor(JNIEnv *env, jobject thiz, jlong handle, jint color) {
    sub_style_set_text_color(sub_engine_get_style(get_engine(handle)), (uint32_t)color);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetOutlineColor(JNIEnv *env, jobject thiz, jlong handle, jint color) {
    sub_style_set_outline_color(sub_engine_get_style(get_engine(handle)), (uint32_t)color);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetOutlineWidth(JNIEnv *env, jobject thiz, jlong handle, jfloat px) {
    SUB_USER_STYLE *style = sub_engine_get_style(get_engine(handle));
    if (style) style->outline_width = (int)px;
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetBackgroundEnabled(JNIEnv *env, jobject thiz, jlong handle, jboolean enabled) {
    SUB_USER_STYLE *style = sub_engine_get_style(get_engine(handle));
    if (style) style->bg_enabled = enabled;
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetBackgroundColor(JNIEnv *env, jobject thiz, jlong handle, jint color) {
    sub_style_set_bg_color(sub_engine_get_style(get_engine(handle)), (uint32_t)color);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetBackgroundOpacity(JNIEnv *env, jobject thiz, jlong handle, jfloat opacity) {
    sub_style_set_bg_opacity(sub_engine_get_style(get_engine(handle)), opacity);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetVerticalOffset(JNIEnv *env, jobject thiz, jlong handle, jfloat fraction) {
    sub_style_set_vertical_offset(sub_engine_get_style(get_engine(handle)), fraction);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetForceOverride(JNIEnv *env, jobject thiz, jlong handle, jboolean force) {
    sub_style_set_force_override(sub_engine_get_style(get_engine(handle)), force);
}
