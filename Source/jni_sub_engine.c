#include "jni_sub_engine.h"
#include "sub_engine.h"
#include "sub_engine_registry.h"
#include <android/native_window_jni.h>
#include <android/bitmap.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <android/log.h>

#define JNI_TAG "SubEngineJNI"
#define JNI_LOGW(...) __android_log_print(ANDROID_LOG_WARN, JNI_TAG, __VA_ARGS__)

// Helper to extract the engine pointer
static SUB_ENGINE* get_engine(jlong handle) {
    return (SUB_ENGINE*)(intptr_t)handle;
}

// --- NATIVE -> JAVA "SUBTITLE CONTENT CHANGED" PUSH (3D path) ---
//
// The engine's render thread calls on_content_changed() (see sub_render_change_cb in
// sub_render_gl.h for exactly when: 3D mode, paused, generation moved). It reaches Java's
// SubtitleEngine.onNativeSubtitleContentChanged() through a global ref to the SubtitleEngine
// instance and a jmethodID resolved up front in nativeCreate() -- resolved there, on a Java
// thread, deliberately: FindClass() from a native thread only sees the system class loader
// (the same reason libavos.c caches its classes in JNI_OnLoad), whereas GetMethodID() on an
// object we already hold needs no class lookup at all.
//
// Lifetime: the listener (and its global ref) is freed in nativeDestroy() strictly AFTER
// sub_engine_destroy() returns. That call joins the render thread, so no on_content_changed()
// can still be running -- there is no window in which the thread uses a freed ref.
typedef struct {
    JavaVM   *vm;
    jobject   obj;   // global ref to the SubtitleEngine
    jmethodID mid;   // void onNativeSubtitleContentChanged()
} SUB_JNI_LISTENER;

static pthread_key_t  s_detach_key;
static pthread_once_t s_detach_key_once = PTHREAD_ONCE_INIT;

// Runs when an attached native thread exits. ART aborts a thread that exits while still
// attached, so every thread we attach must detach -- the documented pthread-key pattern.
static void detach_thread_at_exit(void *vm) {
    if (vm) (*(JavaVM *)vm)->DetachCurrentThread((JavaVM *)vm);
}
static void make_detach_key(void) {
    pthread_key_create(&s_detach_key, detach_thread_at_exit);
}

static void on_content_changed(void *ctx) {
    SUB_JNI_LISTENER *l = (SUB_JNI_LISTENER *)ctx;
    JNIEnv *env = NULL;

    jint rc = (*l->vm)->GetEnv(l->vm, (void **)&env, JNI_VERSION_1_4);
    if (rc == JNI_EDETACHED) {
        // First call from the render thread: attach once, stay attached, detach at thread exit.
        JavaVMAttachArgs args = { JNI_VERSION_1_4, "SubRender", NULL };
        if ((*l->vm)->AttachCurrentThread(l->vm, &env, &args) != JNI_OK) {
            JNI_LOGW("content-changed push: AttachCurrentThread failed");
            return;
        }
        pthread_once(&s_detach_key_once, make_detach_key);
        pthread_setspecific(s_detach_key, l->vm);
    } else if (rc != JNI_OK) {
        return;
    }

    (*env)->CallVoidMethod(env, l->obj, l->mid);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
    }
}

// Best effort: if anything fails (or the Java method has been stripped/renamed by shrinking)
// the push is simply off and the 3D path behaves as before -- pulls on video frames and
// style changes. Never fails engine creation.
static void install_change_listener(JNIEnv *env, jobject thiz, SUB_ENGINE *eng) {
    SUB_JNI_LISTENER *l = (SUB_JNI_LISTENER *)calloc(1, sizeof(*l));
    if (!l) return;
    if ((*env)->GetJavaVM(env, &l->vm) != JNI_OK) { free(l); return; }

    jclass cls = (*env)->GetObjectClass(env, thiz);
    l->mid = cls ? (*env)->GetMethodID(env, cls, "onNativeSubtitleContentChanged", "()V") : NULL;
    if (cls) (*env)->DeleteLocalRef(env, cls);
    if (!l->mid) {
        (*env)->ExceptionClear(env); // NoSuchMethodError from GetMethodID
        JNI_LOGW("SubtitleEngine.onNativeSubtitleContentChanged() not found (kept by shrinker?) -- 3D push disabled");
        free(l);
        return;
    }
    l->obj = (*env)->NewGlobalRef(env, thiz);
    if (!l->obj) { free(l); return; }

    sub_engine_set_change_callback(eng, on_content_changed, l);
}

