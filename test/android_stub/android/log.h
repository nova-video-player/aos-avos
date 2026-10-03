/*
 * Minimal stand-in for the NDK's <android/log.h>. Only ANDROID_LOG_WARN and
 * __android_log_print() are used (by jni_sub_engine.c's JNI_LOGW / REG_LOGW-style macros);
 * the stub body (android_stub.c) discards the message. Diagnostic logging only -- never
 * gates control flow in the files these tests link, same rationale test_stubs.c gives for
 * stubbing serprintf().
 */
#ifndef ANDROID_STUB_LOG_H
#define ANDROID_STUB_LOG_H
enum { ANDROID_LOG_WARN = 5 };
int __android_log_print(int prio, const char *tag, const char *fmt, ...);
#endif
