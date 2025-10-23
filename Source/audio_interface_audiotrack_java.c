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
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <math.h>

#include "global.h"
#include "debug.h"
#include "types.h"
#include "audio_interface.h"
#include "android_audio.h"
#include "device_config.h"
#include "av.h"
#include "atime.h"
#include "util.h"

extern int get_hdmi_supports_iec_8ch192khz(void);
extern int get_hdmi_supports_iec(void);
#include "jni.h"

#define DBG  if(0)
#define DBG2 if(0)
#define ERR  if(1)

#define LOG(fmt, ...) do { serprintf("%s(%p): " fmt "\n", __FUNCTION__, at, ##__VA_ARGS__); } while (0)

#ifndef AUDIO_USAGE_MEDIA
#define AUDIO_USAGE_MEDIA 1
#endif

#ifndef AUDIO_CONTENT_TYPE_MUSIC
#define AUDIO_CONTENT_TYPE_MUSIC 2
#endif

#ifndef AUDIO_CONTENT_TYPE_MOVIE
#define AUDIO_CONTENT_TYPE_MOVIE 3
#endif

typedef unsigned char bool;

#define NO_ERROR 0

extern JavaVM *myVm;
extern jobject myClassLoader;
extern jmethodID myFindClassMethod;

struct audio_ctx {
	int init;
	int rate;
	int format;
	int frame_count;
	size_t frame_size;
	int channel_count;
	uint32_t latency;
	int passthrough;
	JNIEnv * env;
	int willDetach;
	jobject obj;
	jbyteArray jbuffer;
	size_t buf_size;
	jclass audiotrackClass;
	jclass audiosystemClass;
	jclass playbackParamsClass;
	jclass audioAttributesBuilderClass;
	jclass audioFormatBuilderClass;
	uint64_t i_samples_written; // Total samples written to AudioTrack (for dynamic latency tracking)
	jclass audioTimestampClass;
	jobject audioTimestamp;
	int64_t last_timestamp_ns;
	uint64_t last_timestamp_frames;
	uint64_t timestamp_written_offset;
	jmethodID getTimestampMethodID;
	jfieldID framePositionFieldID;
	jfieldID nanoTimeFieldID;
};

static char * AUDIOTRACK_CLASS_NAME = "android/media/AudioTrack";
static char * AUDIOSYSTEM_CLASS_NAME = "android/media/AudioSystem";
static char * PLAYBACKPARAMS_CLASS_NAME = "android/media/PlaybackParams";
static char * AUDIOATTRIBUTES_BUILDER_CLASS_NAME = "android/media/AudioAttributes$Builder";
static char * AUDIOFORMAT_BUILDER_CLASS_NAME = "android/media/AudioFormat$Builder";
static char * AUDIOTIMESTAMP_CLASS_NAME = "android/media/AudioTimestamp";

static int buffer_scale = 1;

static int streamType = 3; /*STREAM_MUSIC*/
static int mode = 1; /*MODE_STREAM*/

static int audio_rate = 1; /* called from stream_sink_audio represents s->audio->samplesPerSec */

static inline void call_void_method(audio_ctx_t *at, const char * name, const char * signature)
{
	DBG2 LOG();
	jmethodID method = (*at->env)->GetMethodID(at->env, at->audiotrackClass, name, signature);

	// Check if method exists - if not, clear exception and return
	if (method == NULL || (*at->env)->ExceptionCheck(at->env)) {
		if ((*at->env)->ExceptionCheck(at->env)) {
			(*at->env)->ExceptionClear(at->env);
		}
		DBG2 LOG("method '%s' not found", name);
		return;
	}

	(*at->env)->CallVoidMethod(at->env, at->obj, method);

	jthrowable exception = (*at->env)->ExceptionOccurred(at->env);
	if (exception) {
		ERR LOG("!!!EXCEPTION");
		(*at->env)->ExceptionDescribe(at->env);
		(*at->env)->ExceptionClear(at->env);
	}
}

static inline int call_int_method(audio_ctx_t *at, const char * name, const char * signature, ...)
{
	DBG2 LOG();
	jmethodID method = (*at->env)->GetMethodID(at->env, at->audiotrackClass, name, signature);

	// Check if method exists - if not, clear exception and return 0
	if (method == NULL || (*at->env)->ExceptionCheck(at->env)) {
		if ((*at->env)->ExceptionCheck(at->env)) {
			(*at->env)->ExceptionClear(at->env);
		}
		DBG2 LOG("method '%s' not found", name);
		return 0;
	}

	va_list args;
	va_start(args, signature);
	jint result = (*at->env)->CallIntMethodV(at->env, at->obj, method, args);
	va_end(args);

	jthrowable exception = (*at->env)->ExceptionOccurred(at->env);
	if (exception) {
		ERR LOG("!!!EXCEPTION: call_int_method");
		(*at->env)->ExceptionDescribe(at->env);
		(*at->env)->ExceptionClear(at->env);
	}

	return result;
}

static inline int call_int_method_with_env(audio_ctx_t *at, JNIEnv *env, const char * name, const char * signature, ...)
{
	DBG2 LOG();
	jmethodID method = (*env)->GetMethodID(env, at->audiotrackClass, name, signature);

	// Check if method exists - if not, clear exception and return 0
	if (method == NULL || (*env)->ExceptionCheck(env)) {
		if ((*env)->ExceptionCheck(env)) {
			(*env)->ExceptionClear(env);
		}
		DBG2 LOG("method '%s' not found", name);
		return 0;
	}

	va_list args;
	va_start(args, signature);
	jint result = (*env)->CallIntMethodV(env, at->obj, method, args);
	va_end(args);

	jthrowable exception = (*env)->ExceptionOccurred(env);
	if (exception) {
		ERR LOG("!!!EXCEPTION: call_int_method_with_env");
		(*env)->ExceptionDescribe(env);
		(*env)->ExceptionClear(env);
	}

	return result;
}

