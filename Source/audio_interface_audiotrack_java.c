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

#include "global.h"
#include "debug.h"
#include "types.h"
#include "audio_interface.h"
#include "android_audio.h"
#include "device_config.h"
#include "av.h"
#include "atime.h"
#include "util.h"

#include "jni.h"

#define DBG  if(0)
#define DBG2 if(0)
#define ERR  if(1)

#define LOG(fmt, ...) do { serprintf("%s(%p): " fmt "\n", __FUNCTION__, at, ##__VA_ARGS__); } while (0)

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
};

static char * AUDIOTRACK_CLASS_NAME = "android/media/AudioTrack";
static char * AUDIOSYSTEM_CLASS_NAME = "android/media/AudioSystem";

static inline void call_void_method(audio_ctx_t *at, const char * name, const char * signature)
{
DBG2	LOG();
	jmethodID method = (*at->env)->GetMethodID(at->env, at->audiotrackClass, name, signature);
	(*at->env)->CallVoidMethod(at->env, at->obj, method);

	jthrowable exception = (*at->env)->ExceptionOccurred(at->env);
	if (exception) {
ERR		LOG("!!!EXCEPTION");
		(*at->env)->ExceptionClear(at->env);
	}
}

static inline int call_int_method(audio_ctx_t *at, const char * name, const char * signature, ...)
{
DBG2	LOG();
	jmethodID method = (*at->env)->GetMethodID(at->env, at->audiotrackClass, name, signature);
	va_list args;
	va_start(args, signature);
	jint result = (*at->env)->CallIntMethodV(at->env, at->obj, method, args);
	va_end(args);

	jthrowable exception = (*at->env)->ExceptionOccurred(at->env);
	if (exception) {
ERR		LOG("!!!EXCEPTION");
		(*at->env)->ExceptionClear(at->env);
	}

	return result;
}


static inline int call_static_int_method(audio_ctx_t *at, jclass clas, const char * name, const char * signature, ...)
{
DBG2	LOG();
	jmethodID method = (*at->env)->GetStaticMethodID(at->env, clas, name, signature);
	va_list args;
	va_start(args, signature);
	jint result = (*at->env)->CallStaticIntMethodV(at->env, clas, method, args);
	va_end(args);

	jthrowable exception = (*at->env)->ExceptionOccurred(at->env);
	if (exception) {
ERR	LOG("!!!EXCEPTION");
		(*at->env)->ExceptionClear(at->env);
	}

	return result;
}

static inline void attach_thread(audio_ctx_t *at) {
	(*myVm)->AttachCurrentThread(myVm, &(at->env), NULL);
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
ERR		LOG("malloc failed");
		return NULL;
	}

DBG	LOG("mode: %i", mode);

	//let's attach to the java VM
	if ((*myVm)->GetEnv(myVm, (void**)&(at->env), JNI_VERSION_1_4) != JNI_OK) {
DBG		LOG("ERROR: GetEnv failed");
		if(((*myVm)->AttachCurrentThread(myVm, &(at->env), NULL)) != 0 ) {
DBG			LOG("ERROR: Attach to JVM failed");
			return 0;
		}
		else
			at->willDetach = 1;
	}

	at->audiotrackClass = (*at->env)->NewGlobalRef(at->env, (*at->env)->FindClass(at->env, AUDIOTRACK_CLASS_NAME));
	at->audiosystemClass = (*at->env)->NewGlobalRef(at->env, (*at->env)->FindClass(at->env, AUDIOSYSTEM_CLASS_NAME));

	return at;
}

static int audiotrack_close(audio_ctx_t **pat)
{
	audio_ctx_t *at = *pat;

DBG	LOG();

	if (at->init) {
		call_void_method(at, "release", "()V");
		(*at->env)->DeleteGlobalRef(at->env, at->obj);
		(*at->env)->DeleteGlobalRef(at->env, at->jbuffer);
		(*at->env)->DeleteGlobalRef(at->env, at->audiotrackClass);
		(*at->env)->DeleteGlobalRef(at->env, at->audiosystemClass);
		//if (at->willDetach)
		//	(*myVm)->DetachCurrentThread(myVm);
		at->init = 0;
	}
	free(at);
	pat = NULL;
	return 0;
}

