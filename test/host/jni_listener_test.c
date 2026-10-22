// Copyright 2026 Courville Software
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/*
 * Lifecycle of jni_sub_engine.c's native->Java "content changed" listener, run against a MOCK
 * JavaVM/JNIEnv that counts attaches and detaches, tracks live global refs, and flags any Java
 * call made on a deleted global ref, from a detached thread, or on the wrong object.
 *
 * Checks: one global ref per engine; the render thread attaches once (named), stays attached,
 * and detaches when it exits; nativeDestroy() frees the ref only after the render thread is
 * joined; a missing Java method leaves the engine working with the push disabled; and 200
 * create / announce / destroy races leak nothing and never touch a freed ref. Run 4 covers the
 * deferred destroy: when a STREAM still holds the engine, nativeDestroy() must not free the engine
 * or its listener ref; the last registry release does, on the releasing thread.
 *
 * This validates OUR use of the JNI contract, not the JVM's behaviour -- see the notes in test_host_native.py.
 */
#include "sub_engine.h"
#include "sub_engine_registry.h"
#include "fake_backend.h"
#include <jni.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ---- mock JVM: counts attaches/detaches, tracks live global refs, flags use-after-free ----
static __thread int t_attached = 0;
static int g_attach=0, g_detach=0, g_live_refs=0, g_calls=0, g_uaf=0, g_no_method=0, g_wrong_obj=0, g_named_ok=0;
static char g_thread_name[32];
static volatile int g_ref_freed = 0; static char g_obj_token, g_gref_token;
static JNIEnv g_env_obj; static JavaVM g_vm_obj;
static jint m_GetEnv(JavaVM*vm,void**pe,jint v){ if(!t_attached) return JNI_EDETACHED; *pe=&g_env_obj; return JNI_OK; }
static jint m_Attach(JavaVM*vm,JNIEnv**pe,void*args){ t_attached=1; __atomic_fetch_add(&g_attach,1,__ATOMIC_RELAXED);
    if(args && ((JavaVMAttachArgs*)args)->name){ strncpy(g_thread_name,((JavaVMAttachArgs*)args)->name,31); __atomic_store_n(&g_named_ok,1,__ATOMIC_RELEASE); }
    *pe=&g_env_obj; return JNI_OK; }
static jint m_Detach(JavaVM*vm){ t_attached=0; __atomic_fetch_add(&g_detach,1,__ATOMIC_RELAXED); return JNI_OK; }
static const struct JNIInvokeInterface_ vm_tbl = { m_GetEnv, m_Attach, m_Detach };
static jint m_GetJavaVM(JNIEnv*e,JavaVM**pv){ *pv=&g_vm_obj; return JNI_OK; }
static jclass m_GetObjectClass(JNIEnv*e,jobject o){ return (jclass)0x77; }
static jmethodID m_GetMethodID(JNIEnv*e,jclass c,const char*n,const char*s){
    if(__atomic_load_n(&g_no_method,__ATOMIC_ACQUIRE)) return NULL;
    return (strcmp(n,"onNativeSubtitleContentChanged")==0 && strcmp(s,"()V")==0) ? (jmethodID)0x1 : NULL; }
static void m_DeleteLocalRef(JNIEnv*e,jobject o){}
static jobject m_NewGlobalRef(JNIEnv*e,jobject o){ __atomic_fetch_add(&g_live_refs,1,__ATOMIC_RELAXED); __atomic_store_n(&g_ref_freed,0,__ATOMIC_RELEASE); return &g_gref_token; }
static void m_DeleteGlobalRef(JNIEnv*e,jobject o){ __atomic_fetch_sub(&g_live_refs,1,__ATOMIC_RELAXED); __atomic_store_n(&g_ref_freed,1,__ATOMIC_RELEASE); }
static void m_CallVoid(JNIEnv*e,jobject o,jmethodID m,...){
    if(__atomic_load_n(&g_ref_freed,__ATOMIC_ACQUIRE)) __atomic_fetch_add(&g_uaf,1,__ATOMIC_RELAXED);      // call on a deleted global ref
    if(o!=&g_gref_token) __atomic_fetch_add(&g_wrong_obj,1,__ATOMIC_RELAXED);
    if(!t_attached) __atomic_fetch_add(&g_uaf,1,__ATOMIC_RELAXED);      // JNI call from a detached thread
    __atomic_fetch_add(&g_calls,1,__ATOMIC_RELAXED); }
static jboolean m_ExcCheck(JNIEnv*e){return 0;} static void m_ExcDesc(JNIEnv*e){} static void m_ExcClear(JNIEnv*e){}
static const struct JNINativeInterface_ env_tbl = { NULL,NULL,NULL,NULL, m_GetJavaVM,m_GetObjectClass,m_GetMethodID,m_DeleteLocalRef,
    m_NewGlobalRef,m_DeleteGlobalRef,m_CallVoid,m_ExcCheck,m_ExcDesc,m_ExcClear };

