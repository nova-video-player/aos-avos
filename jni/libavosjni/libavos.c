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

#define LOG_TAG "libavos"

#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#include <limits.h>

#include "jni.h"
#include "libavos.h"

void libavos_init(const char *name, const char *pkg_name, int has_pluginlib);
void libavos_reload();
void libavos_exit();
void libavos_debug_init();
void libavos_debug_exit();
void libavos_avsh(const char *cmd);
void libavos_set_subtitlepath(const char *path);
void libavos_set_decoder(int decoder);
void libavos_set_dolby_vision_mode(int mode);
void libavos_set_dolby_vision_target_nits(float nits);
void libavos_set_dolby_vision_plane_scaler(int scaler);
void libavos_set_audio_interface(int audio_interface);
void libavos_set_audio_decoder(int audio_decoder);
void libavos_set_mediacodec_audio_capabilities(int64_t capabilities);
void libavos_set_codepage(int codepage);
void libavos_set_output_sample_rate(int sample_rate);
void libavos_set_passthrough(int force_passthrough);
void libavos_set_downmix(int downmix);
void libavos_set_dynamic_audio_delay(int enable);
void libavos_set_hdmi_supported_audio_codecs(long flag);
void libavos_set_audio_transform(int (*transformer)(float* buf, int nsamples));
void libavos_disable_atempo_filter(int disable);

fields_t fields;
extern JavaVM *myVm;
extern jobject myClassLoader;
extern jmethodID myFindClassMethod;

static struct {
    pthread_mutex_t mtx;
    int init;
    int debug;
} libavos = {
    .mtx = PTHREAD_MUTEX_INITIALIZER,
    .init = 0,
    .debug = 0,
};

static const char *player_name = "avos_player";
static const char *scanner_name = "avos_scanner";
static const char *default_name = "avos";

static const char *get_name()
{
    char buf[256];
    ssize_t size;
    int fd;

    if (snprintf(buf, 256, "/proc/%d/cmdline", getpid()) >= 256)
        return default_name;
    fd = open(buf, O_RDONLY);
    if (fd == -1) {
        close(fd);
        return default_name;
    }
    size = read(fd, buf, 255);
    close(fd);
    if (size == -1)
        return default_name;
    buf[size] = '\0';
    if (strstr(buf, "scanner") != NULL)
        return scanner_name;
    else
        return player_name;
}

static void libavos_debug_acquire()
{
    if (!libavos.debug) {
        LOGV("libavos_debug_acquire: first init");
        libavos_debug_init();
        libavos.debug = 1;
    }
}

static void libavos_debug_release()
{
    if (libavos.debug) {
        libavos_debug_exit();
        libavos.debug = 0;
    }
}

static void libavos_acquire(const char *pkg_name, int has_pluginlib)
{
    if (!libavos.init) {
        LOGV("libavos_acquire");
        libavos_init(get_name(), pkg_name, has_pluginlib);
        libavos.init = 1;
    }
}

static void libavos_release()
{
    if (libavos.init) {
        LOGV("libavos_release");
        libavos_exit();
        libavos.init = 0;
    }
}

int register_avosmediaplayer(JNIEnv *env);
int register_avosmediametadataretriever(JNIEnv *env);