// --- LIFECYCLE & SURFACE ---

JNIEXPORT jlong JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeCreate(JNIEnv *env, jobject thiz) {
    SUB_ENGINE *eng = sub_engine_create();
    if (eng) install_change_listener(env, thiz, eng);
    sub_engine_registry_publish(eng);
    return (jlong)(intptr_t)eng;
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeDestroy(JNIEnv *env, jobject thiz, jlong handle) {
    SUB_ENGINE *eng = get_engine(handle);
    // Retract FIRST and block until every STREAM that had acquired a
    // reference to this exact engine has released it (see
    // sub_engine_registry.h). Only once that's guaranteed is it safe to
    // free the engine below -- this is what makes it impossible for a
    // STREAM::sub_engine snapshot to outlive the memory it points to.
    sub_engine_registry_retract(eng);

    // Read the listener BEFORE destroy (the engine is freed by it), free it AFTER: destroy joins
    // the render thread, the only caller of on_content_changed(), so nothing can use it later.
    SUB_JNI_LISTENER *l = (SUB_JNI_LISTENER *)sub_engine_get_change_ctx(eng);
    sub_engine_destroy(eng);
    if (l) {
        (*env)->DeleteGlobalRef(env, l->obj);
        free(l);
    }
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSurfaceCreated(JNIEnv *env, jobject thiz, jlong handle, jobject surface) {
    ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
    sub_engine_attach_surface(get_engine(handle), window);
    // ANativeWindow_fromSurface acquires a ref. We can release ours so it destroys when the Surface does.
    if (window) ANativeWindow_release(window);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSurfaceChanged(JNIEnv *env, jobject thiz, jlong handle, jint width, jint height) {
    sub_engine_surface_resized(get_engine(handle), width, height);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSurfaceDestroyed(JNIEnv *env, jobject thiz, jlong handle) {
    sub_engine_detach_surface(get_engine(handle));
}

// Reports where the video's own on-screen box sits within the subtitle canvas -- see
// sub_engine_set_video_box()'s doc comment in sub_engine.h. Called by
// SurfaceController/SubtitleEngine.setVideoBox() whenever that geometry is recomputed
// (rotation, use_sub_margins toggling, a new video's aspect ratio).
JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetVideoBox(JNIEnv *env, jobject thiz, jlong handle, jint x, jint y, jint w, jint h) {
    sub_engine_set_video_box(get_engine(handle), x, y, w, h);
}

// --- 3D HYBRID RENDER BRIDGE ---

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetUIMode(JNIEnv *env, jobject thiz, jlong handle, jint mode) {
    sub_engine_set_ui_mode(get_engine(handle), mode);
}

// The single pull for the 3D CPU-blend path. Replaces the old trio of
// nativeFillBitmap / nativeSyncFillBitmap / nativeGetSubtitleGeneration, whose separate
// calls let the generation Java recorded describe a different frame than the pixels it had
// just copied.
//
// Returns one of SUB_FILL_* (sub_engine.h; Java mirrors the values as FILL_*):
//   UNCHANGED  the frame identified by `lastGeneration` is still current; bitmap untouched.
//   CLEAR      nothing should be showing; bitmap untouched (Java clears its own surface).
//   FRAME      bitmap was cleared and the current frame blended into it.
//   ERROR      bad handle/bitmap; nothing was decided. Java must not record anything.
// out_generation[0] is set for every non-ERROR result to the generation of the exact frame
// (or absence of one) that result describes -- decided in the same native critical section
// as the pixel copy, so Java can record it as "what I posted" with no further JNI call.
//
// sync=true is the explicit "redraw right now" path (style changes -- infrequent, user
// driven): force a fresh render, block (bounded) until the render thread has applied it,
// and never answer UNCHANGED. sync=false is the cheap once-per-video-frame pull
// (VideoEffectRenderer.onFrameAvailable, 30-60x/sec): no waiting, and UNCHANGED is the
// common answer. Pass lastGeneration = -1 for "nothing valid posted yet".
JNIEXPORT jint JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeFillBitmap(
        JNIEnv *env, jobject thiz, jlong handle, jobject jbitmap,
        jlong lastGeneration, jboolean sync, jlongArray outGeneration) {
    SUB_ENGINE *eng = get_engine(handle);
    if (!eng || !jbitmap || !outGeneration) return SUB_FILL_ERROR;
    if ((*env)->GetArrayLength(env, outGeneration) < 1) return SUB_FILL_ERROR;

    if (sync) {
        // 50ms is well above the render thread's normal wake latency (a mutex + broadcast +
        // context switch) and still cheap enough not to be felt as jank in the rare case it's
        // actually hit (e.g. the render thread is briefly starved under load).
        uint64_t target_gen = sub_engine_force_wake_and_get_generation(eng);
        sub_engine_wait_for_render(eng, target_gen, /*timeout_ms=*/50);
    }

    AndroidBitmapInfo info;
    void *pixels = NULL;
    if (AndroidBitmap_getInfo(env, jbitmap, &info) < 0) return SUB_FILL_ERROR;
    if (info.format != ANDROID_BITMAP_FORMAT_RGBA_8888) return SUB_FILL_ERROR;
    if (AndroidBitmap_lockPixels(env, jbitmap, &pixels) < 0 || !pixels) return SUB_FILL_ERROR;

    // The clear-to-transparent that used to happen here now happens inside the fill, and only
    // when a frame is actually blended.
    uint64_t gen = 0;
    SUB_FILL_RESULT rc = sub_engine_fill_bitmap(eng, pixels, (int)info.width, (int)info.height,
                                                (int)info.stride, (uint64_t)lastGeneration,
                                                sync ? 1 : 0, &gen);
    AndroidBitmap_unlockPixels(env, jbitmap);

    if (rc != SUB_FILL_ERROR) {
        jlong g = (jlong)gen;
        (*env)->SetLongArrayRegion(env, outGeneration, 0, 1, &g);
    }
    return (jint)rc;
}

// --- TYPOGRAPHY & MASTER CONTROL ---

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetFontSize(JNIEnv *env, jobject thiz, jlong handle, jfloat pt) {
    SUB_ENGINE *eng = get_engine(handle);
    if (!eng) return;
    sub_style_set_font_size(sub_engine_get_style(eng), pt);
    sub_engine_force_wake(eng);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetFontScale(JNIEnv *env, jobject thiz, jlong handle, jfloat scale) {
    SUB_ENGINE *eng = get_engine(handle);
    if (!eng) return;
    sub_style_set_font_scale(sub_engine_get_style(eng), scale);
    sub_engine_force_wake(eng);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetFontFamily(JNIEnv *env, jobject thiz, jlong handle, jstring familyName) {
    if (!familyName) return;
    SUB_ENGINE *eng = get_engine(handle);
    if (!eng) return;
    const char *str = (*env)->GetStringUTFChars(env, familyName, 0);
    sub_style_set_font_family(sub_engine_get_style(eng), str);
    (*env)->ReleaseStringUTFChars(env, familyName, str);
    sub_engine_force_wake(eng);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetBold(JNIEnv *env, jobject thiz, jlong handle, jboolean bold) {
    SUB_ENGINE *eng = get_engine(handle);
    if (!eng) return;
    sub_style_set_bold(sub_engine_get_style(eng), bold);
    sub_engine_force_wake(eng);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetTextColor(JNIEnv *env, jobject thiz, jlong handle, jint color) {
    SUB_ENGINE *eng = get_engine(handle);
    if (!eng) return;
    sub_style_set_text_color(sub_engine_get_style(eng), (uint32_t)color);
    sub_engine_force_wake(eng);
}

// --- BORDERS, SHADOWS, AND BACKGROUNDS ---

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetOutlineColor(JNIEnv *env, jobject thiz, jlong handle, jint color) {
    SUB_ENGINE *eng = get_engine(handle);
    if (!eng) return;
    sub_style_set_outline_color(sub_engine_get_style(eng), (uint32_t)color);
    sub_engine_force_wake(eng);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetOutlineWidth(JNIEnv *env, jobject thiz, jlong handle, jfloat px) {
    SUB_ENGINE *eng = get_engine(handle);
    if (!eng) return;
    sub_style_set_outline_width(sub_engine_get_style(eng), px);
    sub_engine_force_wake(eng);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetShadowColor(JNIEnv *env, jobject thiz, jlong handle, jint color) {
    SUB_ENGINE *eng = get_engine(handle);
    if (!eng) return;
    sub_style_set_shadow_color(sub_engine_get_style(eng), (uint32_t)color);
    sub_engine_force_wake(eng);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetShadowWidth(JNIEnv *env, jobject thiz, jlong handle, jfloat px) {
    SUB_ENGINE *eng = get_engine(handle);
    if (!eng) return;
    sub_style_set_shadow_width(sub_engine_get_style(eng), px);
    sub_engine_force_wake(eng);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetBackgroundMode(JNIEnv *env, jobject thiz, jlong handle, jint mode) {
    SUB_ENGINE *eng = get_engine(handle);
    if (!eng) return;
    sub_style_set_bg_mode(sub_engine_get_style(eng), mode);
    sub_engine_force_wake(eng);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetBackgroundColor(JNIEnv *env, jobject thiz, jlong handle, jint color) {
    SUB_ENGINE *eng = get_engine(handle);
    if (!eng) return;
    sub_style_set_bg_color(sub_engine_get_style(eng), (uint32_t)color);
    sub_engine_force_wake(eng);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetBackgroundOpacity(JNIEnv *env, jobject thiz, jlong handle, jfloat opacity) {
    SUB_ENGINE *eng = get_engine(handle);
    if (!eng) return;
    // Convert 0.0-1.0 float to 0-255 Android Alpha; sub_style_set_bg_opacity() does the
    // libass transparency-byte conversion and the locked read-modify-write on bg_color.
    uint8_t android_alpha = (uint8_t)(255.0f * opacity);
    sub_style_set_bg_opacity(sub_engine_get_style(eng), android_alpha);
    sub_engine_force_wake(eng);
}

// --- POSITIONING & OVERRIDES ---

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetVerticalOffset(JNIEnv *env, jobject thiz, jlong handle, jfloat pixels) {
    SUB_ENGINE *eng = get_engine(handle);
    if (!eng) return;
    sub_style_set_margin_bottom(sub_engine_get_style(eng), (int)pixels);
    sub_engine_force_wake(eng);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetOverrideMode(JNIEnv *env, jobject thiz, jlong handle, jint mode) {
    SUB_ENGINE *eng = get_engine(handle);
    if (!eng) return;
    sub_style_set_override_mode(sub_engine_get_style(eng), mode);
    sub_engine_force_wake(eng);
}

// --- CUSTOM FONTS FOLDER (third-party fonts dir, MX Player / mpv-android style) ---

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetFontsFolder(JNIEnv *env, jobject thiz, jlong handle, jstring dirPath) {
    SUB_ENGINE *eng = get_engine(handle);
    if (!eng) return;

    if (!dirPath) {
        // Same "clear" convention as nativeSetFontFamily would use if it
        // supported clearing: NULL jstring -> NULL C string -> feature off.
        sub_engine_set_fonts_dir(eng, NULL);
        return;
    }

    const char *path = (*env)->GetStringUTFChars(env, dirPath, 0);
    sub_engine_set_fonts_dir(eng, path);
    (*env)->ReleaseStringUTFChars(env, dirPath, path);
}

JNIEXPORT void JNICALL Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetDefaultFontName(JNIEnv *env, jobject thiz, jlong handle, jstring familyName) {
    SUB_ENGINE *eng = get_engine(handle);
    if (!eng) return;

    if (!familyName) {
        sub_engine_set_default_font_name(eng, NULL);
        return;
    }

    const char *name = (*env)->GetStringUTFChars(env, familyName, 0);
    sub_engine_set_default_font_name(eng, name);
    (*env)->ReleaseStringUTFChars(env, familyName, name);
}
