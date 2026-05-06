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

#include "global.h"
#include "types.h"
#include "debug.h"
#include "util.h"
#include "audio_interface.h"
#include "stream.h"
#include "stream_sync.h"

#include <string.h>

#ifdef CONFIG_AUDIO_AC3
extern int libavos_get_ac3_recoding_enabled(void);
#else
static inline int libavos_get_ac3_recoding_enabled(void) { return 0; }
#endif

#ifdef CONFIG_STREAM

#define DBGS	DBG_IF(Debug[DBG_STREAM])
#define DBGV   	DBG_IF(Debug[DBG_VID])
#define DBGVY	DBG_IF(Debug[DBG_VID]||Debug[DBG_SYNC])
#define DBGY	DBG_IF(Debug[DBG_SYNC])
#define DBGY2	DBG_IF(Debug[DBG_SYNC] > 1)
#define DBGY3	DBG_IF(Debug[DBG_SYNC] > 2)
#define DBGV1  	DBG_IF(Debug[DBG_VID] == 1)
#define DBGV2  	DBG_IF(Debug[DBG_VID] > 1)
#define DBGV3 	DBG_IF(Debug[DBG_VID] > 2)

#define DBG DBG_IF(Debug[DBG_SYNC])

extern int stream_max_delay;
extern int stream_no_sync;
extern int stream_video_paused;
extern int stream_audio_paused;
extern int stream_bdrop_threshold;
extern int stream_pdrop_threshold;

static volatile int	stream_dbg_delay = 0;
static int atempo_delay_log_count = 0;
static int _stream_get_atempo_delay( STREAM *s );
static int sync_diag_count = 0;
static int sync_diag_last_seek_epoch = -1;
static int sync_diag_last_speed_x100 = -1;
static int sync_diag_last_pause_state = -1;
static int sync_diag_last_state = -1;
static int sync_diag_last_reanchor_pending = -1;

static int stream_use_xbmc_smoothing = 1;

typedef enum {
	STREAM_DELAY_SOURCE_NONE = 0,
	STREAM_DELAY_SOURCE_DYNAMIC,
	STREAM_DELAY_SOURCE_LAST_GOOD,
	STREAM_DELAY_SOURCE_STATIC,
	STREAM_DELAY_SOURCE_UNKNOWN,
} stream_delay_source_t;

typedef struct {
	int effective_delay_ms; // effective delay currently used by sync math
	int is_anchorable;      // safe to use for sink anchoring
	// Mirrors audio_interface_is_delay_valid(). On Android this usually means a
	// trusted timestamp/playhead delay, but it can also be true for deliberate
	// static fallbacks when dynamic timing is disabled, unavailable, or bypassed
	// for passthrough. Do not assume this always means a live dynamic sample.
	int is_dynamic;
	int is_fallback;        // based on last-good or static latency
	int streak;             // current dynamic-valid streak
	stream_delay_source_t source;
	const char *source_tag;
} stream_delay_status_t;

typedef enum {
	STREAM_SYNC_STATE_BOOTSTRAP = 0,
	STREAM_SYNC_STATE_WAIT_AUDIO,
	STREAM_SYNC_STATE_FALLBACK_RUNNING,
	STREAM_SYNC_STATE_DYNAMIC_RUNNING,
} stream_sync_diag_state_t;

static void _sync_diag_reset(void)
{
	sync_diag_count = 0;
	sync_diag_last_seek_epoch = -1;
	sync_diag_last_speed_x100 = -1;
	sync_diag_last_pause_state = -1;
	sync_diag_last_state = -1;
	sync_diag_last_reanchor_pending = -1;
}

static const char *_stream_delay_source_name(stream_delay_source_t source)
{
	switch (source) {
	case STREAM_DELAY_SOURCE_DYNAMIC:
		return "dynamic";
	case STREAM_DELAY_SOURCE_LAST_GOOD:
		return "last_good";
	case STREAM_DELAY_SOURCE_STATIC:
		return "static";
	case STREAM_DELAY_SOURCE_NONE:
		return "none";
	case STREAM_DELAY_SOURCE_UNKNOWN:
	default:
		return "unknown";
	}
}

static stream_delay_source_t _classify_audio_delay_source(const char *tag, int delay_valid)
{
	if( !tag || !tag[0] ) {
		return delay_valid ? STREAM_DELAY_SOURCE_UNKNOWN : STREAM_DELAY_SOURCE_NONE;
	}
	if( !strncmp( tag, "static", 6 ) ) {
		return STREAM_DELAY_SOURCE_STATIC;
	}
	if( !strncmp( tag, "last_good", 9 ) || !strncmp( tag, "cached", 6 ) ) {
		return STREAM_DELAY_SOURCE_LAST_GOOD;
	}
	if( !strncmp( tag, "playhead", 8 ) || !strncmp( tag, "dynamic", 7 ) ) {
		return STREAM_DELAY_SOURCE_DYNAMIC;
	}
	if( !strncmp( tag, "fallback", 8 ) || !strncmp( tag, "throttle_none", 13 ) ||
		!strncmp( tag, "outlier", 7 ) || !strncmp( tag, "exception", 9 ) ||
		!strncmp( tag, "track_null", 10 ) ) {
		return STREAM_DELAY_SOURCE_NONE;
	}
	return delay_valid ? STREAM_DELAY_SOURCE_UNKNOWN : STREAM_DELAY_SOURCE_NONE;
}

static int _sync_diag_should_log(STREAM *s)
{
	int dbg = Debug[DBG_SYNC];
	if (dbg <= 1) {
		return 0;
	}
	if (dbg > 2) {
		return 1;
	}

	int force = 0;
	if (s) {
		int speed_x100 = (int)(audio_interface_get_audio_speed() * 100.0f + 0.5f);
		int pause_state = (stream_audio_paused ? 1 : 0) | (stream_video_paused ? 2 : 0);
		if (sync_diag_last_seek_epoch != s->seek_epoch) {
			force = 1;
		}
		if (sync_diag_last_speed_x100 != speed_x100) {
			force = 1;
		}
		if (sync_diag_last_pause_state != pause_state) {
			force = 1;
		}
		sync_diag_last_seek_epoch = s->seek_epoch;
		sync_diag_last_speed_x100 = speed_x100;
		sync_diag_last_pause_state = pause_state;
	}

	if (force) {
		sync_diag_count = 0;
		return 1;
	}
	return ((sync_diag_count++ % 100) == 0);
}

static int stream_calc_lwma(int current, int *history, int *count)
{
	if (*count < 3) {
		history[*count] = current;
		(*count)++;
	} else {
		history[0] = history[1];
		history[1] = history[2];
		history[2] = current;
	}

	long long sum = 0;
	int i;
	for (i = 0; i < *count; i++) {
		sum += (long long)(i + 1) * history[i];
	}

	int n = *count;
	if (n == 0) return current;

	return (int)(sum * 2 / (n * (n + 1)));
}

static int _stream_is_sink_driven(STREAM *s)
{
	return s && s->video_sink && s->video_sink->put_time;
}

// True only while audio is genuinely not yet ready to anchor/sync:
//   - audio_start_pending:       first audio write has not happened
//   - audio_resume_pending:      first post-resume audio write has not happened
//   - startup_hold_active && !dynamic: AudioTrack timing not yet stable and no fresh
//                                delay available yet; once dynamic delay is valid the
//                                startup clamp no longer blocks sync even if still active
//   - both video hold flags set: pre-first-write resume hold (released on first write)
// Note: video_hold_for_delay alone (after video_hold_for_resume_audio clears) means
// the video thread is still waiting for delay validity, but audio is already running —
// that is not "waiting for audio" from the sync-state perspective.
static int _stream_is_waiting_for_audio(STREAM *s, const stream_delay_status_t *delay_status)
{
	if (!s) {
		return 0;
	}
	int startup_blocking = s->audio_ctx &&
	                       audio_interface_is_startup_hold_active(s->audio_ctx) &&
	                       !(delay_status && delay_status->is_dynamic);
	return s->audio_start_pending ||
	       s->audio_resume_pending ||
	       startup_blocking ||
	       (s->video_hold_for_delay && s->video_hold_for_resume_audio);
}