static int register_libavos(JNIEnv *env)
{
#define GET_CLASS(_clazz, str) do { \
    (_clazz) = (*env)->FindClass(env, (str)); \
    if (!(_clazz)) \
        return -1; \
    (_clazz) = (jclass) (*env)->NewGlobalRef(env, (_clazz)); \
    if (!(_clazz)) \
        return -1; \
} while (0)

    GET_CLASS(fields.AvosMediaPlayerClazz , "com/archos/medialib/AvosMediaPlayer");
    fields.AvosMediaPlayer_postEventMethod =
            (*env)->GetStaticMethodID(env, fields.AvosMediaPlayerClazz, "postEventFromNative",
                    "(Ljava/lang/Object;IIILjava/lang/Object;)V");
    if (!fields.AvosMediaPlayer_postEventMethod)
        return -1;

    GET_CLASS(fields.AvosBitmapHelperClazz, "com/archos/medialib/AvosBitmapHelper");
    fields.AvosBitmapHelper_createRGBBitmapMethod =
            (*env)->GetStaticMethodID(env, fields.AvosBitmapHelperClazz, "createRGBBitmap",
                    "([IIIIIII)Landroid/graphics/Bitmap;");
    if (!fields.AvosBitmapHelper_createRGBBitmapMethod)
        return -1;

    GET_CLASS(fields.SubtitleClazz, "com/archos/medialib/Subtitle");
    fields.Subtitle_createTimedTextSubtitleMethod =
            (*env)->GetStaticMethodID(env, fields.SubtitleClazz, "createTimedTextSubtitle",
                    "(IILjava/lang/String;)"
                    "Ljava/lang/Object;");
    if (!fields.Subtitle_createTimedTextSubtitleMethod)
        return -1;

    fields.Subtitle_createTimedBitmapSubtitleMethod =
            (*env)->GetStaticMethodID(env, fields.SubtitleClazz, "createTimedBitmapSubtitle",
                    "(IIIIIILandroid/graphics/Bitmap;)"
                    "Ljava/lang/Object;");
    if (!fields.Subtitle_createTimedBitmapSubtitleMethod)
        return -1;

    GET_CLASS(fields.ClassLoaderClazz, "java/lang/ClassLoader");
    //we use a dummy instance of one of our classes, not system one, to get correct classLoader
    jclass instancewithLoader = (*env)->GetObjectClass(env, fields.SubtitleClazz);
    jmethodID ClassLoaderMethod = (*env)->GetMethodID(env, instancewithLoader, "getClassLoader", "()Ljava/lang/ClassLoader;");
    fields.myClassLoader = (*env)->NewGlobalRef(env, (*env)->CallObjectMethod(env, fields.SubtitleClazz, ClassLoaderMethod));
    myClassLoader = fields.myClassLoader;
    fields.FindClassMethod = (*env)->GetMethodID(env, fields.ClassLoaderClazz, "findClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    myFindClassMethod = fields.FindClassMethod;

    if (register_avosmediaplayer(env) == -1)
        return -1;
    if (register_avosmediametadataretriever(env) == -1)
        return -1;

    return 0;
}


int unregister_avosmediaplayer(JNIEnv *env);
int unregister_avosmediametadataretriever(JNIEnv *env);

static int unregister_libavos(JNIEnv *env)
{
    (*env)->DeleteGlobalRef(env, fields.AvosMediaPlayerClazz);
    (*env)->DeleteGlobalRef(env, fields.AvosBitmapHelperClazz);
    (*env)->DeleteGlobalRef(env, fields.SubtitleClazz);
    if (fields.AudioTransformerObj) {
        (*env)->DeleteGlobalRef(env, fields.AudioTransformerObj);
        fields.AudioTransformerObj = NULL;
        fields.AudioTransformer_transformAudioMethod = NULL;
    }

    if (unregister_avosmediaplayer(env) == -1)
        return -1;
    if (unregister_avosmediametadataretriever(env) == -1)
        return -1;

    return 0;
}

extern void acodecs_init(void);

jint JNI_OnLoad(JavaVM* vm, void* reserved)
{
    JNIEnv* env = NULL;
    jint result = -1;

    if ((*vm)->GetEnv(vm, (void**) &env, JNI_VERSION_1_4) != JNI_OK) {
        LOGE("ERROR: GetEnv failed\n");
        goto bail;
    }
    if (!env)
        goto bail;
    if (register_libavos(env) < 0) {
        LOGE("ERROR avosmediaplayer registration failed\n");
    }
    myVm = vm;
    acodecs_init();
    result = JNI_VERSION_1_4;

bail:
    return result;
}


void JNI_OnUnload(JavaVM* vm, void* reserved)
{
    JNIEnv* env = NULL;

    if ((*vm)->GetEnv(vm, (void**) &env, JNI_VERSION_1_4) != JNI_OK) {
        LOGE("ERROR: GetEnv failed\n");
        return;
    }
    unregister_libavos(env);

    pthread_mutex_lock(&libavos.mtx);
    libavos_debug_release();
    libavos_release();
    pthread_mutex_unlock(&libavos.mtx);
    myVm = NULL;
}

void
Java_com_archos_medialib_LibAvos_nativeInit(JNIEnv *env, jobject thiz, jstring pkg_name, jboolean has_pluginlib)
{
    const char *c_pkg_name = (*env)->GetStringUTFChars(env, pkg_name, NULL);

    pthread_mutex_lock(&libavos.mtx);
    libavos_acquire(c_pkg_name, has_pluginlib);
    pthread_mutex_unlock(&libavos.mtx);
}

void
Java_com_archos_medialib_LibAvos_nativeDebugInit(JNIEnv *env)
{
    pthread_mutex_lock(&libavos.mtx);
    libavos_debug_acquire();
    pthread_mutex_unlock(&libavos.mtx);
}

