/*
 * Copyright 2017 Archos SA
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

/* Reference from Source/codec_sfdec2.c at 0ab00c6, Apache-2.0.
 * Frozen for differential testing; do not regenerate from the implementation. */
static void advance_reference(priv_t *p, STREAM *s, VIDEO_FRAME *f, int mode2_dynamic_active)
{
int mode2_new_slew_frame = p->mode2_dynamic_slew &&
				(f->android_handle != p->mode2_slew_frame_handle ||
				 f->time != p->mode2_slew_frame_time ||
				 f->epoch != p->mode2_slew_frame_epoch);
int pcm_startup_new_slew_frame = (p->pcm_startup_slew || p->mode1_resume_slew) &&
				(f->android_handle != p->pcm_startup_slew_frame_handle ||
				 f->time != p->pcm_startup_slew_frame_time ||
				 f->epoch != p->pcm_startup_slew_frame_epoch);
if (p->slew_active &&
				(!p->pcm_resume_slew || (!p->pause_armed && (!s || !s->paused))) &&
				(!p->mode2_dynamic_slew || mode2_new_slew_frame) &&
				(!(p->pcm_startup_slew || p->mode1_resume_slew) || pcm_startup_new_slew_frame)) {
				INT64 delta = p->target_offset_ns - p->render_offset_ns;
				// A Mode 2 clock transition can correct hundreds of milliseconds, so
				// converge it at 5ms/frame. PCM startup uses 1ms per distinct frame to
				// remove its bounded residual without a snap. Once established, track
				// ordinary clock drift at 0.2ms/frame: compressed presentation samples
				// can move by a whole codec frame and then hold, and chasing that
				// temporary phase at 5ms/frame produces visible cadence reversals.
				INT64 step = p->mode2_dynamic_fast_slew ? 5000000 :
					(p->pcm_startup_slew ? 1000000 : 200000);
				if( (p->pcm_startup_slew && p->pcm_resume_slew) || p->mode1_resume_slew ) {
					// Rapid pauses can accumulate a phase that takes seconds to
					// remove at the startup rate. Limit resume correction to 10%
					// of the speed-adjusted frame interval, capped at 4ms, so
					// high frame rates/speeds do not get large cadence changes.
					step = f->duration > 0 ? MIN(4000000LL,
						(INT64)f->duration * 100000LL) : 1000000LL;
				}
				if (delta > step) {
					delta = step;
				} else if (delta < -step) {
					delta = -step;
				}
				p->render_offset_ns += delta;
				INT64 remaining = llabs(p->target_offset_ns - p->render_offset_ns);
				// Bounded per-frame slews must carry the remainder to the next
				// frame. Snapping here can double the Mode 2 correction to 10ms.
				if (remaining == 0 || (!(p->pcm_startup_slew || p->mode1_resume_slew ||
					p->mode2_dynamic_slew) && remaining <= step)) {
					p->render_offset_ns = p->target_offset_ns;
					p->slew_active = 0;
					if (!p->mode2_dynamic_fast_slew) {
						p->mode2_dynamic_settle_frames = 0;
					}
					if( !mode2_dynamic_active ) {
						p->mode2_dynamic_slew = 0;
						p->mode2_slew_frame_handle = NULL;
						p->mode2_slew_frame_time = INT_MIN;
						p->mode2_slew_frame_epoch = INT_MIN;
					}
					if( p->mode1_resume_slew ) {
						DBGSI serprintf("android_sync: mode1 phase slew complete offset=%lld\n",
							(long long)p->render_offset_ns);
						p->mode1_resume_slew = 0;
					}
					if( p->pcm_startup_slew ) {
						if( p->pcm_resume_slew ) {
							DBGSI serprintf("android_sync: PCM resume slew complete offset=%lld\n",
								(long long)p->render_offset_ns);
						}
						p->pcm_startup_slew = 0;
						p->pcm_resume_slew = 0;
						if( !p->pcm_resume_calibration_active ) {
							p->pcm_startup_slew_frame_handle = NULL;
							p->pcm_startup_slew_frame_time = INT_MIN;
							p->pcm_startup_slew_frame_epoch = INT_MIN;
						}
					}
				}
			}
if( p->mode2_dynamic_slew && mode2_new_slew_frame ) {
				p->mode2_slew_frame_handle = f->android_handle;
				p->mode2_slew_frame_time = f->time;
				p->mode2_slew_frame_epoch = f->epoch;
			}
if( (p->pcm_startup_slew || p->pcm_resume_calibration_active || p->mode1_resume_slew) && pcm_startup_new_slew_frame ) {
				p->pcm_startup_slew_frame_handle = f->android_handle;
				p->pcm_startup_slew_frame_time = f->time;
				p->pcm_startup_slew_frame_epoch = f->epoch;
			}
}
