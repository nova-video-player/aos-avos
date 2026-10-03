/*
 * The renderer's publish/clear/fill contract: which of UNCHANGED / CLEAR / FRAME / ERROR a pull
 * answers, and that the generation moves exactly when what is on screen changes.
 */
#include "sub_engine.h"
#include "sub_render_gl.h"
#include "fake_backend.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } else printf("ok:   %s\n", msg); } while (0)

int main(void) {
    SUB_RENDERER *r = sub_render_gl_create(NULL);
    uint8_t px[8 * 8 * 4]; uint64_t g; SUB_FILL_RESULT rc; const uint64_t NONE = (uint64_t)-1;

    rc = sub_render_gl_fill_bitmap(r, px, 8, 8, 32, NONE, 0, &g);
    CHECK(rc == SUB_FILL_CLEAR && g == 0, "start: nothing posted yet -> CLEAR, gen 0");
    rc = sub_render_gl_fill_bitmap(r, px, 8, 8, 32, g, 0, &g);
    CHECK(rc == SUB_FILL_UNCHANGED, "same gen, still nothing -> UNCHANGED");
    CHECK(sub_render_gl_publish(r, NULL) == 0, "backend says NULL -> unchanged");
    CHECK(sub_render_gl_publish(r, fake_frame_new(0)) == 0, "empty frame over empty screen -> unchanged (no gen bump)");
    CHECK(sub_render_gl_publish(r, fake_frame_new(1)) == 1, "new frame -> changed");
    rc = sub_render_gl_fill_bitmap(r, px, 8, 8, 32, 0, 0, &g);
    CHECK(rc == SUB_FILL_FRAME && g == 1 && px[3] == 255, "gen moved 0->1 -> FRAME, pixels blended, gen 1 reported with them");
    rc = sub_render_gl_fill_bitmap(r, px, 8, 8, 32, g, 0, &g);
    CHECK(rc == SUB_FILL_UNCHANGED && g == 1, "same gen -> UNCHANGED");
    rc = sub_render_gl_fill_bitmap(r, px, 8, 8, 32, g, 1, &g);
    CHECK(rc == SUB_FILL_FRAME, "force=1 never answers UNCHANGED");
    CHECK(sub_render_gl_publish(r, fake_frame_new(0)) == 1, "empty frame over visible content -> CLEAR, changed");
    rc = sub_render_gl_fill_bitmap(r, px, 8, 8, 32, 1, 0, &g);
    CHECK(rc == SUB_FILL_CLEAR && g == 2, "-> CLEAR, gen 2 (a clear is a real change, not 'unchanged')");
    sub_render_gl_publish(r, fake_frame_new(1));
    sub_render_gl_clear_nowake(r);
    rc = sub_render_gl_fill_bitmap(r, px, 8, 8, 32, 3, 0, &g);
    CHECK(rc == SUB_FILL_CLEAR && g == 4, "track close/open clear of visible content bumps gen -> CLEAR");
    sub_render_gl_clear_nowake(r);
    rc = sub_render_gl_fill_bitmap(r, px, 8, 8, 32, g, 0, &g);
    CHECK(rc == SUB_FILL_UNCHANGED && g == 4, "clear of already-clear screen -> no bump");
    rc = sub_render_gl_fill_bitmap(r, NULL, 8, 8, 32, NONE, 0, &g);
    CHECK(rc == SUB_FILL_ERROR, "bad args -> ERROR (distinct from CLEAR)");

    printf("%s\n", fails ? "FAILED" : "ALL PASSED");
    return fails;
}
