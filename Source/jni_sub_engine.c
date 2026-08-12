#include "jni_sub_engine.h"
#include "sub_engine_registry.h"
#include <android/native_window_jni.h>
#include <android/bitmap.h>
#include <stddef.h>
#include <string.h>

// Helper to extract the engine pointer
static SUB_ENGINE* get_engine(jlong handle) {
    return (SUB_ENGINE*)(intptr_t)handle;
}

// --- LIFECYCLE & SURFACE ---

JNIEXPORT jlong JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeCreate(JNIEnv *env, jobject thiz) {
    SUB_ENGINE *eng = sub_engine_create();
    sub_engine_registry_publish(eng);
    return (jlong)(intptr_t)eng;
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeDestroy(JNIEnv *env, jobject thiz, jlong handle) {
    SUB_ENGINE *eng = get_engine(handle);
    // Retract FIRST and block until every STREAM that had acquired a
    // reference to this exact engine has released it (see
    // sub_engine_registry.h). Only once that's guaranteed is it safe to
    // free the engine below -- this is what makes it impossible for a
    // STREAM::sub_engine snapshot to outlive the memory it points to.
    sub_engine_registry_retract(eng);
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
    SUB_ENGINE *eng = get_engine(handle);
    sub_style_set_font_size(sub_engine_get_style(eng), pt);
    sub_engine_force_wake(eng);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetFontScale(JNIEnv *env, jobject thiz, jlong handle, jfloat scale) {
    SUB_ENGINE *eng = get_engine(handle);
    sub_style_set_font_scale(sub_engine_get_style(eng), scale);
    sub_engine_force_wake(eng);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetFontFamily(JNIEnv *env, jobject thiz, jlong handle, jstring familyName) {
    if (!familyName) return;
    SUB_ENGINE *eng = get_engine(handle);
    const char *str = (*env)->GetStringUTFChars(env, familyName, 0);
    sub_style_set_font_family(sub_engine_get_style(eng), str);
    (*env)->ReleaseStringUTFChars(env, familyName, str);
    sub_engine_force_wake(eng);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetBold(JNIEnv *env, jobject thiz, jlong handle, jboolean bold) {
    SUB_ENGINE *eng = get_engine(handle);
    sub_style_set_bold(sub_engine_get_style(eng), bold);
    sub_engine_force_wake(eng);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetTextColor(JNIEnv *env, jobject thiz, jlong handle, jint color) {
    SUB_ENGINE *eng = get_engine(handle);
    sub_style_set_text_color(sub_engine_get_style(eng), (uint32_t)color);
    sub_engine_force_wake(eng);
}

// --- BORDERS, SHADOWS, AND BACKGROUNDS ---

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetOutlineColor(JNIEnv *env, jobject thiz, jlong handle, jint color) {
    SUB_ENGINE *eng = get_engine(handle);
    sub_style_set_outline_color(sub_engine_get_style(eng), (uint32_t)color);
    sub_engine_force_wake(eng);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetOutlineWidth(JNIEnv *env, jobject thiz, jlong handle, jfloat px) {
    SUB_ENGINE *eng = get_engine(handle);
    sub_style_set_outline_width(sub_engine_get_style(eng), px);
    sub_engine_force_wake(eng);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetShadowColor(JNIEnv *env, jobject thiz, jlong handle, jint color) {
    SUB_ENGINE *eng = get_engine(handle);
    sub_style_set_shadow_color(sub_engine_get_style(eng), (uint32_t)color);
    sub_engine_force_wake(eng);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetShadowWidth(JNIEnv *env, jobject thiz, jlong handle, jfloat px) {
    SUB_ENGINE *eng = get_engine(handle);
    sub_style_set_shadow_width(sub_engine_get_style(eng), px);
    sub_engine_force_wake(eng);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetBackgroundMode(JNIEnv *env, jobject thiz, jlong handle, jint mode) {
    SUB_ENGINE *eng = get_engine(handle);
    sub_style_set_bg_mode(sub_engine_get_style(eng), mode);
    sub_engine_force_wake(eng);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetBackgroundColor(JNIEnv *env, jobject thiz, jlong handle, jint color) {
    SUB_ENGINE *eng = get_engine(handle);
    sub_style_set_bg_color(sub_engine_get_style(eng), (uint32_t)color);
    sub_engine_force_wake(eng);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetBackgroundOpacity(JNIEnv *env, jobject thiz, jlong handle, jfloat opacity) {
    SUB_ENGINE *eng = get_engine(handle);
    // Convert 0.0-1.0 float to 0-255 Android Alpha; sub_style_set_bg_opacity() does the
    // libass transparency-byte conversion and the locked read-modify-write on bg_color.
    uint8_t android_alpha = (uint8_t)(255.0f * opacity);
    sub_style_set_bg_opacity(sub_engine_get_style(eng), android_alpha);
    sub_engine_force_wake(eng);
}

// --- POSITIONING & OVERRIDES ---

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetVerticalOffset(JNIEnv *env, jobject thiz, jlong handle, jfloat pixels) {
    SUB_ENGINE *eng = get_engine(handle);
    sub_style_set_margin_bottom(sub_engine_get_style(eng), (int)pixels);
    sub_engine_force_wake(eng);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetOverrideMode(JNIEnv *env, jobject thiz, jlong handle, jint mode) {
    SUB_ENGINE *eng = get_engine(handle);
    sub_style_set_override_mode(sub_engine_get_style(eng), mode);
    sub_engine_force_wake(eng);
}

// --- CUSTOM FONTS FOLDER (third-party fonts dir, MX Player / mpv-android style) ---

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetFontsFolder(JNIEnv *env, jobject thiz, jlong handle, jstring dirPath) {
    SUB_ENGINE *eng = get_engine(handle);
    if (!eng) return;

    if (!dirPath) {
        // Same "clear" convention as nativeSetFontFamily would use if it
        // supported clearing: NULL jstring -> NULL C string -> feature off.
        sub_engine_set_fonts_dir(eng, NULL);
        return;
    }

    const char *path = (*env)->GetStringUTFChars(env, dirPath, 0);
    sub_engine_set_fonts_dir(eng, path);
    (*env)->ReleaseStringUTFChars(env, dirPath, path);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetDefaultFontName(JNIEnv *env, jobject thiz, jlong handle, jstring familyName) {
    SUB_ENGINE *eng = get_engine(handle);
    if (!eng) return;

    if (!familyName) {
        sub_engine_set_default_font_name(eng, NULL);
        return;
    }

    const char *name = (*env)->GetStringUTFChars(env, familyName, 0);
    sub_engine_set_default_font_name(eng, name);
    (*env)->ReleaseStringUTFChars(env, familyName, name);
}
