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

#pragma once
/*
 * A controllable stand-in for the SSA/SRT/GFX backends (sub_format_ssa/srt/gfx_create(),
 * which sub_engine.c calls directly -- see its open_track()). The tests in this directory
 * link the REAL sub_engine.c/sub_render_gl.c and need a backend that can be told exactly
 * when to answer "unchanged" (NULL) versus a new frame, and one whose render_at() can be
 * made deliberately slow -- neither of which the real SSA/SRT/GFX backends offer control
 * over from a test. This is that backend, plus a couple of frame-construction helpers.
 */
#include <stdint.h>
#include "sub_types.h"

typedef enum {
    FAKE_EMIT_ON_DEMAND,    /* render_at() returns NULL ("unchanged") until fake_backend_emit() */
    FAKE_EMIT_ALWAYS_SLOW   /* render_at() sleeps ~300us and returns a NEW frame every call */
} FAKE_MODE;

void     fake_backend_set_mode(FAKE_MODE mode);
void     fake_backend_emit(void);              /* ON_DEMAND: next render_at() yields one new frame */
int64_t  fake_clock(void *ctx);                /* pass to sub_engine_start() */
SUB_FRAME *fake_frame_new(int with_events);    /* refcount 1; with_events=0 is an empty ("clear") frame */

/* ---- observation hooks (added for the lifetime / loading / pause tests) ----------------------
 * Counters are process-wide and updated with atomics; reset them with fake_backend_reset_counters()
 * at the start of a scenario. "live" is deliberately NOT reset: it is opened-minus-closed backend
 * instances, i.e. a leak detector that must read 0 after the engine is destroyed. */
void    fake_backend_reset_counters(void);
int     fake_backend_live(void);                 /* backends opened and not yet closed */
int     fake_backend_feeds(void);                /* feed() calls that reached a backend */
int     fake_backend_flushes(void);
int     fake_backend_renders(void);              /* render_at() calls */
int     fake_backend_resizes(void);              /* backend->resize() calls */
int     fake_backend_box_sets(void);             /* backend->set_video_box() calls */
void    fake_backend_last_canvas(int *w, int *h);
void    fake_backend_last_box(int *x, int *y, int *w, int *h);
int64_t fake_backend_last_pts(void);             /* pts of the last render_at(); -1 if none */
/* A backend remembers the first byte of the first cue it was fed as its "label". A later cue with
 * a different first byte means a cue from another track reached it: counted here. */
int     fake_backend_foreign(void);
/* Feeds the MOST RECENTLY OPENED backend has accepted since its last flush (or since open). */
int     fake_backend_active_since_flush(void);


/* Per-cue delay inside the backend's feed(), in microseconds (default 0). Makes a bulk feed run long
 * enough (while holding the engine lock, like a real libass feed of a big file) for a test to land a
 * pause / reselect / close in the middle of it. Process-wide; set it back to 0 when done. */
void    fake_backend_set_feed_delay_us(int us);

/* Called from inside a backend's feed() (on whatever thread fed it, with the engine lock held) for every
 * cue, with the cue text. For tests that need to know exactly which cue is the last of a pass. */
typedef void (*fake_feed_hook_fn)(const uint8_t *data, int size);
void    fake_backend_set_feed_hook(fake_feed_hook_fn fn);
