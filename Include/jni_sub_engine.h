/*
 * jni_sub_engine.h — JNI bridge declarations.
 *
 * Corresponding Java class: com.archos.mediacenter.video.player.SubtitleEngine
 */

#pragma once

#include <jni.h>

#ifdef __cplusplus
extern "C" {
    #endif

    /* ── Lifecycle & Surface ── */
    JNIEXPORT jlong   JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeCreate(JNIEnv *env, jobject thiz);
    JNIEXPORT void    JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeDestroy(JNIEnv *env, jobject thiz, jlong handle);
    JNIEXPORT void    JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSurfaceCreated(JNIEnv *env, jobject thiz, jlong handle, jobject surface);
    JNIEXPORT void    JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSurfaceChanged(JNIEnv *env, jobject thiz, jlong handle, jint width, jint height);
    JNIEXPORT void    JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSurfaceDestroyed(JNIEnv *env, jobject thiz, jlong handle);

    /* ── 3D Hybrid Render Bridge ── */
    JNIEXPORT jboolean JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeFillBitmap(JNIEnv *env, jobject thiz, jlong handle, jobject bitmap);
    JNIEXPORT void    JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetUIMode(JNIEnv *env, jobject thiz, jlong handle, jint mode);
    JNIEXPORT jboolean JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSyncFillBitmap(JNIEnv *env, jobject thiz, jlong handle, jobject jbitmap);
    JNIEXPORT jlong    JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeGetSubtitleGeneration(JNIEnv *env, jobject thiz, jlong handle);
    /* ── Typography & Master Control ── */
    JNIEXPORT void    JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetFontSize(JNIEnv *env, jobject thiz, jlong handle, jfloat pt);
    JNIEXPORT void    JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetFontScale(JNIEnv *env, jobject thiz, jlong handle, jfloat scale);
    JNIEXPORT void    JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetFontFamily(JNIEnv *env, jobject thiz, jlong handle, jstring familyName);
    JNIEXPORT void    JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetBold(JNIEnv *env, jobject thiz, jlong handle, jboolean bold);
    JNIEXPORT void    JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetTextColor(JNIEnv *env, jobject thiz, jlong handle, jint color);

    /* ── Borders, Shadows, and Backgrounds ── */
    JNIEXPORT void    JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetOutlineColor(JNIEnv *env, jobject thiz, jlong handle, jint color);
    JNIEXPORT void    JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetOutlineWidth(JNIEnv *env, jobject thiz, jlong handle, jfloat px);
    JNIEXPORT void    JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetShadowColor(JNIEnv *env, jobject thiz, jlong handle, jint color);
    JNIEXPORT void    JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetShadowWidth(JNIEnv *env, jobject thiz, jlong handle, jfloat px);
    JNIEXPORT void    JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetBackgroundMode(JNIEnv *env, jobject thiz, jlong handle, jint mode);
    JNIEXPORT void    JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetBackgroundColor(JNIEnv *env, jobject thiz, jlong handle, jint color);
    JNIEXPORT void    JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetBackgroundOpacity(JNIEnv *env, jobject thiz, jlong handle, jfloat opacity);

    /* ── Positioning & Overrides ── */
    JNIEXPORT void    JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetVerticalOffset(JNIEnv *env, jobject thiz, jlong handle, jfloat pixels);
    JNIEXPORT void    JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetOverrideMode(JNIEnv *env, jobject thiz, jlong handle, jint mode);

    /* ── Custom Fonts Folder (third-party fonts dir) ── */
    JNIEXPORT void    JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetFontsFolder(JNIEnv *env, jobject thiz, jlong handle, jstring dirPath);
    JNIEXPORT void    JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetDefaultFontName(JNIEnv *env, jobject thiz, jlong handle, jstring familyName);

    #ifdef __cplusplus
}
#endif