static stream_sync_diag_state_t _stream_get_sync_diag_state(STREAM *s, const stream_delay_status_t *delay_status)
{
	if (!s) {
		return STREAM_SYNC_STATE_BOOTSTRAP;
	}
	if (_stream_is_waiting_for_audio(s, delay_status)) {
		return STREAM_SYNC_STATE_WAIT_AUDIO;
	}
	if (delay_status && delay_status->is_dynamic) {
		return STREAM_SYNC_STATE_DYNAMIC_RUNNING;
	}
	if (delay_status && delay_status->is_anchorable) {
		return STREAM_SYNC_STATE_FALLBACK_RUNNING;
	}
	return STREAM_SYNC_STATE_BOOTSTRAP;
}

static const char *_stream_get_sync_diag_state_name(stream_sync_diag_state_t state)
{
	switch (state) {
	case STREAM_SYNC_STATE_WAIT_AUDIO:
		return "WAIT_AUDIO";
	case STREAM_SYNC_STATE_FALLBACK_RUNNING:
		return "FALLBACK_RUNNING";
	case STREAM_SYNC_STATE_DYNAMIC_RUNNING:
		return "DYNAMIC_RUNNING";
	case STREAM_SYNC_STATE_BOOTSTRAP:
	default:
		return "BOOTSTRAP";
	}
}

static void _sync_diag_log_state(STREAM *s, const char *origin, const stream_delay_status_t *delay_status)
{
	stream_sync_diag_state_t state;

	if (!s || Debug[DBG_SYNC] <= 1) {
		return;
	}

	// reanchor_pending: a one-shot valid-delay correction is armed and will
	// fire once streak >= 3.  Shown as an overlay on the base state so the
	// log makes the pending snap visible before it happens.
	int reanchor_pending;
	state = _stream_get_sync_diag_state(s, delay_status);
	reanchor_pending = s->audio_resume_valid_pending;
	if (state != sync_diag_last_state || reanchor_pending != sync_diag_last_reanchor_pending || _sync_diag_should_log(s)) {
		DBGY2 serprintf(
			"sync_state[%s]: %s reanchor_pending=%d dyn=%d fallback=%d anchor=%d delay=%d source=%s tag=%s streak=%d "
			"start_pending=%d resume_pending=%d hold=%d hold_resume=%d "
			"sync_a=%d sync_v=%d seek_epoch=%d seek_done=%d\n",
			origin,
			_stream_get_sync_diag_state_name(state),
			reanchor_pending,
			delay_status ? delay_status->is_dynamic : 0,
			delay_status ? delay_status->is_fallback : 0,
			delay_status ? delay_status->is_anchorable : 0,
			delay_status ? delay_status->effective_delay_ms : 0,
			delay_status ? _stream_delay_source_name(delay_status->source) : "none",
			delay_status && delay_status->source_tag ? delay_status->source_tag : "none",
			delay_status ? delay_status->streak : 0,
			s->audio_start_pending,
			s->audio_resume_pending,
			s->video_hold_for_delay,
			s->video_hold_for_resume_audio,
			s->sync_a_time,
			s->sync_v_time,
			s->seek_epoch,
			s->seek_converge_done);
	}
	sync_diag_last_state = state;
	sync_diag_last_reanchor_pending = reanchor_pending;
}

// ************************************************************
//
//	stream_sync_restart
//
// ************************************************************
int stream_sync_restart( STREAM *s )
{
	s->delay         = 0;
	s->delay_valid   = 0;
	s->last_good_delay_ms = 0;
	s->last_good_delay_valid = 0;
	s->last_good_atempo_delay_ms = 0;
	s->drop          = 0;
	s->drop_P        = 0;
	s->drop_B        = 0;
	
	s->delay_history_count = 0;
	s->av_delay_history_count = 0;
	s->sink_ref_time = -1;
	s->vid_ref_time = -1;

	s->mode2_fill_active = 0;
	s->mode2_fill_start_wall_ms = 0;
	s->mode2_fill_start_pts = STREAM_NO_PTS_VALUE;
	s->mode2_fill_vel_start_wall_ms = 0;
	s->mode2_fill_vel_start_audio = 0;
	s->mode2_last_anchor_audio = -1;
	s->mode2_last_anchor_wall_ms = 0;
	s->interpolated_last_heard_ts = -1;

	_sync_diag_reset();

	return 0;
}

// ************************************************************
//
//	stream_get_heard_audio_ts
//
// ************************************************************
static stream_delay_status_t _stream_get_delay_status(STREAM *s, int allow_static)
{
	stream_delay_status_t status = { 0 };
	int delay_valid = s && s->audio_ctx ? audio_interface_is_delay_valid(s->audio_ctx) : 1;
	const char *source_tag = s && s->audio_ctx ? audio_interface_get_delay_source(s->audio_ctx) : "no_ctx";

	status.source_tag = source_tag;
	status.source = _classify_audio_delay_source(source_tag, delay_valid);

	status.streak = s && s->audio_ctx ? audio_interface_get_delay_valid_streak(s->audio_ctx) : 0;

	if (delay_valid) {
		status.effective_delay_ms = s ? stream_sync_av_delay(s) : 0;
		if( s && s->audio_ctx ) {
			status.source_tag = audio_interface_get_delay_source(s->audio_ctx);
			status.source = _classify_audio_delay_source(status.source_tag, delay_valid);
		}
		status.is_anchorable = 1;
		status.is_dynamic = 1;
		return status;
	}

#ifdef CONFIG_ANDROID
	if (s) {
		if (s->last_good_delay_valid) {
			status.effective_delay_ms = s->last_good_delay_ms + _stream_get_atempo_delay( s );
			// Keep last-good delay as a usable anchor when timing drops invalid
			// during steady playback. This avoids sudden loss of latency compensation.
			status.is_anchorable = 1;
			status.is_fallback = 1;
			status.source = STREAM_DELAY_SOURCE_LAST_GOOD;
			status.source_tag = "last_good(stream)";
		} else if (allow_static && s->audio_ctx) {
			int static_latency = audio_interface_get_latency(s->audio_ctx);
			if (static_latency > 0) {
				status.effective_delay_ms = static_latency;
				status.is_anchorable = 1;
				status.is_fallback = 1;
				status.source = STREAM_DELAY_SOURCE_STATIC;
				status.source_tag = "static(stream)";
			}
		}
	}
#else
	(void)allow_static;
#endif

	return status;
}

static int _get_anchor_delay_ms(STREAM *s, int *valid, int allow_static)
{
	stream_delay_status_t status = _stream_get_delay_status(s, allow_static);
	if (valid) {
		*valid = status.is_anchorable;
	}
	return status.is_anchorable ? status.effective_delay_ms : 0;
}

int stream_get_anchor_delay_ms( STREAM *s, int allow_static )
{
	return _get_anchor_delay_ms( s, NULL, allow_static );
}

static int _stream_interpolate_heard_ts( STREAM *s, int wall_now, int heard_delay,
	int elapsed_cap_ms, int relatch_wall_on_regression, int accept_audio_regression,
	int max_forward_lead_ms, int cap_to_candidate )
{
	int elapsed = 0;
	int current_heard_ts = -1;
	int candidate_heard_ts = s->audio_time - heard_delay;
	int heard_ts;
	if( s->mode2_last_anchor_audio != -1 ) {
		elapsed = wall_now - s->mode2_last_anchor_wall_ms;
		if( elapsed < 0 ) {
			elapsed = 0;
		}
		if( elapsed_cap_ms > 0 && elapsed > elapsed_cap_ms ) {
			elapsed = elapsed_cap_ms;
		}
		current_heard_ts = s->mode2_last_anchor_audio - heard_delay + elapsed;
	}

	if( cap_to_candidate && current_heard_ts != -1 ) {
		heard_ts = current_heard_ts;
		if( heard_ts > candidate_heard_ts ) {
			heard_ts = candidate_heard_ts;
		} else if( accept_audio_regression ) {
			heard_ts = candidate_heard_ts;
		}
		s->mode2_last_anchor_audio = heard_ts + heard_delay;
		s->mode2_last_anchor_wall_ms = wall_now;
		s->interpolated_last_heard_ts = heard_ts;
		return heard_ts;
	}

	if( s->audio_time > s->mode2_last_anchor_audio ) {
		int accept_forward = 1;
		if( max_forward_lead_ms > 0 && current_heard_ts != -1 ) {
			accept_forward = candidate_heard_ts <= current_heard_ts + max_forward_lead_ms;
		}
		if( accept_forward ) {
			s->mode2_last_anchor_audio = s->audio_time;
			s->mode2_last_anchor_wall_ms = wall_now;
		}
	} else if( s->audio_time < s->mode2_last_anchor_audio && accept_audio_regression ) {
		s->mode2_last_anchor_audio = s->audio_time;
		if( relatch_wall_on_regression ) {
			s->mode2_last_anchor_wall_ms = wall_now;
		}
	}
	if( s->mode2_last_anchor_audio == -1 ) {
		return s->audio_time - heard_delay;
	}
	elapsed = wall_now - s->mode2_last_anchor_wall_ms;
	if( elapsed < 0 ) {
		elapsed = 0;
	}
	if( elapsed_cap_ms > 0 && elapsed > elapsed_cap_ms ) {
		elapsed = elapsed_cap_ms;
	}
	heard_ts = s->mode2_last_anchor_audio - heard_delay + elapsed;
	s->interpolated_last_heard_ts = heard_ts;
	return heard_ts;
}

