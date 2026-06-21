#include "jni_sub_engine.h"
#include "sub_engine.h"
#include "sub_style.h"
#include <android/native_window_jni.h>
#include <stddef.h>

// NEW: Global pointer so the AVOS core demuxer can easily find the engine
SUB_ENGINE *g_sub_engine = NULL;

// Helper to extract the engine pointer
static SUB_ENGINE* get_engine(jlong handle) {
    return (SUB_ENGINE*)(intptr_t)handle;
}

JNIEXPORT jlong JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeCreate(JNIEnv *env, jobject thiz) {
    SUB_ENGINE *eng = sub_engine_create();

    g_sub_engine = eng; // Store it globally
    return (jlong)(intptr_t)eng;
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeDestroy(JNIEnv *env, jobject thiz, jlong handle) {
    sub_engine_destroy(get_engine(handle));
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSurfaceCreated(JNIEnv *env, jobject thiz, jlong handle, jobject surface) {
    // Extract the raw hardware window from the Java object
    ANativeWindow *window = surface ? ANativeWindow_fromSurface(env, surface) : NULL;

    // STRICTLY filesv2: Only attach the surface here. Sizing happens in SurfaceChanged.
    sub_engine_attach_surface(get_engine(handle), window);

    if (window) {
        ANativeWindow_release(window);
    }
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSurfaceChanged(JNIEnv *env, jobject thiz, jlong handle, jint width, jint height) {
    sub_engine_surface_resized(get_engine(handle), width, height);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSurfaceDestroyed(JNIEnv *env, jobject thiz, jlong handle) {
    sub_engine_detach_surface(get_engine(handle));
}

// ====================================================================
// PHASE 3: USER STYLE SETTERS (Java -> C Bridge)
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
