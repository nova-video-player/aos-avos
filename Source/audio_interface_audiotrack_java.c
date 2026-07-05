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
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <math.h>
#include <time.h>

#include "global.h"
#include "debug.h"
#include "types.h"
#include "audio_interface.h"
#include "android_audio.h"
#include "device_config.h"
#include "av.h"
#include "atime.h"
#include "util.h"
#include "ac3_recode.h"

extern int get_hdmi_supports_iec_8ch192khz(void);
extern int get_hdmi_supports_iec(void);
extern long get_hdmi_supported_audio_codecs(void);
extern int libavos_get_ac3_recoding_enabled(void);
extern int spdif_is_passthrough_on(void);
#include "jni.h"

#define DBG DBG_IF(Debug[DBG_AUD])
#define DBG2 DBG_IF(Debug[DBG_AUD] > 1)
#define DBG3 DBG_IF(Debug[DBG_AUD] > 2)
#define ERR  if(1)

#define LOG(fmt, ...) do { serprintf("%s(%p): " fmt "\n", __FUNCTION__, at, ##__VA_ARGS__); } while (0)

#define AUDIO_FORMAT_ENCODING_E_AC3_JOC 18
#define HDMI_ENCODING_AC3 5
#define HDMI_ENCODING_E_AC3 6
#define HDMI_ENCODING_E_AC3_JOC 18
#define HDMI_ENCODING_DTS 7
#define HDMI_ENCODING_DTS_HD 8
#define HDMI_ENCODING_DTS_HD_MA 29
#define HDMI_ENCODING_DOLBY_TRUEHD 14
#define HDMI_CHECK_BIT(value, position) (((value) >> (position)) & 1)

#ifndef AUDIO_USAGE_MEDIA
#define AUDIO_USAGE_MEDIA 1
#endif

#ifndef AUDIO_CONTENT_TYPE_MUSIC
#define AUDIO_CONTENT_TYPE_MUSIC 2
#endif

#ifndef AUDIO_CONTENT_TYPE_MOVIE
#define AUDIO_CONTENT_TYPE_MOVIE 3
#endif

#ifndef AUDIO_ATTRIBUTES_SPATIALIZATION_BEHAVIOR_AUTO
#define AUDIO_ATTRIBUTES_SPATIALIZATION_BEHAVIOR_AUTO 0
#endif

#ifndef AUDIO_ATTRIBUTES_SPATIALIZATION_BEHAVIOR_NEVER
#define AUDIO_ATTRIBUTES_SPATIALIZATION_BEHAVIOR_NEVER 1
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
	uint32_t latency;           // scheduler-safe latency (= app buffer geometry)
	uint32_t track_latency;     // diagnostic: AudioTrack.getLatency() (may include HAL/platform)
	uint32_t system_latency;    // diagnostic: AudioSystem.getOutputLatency()
	uint32_t app_latency;       // local buffer geometry: buf_size / (frame_size * rate * speed)
	uint32_t pipeline_latency;  // max(track, system+app): selected static delay for mode2; write-gate timeout for mode1
	int passthrough;
	int ac3_recode;                // latched at output config; do not read global recode state in timing code
	int ac3_mode2_plain_policy;    // latched with the clock policy for this playback
	int ac3_mode2_force_pipeline;  // diagnostic A/B: force pipeline latency without changing the clock
	int ac3_recode_target_stereo;  // latched encoder target: stereo uses pipeline latency
	int applied_passthrough;
	int applied_spatialization_behavior;
	JNIEnv * env;
	int willDetach;
	pthread_t attach_thread_id;
	int attach_thread_id_valid;
	jobject obj;
	jbyteArray jbuffer;
	size_t buf_size;
	jclass audiotrackClass;
	jclass audiosystemClass;
	jclass playbackParamsClass;
	jclass audioAttributesBuilderClass;
	jclass audioFormatBuilderClass;
	uint64_t i_samples_written; // Total samples written to AudioTrack (for dynamic latency tracking)
	uint64_t playhead_epoch_offset; // Raw presented-frame base after AudioTrack.flush()
	jclass audioTimestampClass;
	jobject audioTimestamp;
	int64_t last_timestamp_ns;
	uint64_t last_timestamp_frames;
	uint64_t timestamp_written_offset;
	jmethodID getTimestampMethodID;
	jfieldID framePositionFieldID;
	jfieldID nanoTimeFieldID;
	int in_error_recovery; // Flag to indicate we're in error recovery mode
	int last_underrun_count;
	int ts_success_streak;           // consecutive good AudioTrack timestamps
	int ts_use_timestamp;            // 0 until getTimestamp is proven stable
	int ts_last_query_ms;            // last time we queried AudioTrack timing
	int ts_cached_delay_ms;          // cached delay in ms
	int ts_cached_valid;             // cached delay validity
	int last_good_dynamic_delay_ms;  // last trusted dynamic delay
	int last_good_dynamic_ms;        // last time we got a trusted dynamic delay
	int last_good_dynamic_valid;     // last dynamic delay validity
	int last_fallback_delay_ms;      // last fallback delay (playhead/static) for heard-time only
	int last_fallback_ms;            // time of last fallback delay sample
	int last_delay_ret;              // last delay returned by get_delay
	int last_delay_fallback_ms;      // last fallback used in get_delay
	int last_delay_ms;               // last computed dynamic delay (pre-return)
	int last_playhead_delay_ms;      // last playhead-derived delay
	char last_delay_src[32];         // last delay source tag
	int delay_valid;                 // dynamic timestamp is stable (fallbacks may still be used)
	// Sabrina sometimes never yields stable AudioTrack timestamps; allow
	// playhead-derived delay to become "valid" after a small stability streak.
	int playhead_valid_streak;       // consecutive non-zero, advancing playhead samples
	uint64_t headpos_smooth_frames;  // smoothed playback head position
	uint64_t headpos_last_frames;    // last raw playback head position
	int headpos_smooth_valid;        // smoothed headpos validity
	int startup_hold_active;         // clamp to static latency during initial timing warmup
	int startup_hold_start_ms;       // when startup_hold was last armed (for timeout)
	int frozen_ts_streak;            // consecutive getTimestamp calls with non-advancing framePosition
	int startup_latency_log_count;   // cap initial latency diagnostics
	int startup_delay_log_count;     // cap initial get_delay diagnostics
	int delay_diag_count;            // throttling counter for aud_at_delay summary
	int delay_diag_last_ret;         // last reported return value
	int delay_diag_last_valid;       // last reported delay_valid
	int delay_diag_last_fallback;    // last reported fallback value
	int delay_diag_last_ts_use;      // last reported ts_use_timestamp
	int playbackparams_speed_rejected;       // set when HW silently ignores setPlaybackParams speed
	uint64_t can_write_last_playback_frames; // last playhead seen by passthrough can_write gate
	int can_write_stall_start_ms;            // when passthrough can_write stopped making progress
	int passthrough_can_write_blind;         // disable exact gate after proven-stuck passthrough accounting
	int passthrough_restart_after_flush;     // restart paused passthrough track on first post-flush write
	int force_recreate;                      // force set_output_params to rebuild the track even when the config is unchanged
	int track_paused;                        // AudioTrack.pause() called; a blocking write() racing it returns 0 (full paused buffer), which is not a dead track
	int passthrough_playhead_ever_advanced;  // set once playhead advances; queried by audiotrack_passthrough_playhead_advanced()
	uint64_t mode2_logical_samples;          // fakeSize/bpf samples accumulated per write, for playhead audit
	uint64_t mode2_latency_bytes_accum;       // paired cumulative compressed bytes written
	uint64_t mode2_latency_samples_accum;     // paired cumulative logical samples
	int mode2_latency_corrected;
	int mode2_audit_last_ms;                 // last mode2_playhead_audit log timestamp
};

static int audiotrack_log_underruns = 0;
static int audiotrack_disable_recovery = 1;
static int audiotrack_mode2_audit = 0;
// AC3-recode mode2 plain-policy gate (single source of truth in stream_audio.c). When on,
// AC3 recode resolved to mode2 uses app_latency instead of pipeline_latency for the static
// heard delay, paired atomically with the PTS-seeded sample clock in stream_audio.c. Only
// meaningful with that sample clock (no synthetic anchor), where the static value no longer
// cancels at startup.
extern int stream_audio_ac3_mode2_plain_policy( void );
extern int stream_audio_ac3_mode2_force_pipeline( void );

static int audiotrack_delay_from_playhead(struct audio_ctx *at, JNIEnv *env_local);
static int audiotrack_last_good_dynamic(audio_ctx_t *at, int now_ms, int *delay_out);
static int audiotrack_get_latency(audio_ctx_t *at);
static void audiotrack_reset_timing(audio_ctx_t *at);

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
static int enable_dynamic_audio_delay = 1; /* enabled by default */

