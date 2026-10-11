/*
 * When the native render thread announces "subtitle content changed" (the 3D-path push):
 * only while PAUSED, only in a 3D UI mode, once per generation -- and never with an engine or
 * renderer lock held (the callback calls back into both; a held lock would self-deadlock).
 */
#include "sub_engine.h"
#include "sub_render_gl.h"
#include "fake_backend.h"
#include <stdio.h>
#include <unistd.h>

static SUB_ENGINE *g_eng; static SUB_RENDERER *g_r;
static volatile int g_cb = 0, g_bad = 0;

static void on_change(void *ctx) {
    (void)sub_engine_is_paused(g_eng);           /* takes the engine lock */
    (void)sub_engine_get_generation(g_eng);
    sub_engine_force_wake(g_eng);
    (void)sub_render_gl_get_frame_generation(g_r); /* takes the renderer lock */
    if (ctx != (void *)0x1234) __atomic_fetch_add(&g_bad, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_cb, 1, __ATOMIC_RELAXED);
}

static int fails = 0;
#define EXPECT(n, msg) do { usleep(80000); int v = __atomic_load_n(&g_cb, __ATOMIC_RELAXED); \
    if (v != (n)) { printf("FAIL: %s (callbacks=%d, want %d)\n", msg, v, n); fails++; } \
    else printf("ok:   %s (callbacks=%d)\n", msg, v); } while (0)
static void emit(void) { fake_backend_emit(); sub_engine_force_wake(g_eng); }

int main(void) {
    fake_backend_set_mode(FAKE_EMIT_ON_DEMAND);
    g_eng = sub_engine_create();
    g_r = *(SUB_RENDERER **)g_eng;   /* renderer is SUB_ENGINE's first member; test-only peek */
    sub_engine_set_change_callback(g_eng, on_change, (void *)0x1234);
    if (sub_engine_get_change_ctx(g_eng) != (void *)0x1234) { printf("FAIL: get_change_ctx\n"); fails++; }
    sub_render_gl_set_ui_mode(g_r, 1);                       /* a 3D mode */
    sub_engine_start(g_eng, fake_clock, NULL);
    sub_engine_open_track(g_eng, SUB_FMT_SSA, 100, 100, NULL, 0, NULL, 0, NULL);

    emit();                           EXPECT(0, "PLAYING: content changes, nothing announced (per-video-frame pull covers it)");
    sub_engine_set_paused(g_eng, 1);  EXPECT(1, "PAUSE: a change that landed before the pause is announced once");
    emit();                           EXPECT(2, "PAUSED: new frame is announced");
    sub_engine_force_wake(g_eng);     EXPECT(2, "PAUSED: spurious wake with no change -> no repeat announce");
    sub_engine_close_track(g_eng);    EXPECT(3, "PAUSED: track close clears visible content -> announced");
    sub_engine_close_track(g_eng);    EXPECT(3, "PAUSED: closing again (nothing showing) -> silent");
    sub_engine_open_track(g_eng, SUB_FMT_SSA, 100, 100, NULL, 0, NULL, 0, NULL);
    emit();                           EXPECT(4, "PAUSED: new track's first cue arrives -> announced");
    sub_render_gl_set_ui_mode(g_r, 0);
    emit();                           EXPECT(4, "2D mode: never announced (native GL thread draws it itself)");
    sub_render_gl_set_ui_mode(g_r, 1); sub_engine_force_wake(g_eng);
                                      EXPECT(5, "ENTER 3D while paused with an unannounced change: announced once");
    sub_engine_set_paused(g_eng, 0);  EXPECT(5, "UNPAUSE: silent");
    emit();                           EXPECT(5, "PLAYING again: content changes stay silent");

    sub_engine_destroy(g_eng);
    int at_destroy = __atomic_load_n(&g_cb, __ATOMIC_RELAXED); usleep(100000);
    if (__atomic_load_n(&g_cb, __ATOMIC_RELAXED) != at_destroy) { printf("FAIL: callback ran after destroy returned\n"); fails++; }
    else printf("ok:   no callback after sub_engine_destroy() returned\n");
    if (__atomic_load_n(&g_bad, __ATOMIC_RELAXED)) { printf("FAIL: ctx mismatch\n"); fails++; }
    printf("%s\n", fails ? "FAILED" : "ALL PASSED");
    return fails;
}
