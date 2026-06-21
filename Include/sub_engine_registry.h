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
 *   - Java destroying an engine RETRACTs it, and blocks until every
 *     outstanding ACQUIRE has been released -- so destroy cannot complete,
 *     and the memory cannot be freed, while any STREAM is still holding a
 *     reference.
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

// Called ONLY from jni_sub_engine.c's nativeCreate, right after
// sub_engine_create() succeeds, to publish it as the current engine.
void sub_engine_registry_publish(SUB_ENGINE *eng);

// Called ONLY from jni_sub_engine.c's nativeDestroy, BEFORE sub_engine_destroy().
// Retracts `eng` if it is still the published engine (a no-op if some other
// engine has already been published in its place), then blocks until every
// acquire() taken while it was published has been matched by a release().
// Safe/idempotent to call even if `eng` was already superseded.
void sub_engine_registry_retract(SUB_ENGINE *eng);

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