static inline int call_static_int_method(audio_ctx_t *at, jclass clas, const char * name, const char * signature, ...)
{
	DBG2 LOG();
	jmethodID method = (*at->env)->GetStaticMethodID(at->env, clas, name, signature);

	// Check if method exists - if not, clear exception and return 0
	if (method == NULL || (*at->env)->ExceptionCheck(at->env)) {
		if ((*at->env)->ExceptionCheck(at->env)) {
			(*at->env)->ExceptionClear(at->env);
		}
		DBG2 LOG("static method '%s' not found", name);
		return 0;
	}

	va_list args;
	va_start(args, signature);
	jint result = (*at->env)->CallStaticIntMethodV(at->env, clas, method, args);
	va_end(args);

	jthrowable exception = (*at->env)->ExceptionOccurred(at->env);
	if (exception) {
		ERR LOG("!!!EXCEPTION: call_static_int_method");
		(*at->env)->ExceptionDescribe(at->env);
		(*at->env)->ExceptionClear(at->env);
	}

	return result;
}

static inline int call_int_method_current_vm(JNIEnv *jni_env, jclass clas, const char * name, const char * signature, ...)
{
	jmethodID method = (*jni_env)->GetStaticMethodID(jni_env, clas, name, signature);

	// Check if method exists - if not, clear exception and return 0
	if (method == NULL || (*jni_env)->ExceptionCheck(jni_env)) {
		if ((*jni_env)->ExceptionCheck(jni_env)) {
			(*jni_env)->ExceptionClear(jni_env);
		}
		DBG2 serprintf("static method '%s' not found\n", name);
		return 0;
	}

	va_list args;
	va_start(args, signature);
	jint result = (*jni_env)->CallStaticIntMethodV(jni_env, clas, method, args);
	va_end(args);

	jthrowable exception = (*jni_env)->ExceptionOccurred(jni_env);
	if (exception) {
		ERR serprintf("!!!EXCEPTION: call_int_method_current_vm\n");
		(*jni_env)->ExceptionDescribe(jni_env);
		(*jni_env)->ExceptionClear(jni_env);
	}

	return result;
}

static inline void attach_thread(audio_ctx_t *at) {
	(*myVm)->AttachCurrentThread(myVm, &(at->env), NULL);
}

static inline JNIEnv * attach_thread_current_vm() {
	JNIEnv *myEnv = NULL;
	if ((*myVm)->GetEnv(myVm, (void**)&(myEnv), JNI_VERSION_1_4) != JNI_OK) {
		DBG serprintf("ERROR: audio_interface_audiotrack_java:attach_thread_current_vm GetEnv failed\n");
		if(((*myVm)->AttachCurrentThread(myVm, &(myEnv), NULL)) != 0 ) {
			ERR serprintf("ERROR: audio_interface_audiotrack_java:attach_thread_current_vm Attach to JVM failed\n");
			return NULL;
		} else {
			return myEnv;
		}
	} else {
		return myEnv;
	}
}

static int audiotrack_init(void)
{
	return 0;
}

static void audiotrack_exit(void)
{
}

static audio_ctx_t *audiotrack_open(int mode)
{
	audio_ctx_t *at;

	if (mode != AUDIO_OUTPUT_MODE)
		return NULL;

	at = (audio_ctx_t *)calloc(1, sizeof(audio_ctx_t));
	if (!at) {
		ERR LOG("malloc failed");
		return NULL;
	}

	DBG	LOG("mode: %i", mode);

	//let's attach to the java VM
	if ((*myVm)->GetEnv(myVm, (void**)&(at->env), JNI_VERSION_1_4) != JNI_OK) {
		DBG LOG("ERROR: audio_interface_audiotrack_java:audiotrack_open GetEnv failed");
		if(((*myVm)->AttachCurrentThread(myVm, &(at->env), NULL)) != 0 ) {
			ERR LOG("ERROR: Attach to JVM failed");
			return 0;
		}
		else
			at->willDetach = 1;
	}

	at->audiotrackClass = (*at->env)->NewGlobalRef(at->env, (*at->env)->FindClass(at->env, AUDIOTRACK_CLASS_NAME));
	at->audiosystemClass = (*at->env)->NewGlobalRef(at->env, (*at->env)->FindClass(at->env, AUDIOSYSTEM_CLASS_NAME));
	at->playbackParamsClass = (*at->env)->NewGlobalRef(at->env, (*at->env)->FindClass(at->env, PLAYBACKPARAMS_CLASS_NAME));
	at->audioAttributesBuilderClass = (*at->env)->NewGlobalRef(at->env, (*at->env)->FindClass(at->env, AUDIOATTRIBUTES_BUILDER_CLASS_NAME));
	at->audioFormatBuilderClass = (*at->env)->NewGlobalRef(at->env, (*at->env)->FindClass(at->env, AUDIOFORMAT_BUILDER_CLASS_NAME));
	at->audioTimestampClass = (*at->env)->NewGlobalRef(at->env, (*at->env)->FindClass(at->env, AUDIOTIMESTAMP_CLASS_NAME));

	// Create AudioTimestamp object for getTimestamp() calls (API 19+)
	// and cache method/field IDs for later use
	at->getTimestampMethodID = NULL;
	at->framePositionFieldID = NULL;
	at->nanoTimeFieldID = NULL;
	at->audioTimestamp = NULL;

	if (at->audioTimestampClass) {
		// Cache the getTimestamp method ID
		at->getTimestampMethodID = (*at->env)->GetMethodID(at->env, at->audiotrackClass,
			"getTimestamp", "(Landroid/media/AudioTimestamp;)Z");
		if ((*at->env)->ExceptionCheck(at->env)) {
			(*at->env)->ExceptionClear(at->env);
			at->getTimestampMethodID = NULL;
		}

		// Cache field IDs for AudioTimestamp
		at->framePositionFieldID = (*at->env)->GetFieldID(at->env, at->audioTimestampClass, "framePosition", "J");
		if ((*at->env)->ExceptionCheck(at->env)) {
			(*at->env)->ExceptionClear(at->env);
			at->framePositionFieldID = NULL;
		}

		at->nanoTimeFieldID = (*at->env)->GetFieldID(at->env, at->audioTimestampClass, "nanoTime", "J");
		if ((*at->env)->ExceptionCheck(at->env)) {
			(*at->env)->ExceptionClear(at->env);
			at->nanoTimeFieldID = NULL;
		}

		// Create AudioTimestamp instance if we have all the required IDs
		if (at->getTimestampMethodID && at->framePositionFieldID && at->nanoTimeFieldID) {
			jmethodID audioTimestampCtor = (*at->env)->GetMethodID(at->env, at->audioTimestampClass, "<init>", "()V");
			if (audioTimestampCtor) {
				jobject localTimestamp = (*at->env)->NewObject(at->env, at->audioTimestampClass, audioTimestampCtor);
				if (localTimestamp) {
					at->audioTimestamp = (*at->env)->NewGlobalRef(at->env, localTimestamp);
					(*at->env)->DeleteLocalRef(at->env, localTimestamp);
					DBG LOG("AudioTimestamp support initialized successfully");
				}
			}
		}
	}

	return at;
}

