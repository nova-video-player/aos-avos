/*
 * Stale-frame-after-close race.
 *
 * A real render thread polls a deliberately slow backend while the main thread opens and closes
 * tracks. Once close_track() has RETURNED there is no open track, so nothing may be on screen --
 * not immediately, and not a moment later. Before poll+publish became one engine-lock hold, the
 * render thread could poll a frame from the old backend, lose the race to close_track()'s clear,
 * and then install that frame: a cue from a closed track left on screen.
 */
#include "sub_engine.h"
#include "fake_backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int something_visible(SUB_ENGINE *e) {
    uint64_t gen; uint8_t px[64];
    return sub_engine_fill_bitmap(e, px, 4, 4, 16, (uint64_t)-1, /*force=*/1, &gen) == SUB_FILL_FRAME;
}

int main(int argc, char **argv) {
    int iters = argc > 1 ? atoi(argv[1]) : 1500, stale = 0;
    fake_backend_set_mode(FAKE_EMIT_ALWAYS_SLOW);
    SUB_ENGINE *e = sub_engine_create();
    sub_engine_start(e, fake_clock, NULL);
    for (int i = 0; i < iters; i++) {
        sub_engine_open_track(e, SUB_FMT_SSA, 100, 100, NULL, 0, NULL, 0, NULL);
        usleep(100 + rand() % 700);          /* let the render thread be mid-poll when we close */
        sub_engine_close_track(e);
        if (something_visible(e)) stale++;   /* anything showing with NO track open is the bug */
        usleep(rand() % 200);
        if (something_visible(e)) stale++;   /* ...and it must stay clear */
    }
    sub_engine_destroy(e);
    printf("iterations=%d stale-frame-after-close=%d\n", iters, stale);
    return stale ? 1 : 0;
}