static int _stream_get_heard_audio_ts_internal( STREAM *s, int fallback_ts )
{
	if( !s || !s->audio || !s->audio->valid || s->audio_time < 0 ) {
		return fallback_ts;
	}
#ifdef CONFIG_ANDROID
	int allow_static = 1;
#else
	int allow_static = 0;
#endif

	stream_delay_status_t delay_status = _stream_get_delay_status(s, allow_static);
	int delay_valid = delay_status.is_dynamic;
	int static_latency = s->audio_ctx ? audio_interface_get_latency( s->audio_ctx ) : 0;
	int heard_delay;
	
	// During startup hold, prioritize fresh static latency over potentially stale smoothed values.
	if (s->audio_ctx && audio_interface_is_startup_hold_active(s->audio_ctx)) {
		heard_delay = delay_status.effective_delay_ms;
	} else {
		heard_delay = (s->smoothed_av_delay >= 0) ?
			s->smoothed_av_delay + _stream_get_atempo_delay( s ) :
			delay_status.effective_delay_ms;
	}

	int passthrough_mode = s->audio_sink ? s->audio_sink->get_passthrough( s ) : 0;
	int is_mode2_sync = (passthrough_mode >= 2) || libavos_get_ac3_recoding_enabled();
	int wall_now = atime();

	// Mode 2 startup fill:
	// Compressed writes can advance audio_time before the high-latency passthrough
	// route is audibly stable. During this startup/seek window, drive heard_ts from
	// wall-clock time plus platform static latency instead of the bursty producer
	// cadence. This gives the video scheduler a monotonic audible-time baseline
	// until submitted audio is advancing at real-time speed, then hands off to the
	// steady-state static-latency interpolator.
	if (is_mode2_sync) {
		// Initialize fill window if we are at the very start of a seek epoch
		if (!s->mode2_fill_active && s->sink_ref_time == -1) {
			s->mode2_fill_active = 1;
			s->mode2_fill_start_wall_ms = wall_now;
			s->mode2_fill_start_pts = s->audio_time;
			s->mode2_fill_vel_start_wall_ms = wall_now;
			s->mode2_fill_vel_start_audio = s->audio_time;
			DBG serprintf("mode2_fill_start: wall=%d pts=%d video=%d\n", 
				wall_now, s->mode2_fill_start_pts, s->video_time);
		}

		if (s->mode2_fill_active) {
			int fill_duration = wall_now - s->mode2_fill_start_wall_ms;
			int effective_latency = static_latency;
			if( effective_latency < 0 ) {
				effective_latency = 0;
			}
			int audio_vel_elapsed = wall_now - s->mode2_fill_vel_start_wall_ms;
			int real_dynamic = 0;
			if( audio_vel_elapsed >= 500 ) {
				int a_adv = s->audio_time - s->mode2_fill_vel_start_audio;
				// Exit once submitted audio has advanced close to real time for a
				// short window. This proves the producer side is no longer in the
				// initial fill burst pattern; it is not a latency calibration signal.
				if( a_adv >= 450 ) {
					real_dynamic = 1;
				} else {
					s->mode2_fill_vel_start_wall_ms = wall_now;
					s->mode2_fill_vel_start_audio = s->audio_time;
				}
			}

			// Synthetic Heard TS: strictly track real-world time during fill
			int heard_ts = s->mode2_fill_start_pts - effective_latency + fill_duration;

			// CLAMP: Apply the 50ms startup clamp to the synthetic timeline.
			int passthrough_playhead_proven = s->audio_ctx ?
				audio_interface_passthrough_playhead_advanced( s->audio_ctx ) : 1;
			if( s->sink_ref_time == -1 &&
				passthrough_playhead_proven &&
				heard_ts < s->audio_time - 50 ) {
				int clamped = s->audio_time - 50;
				DBG serprintf( "stream_get_heard_audio_ts: mode2 startup hold clamp: %d->%d (audio=%d video=%d)\n",
					heard_ts, clamped, s->audio_time, s->video_time );
				heard_ts = clamped;
				// Rebase synthetic start so pacing continues from this clamped point
				s->mode2_fill_start_pts = heard_ts + effective_latency - fill_duration;
			}

			if (real_dynamic || fill_duration >= 2000) {
				// EXIT: Handoff to normal logical clock
				s->mode2_fill_active = 0;
				// Rebase audio_time so that (audio_time - effective latency) matches the last synthetic baseline.
				int rebase_audio = heard_ts + effective_latency;
				int cap_video_time = s->video_time;
				if( s->sync_v_time >= 0 &&
					(cap_video_time < 0 || s->sync_v_time > cap_video_time) &&
					(cap_video_time < 0 || s->sync_v_time - cap_video_time < 1000) ) {
					cap_video_time = s->sync_v_time;
				}
				if( cap_video_time >= 0 ) {
					const int rebase_video_margin_ms = 0;
					int rebase_cap = cap_video_time + effective_latency + rebase_video_margin_ms;
					if( rebase_audio > rebase_cap ) {
						DBG serprintf("mode2_fill_exit: capping rebase_audio %d->%d (video=%d sync_v=%d cap_video=%d effective_latency=%d)\n",
							rebase_audio, rebase_cap, s->video_time, s->sync_v_time, cap_video_time, effective_latency);
						rebase_audio = rebase_cap;
					}
				}
				s->audio_time = rebase_audio;
				DBG serprintf("mode2_fill_exit: wall=%d dur=%d rebase_audio=%d real_dynamic=%d playhead_proven=%d\n",
					wall_now, fill_duration, s->audio_time, real_dynamic, passthrough_playhead_proven);

				// SEAMLESS HANDOFF: Force the video scheduler to refresh its anchor
				// using the newly rebased timeline to avoid a discontinuity jump.
				sfdec2_refresh_sched_anchor( s );

				// Initialize Interpolator Reference for a perfect handoff
				s->mode2_last_anchor_audio = s->audio_time;
				s->mode2_last_anchor_wall_ms = wall_now;
			} else {
				// ACTIVE: Follow the synthetic wall-clock Pace
				return heard_ts;
			}
		}
	}

	if (!delay_valid) {
#ifdef CONFIG_ANDROID
		// When atempo is active, include filter delay in heard-time even if timing is invalid.
		if( audio_interface_is_audio_speed_enabled() && audio_interface_is_using_atempo() ) {
			int chain_delay = stream_sync_av_delay( s );
			if( chain_delay > heard_delay ) {
				heard_delay = chain_delay;
			}
		}
#endif
#ifdef CONFIG_ANDROID
		// Startup grace for Mode 1: if timing is invalid at the very start, include static latency.
		if( !is_mode2_sync && s->put_time_mode && s->audio_time > 0 && s->sync_v_time >= 0 &&
			s->sync_v_time < 500 && s->audio_ctx ) {
			if( static_latency > heard_delay ) {
				heard_delay = static_latency;
			}
		}
#endif
	}

	int heard_ts = s->audio_time - heard_delay;

	// PCM AudioTrack timing is sampled in chunks: audio_time advances when we write,
	// while the physical playhead advances continuously between writes. Interpolate
	// heard time during steady put_time playback, but cap it to the latest submitted
	// audio_time - delay frontier so this cannot become an unbounded predictor. This
	// scales with wall time and write cadence instead of a fixed per-call or per-frame
	// slew.
	if( !is_mode2_sync && !passthrough_mode && s->put_time_mode && delay_valid ) {
		heard_ts = _stream_interpolate_heard_ts( s, wall_now, heard_delay,
			0, 1, 0, 0, 1 );
	}

	// 3. STARTUP CLAMP (Non-Mode 2 only)
	if( !is_mode2_sync && passthrough_mode && s->sink_ref_time == -1 &&
		heard_ts < s->audio_time - 50 && s->audio_time > s->video_time ) {
		int clamped = s->audio_time - 50;
		DBG serprintf( "stream_get_heard_audio_ts: mode1 startup hold clamp: %d->%d (audio=%d video=%d)\n",
			heard_ts, clamped, s->audio_time, s->video_time );
		heard_ts = clamped;
	}

	// 4. STEADY-STATE INTERPOLATOR (Mode 2 only)
	// elapsed_cap=1500: must exceed the passthrough burst interval (EAC3 ~576ms,
	// TrueHD ~300ms) so heard_ts rises continuously between bursts at the same
	// 1ms/ms rate as video, keeping diff constant.  500ms was shorter than EAC3
	// burst interval, causing heard_ts to plateau 76ms before each burst and
	// then jump at burst time, creating a visible sawtooth in the A/V diff.
	if (is_mode2_sync && !s->mode2_fill_active) {
		heard_ts = _stream_interpolate_heard_ts( s, wall_now, heard_delay,
			1500, 0, 1, 50, 0 );
	}

	// Preserves full physical delay offset for passthrough startup.
	// For PCM in put_time mode, allow negative heard_ts during the buffer-fill phase
	// (sink_ref_time <= 0) so sfdec2 can schedule frames relative to when audio is heard.
	if( heard_ts < 0 && !passthrough_mode && s->sink_ref_time > 0 ) {
		heard_ts = 0;
	}

	if (_sync_diag_should_log(s)) {
		DBGY2 serprintf("heard_ts_calc: audio_time=%d heard_ts=%d\n", s->audio_time, heard_ts);
	}

	static int last_diag_wall = 0;
	if (wall_now > last_diag_wall + 2000) {
		last_diag_wall = wall_now;
		DBG serprintf("heard_ts_diag: wall=%d audio=%d heard=%d h_delay=%d smoothed=%d eff=%d dyn=%d source=%s tag=%s fill=%d\n",
			wall_now, s->audio_time, heard_ts, heard_delay, s->smoothed_av_delay, 
			delay_status.effective_delay_ms, delay_status.is_dynamic,
			_stream_delay_source_name(delay_status.source),
			delay_status.source_tag ? delay_status.source_tag : "none",
			s->mode2_fill_active);
	}

	return heard_ts;
}

