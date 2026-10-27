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

/* See fake_backend.h for what this stands in for and why. */
#include "fake_backend.h"
#include "sub_engine.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pthread.h>
#include <time.h>

static volatile int  g_emit = 0;
static volatile int  g_mode = FAKE_EMIT_ON_DEMAND;
static volatile int  g_feed_delay_us = 0;
static fake_feed_hook_fn g_feed_hook;

typedef struct { int label; int feeds; int since_flush; } FAKE_CTX;
static int g_live, g_feeds, g_flushes, g_renders, g_resizes, g_boxes, g_foreign;
static int64_t g_last_pts = -1;
static int g_cw, g_ch, g_bx, g_by, g_bw, g_bh;
static FAKE_CTX *g_latest;
static pthread_mutex_t g_m = PTHREAD_MUTEX_INITIALIZER;
#define INC(v) __atomic_fetch_add(&(v), 1, __ATOMIC_RELAXED)
#define GET(v) __atomic_load_n(&(v), __ATOMIC_RELAXED)

void fake_backend_reset_counters(void) {
    __atomic_store_n(&g_feeds, 0, __ATOMIC_RELAXED);  __atomic_store_n(&g_flushes, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_renders, 0, __ATOMIC_RELAXED); __atomic_store_n(&g_resizes, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_boxes, 0, __ATOMIC_RELAXED);  __atomic_store_n(&g_foreign, 0, __ATOMIC_RELAXED);
    pthread_mutex_lock(&g_m); g_last_pts = -1; g_cw = g_ch = g_bx = g_by = g_bw = g_bh = 0; pthread_mutex_unlock(&g_m);
}
int fake_backend_live(void)     { return GET(g_live); }
int fake_backend_feeds(void)    { return GET(g_feeds); }
int fake_backend_flushes(void)  { return GET(g_flushes); }
int fake_backend_renders(void)  { return GET(g_renders); }
int fake_backend_resizes(void)  { return GET(g_resizes); }
int fake_backend_box_sets(void) { return GET(g_boxes); }
int fake_backend_foreign(void)  { return GET(g_foreign); }
void fake_backend_last_canvas(int *w, int *h) { pthread_mutex_lock(&g_m); *w = g_cw; *h = g_ch; pthread_mutex_unlock(&g_m); }
void fake_backend_last_box(int *x, int *y, int *w, int *h) {
    pthread_mutex_lock(&g_m); *x = g_bx; *y = g_by; *w = g_bw; *h = g_bh; pthread_mutex_unlock(&g_m);
}
int64_t fake_backend_last_pts(void) { pthread_mutex_lock(&g_m); int64_t p = g_last_pts; pthread_mutex_unlock(&g_m); return p; }
int fake_backend_active_since_flush(void) {
    pthread_mutex_lock(&g_m);
    int n = g_latest ? __atomic_load_n(&g_latest->since_flush, __ATOMIC_RELAXED) : -1;
    pthread_mutex_unlock(&g_m);
    return n;
}

void fake_backend_set_feed_hook(fake_feed_hook_fn fn) { __atomic_store_n(&g_feed_hook, fn, __ATOMIC_RELEASE); }
void fake_backend_set_feed_delay_us(int us) { __atomic_store_n(&g_feed_delay_us, us, __ATOMIC_RELEASE); }
void fake_backend_set_mode(FAKE_MODE m) { __atomic_store_n(&g_mode, (int)m, __ATOMIC_RELEASE); }
void fake_backend_emit(void)            { __atomic_store_n(&g_emit, 1, __ATOMIC_RELEASE); }
int64_t fake_clock(void *ctx)           { (void)ctx; return 1000; }

SUB_FRAME *fake_frame_new(int with_events) {
    SUB_FRAME *f = calloc(1, sizeof(*f));
    f->refcount = 1;
    f->video_w = f->video_h = 8;
    if (with_events) {
        SUB_EVENT *e = calloc(1, sizeof(*e));
        e->kind = SUB_EVENT_BITMAP; e->w = e->h = 2;
        uint8_t *px = malloc(16); memset(px, 255, 16);
        e->data.bitmap.rgba = px; e->data.bitmap.stride = 8;
        f->events = e;
    }
    return f;
}