static int audiotrack_close(audio_ctx_t **pat)
{
	audio_ctx_t *at = *pat;

	if (at->init) {
		attach_thread(at);
		int underrun_count = call_int_method(at, "getUnderrunCount", "()I");
		if (underrun_count > 0)
			ERR LOG("Underrun count: %d", underrun_count);

		call_void_method(at, "release", "()V");
		(*at->env)->DeleteGlobalRef(at->env, at->obj);
		(*at->env)->DeleteGlobalRef(at->env, at->jbuffer);
		(*at->env)->DeleteGlobalRef(at->env, at->audiotrackClass);
		(*at->env)->DeleteGlobalRef(at->env, at->audiosystemClass);
		(*at->env)->DeleteGlobalRef(at->env, at->playbackParamsClass);
		(*at->env)->DeleteGlobalRef(at->env, at->audioAttributesBuilderClass);
		(*at->env)->DeleteGlobalRef(at->env, at->audioFormatBuilderClass);
		if (at->audioTimestamp)
			(*at->env)->DeleteGlobalRef(at->env, at->audioTimestamp);
		if (at->audioTimestampClass)
			(*at->env)->DeleteGlobalRef(at->env, at->audioTimestampClass);
		//if (at->willDetach)
		//	(*myVm)->DetachCurrentThread(myVm);
		at->init = 0;
	}
	free(at);
	pat = NULL;
	return 0;
}

static void audiotrack_update_latency(audio_ctx_t *at, JNIEnv *env)
{
	if (!at || !env) return;

	float speed = get_effective_audio_speed();

	// AudioTrack.getLatency() available on API 29+ (returns 0 if method doesn't exist)
	uint32_t track_latency = call_int_method_with_env(at, env, "getLatency", "()I");

	// Fallback: AudioSystem.getOutputLatency() + manual buffer calculation
	uint32_t system_latency = call_int_method_current_vm( env, at->audiosystemClass, "getOutputLatency", "(I)I", streamType );
	uint32_t app_latency = (uint32_t)lrint( ( 1000.0 * (double)at->buf_size ) / ( (double)at->frame_size * (double)at->rate * speed ) );

	DBG LOG( "audiotrack_update_latency: speed=%.2f, track_latency=%d, system_latency=%d, app_latency=%d, total_latency=%d, use track latency=%d",
			 speed, track_latency, system_latency, app_latency, system_latency + app_latency , track_latency > 0);

	if( !at->passthrough && track_latency > 0 ) {
		// AudioTrack.getLatency() by default (API 29+) when not in passthrough mode since it induces a delay
		at->latency = track_latency;
	} else {
		// Fallback: AudioSystem.getOutputLatency() + manual buffer calculation (used for passthrough or when track_latency unavailable)
		at->latency = system_latency + app_latency;
	}
}

