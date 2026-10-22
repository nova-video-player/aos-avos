// Copyright 2026 Courville Software
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

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
