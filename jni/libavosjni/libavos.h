/*
 * Copyright 2017 Archos SA
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef LIBAVOS_H
#define LIBAVOS_H

#include "avos_mp.h"
#include "avos_mr.h"
#include "jni.h"

#include "android/log.h"

#undef LOGD
#undef LOGV
#undef LOGE
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, ##__VA_ARGS__);
#define LOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG, ##__VA_ARGS__);
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, ##__VA_ARGS__);

typedef struct fields_t {
	jclass AvosMediaPlayerClazz;
	jmethodID AvosMediaPlayer_postEventMethod;

	jclass AvosBitmapHelperClazz;
	jmethodID AvosBitmapHelper_createRGBBitmapMethod;

	jclass SubtitleClazz;
	jmethodID Subtitle_createTimedTextSubtitleMethod;
	jmethodID Subtitle_createTimedBitmapSubtitleMethod;

	jclass ClassLoaderClazz;
	jobject myClassLoader;
	jmethodID FindClassMethod;

} fields_t;

extern fields_t fields;

jobject create_bitmap(JNIEnv *env, avos_bgra_bitmap_t *avos_bitmap, uint32_t out_width, uint32_t out_height);

#endif