static int audiotrack_set_output_params(audio_ctx_t *at, int rate, int channels, int bits, int format)
{
	uint32_t track_chanmask;
	audio_format_t track_format;
	int status = 0;

	float as = get_effective_audio_speed();
	int is_audio_speed_enabled = audio_interface_is_audio_speed_enabled();

	DBG LOG( "rate %d, channels %d, bits %d, format %d, passthrough mode %d, as %f", rate, channels, bits, format, at->passthrough, as );

	attach_thread( at );

	at->format = format;
	int output_channels = channels;
	switch( output_channels ) {
	case 1:
		track_chanmask = AUDIO_CHANNEL_OUT_MONO;
		break;
	case 2:
		track_chanmask = AUDIO_CHANNEL_OUT_STEREO;
		break;
	case 3:
		track_chanmask = AUDIO_CHANNEL_OUT_STEREO | AUDIO_CHANNEL_OUT_LOW_FREQUENCY;
		break;
	case 4:
		track_chanmask = AUDIO_CHANNEL_OUT_SURROUND;
		break;
	case 5:
		track_chanmask = AUDIO_CHANNEL_OUT_QUAD | AUDIO_CHANNEL_OUT_LOW_FREQUENCY;
		break;
	case 6:
		track_chanmask = AUDIO_CHANNEL_OUT_5POINT1;
		break;
	case 7:
		track_chanmask = AUDIO_CHANNEL_OUT_5POINT1 | AUDIO_CHANNEL_OUT_BACK_CENTER;
		break;
	case 8:
		track_chanmask = AUDIO_CHANNEL_OUT_7POINT1;
		break;
	default:
		track_chanmask = AUDIO_CHANNEL_OUT_STEREO;
		break;
	}

	switch( bits ) {
	case 8:
		track_format = 3; // AudioFormat.ENCODING_PCM_8BIT
		break;
	case 16:
		track_format = 2; // AudioFormat.ENCODING_PCM_16BIT
		break;
	case 24:
	case 32:
	default:
		ERR LOG( "cannot set bits %d", bits );
		return -1;
	}
	size_t frame_size = 0;

	if( at->passthrough == 2 ) {
		// Keep latency math consistent with the IEC61937 container the HAL sees:
		// most compressed frames map to ~4 bytes per PCM sample equivalent.
		frame_size = 4;
		switch( at->format ) {
		case WAVE_FORMAT_AC3:
			track_format = 5; // AudioFormat.ENCODING_AC3;
			break;
		case WAVE_FORMAT_EAC3:
			track_format = 6; // AudioFormat.ENCODING_E_AC3;
			break;
		case WAVE_FORMAT_DTS:
			track_format = 7; // AudioFormat.ENCODING_DTS;
			break;
		case WAVE_FORMAT_DTS_HD_MA:
		case WAVE_FORMAT_DTS_HD:
			track_format = 8; // AudioFormat.ENCODING_DTS_HD;
			break;
		case WAVE_FORMAT_TRUEHD:
			track_format = 14; // AudioFormat.ENCODING_DOLBY_TRUEHD;
			break;
		default:
			track_format = 13; // AudioFormat.ENCODING_IEC61937
		}
	} else if(at->passthrough == 1 && device_get_android_api() >= 24 && get_hdmi_supports_iec()) {
        track_format = 13; // AudioFormat.ENCODING_IEC61937
        switch(at->format) {
            case WAVE_FORMAT_AC3:
            case WAVE_FORMAT_DTS:
                track_chanmask = AUDIO_CHANNEL_OUT_STEREO;
                output_channels = 2;
                rate = 48000;
                break;
            case WAVE_FORMAT_EAC3:
                track_chanmask = AUDIO_CHANNEL_OUT_STEREO;
                output_channels = 2;
                rate = 192000;
                break;
            case WAVE_FORMAT_DTS_HD_MA:
            case WAVE_FORMAT_DTS_HD:
                // Note: the logic to select DTS-core vs DTS-HD needs to be identical with the one selecting dtshd_rate
                if (get_hdmi_supports_iec_8ch192khz()) {
                    track_chanmask = AUDIO_CHANNEL_OUT_7POINT1;
                    output_channels = 8;
                    rate = 192000;
                } else {
                    track_chanmask = AUDIO_CHANNEL_OUT_STEREO;
                    output_channels = 2;
                    rate = 48000;
                }
                break;
            case WAVE_FORMAT_TRUEHD:
                track_chanmask = AUDIO_CHANNEL_OUT_7POINT1;
                output_channels = 8;
                rate = 192000;
                break;
        }
    }

	if( at->passthrough != 2 ) {
		frame_size = (bits / 8) * output_channels;
	}

	audio_rate = rate;
	at->rate = rate;
	at->channel_count = output_channels;
	at->frame_size = frame_size;
	channels = output_channels;

	int reinit = 0;
	if( at->init ) {
		DBG LOG( "deleting track" );
		call_void_method( at, "release", "()V" );
		( *at->env )->DeleteGlobalRef( at->env, at->obj );
		jthrowable exception = ( *at->env )->ExceptionOccurred( at->env );
		if( exception ) {
			ERR LOG( "!!!EXCEPTION DeleteGlobalRef" );
			( *at->env )->ExceptionClear( at->env );
		}

		at->obj = NULL;
		at->init = 0;
		reinit = 1;
	}

	streamType = 3; /*STREAM_MUSIC*/
	int sampleRateInHz = rate;
	int channelConfig = track_chanmask << 2;
	int audioFormat = track_format;
	mode = 1; /*MODE_STREAM*/

	if(is_audio_speed_enabled && at->passthrough == 0 && device_get_android_api() >= 23) {
		buffer_scale = 2; // for 2.0x max audio speed
	} else {
		buffer_scale = 1;
	}
	DBG LOG( "audio_interface_audiotrack_java:audiotrack_set_output_params buffer_scale=%d", buffer_scale );

	int min_buffer_size = call_static_int_method(at, at->audiotrackClass, "getMinBufferSize", "(III)I",
			sampleRateInHz, channelConfig, audioFormat);

	if (min_buffer_size <= 0) {
		ERR LOG("getMinBufferSize returned %d, falling back to 32768", min_buffer_size);
		min_buffer_size = 32768;
	}

	if (at->passthrough == 2) {
		// For compressed passthrough, use getMinBufferSize() with a safety margin
		// Different formats have varying frame sizes (AC3 ~6KB, DTS ~2KB, TrueHD ~20KB)
		// Ensure minimum of 32KB for compatibility, but respect larger system requirements
		at->buf_size = (min_buffer_size > 32768) ? min_buffer_size : 32768;
	} else {
		// Use scaled minimum (buffer_scale=2 for audio speed support)
		// Let Android's getMinBufferSize() determine the requirements
		at->buf_size = buffer_scale * min_buffer_size;
	}

	DBG LOG ( "audio_interface_audiotrack_java:audiotrack_set_output_params getMinBufferSize=%d, final buf_size=%d\n",
		min_buffer_size, at->buf_size);

	int failed = 0;
	jobject audioTrack = NULL;
	jobject audioAttributesBuilder = NULL;
	jobject audioFormatBuilder = NULL;
	jobject audioAttributes = NULL;
	jobject audioFormatObj = NULL;
	jthrowable exception = NULL;

	// Build AudioAttributes with usage/content type matching legacy STREAM_MUSIC
	if (!failed) {
		jmethodID audioAttributesBuilderCtor = (*at->env)->GetMethodID(at->env, at->audioAttributesBuilderClass, "<init>", "()V");
		audioAttributesBuilder = (*at->env)->NewObject(at->env, at->audioAttributesBuilderClass, audioAttributesBuilderCtor);
		exception = (*at->env)->ExceptionOccurred(at->env);
		if (exception) {
			ERR LOG("exception creating AudioAttributes.Builder");
			(*at->env)->ExceptionDescribe(at->env);
			(*at->env)->ExceptionClear(at->env);
			failed = 1;
		}
	}

	if (!failed) {
		jmethodID setUsageMethod = (*at->env)->GetMethodID(at->env, at->audioAttributesBuilderClass, "setUsage", "(I)Landroid/media/AudioAttributes$Builder;");
		(*at->env)->CallObjectMethod(at->env, audioAttributesBuilder, setUsageMethod, AUDIO_USAGE_MEDIA);
		exception = (*at->env)->ExceptionOccurred(at->env);
		if (exception) {
			ERR LOG("exception during AudioAttributes.Builder.setUsage");
			(*at->env)->ExceptionDescribe(at->env);
			(*at->env)->ExceptionClear(at->env);
			failed = 1;
		}
	}

	if (!failed) {
		jmethodID setContentTypeMethod = (*at->env)->GetMethodID(at->env, at->audioAttributesBuilderClass, "setContentType", "(I)Landroid/media/AudioAttributes$Builder;");
		// Use CONTENT_TYPE_MOVIE for video player application
		(*at->env)->CallObjectMethod(at->env, audioAttributesBuilder, setContentTypeMethod, AUDIO_CONTENT_TYPE_MOVIE);
		exception = (*at->env)->ExceptionOccurred(at->env);
		if (exception) {
			ERR LOG("exception during AudioAttributes.Builder.setContentType");
			(*at->env)->ExceptionDescribe(at->env);
			(*at->env)->ExceptionClear(at->env);
			failed = 1;
		}
	}

	if (!failed) {
		jmethodID buildAttributesMethod = (*at->env)->GetMethodID(at->env, at->audioAttributesBuilderClass, "build", "()Landroid/media/AudioAttributes;");
		audioAttributes = (*at->env)->CallObjectMethod(at->env, audioAttributesBuilder, buildAttributesMethod);
		exception = (*at->env)->ExceptionOccurred(at->env);
		if (exception) {
			ERR LOG("exception during AudioAttributes.Builder.build");
			(*at->env)->ExceptionDescribe(at->env);
			(*at->env)->ExceptionClear(at->env);
			failed = 1;
		}
	}

	// Build AudioFormat describing the PCM/compressed stream
	if (!failed) {
		jmethodID audioFormatBuilderCtor = (*at->env)->GetMethodID(at->env, at->audioFormatBuilderClass, "<init>", "()V");
		audioFormatBuilder = (*at->env)->NewObject(at->env, at->audioFormatBuilderClass, audioFormatBuilderCtor);
		exception = (*at->env)->ExceptionOccurred(at->env);
		if (exception) {
			ERR LOG("exception creating AudioFormat.Builder");
			(*at->env)->ExceptionDescribe(at->env);
			(*at->env)->ExceptionClear(at->env);
			failed = 1;
		}
	}

	if (!failed) {
		jmethodID setSampleRateMethod = (*at->env)->GetMethodID(at->env, at->audioFormatBuilderClass, "setSampleRate", "(I)Landroid/media/AudioFormat$Builder;");
		(*at->env)->CallObjectMethod(at->env, audioFormatBuilder, setSampleRateMethod, sampleRateInHz);
		exception = (*at->env)->ExceptionOccurred(at->env);
		if (exception) {
			ERR LOG("exception during AudioFormat.Builder.setSampleRate");
			(*at->env)->ExceptionDescribe(at->env);
			(*at->env)->ExceptionClear(at->env);
			failed = 1;
		}
	}

	if (!failed) {
		jmethodID setChannelMaskMethod = (*at->env)->GetMethodID(at->env, at->audioFormatBuilderClass, "setChannelMask", "(I)Landroid/media/AudioFormat$Builder;");
		(*at->env)->CallObjectMethod(at->env, audioFormatBuilder, setChannelMaskMethod, channelConfig);
		exception = (*at->env)->ExceptionOccurred(at->env);
		if (exception) {
			ERR LOG("exception during AudioFormat.Builder.setChannelMask");
			(*at->env)->ExceptionDescribe(at->env);
			(*at->env)->ExceptionClear(at->env);
			failed = 1;
		}
	}

	if (!failed) {
		jmethodID setEncodingMethod = (*at->env)->GetMethodID(at->env, at->audioFormatBuilderClass, "setEncoding", "(I)Landroid/media/AudioFormat$Builder;");
		(*at->env)->CallObjectMethod(at->env, audioFormatBuilder, setEncodingMethod, audioFormat);
		exception = (*at->env)->ExceptionOccurred(at->env);
		if (exception) {
			ERR LOG("exception during AudioFormat.Builder.setEncoding");
			(*at->env)->ExceptionDescribe(at->env);
			(*at->env)->ExceptionClear(at->env);
			failed = 1;
		}
	}

	if (!failed) {
		jmethodID buildFormatMethod = (*at->env)->GetMethodID(at->env, at->audioFormatBuilderClass, "build", "()Landroid/media/AudioFormat;");
		audioFormatObj = (*at->env)->CallObjectMethod(at->env, audioFormatBuilder, buildFormatMethod);
		exception = (*at->env)->ExceptionOccurred(at->env);
		if (exception) {
			ERR LOG("exception during AudioFormat.Builder.build");
			(*at->env)->ExceptionDescribe(at->env);
			(*at->env)->ExceptionClear(at->env);
			failed = 1;
		}
	}

	// audioTrack will be saved in at->obj if created correctly
	if (!failed) {
		jmethodID audioTrackCtor = (*at->env)->GetMethodID(at->env, at->audiotrackClass, "<init>", "(Landroid/media/AudioAttributes;Landroid/media/AudioFormat;III)V");
		audioTrack = (*at->env)->NewObject(at->env, at->audiotrackClass, audioTrackCtor,
			audioAttributes, audioFormatObj, at->buf_size, mode, 0);
		exception = (*at->env)->ExceptionOccurred(at->env);
		if (exception) {
			ERR LOG("exception during AudioTrack(AudioAttributes, AudioFormat, bufferSize, mode, session) constructor call");
			(*at->env)->ExceptionDescribe(at->env);
			(*at->env)->ExceptionClear(at->env);
			failed = 1;
		}
	}

	if (audioAttributesBuilder) {
		(*at->env)->DeleteLocalRef(at->env, audioAttributesBuilder);
	}
	if (audioFormatBuilder) {
		(*at->env)->DeleteLocalRef(at->env, audioFormatBuilder);
	}
	if (audioAttributes) {
		(*at->env)->DeleteLocalRef(at->env, audioAttributes);
	}
	if (audioFormatObj) {
		(*at->env)->DeleteLocalRef(at->env, audioFormatObj);
	}

	jobject playbackParams;

	if(!failed && is_audio_speed_enabled && at->passthrough == 0 && device_get_android_api() >= 23 && fabsf(as - 1.0f) > 1e-6f) { // adapt audio_speed only when passthrough disabled and audio_speed != 1.0
		DBG LOG( "audio_interface_audiotrack_java:audiotrack_set_output_params audio_speed=%f (ENTERING speed set block)", as);
		// get current audioparams
		DBG LOG( "audio_interface_audiotrack_java:audiotrack_set_output_params calling getPlaybackParams");
		playbackParams = (*at->env)->CallObjectMethod(at->env, audioTrack,
			(*at->env)->GetMethodID(at->env, at->audiotrackClass, "getPlaybackParams", "()Landroid/media/PlaybackParams;"));
		DBG LOG( "audio_interface_audiotrack_java:audiotrack_set_output_params getPlaybackParams returned, calling setSpeed");

		// change that audioparam's speed
		(*at->env)->CallObjectMethod(at->env, playbackParams,
				(*at->env)->GetMethodID(at->env, at->playbackParamsClass, "setSpeed", "(F)Landroid/media/PlaybackParams;"), as);
		DBG LOG( "audio_interface_audiotrack_java:audiotrack_set_output_params setSpeed completed");

		exception = (*at->env)->ExceptionOccurred(at->env);
		if (exception) { // not failing
			ERR LOG("exception during setSpeed");
			(*at->env)->ExceptionDescribe(at->env);
			(*at->env)->ExceptionClear(at->env);
			audio_interface_set_audio_speed(1.0f); // if failing then revert to 1x ratio
		}

		// set audiotrack's audioparams
		DBG LOG( "audio_interface_audiotrack_java:audiotrack_set_output_params calling setPlaybackParams");
		(*at->env)->CallVoidMethod(at->env, audioTrack,
				(*at->env)->GetMethodID(at->env, at->audiotrackClass, "setPlaybackParams", "(Landroid/media/PlaybackParams;)V"), playbackParams);
		DBG LOG( "audio_interface_audiotrack_java:audiotrack_set_output_params setPlaybackParams completed");

		// Check for exception after setPlaybackParams
		exception = (*at->env)->ExceptionOccurred(at->env);
		if (exception) {
			ERR LOG("exception during setPlaybackParams - buffer may be too small for requested speed");
			(*at->env)->ExceptionDescribe(at->env);
			(*at->env)->ExceptionClear(at->env);
			failed = 1;
			audio_interface_set_audio_speed(1.0f); // revert to 1x ratio
		} else {
			DBG LOG( "audio_interface_audiotrack_java:audiotrack_set_output_params setPlaybackParams SUCCESS - audio speed should be %.2fx", as);
		}
	} else {
		DBG LOG( "audio_interface_audiotrack_java:audiotrack_set_output_params not applying setSpeed on AudioTrack PlaybackParams (failed=%d, is_enabled=%d, passthrough=%d, api=%d, speed_diff=%.3f)",
			failed, is_audio_speed_enabled, at->passthrough, device_get_android_api(), fabsf(as - 1.0f));
	}

	if (!failed) {
		at->obj = (*at->env)->NewGlobalRef(at->env, audioTrack);

		status = call_int_method(at, "getState", "()I");
		if (status != 1) { // STATE_INITIALIZED is 1 ; 0 for uninit
			ERR LOG("audiotrack ctor failed");
			failed = 1;
			// If DTS HD failed, fallback to DTS for DTS core mode
			if (at->format == WAVE_FORMAT_DTS_HD || at->format == WAVE_FORMAT_DTS_HD_MA) {
				return audiotrack_set_output_params(at, 48000, 2, 16, WAVE_FORMAT_DTS);
			}
		}

		//frame_size reported can be false for compressed formats
		at->frame_count = at->buf_size / at->frame_size; // number of frames in the buffer
		DBG LOG("len buf size %d frc %d frs %d nbChs %d", at->buf_size, at->frame_count, at->frame_size, at->channel_count);
		at->jbuffer = (jbyteArray) (*at->env)->NewGlobalRef(at->env, (*at->env)->NewByteArray(at->env, at->buf_size));
	}

	if (failed && reinit) {
		msec_sleep( 30 );
		ERR LOG("audio_interface_audiotrack_java:audiotrack_set_output_params self calls audiotrack_set_output_params\n");
		return audiotrack_set_output_params(at, rate, channels, bits, format);
	}

	if(failed)
		return -1;

	audiotrack_update_latency(at, at->env);

	at->init = 1;
	// Initialize timestamp tracking for dynamic latency calculation
	at->i_samples_written = 0;
	at->last_timestamp_ns = 0;
	at->last_timestamp_frames = 0;
	at->timestamp_written_offset = 0;
	DBG LOG("track created");

	return 0;
}

