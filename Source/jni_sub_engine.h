/*
 * jni_sub_engine.h — JNI bridge declarations.
 *
 * Corresponding Java class: com.archos.mediacenter.video.player.SubtitleEngine
 * (new — replaces SubtitleManager's rendering responsibilities; settings
 * UI screens call SubtitleEngine.setXxx(...) which forwards here).
 *
 * Naming follows the standard JNI convention:
 *   Java_com_archos_mediacenter_video_player_SubtitleEngine_<method>
 *
 * Every native method takes the long handle (jlong) returned by
 * nativeCreate(), matching the opaque SUB_ENGINE* pattern used
 * elsewhere in avos's JNI glue (see android_codecs.c's myVm pattern).
 */

#pragma once

#include <jni.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Lifecycle ── */
JNIEXPORT jlong  JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeCreate(JNIEnv *env, jobject thiz);
JNIEXPORT void   JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeDestroy(JNIEnv *env, jobject thiz, jlong handle);

/* ── Track control (called by avos's existing subtitle-track-switch path,
 *    likely from native code directly rather than JNI — see note in
 *    sub_engine.h; exposed here too for cases where Java drives it) ── */
JNIEXPORT jint   JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeOpenTrack(JNIEnv *env, jobject thiz, jlong handle, jint formatId, jint videoW, jint videoH, jbyteArray codecPrivate);
JNIEXPORT void   JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeCloseTrack(JNIEnv *env, jobject thiz, jlong handle);

/* ── Surface lifecycle — call from SubtitleSurfaceView's SurfaceHolder.Callback ── */
JNIEXPORT void   JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSurfaceCreated(JNIEnv *env, jobject thiz, jlong handle, jobject surface);
JNIEXPORT void   JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSurfaceChanged(JNIEnv *env, jobject thiz, jlong handle, jint width, jint height);
JNIEXPORT void   JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSurfaceDestroyed(JNIEnv *env, jobject thiz, jlong handle);

/* ── Playback state ── */
JNIEXPORT void   JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetPaused(JNIEnv *env, jobject thiz, jlong handle, jboolean paused);

/* ── User style setters ── */
JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetTextColor(JNIEnv *env, jobject thiz, jlong handle, jint color);
JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetOutlineColor(JNIEnv *env, jobject thiz, jlong handle, jint color);
JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetOutlineWidth(JNIEnv *env, jobject thiz, jlong handle, jfloat px);
JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetBackgroundEnabled(JNIEnv *env, jobject thiz, jlong handle, jboolean enabled);
JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetBackgroundColor(JNIEnv *env, jobject thiz, jlong handle, jint color);
JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetBackgroundOpacity(JNIEnv *env, jobject thiz, jlong handle, jfloat opacity);
JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetFontSize(JNIEnv *env, jobject thiz, jlong handle, jfloat pt);
JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetFontScale(JNIEnv *env, jobject thiz, jlong handle, jfloat scale);
JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetFontFamily(JNIEnv *env, jobject thiz, jlong handle, jstring familyName);
JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetBold(JNIEnv *env, jobject thiz, jlong handle, jboolean bold);
JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetItalic(JNIEnv *env, jobject thiz, jlong handle, jboolean italic);
JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetVerticalOffset(JNIEnv *env, jobject thiz, jlong handle, jfloat fraction);
JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetForceOverride(JNIEnv *env, jobject thiz, jlong handle, jboolean force);

/* ── Diagnostics (optional, e.g. for a debug overlay / settings "info" panel) ── */
JNIEXPORT jstring JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeGetStatsString(JNIEnv *env, jobject thiz, jlong handle);

#ifdef __cplusplus
}
#endif
