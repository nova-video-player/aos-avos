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
 * Engine lifetime in sub_engine_registry.c, with no real engine: the registry only ever treats a
 * SUB_ENGINE* as an opaque key, so heap blocks stand in for engines and a counting destroy
 * callback records what was destroyed, when, and on which thread.
 *
 * The rules under test: an engine is destroyed exactly once; never while any acquire() is
 * un-released; immediately when nothing holds it, otherwise by the LAST release; retract() never
 * blocks; publishing a replacement never loses older references; the destroy callback runs
 * outside the registry lock; stray releases are ignored.
 */
#include "sub_engine.h"
#include "sub_engine_registry.h"
#include "host_test_util.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { atomic_int holders; atomic_int destroyed; pthread_t destroyer; } DUMMY;
#define ENG(d) ((SUB_ENGINE *)(d))
static atomic_int g_destroy_while_held;   /* destroy ran with a live holder: lifetime violation */
static atomic_int g_destroy_calls;

static void destroy_cb(SUB_ENGINE *e) {
    DUMMY *d = (DUMMY *)e;
    if (atomic_load(&d->holders) != 0) atomic_fetch_add(&g_destroy_while_held, 1);
    atomic_fetch_add(&d->destroyed, 1);
    d->destroyer = pthread_self();
    atomic_fetch_add(&g_destroy_calls, 1);
}
static DUMMY *g_all[1024];   /* every dummy, freed at the end so ASan's leak check is meaningful */
static int g_nall;
static DUMMY *mk(void) { DUMMY *d = calloc(1, sizeof(DUMMY)); g_all[g_nall++] = d; return d; }

/* A destroy callback that re-enters the registry: proves it is not run under the registry lock. */
static DUMMY *g_reentry_next;
static atomic_int g_reentry_ran;
static void destroy_reentrant_cb(SUB_ENGINE *e) {
    destroy_cb(e);
    SUB_ENGINE *cur = sub_engine_registry_acquire();      /* would self-deadlock if the lock were held */
    if (cur) sub_engine_registry_release(cur);
    sub_engine_registry_publish(ENG(g_reentry_next));     /* ditto */
    atomic_fetch_add(&g_reentry_ran, 1);
}

/* ---- stress: Java thread publishes/retracts engines while stream threads acquire/release ---- */
#define N_ENGINES 400
static DUMMY *g_eng[N_ENGINES];
static atomic_int g_stop;
static atomic_long g_acquires;
static void *stream_thread(void *arg) {
    unsigned seed = (unsigned)(uintptr_t)arg;
    while (!atomic_load(&g_stop)) {
        SUB_ENGINE *e = sub_engine_registry_acquire();
        if (!e) { sched_yield(); continue; }
        DUMMY *d = (DUMMY *)e;
        atomic_fetch_add(&d->holders, 1);
        atomic_fetch_add(&g_acquires, 1);
        if (rand_r(&seed) % 4 == 0) usleep(rand_r(&seed) % 50);
        atomic_fetch_sub(&d->holders, 1);
        sub_engine_registry_release(e);
    }
    return NULL;
}