int stream_get_heard_audio_ts( STREAM *s, int fallback_ts )
{
	return _stream_get_heard_audio_ts_internal( s, fallback_ts );
}

// ************************************************************
//
//	stream_sync_init
//
// ************************************************************
int stream_sync_init( STREAM *s, int time )
{
	char hms_buf[32];
	DBG serprintf("stream_sync_init(time = %d (%s))\n", time, ms_to_hms_string(time, hms_buf, sizeof(hms_buf)));
	atempo_delay_log_count = 0;
	_sync_diag_reset();

	s->video_time     = -1;
	s->audio_time     = -1;
	s->audio_ref_time = -1;
	s->audio_start_pending = 0;
	s->audio_start_pts = STREAM_NO_PTS_VALUE;
	s->audio_start_target_ts = STREAM_NO_PTS_VALUE;
	s->smoothed_av_delay = -1;
	s->last_good_delay_ms = 0;
	s->last_good_delay_valid = 0;
	s->last_good_atempo_delay_ms = 0;
	s->warmup_video_frames = 0;
	s->mode2_fill_active = 0;
	s->mode2_fill_start_wall_ms = 0;
	s->mode2_fill_start_pts = STREAM_NO_PTS_VALUE;
	s->mode2_fill_vel_start_wall_ms = 0;
	s->mode2_fill_vel_start_audio = 0;
	s->mode2_last_anchor_audio = -1;
	s->mode2_last_anchor_wall_ms = 0;
	s->interpolated_last_heard_ts = -1;

	if( time != -1 ) {
		s->video_time = time;
	}

	if( s->video->valid && s->audio->valid && !s->slideshow ) {
DBGS serprintf("sync_init\r\n");
 		s->sync_audio   =  1;
		s->sync_video   =  1;
		s->sync_v_time  = -1;
		s->sync_a_time  = -1;
	}

	stream_sync_restart( s );

	return 0;
}

// *****************************************************************************
//
//	_stream_sync_av_delay
//	returns delay in ms in real world domain unscaled by audio speed
//	it contains all hardware delays in the audio and video chain
//
// *****************************************************************************
int stream_sync_av_delay( STREAM *s )
{
	// Defensive check: validate stream pointer and audio/video validity to prevent crashes
	if (!s || !s->audio || !s->video) {
		return 0;
	}

	// this returns delta(audio_delay - video_delay) in ms in real world domain (i.e. ts delta)
	if ( !s->audio->valid || !s->video->valid ) {
		// if we have no audio & video
		return 0;
	}

	// Additional safety: check if sinks are being torn down
	if (s->audio_sink && !s->audio_sink_open) {
		return 0;
	}
	if (s->video_sink && !s->video_sink->is_open) {
		return 0;
	} 
	
	// audio data passes through decoder, filters, and sink
	// world time audio decoder delay not dependant on audio speed
	int codec_delay = s->audio_dec ? s->audio_dec->delay( s->audio ) : 0;

	// world time audio filter delay (sum of all active filters) not dependant on audio speed
	// Only count delays from filters that are actually being applied
	int filter_delay = 0;
	int passthrough = s->audio_sink ? s->audio_sink->get_passthrough( s ) : 0;
	int ac3_recoding = 0;
#ifdef CONFIG_AUDIO_AC3
	ac3_recoding = libavos_get_ac3_recoding_enabled();
#endif

	// atempo filter runs independently of other filters (controls playback speed)
	int atempo_delay = 0;
	int audio_speed_enabled = audio_interface_is_audio_speed_enabled();
	int using_atempo_pref = audio_interface_is_using_atempo();
	int diag_log = _sync_diag_should_log(s);
	int use_atempo = (s->audio_filter_atempo != NULL);
	if (!audio_speed_enabled || !using_atempo_pref) {
		use_atempo = 0;
	}
	// Keep delay accounting aligned with the actual runtime filter path:
	// passthrough and AC3 recoding do not run atempo on samples.
	if (passthrough || ac3_recoding) {
		use_atempo = 0;
	}
	if( use_atempo && s->audio_filter_atempo->delay ) {
		atempo_delay = s->audio_filter_atempo->delay( s->audio_filter_atempo );
		filter_delay += atempo_delay;
	}
	DBGY2 serprintf("stream_sync_av_delay: atempo_delay=%d filter_atempo=%p delay_fn=%p enabled=%d\n",
		atempo_delay, s->audio_filter_atempo,
		s->audio_filter_atempo ? s->audio_filter_atempo->delay : NULL, use_atempo);
	if (atempo_delay_log_count < 10) {
		DBGY2 serprintf("stream_sync_av_delay: atempo_gate[%d] filter=%p speed_enabled=%d using_atempo_pref=%d use=%d delay=%d speed=%.3f\n",
			atempo_delay_log_count, s->audio_filter_atempo, audio_speed_enabled,
			using_atempo_pref, use_atempo, atempo_delay, audio_interface_get_audio_speed());
		atempo_delay_log_count++;
	}

	// Filters run in: normal PCM mode OR AC3 recoding mode (all formats)
	int run_filter = (!passthrough || ac3_recoding);
	if( run_filter ) {
		// Sum delays from filters that are actually applied
		if( s->audio_filter_compress && s->audio_filter_compress->delay ) {
			filter_delay += s->audio_filter_compress->delay( s->audio_filter_compress );
		}
		if( s->audio_filter_ac3 && s->audio_filter_ac3->delay ) {
			filter_delay += s->audio_filter_ac3->delay( s->audio_filter_ac3 );
		}
		if( s->audio_filter && s->audio_filter->delay ) {
			filter_delay += s->audio_filter->delay( s->audio_filter );
		}
	}

	// world time audio sink delay (audiotrack system_delay on android) not dependant on audio speed
	int sink_delay = s->audio_sink ? s->audio_sink->delay( s ) : 0;
	// wold time video sink delay not dependant on audio speed
	int video_delay;
	if( s->vtime_post_sink ) {
		// we sample after the sink, so the time stamps are the one we get out of the sink
		video_delay = 0;
	} else {
		// we sample before the sink, so the video frames have to pass through the sink
	  	video_delay = ( s->video_sink && s->video_sink->delay )  ? s->video_sink->delay( s->video_sink ) : 0;
 	}
	if( s->sync_mode == STREAM_SYNC_SAMPLES ) {
		// In sample-based sync, the audio sink's sample counter is the master clock.
		// The codec_delay is upstream from the sink and not part of this clock,
		// so it's excluded to prevent an incorrect sync bias.
		int total_delay = /*codec_delay +*/ filter_delay + sink_delay - video_delay;
		if (diag_log) {
			DBGY2 serprintf("stream_sync_av_delay: samples mode codec=%d filter=%d (atempo=%d) sink=%d video=%d total=%d speed=%.3f using_atempo=%d passthrough=%d ac3=%d\n",
				codec_delay, filter_delay, atempo_delay, sink_delay, video_delay, total_delay,
				audio_interface_get_audio_speed(), s->audio_filter_atempo != NULL, passthrough, ac3_recoding);
		}
		return total_delay;
	} else {
		int total_delay = codec_delay + filter_delay + sink_delay - video_delay;
		if (diag_log) {
			DBGY2 serprintf("stream_sync_av_delay: codec=%d filter=%d (atempo=%d) sink=%d video=%d total=%d speed=%.3f using_atempo=%d passthrough=%d ac3=%d\n",
				codec_delay, filter_delay, atempo_delay, sink_delay, video_delay, total_delay,
				audio_interface_get_audio_speed(), s->audio_filter_atempo != NULL, passthrough, ac3_recoding);
		}
		return total_delay;
	}
}

