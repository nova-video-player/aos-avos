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

#define LOG_TAG "avos_media_metadata_retriever"

#include <dlfcn.h>
#include <stdio.h>
#include <pthread.h>

#include "jni.h"

#include "libavos.h"

static const avos_mr_handle_t *avos = NULL;
static const avos_metadata_handle_t *avos_metadata = NULL;

static struct {
    jfieldID    handle;
} mr_fields;

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

int
register_avosmediametadataretriever(JNIEnv *env)
{
    jclass clazz;

    clazz = (*env)->FindClass(env, "com/archos/medialib/AvosMediaMetadataRetriever");
    if (!clazz)
        return -1;
    mr_fields.handle = (*env)->GetFieldID(env, clazz, "mMediaMetadataRetrieverHandle", "J");
    if (!mr_fields.handle)
        return -1;

    avos = avos_mr_get_handle();
    avos_metadata = avos_metadata_get_handle();
    return 0;
}

int
unregister_avosmediametadataretriever(JNIEnv *env)
{
    return 0;
}

static inline int handle_ret(JNIEnv *env, int ret, const char *cmd)
{
    if (ret == AVOS_ERR_OK) {
        return ret;
    } else {
        char err_msg[256];

        snprintf(err_msg, 256, "%s returned %s", cmd,
          ret == AVOS_ERR ? "AVOS_ERR" : "AVOS_ERR_CRITICAL");
        jniThrowException(env, "java/lang/IllegalStateException", err_msg);
        return ret;
    }
}

#define CHECK(cmd) handle_ret(env, (cmd), #cmd)

static inline void set_mr(JNIEnv *env, jobject thiz, avos_mr_t *mr)
{
    pthread_mutex_lock(&mtx);
    (*env)->SetLongField(env, thiz, mr_fields.handle, (jlong) mr);
    pthread_mutex_unlock(&mtx);
}

static inline avos_mr_t *get_mr(JNIEnv *env, jobject thiz)
{
    avos_mr_t *mr;
    pthread_mutex_lock(&mtx);
    mr = (avos_mr_t *)(*env)->GetLongField(env, thiz, mr_fields.handle);
    pthread_mutex_unlock(&mtx);
    return mr;
}

static inline avos_mr_t *get_mr_or_throw(JNIEnv *env, jobject thiz)
{
    avos_mr_t *mr = get_mr(env, thiz);
    if (!mr)
        jniThrowException(env, "java/lang/IllegalStateException", "no mr object");
    return mr;
}

void
Java_com_archos_medialib_AvosMediaMetadataRetriever_create(JNIEnv *env, jobject thiz, jobject weak_thiz)
{
    avos_mr_t *mr;

    if (!avos)
        goto err;
    mr = avos->create();
    if (!mr)
        goto err;

    set_mr(env, thiz, mr);
    return;
err:
    jniThrowException(env, "java/lang/IllegalStateException", "can't create mr");
}

void
Java_com_archos_medialib_AvosMediaMetadataRetriever_nativeRelease(JNIEnv *env, jobject thiz)
{
    avos_mr_t *mr = get_mr(env, thiz);
    if (!mr)
        return;
    LOGV("nativeRelease\n");

    avos->destroy(mr);

    set_mr(env, thiz, NULL);
}

void
Java_com_archos_medialib_AvosMediaMetadataRetriever_setDataSource(JNIEnv *env, jobject thiz, jstring path, jobjectArray keys, jobjectArray values)
{
    avos_mr_t *mr = get_mr_or_throw(env, thiz);
    if (!mr)
        return;

    if (path == NULL) {
        jniThrowException(env, "java/lang/IllegalArgumentException", "path is NULL");
        return;
    }

    const char *c_path = (*env)->GetStringUTFChars(env, path, NULL);
    if (c_path == NULL) {
        return;
    }
    CHECK(avos->setdatasource(mr, c_path, NULL, NULL));
}

