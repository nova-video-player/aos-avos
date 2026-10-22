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

#include "sub_engine_registry.h"
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <android/log.h>

#define REG_TAG "SubEngineRegistry"
#define REG_LOGW(...) __android_log_print(ANDROID_LOG_WARN, REG_TAG, __VA_ARGS__)

// Per-engine reference tracking.
//
// Every published engine gets its own entry holding its own outstanding-reference count, so
// references can never be attributed to the wrong engine: publishing a replacement no longer
// resets or drops anything for the engine it supersedes, and a late release() for an old engine
// still lands on that engine's own counter.
//
// retract() never blocks and never frees anything while a reference is outstanding. It marks the
// engine retracted (no NEW acquire() can return it) and hands it the destroy callback. The engine
// is destroyed exactly once, by whichever happens last: retract() (if nobody holds it) or the
// final release(). That makes the lifetime rule "alive until its last user lets go" unconditional;
// there is no timeout after which memory is freed under a holder, and no thread (typically the
// UI thread calling nativeDestroy()) ever waits on a stream.
typedef struct REG_ENTRY {
    SUB_ENGINE          *eng;
    int                  refs;       // outstanding acquire()s on this engine
    int                  retracted;  // retract() has been called; destroy once refs reaches 0
    sub_engine_destroy_fn destroy;   // set by retract(); called exactly once, outside s_lock
    struct REG_ENTRY    *next;
} REG_ENTRY;

static pthread_mutex_t s_lock    = PTHREAD_MUTEX_INITIALIZER;
static REG_ENTRY      *s_entries = NULL;  // every engine that is published and not yet destroyed
static REG_ENTRY      *s_current = NULL;  // the entry acquire() hands out, or NULL

// Caller holds s_lock.
static REG_ENTRY *find_locked(SUB_ENGINE *eng) {
    for (REG_ENTRY *e = s_entries; e; e = e->next) {
        if (e->eng == eng) return e;
    }
    return NULL;
}

// Caller holds s_lock. Unlinks and frees the entry (not the engine).
static void unlink_free_locked(REG_ENTRY *victim) {
    REG_ENTRY **pp = &s_entries;
    while (*pp && *pp != victim) pp = &(*pp)->next;
    if (*pp) *pp = victim->next;
    if (s_current == victim) s_current = NULL;
    free(victim);
}

void sub_engine_registry_publish(SUB_ENGINE *eng) {
    pthread_mutex_lock(&s_lock);
    if (!eng) {
        s_current = NULL;
        pthread_mutex_unlock(&s_lock);
        return;
    }
    REG_ENTRY *e = find_locked(eng);
    if (!e) {
        e = calloc(1, sizeof(*e));
        if (!e) {
            // Cannot track it, so it must not be handed out: an untracked engine could be
            // destroyed under a holder. Leave it unpublished.
            REG_LOGW("publish: out of memory, engine %p NOT published", (void *)eng);
            s_current = NULL;
            pthread_mutex_unlock(&s_lock);
            return;
        }
        e->eng = eng;
        e->next = s_entries;
        s_entries = e;
    }
    if (!e->retracted) {
        // Any previous engine keeps its own entry and its own references. It only stops being
        // the one acquire() returns.
        s_current = e;
    }
    pthread_mutex_unlock(&s_lock);
}

int sub_engine_registry_retract(SUB_ENGINE *eng, sub_engine_destroy_fn destroy) {
    if (!eng) return 0;
    pthread_mutex_lock(&s_lock);
    REG_ENTRY *e = find_locked(eng);
    if (!e) {
        // Never published (or already destroyed): nobody can hold a reference to it, since
        // acquire() only returns published engines. Safe to destroy right away.
        pthread_mutex_unlock(&s_lock);
        if (destroy) destroy(eng);
        return 1;
    }
    if (e->retracted) {
        // Second retract of the same engine: the first one owns the destroy.
        pthread_mutex_unlock(&s_lock);
        return 0;
    }
    e->retracted = 1;
    e->destroy = destroy;
    if (s_current == e) s_current = NULL;   // no NEW acquire() can return it from here on

    if (e->refs > 0) {
        // Deferred: the last release() destroys it.
        pthread_mutex_unlock(&s_lock);
        return 0;
    }
    sub_engine_destroy_fn fn = e->destroy;
    unlink_free_locked(e);
    pthread_mutex_unlock(&s_lock);
    if (fn) fn(eng);
    return 1;
}

SUB_ENGINE *sub_engine_registry_acquire(void) {
    pthread_mutex_lock(&s_lock);
    SUB_ENGINE *eng = NULL;
    if (s_current && !s_current->retracted) {
        s_current->refs++;
        eng = s_current->eng;
    }
    pthread_mutex_unlock(&s_lock);
    return eng;
}

void sub_engine_registry_release(SUB_ENGINE *eng) {
    if (!eng) return;
    pthread_mutex_lock(&s_lock);
    REG_ENTRY *e = find_locked(eng);
    if (!e || e->refs <= 0) {
        // Stray or double release: ignore rather than disturb another holder's count.
        pthread_mutex_unlock(&s_lock);
        return;
    }
    e->refs--;
    if (e->refs == 0 && e->retracted) {
        sub_engine_destroy_fn fn = e->destroy;
        unlink_free_locked(e);
        pthread_mutex_unlock(&s_lock);
        // Last user of a retracted engine: perform the deferred cleanup, outside the lock.
        if (fn) fn(eng);
        return;
    }
    pthread_mutex_unlock(&s_lock);
}