static int audiotrack_set_passthrough(audio_ctx_t *at, int passthrough)
{
	at->passthrough = passthrough;
	audiotrack_set_output_params(at, at->rate, at->channel_count, (passthrough == 2) ? 16 : at->frame_size * 8 / at->channel_count, at->format);
	return 0;
}

static int audiotrack_get_passthrough(audio_ctx_t *at)
{
	return at->passthrough;
}

static int audiotrack_start(audio_ctx_t *at)
{
DBG	LOG();
	if (!at->init) {
ERR		LOG("track not valid, error");
		return -1;
	}

	attach_thread(at);
	call_void_method(at, "play", "()V");
	return 0;
}

static int audiotrack_stop(audio_ctx_t *at)
{
DBG	LOG();
	if (!at->init) {
ERR		LOG("track not valid, error");
		return -1;
	}

	attach_thread(at);
	call_void_method(at, "stop", "()V");
	at->timestamp_written_offset = at->i_samples_written;
	at->last_timestamp_frames = 0;
	at->last_timestamp_ns = 0;
	return 0;
}

static int audiotrack_can_write(audio_ctx_t *at, int len)
{
	return 1;
}

static int audiotrack_write(audio_ctx_t *at, unsigned char *buffer, int len)
{
DBG2	LOG("len %d", len);
	if (!at->init) {
ERR		LOG("track not valid, error");
		return -1;
	}

	attach_thread(at);
	if (call_int_method(at, "getPlayState", "()I") != 3 ) /* PLAYSTATE_PLAYING */
		audiotrack_start(at);

	ssize_t ret = 0;
	ssize_t len_to_write = MIN(at->buf_size, len);
	(*at->env)->SetByteArrayRegion(at->env, at->jbuffer, 0, len_to_write, buffer);
	ret = call_int_method(at, "write", "([BII)I", at->jbuffer, 0, len_to_write);
DBG2	LOG("wrote %d out of %d", ret, len);

	// Track samples written for dynamic latency calculation
	if (ret > 0) {
		at->i_samples_written += (uint64_t)(ret / at->frame_size);
	}

	if (at->passthrough && ret == -6 /* ERROR_DEAD_OBJECT */) {
ERR		LOG("audiotrack_interface_audiotrack_java:audiotrack_write dead object -> call audiotrack_set_output_params");
		audiotrack_set_passthrough(at, at->passthrough);
	}
	return ret;
}