void
Java_com_archos_medialib_AvosMediaMetadataRetriever_setDataSourceFD(JNIEnv *env, jobject thiz, jobject fileDescriptor, jlong offset, jlong length)
{
    avos_mr_t *mr = get_mr_or_throw(env, thiz);
    if (!mr)
        return;

    if (fileDescriptor == NULL) {
        jniThrowException(env, "java/lang/IllegalArgumentException", "fileDescriptor is NULL");
        return;
    }
    int fd = jniGetFDFromFileDescriptor(env, fileDescriptor);
    CHECK(avos->setdatasource_fd(mr, fd, offset, length));
}

jobject
Java_com_archos_medialib_AvosMediaMetadataRetriever_extractMetadata(JNIEnv *env, jobject thiz, jint keyCode)
{
    const char *str;
    avos_mr_t *mr = get_mr_or_throw(env, thiz);
    if (!mr) return NULL;
    str = avos->extractmetadata(mr, keyCode);
    if (str)
        return (*env)->NewStringUTF(env, str);
    else
        return NULL;
}

jbyteArray
Java_com_archos_medialib_AvosMediaMetadataRetriever_getMetadata(JNIEnv *env, jobject thiz)
{
    metadata_buffer_t *buffer = NULL;
    avos_mr_t *mr = get_mr_or_throw(env, thiz);
    jbyteArray array = NULL;
    if (!mr) return NULL;

    if (avos->getmetadata(mr, &buffer))
        return NULL;

    array =  (*env)->NewByteArray(env, avos_metadata->size(buffer));
    if (!array)
	    goto end;

    jbyte* bytes = (*env)->GetByteArrayElements(env, array, NULL);
    if (!bytes)
	    goto end;

    memcpy(bytes, avos_metadata->data(buffer), avos_metadata->size(buffer));

    (*env)->ReleaseByteArrayElements(env, array, bytes, 0);
end:
    if (buffer)
	    avos_metadata->destroy(&buffer);
    return array;
}

#define ANDROID_THUMB_WIDTH 512

jobject
Java_com_archos_medialib_AvosMediaMetadataRetriever_nativeGetFrameAtTime(JNIEnv *env, jobject thiz, jlong timeUs, jint option)
{
    avos_bgra_bitmap_t *frame = NULL;
    avos_mr_t *mr = get_mr_or_throw(env, thiz);
    jobject bitmap;
    float scale;

    if (!mr) return NULL;
    CHECK(avos->getframe(mr, timeUs == -1 ? -1 : timeUs / 1000, &frame));
    if (!frame)
        return NULL;
    scale = (float)ANDROID_THUMB_WIDTH / (float)frame->width;
    bitmap = create_bitmap(env, frame, ANDROID_THUMB_WIDTH, (uint32_t)(frame->height * scale));
    free(frame);
    return bitmap;
}

jbyteArray
Java_com_archos_medialib_AvosMediaMetadataRetriever_getEmbeddedPicture(JNIEnv *env, jobject thiz, jint pictureType)
{
    LOGV("getEmbeddedPicture: %d", pictureType);
    avos_mr_t *mr = get_mr_or_throw(env, thiz);
    avos_apic_t *apic;
    jbyteArray array;

    if (!mr) return NULL;
    CHECK(avos->getapic(mr, &apic));
    if (!apic)
        return NULL;

    array = (*env)->NewByteArray(env, apic->size);
    if (!array) {  // OutOfMemoryError exception has already been thrown.
        LOGE("getEmbeddedPicture: OutOfMemoryError is thrown.");
    } else {
        jbyte* bytes = (*env)->GetByteArrayElements(env, array, NULL);
        if (bytes != NULL) {
            memcpy(bytes, apic->data, apic->size);
            (*env)->ReleaseByteArrayElements(env, array, bytes, 0);
        }
    }
    free(apic);

    // No need to delete mediaAlbumArt here
    return array;
}