static SUB_FRAME *fake_render_at(SUB_FORMAT_BACKEND *be, int64_t pts) {
    (void)be;
    INC(g_renders);
    pthread_mutex_lock(&g_m); g_last_pts = pts; pthread_mutex_unlock(&g_m);
    if (__atomic_load_n(&g_mode, __ATOMIC_ACQUIRE) == FAKE_EMIT_ALWAYS_SLOW) {
        usleep(300);                                   /* widens the poll->publish window */
    } else if (!__atomic_exchange_n(&g_emit, 0, __ATOMIC_ACQ_REL)) {
        return NULL;                                   /* NULL == "unchanged" (as ssa/gfx do) */
    }
    return fake_frame_new(1);
}
static int fake_open(SUB_FORMAT_BACKEND *be, const SUB_FORMAT_OPEN_PARAMS *p) {
    (void)p;
    FAKE_CTX *ctx = calloc(1, sizeof(*ctx));
    be->priv = ctx;
    pthread_mutex_lock(&g_m); g_latest = ctx; pthread_mutex_unlock(&g_m);
    INC(g_live);
    return 0;
}
static int fake_close(SUB_FORMAT_BACKEND *be) {
    FAKE_CTX *ctx = (FAKE_CTX *)be->priv;
    if (!ctx) return 0;            /* open() never ran (engine's failed-open path calls close too) */
    pthread_mutex_lock(&g_m); if (g_latest == ctx) g_latest = NULL; pthread_mutex_unlock(&g_m);
    free(ctx); be->priv = NULL;
    __atomic_fetch_sub(&g_live, 1, __ATOMIC_RELAXED);
    return 0;
}
static int fake_feed(SUB_FORMAT_BACKEND *be, const uint8_t *data, int size, int64_t pts, int64_t dur) {
    (void)pts; (void)dur;
    FAKE_CTX *ctx = (FAKE_CTX *)be->priv;
    int delay = __atomic_load_n(&g_feed_delay_us, __ATOMIC_ACQUIRE);
    if (delay > 0) {                                   /* a slow backend: gives a feed a long, interruptible run */
        if (delay >= 100) usleep((useconds_t)delay);
        else {                                         /* usleep() can't do less than ~50 us: spin for short delays */
            struct timespec a, b;
            clock_gettime(CLOCK_MONOTONIC, &a);
            do { clock_gettime(CLOCK_MONOTONIC, &b); }
            while ((b.tv_sec - a.tv_sec) * 1000000L + (b.tv_nsec - a.tv_nsec) / 1000L < delay);
        }
    }
    int label = size > 0 ? data[0] : 0;
    if (!ctx->label) ctx->label = label;
    else if (ctx->label != label) INC(g_foreign);
    ctx->feeds++;
    __atomic_fetch_add(&ctx->since_flush, 1, __ATOMIC_RELAXED);
    INC(g_feeds);
    fake_feed_hook_fn hook = __atomic_load_n(&g_feed_hook, __ATOMIC_ACQUIRE);
    if (hook) hook(data, size);
    return 1;
}
static int fake_flush(SUB_FORMAT_BACKEND *be) {
    FAKE_CTX *ctx = (FAKE_CTX *)be->priv;
    __atomic_store_n(&ctx->since_flush, 0, __ATOMIC_RELAXED);
    INC(g_flushes);
    return 0;
}
static int fake_resize(SUB_FORMAT_BACKEND *be, int w, int h) {
    (void)be; INC(g_resizes);
    pthread_mutex_lock(&g_m); g_cw = w; g_ch = h; pthread_mutex_unlock(&g_m);
    return 0;
}
static int fake_set_box(SUB_FORMAT_BACKEND *be, int x, int y, int w, int h) {
    (void)be; INC(g_boxes);
    pthread_mutex_lock(&g_m); g_bx = x; g_by = y; g_bw = w; g_bh = h; pthread_mutex_unlock(&g_m);
    return 0;
}
/* RST timing contract (get_schedule replaced the old get_timeout_ms, which this returned 1 from).
 * A deadline that is already due (next_rst_ms <= now) makes the engine's schedule_to_wall_ms()
 * wait the minimum, 1 ms, then re-poll -- the same cadence as the old 1 ms timeout, which is
 * what race_harness relies on (a poll loop that never goes idle). The tests drive their own
 * wake-ups too (fake_backend_emit() + sub_engine_force_wake()). */
static void fake_schedule(SUB_FORMAT_BACKEND *be, int64_t pts_rst_ms, SUB_SCHEDULE *out) {
    (void)be; (void)pts_rst_ms;
    out->next_rst_ms = 0;
    out->animating   = 0;
}

SUB_FORMAT_BACKEND *sub_format_ssa_create(void) {
    SUB_FORMAT_BACKEND *b = calloc(1, sizeof(*b));
    b->open = fake_open; b->close = fake_close;
    b->render_at = fake_render_at; b->get_schedule = fake_schedule;
    b->feed = fake_feed; b->flush = fake_flush; b->resize = fake_resize; b->set_video_box = fake_set_box;
    return b;
}
SUB_FORMAT_BACKEND *sub_format_srt_create(void) { return sub_format_ssa_create(); }
SUB_FORMAT_BACKEND *sub_format_gfx_create(void) { return sub_format_ssa_create(); }