void
Java_com_archos_medialib_LibAvos_nativeLoadLibraryRTLDGlobal(JNIEnv *env, jobject thiz, jstring lib)
{
    if (lib) {
        const char *c_lib = (*env)->GetStringUTFChars(env, lib, NULL);

        pthread_mutex_lock(&libavos.mtx);
        libavos_debug_acquire();
        dlopen(c_lib, RTLD_GLOBAL);
        pthread_mutex_unlock(&libavos.mtx);
    }
}


void
Java_com_archos_medialib_LibAvos_nativeAvsh(JNIEnv *env, jobject thiz, jstring cmd)
{
    if (cmd) {
        const char *c_cmd = (*env)->GetStringUTFChars(env, cmd, NULL);

        pthread_mutex_lock(&libavos.mtx);
		libavos_debug_acquire();
        libavos_avsh(c_cmd);
        pthread_mutex_unlock(&libavos.mtx);
    }
}


void
Java_com_archos_medialib_LibAvos_nativeSetSubtitlePath(JNIEnv *env, jobject thiz, jstring path)
{
    if (path) {
        const char *c_path = (*env)->GetStringUTFChars(env, path, NULL);

        pthread_mutex_lock(&libavos.mtx);
        libavos_set_subtitlepath(c_path);
        pthread_mutex_unlock(&libavos.mtx);
    }
}

void
Java_com_archos_medialib_LibAvos_nativeSetDecoder(JNIEnv *env, jobject thiz, jint decoder)
{
    pthread_mutex_lock(&libavos.mtx);
    libavos_set_decoder(decoder);
    pthread_mutex_unlock(&libavos.mtx);
}

void
Java_com_archos_medialib_LibAvos_nativeSetDolbyVisionMode(JNIEnv *env, jobject thiz, jint mode)
{
    pthread_mutex_lock(&libavos.mtx);
    libavos_set_dolby_vision_mode(mode);
    pthread_mutex_unlock(&libavos.mtx);
}

void
Java_com_archos_medialib_LibAvos_nativeSetDolbyVisionPlaneScaler(JNIEnv *env, jobject thiz, jint scaler)
{
    pthread_mutex_lock(&libavos.mtx);
    libavos_set_dolby_vision_plane_scaler(scaler);
    pthread_mutex_unlock(&libavos.mtx);
}

void
Java_com_archos_medialib_LibAvos_nativeSetDolbyVisionTargetNits(JNIEnv *env, jobject thiz, jfloat nits)
{
    pthread_mutex_lock(&libavos.mtx);
    libavos_set_dolby_vision_target_nits((float) nits);
    pthread_mutex_unlock(&libavos.mtx);
}


void
Java_com_archos_medialib_LibAvos_nativeSetAudioInterface(JNIEnv *env, jobject thiz, jint audio_interface)
{
    pthread_mutex_lock(&libavos.mtx);
    libavos_set_audio_interface(audio_interface);
    pthread_mutex_unlock(&libavos.mtx);
}

void
Java_com_archos_medialib_LibAvos_nativeSetAudioDecoder(JNIEnv *env, jobject thiz, jint audio_decoder)
{
    pthread_mutex_lock(&libavos.mtx);
    libavos_set_audio_decoder(audio_decoder);
    pthread_mutex_unlock(&libavos.mtx);
}

void
Java_com_archos_medialib_LibAvos_nativeSetCodepage(JNIEnv *env, jobject thiz, jint codepage)
{
    pthread_mutex_lock(&libavos.mtx);
    libavos_set_codepage(codepage);
    pthread_mutex_unlock(&libavos.mtx);
}

void
Java_com_archos_medialib_LibAvos_nativeSetOutputSampleRate(JNIEnv *env, jobject thiz, jint sample_rate)
{
    pthread_mutex_lock(&libavos.mtx);
    libavos_set_output_sample_rate(sample_rate);
    pthread_mutex_unlock(&libavos.mtx);
}

void
Java_com_archos_medialib_LibAvos_nativeSetPassthrough(JNIEnv *env, jobject thiz, jint force_passthrough)
{
    pthread_mutex_lock(&libavos.mtx);
    libavos_set_passthrough(force_passthrough);
    pthread_mutex_unlock(&libavos.mtx);
}

void
Java_com_archos_medialib_LibAvos_nativeSetHdmiSupportedAudioCodecs(JNIEnv *env, jobject thiz, jlong hdmi_audio_codecs_flag)
{
    pthread_mutex_lock(&libavos.mtx);
    libavos_set_hdmi_supported_audio_codecs(hdmi_audio_codecs_flag);
    pthread_mutex_unlock(&libavos.mtx);
}