static int audiotrack_set_output_params(audio_ctx_t *at, int rate, int channels, int bits, int format)
{
	uint32_t track_chanmask;
	audio_format_t track_format;
	int status = 0;

DBG	LOG("rate %d, channels %d, bits %d, format %d, passthrough mode %d", rate, channels, bits, format, at->passthrough);

	attach_thread(at);

	at->rate = rate;
	at->channel_count = channels;
	at->format = format;
	switch (at->channel_count) {
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
		case 6:
			track_chanmask = AUDIO_CHANNEL_OUT_5POINT1;
			break;
		case 8:
			track_chanmask = AUDIO_CHANNEL_OUT_7POINT1;
			break;
		default:
			track_chanmask = AUDIO_CHANNEL_OUT_STEREO;
			break;
	}

	switch (bits) {
		case 8:
			track_format = 3; //AudioFormat.ENCODING_PCM_8BIT
			break;
		case 16:
			track_format = 2; //AudioFormat.ENCODING_PCM_16BIT
			break;
		case 24:
		case 32:
		default:
ERR			LOG("cannot set bits %d", bits);
			return -1;
	}
	at->frame_size = bits / 8 * at->channel_count;

	if(at->passthrough == 2) {
		at->frame_size = bits / 8;
		switch (at->format) {
			case WAVE_FORMAT_AC3:
				track_format = 5; //AudioFormat.ENCODING_AC3;
				break;
			case WAVE_FORMAT_EAC3:
				track_format = 6; //AudioFormat.ENCODING_E_AC3;
				break;
			case WAVE_FORMAT_DTS:
				track_format = 7; //AudioFormat.ENCODING_DTS;
				break;
			case WAVE_FORMAT_DTS_HD:
				track_format = 8; //AudioFormat.ENCODING_DTS_HD;
				break;
			case WAVE_FORMAT_TRUEHD:
				track_format = 14; //AudioFormat.ENCODING_DOLBY_TRUEHD;
				break;
			default:
				track_format = 13; //AudioFormat.ENCODING_IEC61937
		}
	}

	int reinit = 0;
	if (at->init) {
DBG		LOG("deleting track");
		call_void_method(at, "release", "()V");
		(*at->env)->DeleteGlobalRef(at->env, at->obj);
		jthrowable exception = (*at->env)->ExceptionOccurred(at->env);
		if (exception) {
ERR			LOG("!!!EXCEPTION DeleteGlobalRef");
			(*at->env)->ExceptionClear(at->env);
		}

		at->obj = NULL;
		at->init = 0;
		reinit = 1;
	}

	int streamType = 3; /*STREAM_MUSIC*/
	int sampleRateInHz = rate; 
	int channelConfig = track_chanmask << 2;
	int audioFormat = track_format;
	int mode = 1; /*MODE_STREAM*/
	at->buf_size = (at->passthrough == 2) ? 32768 : call_static_int_method(at, at->audiotrackClass, "getMinBufferSize", "(III)I",
			sampleRateInHz, channelConfig, audioFormat);

	jobject audioTrack = (*at->env)->NewObject(at->env, at->audiotrackClass,
		(*at->env)->GetMethodID(at->env, at->audiotrackClass, "<init>", "(IIIIII)V"),
		streamType, sampleRateInHz, channelConfig,
		audioFormat, at->buf_size, mode);

	int failed = 0;
	jthrowable exception = (*at->env)->ExceptionOccurred(at->env);
	if (exception) {
ERR		LOG("exception during constructor call");
		(*at->env)->ExceptionClear(at->env);
		failed = 1;
	}

	if (!failed) {
		at->obj = (*at->env)->NewGlobalRef(at->env,  audioTrack);

		status = call_int_method(at, "getState", "()I");
		if (status != 1) { // STATE_INITIALIZED is 1 ; 0 for uninit
ERR			LOG("audiotrack ctor failed");
			failed = 1;
		}
	}

	if (!failed) {
		//frame_size reported can be false for compressed formats
		at->frame_count = at->buf_size / at->frame_size;
DBG		LOG("len buf size %d frc %d frs %d nbChs %d", at->buf_size, at->frame_count, at->frame_size, at->channel_count);
		at->jbuffer = (jbyteArray) (*at->env)->NewGlobalRef(at->env, (*at->env)->NewByteArray(at->env, at->buf_size));
	}

	if (failed && reinit) {
		msec_sleep( 30 );
		return audiotrack_set_output_params(at, rate, channels, bits, format);
	}

	if(failed)
		return -1;

	at->latency = call_static_int_method(at, at->audiosystemClass, "getOutputLatency", "(I)I", streamType);
	at->latency += (1000 * at->frame_count) / at->rate;
	at->init = 1;

DBG	LOG("track created");

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
DBG	LOG("wrote %d", ret);

	if (at->passthrough && ret == -6 /* ERROR_DEAD_OBJECT */) {
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
DBG2	LOG("%d:", at->latency);
	return at->latency;
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

	len = at->frame_count * at->frame_size * ((at->passthrough == 2) ? 4 : at->channel_count);
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
};
