/*
 * Copyright 2026 Courville Software
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef VIDEO_STARTUP_WATCHDOG_H
#define VIDEO_STARTUP_WATCHDOG_H

#include <stdint.h>

/* Only diagnose a decoder that has not returned its first frame since open or
 * flush. Never time presentation/sink backpressure after frames are flowing.
 * Count active input attempts (or EOS draining), not wall time spent paused,
 * seeking, buffering or waiting for the network. All calls hold the codec lock.
 */
typedef struct {
	int enabled, output_seen, inputs;
	int64_t last_attempt_ms, active_ms;
} VIDEO_STARTUP_WATCHDOG;

static inline void video_startup_watchdog_suspend(VIDEO_STARTUP_WATCHDOG *w)
{
	w->last_attempt_ms = 0;
	w->active_ms = 0;
}

static inline void video_startup_watchdog_reset(VIDEO_STARTUP_WATCHDOG *w, int enabled)
{
	w->enabled = enabled;
	w->output_seen = w->inputs = 0;
	video_startup_watchdog_suspend(w);
}

static inline int video_startup_watchdog_step(VIDEO_STARTUP_WATCHDOG *w,
	int64_t now_ms, int accepted, int blocked, int draining, int active)
{
	if (!w->enabled || w->output_seen)
		return 0;
	if (accepted && w->inputs < 8)
		w->inputs++;
	if (!active || !w->inputs || (w->inputs < 8 && !blocked && !draining)) {
		video_startup_watchdog_suspend(w);
		return 0;
	}
	int64_t gap = now_ms - w->last_attempt_ms;
	if (w->last_attempt_ms && gap >= 0 && gap <= 1000)
		w->active_ms += gap;
	else
		w->active_ms = 0;
	w->last_attempt_ms = now_ms;
	return w->active_ms >= 10000;
}

#endif