void
Java_com_archos_medialib_LibAvos_nativeSetMediaCodecAudioCapabilities(JNIEnv *env, jobject thiz, jlong media_codec_audio_capabilities)
{
    pthread_mutex_lock(&libavos.mtx);
    libavos_set_mediacodec_audio_capabilities(media_codec_audio_capabilities);
    pthread_mutex_unlock(&libavos.mtx);
}

void
Java_com_archos_medialib_LibAvos_nativeSetSpatializerCapabilities(JNIEnv *env, jobject thiz, jint spatializer_capabilities)
{
    pthread_mutex_lock(&libavos.mtx);
    libavos_set_spatializer_capabilities(spatializer_capabilities);
    pthread_mutex_unlock(&libavos.mtx);
}

void
Java_com_archos_medialib_LibAvos_nativeSetSpatializerEnabled(JNIEnv *env, jobject thiz, jboolean enabled)
{
    pthread_mutex_lock(&libavos.mtx);
    libavos_set_spatializer_enabled(enabled ? 1 : 0);
    pthread_mutex_unlock(&libavos.mtx);
}

void
Java_com_archos_medialib_LibAvos_nativeSetMaxPcmChannels(JNIEnv *env, jobject thiz, jint max_channels)
{
    pthread_mutex_lock(&libavos.mtx);
    libavos_set_max_pcm_channels(max_channels);
    LOGV("nativeSetMaxPcmChannels(%d)\n", max_channels);
    pthread_mutex_unlock(&libavos.mtx);
}

void
Java_com_archos_medialib_LibAvos_nativeSetPcmChannelMasks(JNIEnv *env, jobject thiz, jintArray masks)
{
    jint *elems = NULL;
    jsize len = 0;

    if (masks) {
        len = (*env)->GetArrayLength(env, masks);
        if (len > 0) {
            elems = (*env)->GetIntArrayElements(env, masks, NULL);
        }
    }

    pthread_mutex_lock(&libavos.mtx);
    libavos_set_pcm_channel_masks((const int *)elems, (int)len);
    pthread_mutex_unlock(&libavos.mtx);

    if (masks && elems) {
        (*env)->ReleaseIntArrayElements(env, masks, elems, JNI_ABORT);
    }
}

void
Java_com_archos_medialib_LibAvos_nativeSetAudioSpeed(JNIEnv *env, jobject thiz, jfloat audio_speed)
{
    pthread_mutex_lock(&libavos.mtx);
    libavos_set_audio_speed(audio_speed);
    pthread_mutex_unlock(&libavos.mtx);
}

jfloat
Java_com_archos_medialib_LibAvos_nativeGetAudioSpeed(JNIEnv *env, jobject thiz)
{
    return (jfloat)libavos_get_audio_speed();
}

void
Java_com_archos_medialib_LibAvos_nativeSetAndroidFrameTiming(JNIEnv *env, jobject thiz, jboolean enable)
{
    pthread_mutex_lock(&libavos.mtx);
    libavos_set_android_frame_timing(enable);
    pthread_mutex_unlock(&libavos.mtx);
}

void
Java_com_archos_medialib_LibAvos_nativeEnableAudioSpeed(JNIEnv *env, jobject thiz, jboolean enable)
{
	pthread_mutex_lock(&libavos.mtx);
	libavos_enable_audio_speed(enable);
	pthread_mutex_unlock(&libavos.mtx);
}

void
Java_com_archos_medialib_LibAvos_nativeParserSyncMode(JNIEnv *env, jobject thiz, jint mode)
{
	pthread_mutex_lock(&libavos.mtx);
	libavos_set_parser_sync_mode(mode);
	pthread_mutex_unlock(&libavos.mtx);
}

void
Java_com_archos_medialib_LibAvos_nativeSetDownmix(JNIEnv *env, jobject thiz, jint downmix)
{
    pthread_mutex_lock(&libavos.mtx);
    libavos_set_downmix(downmix);
    pthread_mutex_unlock(&libavos.mtx);
}

void
Java_com_archos_medialib_LibAvos_nativeSetDynamicAudioDelay(JNIEnv *env, jobject thiz, jboolean enable)
{
    pthread_mutex_lock(&libavos.mtx);
    libavos_set_dynamic_audio_delay(enable);
    pthread_mutex_unlock(&libavos.mtx);
}

void
Java_com_archos_medialib_LibAvos_nativeSetStreamMaxIframeSize(JNIEnv *env, jobject thiz, jint size)
{
	pthread_mutex_lock(&libavos.mtx);
	libavos_set_default_stream_max_iframe_size(size);
	pthread_mutex_unlock(&libavos.mtx);
}

