/*
 * jni_font_name_parser.h — JNI bridge declaration for FontNameParser.java.
 *
 * Corresponding Java class: com.archos.mediacenter.video.utils.FontNameParser
 */

#pragma once

#include <jni.h>

#ifdef __cplusplus
extern "C" {
#endif

JNIEXPORT jobjectArray JNICALL
Java_com_archos_mediacenter_video_utils_FontNameParser_nativeParseFontFile(JNIEnv *env, jclass clazz, jstring path);

#ifdef __cplusplus
}
#endif