static inline void call_void_method(audio_ctx_t *at, const char * name, const char * signature)
{
	DBG2 LOG();

	// Check if AudioTrack object is valid before calling methods on it
	if (!at->obj) {
		ERR LOG("AudioTrack object is NULL, cannot call method '%s'", name);
		return;
	}

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

static inline void call_void_method_with_env(audio_ctx_t *at, JNIEnv *env, const char *name, const char *signature)
{
	DBG2 LOG();

	if (!env) {
		ERR LOG("call_void_method_with_env: NULL env for method '%s'", name);
		return;
	}
	if (!at->obj) {
		ERR LOG("AudioTrack object is NULL, cannot call method '%s'", name);
		return;
	}

	jmethodID method = (*env)->GetMethodID(env, at->audiotrackClass, name, signature);
	if (method == NULL || (*env)->ExceptionCheck(env)) {
		if ((*env)->ExceptionCheck(env)) {
			(*env)->ExceptionClear(env);
		}
		DBG2 LOG("method '%s' not found", name);
		return;
	}

	(*env)->CallVoidMethod(env, at->obj, method);

	jthrowable exception = (*env)->ExceptionOccurred(env);
	if (exception) {
		ERR LOG("!!!EXCEPTION");
		(*env)->ExceptionDescribe(env);
		(*env)->ExceptionClear(env);
	}
}

static inline int call_int_method(audio_ctx_t *at, const char * name, const char * signature, ...)
{
	DBG2 LOG();

	// Check if AudioTrack object is valid before calling methods on it
	if (!at->obj) {
		ERR LOG("AudioTrack object is NULL, cannot call method '%s'", name);
		return 0;
	}

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

	// Check if AudioTrack object is valid before calling methods on it
	if (!at->obj) {
		ERR LOG("AudioTrack object is NULL, cannot call method '%s'", name);
		return 0;
	}

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

static int audiotrack_get_requested_spatialization_behavior(audio_ctx_t *at, int output_channels, int track_format)
{
	int capabilities = device_config_get_spatializer_capabilities();

	if (device_get_android_api() < 32 || at->passthrough != 0)
		return -1;

	if (track_format != 2 && track_format != 3)
		return -1;

	if (output_channels <= 2)
		return -1;

	if ((capabilities & 1) == 0 || (capabilities & (1 << 1)) == 0)
		return -1;

	if (device_config_get_spatializer_enabled())
		return AUDIO_ATTRIBUTES_SPATIALIZATION_BEHAVIOR_AUTO;

	return AUDIO_ATTRIBUTES_SPATIALIZATION_BEHAVIOR_NEVER;
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
		DBG2 serprintf("audio_interface_audiotrack_java:attach_thread_current_vm thread not attached, attaching now\n");
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
	// calloc zeroes memory, but explicitly initialize our error recovery flag
	at->in_error_recovery = 0;
	at->last_underrun_count = 0;
	at->ts_success_streak = 0;
	at->ts_use_timestamp = 0;
	at->ts_last_query_ms = 0;
	at->ts_cached_delay_ms = 0;
	at->ts_cached_valid = 0;
	at->delay_valid = 0;
	at->startup_hold_active = 1;
	at->last_fallback_delay_ms = 0;
	at->last_fallback_ms = 0;
	at->applied_passthrough = -1;
	at->applied_spatialization_behavior = -1;
	at->startup_latency_log_count = 0;
	at->startup_delay_log_count = 0;
	at->delay_diag_count = 0;
	at->delay_diag_last_ret = -1;
	at->delay_diag_last_valid = -1;
	at->delay_diag_last_fallback = -1;
	at->delay_diag_last_ts_use = -1;

	DBG	LOG("mode: %i", mode);

	//let's attach to the java VM
	if ((*myVm)->GetEnv(myVm, (void**)&(at->env), JNI_VERSION_1_4) != JNI_OK) {
		DBG2 LOG("Thread not attached to JVM, attaching now");
		if(((*myVm)->AttachCurrentThread(myVm, &(at->env), NULL)) != 0 ) {
			ERR LOG("ERROR: Attach to JVM failed");
			return 0;
		}
		else {
			at->willDetach = 1;
			at->attach_thread_id = pthread_self();
			at->attach_thread_id_valid = 1;
		}
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
	if (!pat || !*pat) return 0;
	audio_ctx_t *at = *pat;

	if (at->init) {
		// Attach close thread to JVM if not already attached.
		// Track whether we attached it so we can detach afterwards.
		int close_thread_attached = 0;
		JNIEnv *env = NULL;
		if ((*myVm)->GetEnv(myVm, (void**)&env, JNI_VERSION_1_4) != JNI_OK) {
			if ((*myVm)->AttachCurrentThread(myVm, &env, NULL) == 0) {
				close_thread_attached = 1;
			} else {
				ERR LOG("audiotrack_close: AttachCurrentThread failed, skipping JNI cleanup");
				at->init = 0;
				free(at);
				*pat = NULL;
				return -1;
			}
		}
		at->env = env;

		int underrun_count = call_int_method(at, "getUnderrunCount", "()I");
		if (underrun_count > 0)
			ERR LOG("Underrun count: %d", underrun_count);

		call_void_method(at, "release", "()V");
		(*at->env)->DeleteGlobalRef(at->env, at->obj);
		at->obj = NULL;  // Prevent use-after-free
		(*at->env)->DeleteGlobalRef(at->env, at->jbuffer);
		at->jbuffer = NULL;
		(*at->env)->DeleteGlobalRef(at->env, at->audiotrackClass);
		(*at->env)->DeleteGlobalRef(at->env, at->audiosystemClass);
		(*at->env)->DeleteGlobalRef(at->env, at->playbackParamsClass);
		(*at->env)->DeleteGlobalRef(at->env, at->audioAttributesBuilderClass);
		(*at->env)->DeleteGlobalRef(at->env, at->audioFormatBuilderClass);
		if (at->audioTimestamp) {
			(*at->env)->DeleteGlobalRef(at->env, at->audioTimestamp);
			at->audioTimestamp = NULL;
		}
		if (at->audioTimestampClass) {
			(*at->env)->DeleteGlobalRef(at->env, at->audioTimestampClass);
			at->audioTimestampClass = NULL;
		}

		// Detach the owner thread (from audiotrack_open) if close runs on it
		if (at->willDetach && at->attach_thread_id_valid &&
		    pthread_equal(pthread_self(), at->attach_thread_id)) {
			// Close is on the same thread that opened — detach it.
			// This also covers the close_thread_attached case since it's the same thread.
			(*myVm)->DetachCurrentThread(myVm);
			close_thread_attached = 0; // already detached
		} else if (close_thread_attached) {
			// Close is on a different thread that we temporarily attached — detach it
			(*myVm)->DetachCurrentThread(myVm);
		}

		at->init = 0;
	}
	free(at);
	*pat = NULL;
	return 0;
}

static int audiotrack_update_latency(audio_ctx_t *at, JNIEnv *env)
{
	if (!at || !env) return 0;

	float speed = get_effective_audio_speed();

	// AudioTrack.getLatency() available on API 29+ (returns 0 if method doesn't exist)
	int track_latency_raw = call_int_method_with_env(at, env, "getLatency", "()I");
	uint32_t track_latency = (track_latency_raw > 0) ? (uint32_t)track_latency_raw : 0;

	// Fallback: AudioSystem.getOutputLatency() + manual buffer calculation
	int system_latency_raw = call_int_method_current_vm( env, at->audiosystemClass, "getOutputLatency", "(I)I", streamType );
	uint32_t system_latency = (system_latency_raw > 0) ? (uint32_t)system_latency_raw : 0;
	uint32_t app_latency = (uint32_t)lrint( ( 1000.0 * (double)at->buf_size ) / ( (double)at->frame_size * (double)at->rate * speed ) );

	// Scheduler-safe latency: local buffer geometry only.
	// getLatency() / getOutputLatency() can include large HAL/HDMI/eARC pipeline delays
	// that nova cannot reason about. Keep them as diagnostics only.
	uint32_t scheduler_latency = app_latency;

	// Pipeline latency: platform-aware ceiling. Used as the selected static heard-delay
	// for mode2 passthrough (audiotrack_get_latency returns this for mode2) and as the
	// write-gate stall timeout for mode1 passthrough.
	uint32_t pipeline_latency = system_latency + app_latency;
	if (track_latency > pipeline_latency) {
		pipeline_latency = track_latency;
	}

	// Mode2-wide latency normalization. The paired cumulative ratio derives
	// compressed buffer duration without assuming a codec-specific packet size,
	// and applies to Dolby, TrueHD and the DTS family alike.
	if (at->passthrough == 2 && at->mode2_latency_bytes_accum > 0 && at->mode2_latency_samples_accum > 0) {
		uint64_t bytes_written = at->mode2_latency_bytes_accum;
		uint64_t logical_samples = at->mode2_latency_samples_accum;

		// Calculate buffer capacity dynamically using the paired cumulative ratio of compressed bytes
		// written to logical samples accepted. This avoids hardcoding 1536 samples/packet or
		// relying on the first packet size only, adapting naturally to VBR (EAC3) and different syncframes.
		// capacity_ms = (buf_size * logical_samples * 1000) / (bytes_written * rate * speed)
		uint32_t capacity_ms = (uint32_t)lrint( ( 1000.0 * (double)at->buf_size * (double)logical_samples ) /
		                                         ( (double)bytes_written * (double)at->rate * speed ) );

		// Isolate platform residual latency by subtracting the platform's nominal buffer delay.
		// Note: On this device, Android calculates compressed AudioTrack getLatency() by treating
		// 1 compressed byte as 1 frame (1 byte/frame geometry), yielding a nominal delay of buf_size / rate.
		uint32_t reported_buffer_latency = (uint32_t)lrint( ( 1000.0 * (double)at->buf_size ) / (double)at->rate );
		uint32_t residual_ms = 0;
		if (track_latency > reported_buffer_latency) {
			residual_ms = track_latency - reported_buffer_latency;
		}
		uint32_t corrected_pipeline = residual_ms + capacity_ms;
		if (system_latency + capacity_ms > corrected_pipeline) {
			corrected_pipeline = system_latency + capacity_ms;
		}

		// No empirical cap. A former 1000ms ceiling truncated the selected delay for
		// low-bitrate streams (e.g. 209kbps AC3 2.0: 32KB buffer really holds ~1365ms;
		// blocking writes keep it full, HAL drains in ~660ms quanta). Capping made the
		// heard clock overestimate physical presentation by capacity-cap (~365ms), so
		// the video scheduler slewed toward a false clock and drifted out of sync after
		// a track change (avos-443). The paired-ratio capacity is the physically
		// buffered duration and must be used unmodified.

		// Keep a production diagnostic whenever the normalized estimate is calculated.
		// It is needed to diagnose route-specific Android/HAL reports from field logs.
		LOG("mode2_normalized_latency: fmt=%04X raw_track=%u system=%u app=%u residual=%u bytes_written=%llu logical_samples=%llu capacity=%u selected=%u",
			at->format, track_latency, system_latency, app_latency, residual_ms,
			(unsigned long long)bytes_written, (unsigned long long)logical_samples,
			capacity_ms, corrected_pipeline);

		pipeline_latency = corrected_pipeline;
	}

	at->track_latency = track_latency;
	at->system_latency = system_latency;
	at->app_latency = app_latency;
	at->pipeline_latency = pipeline_latency;

	DBG LOG("audiotrack_update_latency: scheduler=%u app=%u system=%u track=%u pipeline=%u",
		scheduler_latency, app_latency, system_latency, track_latency, pipeline_latency);
	if (at->startup_latency_log_count < 5) {
		DBG2 LOG("startup_latency[%d]: format=%04X rate=%d ch=%d frame_size=%zu buf=%zu track=%u system=%u app=%u scheduler=%u pipeline=%u",
			at->startup_latency_log_count, at->format, at->rate, at->channel_count,
			at->frame_size, at->buf_size, track_latency, system_latency, app_latency, scheduler_latency, pipeline_latency);
		at->startup_latency_log_count++;
	}

	at->latency = scheduler_latency;
	return 1;
}

static uint32_t audiotrack_default_channel_mask(int channels)
{
	switch( channels ) {
	case 1: return AUDIO_CHANNEL_OUT_MONO;
	case 2: return AUDIO_CHANNEL_OUT_STEREO;
	case 3: return AUDIO_CHANNEL_OUT_STEREO | AUDIO_CHANNEL_OUT_LOW_FREQUENCY;
	case 4: return AUDIO_CHANNEL_OUT_SURROUND;
	case 5: return AUDIO_CHANNEL_OUT_QUAD | AUDIO_CHANNEL_OUT_LOW_FREQUENCY;
	case 6: return AUDIO_CHANNEL_OUT_5POINT1;
	case 7: return AUDIO_CHANNEL_OUT_5POINT1 | AUDIO_CHANNEL_OUT_BACK_CENTER;
	case 8: return AUDIO_CHANNEL_OUT_7POINT1;
	default: return 0;
	}
}

static int audiotrack_set_output_params(audio_ctx_t *at, int rate, int channels, int bits, int format)
{
	uint32_t track_chanmask;
	audio_format_t track_format;
	int status = 0;
	int prev_rate = at->rate;
	int prev_channels = at->channel_count;
	int prev_format = at->format;
	int prev_applied_passthrough = at->applied_passthrough;
	int prev_applied_spatialization_behavior = at->applied_spatialization_behavior;
	size_t prev_frame_size = at->frame_size;

	float as = get_effective_audio_speed();
	int is_audio_speed_enabled = audio_interface_is_audio_speed_enabled();
	int using_atempo = audio_interface_is_using_atempo();

	// For AC3 recoding, force format to WAVE_FORMAT_AC3 regardless of input format
	// This ensures AudioTrack is created with AC3 format (2000) instead of original format (e.g., EAC3 18247)
	int ac3_recoding_enabled = libavos_get_ac3_recoding_enabled();
	int requested_passthrough = at->passthrough;
	at->ac3_recode = ac3_recoding_enabled ? 1 : 0;
	at->ac3_mode2_plain_policy = ac3_recoding_enabled &&
		stream_audio_ac3_mode2_plain_policy();
	at->ac3_mode2_force_pipeline = ac3_recoding_enabled &&
		stream_audio_ac3_mode2_force_pipeline();
	if(ac3_recoding_enabled) {
		// The encoder output is always AC3 at 48 kHz. Configure that final
		// output domain on the first AudioTrack creation instead of opening a
		// provisional source-rate AC3 track (for example 44.1 kHz) and replacing
		// it after the first encoded frame.
		rate = AC3_RECODE_SAMPLE_RATE;
		format = WAVE_FORMAT_AC3;
		// Respect the current passthrough mode selected in native (may be 1 or 2)
		requested_passthrough = spdif_is_passthrough_on();
		if (requested_passthrough != 1 && requested_passthrough != 2) {
			requested_passthrough = 1;  // default to IEC if unset
		}
		// If IEC is unavailable on the current route, force codec-specific passthrough.
		if (requested_passthrough == 1 && !get_hdmi_supports_iec()) {
			requested_passthrough = 2;
		}
		channels = 2;  // IEC/codec-specific container is stereo for compressed payload
		// Latch the recode output layout published by the encoder filter (which opens
		// before this sink is configured) into this context, so a later overlapping
		// playback that changes the process-global cannot alter this AudioTrack's
		// latency policy mid-stream. audiotrack_get_latency reads only this context copy.
		at->ac3_recode_target_stereo = libavos_get_ac3_recode_target_stereo();
		DBG LOG( "AC3 recoding: forcing format to WAVE_FORMAT_AC3 (2000), passthrough mode %d, channels=%d, plain_policy=%d, force_pipeline=%d, target_stereo=%d", requested_passthrough, channels, at->ac3_mode2_plain_policy, at->ac3_mode2_force_pipeline, at->ac3_recode_target_stereo );
	} else {
		at->ac3_recode_target_stereo = 0;
	}
	at->passthrough = requested_passthrough;

	DBG LOG( "rate %d, channels %d, bits %d, format %d, passthrough mode %d, as %f", rate, channels, bits, format, at->passthrough, as );
	DBG LOG( "audiotrack_set_output_params: enter req_rate=%d req_channels=%d req_bits=%d req_format=%04X passthrough=%d using_atempo=%d speed=%.3f init=%d prev_rate=%d prev_channels=%d prev_format=%04X prev_passthrough=%d prev_frame_size=%zu",
		rate, channels, bits, format, at->passthrough, using_atempo, as, at->init,
		prev_rate, prev_channels, prev_format, prev_applied_passthrough, prev_frame_size );

	attach_thread( at );

	at->format = format;
	int output_channels = channels;
	int retry_rate = rate;
	int retry_channels = channels;
	int retry_bits = bits;
	int retry_format = format;
	track_chanmask = audiotrack_default_channel_mask(output_channels);
	if (!track_chanmask) {
		track_chanmask = AUDIO_CHANNEL_OUT_STEREO;
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
		// Mode 2: Delegate encapsulation to Android using codec-specific encodings.
		// Use content sample rate (typically 48kHz), not IEC container rate (192kHz).
		// Android handles the container format internally when using codec-specific encodings.
		frame_size = (bits / 8) * channels; // keep PCM-equivalent frame size for latency calc

		switch( at->format ) {
			case WAVE_FORMAT_AC3:
				if (HDMI_CHECK_BIT(get_hdmi_supported_audio_codecs(), HDMI_ENCODING_AC3)) {
					track_format = 5; // AudioFormat.ENCODING_AC3
				} else if (HDMI_CHECK_BIT(get_hdmi_supported_audio_codecs(), HDMI_ENCODING_E_AC3)) {
					track_format = 6; // AudioFormat.ENCODING_E_AC3 compatibility fallback
				} else {
					track_format = 5; // Try AC3 when caps are missing/unknown.
				}
				track_chanmask = AUDIO_CHANNEL_OUT_STEREO;
				output_channels = 2;
				// Keep content rate (typically 48kHz from demuxer)
				break;
			case WAVE_FORMAT_EAC3:
				track_format = 6; // AudioFormat.ENCODING_E_AC3
				track_chanmask = AUDIO_CHANNEL_OUT_STEREO;
				output_channels = 2;
				// Keep content rate (typically 48kHz), not IEC container rate (192kHz)
				break;
			case WAVE_FORMAT_E_AC3_JOC:
				if (device_get_android_api() >= 29 &&
				    HDMI_CHECK_BIT(get_hdmi_supported_audio_codecs(), HDMI_ENCODING_E_AC3_JOC)) {
					track_format = AUDIO_FORMAT_ENCODING_E_AC3_JOC; // AudioFormat.ENCODING_E_AC3_JOC
				} else {
					track_format = 6; // AudioFormat.ENCODING_E_AC3 base-layer fallback
				}
				track_chanmask = AUDIO_CHANNEL_OUT_STEREO;
				output_channels = 2;
				// Keep content rate (typically 48kHz), not IEC container rate (192kHz)
				break;
			case WAVE_FORMAT_DTS:
				track_format = 7; // AudioFormat.ENCODING_DTS
				track_chanmask = AUDIO_CHANNEL_OUT_STEREO;
				output_channels = 2;
				break;
			case WAVE_FORMAT_DTS_HD_MA:
				if (device_get_android_api() >= 34 &&
				    HDMI_CHECK_BIT(get_hdmi_supported_audio_codecs(), HDMI_ENCODING_DTS_HD_MA)) {
					track_format = 29; // AudioFormat.ENCODING_DTS_HD_MA
				} else if (HDMI_CHECK_BIT(get_hdmi_supported_audio_codecs(), HDMI_ENCODING_DTS_HD)) {
					track_format = 8; // AudioFormat.ENCODING_DTS_HD
				} else if (HDMI_CHECK_BIT(get_hdmi_supported_audio_codecs(), HDMI_ENCODING_DTS)) {
					track_format = 7; // AudioFormat.ENCODING_DTS core fallback
				} else {
					track_format = device_get_android_api() >= 34 ? 29 : 8; // Try best DTS-HD mode when caps are missing/unknown.
				}
				if (track_format != 7 && channels > 2) {
					output_channels = channels > 8 ? 8 : channels;
					track_chanmask = audiotrack_default_channel_mask(output_channels);
					if (!track_chanmask) {
						track_chanmask = AUDIO_CHANNEL_OUT_STEREO;
						output_channels = 2;
					}
				} else {
					track_chanmask = AUDIO_CHANNEL_OUT_STEREO;
					output_channels = 2;
				}
				break;
			case WAVE_FORMAT_DTS_HD:
				if (HDMI_CHECK_BIT(get_hdmi_supported_audio_codecs(), HDMI_ENCODING_DTS_HD)) {
					track_format = 8; // AudioFormat.ENCODING_DTS_HD
				} else if (HDMI_CHECK_BIT(get_hdmi_supported_audio_codecs(), HDMI_ENCODING_DTS)) {
					track_format = 7; // AudioFormat.ENCODING_DTS core fallback
				} else {
					track_format = 8; // Try DTS-HD when caps are missing/unknown; creation fallback remains below.
				}
				if (track_format == 8 && channels > 2) {
					output_channels = channels > 8 ? 8 : channels;
					track_chanmask = audiotrack_default_channel_mask(output_channels);
					if (!track_chanmask) {
						track_chanmask = AUDIO_CHANNEL_OUT_STEREO;
						output_channels = 2;
					}
				} else {
					track_chanmask = AUDIO_CHANNEL_OUT_STEREO;
					output_channels = 2;
				}
				break;
			case WAVE_FORMAT_TRUEHD:
				track_format = 14; // AudioFormat.ENCODING_DOLBY_TRUEHD
				if (device_get_android_api() >= 25 && channels > 2 &&
				    HDMI_CHECK_BIT(get_hdmi_supported_audio_codecs(), HDMI_ENCODING_DOLBY_TRUEHD)) {
					output_channels = channels > 8 ? 8 : channels;
					track_chanmask = audiotrack_default_channel_mask(output_channels);
					if (!track_chanmask) {
						track_chanmask = AUDIO_CHANNEL_OUT_STEREO;
						output_channels = 2;
					}
				} else {
					// Some Android TV routes reject TrueHD with a 7.1 mask in "Auto".
					// Retry creation below with stereo before giving up.
					track_chanmask = AUDIO_CHANNEL_OUT_STEREO;
					output_channels = 2;
				}
				break;
			default:
				// Fallback to IEC61937 for unknown formats
				track_format = 13; // AudioFormat.ENCODING_IEC61937
		}
		DBG LOG("Mode 2: codec-specific encoding=%d for format=%04X, channels=%d, rate=%d",
			track_format, at->format, output_channels, rate);
	} else if(at->passthrough == 1 && device_get_android_api() >= 24 && get_hdmi_supports_iec()) {
        track_format = 13; // AudioFormat.ENCODING_IEC61937
        switch(at->format) {
            case WAVE_FORMAT_AC3:
            case WAVE_FORMAT_DTS:
                track_chanmask = AUDIO_CHANNEL_OUT_STEREO;
                output_channels = 2;
                // Keep native 32/44.1kHz for AC3/DTS to avoid timing drift.
                if (rate != 32000 && rate != 44100) {
                    rate = 48000;
                }
                break;
			case WAVE_FORMAT_EAC3:
			case WAVE_FORMAT_E_AC3_JOC:
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

	int requested_spatialization_behavior = audiotrack_get_requested_spatialization_behavior(at, output_channels, track_format);
	int same_config = 0;
	if (at->init &&
		prev_rate == rate &&
		prev_channels == output_channels &&
		prev_format == format &&
		prev_applied_passthrough == at->passthrough &&
		prev_applied_spatialization_behavior == requested_spatialization_behavior &&
		prev_frame_size == frame_size) {
		same_config = 1;
	}

	// A caller can demand a genuine track rebuild even when the resolved
	// configuration is identical (e.g. resume after a user pause, where the
	// reused compressed passthrough track is wedged after flush()).
	if (at->force_recreate) {
		same_config = 0;
		at->force_recreate = 0;
	}

	audio_rate = rate;
	at->rate = rate;
	at->channel_count = output_channels;
	at->frame_size = frame_size;
	at->mode2_latency_bytes_accum = 0;
	at->mode2_latency_samples_accum = 0;
	at->mode2_latency_corrected = 0;
	channels = output_channels;
	DBG LOG("audiotrack_set_output_params: resolved out_rate=%d out_channels=%d frame_size=%zu track_format=%d chanmask=0x%x same_config=%d passthrough=%d",
		rate, output_channels, frame_size, track_format, track_chanmask, same_config, at->passthrough);

	if (same_config) {
		DBG LOG("audiotrack_set_output_params: reusing existing track (rate=%d ch=%d fmt=%d passthrough=%d speed=%.3f using_atempo=%d)",
			rate, output_channels, format, at->passthrough, as, using_atempo);
		if (!using_atempo && is_audio_speed_enabled && at->passthrough == 0 &&
			device_get_android_api() >= 23 && fabsf(as - 1.0f) > 1e-6f && at->obj) {
			jobject playbackParams;
			jthrowable exception;

			DBG LOG("audiotrack_set_output_params: updating PlaybackParams on existing track");
			playbackParams = (*at->env)->CallObjectMethod(at->env, at->obj,
				(*at->env)->GetMethodID(at->env, at->audiotrackClass, "getPlaybackParams", "()Landroid/media/PlaybackParams;"));
			exception = (*at->env)->ExceptionOccurred(at->env);
			if (exception) {
				ERR LOG("exception during getPlaybackParams on existing track");
				(*at->env)->ExceptionDescribe(at->env);
				(*at->env)->ExceptionClear(at->env);
			} else {
				(*at->env)->CallObjectMethod(at->env, playbackParams,
					(*at->env)->GetMethodID(at->env, at->playbackParamsClass, "setSpeed", "(F)Landroid/media/PlaybackParams;"), as);
				exception = (*at->env)->ExceptionOccurred(at->env);
				if (exception) {
					ERR LOG("exception during setSpeed on existing track");
					(*at->env)->ExceptionDescribe(at->env);
					(*at->env)->ExceptionClear(at->env);
				} else {
					(*at->env)->CallVoidMethod(at->env, at->obj,
						(*at->env)->GetMethodID(at->env, at->audiotrackClass, "setPlaybackParams", "(Landroid/media/PlaybackParams;)V"), playbackParams);
					exception = (*at->env)->ExceptionOccurred(at->env);
					if (exception) {
						ERR LOG("exception during setPlaybackParams on existing track");
						(*at->env)->ExceptionDescribe(at->env);
						(*at->env)->ExceptionClear(at->env);
					} else {
						DBG LOG("audiotrack_set_output_params: PlaybackParams updated on existing track speed=%.3f rate=%d channels=%d frame_size=%zu",
							as, rate, output_channels, frame_size);
						return 0;
					}
				}
			}
		} else {
			DBG LOG("audiotrack_set_output_params: same_config reuse without speed update using_atempo=%d speed_enabled=%d passthrough=%d api=%d speed=%.3f obj=%p",
				using_atempo, is_audio_speed_enabled, at->passthrough, device_get_android_api(), as, at->obj);
			return 0;
		}
		// Fall through to recreate if PlaybackParams update failed.
	}

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
		at->applied_passthrough = -1;
		at->applied_spatialization_behavior = -1;
		reinit = 1;
	}

	streamType = 3; /*STREAM_MUSIC*/
	int sampleRateInHz = rate;
	int channelConfig = track_chanmask << 2;
	int audioFormat = track_format;
	mode = 1; /*MODE_STREAM*/
	DBG LOG("audiotrack_set_output_params: creating new track reinit=%d speed=%.3f using_atempo=%d passthrough=%d sampleRate=%d channels=%d audioFormat=%d channelConfig=0x%x",
		reinit, as, using_atempo, at->passthrough, sampleRateInHz, channels, audioFormat, channelConfig);

	// When using atempo filter, AudioTrack always plays at 1.0x, so no need for larger buffers
	DBG LOG( "audiotrack_set_output_params: track_format=%d, track_chanmask=0x%x, channelConfig=0x%x (format=%d, passthrough=%d, channels=%d)",
	         track_format, track_chanmask, channelConfig, at->format, at->passthrough, channels );

	if(is_audio_speed_enabled && !using_atempo && at->passthrough == 0 && device_get_android_api() >= 23) {
		buffer_scale = 2; // for 2.0x max audio speed (when using PlaybackParams)
	} else {
		buffer_scale = 1;
	}
	DBG LOG( "audio_interface_audiotrack_java:audiotrack_set_output_params buffer_scale=%d (using_atempo=%d)", buffer_scale, using_atempo );

	int min_buffer_size = call_static_int_method(at, at->audiotrackClass, "getMinBufferSize", "(III)I",
			sampleRateInHz, channelConfig, audioFormat);

	if (min_buffer_size <= 0) {
		ERR LOG("getMinBufferSize returned %d, falling back to 32768", min_buffer_size);
		min_buffer_size = 32768;
	}

	if (at->passthrough) {
		// All compressed passthrough modes (mode 1 IEC encapsulation and mode 2
		// codec-specific encodings) write whole compressed bursts that must reach
		// AudioTrack atomically. getMinBufferSize() returns a PCM-style minimum that
		// can be smaller than a single IEC burst (e.g. 5672 < 6144 for AC3), which
		// causes audiotrack_write()'s MIN(buf_size, len) to truncate every burst and
		// drop the remainder -> progressive passthrough desync.
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

	int applied_spatialization_behavior = -1;
	if (!failed && requested_spatialization_behavior != -1 && device_get_android_api() >= 32) {
		jmethodID setSpatializationBehaviorMethod = (*at->env)->GetMethodID(at->env, at->audioAttributesBuilderClass, "setSpatializationBehavior", "(I)Landroid/media/AudioAttributes$Builder;");
		exception = (*at->env)->ExceptionOccurred(at->env);
		if (exception) {
			DBG LOG("audiotrack_set_output_params: setSpatializationBehavior lookup failed");
			(*at->env)->ExceptionClear(at->env);
		} else if (setSpatializationBehaviorMethod) {
			(*at->env)->CallObjectMethod(at->env, audioAttributesBuilder, setSpatializationBehaviorMethod, requested_spatialization_behavior);
			exception = (*at->env)->ExceptionOccurred(at->env);
			if (exception) {
				DBG LOG("audiotrack_set_output_params: setSpatializationBehavior(%d) failed", requested_spatialization_behavior);
				(*at->env)->ExceptionDescribe(at->env);
				(*at->env)->ExceptionClear(at->env);
			} else {
				applied_spatialization_behavior = requested_spatialization_behavior;
				DBG LOG("audiotrack_set_output_params: spatialization behavior=%d channels=%d format=%d",
					applied_spatialization_behavior, output_channels, track_format);
			}
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

	// Skip AudioTrack playback rate if using atempo filter (speed is handled in PCM resampling)
	// Note: using_atempo was already declared earlier in this function
	if(!failed && is_audio_speed_enabled && !using_atempo && at->passthrough == 0 && device_get_android_api() >= 23 && fabsf(as - 1.0f) > 1e-6f) { // adapt audio_speed only when passthrough disabled and audio_speed != 1.0
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
			ERR LOG("audiotrack ctor failed (status=%d) - backing off and retrying", status);
			failed = 1;
			if (at->obj) {
				(*at->env)->DeleteGlobalRef(at->env, at->obj);
				at->obj = NULL;
			}
			if (at->format == WAVE_FORMAT_AC3 && track_format == 5 &&
			    HDMI_CHECK_BIT(get_hdmi_supported_audio_codecs(), HDMI_ENCODING_E_AC3)) {
				DBG LOG("audiotrack_set_output_params: AC3 AudioTrack failed, retrying as EAC3 compatibility layer");
				return audiotrack_set_output_params(at, rate, 2, 16, WAVE_FORMAT_EAC3);
			}
			// If DTS HD failed, fallback to DTS for DTS core mode
			if ((at->format == WAVE_FORMAT_DTS_HD || at->format == WAVE_FORMAT_DTS_HD_MA) &&
			    track_format != 7 && track_chanmask != AUDIO_CHANNEL_OUT_STEREO) {
				DBG LOG("audiotrack_set_output_params: DTS-HD multichannel AudioTrack failed, retrying with stereo channel mask");
				return audiotrack_set_output_params(at, rate, 2, bits, at->format);
			}
			if (at->format == WAVE_FORMAT_DTS_HD_MA && track_format == 29) {
				DBG LOG("audiotrack_set_output_params: DTS-HD-MA AudioTrack failed, retrying as generic DTS-HD");
				return audiotrack_set_output_params(at, rate, channels, bits, WAVE_FORMAT_DTS_HD);
			}
			if (at->format == WAVE_FORMAT_DTS_HD || at->format == WAVE_FORMAT_DTS_HD_MA) {
				return audiotrack_set_output_params(at, 48000, 2, 16, WAVE_FORMAT_DTS);
			}
			if (at->format == WAVE_FORMAT_TRUEHD && track_chanmask != AUDIO_CHANNEL_OUT_STEREO) {
				DBG LOG("audiotrack_set_output_params: TrueHD multichannel AudioTrack failed, retrying with stereo channel mask");
				return audiotrack_set_output_params(at, rate, 2, bits, WAVE_FORMAT_TRUEHD);
			}
			if (at->format == WAVE_FORMAT_E_AC3_JOC && track_format == AUDIO_FORMAT_ENCODING_E_AC3_JOC) {
				DBG LOG("audiotrack_set_output_params: EAC3_JOC AudioTrack failed, retrying as EAC3 base layer");
				return audiotrack_set_output_params(at, rate, 2, 16, WAVE_FORMAT_EAC3);
			}
			msec_sleep(100); // give AudioFlinger more time to recover before re-entering
		}

		// Diagnostic: compare requested compressed config vs actual AudioTrack config.
		// Some HALs may silently force PCM/stereo while passthrough remains enabled.
		if (!failed) {
			int actual_format = call_int_method(at, "getAudioFormat", "()I");
			int actual_chmask = call_int_method(at, "getChannelConfiguration", "()I");
			int actual_rate = call_int_method(at, "getSampleRate", "()I");
			DBG2 LOG("audiotrack_set_output_params: actual AudioTrack format=%d chmask=0x%x rate=%d (requested format=%d chmask=0x%x passthrough=%d channels=%d)",
				actual_format, actual_chmask, actual_rate,
				track_format, track_chanmask, at->passthrough, channels);
		}

		//frame_size reported can be false for compressed formats
		at->frame_count = at->buf_size / at->frame_size; // number of frames in the buffer
		DBG LOG("len buf size %d frc %d frs %d nbChs %d", at->buf_size, at->frame_count, at->frame_size, at->channel_count);
		at->jbuffer = (jbyteArray) (*at->env)->NewGlobalRef(at->env, (*at->env)->NewByteArray(at->env, at->buf_size));
	}

	if (failed && reinit) {
		msec_sleep( 100 );
		ERR LOG("audio_interface_audiotrack_java:audiotrack_set_output_params self calls audiotrack_set_output_params\n");
		return audiotrack_set_output_params(at, retry_rate, retry_channels, retry_bits, retry_format);
	}

	if(failed) {
		if (is_audio_speed_enabled && !using_atempo && at->passthrough == 0 && fabsf(as - 1.0f) > 1e-6f) {
			ERR LOG("audiotrack_set_output_params: AudioTrack creation failed with speed request %.3f, reverting to 1x", as);
			audio_interface_set_audio_speed(1.0f);
		}
		return -1;
	}

	at->i_samples_written = 0;
	at->timestamp_written_offset = 0;
	audiotrack_reset_timing(at);
	audiotrack_update_latency(at, at->env);

	at->init = 1;
	at->applied_passthrough = at->passthrough;
	at->applied_spatialization_behavior = applied_spatialization_behavior;
	DBG LOG("track created");

	return 0;
}

static int audiotrack_set_passthrough(audio_ctx_t *at, int passthrough)
{
	int old_passthrough = at->passthrough;
	at->passthrough = passthrough;
	DBG LOG("audiotrack_set_passthrough: old=%d new=%d init=%d format=%04X rate=%d ch=%d frame_size=%zu applied=%d error_recovery=%d",
		old_passthrough, passthrough, at->init, at->format, at->rate,
		at->channel_count, at->frame_size, at->applied_passthrough, at->in_error_recovery);

	// DO NOT call audiotrack_set_output_params here during format changes!
	//
	// When the audio thread loop detects a format change and calls set_passthrough, the values
	// in audio_ctx_t (at->format, at->rate, at->channel_count, at->frame_size) all contain
	// STALE data from the previous track/format. The correct values are in s->audio, which will
	// be used when start() is called after the decoder is opened.
	//
	// Calling audiotrack_set_output_params with stale data causes multiple problems:
	// 1. Stale channel count (e.g., channels=2 from AAC when switching to EAC3 5.1 with channels=6)
	//    → Creates stereo AudioTrack instead of 5.1, causing Android to output PCM multichannel
	// 2. Stale format with wrong passthrough mode (e.g., format=EAC3 + passthrough=0)
	//    → Android blocks for seconds with invalid PCM configuration
	// 3. Stale format with passthrough enabled (e.g., format=AAC + passthrough=2)
	//    → Android falls back to PCM instead of compressed passthrough
	//
	// The passthrough mode has been updated (above), and start() will apply it with the
	// correct parameters from s->audio when the sink is reconfigured.

	// HOWEVER, we need to handle AudioTrack error recovery here for two cases:
	// 1. When audiotrack_write() encounters ERROR_DEAD_OBJECT and calls us to recover
	// 2. When the device explicitly needs to recreate an AudioTrack during recovery

	// Only recreate AudioTrack if we're in error recovery mode (flag set by audiotrack_write)
	if (at->in_error_recovery) {
		if (audiotrack_disable_recovery) {
DBG			LOG("audiotrack_set_passthrough: recovery disabled, skipping recreate (passthrough=%d)", passthrough);
			at->in_error_recovery = 0;
			return 0;
		}

		// We're in error recovery mode, recreate the track
		at->in_error_recovery = 0; // Reset the flag
DBG		LOG("audiotrack_set_passthrough: recreating track for error recovery (passthrough=%d)", passthrough);

		// Choose appropriate format for recovery
		int recovery_format = at->format;

		audiotrack_set_output_params(at, at->rate, at->channel_count,
			(passthrough == 2) ? 16 : at->frame_size * 8 / at->channel_count, recovery_format);
	}

	return 0;
}

static int audiotrack_get_passthrough(audio_ctx_t *at)
{
	return at->passthrough;
}

static int audiotrack_start(audio_ctx_t *at)
{
DBG	LOG("audiotrack_start: format=%04X, passthrough=%d", at->format, at->passthrough);
	if (!at->init) {
ERR		LOG("audiotrack_start: track not valid, error");
		return -1;
	}

	// Cold start needs the static clamp. On pause/resume, a preserved dynamic
	// delay is better evidence than static latency and avoids storm-induced
	// startup_hold loops.
	at->startup_hold_active = at->last_good_dynamic_valid ? 0 : 1;
	at->startup_hold_start_ms = atime();
	at->frozen_ts_streak = 0;

	JNIEnv *env_local = attach_thread_current_vm();
	if (!env_local) {
		return -1;
	}
	at->track_paused = 0;
	if (at->passthrough && at->passthrough_restart_after_flush) {
DBG		LOG("audiotrack_start: deferring passthrough restart until first post-flush write");
		return 0;
	}
	call_void_method_with_env(at, env_local, "play", "()V");
	at->passthrough_restart_after_flush = 0;

	return 0;
}

static int audiotrack_pause(audio_ctx_t *at)
{
DBG	LOG("audiotrack_pause: format=%04X, passthrough=%d", at->format, at->passthrough);
	if (!at->init) {
ERR		LOG("audiotrack_pause: track not valid, error");
		return -1;
	}

	// Always freeze the track with AudioTrack.pause(), passthrough included.
	// Leaving a passthrough track PLAYING (b2-style drain to underrun) was tried
	// and breaks the submitted-ledger sync model: the buffered audio (up to ~1s
	// in mode 2) plays out audibly while video is frozen, and every pause/resume
	// cycle accumulates that much permanent A/V desync (avos-440). Zero-preload
	// phase repair on resume is not available either for passthrough (acf158d:
	// caused permanent silence). The cost of pause() is the sink re-acquiring
	// its codec lock on resume (short muted stretch), which is the lesser evil.
	// A pause changes neither codec nor track parameters, so no recreation is
	// armed; resume is a plain play().
	JNIEnv *env_local = attach_thread_current_vm();
	if (!env_local) {
		return -1;
	}
	call_void_method_with_env(at, env_local, "pause", "()V");
	at->track_paused = 1;

	return 0;
}

static int audiotrack_unpause(audio_ctx_t *at)
{
	return audiotrack_start(at);
}

static int audiotrack_stop(audio_ctx_t *at)
{
DBG	LOG();
	if (!at->init) {
ERR		LOG("track not valid, error");
		return -1;
	}

	JNIEnv *env_local = attach_thread_current_vm();
	if (!env_local) {
		return -1;
	}
	// Flush buffered audio before stop. flush() is only valid on a paused or
	// stopped track, and passthrough tracks are deliberately left PLAYING across
	// stream_pause() to keep the sink codec lock, so pause here first (the track
	// is being torn down, losing the lock no longer matters).
	// Without the flush, AudioTrack.stop() starts a drain into the HAL pipeline;
	// when release() is called immediately after, residual HAL audio from the old
	// file can overlap the startup of the next playback and corrupt its timing window.
	call_void_method_with_env(at, env_local, "pause", "()V");
	call_void_method_with_env(at, env_local, "flush", "()V");
	call_void_method_with_env(at, env_local, "stop", "()V");
	at->timestamp_written_offset = at->i_samples_written;
	at->last_timestamp_frames = 0;
	at->last_timestamp_ns = 0;
	return 0;
}

static int audiotrack_can_write(audio_ctx_t *at, int len)
{
	const int passthrough_stall_fallback_min_ms = 250;
	const int passthrough_stall_fallback_margin_ms = 250;
	const int passthrough_stall_fallback_max_ms = 1500;

	if (!at->init) {
		ERR LOG("audiotrack_can_write: track not valid, error");
		return 0;
	}

	if (len <= 0) {
		DBG LOG("audiotrack_can_write: format=%04X, passthrough=%d, len=%d -> true",
			at->format, at->passthrough, len);
		return 1;
	}

	// PCM: always ready to accept writes.
	if (!at->passthrough) {
		DBG LOG("audiotrack_can_write: format=%04X, passthrough=%d, len=%d (pcm fast-path=true)",
			at->format, at->passthrough, len);
		return 1;
	}

	// Mode2 passthrough: byte/playhead gate is unreliable because compressed
	// packet duration is logical (fakeSize), not proportional to raw byte count,
	// and the playhead often does not advance during startup on eARC/HDMI routes.
	// Pacing is handled by the stream-level logical lead gate instead.
	if (at->passthrough >= 2) {
		DBG LOG("audiotrack_can_write: format=%04X, passthrough=%d, len=%d (mode2 bypass=true)",
			at->format, at->passthrough, len);
		return 1;
	}

	if (at->passthrough_can_write_blind) {
		DBG LOG("audiotrack_can_write: format=%04X, passthrough=%d, len=%d (blind fallback=true)",
			at->format, at->passthrough, len);
		return 1;
	}

	if (at->frame_size == 0 || at->frame_count <= 0) {
		DBG LOG("audiotrack_can_write: format=%04X, passthrough=%d, len=%d (invalid frame geometry, fallback=true)",
			at->format, at->passthrough, len);
		return 1;
	}

	JNIEnv *env_local = attach_thread_current_vm();
	if (!env_local) {
		DBG LOG("audiotrack_can_write: format=%04X, passthrough=%d, len=%d (no env, fallback=true)",
			at->format, at->passthrough, len);
		return 1;
	}

	jint playback_frames = call_int_method_with_env(at, env_local, "getPlaybackHeadPosition", "()I");
	if (playback_frames < 0) {
		DBG LOG("audiotrack_can_write: format=%04X, passthrough=%d, len=%d (playhead unavailable=%d, fallback=true)",
			at->format, at->passthrough, len, playback_frames);
		return 1;
	}

	uint64_t frames_presented = (uint64_t)playback_frames;
	uint64_t frames_written_adjusted = 0;
	if (at->i_samples_written > at->timestamp_written_offset) {
		frames_written_adjusted = at->i_samples_written - at->timestamp_written_offset;
	}

	int64_t frames_pending = (int64_t)frames_written_adjusted - (int64_t)frames_presented;
	if (frames_pending < 0) {
		frames_pending = 0;
	}

	int64_t frames_requested = ((int64_t)len + (int64_t)at->frame_size - 1) / (int64_t)at->frame_size;
	int64_t frames_available = (int64_t)at->frame_count - frames_pending;
	int can_write = (frames_available >= frames_requested);
	int now_ms = atime();
	int passthrough_stall_fallback_ms = passthrough_stall_fallback_min_ms;
	if (frames_presented > 0) {
		at->passthrough_playhead_ever_advanced = 1;
	}
	// Use pipeline_latency (HAL-aware) for write-gate timeouts, not scheduler latency.
	// On eARC/HDMI routes, track_latency can be 700ms+ while app_latency is ~170ms;
	// using only app_latency causes premature blind fallback before the playhead advances.
	int stall_base_ms = (int)(at->pipeline_latency > 0 ? at->pipeline_latency : at->latency);
	passthrough_stall_fallback_ms = stall_base_ms + passthrough_stall_fallback_margin_ms;
	if (passthrough_stall_fallback_ms < passthrough_stall_fallback_min_ms) {
		passthrough_stall_fallback_ms = passthrough_stall_fallback_min_ms;
	} else if (passthrough_stall_fallback_ms > passthrough_stall_fallback_max_ms) {
		passthrough_stall_fallback_ms = passthrough_stall_fallback_max_ms;
	}

	if (frames_presented != at->can_write_last_playback_frames) {
		at->can_write_last_playback_frames = frames_presented;
		at->can_write_stall_start_ms = 0;
	} else if (!can_write) {
		if (!at->can_write_stall_start_ms) {
			at->can_write_stall_start_ms = now_ms;
		} else if (now_ms - at->can_write_stall_start_ms >= passthrough_stall_fallback_ms) {
			at->passthrough_can_write_blind = 1;
			DBG LOG("audiotrack_can_write: format=%04X, passthrough=%d, len=%d exact gate stalled for %dms (threshold=%d scheduler=%d pipeline=%d pending=%lld available=%lld requested=%lld) -> enabling blind fallback",
				at->format, at->passthrough, len,
				now_ms - at->can_write_stall_start_ms, passthrough_stall_fallback_ms, at->latency, at->pipeline_latency,
				(long long)frames_pending, (long long)frames_available, (long long)frames_requested);
			return 1;
		}
	} else {
		at->can_write_stall_start_ms = 0;
	}

	DBG LOG("audiotrack_can_write: format=%04X, passthrough=%d, len=%d pending=%lld available=%lld requested=%lld frame_count=%d frame_size=%zu -> %d",
		at->format, at->passthrough, len,
		(long long)frames_pending, (long long)frames_available, (long long)frames_requested,
		at->frame_count, at->frame_size, can_write);

	return can_write;
}

// Periodic diagnostic: log playhead, timestamp, and derived delay for mode2.
// Logs once every 2s after logical samples are updated. No behavior change.
// Called from audiotrack_add_logical_samples() while the thread is attached.
static void audiotrack_mode2_playhead_audit(audio_ctx_t *at)
{
	if (!audiotrack_mode2_audit) return;
	int now_ms = atime();
	if (now_ms - at->mode2_audit_last_ms < 2000) return;
	at->mode2_audit_last_ms = now_ms;

	jint playhead = call_int_method(at, "getPlaybackHeadPosition", "()I");

	int64_t ts_frames = 0;
	int64_t ts_ns = 0;
	if (at->getTimestampMethodID && at->audioTimestamp && at->env) {
		jboolean ok = (*at->env)->CallBooleanMethod(at->env, at->obj,
			at->getTimestampMethodID, at->audioTimestamp);
		if (ok) {
			ts_frames = (int64_t)(*at->env)->GetLongField(at->env, at->audioTimestamp,
				at->framePositionFieldID);
			ts_ns = (int64_t)(*at->env)->GetLongField(at->env, at->audioTimestamp,
				at->nanoTimeFieldID);
		}
	}

	int selected = audiotrack_get_latency(at);

	// derived_logical: fakeSize-based logical samples vs getTimestamp frames.
	// derived_playhead: fakeSize-based logical samples vs getPlaybackHeadPosition.
	// Both compare the same logical written duration against two playhead sources.
	// Negative values are valid evidence: reset, unit mismatch, wrap, or bad timestamp.
	// i_samples_written (compressed bytes / frame_size) is logged but not used
	// for derivation — it is not logical audio duration for compressed mode2.
	int derived_logical_ms = INT_MIN;
	int derived_playhead_ms = INT_MIN;
	if (at->rate > 0) {
		if (ts_frames > 0) {
			int64_t delta_ts = (int64_t)at->mode2_logical_samples - ts_frames;
			derived_logical_ms = (int)(delta_ts * 1000 / at->rate);
		}
		if (playhead > 0) {
			int64_t delta_playhead = (int64_t)at->mode2_logical_samples - (int64_t)playhead;
			derived_playhead_ms = (int)(delta_playhead * 1000 / at->rate);
		}
	}

	LOG("mode2_playhead_audit: fmt=%04X logical=%llu i_written=%llu playhead=%d "
		"ts_frames=%lld ts_ns=%lld derived_logical=%d derived_playhead=%d "
		"selected=%d pipeline=%u app=%u",
		at->format,
		(unsigned long long)at->mode2_logical_samples,
		(unsigned long long)at->i_samples_written,
		(int)playhead,
		(long long)ts_frames,
		(long long)ts_ns,
		derived_logical_ms,
		derived_playhead_ms,
		selected,
		at->pipeline_latency,
		at->app_latency);
}

// Accumulate fakeSize-derived logical samples for mode2 playhead audit.
// Called from stream_sink_audio after each successful mode2 write.
// samples = fakeSize * ret / frame->size / bpf (scaled for partial writes).
// Triggers the periodic audit after updating so each log reflects current state.
static void audiotrack_add_logical_samples(audio_ctx_t *at, int samples, int accepted_bytes)
{
	if (!at || samples <= 0) return;
	at->mode2_logical_samples += (uint64_t)samples;

	if (at->passthrough == 2) {
		if (!at->mode2_latency_corrected) {
			if (accepted_bytes > 0) {
				at->mode2_latency_bytes_accum += (uint64_t)accepted_bytes;
				at->mode2_latency_samples_accum += (uint64_t)samples;
			}

			// Wait until we have accumulated at least 250ms of logical audio (e.g. 12000 samples @ 48kHz)
			// before calculating and freezing the pipeline latency.
			uint64_t threshold_samples = at->rate > 0 ? (uint64_t)(at->rate / 4) : 12000;
			if (at->mode2_latency_samples_accum >= threshold_samples) {
				JNIEnv *env = attach_thread_current_vm();
				if (env) {
					// Latch the correction flag only if the VM update executes successfully
					if (audiotrack_update_latency(at, env)) {
						at->mode2_latency_corrected = 1;
					}
				}
			}
		}
	}

	if (audiotrack_mode2_audit) {
		audiotrack_mode2_playhead_audit(at);
	}
}

static uint64_t audiotrack_epoch_adjust_presented_frames(audio_ctx_t *at, uint64_t raw_frames)
{
	if (!at || at->playhead_epoch_offset == 0) {
		return raw_frames;
	}
	if (raw_frames >= at->playhead_epoch_offset) {
		return raw_frames - at->playhead_epoch_offset;
	}
	// Treat wrap/reset as a new raw epoch. This keeps diagnostics bounded
	// instead of underflowing against a stale offset.
	return raw_frames;
}

static int audiotrack_write(audio_ctx_t *at, unsigned char *buffer, int len)
{
DBG	LOG("audiotrack_write: format=%04X, passthrough=%d, len=%d", at->format, at->passthrough, len);
	if (!at->init) {
ERR		LOG("audiotrack_write: track not valid, error");
		return -1;
	}

	attach_thread(at);

	ssize_t ret = 0;
	ssize_t len_to_write = MIN(at->buf_size, len);
	(*at->env)->SetByteArrayRegion(at->env, at->jbuffer, 0, len_to_write, buffer);
	ret = call_int_method(at, "write", "([BII)I", at->jbuffer, 0, len_to_write);
DBG	LOG("audiotrack_write: wrote %d out of %d bytes (format=%04X, passthrough=%d)",
		ret, len_to_write, at->format, at->passthrough);

	// Track samples written for dynamic latency calculation
	if (ret > 0) {
		if (at->passthrough && at->passthrough_restart_after_flush) {
DBG			LOG("audiotrack_write: restarting passthrough track after first post-flush write");
			call_void_method(at, "play", "()V");
			at->passthrough_restart_after_flush = 0;
			at->track_paused = 0;
		}
		at->i_samples_written += (uint64_t)(ret / at->frame_size);

		if (audiotrack_log_underruns) {
			int underrun_count = call_int_method(at, "getUnderrunCount", "()I");
			if (underrun_count >= 0 && underrun_count != at->last_underrun_count) {
				ERR LOG("AudioTrack underruns: total=%d delta=%+d (format=%04X, passthrough=%d)",
					 underrun_count,
					 underrun_count - at->last_underrun_count,
					 at->format, at->passthrough);
				at->last_underrun_count = underrun_count;
			}
		}
	}

	// Handle dead/broken AudioTrack: write() returns 0 or ERROR_DEAD_OBJECT (-6)
	// Both indicate the AudioTrack is in an unusable state and needs to be recreated.
	// Returning 0 immediately prevents tight loops that cause ANRs on devices with
	// slow/broken audio HALs (e.g., MediaTek HAL timeouts on Google TV devices).
	if (ret == 0 || ret == -6 /* ERROR_DEAD_OBJECT */) {
		if (ret == 0 && at->track_paused) {
			// Not a dead track: a blocking write() racing AudioTrack.pause()
			// returns 0 once the paused buffer is full. Drop just this chunk
			// (the audio thread stops on s->paused right after) and do not
			// trigger recovery/recreation.
DBG			LOG("audiotrack_write: write returned 0 on paused track (pause race) -> dropping chunk, no recovery");
			return -1;
		}
		if (ret == 0) {
ERR			LOG("audiotrack_write: write returned 0 (AudioTrack dead/broken) -> recovering track");
		} else {
ERR			LOG("audiotrack_write: ERROR_DEAD_OBJECT (-6) -> recovering track");
		}
		if (audiotrack_disable_recovery) {
ERR			LOG("audiotrack_write: recovery disabled, dropping write");
			return -1;
		}
		// Set error recovery flag
		at->in_error_recovery = 1;
		// Sleep briefly to avoid tight loop during recovery and give AudioFlinger time to stabilize
		msec_sleep(100);
		// Recreate the AudioTrack by passing current values
		audiotrack_set_passthrough(at, at->passthrough);
		// Return -1 to signal fatal write failure to upper layer
		// This prevents the infinite loop in stream_audio.c where size -= 0 never decreases
		return -1;
	} else if (ret < 0) {
ERR		LOG("audiotrack_write: ERROR code %d returned from Java write()", ret);
	}
	return ret;
}

static int audiotrack_get_delay(audio_ctx_t *at)
{
	const int delay_spike_threshold_ms = 200;
	const int delay_spike_threshold_stable_ms = 100;
	const int delay_max_ms = 5000;
	const int delay_suspect_ms = 2000;
	const int stable_streak_required = 10;
	const int playhead_streak_required = 3; // playhead advances in large chunks on some devices
	const int max_smooth_drift_ms = 1000;
	const char *src = "unknown";
	int ret = -1;
	int fallback_delay = -1;
	int delay_ms = -1;
#define AUD_RETURN(tag, value) \
	do { \
		src = tag; \
		ret = (value); \
		goto done; \
	} while (0)

	if (!at->init) {
ERR		LOG("track not valid, error");
		ret = -1;
		goto done;
	}


	// Keep static latency for all passthrough modes (mode 1 IEC wrapping and mode 2).
	// Mode 2 dynamic delay correction was removed in Commit A/D.
	// For mode2, audiotrack_get_latency() returns pipeline_latency which is the
	// physically correct selected delay on HAL-heavy routes (e.g. eARC).
	if (at->passthrough) {
		int static_delay = audiotrack_get_latency(at);
		if (at->passthrough >= 2 && at->startup_delay_log_count < 5) {
			LOG("passthrough_selected_delay[%d]: fmt=%04X passthrough=%d selected=%d pipeline=%u app=%u",
				at->startup_delay_log_count, at->format, at->passthrough,
				static_delay, at->pipeline_latency, at->app_latency);
			at->startup_delay_log_count++;
		}
		// Treat static passthrough delay as stable/valid for sync gating.
		if (at->ts_success_streak < stable_streak_required) {
			at->ts_success_streak = stable_streak_required;
		}
		at->delay_valid = 1;
		AUD_RETURN("static(passthrough)", static_delay);
	}

	// Use AudioTrack.getTimestamp() for dynamic latency calculation (API 19+)
	// This automatically accounts for Bluetooth and other output latencies
	// Can be disabled via preference if user experiences sync issues
	if (!enable_dynamic_audio_delay) {

		// User disabled dynamic latency, use static latency
DBG3		LOG("Dynamic latency disabled by user preference, using static latency: %d ms", at->latency);
		// Do NOT bump ts_success_streak here. Leaving it at 0 prevents
		// resume_rebase_delay_valid in stream_audio.c from treating static
		// latency as a "newly measured" delay and firing a bogus audio_time
		// rebase that leaves render_offset_ns stale in codec_sfdec2.c.
		at->delay_valid = 1;
		AUD_RETURN("static(disabled)", at->latency);
	}

	if (!at->audioTimestamp || !at->getTimestampMethodID || !at->framePositionFieldID || !at->nanoTimeFieldID) {
		// Fallback to static latency if AudioTimestamp not available
DBG3		LOG("Using static latency: %d ms", at->latency);
		at->delay_valid = 1;

		AUD_RETURN("static(no_timestamp)", at->latency);
	}

	if (at->rate <= 0) {
DBG3		LOG("Invalid sample rate, using static latency: %d ms", at->latency);
		at->delay_valid = 1;

		AUD_RETURN("static(bad_rate)", at->latency);
	}

	int now_ms = atime();
	int timing_query_interval_ms = at->ts_use_timestamp ? 2000 : 100;
	if (at->ts_last_query_ms > 0 && now_ms - at->ts_last_query_ms < timing_query_interval_ms) {
		// Throttle timing queries; reuse cached values during the stable window.
		if (at->ts_cached_valid) {
			at->delay_valid = (at->ts_use_timestamp || at->last_good_dynamic_valid) ? 1 : 0;
			AUD_RETURN("cached(throttle)", at->ts_cached_delay_ms);
		}
		if (audiotrack_last_good_dynamic(at, now_ms, &at->ts_cached_delay_ms)) {
			if (at->ts_cached_delay_ms <= 0 && at->latency > 0) {
				AUD_RETURN("static(last_good_zero)", at->latency);
			}
			at->ts_cached_valid = 1;
			at->delay_valid = (at->ts_use_timestamp || at->last_good_dynamic_valid) ? 1 : 0;
			AUD_RETURN("last_good(throttle)", at->ts_cached_delay_ms);
		}
		// No cached timing; reuse last fallback delay for heard-time only.
		if (at->last_fallback_delay_ms > 0 && (now_ms - at->last_fallback_ms) < 5000) {
			if (at->playhead_valid_streak >= playhead_streak_required) {
				at->ts_cached_delay_ms = at->last_fallback_delay_ms;
				at->ts_cached_valid = 1;
				at->last_good_dynamic_delay_ms = at->last_fallback_delay_ms;
				at->last_good_dynamic_ms = now_ms;
				at->last_good_dynamic_valid = 1;
				at->delay_valid = 1;
				DBG2 LOG("playhead(stable_throttle) t=%d", atime());
				AUD_RETURN("playhead(stable_throttle)", at->last_fallback_delay_ms);
			}
			at->delay_valid = 0;
			AUD_RETURN("fallback(throttle)", at->last_fallback_delay_ms);
		}
		at->delay_valid = 0;
		AUD_RETURN("throttle_none", (at->startup_hold_active && at->latency > 0) ? at->latency : 0);
	}

	// IMPORTANT: Get the JNIEnv for the CURRENT thread, not the cached one
	// audiotrack_get_delay() is called from the audio thread, which is different
	// from the thread that created at->env
	JNIEnv *env = attach_thread_current_vm();
	if (!env) {
		// Can't attach to current thread, fallback to static latency
DBG2		LOG("Failed to attach to current thread, using static latency: %d ms", at->latency);
		at->delay_valid = 1;

		AUD_RETURN("static(no_env)", at->latency);
	}

	// Check if AudioTrack object is still valid (could be NULL during teardown)
	if (!at->obj) {
ERR		LOG("AudioTrack object is NULL, using static latency: %d ms", at->latency);
		at->delay_valid = 1;

		AUD_RETURN("static(track_null)", at->latency);
	}

	fallback_delay = audiotrack_delay_from_playhead(at, env);
	if (fallback_delay < 0) {
		// Playback head unavailable; avoid static latency for heard-time fallback.
		fallback_delay = 0;
	}
	if (fallback_delay > 0) {
		// Cache fallback for throttled windows (heard-time only, not valid for anchoring).
		at->last_fallback_delay_ms = fallback_delay;
		at->last_fallback_ms = now_ms;
	}
	// Startup override: keep delay >= static latency during warmup.
	int startup_fallback = fallback_delay;
	if (at->startup_hold_active && at->latency > 0) {
		if (startup_fallback < at->latency) {
			startup_fallback = at->latency;
		}
	}

	// Re-check AudioTrack just before calling into Java to avoid races with teardown
	jobject track_obj = at->obj;
	if (!track_obj) {
ERR		LOG("AudioTrack object became NULL during getTimestamp, using fallback latency: %d ms", fallback_delay);
		at->ts_success_streak = 0;
		at->ts_use_timestamp = 0;
		at->ts_last_query_ms = now_ms;
		if (audiotrack_last_good_dynamic(at, now_ms, &at->ts_cached_delay_ms)) {
			at->ts_cached_valid = 1;
			at->delay_valid = 1;
			src = "last_good(track_null)";
			ret = at->ts_cached_delay_ms;
			goto done;
		}
		at->delay_valid = 0;
		AUD_RETURN("track_null", 0);
	}

	// Call AudioTrack.getTimestamp(AudioTimestamp)
	jboolean success = (*env)->CallBooleanMethod(env, track_obj, at->getTimestampMethodID, at->audioTimestamp);

	// Check for exceptions
	if ((*env)->ExceptionCheck(env)) {
		(*env)->ExceptionClear(env);
		DBG2 LOG("Exception in getTimestamp, using static latency: %d ms", at->latency);
		at->ts_success_streak = 0;
		at->ts_use_timestamp = 0;
		at->ts_last_query_ms = now_ms;
		if (audiotrack_last_good_dynamic(at, now_ms, &at->ts_cached_delay_ms)) {
			at->ts_cached_valid = 1;
			at->delay_valid = 1;
			src = "last_good(exception)";
			ret = at->ts_cached_delay_ms;
			goto done;
		}
		at->delay_valid = 0;
		AUD_RETURN("exception", 0);
	}

	if (!success) {
		// getTimestamp failed (can happen during warmup or if not supported), use static latency
DBG2		LOG("getTimestamp returned false, using fallback playback-head latency: %d ms", fallback_delay);
		at->ts_success_streak = 0;
		at->ts_use_timestamp = 0;
		at->ts_last_query_ms = now_ms;
		// If timestamps never stabilize (e.g., Sabrina), promote stable playhead fallback.
		if (fallback_delay > 0 && at->playhead_valid_streak >= playhead_streak_required) {
			at->ts_cached_delay_ms = fallback_delay;
			at->ts_cached_valid = 1;
			at->last_good_dynamic_delay_ms = fallback_delay;
			at->last_good_dynamic_ms = now_ms;
			at->last_good_dynamic_valid = 1;
			at->delay_valid = 1;
			src = "playhead(stable)";
			ret = fallback_delay;
			goto done;
		}
		// If we already had a valid dynamic delay, keep it instead of static.
		if (at->last_good_dynamic_valid && at->last_good_dynamic_delay_ms > 0) {
			src = "last_good(getTimestamp_false)";
			ret = at->last_good_dynamic_delay_ms;
			goto done;
		}
		if (audiotrack_last_good_dynamic(at, now_ms, &at->ts_cached_delay_ms)) {
			at->ts_cached_valid = 1;
			at->delay_valid = 1;
			src = "last_good(getTimestamp_false)";
			ret = at->ts_cached_delay_ms;
			goto done;
		}
		// No trusted delay; return fallback for heard-time only.
		at->delay_valid = 0;
		src = "fallback(ts_false)";
		ret = (at->startup_hold_active && startup_fallback > 0) ? startup_fallback :
		      (fallback_delay > 0) ? fallback_delay : 0;
		goto done;
	}

	// Extract framePosition and nanoTime from AudioTimestamp using cached field IDs
	int64_t framePosition = (*env)->GetLongField(env, at->audioTimestamp, at->framePositionFieldID);
	int64_t nanoTime = (*env)->GetLongField(env, at->audioTimestamp, at->nanoTimeFieldID);
DBG2	LOG("getTimestamp success=%d framePosition=%lld nanoTime=%lld rate=%d ts_use=%d streak=%d",
		(int)success, (long long)framePosition, (long long)nanoTime, at->rate,
		at->ts_use_timestamp, at->ts_success_streak);

	if (framePosition <= 0 || nanoTime <= 0) {
		// Timestamp not usable; rely on playback-head or static fallback.
DBG2		LOG("Non-positive timestamp values, timing unavailable (framePosition=%lld nanoTime=%lld)",
			(long long)framePosition, (long long)nanoTime);
		at->ts_last_query_ms = now_ms;
		if (fallback_delay > 0 && at->playhead_valid_streak >= playhead_streak_required) {
			at->ts_cached_delay_ms = fallback_delay;
			at->ts_cached_valid = 1;
			at->last_good_dynamic_delay_ms = fallback_delay;
			at->last_good_dynamic_ms = now_ms;
			at->last_good_dynamic_valid = 1;
			at->delay_valid = 1;
			src = "playhead(stable)";
			ret = fallback_delay;
			goto done;
		}
		// Keep last-good dynamic delay if available.
		if (at->last_good_dynamic_valid && at->last_good_dynamic_delay_ms > 0) {
			at->delay_valid = 1;
			src = "last_good(bad_ts)";
			ret = at->last_good_dynamic_delay_ms;
			goto done;
		}
		if (audiotrack_last_good_dynamic(at, now_ms, &at->ts_cached_delay_ms)) {
			at->ts_cached_valid = 1;
			at->delay_valid = 1;
			src = "last_good(bad_ts)";
			ret = at->ts_cached_delay_ms;
			goto done;
		}
		// No trusted delay; return fallback for heard-time only.
		at->delay_valid = 0;
		src = "fallback(bad_ts)";
		ret = (at->startup_hold_active && startup_fallback > 0) ? startup_fallback :
		      (fallback_delay > 0) ? fallback_delay : 0;
		goto done;
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

	// Convert frames to wall/output ms.
	// With AudioTrack PlaybackParams(speed S), the hardware consumes frames at rate*S per
	// wall-clock second, so wall_delay = frames_pending * 1000 / (rate * S).
	// For atempo the filter already resampled before AudioTrack, so AT drains at 1x;
	// the frame backlog is already wall time and must NOT be divided by speed.
	int delay_media_ms = (int)((frames_pending * 1000) / at->rate);
	float at_speed = 1.0f;
	int using_atempo_now = audio_interface_is_audio_speed_enabled() &&
		audio_interface_is_using_atempo();
	if (!using_atempo_now) {
		at_speed = get_effective_audio_speed();
	}
	if (at_speed > 1e-3f && fabsf(at_speed - 1.0f) > 1e-6f) {
		delay_ms = (int)(delay_media_ms / at_speed);
	} else {
		delay_ms = delay_media_ms;
	}
DBG2	LOG("delay_clock: fp=%lld ns=%lld pending=%lld delay_media=%dms delay_wall=%dms speed=%.3f rate=%d using_atempo=%d",
		(long long)frames_presented, (long long)nanoTime,
		(long long)frames_pending, delay_media_ms, delay_ms, at_speed, at->rate, using_atempo_now);

	// Guard against unrealistic estimates (e.g., during startup) and fallback to static latency
	if (delay_ms < 0 || delay_ms > delay_max_ms) {
		// Outlier: drop dynamic delay and fall back.
DBG2		LOG("Dynamic latency %d ms out of range, fallback to static: %d ms", delay_ms, at->latency);
		at->ts_success_streak = 0;
		at->ts_use_timestamp = 0;
		at->ts_last_query_ms = now_ms;
		if (audiotrack_last_good_dynamic(at, now_ms, &at->ts_cached_delay_ms)) {
			at->ts_cached_valid = 1;
			at->delay_valid = 1;
			src = "last_good(outlier)";
			ret = at->ts_cached_delay_ms;
			goto done;
		}
		at->delay_valid = 0;
		AUD_RETURN("outlier", 0);
	}

	// Require a streak of advancing timestamps before trusting them to avoid startup jumps.
	if (nanoTime <= at->last_timestamp_ns || frames_presented <= at->last_timestamp_frames) {
		at->ts_success_streak = 0;
		at->frozen_ts_streak++;
	} else {
		at->ts_success_streak++;
		at->frozen_ts_streak = 0;
	}
	if (at->ts_success_streak >= stable_streak_required) {
		if (!at->ts_use_timestamp) {
			// First transition to timestamp-based delay: discard the warmup-era cached value
			// (which comes from the near-empty-buffer playhead fallback and is far too low).
			// The first live getTimestamp measurement will initialize ts_cached_delay_ms from
			// the raw value without smoothing, giving a much better initial estimate.
			at->ts_cached_valid = 0;
		}
		at->ts_use_timestamp = 1;
	}

	// Cache the timestamp for debugging/monitoring
	at->last_timestamp_ns = nanoTime;
	at->last_timestamp_frames = frames_presented;

	if (!at->ts_use_timestamp) {
		// Warmup: require a streak of advancing timestamps before trusting them.
DBG2		LOG("Dynamic latency warming up (streak %d), using fallback playback-head latency: %d ms", at->ts_success_streak, fallback_delay);
DBG2		LOG("delay: latency=%d startup=%d fallback=%d", at->latency, at->startup_hold_active, startup_fallback);
		at->ts_last_query_ms = now_ms;
		// Do not re-inject static latency once a real delay is known.
		if (at->last_good_dynamic_valid && at->last_good_dynamic_delay_ms > 0) {
			at->delay_valid = 1;
			src = "last_good(warmup)";
			ret = at->last_good_dynamic_delay_ms;
			goto done;
		}
		if (audiotrack_last_good_dynamic(at, now_ms, &at->ts_cached_delay_ms)) {
			at->ts_cached_valid = 1;
			at->delay_valid = 1;
			src = "last_good(warmup)";
			ret = at->ts_cached_delay_ms;
			goto done;
		}
		// No trusted delay; return fallback for heard-time only.
		at->delay_valid = 0;
		src = "fallback(warmup)";
		ret = (fallback_delay > 0) ? fallback_delay : 0;
		goto done;
	}
	// Once dynamic delay approaches static latency or is stable for a while, drop startup clamp.
	if (at->startup_hold_active && at->latency > 0) {
		if (delay_ms >= at->latency - 20 || at->ts_success_streak >= stable_streak_required + 20) {
			at->startup_hold_active = 0;
		}

		if (at->startup_hold_active) {
			if (at->last_good_dynamic_valid && at->last_good_dynamic_delay_ms > 0) {
				at->delay_valid = 1;
				src = "last_good(startup_hold)";
				ret = at->last_good_dynamic_delay_ms;
				goto done;
			}
			// Fix A: getTimestamp() is returning a frozen framePosition after seek (observed on
			// Google Streamer 4K).  The two normal exit conditions (delay_ms >= latency-20 and
			// ts_success_streak >= 30) can never be met because the frozen framePosition prevents
			// the streak from building and caps delay_ms below the threshold.
			// Fix B: catch-all timeout in case any other device gets stuck in startup_hold.
			// In both cases, if a fresh playhead-based delay is available and sane, trust it and
			// exit the hold so delay_valid can be set and resume_rebase_delay_valid can fire.
			const int frozen_ts_threshold = 10;
			const int startup_hold_timeout_ms = 1500;
			int frozen_escape = (at->frozen_ts_streak >= frozen_ts_threshold);
			int timeout_escape = (at->startup_hold_start_ms > 0 &&
			                      (now_ms - at->startup_hold_start_ms) >= startup_hold_timeout_ms);
			if ((frozen_escape || timeout_escape) &&
			     fallback_delay > 0 &&
			     at->last_fallback_ms > 0 &&
			     (now_ms - at->last_fallback_ms) < 500 &&
			     fallback_delay >= at->latency / 4) {
				at->startup_hold_active = 0;
				at->frozen_ts_streak = 0;
				at->ts_cached_delay_ms = fallback_delay;
				at->ts_cached_valid = 1;
				at->last_good_dynamic_delay_ms = fallback_delay;
				at->last_good_dynamic_ms = now_ms;
				at->last_good_dynamic_valid = 1;
				at->delay_valid = 1;
				src = frozen_escape ? "playhead(frozen_ts_exit)" : "playhead(startup_hold_timeout)";
				ret = fallback_delay;
				goto done;
			}

			// Keep startup clamp but return fallback for heard-time only.
			at->delay_valid = 0;
			src = "fallback(startup_hold)";
			ret = (startup_fallback > 0) ? startup_fallback : 0;
			goto done;
		}
	}

DBG2	LOG("Dynamic latency: %d ms (written: %llu, presented: %llu, pending: %lld frames)",
		delay_ms, (unsigned long long)frames_written_adjusted, (unsigned long long)frames_presented, (long long)frames_pending);
DBG2	LOG("delay: latency=%d startup=%d fallback=%d returned=%d", at->latency, at->startup_hold_active, fallback_delay, delay_ms);

	at->ts_last_query_ms = now_ms;
	if (at->ts_cached_valid) {
		if (delay_ms > delay_suspect_ms) {
			// Large jump: keep last good delay rather than trusting the spike.
			at->delay_valid = 1;
			src = "cached(spike)";
			ret = at->ts_cached_delay_ms;
			goto done;
		}
		int spike_threshold = at->ts_use_timestamp ? delay_spike_threshold_stable_ms
			: delay_spike_threshold_ms;
		if (delay_ms > at->ts_cached_delay_ms + spike_threshold) {
			// Ignore spikes; keep last good delay to avoid jitter.
			at->delay_valid = 1;
			src = "cached(spike_threshold)";
			ret = at->ts_cached_delay_ms;
			goto done;
		}
		if (abs(delay_ms - at->ts_cached_delay_ms) > max_smooth_drift_ms) {
			// Drift too large to smooth; jump to the new value.
			at->ts_cached_delay_ms = delay_ms;
			at->ts_cached_valid = 1;
			at->last_good_dynamic_delay_ms = delay_ms;
			at->last_good_dynamic_ms = now_ms;
			at->last_good_dynamic_valid = 1;
			at->delay_valid = 1;
			src = "dynamic(jump)";
			ret = delay_ms;
			goto done;
		}
		// Exponential smoothing to reduce jitter in the dynamic delay.
		delay_ms = (at->ts_cached_delay_ms * 8 + delay_ms * 2) / 10;
	}
	at->ts_cached_delay_ms = delay_ms;
	at->ts_cached_valid = 1;
	at->last_good_dynamic_delay_ms = delay_ms;
	at->last_good_dynamic_ms = now_ms;
	at->last_good_dynamic_valid = 1;
	at->delay_valid = 1;
	src = "dynamic";
	ret = delay_ms;
done:
DBG3	LOG("get_delay: src=%s ret=%d latency=%d fallback=%d delay=%d ts_use=%d streak=%d",
		src, ret, at->latency, fallback_delay, delay_ms, at->ts_use_timestamp,
		at->ts_success_streak);
	at->last_delay_ret = ret;
	at->last_delay_fallback_ms = fallback_delay;
	at->last_delay_ms = delay_ms;
	snprintf(at->last_delay_src, sizeof(at->last_delay_src), "%s", src ? src : "unknown");
	if (Debug[DBG_AUD] > 2) {
		serprintf("aud_at_delay: src=%s ret=%d delay_valid=%d latency=%d fallback=%d delay=%d ts_use=%d streak=%d\n",
			src, ret, at->delay_valid, at->latency, fallback_delay, delay_ms,
			at->ts_use_timestamp, at->ts_success_streak);
	} else if (Debug[DBG_AUD] > 1 && at) {
		int emit = 0;
		if ((at->delay_diag_count % 100) == 0) {
			emit = 1;
		}
		if (at->delay_diag_last_valid != at->delay_valid ||
		    at->delay_diag_last_ret != ret ||
		    at->delay_diag_last_fallback != fallback_delay ||
		    at->delay_diag_last_ts_use != at->ts_use_timestamp) {
			emit = 1;
		}
		if (emit) {
			serprintf("aud_at_delay: src=%s ret=%d delay_valid=%d latency=%d fallback=%d delay=%d ts_use=%d streak=%d\n",
				src, ret, at->delay_valid, at->latency, fallback_delay, delay_ms,
				at->ts_use_timestamp, at->ts_success_streak);
		}
		at->delay_diag_count++;
		at->delay_diag_last_valid = at->delay_valid;
		at->delay_diag_last_ret = ret;
		at->delay_diag_last_fallback = fallback_delay;
		at->delay_diag_last_ts_use = at->ts_use_timestamp;
	}
	if (at && at->startup_delay_log_count < 5) {
		DBG2 LOG("startup_delay[%d]: src=%s ret=%d valid=%d latency=%d fallback=%d delay=%d ts_use=%d streak=%d hold=%d frozen=%d",
			at->startup_delay_log_count, src ? src : "unknown", ret, at->delay_valid,
			at->latency, fallback_delay, delay_ms, at->ts_use_timestamp,
			at->ts_success_streak, at->startup_hold_active, at->frozen_ts_streak);
		at->startup_delay_log_count++;
	}
#undef AUD_RETURN
	return ret;
}

static void audiotrack_flush_output(audio_ctx_t *at)
{
DBG	LOG();
	if (!at->init) {
ERR		LOG("track not valid, error");
		return;
	}

	// Use thread-local env to avoid cross-thread JNIEnv* usage.
	JNIEnv *env_local = attach_thread_current_vm();
	if (!env_local) {
		ERR LOG("flush_output: failed to attach thread to JVM");
		return;
	}
	call_void_method_with_env(at, env_local, "pause", "()V");
	call_void_method_with_env(at, env_local, "flush", "()V");
	at->track_paused = 1;

	// Reset timing state after flush
	at->i_samples_written = 0;
	audiotrack_reset_timing(at);
	jint flush_playhead = call_int_method_with_env(at, env_local, "getPlaybackHeadPosition", "()I");
	if (flush_playhead > 0) {
		at->playhead_epoch_offset = (uint64_t)(uint32_t)flush_playhead;
		DBG LOG("audiotrack_flush_output: playhead_epoch_offset=%llu",
			(unsigned long long)at->playhead_epoch_offset);
	} else {
		at->playhead_epoch_offset = 0;
		DBG LOG("audiotrack_flush_output: playhead_epoch_offset unavailable (%d)", flush_playhead);
	}
	if (at->passthrough) {
		DBG LOG("audiotrack_flush_output: scheduling passthrough restart after flush");
		at->passthrough_restart_after_flush = 1;
	}
}

static int audiotrack_last_good_dynamic(audio_ctx_t *at, int now_ms, int *delay_out)
{
	const int last_good_stale_ms = 5000;

	if (!at || !delay_out) {
		return 0;
	}
	if (!at->last_good_dynamic_valid) {
		return 0;
	}
	if (now_ms - at->last_good_dynamic_ms > last_good_stale_ms) {
		return 0;
	}
	*delay_out = at->last_good_dynamic_delay_ms;
	return 1;
}

static int audiotrack_get_latency(audio_ctx_t *at)
{
	if (!at || !at->init) {
		return -1;
	}
	// Mode2 startup/fallback delay policy. Once enough paired compressed-byte and
	// logical-sample evidence has been collected, every mode2 codec uses the
	// normalized pipeline latency below instead.
	//
	// EAC3 (plain) and E_AC3_JOC (Atmos): pipeline_latency captures the real HAL
	// delay on this route; app_latency underestimates it, causing the lead gate to
	// throttle too early and audio to fall behind (sound late) or drift (desync).
	// JOC is E-AC3 plus Atmos metadata and uses the same HAL decode path as plain
	// E-AC3, so it must use the same pipeline_latency policy (Atmos-tagged streams
	// such as scarpetta were ~30ms late under app_latency, while non-Atmos EAC3
	// such as belfast stayed in sync).
	//
	// TrueHD, DTS-HD, DTS-HD MA use app_latency until normalization is available;
	// the raw platform pipeline value overestimates their startup delay.
	//
	// AC3 and unknown formats: default to pipeline_latency (conservative).
	if (at->passthrough >= 2) {
		// Once sufficient paired byte/sample evidence exists, use the normalized
		// pipeline for every compressed mode2 format. Before that point retain the
		// startup fallback rather than exposing raw pipeline latency to TrueHD/DTS-HD.
		if (at->mode2_latency_corrected) {
DBG3		LOG("audiotrack_get_latency: mode2 format=%04X using normalized pipeline_latency=%u (app=%u)",
				at->format, at->pipeline_latency, at->latency);
			return (int)at->pipeline_latency;
		}
		if (at->format == WAVE_FORMAT_TRUEHD ||
		    at->format == WAVE_FORMAT_DTS_HD ||
		    at->format == WAVE_FORMAT_DTS_HD_MA) {
DBG3		LOG("audiotrack_get_latency: mode2 format=%04X using app_latency=%u (pipeline=%u)",
				at->format, at->latency, at->pipeline_latency);
			return (int)at->latency;
		}
		// AC3-recode resolved-mode2 plain policy: use app_latency (AudioTrack buffer
		// geometry) instead of pipeline_latency, which overestimates the eARC/HDMI route.
		// Coupled with the STREAM_SYNC_SAMPLES clock in stream_audio.c; both apply together.
		if (at->ac3_mode2_plain_policy &&
		    at->format == WAVE_FORMAT_AC3 &&
		    at->ac3_recode) {
			if (at->ac3_mode2_force_pipeline) {
DBG3			LOG("audiotrack_get_latency: AC3-recode mode2 A/B forcing pipeline_latency=%u (app=%u stereo=%d)",
					at->pipeline_latency, at->latency, at->ac3_recode_target_stereo);
				return (int)at->pipeline_latency;
			}
			// Output-aware latency: a stereo (2.0/192k) recode shows a steady ~553ms
			// picture-leads-sound error (= pipeline-app, 724-171) that app_latency
			// under-compensates, so the stereo path uses pipeline_latency. Multichannel
			// recode stays on app_latency. Distinguish by the encoder target
			// channels, not AudioTrack ch (both compressed payloads report ch=2).
			if (at->ac3_recode_target_stereo) {
DBG3			LOG("audiotrack_get_latency: AC3-recode mode2 STEREO pipeline_latency=%u (app=%u)",
					at->pipeline_latency, at->latency);
				return (int)at->pipeline_latency;
			}
DBG3		LOG("audiotrack_get_latency: AC3-recode mode2 plain policy app_latency=%u (pipeline=%u)",
				at->latency, at->pipeline_latency);
			return (int)at->latency;
		}
		if (at->pipeline_latency > at->latency) {
			return (int)at->pipeline_latency;
		}
	}
	return (int)at->latency;
}

static int audiotrack_is_delay_valid(audio_ctx_t *at)
{
	return at ? at->delay_valid : 0;
}

static const char *audiotrack_get_delay_source(audio_ctx_t *at)
{
	return (at && at->last_delay_src[0]) ? at->last_delay_src : "unknown";
}

static int audiotrack_get_delay_valid_streak(audio_ctx_t *at)
{
	// Sabrina can report late/unstable timestamps; expose streak to gate rebases.
	return at ? at->ts_success_streak : 0;
}

static void audiotrack_invalidate_delay_cache(audio_ctx_t *at)
{
	if (!at) {
		return;
	}
	int using_atempo = audio_interface_is_audio_speed_enabled() &&
		audio_interface_is_using_atempo();
	DBG LOG("audiotrack_invalidate_delay_cache: using_atempo=%d ts_use=%d startup=%d cached=%d last_good_valid=%d last_good=%d",
		using_atempo, at->ts_use_timestamp, at->startup_hold_active,
		at->ts_cached_valid, at->last_good_dynamic_valid,
		at->last_good_dynamic_delay_ms);
	at->ts_last_query_ms = 0;
	at->ts_cached_valid = 0;
	if (using_atempo) {
		// Atempo changes can quickly fill/drain the AudioTrack queue while the
		// sink still plays at 1x. A pre-change last-good delay is stale evidence.
		at->last_good_dynamic_delay_ms = 0;
		at->last_good_dynamic_ms = 0;
		at->last_good_dynamic_valid = 0;
		at->delay_valid = 0;
		if (at->ts_use_timestamp) {
			at->startup_hold_active = 0;
		}
		at->startup_hold_start_ms = atime();
	}
}

static int audiotrack_is_startup_hold_active(audio_ctx_t *at)
{
	return at ? at->startup_hold_active : 0;
}

static int audiotrack_passthrough_playhead_advanced(audio_ctx_t *at)
{
	return at ? at->passthrough_playhead_ever_advanced : 1;
}

// Compute latency using playback head position as a safe fallback when getTimestamp is
// unavailable or unstable. Do not reuse stale headpos; a zero value means timing is unavailable.
static int audiotrack_delay_from_playhead(audio_ctx_t *at, JNIEnv *env_local)
{
	if (!env_local || !at) {
		if (at) {
			at->last_playhead_delay_ms = -1;
		}
		return 0;
	}

	jint playback_frames = call_int_method_with_env(at, env_local, "getPlaybackHeadPosition", "()I");
	if (playback_frames <= 0) {
DBG2		LOG("getPlaybackHeadPosition returned %d, timing unavailable", playback_frames);
		at->playhead_valid_streak = 0;
		at->last_playhead_delay_ms = -1;

		return -1;
	}

	uint64_t frames_presented = (uint64_t)playback_frames;
	if (frames_presented > at->headpos_last_frames) {
		at->playhead_valid_streak++;
	} else {
		at->playhead_valid_streak = 0;
	}
	at->headpos_last_frames = frames_presented;
DBG2	LOG("playhead_streak: presented=%llu last=%llu streak=%d",
		(unsigned long long)frames_presented,
		(unsigned long long)at->headpos_last_frames,
		at->playhead_valid_streak);
	if (frames_presented < at->last_timestamp_frames && at->i_samples_written > frames_presented) {
		at->timestamp_written_offset = at->i_samples_written - frames_presented;
DBG2		LOG("Playback head reset detected, offset=%llu", (unsigned long long)at->timestamp_written_offset);
	}

	uint64_t frames_written_adjusted = 0;
	if (at->i_samples_written > at->timestamp_written_offset) {
		frames_written_adjusted = at->i_samples_written - at->timestamp_written_offset;
	}

	int64_t frames_pending = (int64_t)frames_written_adjusted - (int64_t)frames_presented;
	if (frames_pending < 0)
		frames_pending = 0;

	// Same domain correction as audiotrack_get_delay: convert frame backlog to wall/output ms.
	// With PlaybackParams(speed S), frames drain at rate*S per wall-clock second.
	// Atempo is excluded: AT drains at 1x when atempo pre-resamples.
	int delay_media_ms = (int)((frames_pending * 1000) / at->rate);
	int delay_ms;
	float ph_speed = 1.0f;
	int using_atempo_now = audio_interface_is_audio_speed_enabled() &&
		audio_interface_is_using_atempo();
	if (!using_atempo_now) {
		ph_speed = get_effective_audio_speed();
	}
	if (ph_speed > 1e-3f && fabsf(ph_speed - 1.0f) > 1e-6f) {
		delay_ms = (int)(delay_media_ms / ph_speed);
	} else {
		delay_ms = delay_media_ms;
	}
	at->last_playhead_delay_ms = delay_ms;
DBG2	LOG("playhead_delay: presented=%llu written=%llu pending=%lld delay_media=%dms delay_wall=%dms speed=%.3f rate=%d smooth_valid=%d",
		(unsigned long long)frames_presented, (unsigned long long)frames_written_adjusted,
		(long long)frames_pending, delay_media_ms, delay_ms, ph_speed, at->rate, at->headpos_smooth_valid);
	if (delay_ms < 0 || delay_ms > 2000) {
DBG2		LOG("Playback-head latency %d ms out of range, timing unavailable", delay_ms);
		at->last_playhead_delay_ms = -1;
		return -1;
	}
	return delay_ms;
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
	// Mode 2 now uses raw compressed data (not IEC), so use channel_count for all modes
	// original: len = at->frame_count * at->frame_size * ((at->passthrough == 2) ? 4 : at->channel_count);
	len = at->buf_size * at->channel_count;

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

static void audiotrack_reset_timing(audio_ctx_t *at)
{
	if (!at) {
		return;
	}
	at->last_timestamp_ns = 0;
	at->last_timestamp_frames = 0;
	at->timestamp_written_offset = 0;
	at->playhead_epoch_offset = 0;
	at->ts_success_streak = 0;
	at->ts_use_timestamp = 0;
	at->ts_last_query_ms = 0;
	at->ts_cached_delay_ms = 0;
	at->ts_cached_valid = 0;
	at->last_good_dynamic_delay_ms = 0;
	at->last_good_dynamic_ms = 0;
	at->last_good_dynamic_valid = 0;
	at->delay_valid = 0;
	at->playhead_valid_streak = 0;
	at->headpos_smooth_frames = 0;
	at->headpos_last_frames = 0;
	at->headpos_smooth_valid = 0;
	at->startup_hold_active = 1;
	at->startup_hold_start_ms = atime();
	at->frozen_ts_streak = 0;
	at->last_fallback_delay_ms = 0;
	at->last_fallback_ms = 0;
	at->can_write_last_playback_frames = 0;
	at->can_write_stall_start_ms = 0;
	at->passthrough_can_write_blind = 0;
	at->passthrough_restart_after_flush = 0;
	at->passthrough_playhead_ever_advanced = 0;
	at->mode2_logical_samples = 0;
	at->mode2_latency_bytes_accum = 0;
	at->mode2_latency_samples_accum = 0;
	at->mode2_latency_corrected = 0;
	at->mode2_audit_last_ms = 0;
}

static int audiotrack_change_audio_speed(audio_ctx_t *at, float speed)
{
	// Bail out if context is not initialized or was released
	if( !at || !at->init ) {
		ERR LOG("audiotrack_change_audio_speed: AudioTrack context not initialized");
		return 0;
	}

	int using_atempo = audio_interface_is_using_atempo();

	if(audio_interface_is_audio_speed_enabled() && !using_atempo && at->passthrough == 0 && device_get_android_api() >= 23) { // adapt audio_speed only when passthrough disabled and API23+
DBG	LOG("audio_interface_audiotrack_java:audiotrack_change_audio_speed speed=%f", speed);
		DBG LOG("audio_interface_audiotrack_java:audiotrack_change_audio_speed path using_atempo=%d passthrough=%d api=%d rate=%d channels=%d format=%04X frame_size=%d buf_size=%d obj=%p current_speed=%.3f",
			using_atempo, at->passthrough, device_get_android_api(), at->rate, at->channel_count,
			at->format, at->frame_size, at->buf_size, at->obj, audio_interface_get_audio_speed());

		JNIEnv *myEnv = attach_thread_current_vm();
		if (*myEnv == NULL) return 0;

		DBG LOG( "audio_interface_audiotrack_java:audiotrack_change_audio_speed attached to current thread" );

		// Check if AudioTrack object is still valid (could be NULL during teardown)
		if (!at->obj) {
			ERR LOG("audiotrack_change_audio_speed: AudioTrack object is NULL, cannot change speed");
			return 0;
		}

		// reuse already created audioTrack
		jobject audioTrack = at->obj;

		// get current audioparams
		jobject playbackParams =
			( *myEnv )
				->CallObjectMethod( myEnv, audioTrack,
									( *myEnv )
										->GetMethodID( myEnv, at->audiotrackClass, "getPlaybackParams",
													   "()Landroid/media/PlaybackParams;" ) );

		int failed = 0;
		jthrowable exception = ( *myEnv )->ExceptionOccurred( myEnv );
		if( exception ) {
			ERR LOG( "audio_interface_audiotrack_java:audiotrack_change_audio_speed exception during getPlaybackParams" );
			( *myEnv )->ExceptionDescribe( myEnv );
			( *myEnv )->ExceptionClear( myEnv );
			failed = 1;
		}

		if( playbackParams == NULL ) {
			ERR LOG( "audio_interface_audiotrack_java:audiotrack_change_audio_speed getPlaybackParams returned NULL" );
			failed = 1;
		}

		if( !failed ) {
			DBG LOG( "audio_interface_audiotrack_java:audiotrack_change_audio_speed playbackparams fetched" );

			// change that audioparam's speed
			( *myEnv )
				->CallObjectMethod(
					myEnv, playbackParams,
					( *myEnv )
						->GetMethodID( myEnv, at->playbackParamsClass, "setSpeed", "(F)Landroid/media/PlaybackParams;" ),
					speed );

			DBG LOG( "audio_interface_audiotrack_java:audiotrack_change_audio_speed setspeed done" );

			exception = ( *myEnv )->ExceptionOccurred( myEnv );
			if( exception ) {
				ERR LOG( "audio_interface_audiotrack_java:audiotrack_change_audio_speed exception during setSpeed call" );
				( *myEnv )->ExceptionDescribe( myEnv );
				( *myEnv )->ExceptionClear( myEnv );
				failed = 1;
			}
		}

		// set audiotrack's audioparams
		if( !failed ) {
			( *myEnv )
				->CallVoidMethod( myEnv, audioTrack,
								  ( *myEnv )
									  ->GetMethodID( myEnv, at->audiotrackClass, "setPlaybackParams",
													 "(Landroid/media/PlaybackParams;)V" ),
								  playbackParams );

			DBG LOG( "audio_interface_audiotrack_java:audiotrack_change_audio_speed audioparams set" );

			// Catch exceptions from setPlaybackParams (e.g., speed or params out of range)
			exception = ( *myEnv )->ExceptionOccurred( myEnv );
			if( exception ) {
				ERR LOG( "audio_interface_audiotrack_java:audiotrack_change_audio_speed exception during setPlaybackParams" );
				( *myEnv )->ExceptionDescribe( myEnv );
				( *myEnv )->ExceptionClear( myEnv );
				failed = 1;
			}
		}

		int status =
			( *myEnv ) ->CallIntMethod( myEnv, audioTrack,
									   ( *myEnv ) ->GetMethodID( myEnv, at->audiotrackClass, "getState", "()I" ) );
		if( status != 1 ) { // STATE_INITIALIZED is 1 ; 0 for uninit
			failed = 1;
		}

	 	DBG LOG( "audio_interface_audiotrack_java:audiotrack_change_audio_speed getstate %d",status );

		// Read back the speed that was actually accepted by the hardware.
		// On some routes (e.g., multichannel PCM via HDMI/AVR) Android silently
		// accepts setPlaybackParams() but the driver clamps the speed to 1.0.
		float applied_speed = failed ? 1.0f : speed;
		int readback_ok = 0;
		if( !failed ) {
			jobject readback_params = ( *myEnv )->CallObjectMethod( myEnv, audioTrack,
				( *myEnv )->GetMethodID( myEnv, at->audiotrackClass, "getPlaybackParams",
					"()Landroid/media/PlaybackParams;" ) );
			jthrowable rb_ex = ( *myEnv )->ExceptionOccurred( myEnv );
			if( rb_ex ) { ( *myEnv )->ExceptionClear( myEnv ); }
			if( readback_params && !rb_ex ) {
				jfloat hw_speed = ( *myEnv )->CallFloatMethod( myEnv, readback_params,
					( *myEnv )->GetMethodID( myEnv, at->playbackParamsClass, "getSpeed", "()F" ) );
				jthrowable gs_ex = ( *myEnv )->ExceptionOccurred( myEnv );
				if( !gs_ex ) {
					applied_speed = (float)hw_speed;
					readback_ok = 1;
				} else {
					( *myEnv )->ExceptionClear( myEnv );
				}
				( *myEnv )->DeleteLocalRef( myEnv, readback_params );
			}
		}

		int mismatch     = readback_ok && fabsf( applied_speed - speed ) >= 0.01f;
		int rejected_to_1x = readback_ok && fabsf( applied_speed - 1.0f ) < 0.01f
			&& fabsf( speed - 1.0f ) >= 0.01f;
		at->playbackparams_speed_rejected = rejected_to_1x;

		if( mismatch || rejected_to_1x || !readback_ok ) {
			LOG( "at_speed_hw: req=%.3f applied=%.3f failed=%d readback_ok=%d mismatch=%d rejected_to_1x=%d channels=%d rate=%d passthrough=%d using_atempo=%d",
				speed, applied_speed, failed, readback_ok, mismatch, rejected_to_1x,
				at->channel_count, at->rate, at->passthrough, using_atempo );
		} else {
			DBG LOG( "at_speed_hw: req=%.3f applied=%.3f readback_ok=%d channels=%d rate=%d",
				speed, applied_speed, readback_ok, at->channel_count, at->rate );
		}

		if( failed ) {
			ERR LOG( "audio_interface_audiotrack_java:audiotrack_change_audio_speed audiotrack change params failed: reverting to 1x" );
			audio_interface_set_audio_speed(1.0f);
		} else {
			DBG LOG( "audio_interface_audiotrack_java:audiotrack_change_audio_speed audio speed changed" );
			audio_interface_set_audio_speed(applied_speed);
		}

		// PlaybackParams does not flush the AudioTrack. Keep playhead/timestamp
		// continuity across speed changes; resetting here makes rapid ramps run
		// permanently with invalid delay evidence and unstable video pacing.
		audiotrack_update_latency(at, myEnv);
	} else {
		DBG LOG("audio_interface_audiotrack_java:audiotrack_change_audio_speed skipped speed=%f speed_enabled=%d using_atempo=%d passthrough=%d api=%d init=%d obj=%p",
			speed, audio_interface_is_audio_speed_enabled(), using_atempo, at ? at->passthrough : -1,
			device_get_android_api(), at ? at->init : 0, at ? at->obj : NULL);
	}
	return 0;
}

// Returns the current AudioTrack presented frame position and sample rate.
// Prefers getTimestamp if it was queried within 500ms (well within the 2000ms throttle window).
// Falls back to a fresh getPlaybackHeadPosition() JNI call otherwise.
// Frame position is in the RST/media-sample domain — do not divide by speed;
// use RST_TO_TS_DELTA() in the stream layer to convert a delta to TS domain.
static int audiotrack_get_presented_frames(audio_ctx_t *at, uint64_t *frames, int *rate, int *source, int *age_ms, int prefer_fresh)
{
	if (!at || !frames || !rate || at->rate <= 0) return 0;

	// DAC-accurate source: extrapolate the last stable AudioTrack timestamp to now.
	// framePosition is the frame actually presented at the DAC, so this position is
	// latency-free, unlike getPlaybackHeadPosition() (frames handed to the mixer).
	// Extrapolation covers the 2000ms getTimestamp throttle window; if the sample is
	// older than 2500ms (pause, stall) fall through to the playhead query below.
	if (at->ts_use_timestamp && at->last_timestamp_ns > 0 && at->last_timestamp_frames > 0) {
		struct timespec now_ts;
		clock_gettime(CLOCK_MONOTONIC, &now_ts);
		int64_t now_ns = (int64_t)now_ts.tv_sec * 1000000000LL + now_ts.tv_nsec;
		int64_t age_ns = now_ns - at->last_timestamp_ns;
		if (age_ns >= 0 && age_ns < 2500LL * 1000000LL) {
			int64_t adv = (age_ns * at->rate) / 1000000000LL;
			// With AudioTrack PlaybackParams(speed S) the DAC consumes frames at
			// rate*S; with atempo the track drains at 1x (filter already resampled).
			if (!(audio_interface_is_audio_speed_enabled() && audio_interface_is_using_atempo())) {
				float spd = get_effective_audio_speed();
				if (spd > 1e-3f && fabsf(spd - 1.0f) > 1e-6f)
					adv = (int64_t)(adv * spd);
			}
			uint64_t f = at->last_timestamp_frames + (uint64_t)adv;
			// Never report beyond what was written (underrun/pause guard).
			uint64_t written_adj = at->i_samples_written > at->timestamp_written_offset ?
				at->i_samples_written - at->timestamp_written_offset : 0;
			if (written_adj > 0 && f > written_adj)
				f = written_adj;
			*frames = audiotrack_epoch_adjust_presented_frames(at, f);
			*rate = at->rate;
			if (source) *source = AT_PRESENTED_FRAMES_SRC_TIMESTAMP;
			if (age_ms) *age_ms = (int)(age_ns / 1000000LL);
			return 1;
		}
	}

	// Stale-cache fallback for callers that do not need a fresh sample.
	if (!prefer_fresh && at->ts_use_timestamp && at->last_timestamp_frames > 0 && at->ts_last_query_ms > 0) {
		int ts_age = atime() - at->ts_last_query_ms;
		if (ts_age < 500) {
			*frames = audiotrack_epoch_adjust_presented_frames(at, at->last_timestamp_frames);
			*rate = at->rate;
			if (source) *source = AT_PRESENTED_FRAMES_SRC_TIMESTAMP;
			if (age_ms) *age_ms = ts_age;
			return 1;
		}
	}

	// prefer_fresh=1, or timestamp cache is stale — call getPlaybackHeadPosition() directly.
	// This bypasses the delay throttle for this lightweight 32-bit position query only.
	JNIEnv *env = attach_thread_current_vm();
	if (!env || !at->obj) return 0;

	jint ph_frames = call_int_method_with_env(at, env, "getPlaybackHeadPosition", "()I");
	if (ph_frames <= 0) return 0;

	*frames = audiotrack_epoch_adjust_presented_frames(
		at, (uint64_t)(uint32_t)ph_frames);  // 32-bit position; wraps at ~27h at 44100Hz
	*rate = at->rate;
	if (source) *source = AT_PRESENTED_FRAMES_SRC_PLAYHEAD;
	if (age_ms) *age_ms = 0;
	return 1;
}

static int audiotrack_get_written_frames(audio_ctx_t *at, uint64_t *frames, int *rate)
{
	if (!at || !frames || !rate || at->rate <= 0) return 0;
	*frames = at->i_samples_written;
	*rate = at->rate;
	return 1;
}

void libavos_set_dynamic_audio_delay(int enable)
{
	DBG serprintf("audio_interface_audiotrack_java:libavos_set_dynamic_audio_delay enable=%d\n", enable);
	enable_dynamic_audio_delay = enable;
}

const audio_interface_impl_t audio_interface_impl_audiotrack_java = {
	.name = "audiotrack_java",
	.init = audiotrack_init,
	.exit = audiotrack_exit,
	.open = audiotrack_open,
	.close = audiotrack_close,
	.start = audiotrack_start,
	.stop = audiotrack_stop,
	.pause = audiotrack_pause,
	.unpause = audiotrack_unpause,
	.can_write = audiotrack_can_write,
	.write = audiotrack_write,
	.set_output_params = audiotrack_set_output_params,
	.get_delay = audiotrack_get_delay,
	.get_latency = audiotrack_get_latency,
	.flush_output = audiotrack_flush_output,
	.preload = audiotrack_preload,
	.get_session_id = audiotrack_get_session_id,
	.set_passthrough = audiotrack_set_passthrough,
	.get_passthrough = audiotrack_get_passthrough,
	.change_audio_speed = audiotrack_change_audio_speed,
	.delay_valid = audiotrack_is_delay_valid,
	.delay_source = audiotrack_get_delay_source,
	.delay_valid_streak = audiotrack_get_delay_valid_streak,
	.is_startup_hold_active = audiotrack_is_startup_hold_active,
	.passthrough_playhead_advanced = audiotrack_passthrough_playhead_advanced,
	.invalidate_delay_cache = audiotrack_invalidate_delay_cache,
	.add_logical_samples = audiotrack_add_logical_samples,
	.get_presented_frames = audiotrack_get_presented_frames,
	.get_written_frames = audiotrack_get_written_frames,
};

#ifdef DEBUG_MSG
DECLARE_DEBUG_PARAM("at_underrun", audiotrack_log_underruns );
DECLARE_DEBUG_PARAM("at_disable_recovery", audiotrack_disable_recovery );
DECLARE_DEBUG_PARAM("at_mode2_audit", audiotrack_mode2_audit );
#endif
