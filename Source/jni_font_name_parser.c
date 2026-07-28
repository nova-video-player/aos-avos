#include "jni_font_name_parser.h"
#include "font_name_parser.h"
#include <jni.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <android/log.h>

#define LOG_TAG "JniFontNameParser"
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

// Encodes one FONT_NAME_ENTRY as "family\x01style\x01faceIndex\x01namedInstance" for the
// Java side (FontNameParser.java) to split back apart. \x01 (SOH, a control character) is
// used as the delimiter rather than something printable like '|' or ',' because a real font
// family name CAN legitimately contain a comma (confirmed via fc-scan: some fonts report
// "Roboto,Roboto Medium" as a single combined name-table value) -- a delimiter that can
// appear in the data itself would silently corrupt the split on the Java side, whereas a
// control character never legitimately appears in a font family/style string.
static jstring encode_entry(JNIEnv *env, const FONT_NAME_ENTRY *e) {
    char buf[600]; // family[256] + style[256] + delimiters + two small ints, comfortably bounded
    snprintf(buf, sizeof(buf), "%s\x01%s\x01%d\x01%d", e->family, e->style, e->face_index, e->named_instance);
    return (*env)->NewStringUTF(env, buf);
}

JNIEXPORT jobjectArray JNICALL
Java_com_archos_mediacenter_video_utils_FontNameParser_nativeParseFontFile(JNIEnv *env, jclass clazz, jstring jpath) {
    if (!jpath) return NULL;

    const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
    if (!path) return NULL;

    FONT_NAME_RESULT result;
    FONT_NAME_STATUS status = font_name_parse_file(path, &result);

    if (status != FONT_NAME_OK || result.count == 0) {
        LOGW("nativeParseFontFile: '%s' -> %s", path, font_name_status_string(status));
        (*env)->ReleaseStringUTFChars(env, jpath, path);
        // Return a zero-length array rather than NULL -- FontNameParser.parse() on the Java
        // side treats null and empty identically, but returning a real (empty) array here
        // is a cleaner JNI contract: callers never need a null-check before iterating.
        jclass stringClass = (*env)->FindClass(env, "java/lang/String");
        return (*env)->NewObjectArray(env, 0, stringClass, NULL);
    }

    jclass stringClass = (*env)->FindClass(env, "java/lang/String");
    jobjectArray arr = (*env)->NewObjectArray(env, result.count, stringClass, NULL);

    for (int i = 0; i < result.count; i++) {
        jstring entry = encode_entry(env, &result.entries[i]);
        (*env)->SetObjectArrayElement(env, arr, i, entry);
        (*env)->DeleteLocalRef(env, entry);
    }

    (*env)->ReleaseStringUTFChars(env, jpath, path);
    return arr;
}