void
Java_com_archos_medialib_LibAvos_nativeSetStreamBufferSize(JNIEnv *env, jobject thiz, jint size)
{
	pthread_mutex_lock(&libavos.mtx);
	libavos_set_default_stream_buffer_size(size);
	pthread_mutex_unlock(&libavos.mtx);
}

void
Java_com_archos_medialib_LibAvos_nativeDisableAtempoFilter(JNIEnv *env, jobject thiz, jboolean disable)
{
	pthread_mutex_lock(&libavos.mtx);
	libavos_disable_atempo_filter(disable);
	pthread_mutex_unlock(&libavos.mtx);
}

int libavos_transform_audio_impl(float* samples, int size)
{
    if (!fields.AudioTransformerObj || !fields.AudioTransformer_transformAudioMethod) {
        return 0;
    }
    JNIEnv* env;
    if ((*myVm)->AttachCurrentThread(myVm, &env, NULL) != JNI_OK) {
        return 0;
    }
    jfloatArray input = (*env)->NewFloatArray(env, size);
    if (!input) {
        (*myVm)->DetachCurrentThread(myVm);
        return 0;
    }
    (*env)->SetFloatArrayRegion(env, input, 0, size, samples);
    jfloatArray output = (jfloatArray)(*env)->CallObjectMethod(env, fields.AudioTransformerObj, fields.AudioTransformer_transformAudioMethod, input);
    (*env)->DeleteLocalRef(env, input);
    if (output) {
        int len = (*env)->GetArrayLength(env, output);
        if (len == size) {
            (*env)->GetFloatArrayRegion(env, output, 0, size, samples);
        }
        (*env)->DeleteLocalRef(env, output);
    }
    (*myVm)->DetachCurrentThread(myVm);
    return 1;
}

void
Java_com_archos_medialib_LibAvos_nativeSetAudioTransformer(JNIEnv *env, jobject thiz, jobject transformer)
{
    if (fields.AudioTransformerObj) {
        (*env)->DeleteGlobalRef(env, fields.AudioTransformerObj);
        fields.AudioTransformerObj = NULL;
        fields.AudioTransformer_transformAudioMethod = NULL;
    }
    if (transformer) {
        fields.AudioTransformerObj = (*env)->NewGlobalRef(env, transformer);
        jclass clazz = (*env)->GetObjectClass(env, transformer);
        fields.AudioTransformer_transformAudioMethod = (*env)->GetMethodID(env, clazz, "transformAudio", "([F)[F");
	libavos_set_audio_transform(libavos_transform_audio_impl);
    } else {
	libavos_set_audio_transform(NULL);
    }
}

jobject create_bitmap(JNIEnv *env, avos_bgra_bitmap_t *avos_bitmap, uint32_t out_width, uint32_t out_height)
{
    jintArray array;
    size_t ints_len;

    LOGV("avos_bitmap: %dx%d - %d -> %dx%d\n",
        avos_bitmap->width, avos_bitmap->height, avos_bitmap->linestep,
        out_width, out_height);

    if (!avos_bitmap || !avos_bitmap->data || avos_bitmap->data_size == 0) {
        LOGE("create_bitmap: invalid bitmap input");
        return NULL;
    }

    if ((avos_bitmap->data_size % sizeof(jint)) != 0) {
        LOGE("create_bitmap: data_size %u not multiple of %zu", avos_bitmap->data_size, sizeof(jint));
        return NULL;
    }
    ints_len = avos_bitmap->data_size / sizeof(jint);
    if (ints_len > INT_MAX) {
        LOGE("create_bitmap: data too large (%zu ints)", ints_len);
        return NULL;
    }

    array = (*env)->NewIntArray(env, (jsize)ints_len);
    if (!array)
        return NULL;
    // Use SetIntArrayRegion instead of GetIntArrayElements: on large frames
    // (up to 8K source resolution) GetIntArrayElements can fail to allocate
    // its pinned/copy buffer under memory pressure, and that failure throws
    // a C++ exception inside ART that cannot unwind through this JNI code,
    // aborting the process. SetIntArrayRegion reports the same out-of-memory
    // condition as a normal pending Java exception instead.
    (*env)->SetIntArrayRegion(env, array, 0, (jsize)ints_len, (const jint *)avos_bitmap->data);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, array);
        return NULL;
    }

    jobject jBitmap = (*env)->CallStaticObjectMethod(env, fields.AvosBitmapHelperClazz,
                                       fields.AvosBitmapHelper_createRGBBitmapMethod,
                                       array,
                                       avos_bitmap->width, avos_bitmap->height,
                                       avos_bitmap->linestep,
                                       avos_bitmap->rotation,
                                       out_width, out_height);
    (*env)->DeleteLocalRef(env, array);

    return jBitmap;
}