// ************************************************************
//
//	_stream_av_diff
//	returns diff in ms in real world domain: i.e. uses ts 
//	scaled timestamps when audio_speed != 1.0 and adding real
//	world delay between audio and video	(based on ts timestamps)
//	and real world hardware delays
//
// ************************************************************
static int _stream_av_diff( STREAM *s, int video_time, int audio_time )
{
	if( s && !s->put_time_mode && _stream_is_sink_driven(s) ) {
		// Lazy init: ensure put_time_mode is active once the sink is available.
		s->put_time_mode = 1;
	}
DBGY	serprintf("stream_av_diff: put_time_mode=%d\n", s ? s->put_time_mode : -1);
	// Computes video presentation time - audio presentation time
	// Positive value means video is ahead of audio, negative means audio is ahead
	// Formula accounts for buffering delays: audio/video timestamps represent generation time,
	// but actual presentation happens later after passing through decoder/filter/sink pipelines
	// So the formula is: ( video_time - video_delay ) - ( audio_time - ( codec_delay + filter_delay + sink_delay ) )
	//                  = video_time - audio_time + codec_delay + filter_delay + sink_delay - video_delay
	// The sync difference is the video timestamp (V_pts) minus the audio clock predicted for when the video frame displays: diff = V_pts - A_clk_pred.
	// This predicted audio clock is A_clk_pred = (A_pts - A_latency) + V_latency, so the final formula is diff = V_pts - A_pts + A_latency - V_latency.
	int sync_delay = stream_sync_av_delay( s );
	int use_heard_time = 0;
	if( s->put_time_mode ) {
		// In put_time mode, audio_time is anchored to heard time via stream_get_heard_audio_ts.
		// Avoid double-counting delay in the diff by using the same heard-time reference.
		int heard_audio_ts = stream_get_heard_audio_ts( s, audio_time );
		audio_time = heard_audio_ts;
		sync_delay = 0;
		use_heard_time = 1;
	}
#ifdef CONFIG_ANDROID
	// In put_time_mode, heard-time is already in the audio-presented domain.
	// Do not re-add smoothed delay here or we double-count latency.
	if( !use_heard_time && s->put_time_mode && s->smoothed_av_delay > 0 ) {
		// Keep diff aligned with the smoothed anchor when not using heard-time.
		sync_delay = s->smoothed_av_delay;
	}
#endif
#ifdef CONFIG_ANDROID

	if( !use_heard_time && sync_delay <= 0 ) {
		int delay_valid = s->audio_ctx ? audio_interface_is_delay_valid( s->audio_ctx ) : 1;
		if( delay_valid && s->smoothed_av_delay > 0 ) {
			sync_delay = s->smoothed_av_delay;
		} else if( delay_valid && s->audio_ctx ) {
			int static_latency = audio_interface_get_latency( s->audio_ctx );
			if( static_latency > 0 ) {
				sync_delay = static_latency;
			}
		}
	}
#endif
	int using_atempo = (s->audio_filter_atempo != NULL);
	// User AV delay is part of A/V relationship and must be visible to sync gating.
	int user_av_delay = s->av_delay + stream_dbg_delay;
	int diff = ( video_time - audio_time ) + sync_delay + RST_TO_TS_DELTA( user_av_delay, int );
DBGY	serprintf("stream_av_diff: v=%d a=%d sync_delay=%d av_delay=%d dbg_delay=%d diff=%d speed=%.3f using_atempo=%d\n",
		video_time, audio_time, sync_delay, s->av_delay, stream_dbg_delay, diff,
		audio_interface_get_audio_speed(), using_atempo);
	return diff;
}

// ************************************************************
//
//	stream_sync_audio
//
// ************************************************************
static int _stream_get_atempo_delay( STREAM *s )
{
	int use_atempo = (s && s->audio_filter_atempo != NULL);
	int ac3_recoding = 0;
	int passthrough = 0;
#ifdef CONFIG_AUDIO_AC3
	ac3_recoding = libavos_get_ac3_recoding_enabled();
#endif
	if( s && s->audio_sink ) {
		passthrough = s->audio_sink->get_passthrough( s );
	}
	if( !use_atempo || !audio_interface_is_audio_speed_enabled() || !audio_interface_is_using_atempo() ) {
		return 0;
	}
	if( passthrough || ac3_recoding ) {
		return 0;
	}
	if( s->audio_filter_atempo->delay ) {
		return s->audio_filter_atempo->delay( s->audio_filter_atempo );
	}
	return 0;
}

static int _apply_user_av_delay_ts( STREAM *s, int ts )
{
	(void)s;
	// Keep put_time anchors in the physical timeline.
	// Manual user AV delay is handled by sync diff and, for
	// negative offsets, by audio-side hold.
	return ts;
}