int main(void) {
    t_watchdog(60, "registry_lifetime");

    /* 1. Held engine: retract defers, last release destroys, no new acquires once retracted. */
    DUMMY *a = mk();
    sub_engine_registry_publish(ENG(a));
    SUB_ENGINE *got = sub_engine_registry_acquire();
    CHECK(got == ENG(a), "acquire returns the published engine");
    int r = sub_engine_registry_retract(ENG(a), destroy_cb);
    CHECK(r == 0 && atomic_load(&a->destroyed) == 0, "retract with a holder: deferred (returned %d), not destroyed", r);
    CHECK(sub_engine_registry_acquire() == NULL, "retracted engine can no longer be acquired");
    sub_engine_registry_release(got);
    CHECK(atomic_load(&a->destroyed) == 1 && pthread_equal(a->destroyer, pthread_self()),
          "last release destroys it, on the releasing thread");
    sub_engine_registry_release(got);   /* stray: engine already destroyed */
    CHECK(atomic_load(&a->destroyed) == 1, "stray release after destroy is ignored");

    /* 2. Not held: retract destroys immediately. */
    DUMMY *b = mk();
    sub_engine_registry_publish(ENG(b));
    r = sub_engine_registry_retract(ENG(b), destroy_cb);
    CHECK(r == 1 && atomic_load(&b->destroyed) == 1, "retract with no holders destroys immediately (returned %d)", r);
    CHECK(sub_engine_registry_acquire() == NULL, "nothing published after the only engine was retracted");

    /* 3. Replacement: older references survive a newer publish (this is the old "s_refcount = 0" bug). */
    DUMMY *c = mk(), *d = mk(), *e = mk();
    sub_engine_registry_publish(ENG(c));
    SUB_ENGINE *c1 = sub_engine_registry_acquire(), *c2 = sub_engine_registry_acquire();
    sub_engine_registry_publish(ENG(d));
    SUB_ENGINE *dh = sub_engine_registry_acquire();
    sub_engine_registry_publish(ENG(e));
    CHECK(dh == ENG(d) && sub_engine_registry_acquire() == ENG(e), "newest publish becomes the current engine");
    sub_engine_registry_release(ENG(e));   /* balance the probe acquire above */
    r = sub_engine_registry_retract(ENG(c), destroy_cb);
    CHECK(r == 0 && atomic_load(&c->destroyed) == 0, "superseded engine with 2 holders: retract defers");
    sub_engine_registry_release(c1);
    CHECK(atomic_load(&c->destroyed) == 0, "one of two holders released: still alive");
    sub_engine_registry_release(c2);
    CHECK(atomic_load(&c->destroyed) == 1, "second holder released: destroyed exactly once");
    CHECK(atomic_load(&d->destroyed) == 0, "the other superseded engine (1 holder, never retracted) is untouched");
    r = sub_engine_registry_retract(ENG(d), destroy_cb);
    CHECK(r == 0, "retracting the engine that still has a holder defers");
    sub_engine_registry_release(dh);
    CHECK(atomic_load(&d->destroyed) == 1, "and its release destroys it");
    CHECK(sub_engine_registry_retract(ENG(e), destroy_cb) == 1 && atomic_load(&e->destroyed) == 1,
          "current engine, no holders: retract destroys immediately");

    /* 4. Idempotent retract, stray releases, publish(NULL). */
    DUMMY *f = mk();
    sub_engine_registry_publish(ENG(f));
    SUB_ENGINE *fh = sub_engine_registry_acquire();
    sub_engine_registry_retract(ENG(f), destroy_cb);
    r = sub_engine_registry_retract(ENG(f), destroy_cb);
    CHECK(r == 0 && atomic_load(&f->destroyed) == 0, "second retract of a deferred engine is a no-op");
    sub_engine_registry_release(fh);
    sub_engine_registry_release(fh);   /* more releases than acquires */
    CHECK(atomic_load(&f->destroyed) == 1, "extra release cannot trigger a second destroy");
    DUMMY *never = mk();
    sub_engine_registry_release(ENG(never));   /* never published */
    sub_engine_registry_release(NULL);
    CHECK(atomic_load(&never->destroyed) == 0, "release of an unknown pointer / NULL is ignored");
    DUMMY *g = mk();
    sub_engine_registry_publish(ENG(g));
    sub_engine_registry_publish(NULL);
    CHECK(sub_engine_registry_acquire() == NULL, "publish(NULL) clears the current engine");
    CHECK(sub_engine_registry_retract(ENG(g), destroy_cb) == 1, "engine unpublished by publish(NULL) can still be retracted");
    DUMMY *unreg = mk();
    CHECK(sub_engine_registry_retract(ENG(unreg), destroy_cb) == 1 && atomic_load(&unreg->destroyed) == 1,
          "retract of an engine that was never published destroys it (nothing can hold it)");

    /* 5. destroy callback runs outside the registry lock. */
    DUMMY *h = mk(), *next = mk();
    g_reentry_next = next;
    sub_engine_registry_publish(ENG(h));
    SUB_ENGINE *hh = sub_engine_registry_acquire();
    sub_engine_registry_retract(ENG(h), destroy_reentrant_cb);
    sub_engine_registry_release(hh);      /* destroy runs here and calls back into the registry */
    CHECK(atomic_load(&g_reentry_ran) == 1 && sub_engine_registry_acquire() == ENG(next),
          "destroy callback may call acquire/publish (not run under the registry lock)");
    sub_engine_registry_release(ENG(next));
    sub_engine_registry_retract(ENG(next), destroy_cb);

    /* 6. Concurrent stress. */
    atomic_store(&g_destroy_calls, 0);
    atomic_store(&g_destroy_while_held, 0);
    pthread_t th[6];
    for (int i = 0; i < 6; i++) pthread_create(&th[i], NULL, stream_thread, (void *)(uintptr_t)(i + 1));
    for (int i = 0; i < N_ENGINES; i++) {
        g_eng[i] = mk();
        sub_engine_registry_publish(ENG(g_eng[i]));
        usleep(rand() % 200);
        if (i > 0) sub_engine_registry_retract(ENG(g_eng[i - 1]), destroy_cb);   /* retract the previous one */
    }
    sub_engine_registry_retract(ENG(g_eng[N_ENGINES - 1]), destroy_cb);
    atomic_store(&g_stop, 1);
    for (int i = 0; i < 6; i++) pthread_join(th[i], NULL);
    int bad_count = 0, leaked = 0;
    for (int i = 0; i < N_ENGINES; i++) {
        int n = atomic_load(&g_eng[i]->destroyed);
        if (n > 1) bad_count++;
        if (n == 0) leaked++;
    }
    CHECK(atomic_load(&g_destroy_while_held) == 0, "stress: no engine destroyed while a stream still held it (%ld acquires)", (long)atomic_load(&g_acquires));
    CHECK(bad_count == 0, "stress: no engine destroyed twice");
    CHECK(leaked == 0, "stress: every retracted engine was destroyed once its holders were gone (leaked %d)", leaked);

    for (int i = 0; i < g_nall; i++) free(g_all[i]);
    return t_finish();
}