static int audiotrack_get_delay(audio_ctx_t *at)
{
	if (!at->init) {
ERR		LOG("track not valid, error");
		return -1;
	}

	// Use AudioTrack.getTimestamp() for dynamic latency calculation (API 19+)
	// This automatically accounts for Bluetooth and other output latencies
	if (!at->audioTimestamp || !at->getTimestampMethodID || !at->framePositionFieldID || !at->nanoTimeFieldID) {
		// Fallback to static latency if AudioTimestamp not available
DBG2		LOG("Using static latency: %d ms", at->latency);
		return at->latency;
	}

	if (at->rate <= 0) {
DBG2		LOG("Invalid sample rate, using static latency: %d ms", at->latency);
		return at->latency;
	}

	// IMPORTANT: Get the JNIEnv for the CURRENT thread, not the cached one
	// audiotrack_get_delay() is called from the audio thread, which is different
	// from the thread that created at->env
	JNIEnv *env = attach_thread_current_vm();
	if (!env) {
		// Can't attach to current thread, fallback to static latency
DBG2		LOG("Failed to attach to current thread, using static latency: %d ms", at->latency);
		return at->latency;
	}

	// Call AudioTrack.getTimestamp(AudioTimestamp)
	jboolean success = (*env)->CallBooleanMethod(env, at->obj, at->getTimestampMethodID, at->audioTimestamp);

	// Check for exceptions
	if ((*env)->ExceptionCheck(env)) {
		(*env)->ExceptionClear(env);
		DBG2 LOG("Exception in getTimestamp, using static latency: %d ms", at->latency);
		return at->latency;
	}

	if (!success) {
		// getTimestamp failed (can happen during warmup or if not supported), use static latency
DBG2		LOG("getTimestamp returned false, using static latency: %d ms", at->latency);
		return at->latency;
	}

	// Extract framePosition and nanoTime from AudioTimestamp using cached field IDs
	int64_t framePosition = (*env)->GetLongField(env, at->audioTimestamp, at->framePositionFieldID);
	int64_t nanoTime = (*env)->GetLongField(env, at->audioTimestamp, at->nanoTimeFieldID);

	if (framePosition < 0) {
DBG2		LOG("Negative frame position, using static latency: %d ms", at->latency);
		return at->latency;
	}

	uint64_t frames_presented = (uint64_t)framePosition;

	// Detect playback head resets (stop/flush) or wrap-around and realign counters
	if (frames_presented < at->last_timestamp_frames) {
		uint64_t new_offset = 0;
		if (at->i_samples_written > frames_presented) {
			new_offset = at->i_samples_written - frames_presented;
		}
		at->timestamp_written_offset = new_offset;
DBG2		LOG("Timestamp reset detected, offset=%llu", (unsigned long long)at->timestamp_written_offset);
	}

	uint64_t frames_written_adjusted = 0;
	if (at->i_samples_written > at->timestamp_written_offset) {
		frames_written_adjusted = at->i_samples_written - at->timestamp_written_offset;
	}

	// Calculate delay: (samples_written - samples_presented) / sample_rate * 1000
	// Note: framePosition is the last frame that was presented
	int64_t frames_pending = (int64_t)frames_written_adjusted - (int64_t)frames_presented;

	if (frames_pending < 0) {
		// This can happen if framePosition wraps around (32-bit) or during initialization
		frames_pending = 0;
	}

	// Convert frames to milliseconds: frames / (rate / 1000) = frames * 1000 / rate
	int delay_ms = (int)((frames_pending * 1000) / at->rate);

	// Guard against unrealistic estimates (e.g., during startup) and fallback to static latency
	if (delay_ms < 0 || delay_ms > 2000) {
DBG2		LOG("Dynamic latency %d ms out of range, fallback to static: %d ms", delay_ms, at->latency);
		delay_ms = at->latency;
	}

	// Cache the timestamp for debugging/monitoring
	at->last_timestamp_ns = nanoTime;
	at->last_timestamp_frames = frames_presented;

DBG2	LOG("Dynamic latency: %d ms (written: %llu, presented: %llu, pending: %lld frames)",
		delay_ms, (unsigned long long)at->i_samples_written, (unsigned long long)frames_presented, (long long)frames_pending);

	return delay_ms;
}

