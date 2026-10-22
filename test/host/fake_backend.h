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