int stream_sync_audio( STREAM *s, int audio_time )
{
	// Defensive check: validate stream pointer to prevent JNI abort crashes
	if (!s) {
		return 0;
	}
	if( s->sync_a_time == -1 && audio_time != -1 ) {
		DBG serprintf(
			"FIRST_AUDIO_POST_SEEK: audio_time=%d video_time=%d seek_epoch=%d target_sync=%d use_target=%d drop=%d target_ts=%d\n",
			audio_time, s->video_time, s->seek_epoch, s->seek_target_sync_time,
			s->seek_use_target_sync, s->seek_audio_drop, s->seek_audio_target_ts );
	}

#ifdef CONFIG_ANDROID
	int allow_static = 1;
#else
	int allow_static = 0;
#endif
	stream_delay_status_t delay_status = _stream_get_delay_status(s, allow_static);
	int anchor_valid = delay_status.is_anchorable;
	int delay_valid = delay_status.is_dynamic;
	int current_av_delay = delay_status.effective_delay_ms;
	int anchor_delay = current_av_delay;
	int diag_log = _sync_diag_should_log(s);

	_sync_diag_log_state(s, "audio", &delay_status);

	if( !delay_valid && !anchor_valid ) {
		// No usable timing (no dynamic and no static/last-good); disable delay compensation.
		current_av_delay = 0;
		anchor_delay = 0;
	}
	if( delay_valid ) {
		int delay_streak = delay_status.streak;
		int current_atempo_delay = _stream_get_atempo_delay( s );
		int startup_hold_active = s->audio_ctx ? audio_interface_is_startup_hold_active( s->audio_ctx ) : 0;
		int sensitive_phase =
			(startup_hold_active && !delay_valid) ||
			s->audio_start_pending ||
			s->audio_resume_pending ||
			(s->seek_epoch > 0 && !s->seek_converge_done);
		int allow_update = !sensitive_phase || delay_streak >= 3;

		if( !allow_update ) {
			DBG serprintf( "stream_sync_audio: defer last_good update (delay_streak=%d current=%d sensitive=%d)\n",
				delay_streak, current_av_delay, sensitive_phase );
		} else {
			int old_last_good_delay = s->last_good_delay_ms;
			int old_last_good_atempo = s->last_good_atempo_delay_ms;
			int new_last_good_atempo = _stream_get_atempo_delay( s );
			s->last_good_delay_ms = current_av_delay - new_last_good_atempo;
			s->last_good_delay_valid = 1;
			s->last_good_atempo_delay_ms = new_last_good_atempo;
			DBG serprintf( "stream_sync_audio: last_good_delay %d->%d last_good_atempo %d->%d speed=%.3f raw=%d streak=%d sensitive=%d hist=%d\n",
				old_last_good_delay, s->last_good_delay_ms,
				old_last_good_atempo, s->last_good_atempo_delay_ms,
				audio_interface_get_audio_speed(), current_av_delay, delay_streak,
				sensitive_phase, s->av_delay_history_count );
		}
		int old_smoothed = s->smoothed_av_delay;
		int hw_delay = current_av_delay - current_atempo_delay;
		if( s->smoothed_av_delay == -1 ) {
			s->smoothed_av_delay = hw_delay;
		} else {
			// Check if passthrough mode is active (constant latency, no smoothing needed)
			int passthrough = s->audio_sink ? s->audio_sink->get_passthrough( s ) : 0;
			if( !passthrough ) {
				// Normal mode: smooth hw-only delay (atempo FIFO added live at consumption)
				if (stream_use_xbmc_smoothing) {
					s->smoothed_av_delay = stream_calc_lwma(hw_delay, s->av_delay_history, &s->av_delay_history_count);
				} else {
					s->smoothed_av_delay = (s->smoothed_av_delay * s->delay_fb + hw_delay * (1000 - s->delay_fb)) / 1000;
				}
			}
		}
		DBG serprintf( "stream_sync_audio: smoothed_av_delay %d->%d raw=%d speed=%.3f atempo=%d streak=%d hist=%d\n",
			old_smoothed, s->smoothed_av_delay, current_av_delay,
			audio_interface_get_audio_speed(), _stream_get_atempo_delay( s ),
			delay_streak, s->av_delay_history_count );
	}
	// Check if passthrough mode is active - static delay is immediately valid
	int passthrough_mode = s->audio_sink ? s->audio_sink->get_passthrough( s ) : 0;
	if( passthrough_mode == 1 ) {
		DBG serprintf("pt_mode1_sync_audio: in=%d stored=%d sync_a=%d video=%d seek_epoch=%d anchor_valid=%d delay_valid=%d source=%s tag=%s anchor_delay=%d current_delay=%d sink_ref=%d\n",
			audio_time, s->audio_time, s->sync_a_time, s->video_time, s->seek_epoch,
			anchor_valid, delay_valid, _stream_delay_source_name(delay_status.source),
			delay_status.source_tag ? delay_status.source_tag : "none",
			anchor_delay, current_av_delay, s->sink_ref_time);
	}
	
	if( anchor_delay > 0 && _stream_is_sink_driven(s) && audio_time != -1 && !passthrough_mode ) {
		int anchor_ts_raw = audio_time - anchor_delay;
		if( anchor_ts_raw < 0 ) {
			if (diag_log) {
				DBGY2 serprintf("anchor_wait: audio_time=%d delay=%d av_delay=%d\n",
					audio_time, anchor_delay, s->av_delay);
			}
			// Defer anchoring until audible time exists.
			// Set sync_a_time so _check_sink_ref_time in the video thread can proceed
			// to seed the initial negative anchor on sfdec2.
			s->sync_a_time = audio_time;
			return 0;
		}
	}

	if( anchor_valid && _stream_is_sink_driven(s) && audio_time != -1 ) {
		if( !stream_no_sync || s->sync_a_time == -1 ) {
			// Centralized heard_ts calculation (includes Mode 2 Fill and Startup Clamps)
			int anchor_ts = stream_get_heard_audio_ts( s, audio_time );

			int force_passthrough_reanchor =
				(passthrough_mode > 0) && 
				s->video_sink &&
				s->video_sink->put_time &&
				(s->sink_ref_time == -1 || s->sync_a_time == -1);

			if (diag_log) {
				DBGY2 serprintf("anchor_ts: audio_time=%d smoothed=%d current=%d source=%s tag=%s av_delay=%d anchor=%d mode2_fill=%d\n",
					audio_time, s->smoothed_av_delay, current_av_delay,
					_stream_delay_source_name(delay_status.source),
					delay_status.source_tag ? delay_status.source_tag : "none",
					s->av_delay, anchor_ts, s->mode2_fill_active);
			}
			anchor_ts = _apply_user_av_delay_ts( s, anchor_ts );
			anchor_ts -= RST_TO_TS_DELTA( stream_dbg_delay, int );

			if( force_passthrough_reanchor && s->sink_ref_time != -1 ) {
				DBG serprintf("stream_sync_audio: passthrough explicit reanchor: old_sink=%d audio=%d video=%d seek_epoch=%d resume_pending=%d sync_a=%d\n",
					s->sink_ref_time, audio_time, s->video_time, s->seek_epoch,
					s->audio_resume_pending, s->sync_a_time);
				sfdec2_refresh_sched_anchor( s );
			}
			
			// In put_time mode, steady-state anchoring is now driven by the video thread.
			// EXCEPTION: Always allow the audio thread to provide the VERY FIRST anchor
			// (sink_ref_time == -1) to seed the scheduler immediately.
			if (!s->put_time_mode || s->sink_ref_time == -1) {
				s->video_sink->put_time( s->video_sink, anchor_ts );
				s->sink_ref_time = anchor_ts;
				s->vid_ref_time = s->video_time;
			}
		}
	}

	if (diag_log) {
		DBGY2 serprintf("smoothed_av_delay: %d (raw: %d)\n", s->smoothed_av_delay, current_av_delay);
	}

	s->sync_a_time = audio_time;
	
	if( !s->sync_audio || s->play_n_audio_frames || stream_no_sync ) {
		return 0;
	}
	
	// let data without timestamp pass!
	if( audio_time == -1 ) {
		return 0;
	}

	// video is already at end, let data pass!
	if( s->video_end ) {
		return 0;
	}

	// video is paused, ignore
	if( stream_video_paused ) {
		return 0;
	}

DBGY serprintf("{SSA %d}} ", audio_time );
	// both audio and video need to have a valid timestamp before we can start
	int audio_time_for_diff = s->sync_a_time;
	if( s->put_time_mode && s->audio_time != -1 ) {
		// In put_time mode, compare against heard time to stay aligned with sink anchoring.
		// CRITICAL: For Mode 2, use the raw logical heard-time for flow control.
		// Decouple from synthetic wall-clock pacing (fill window) to prevent the audio
		// thread from blocking itself.
		int passthrough_mode = s->audio_sink ? s->audio_sink->get_passthrough( s ) : 0;
		int is_mode2_sync = (passthrough_mode >= 2) || libavos_get_ac3_recoding_enabled();
		if (is_mode2_sync) {
			int static_latency = s->audio_ctx ? audio_interface_get_latency( s->audio_ctx ) : 0;
			audio_time_for_diff = s->audio_time - static_latency;
		} else {
			audio_time_for_diff = stream_get_heard_audio_ts( s, s->audio_time );
		}
	}
	if( s->sync_v_time == -1 || audio_time_for_diff == -1 ) {
		return 1;
	}
	
	// if audio is in the future, delay it (but only if significantly ahead)
	int diff = _stream_av_diff( s, s->sync_v_time, audio_time_for_diff );

	// Only block audio if it's significantly ahead (more than threshold)
	if( diff < 0 ) {
DBGY serprintf("{{A %d}} ", diff );
		// In put_time mode, keep video gating active; audio sets the anchor.
		if( !s->put_time_mode ) {
			s->sync_video = 0;
		}
		return 1;
	}
	s->sync_audio = 0;
	return 0;
}