jlong Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeCreate(JNIEnv*,jobject);
void  Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeDestroy(JNIEnv*,jobject,jlong);
void  Java_com_archos_mediacenter_video_player_SubtitleEngine_nativeSetUIMode(JNIEnv*,jobject,jlong,jint);
#define J(n) Java_com_archos_mediacenter_video_player_SubtitleEngine_##n

static int fails=0;
#define CHECK(c,msg) do{ if(!(c)){printf("FAIL: %s\n",msg);fails++;} else printf("ok:   %s\n",msg);}while(0)
static int calls(void){return __atomic_load_n(&g_calls,__ATOMIC_RELAXED);}
static void settle(void){ usleep(100000); }
// A native (non-JVM) thread, like the avos thread that closes a video: it releases the engine.
static void *release_thread(void *p){ sub_engine_registry_release((SUB_ENGINE*)p); return NULL; }
int main(void){
    fake_backend_set_mode(FAKE_EMIT_ON_DEMAND);
    g_env_obj=&env_tbl; g_vm_obj=&vm_tbl; JNIEnv *env=&g_env_obj; jobject thiz=&g_obj_token;
    // The main thread plays the Java thread that calls nativeCreate/nativeDestroy: already attached,
    // so GetEnv() succeeds there and the engine teardown must NOT attach it again. (Before the
    // deferred-destroy change the env was passed in; the destroy callback now asks the JVM itself,
    // because the last release may happen on a native thread that is not attached.)
    t_attached=1;

    // ---- run 1: normal life cycle ----
    jlong h=J(nativeCreate)(env,thiz); SUB_ENGINE *eng=(SUB_ENGINE*)(intptr_t)h;
    CHECK(eng!=NULL && __atomic_load_n(&g_live_refs,__ATOMIC_ACQUIRE)==1,"nativeCreate: engine created, exactly one global ref held for the listener");
    J(nativeSetUIMode)(env,thiz,h,1);
    sub_engine_start(eng,fake_clock,NULL);
    sub_engine_open_track(eng,SUB_FMT_SSA,100,100,NULL,0,NULL,0,NULL);
    sub_engine_set_paused(eng,1);  settle();
    CHECK(calls()==1 && __atomic_load_n(&g_attach,__ATOMIC_ACQUIRE)==1,"pause with unannounced state: one Java call, thread attached once");
    CHECK(__atomic_load_n(&g_named_ok,__ATOMIC_ACQUIRE) && !strcmp(g_thread_name,"SubRender"),"render thread attached with a name");
    fake_backend_emit(); sub_engine_force_wake(eng); settle();
    CHECK(calls()==2 && __atomic_load_n(&g_attach,__ATOMIC_ACQUIRE)==1,"second paused change: another call, NO re-attach");
    sub_engine_close_track(eng); settle();
    CHECK(calls()==3,"paused track close announced to Java");
    CHECK(__atomic_load_n(&g_detach,__ATOMIC_ACQUIRE)==0,"still attached while the render thread lives");
    J(nativeDestroy)(env,thiz,h);
    CHECK(__atomic_load_n(&g_live_refs,__ATOMIC_ACQUIRE)==0,"nativeDestroy: global ref released");
    CHECK(__atomic_load_n(&g_detach,__ATOMIC_ACQUIRE)==1,"render thread detached from the JVM when it exited (pthread-key destructor)");
    int after=calls(); settle();
    CHECK(calls()==after && __atomic_load_n(&g_uaf,__ATOMIC_ACQUIRE)==0 && __atomic_load_n(&g_wrong_obj,__ATOMIC_ACQUIRE)==0,"no Java call after destroy; none on a freed ref, detached thread or wrong object");

    // ---- run 2: Java method missing (e.g. stripped by the shrinker) ----
    __atomic_store_n(&g_no_method,1,__ATOMIC_RELEASE); int c0=calls();
    h=J(nativeCreate)(env,thiz); eng=(SUB_ENGINE*)(intptr_t)h;
    CHECK(eng!=NULL && __atomic_load_n(&g_live_refs,__ATOMIC_ACQUIRE)==0,"method missing: engine still created, no global ref leaked");
    J(nativeSetUIMode)(env,thiz,h,1); sub_engine_start(eng,fake_clock,NULL);
    sub_engine_open_track(eng,SUB_FMT_SSA,100,100,NULL,0,NULL,0,NULL);
    sub_engine_set_paused(eng,1); fake_backend_emit(); sub_engine_force_wake(eng); settle();
    CHECK(calls()==c0,"method missing: push disabled, no Java calls");
    J(nativeDestroy)(env,thiz,h);
    CHECK(__atomic_load_n(&g_live_refs,__ATOMIC_ACQUIRE)==0 && __atomic_load_n(&g_uaf,__ATOMIC_ACQUIRE)==0,"method missing: clean destroy");

    // ---- run 3: destroy while announcements are flowing ----
    __atomic_store_n(&g_no_method,0,__ATOMIC_RELEASE);
    for(int i=0;i<200;i++){
        h=J(nativeCreate)(env,thiz); eng=(SUB_ENGINE*)(intptr_t)h;
        J(nativeSetUIMode)(env,thiz,h,1); sub_engine_start(eng,fake_clock,NULL);
        sub_engine_open_track(eng,SUB_FMT_SSA,100,100,NULL,0,NULL,0,NULL);
        sub_engine_set_paused(eng,1);
        for(int k=0;k<5;k++){ fake_backend_emit(); sub_engine_force_wake(eng); }
        usleep(i%300);
        J(nativeDestroy)(env,thiz,h);
    }
    CHECK(__atomic_load_n(&g_live_refs,__ATOMIC_ACQUIRE)==0 && __atomic_load_n(&g_uaf,__ATOMIC_ACQUIRE)==0 && __atomic_load_n(&g_wrong_obj,__ATOMIC_ACQUIRE)==0,"200 create/announce/destroy races: no leaked ref, no call on a freed ref");
    CHECK(__atomic_load_n(&g_attach,__ATOMIC_ACQUIRE)==g_detach,"every attached render thread detached (attaches == detaches)");

    // ---- run 4: Java destroys the engine while a STREAM still holds it (deferred destroy) ----
    {
        int a0=__atomic_load_n(&g_attach,__ATOMIC_ACQUIRE), d0=__atomic_load_n(&g_detach,__ATOMIC_ACQUIRE);
        h=J(nativeCreate)(env,thiz); eng=(SUB_ENGINE*)(intptr_t)h;
        SUB_ENGINE *held=sub_engine_registry_acquire();       // what avos_mp_video_open() does
        CHECK(held==eng && __atomic_load_n(&g_live_refs,__ATOMIC_ACQUIRE)==1,"deferred: stream acquired the published engine");
        J(nativeSetUIMode)(env,thiz,h,1); sub_engine_start(eng,fake_clock,NULL);
        sub_engine_open_track(eng,SUB_FMT_SSA,100,100,NULL,0,NULL,0,NULL);
        sub_engine_set_paused(eng,1); settle();
        J(nativeDestroy)(env,thiz,h);                          // must return without freeing anything
        CHECK(__atomic_load_n(&g_live_refs,__ATOMIC_ACQUIRE)==1,"deferred: nativeDestroy with a holder keeps the engine and the listener's global ref");
        CHECK(sub_engine_registry_acquire()==NULL,"deferred: a retracted engine cannot be acquired by a new stream");
        int c=calls(); fake_backend_emit(); sub_engine_force_wake(eng); settle();
        CHECK(calls()>c && __atomic_load_n(&g_uaf,__ATOMIC_ACQUIRE)==0,"deferred: the holder can still use the engine and pushes keep reaching Java");
        sub_engine_stop(held); sub_engine_close_track(held);   // avos_mp_video_close() order
        pthread_t rt; pthread_create(&rt,NULL,release_thread,held); pthread_join(rt,NULL);   // last holder: destroy runs THERE
        CHECK(__atomic_load_n(&g_live_refs,__ATOMIC_ACQUIRE)==0,"deferred: the last release frees the listener's global ref");
        CHECK((__atomic_load_n(&g_attach,__ATOMIC_ACQUIRE)-a0)==2 && (__atomic_load_n(&g_detach,__ATOMIC_ACQUIRE)-d0)==2,
              "deferred: render thread + the un-attached releasing thread each attached once and detached once");
        int after=calls(); settle();
        CHECK(calls()==after && __atomic_load_n(&g_uaf,__ATOMIC_ACQUIRE)==0 && __atomic_load_n(&g_wrong_obj,__ATOMIC_ACQUIRE)==0,
              "deferred: no Java call after the deferred destroy");
        // The next stream gets the NEW engine, never the retracted one.
        jlong h2=J(nativeCreate)(env,thiz); SUB_ENGINE *next=sub_engine_registry_acquire();
        CHECK(next==(SUB_ENGINE*)(intptr_t)h2,"deferred: after a rebuild, acquire() returns the new engine");
        sub_engine_registry_release(next);
        J(nativeDestroy)(env,thiz,h2);
        CHECK(__atomic_load_n(&g_live_refs,__ATOMIC_ACQUIRE)==0,"deferred: replacement engine destroyed cleanly");
    }
    printf("%s\n",fails?"FAILED":"ALL PASSED"); return fails;
}