static void audiotrack_flush_output(audio_ctx_t *at)
{
DBG	LOG();
	if (!at->init) {
ERR		LOG("track not valid, error");
		return;
	}

	attach_thread(at);
	call_void_method(at, "pause", "()V");
	call_void_method(at, "flush", "()V");

	// Reset timestamp tracking after flush
	at->i_samples_written = 0;
	at->last_timestamp_ns = 0;
	at->last_timestamp_frames = 0;
	at->timestamp_written_offset = 0;
}

static int audiotrack_preload(audio_ctx_t *at)
{
	unsigned char *buffer;
	size_t len;
	ssize_t ret;

DBG	LOG();
	if (!at->init) {
ERR		LOG("track not valid, error");
		return -1;
	}

	// since at->frame_count = at->buf_size / at->frame_size; simplify
	// original: len = at->frame_count * at->frame_size * ((at->passthrough == 2) ? 4 : at->channel_count);
	len = at->buf_size * ( ( at->passthrough == 2 ) ? 4 : at->channel_count );

	if ((buffer = (unsigned char *)malloc(len)) == NULL)
		return -1;
	memset(buffer, 0, len);
	ret = audio_interface_write(at, buffer, len);
	free(buffer);
	if (ret <= 0) {
ERR		LOG("preload failed\n");
		return -1;
	}

DBG	LOG("preloaded %zu bytes\n", ret);
	return ret;
}

