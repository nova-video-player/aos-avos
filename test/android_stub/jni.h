/*
 * Minimal stand-in for the NDK's top-level <jni.h> (real NDK layout: jni.h sits at the
 * include root, not under android/). Only the function-table entries jni_sub_engine.c
 * actually calls are declared -- JNIEnv/JavaVM are (like the real headers) pointers to
 * const function-table structs, so a test provides its OWN table (see
 * jni_listener_lifecycle_test.c's mock JavaVM) by filling in just the slots it cares about;
 * this header only fixes the layout both sides agree on.
 */
#ifndef ANDROID_STUB_JNI_H
#define ANDROID_STUB_JNI_H
#include <stdint.h>

typedef int32_t  jint;
typedef int64_t  jlong;
typedef float    jfloat;
typedef unsigned char jboolean;
typedef void    *jobject, *jstring, *jlongArray, *jclass, *jmethodID;

#define JNI_TRUE  1
#define JNI_FALSE 0
#define JNI_OK        0
#define JNI_EDETACHED (-2)
#define JNI_VERSION_1_4 0x00010004
#define JNIEXPORT
#define JNICALL

struct JNINativeInterface_;
typedef const struct JNINativeInterface_ *JNIEnv;
struct JNIInvokeInterface_;
typedef const struct JNIInvokeInterface_ *JavaVM;

typedef struct { jint version; const char *name; jobject group; } JavaVMAttachArgs;

struct JNIInvokeInterface_ {
    jint (*GetEnv)(JavaVM *vm, void **penv, jint version);
    jint (*AttachCurrentThread)(JavaVM *vm, JNIEnv **penv, void *args);
    jint (*DetachCurrentThread)(JavaVM *vm);
};

struct JNINativeInterface_ {
    const char *(*GetStringUTFChars)(JNIEnv *env, jstring str, jboolean *isCopy);
    void        (*ReleaseStringUTFChars)(JNIEnv *env, jstring str, const char *utf);
    jint        (*GetArrayLength)(JNIEnv *env, jlongArray array);
    void        (*SetLongArrayRegion)(JNIEnv *env, jlongArray array, jint start, jint len, const jlong *buf);
    jint        (*GetJavaVM)(JNIEnv *env, JavaVM **vm);
    jclass      (*GetObjectClass)(JNIEnv *env, jobject obj);
    jmethodID   (*GetMethodID)(JNIEnv *env, jclass clazz, const char *name, const char *sig);
    void        (*DeleteLocalRef)(JNIEnv *env, jobject obj);
    jobject     (*NewGlobalRef)(JNIEnv *env, jobject obj);
    void        (*DeleteGlobalRef)(JNIEnv *env, jobject globalRef);
    void        (*CallVoidMethod)(JNIEnv *env, jobject obj, jmethodID methodID, ...);
    jboolean    (*ExceptionCheck)(JNIEnv *env);
    void        (*ExceptionDescribe)(JNIEnv *env);
    void        (*ExceptionClear)(JNIEnv *env);
};
#endif