// ************************************************************
//
//	stream_sync_video
//
// ************************************************************
int stream_sync_video( STREAM *s, int video_time )
{
	// Defensive check: validate stream pointer to prevent JNI abort crashes
	if (!s) {
		return 0;
	}

	// Additional validation: check critical nested pointers to prevent use-after-free crashes
	// This can occur during stream teardown when the STREAM structure is being deallocated
	// while another thread is still calling this function
	if (!s->audio || !s->video) {
		return 0;
	}

	s->sync_v_time = video_time;

	// For passthrough mode, don't bypass sync even during play_n_video_frames.
	// The large static latency means we need to block video until audio buffer fills.
	int passthrough_mode = (s->audio_sink && s->audio_sink->get_passthrough(s)) ? 1 : 0;
	if( !s->sync_video || s->speed != STREAM_SPEED_NORMAL || (s->play_n_video_frames && !passthrough_mode) || stream_no_sync ) {
		return 0;
	}
	// let data without timestamp pass!
	if( video_time == -1 ) {
		return 0;
	}
	
	// audio is already at end, let data pass!
	if( s->audio_end ) {
		return 0;
	}
		
	// audio is paused, ignore
	if( stream_audio_paused ) {
		return 0;
	}

#ifdef CONFIG_ANDROID
	{
		stream_delay_status_t delay_status = _stream_get_delay_status(s, 1);
		int anchor_valid = delay_status.is_anchorable;
		int passthrough_mode = (s->audio_sink && s->audio_sink->get_passthrough(s)) ? 1 : 0;
		int delay_valid = delay_status.is_dynamic;

		_sync_diag_log_state(s, "video", &delay_status);

		// PCM static-start guard: keep heard_ts calculation pure and handle the
		// late-audio startup heuristic here as a video-release decision. Static
		// fallback can over-shift heard time before dynamic timing settles, so if
		// audio is already far ahead of early video, briefly suppress anchoring.
		if( !passthrough_mode && anchor_valid && delay_valid == 1 && s->put_time_mode &&
			delay_status.source == STREAM_DELAY_SOURCE_STATIC &&
			delay_status.effective_delay_ms > 0 &&
			s->audio_time > 0 && s->sync_v_time >= 0 && s->sync_v_time < 1000 ) {
			int audio_lead = s->audio_time - s->sync_v_time;
			if( audio_lead > 150 ) {
				anchor_valid = 0;
			}
		}

		// For passthrough with static latency, prefer anchoring even if timing is "invalid".
		if( passthrough_mode && delay_valid == 1 ) {
			anchor_valid = 1;
		}

		if( !anchor_valid ) {
			// Startup grace: if audio has started but timing is invalid, allow a brief
			// static-latency anchor to avoid large A/V offset at start.
			if( s->put_time_mode && s->audio_time > 0 && s->sync_v_time >= 0 &&
				s->sync_v_time < 500 && delay_valid == 0 ) {
				int static_latency = s->audio_ctx ? audio_interface_get_latency(s->audio_ctx) : 0;
				if( static_latency > 0 ) {
					// We only need anchor_valid to allow sync_video gating; anchor delay itself
					// is still computed via stream_get_heard_audio_ts().
					anchor_valid = 1;
				}
			}
		}
		if( !anchor_valid ) {
			// Passthrough: don't free-run video when timing is unavailable; block instead.
			if( passthrough_mode && s->put_time_mode ) {
				return 1;
			}
DBGY			serprintf("sync_video: timing unavailable, free-run video (anchor_valid=0 smoothed=%d audio_time=%d sync_a=%d vtime=%d put_time=%d delay_valid=%d source=%s tag=%s)\n",
				s->smoothed_av_delay, s->audio_time, s->sync_a_time, s->sync_v_time,
				s->put_time_mode, delay_valid,
				_stream_delay_source_name(delay_status.source),
				delay_status.source_tag ? delay_status.source_tag : "none");
			return 0;
		}
	}
#else
	{
		stream_delay_status_t delay_status = _stream_get_delay_status(s, 0);
		int anchor_valid = delay_status.is_anchorable;
		_sync_diag_log_state(s, "video", &delay_status);
		if( !anchor_valid ) {
DBGY			serprintf("sync_video: timing unavailable, free-run video (anchor_valid=0 smoothed=%d audio_time=%d sync_a=%d vtime=%d put_time=%d)\n",
				s->smoothed_av_delay, s->audio_time, s->sync_a_time, s->sync_v_time,
				s->put_time_mode);
			return 0;
		}
	}
#endif
	
DBGY serprintf("{SSV %d}} ", video_time );
	// both audio and video need to have a valid timestamp before we can start
	int audio_time_for_diff = s->sync_a_time;
	if( s->put_time_mode && s->audio_time != -1 ) {
		// In put_time mode, compare against heard time to stay aligned with sink anchoring.
		audio_time_for_diff = stream_get_heard_audio_ts( s, s->audio_time );
	}
	if( s->sync_v_time == -1 || audio_time_for_diff == -1 )
		return 1;
	if( s->put_time_mode && audio_time_for_diff <= 0 ) {
		// For passthrough mode, don't allow ANY video frames until audio reaches audible time.
		// The large static latency means audio won't be heard until buffer fills completely.
		int passthrough_mode = s->audio_sink ? s->audio_sink->get_passthrough( s ) : 0;
		if( passthrough_mode ) {
			DBG serprintf("stream_sync_video: blocking video at vtime=%d (heard_ts=%d <= 0, passthrough=%d)\n",
				s->sync_v_time, audio_time_for_diff, passthrough_mode);
			return 1;  // Block video completely until audio catches up
		}
		// Non-passthrough: Startup/resume warm-up: allow a few frames before audible time exists.
		if( s->warmup_video_frames < 5 ) {
			s->warmup_video_frames++;
			return 0;
		}
		return 1;
	}

	// if video is in the future, delay it
	int diff = _stream_av_diff( s, s->sync_v_time, audio_time_for_diff );

	if ( s->put_time_mode && _stream_is_sink_driven(s) ) {
		// ANCHOR: Continuously update the sink anchor from the smooth 
		// wall-clock interpolated timeline. This must happen before 
		// any early-returns to ensure the scheduler stays stable.
		int anchor_ts = stream_get_heard_audio_ts( s, s->audio_time );
		anchor_ts = _apply_user_av_delay_ts( s, anchor_ts );
		anchor_ts -= RST_TO_TS_DELTA( stream_dbg_delay, int );
		
		s->video_sink->put_time( s->video_sink, anchor_ts );
		s->sink_ref_time = anchor_ts;
		s->vid_ref_time = s->video_time;
	}

	// Legacy post-sink pipelines needed a large early-start allowance because
	// video_time was sampled after the sink. In put_time mode the sink is
	// already paced from audio-driven anchors, so carrying that 500 ms grace
	// forward lets video render materially ahead of heard audio during atempo
	// speed states. Keep the grace only for non-put_time sinks.
	int max_rst = (s->vtime_post_sink && !s->put_time_mode) ? 500 : 0;
	if( s->seek_epoch > 0 && s->put_time_mode && !s->seek_converge_done ) {
		// During post-seek convergence, don't allow early start.
		max_rst = 0;
	}

	// Post-seek convergence: allow a short window to align to heard audio,
	// then re-anchor once and stop gating to avoid stutter.
	if( s->seek_epoch > 0 && s->put_time_mode && s->audio_time != -1 ) {
		if( s->seek_converge_epoch != s->seek_epoch ) {
			s->seek_converge_epoch = s->seek_epoch;
			s->seek_converge_until_ms = atime() + 500;
			s->seek_converge_done = 0;
		}
		if( !s->seek_converge_done && atime() >= s->seek_converge_until_ms ) {
			if( _stream_is_sink_driven(s) ) {
				int passthrough_mode = s->audio_sink ? s->audio_sink->get_passthrough( s ) : 0;
				int frame_ms = (s->video && s->video->msPerFrame > 0) ? s->video->msPerFrame : 33;
				int catchup_threshold = MAX( stream_get_anchor_delay_ms( s, 1 ), frame_ms * 8 );
				int converge_threshold = MAX( frame_ms * 2, 60 );
				if( passthrough_mode == 2 && s->mode2_fill_active ) {
					// Do not complete post-seek convergence while mode-2 is still
					// running the synthetic fill clock.  The fill-exit rebase is the
					// first authoritative handoff to steady-state timing.
DBGY					serprintf("post-seek converge pending: fill active diff=%d threshold=%d passthrough=%d\n",
						diff, converge_threshold, passthrough_mode);
					s->seek_converge_until_ms = atime() + 100;
				} else if( passthrough_mode == 2 &&
					(diff < -converge_threshold || diff > converge_threshold) ) {
					// Mode-2 encapsulation can briefly expose decoded video far from
					// the heard timeline after the synthetic fill window exits.  Do
					// not turn a multi-second mismatch into a fresh scheduler truth:
					// negative gaps should keep using the existing mapping so the
					// sink can drop toward the heard frontier, while positive gaps
					// must keep the video gate active until audio catches up.
DBGY					serprintf("post-seek converge pending: skip anchor diff=%d threshold=%d catchup=%d passthrough=%d\n",
						diff, converge_threshold, catchup_threshold, passthrough_mode);
					s->seek_converge_until_ms = atime() + 100;
				} else {
					int anchor_ts = stream_get_heard_audio_ts( s, s->audio_time );
DBGY				serprintf("post-seek converge anchor: diff=%d anchor_ts=%d\n",
						diff, anchor_ts);
					anchor_ts = _apply_user_av_delay_ts( s, anchor_ts );
					// The audio path may have already called put_time() with the same
					// heard timestamp, making this call a no-op in codec_sfdec2
					// (disc=0, allow_reanchor=0 → sched anchors unchanged).  Force a
					// scheduler reset so the wall-clock anchor is always refreshed at
					// the converge point regardless of whether the value changed.
					sfdec2_refresh_sched_anchor( s );
					s->video_sink->put_time( s->video_sink, anchor_ts );
					s->sink_ref_time = anchor_ts;
					s->vid_ref_time = s->video_time;
					s->seek_converge_done = 1;
				}
			} else {
				s->seek_converge_done = 1;
			}
		}
		if( s->seek_converge_done ) {
			// After convergence, stop gating to avoid stutter.
			return 0;
		}
	}
	// Wait if video is LATE by more than the threshold.
	int max_wait = RST_TO_TS_DELTA( max_rst, int );
	if( diff > max_wait ) {
DBGY serprintf( "{{V %d}} ", diff );
		s->sync_audio = 0;
		return 1; // Wait
	}

	// allow video to play from now on
	if( !s->put_time_mode ) {
		s->sync_video = 0;
	}
	
	return 0;
}

