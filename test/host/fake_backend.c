/* See fake_backend.h for what this stands in for and why. */
#include "fake_backend.h"
#include "sub_engine.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile int  g_emit = 0;
static volatile int  g_mode = FAKE_EMIT_ON_DEMAND;

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
    (void)be; (void)pts;
    if (__atomic_load_n(&g_mode, __ATOMIC_ACQUIRE) == FAKE_EMIT_ALWAYS_SLOW) {
        usleep(300);                                   /* widens the poll->publish window */
    } else if (!__atomic_exchange_n(&g_emit, 0, __ATOMIC_ACQ_REL)) {
        return NULL;                                   /* NULL == "unchanged" (as ssa/gfx do) */
    }
    return fake_frame_new(1);
}
static int fake_open(SUB_FORMAT_BACKEND *be, const SUB_FORMAT_OPEN_PARAMS *p) { (void)be; (void)p; return 0; }
static int fake_close(SUB_FORMAT_BACKEND *be) { (void)be; return 0; }
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
    return b;
}
SUB_FORMAT_BACKEND *sub_format_srt_create(void) { return sub_format_ssa_create(); }
SUB_FORMAT_BACKEND *sub_format_gfx_create(void) { return sub_format_ssa_create(); }