static int audiotrack_get_session_id(audio_ctx_t *at)
{
DBG	LOG();
	//TODO 
	return -1;
}

static int audiotrack_change_audio_speed(audio_ctx_t *at, float speed)
{
	if(audio_interface_is_audio_speed_enabled() && at->passthrough == 0 && device_get_android_api() >= 23) { // adapt audio_speed only when passthrough disabled and API23+
DBG	LOG("audio_interface_audiotrack_java:audiotrack_change_audio_speed speed=%f", speed);

		JNIEnv *myEnv = attach_thread_current_vm();
		if (*myEnv == NULL) return 0;

		DBG LOG( "audio_interface_audiotrack_java:audiotrack_change_audio_speed attached to current thread" );

		// reuse already created audioTrack
		jobject audioTrack = at->obj;

		// get current audioparams
		jobject playbackParams =
			( *myEnv )
				->CallObjectMethod( myEnv, audioTrack,
									( *myEnv )
										->GetMethodID( myEnv, at->audiotrackClass, "getPlaybackParams",
													   "()Landroid/media/PlaybackParams;" ) );

		DBG LOG( "audio_interface_audiotrack_java:audiotrack_change_audio_speed playbackparams fetched" );

		// change that audioparam's speed
		( *myEnv )
			->CallObjectMethod(
				myEnv, playbackParams,
				( *myEnv )
					->GetMethodID( myEnv, at->playbackParamsClass, "setSpeed", "(F)Landroid/media/PlaybackParams;" ),
				speed );

		DBG LOG( "audio_interface_audiotrack_java:audiotrack_change_audio_speed setspeed done" );

		int failed = 0;
		jthrowable exception = ( *myEnv )->ExceptionOccurred( myEnv );
		if( exception ) {
			ERR LOG( "audio_interface_audiotrack_java:audiotrack_change_audio_speed exception during setSpeed call" );
			( *myEnv )->ExceptionDescribe( myEnv );
			( *myEnv )->ExceptionClear( myEnv );
			failed = 1;
		}

		// set audiotrack's audioparams
		( *myEnv )
			->CallVoidMethod( myEnv, audioTrack,
							  ( *myEnv )
								  ->GetMethodID( myEnv, at->audiotrackClass, "setPlaybackParams",
												 "(Landroid/media/PlaybackParams;)V" ),
							  playbackParams );

		DBG LOG( "audio_interface_audiotrack_java:audiotrack_change_audio_speed audioparams set" );

		int status =
			( *myEnv ) ->CallIntMethod( myEnv, audioTrack,
									   ( *myEnv ) ->GetMethodID( myEnv, at->audiotrackClass, "getState", "()I" ) );
		if( status != 1 ) { // STATE_INITIALIZED is 1 ; 0 for uninit
			failed = 1;
		}

	 	DBG LOG( "audio_interface_audiotrack_java:audiotrack_change_audio_speed getstate %d",status );

		if( failed ) {
			ERR LOG( "audio_interface_audiotrack_java:audiotrack_change_audio_speed audiotrack change params failed: reverting to 1x" );
			audio_interface_set_audio_speed(1.0f);
		} else {
			DBG LOG( "audio_interface_audiotrack_java:audiotrack_change_audio_speed audio speed changed" );
			audio_interface_set_audio_speed(speed);
		}

		audiotrack_update_latency(at, myEnv);
	} else {
		DBG LOG("audio_interface_audiotrack_java:audiotrack_change_audio_speed no change in audio_speed in passthrough");
	}
	return 0;
}

const audio_interface_impl_t audio_interface_impl_audiotrack_java = {
	.name = "audiotrack_java",
	.init = audiotrack_init,
	.exit = audiotrack_exit,
	.open = audiotrack_open,
	.close = audiotrack_close,
	.start = audiotrack_start,
	.stop = audiotrack_stop,
	.can_write = audiotrack_can_write,
	.write = audiotrack_write,
	.set_output_params = audiotrack_set_output_params,
	.get_delay = audiotrack_get_delay,
	.flush_output = audiotrack_flush_output,
	.preload = audiotrack_preload,
	.get_session_id = audiotrack_get_session_id,
	.set_passthrough = audiotrack_set_passthrough,
	.get_passthrough = audiotrack_get_passthrough,
	.change_audio_speed = audiotrack_change_audio_speed,
};