// ************************************************************
//
//	stream_sync
//
// ************************************************************
void stream_sync( STREAM *s )
{
	// Defensive check: validate stream pointer and nested pointers to prevent JNI abort crashes
	// This crash can occur during stream teardown when the STREAM structure is being deallocated
	// while another thread (e.g., FileObserver) is still calling this function
	if (!s || !s->audio || !s->video) {
		return;
	}

	// if we have audio ...

	if ( !s->audio->valid || !s->video->valid )
		goto EXIT;
		
	if ( s->speed )
		goto EXIT;

	if ( s->slideshow )
		goto EXIT;
	
	// sync machine still working?
	if( (s->sync_audio || s->sync_video) && !stream_no_sync ) 
		goto EXIT;

	// audio and video time known?
	if ( s->audio_time <= 0 || s->video_time <= 0 )
		goto EXIT;
	
	// audio done?
	if ( s->audio_end )
		goto EXIT;
	
	// ... we calc the delay between audio and video frames and
	// try adjust it to zero
	int rdiff = _stream_av_diff( s, s->video_time, s->audio_time );
	// let's clamp to avoid big correction visual effect to the user and grant stability
	int clamp_ts = RST_TO_TS_DELTA( 250, int );
	int diff  = MAX( MIN( rdiff,  clamp_ts ), -clamp_ts );
	
 	if( !s->delay_valid ) {
DBGVY serprintf("(D %d)", diff );
		s->delay = diff;
	} else {
		// to avoid oscillations, we use moving average to allow convergence and grant stability
		// 900 s->delay_fb is used for a exponential moving average window and thus has no scale
		if (stream_use_xbmc_smoothing) {
			s->delay = stream_calc_lwma(diff, s->delay_history, &s->delay_history_count);
		} else {
			s->delay = (s->delay * s->delay_fb + diff * (1000 - s->delay_fb)) / 1000;
		}
	}
	s->delay_valid = 1;
	
DBGVY serprintf("(%3d|%3d|%3d)", rdiff, diff, s->delay );

	if ( stream_no_sync || _stream_is_sink_driven(s) ) {
		// ANDROID: this is the android mode with a put_time function in the video sink which basically disables the sync logic
		goto EXIT;
	}
		 
	s->drop_B = 0;

	// --- A/V Sync Correction Logic ---
	// The decision to drop/double frames is made by comparing the A/V delay in the RST (real-world ms) domain
	// to ensure the sync window tolerance is constant regardless of playback speed.
	// The internal state variable s->delay remains in the TS (Time-Scaled) domain, and adjustments to it
	// must also be in the TS domain.

	// 1. Define the threshold in the RST domain for making decisions. stream_max_delay default value is 1 i.e. threshold is one frame duration.
	int threshold_rst = stream_max_delay * s->video->msPerFrame;
	int pdrop_threshold_rst = stream_pdrop_threshold; // default value is 0

	// 2. Define the adjustment value in the TS domain for modifying the state variable.
	int adjustment_ts = RST_TO_TS_DELTA(threshold_rst, int);

	// 3. Perform comparisons in the TS domain to avoid accumulating rounding errors from TS->RST conversion at variable speeds.
	if( s->delay > adjustment_ts ) {
		// video is too fast, we have to slow down
		s->drop = -1;
		// Adjust the TS-domain state variable by a TS-domain value for reactivity and do not go through the averaging smoothering
		s->delay -= adjustment_ts;
DBGVY serprintf( "_S(%3d)_", s->delay );
	} else if( s->delay < (-1 * adjustment_ts) ) {
		// video is late, check for P-frame drop condition (note that this is disabled by default	)
		if( stream_pdrop_threshold && TS_TO_RST_DELTA(rdiff, int) < (-1 * pdrop_threshold_rst) ) {
			// we are totally late, see if we can skip to next key frame
			int max_time = s->video_time - rdiff + 500;
			int key_time;
			int num;
			if( (num = stream_parser_find_key_frame( s, max_time, &key_time )) ) {
				int dropped = stream_parser_drop_video( s, key_time );
DBGVY serprintf("XX(%d %d %d) ", num, key_time, dropped );
				// set drop_P to 1 which means drop all you have and restart
				s->drop_P = 1;
				return;
			}
		}

		// video is late, hurry up (B-frame or P-frame drop)
		// Note: stream_bdrop_threshold is a frame-count multiplier, not a time duration.
		// The comparison uses s->delay (TS) and a threshold based on the frame duration converted to TS.
		if( stream_bdrop_threshold && s->delay < ( -1 * stream_bdrop_threshold * RST_TO_TS_DELTA(s->video->msPerFrame, int) ) ) {
			s->drop_B = 1;
		}
		s->drop = 1;
		// Adjust the TS-domain state variable by a TS-domain value for reactivity and do not go through the averaging smoothering
		s->delay += adjustment_ts;
DBGVY serprintf("_%s(%3d)_", s->drop_B ? "B" : "F", s->delay );
	} else {
		DBGVY serprintf( "  (   ) " );
	}
	return;
EXIT:
DBGV serprintf("  (---) " );
}

#ifdef DEBUG_MSG
void *AV_get_ctx( void );

static void _stream_delay_plus( int argc, char *argv[] )
{
	if( argc > 1 ) {
		stream_dbg_delay += atoi( argv[1] );
	} else {
		stream_dbg_delay += 20;
	}
	
	STREAM *s = AV_get_ctx();
	if( s ) {
serprintf("av_delay  %5d\n", stream_sync_av_delay( s ) );
	}
serprintf("dbg_delay %5d\n", stream_dbg_delay );
}

static void _stream_delay_minus( int argc, char *argv[] )
{
	if( argc > 1 ) {
		stream_dbg_delay -= atoi( argv[1] );
	} else {
		stream_dbg_delay -= 20;
	}
	STREAM *s = AV_get_ctx();
	if( s ) {
serprintf("av_delay  %5d\n", stream_sync_av_delay( s ) );
	}
serprintf("dbg_delay %5d\n", stream_dbg_delay );
}

static void _stream_delay_set( int argc, char *argv[] )
{
	if( argc > 1 ) {
		stream_dbg_delay = atoi( argv[1] );
	}
	STREAM *s = AV_get_ctx();
	if( s ) {
serprintf("av_delay  %5d\n", stream_sync_av_delay( s ) );
	}
serprintf("dbg_delay %5d\n", stream_dbg_delay );
}

DECLARE_DEBUG_COMMAND("sep", 	_stream_delay_plus   );
DECLARE_DEBUG_COMMAND("sem", 	_stream_delay_minus  );
DECLARE_DEBUG_COMMAND("ses", 	_stream_delay_set    );

static void _stream_toggle_xbmc( int argc, char *argv[] )
{
	stream_use_xbmc_smoothing = !stream_use_xbmc_smoothing;
	serprintf("stream_use_xbmc_smoothing: %d\n", stream_use_xbmc_smoothing );
}
DECLARE_DEBUG_COMMAND("sxbmc", _stream_toggle_xbmc );
#endif

#endif
