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

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "debug.h"
#include "av.h"
#include "device_config.h"

#include "jni.h"

/*
 * Android only:
 * read /system/etc/media_codecs.xml using MediaCodecList c++ class in order to
 * get supported codecs by the device.
 *
 * Used only for DTS/WMA WMV/MPEG2
 */

#define DLHELPER_HEADER "dlhelper_mediacodeclist.h"
#include "dlhelper.h"

#define DBG if(0)

extern JavaVM *myVm;
extern jobject myClassLoader;
extern jmethodID myFindClassMethod;

static int acodecs_is_type_supported(const char *type, int is_sw_allowed)
{
	JNIEnv * env = NULL;
	int i;
	int willDetach = 0;
	jboolean result = 0;

	if ((*myVm)->GetEnv(myVm, (void**)&env, JNI_VERSION_1_4) != JNI_OK) {
		DBG serprintf("ERROR: GetEnv failed\n");
		if(((*myVm)->AttachCurrentThread(myVm, &env, NULL)) != 0 ) {
			DBG serprintf("ERROR: Attach to JVM failed\n");
                        return 0;
		}
		else
			willDetach = 1;
		// if ((*myVm)->GetEnv(myVm, (void**)&env, JNI_VERSION_1_4) != JNI_OK) {
	}
	const char * pkgName = "com/archos/mediacenter/video"; //device_config_get_android_pkg_name();
	size_t lenPkgName = strlen(pkgName);
	const char * suffix = "/utils/CodecDiscovery";
	size_t lenSuffix = strlen(suffix);
	char * javaName = (char*) malloc(sizeof(char)*(lenPkgName+lenSuffix+1));

	strncpy(javaName, pkgName, lenPkgName+1);
	for (i = 0; i < lenPkgName; i++)
		if (javaName[i] == '.')
			javaName[i] = '/';
	strncat(javaName, suffix, lenSuffix);
	DBG serprintf("%s\n", javaName);
	jclass clazz = NULL;
	clazz = (*env)->FindClass(env, javaName);
	jthrowable exception = (*env)->ExceptionOccurred(env);
	if (exception) {
		DBG serprintf("!!!EXCEPTION");
                (*env)->ExceptionClear(env);
		const jstring javaStringName = (*env)->NewStringUTF(env, javaName);
		clazz = (jclass) (*env)->CallObjectMethod(env, myClassLoader,
                                                          myFindClassMethod, javaStringName);
	}
	jmethodID method = (*env)->GetStaticMethodID(env, clazz, "isCodecTypeSupported", "(Ljava/lang/String;Z)Z");
	const jstring string = (*env)->NewStringUTF(env, type);
	result = (*env)->CallStaticBooleanMethod(env, clazz, method, string, (jboolean) is_sw_allowed);

	(*env)->DeleteLocalRef(env, string);
	
	free(javaName);
	if (willDetach)
		(*myVm)->DetachCurrentThread(myVm);

	return result;
}

int acodecs_is_supported(int format, int is_video, int is_sw_allowed)
{
	int i;
	const char *types[4] = { NULL, NULL, NULL, NULL };

	DBG serprintf("acodecs_is_supported(%d, is_video %d)\n", format, is_video);
	if (is_video) {
		switch (format) {
		case VIDEO_FORMAT_MPEG:
			types[0] = "video/mpeg2";
			break;
		case VIDEO_FORMAT_WMV1:
		case VIDEO_FORMAT_WMV2:
		case VIDEO_FORMAT_WMV3:
		case VIDEO_FORMAT_WMV3B:
		case VIDEO_FORMAT_VC1:
			types[0] = "video/x-ms-wmv";
			types[1] = "video/wmv";
			types[2] = "video/wmv9";
			break;
		case VIDEO_FORMAT_HEVC:
			types[0] = "video/hevc";
			break;
		}
	} else {
		switch (format) {
		case WAVE_FORMAT_MSAUDIO_LOSSLESS:
		case WAVE_FORMAT_MSAUDIO1:
		case WAVE_FORMAT_MSAUDIO2:
		case WAVE_FORMAT_MSAUDIO3:
		case WAVE_FORMAT_MSAUDIO_SPEECH:
			types[0] = "audio/x-ms-wma";
			types[1] = "audio/wma";
			types[2] = "audio/wmapro";
			break;
		case WAVE_FORMAT_DTS:
		case WAVE_FORMAT_DTS_HD:
		case WAVE_FORMAT_MS_DTS:
			types[0] = "audio/dts";
			break;
		case WAVE_FORMAT_OPUS:
			types[0] = "audio/opus";
			break;
		}
	}

	i = 0;
	if (types[0]) {
		while (types[i] != NULL) {
			if (acodecs_is_type_supported(types[i], is_sw_allowed)) {
				DBG serprintf("acodecs_is_supported(%d, is_video %d) YES!\n", format, is_video);
				return 1;
			}
			++i;
		}
		return 0;
	}
	return 1;
}

#ifdef DEBUG_MSG
static void dump_acodecs()
{
	int num_codecs, i;
	const void *mcl;

	if (dlhelper_mcl_init()) {
		serprintf("dlhelper_mcl_init: FAIL\n");
		return;
	}

	serprintf("android codecs (from /system/etc/media_codecs.xml):\n");
	mcl = dl_mcl.getInstance();
	num_codecs = dl_mcl.countCodecs(mcl);
	serprintf("num_codecs: %d\n", num_codecs);
	for (i = 0; i < num_codecs; ++i) {
		serprintf("\t%s\n", dl_mcl.getCodecName(mcl, i));
	}
}

static void test_acodecs (int argc, char *argv[] ) 
{
	if( argc > 1 ) {
		int ret = acodecs_is_type_supported( argv[1], 1);
	serprintf("%s is supported: %d\n", argv[1], ret);
	}
}

DECLARE_DEBUG_COMMAND_VOID("acodecs", dump_acodecs);
DECLARE_DEBUG_COMMAND_VOID("acodecst", test_acodecs);
#endif
