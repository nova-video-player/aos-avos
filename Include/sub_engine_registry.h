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
 * sub_engine_registry.h — single source of truth for "which SUB_ENGINE is
 * currently live", replacing the old bare `extern SUB_ENGINE *g_sub_engine`.
 *
 * Why this exists: the JNI layer (jni_sub_engine.c) owns the SUB_ENGINE's
 * lifetime -- Java's SubtitleEngine.nativeCreate()/nativeDestroy() can be
 * called again at any time the Java side decides to build a new
 * SubtitleEngine (e.g. after PlayerService is torn down and rebuilt), fully
 * independently of whatever STREAM the AVOS core is currently playing.
 * Previously, avos_mp_video_open() copied the raw g_sub_engine pointer once
 * into STREAM::sub_engine at stream-open time, with no synchronization
 * against a concurrent nativeDestroy() and no way for that STREAM to find
 * out its copy went stale afterwards. That copy could end up pointing at
 * freed memory (nativeDestroy() already called) for the rest of the
 * STREAM's life, with only lucky timing preventing a use-after-free.
 *
 * This registry makes that handoff explicit and safe:
 *   - Java creating a new engine PUBLISHes it here.
 *   - Java destroying an engine RETRACTs it with a destroy callback. Retract
 *     does not block: the engine is freed immediately if nobody holds it,
 *     otherwise by the last RELEASE -- so the memory cannot be freed while
 *     any STREAM is still holding a reference.
 *   - Native code (avos_mp_video.c) ACQUIREs a reference when it wants to
 *     bind an engine to a STREAM, and RELEASEs it when that STREAM no
 *     longer needs it (stream close). Nothing outside this file touches a
 *     raw SUB_ENGINE* for the purpose of establishing a new reference.
 *
 * A STREAM's own use of its acquired pointer between acquire and release is
 * still the caller's responsibility to serialize against sub_engine.c's own
 * internal locking (unchanged) -- this registry only protects the
 * *lifetime* handoff, not concurrent calls into a live engine.
 */
#pragma once

#include "sub_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*sub_engine_destroy_fn)(SUB_ENGINE *eng);

// Called ONLY from jni_sub_engine.c's nativeCreate, right after
// sub_engine_create() succeeds, to publish it as the current engine.
void sub_engine_registry_publish(SUB_ENGINE *eng);

// Called ONLY from jni_sub_engine.c's nativeDestroy, INSTEAD of destroying the engine directly.
// Unpublishes `eng` (no new acquire() can take it) and NEVER blocks. If no STREAM holds a
// reference, `destroy(eng)` runs immediately on the calling thread and 1 is returned. Otherwise
// destruction is deferred: the last sub_engine_registry_release() of `eng` runs `destroy(eng)` on
// the releasing thread (outside the registry lock) and 0 is returned. A repeated retract of the
// same engine is a no-op returning 0. Retracting an engine that was superseded by a newer publish
// is fine: older engines stay tracked until their own last release.
int sub_engine_registry_retract(SUB_ENGINE *eng, sub_engine_destroy_fn destroy);

// Takes a reference to whatever engine is currently published (or NULL if
// none is). Every non-NULL return MUST be matched by exactly one
// sub_engine_registry_release() call once the caller is done needing the
// engine to stay alive (i.e. at STREAM close, not merely "done with this
// function call").
SUB_ENGINE *sub_engine_registry_acquire(void);

// Releases a reference previously taken by sub_engine_registry_acquire().
// Pass the exact same pointer that acquire() returned. Safe to call with
// NULL (no-op), so callers don't need to guard every release with a null
// check of their own STREAM::sub_engine field.
void sub_engine_registry_release(SUB_ENGINE *eng);

#ifdef __cplusplus
}
#endif
