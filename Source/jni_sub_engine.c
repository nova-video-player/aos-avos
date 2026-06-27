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

// --- LIFECYCLE & SURFACE ---

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
    ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
    sub_engine_attach_surface(get_engine(handle), window);
    // ANativeWindow_fromSurface acquires a ref. We can release ours so it destroys when the Surface does.
    if (window) ANativeWindow_release(window);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSurfaceChanged(JNIEnv *env, jobject thiz, jlong handle, jint width, jint height) {
    sub_engine_surface_resized(get_engine(handle), width, height);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSurfaceDestroyed(JNIEnv *env, jobject thiz, jlong handle) {
    sub_engine_detach_surface(get_engine(handle));
}

// --- 3D HYBRID RENDER BRIDGE ---

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetUIMode(JNIEnv *env, jobject thiz, jlong handle, jint mode) {
    sub_engine_set_ui_mode(get_engine(handle), mode);
}

JNIEXPORT jboolean JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeFillBitmap(JNIEnv *env, jobject thiz, jlong handle, jobject jbitmap) {
    SUB_ENGINE *eng = get_engine(handle);
    if (!eng) return JNI_FALSE;

    AndroidBitmapInfo info;
    void* pixels;

    if (AndroidBitmap_getInfo(env, jbitmap, &info) < 0) return JNI_FALSE;
    if (info.format != ANDROID_BITMAP_FORMAT_RGBA_8888) return JNI_FALSE;
    if (AndroidBitmap_lockPixels(env, jbitmap, &pixels) < 0) return JNI_FALSE;

    // Fast clear the frame to transparent
    memset(pixels, 0, info.stride * info.height);

    int has_subs = sub_engine_fill_bitmap(eng, pixels, info.width, info.height, info.stride);

    AndroidBitmap_unlockPixels(env, jbitmap);
    return has_subs ? JNI_TRUE : JNI_FALSE;
}

// --- TYPOGRAPHY & MASTER CONTROL ---

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetFontSize(JNIEnv *env, jobject thiz, jlong handle, jfloat pt) {
    sub_style_set_font_size(sub_engine_get_style(get_engine(handle)), pt);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetFontScale(JNIEnv *env, jobject thiz, jlong handle, jfloat scale) {
    sub_style_set_font_scale(sub_engine_get_style(get_engine(handle)), scale);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetFontFamily(JNIEnv *env, jobject thiz, jlong handle, jstring familyName) {
    if (!familyName) return;
    const char *str = (*env)->GetStringUTFChars(env, familyName, 0);
    sub_style_set_font_family(sub_engine_get_style(get_engine(handle)), str);
    (*env)->ReleaseStringUTFChars(env, familyName, str);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetBold(JNIEnv *env, jobject thiz, jlong handle, jboolean bold) {
    sub_style_set_bold(sub_engine_get_style(get_engine(handle)), bold);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetTextColor(JNIEnv *env, jobject thiz, jlong handle, jint color) {
    sub_style_set_text_color(sub_engine_get_style(get_engine(handle)), (uint32_t)color);
}

// --- BORDERS, SHADOWS, AND BACKGROUNDS ---

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetOutlineColor(JNIEnv *env, jobject thiz, jlong handle, jint color) {
    sub_style_set_outline_color(sub_engine_get_style(get_engine(handle)), (uint32_t)color);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetOutlineWidth(JNIEnv *env, jobject thiz, jlong handle, jfloat px) {
    sub_style_set_outline_width(sub_engine_get_style(get_engine(handle)), px);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetShadowColor(JNIEnv *env, jobject thiz, jlong handle, jint color) {
    sub_style_set_shadow_color(sub_engine_get_style(get_engine(handle)), (uint32_t)color);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetShadowWidth(JNIEnv *env, jobject thiz, jlong handle, jfloat px) {
    sub_style_set_shadow_width(sub_engine_get_style(get_engine(handle)), px);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetBackgroundMode(JNIEnv *env, jobject thiz, jlong handle, jint mode) {
    sub_style_set_bg_mode(sub_engine_get_style(get_engine(handle)), mode);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetBackgroundColor(JNIEnv *env, jobject thiz, jlong handle, jint color) {
    sub_style_set_bg_color(sub_engine_get_style(get_engine(handle)), (uint32_t)color);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetBackgroundOpacity(JNIEnv *env, jobject thiz, jlong handle, jfloat opacity) {
    SUB_USER_STYLE *style = sub_engine_get_style(get_engine(handle));
    if (style) {
        // Convert 0.0-1.0 float to 0-255 Android Alpha
        uint8_t android_alpha = (uint8_t)(255.0f * opacity);
        // Libass uses RGBA where A is transparency (0 = solid, 255 = fully transparent)
        uint8_t ass_transparency = 255 - android_alpha;

        // Isolate RGB and replace Alpha
        style->bg_color = (style->bg_color & 0xFFFFFF00) | ass_transparency;
        style->serial++;
    }
}

// --- POSITIONING & OVERRIDES ---

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetVerticalOffset(JNIEnv *env, jobject thiz, jlong handle, jfloat pixels) {
    sub_style_set_margin_bottom(sub_engine_get_style(get_engine(handle)), (int)pixels);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetOverrideMode(JNIEnv *env, jobject thiz, jlong handle, jint mode) {
    sub_style_set_override_mode(sub_engine_get_style(get_engine(handle)), mode);
}
