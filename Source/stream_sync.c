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
int stream_get_atempo_delay( STREAM *s );
static void _stream_reset_mode2_clock( STREAM *s );
static int sync_diag_count = 0;
static int sync_diag_last_seek_epoch = -1;
static int sync_diag_last_speed_x100 = -1;
static int sync_diag_last_pause_state = -1;
static int sync_diag_last_state = -1;
static int sync_diag_last_reanchor_pending = -1;

static int stream_use_xbmc_smoothing = 1;
int stream_mode2_dynamic_delay = 1;
static int stream_mode2_dynamic_max_ms = 150;
static int stream_mode2_dynamic_slew_ms = 5;
static int stream_mode2_dynamic_streak = 10;

#define STREAM_MODE1_STARTUP_CLAMP_MS        50
#define STREAM_SEEK_CONVERGE_WINDOW_MS       500
#define STREAM_SEEK_CONVERGE_MAX_WAIT_MS     1500
#define STREAM_SEEK_CONVERGE_APPLY_DIFF_MS   80
#define STREAM_PCM_AUDIO_LEAD_HOLD_THRESHOLD_MS 250
#define STREAM_PCM_AUDIO_LEAD_HOLD_LOG_INTERVAL 50

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
	// for passthrough. Check source before assuming a live dynamic sample.
	int is_delay_valid;
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

static int _stream_is_mode2_audio_interface_delay(const stream_delay_status_t *status)
{
	if( !status || !status->source_tag ) {
		return 0;
	}
	if( status->source == STREAM_DELAY_SOURCE_DYNAMIC ) {
		return 1;
	}
	if( status->source != STREAM_DELAY_SOURCE_LAST_GOOD ) {
		return 0;
	}
	return strncmp( status->source_tag, "last_good(stream)", 17 ) != 0;
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
	                       !(delay_status && delay_status->is_delay_valid);
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
	if (delay_status && delay_status->is_delay_valid) {
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
			"sync_state[%s]: %s reanchor_pending=%d delay_valid=%d fallback=%d anchor=%d delay=%d source=%s tag=%s streak=%d "
			"start_pending=%d resume_pending=%d hold=%d hold_resume=%d "
			"sync_a=%d sync_v=%d seek_epoch=%d seek_done=%d\n",
			origin,
			_stream_get_sync_diag_state_name(state),
			reanchor_pending,
			delay_status ? delay_status->is_delay_valid : 0,
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
	s->pcm_reanchor_state = STREAM_PCM_REANCHOR_INACTIVE;
	s->pcm_reanchor_seek_epoch = -1;
	s->pcm_reanchor_source = 0;
	s->pcm_reanchor_delay_ms = 0;
	s->drop          = 0;
	s->drop_P        = 0;
	s->drop_B        = 0;
	
	s->delay_history_count = 0;
	s->av_delay_history_count = 0;
	s->sink_ref_time = -1;
	s->vid_ref_time = -1;
	s->seek_converge_state = STREAM_SEEK_CONVERGE_INACTIVE;
	s->seek_converge_anchor_ts = STREAM_NO_PTS_VALUE;
	s->pcm_audio_lead_hold_count = 0;
	s->pcm_audio_lead_last_diff = 0;

	s->heard_interp_anchor_audio = -1;
	s->heard_interp_anchor_wall_ms = 0;
	s->heard_interp_last_ts = -1;
	_stream_reset_mode2_clock( s );

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

	if( s && s->audio_ctx && s->audio_sink && stream_mode2_dynamic_delay &&
		s->audio_sink->get_passthrough( s ) >= 2 ) {
		int measured_delay = audio_interface_get_delay( s->audio_ctx );
		delay_valid = audio_interface_is_delay_valid( s->audio_ctx );
		status.source_tag = audio_interface_get_delay_source( s->audio_ctx );
		status.source = _classify_audio_delay_source( status.source_tag, delay_valid );
		status.streak = audio_interface_get_delay_valid_streak( s->audio_ctx );
		if( delay_valid ) {
			status.effective_delay_ms = measured_delay;
			status.is_anchorable = 1;
			status.is_delay_valid = 1;
			return status;
		}
		status.is_delay_valid = 0;
		status.is_anchorable = 0;
	}

	if (delay_valid) {
		status.effective_delay_ms = s ? stream_sync_av_delay(s) : 0;
		if( s && s->audio_ctx ) {
			status.source_tag = audio_interface_get_delay_source(s->audio_ctx);
			status.source = _classify_audio_delay_source(status.source_tag, delay_valid);
		}
		status.is_anchorable = 1;
		status.is_delay_valid = 1;
		return status;
	}

#ifdef CONFIG_ANDROID
	if (s) {
		if (s->last_good_delay_valid) {
			status.effective_delay_ms = s->last_good_delay_ms + stream_get_atempo_delay( s );
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
	if( s->heard_interp_anchor_audio != -1 ) {
		elapsed = wall_now - s->heard_interp_anchor_wall_ms;
		if( elapsed < 0 ) {
			elapsed = 0;
		}
		if( elapsed_cap_ms > 0 && elapsed > elapsed_cap_ms ) {
			elapsed = elapsed_cap_ms;
		}
		current_heard_ts = s->heard_interp_anchor_audio - heard_delay + elapsed;
	}

	if( cap_to_candidate && current_heard_ts != -1 ) {
		heard_ts = current_heard_ts;
		if( heard_ts > candidate_heard_ts ) {
			heard_ts = candidate_heard_ts;
		} else if( accept_audio_regression ) {
			heard_ts = candidate_heard_ts;
		}
		s->heard_interp_anchor_audio = heard_ts + heard_delay;
		s->heard_interp_anchor_wall_ms = wall_now;
		s->heard_interp_last_ts = heard_ts;
		return heard_ts;
	}

	if( s->audio_time > s->heard_interp_anchor_audio ) {
		int accept_forward = 1;
		if( max_forward_lead_ms > 0 && current_heard_ts != -1 ) {
			accept_forward = candidate_heard_ts <= current_heard_ts + max_forward_lead_ms;
		}
		if( accept_forward ) {
			s->heard_interp_anchor_audio = s->audio_time;
			s->heard_interp_anchor_wall_ms = wall_now;
		}
	} else if( s->audio_time < s->heard_interp_anchor_audio && accept_audio_regression ) {
		s->heard_interp_anchor_audio = s->audio_time;
		if( relatch_wall_on_regression ) {
			s->heard_interp_anchor_wall_ms = wall_now;
		}
	}
	if( s->heard_interp_anchor_audio == -1 ) {
		return s->audio_time - heard_delay;
	}
	elapsed = wall_now - s->heard_interp_anchor_wall_ms;
	if( elapsed < 0 ) {
		elapsed = 0;
	}
	if( elapsed_cap_ms > 0 && elapsed > elapsed_cap_ms ) {
		elapsed = elapsed_cap_ms;
	}
	heard_ts = s->heard_interp_anchor_audio - heard_delay + elapsed;
	s->heard_interp_last_ts = heard_ts;
	return heard_ts;
}

static void _stream_reset_mode2_clock( STREAM *s )
{
	if( !s ) {
		return;
	}
	s->mode2_clock_anchor_heard_ts = STREAM_NO_PTS_VALUE;
	s->mode2_clock_anchor_audio = STREAM_NO_PTS_VALUE;
	s->mode2_clock_anchor_wall_ms = 0;
	s->mode2_clock_anchor_seek_epoch = -1;
	s->mode2_clock_anchor_latency_ms = 0;
	s->mode2_clock_last_heard_ts = STREAM_NO_PTS_VALUE;
	s->mode2_clock_last_audio_time = STREAM_NO_PTS_VALUE;
	s->mode2_dynamic_correction_ms = 0;
	s->mode2_dynamic_correction_target_ms = 0;
	s->mode2_dynamic_last_update_wall_ms = 0;
	s->mode2_dynamic_last_log_wall_ms = 0;
}

static int _stream_clamp_ms( int value, int min_value, int max_value )
{
	if( value < min_value ) {
		return min_value;
	}
	if( value > max_value ) {
		return max_value;
	}
	return value;
}

static int _stream_mode2_dynamic_correction( STREAM *s, int wall_now,
	int static_latency, const stream_delay_status_t *delay_status )
{
	int target = 0;
	int active = 0;

	if( !s || !stream_mode2_dynamic_delay || static_latency <= 0 ||
		s->audio_start_pending || s->audio_resume_pending ||
		(s->seek_epoch > 0 && !s->seek_converge_done) ||
		!delay_status ) {
		target = 0;
	} else if( _stream_is_mode2_audio_interface_delay( delay_status ) &&
		delay_status->streak >= stream_mode2_dynamic_streak &&
		delay_status->effective_delay_ms > 0 ) {
		target = delay_status->effective_delay_ms - static_latency;
		target = _stream_clamp_ms( target, 0, stream_mode2_dynamic_max_ms );
		active = 1;
	}

	s->mode2_dynamic_correction_target_ms = target;

	if( s->mode2_dynamic_last_update_wall_ms == 0 ) {
		s->mode2_dynamic_last_update_wall_ms = wall_now;
	}
	if( wall_now - s->mode2_dynamic_last_update_wall_ms >= 200 ) {
		int delta = target - s->mode2_dynamic_correction_ms;
		int max_slew = stream_mode2_dynamic_slew_ms;
		if( max_slew < 1 ) {
			max_slew = 1;
		}
		delta = _stream_clamp_ms( delta, -max_slew, max_slew );
		s->mode2_dynamic_correction_ms += delta;
		s->mode2_dynamic_last_update_wall_ms = wall_now;
	}

	if( wall_now > s->mode2_dynamic_last_log_wall_ms + 2000 ||
		(active && s->mode2_dynamic_correction_ms != target) ) {
		s->mode2_dynamic_last_log_wall_ms = wall_now;
		DBG serprintf("mode2_dynamic_delay: enabled=%d active=%d source=%s tag=%s streak=%d static=%d measured=%d target=%d applied=%d max=%d slew=%d\n",
			stream_mode2_dynamic_delay, active,
			delay_status ? _stream_delay_source_name(delay_status->source) : "none",
			delay_status && delay_status->source_tag ? delay_status->source_tag : "none",
			delay_status ? delay_status->streak : 0, static_latency,
			delay_status ? delay_status->effective_delay_ms : 0,
			target, s->mode2_dynamic_correction_ms,
			stream_mode2_dynamic_max_ms, stream_mode2_dynamic_slew_ms);
	}

	return s->mode2_dynamic_correction_ms;
}

static int _stream_mode2_wall_heard_ts( STREAM *s, int wall_now, int heard_delay )
{
	int raw_heard_ts = s->audio_time - heard_delay;
	int reason = 0;
	const char *reason_name = "keep";

	if( s->mode2_clock_anchor_heard_ts == STREAM_NO_PTS_VALUE ||
		s->mode2_clock_anchor_audio == STREAM_NO_PTS_VALUE ) {
		reason = 1;
		reason_name = "init";
	} else if( s->mode2_clock_anchor_seek_epoch != s->seek_epoch ) {
		reason = 1;
		reason_name = "seek";
	} else if( s->audio_time < s->mode2_clock_anchor_audio ) {
		reason = 1;
		reason_name = "audio_regress";
	} else if( wall_now < s->mode2_clock_anchor_wall_ms ) {
		reason = 1;
		reason_name = "wall_regress";
	}

	if( reason ) {
		s->mode2_clock_anchor_heard_ts = raw_heard_ts;
		s->mode2_clock_anchor_audio = s->audio_time;
		s->mode2_clock_anchor_wall_ms = wall_now;
		s->mode2_clock_anchor_seek_epoch = s->seek_epoch;
		s->mode2_clock_anchor_latency_ms = heard_delay;
		s->mode2_clock_last_heard_ts = raw_heard_ts;
		s->mode2_clock_last_audio_time = s->audio_time;
		DBG serprintf("mode2_clock_anchor: reason=%s wall=%d audio=%d raw_heard=%d latency=%d seek_epoch=%d\n",
			reason_name, wall_now, s->audio_time, raw_heard_ts, heard_delay, s->seek_epoch);
		return raw_heard_ts;
	}

	int elapsed = wall_now - s->mode2_clock_anchor_wall_ms;
	if( elapsed < 0 ) {
		elapsed = 0;
	}

	int heard_ts = s->mode2_clock_anchor_heard_ts + elapsed;
	if( s->mode2_clock_last_heard_ts != STREAM_NO_PTS_VALUE &&
		heard_ts < s->mode2_clock_last_heard_ts ) {
		heard_ts = s->mode2_clock_last_heard_ts;
	}

	s->mode2_clock_last_heard_ts = heard_ts;
	s->mode2_clock_last_audio_time = s->audio_time;
	return heard_ts;
}

static int _stream_mode2_heard_delay( STREAM *s, int static_latency, int fallback_delay )
{
	(void)s;
	return static_latency > 0 ? static_latency : fallback_delay;
}

static int _stream_current_heard_delay( STREAM *s,
	const stream_delay_status_t *delay_status, int is_mode2_sync )
{
	if( !s || !delay_status ) {
		return 0;
	}
	if( is_mode2_sync ) {
		int static_latency = s->audio_ctx ? audio_interface_get_latency( s->audio_ctx ) : 0;
		return _stream_mode2_heard_delay( s, static_latency, delay_status->effective_delay_ms );
	}
	if( s->audio_ctx && audio_interface_is_startup_hold_active(s->audio_ctx) ) {
		// During startup hold, prioritize fresh static latency over potentially stale smoothed values.
		return delay_status->effective_delay_ms;
	}
	return (s->smoothed_av_delay >= 0) ?
		s->smoothed_av_delay + stream_get_atempo_delay( s ) :
		delay_status->effective_delay_ms;
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
	int delay_valid = delay_status.is_delay_valid;
	int passthrough_mode = s->audio_sink ? s->audio_sink->get_passthrough( s ) : 0;
	int is_mode2_sync = (passthrough_mode >= 2) || libavos_get_ac3_recoding_enabled();
	int use_mode2_wall_clock = passthrough_mode >= 2;
	int static_latency = s->audio_ctx ? audio_interface_get_latency( s->audio_ctx ) : 0;
	int heard_delay = _stream_current_heard_delay( s, &delay_status, is_mode2_sync );

	int wall_now = atime();

	if (!delay_valid && !is_mode2_sync) {
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

	if( use_mode2_wall_clock ) {
		heard_delay += _stream_mode2_dynamic_correction( s, wall_now, static_latency, &delay_status );
	}

	int heard_ts = s->audio_time - heard_delay;

	if( use_mode2_wall_clock && (s->audio_start_pending || s->audio_resume_pending) ) {
		_stream_reset_mode2_clock( s );
	} else if( use_mode2_wall_clock ) {
		heard_ts = _stream_mode2_wall_heard_ts( s, wall_now, heard_delay );
	}

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
		heard_ts < s->audio_time - STREAM_MODE1_STARTUP_CLAMP_MS && s->audio_time > s->video_time ) {
		int clamped = s->audio_time - STREAM_MODE1_STARTUP_CLAMP_MS;
		DBG serprintf( "stream_get_heard_audio_ts: mode1 startup hold clamp: %d->%d (audio=%d video=%d)\n",
			heard_ts, clamped, s->audio_time, s->video_time );
		heard_ts = clamped;
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
		DBG serprintf("heard_ts_diag: wall=%d audio=%d heard=%d h_delay=%d smoothed=%d eff=%d delay_valid=%d source=%s tag=%s\n",
			wall_now, s->audio_time, heard_ts, heard_delay, s->smoothed_av_delay, 
			delay_status.effective_delay_ms, delay_status.is_delay_valid,
			_stream_delay_source_name(delay_status.source),
			delay_status.source_tag ? delay_status.source_tag : "none");
		if( use_mode2_wall_clock ) {
			int user_av_delay = s->av_delay + stream_dbg_delay;
			int diff = STREAM_NO_PTS_VALUE;
			int raw_heard_ts = s->audio_time - heard_delay;
			int clock_elapsed = s->mode2_clock_anchor_wall_ms ?
				wall_now - s->mode2_clock_anchor_wall_ms : 0;
			if( s->sync_v_time != STREAM_NO_PTS_VALUE ) {
				diff = (s->sync_v_time - heard_ts) + RST_TO_TS_DELTA( user_av_delay, int );
			}
			DBG serprintf("mode2_timeline: wall=%d fmt=%04X passthrough=%d audio=%d heard=%d raw_heard=%d video=%d sync_v=%d diff=%d latency=%d dyn_corr=%d dyn_target=%d anchor_heard=%d anchor_audio=%d anchor_wall=%d anchor_elapsed=%d source=%s tag=%s\n",
				wall_now, s->audio ? s->audio->format : 0, passthrough_mode,
				s->audio_time, heard_ts, raw_heard_ts, s->video_time, s->sync_v_time,
				diff, heard_delay, s->mode2_dynamic_correction_ms,
				s->mode2_dynamic_correction_target_ms,
				s->mode2_clock_anchor_heard_ts, s->mode2_clock_anchor_audio, s->mode2_clock_anchor_wall_ms,
				clock_elapsed, _stream_delay_source_name(delay_status.source),
				delay_status.source_tag ? delay_status.source_tag : "none");
		}
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
	s->heard_interp_anchor_audio = -1;
	s->heard_interp_anchor_wall_ms = 0;
	s->heard_interp_last_ts = -1;
	_stream_reset_mode2_clock( s );

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
int stream_get_atempo_delay( STREAM *s )
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

static int _stream_pcm_delay_sensitive_phase( STREAM *s, int delay_valid )
{
	if( !s ) {
		return 0;
	}
	int startup_hold_active = s->audio_ctx ?
		audio_interface_is_startup_hold_active( s->audio_ctx ) : 0;
	return (startup_hold_active && !delay_valid) ||
		s->audio_start_pending ||
		s->audio_resume_pending ||
		(s->seek_epoch > 0 && !s->seek_converge_done);
}

static int _stream_pcm_should_update_delay_cache( int sensitive_phase, int delay_streak )
{
	return !sensitive_phase || delay_streak >= STREAM_PCM_DELAY_STABLE_STREAK;
}

static void _stream_pcm_update_delay_cache( STREAM *s, int current_av_delay,
	int delay_streak, int sensitive_phase )
{
	int old_last_good_delay = s->last_good_delay_ms;
	int old_last_good_atempo = s->last_good_atempo_delay_ms;
	int new_last_good_atempo = stream_get_atempo_delay( s );
	s->last_good_delay_ms = current_av_delay - new_last_good_atempo;
	s->last_good_delay_valid = 1;
	s->last_good_atempo_delay_ms = new_last_good_atempo;
	DBG serprintf( "stream_sync_audio: last_good_delay %d->%d last_good_atempo %d->%d speed=%.3f raw=%d streak=%d sensitive=%d hist=%d\n",
		old_last_good_delay, s->last_good_delay_ms,
		old_last_good_atempo, s->last_good_atempo_delay_ms,
		audio_interface_get_audio_speed(), current_av_delay, delay_streak,
		sensitive_phase, s->av_delay_history_count );
}

static void _stream_pcm_update_smoothed_hw_delay( STREAM *s, int current_av_delay,
	int current_atempo_delay, int delay_streak )
{
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
		audio_interface_get_audio_speed(), stream_get_atempo_delay( s ),
		delay_streak, s->av_delay_history_count );
}

static const char *_stream_seek_converge_state_name( int state )
{
	switch( state ) {
	case STREAM_SEEK_CONVERGE_ARMED:
		return "armed";
	case STREAM_SEEK_CONVERGE_WAITING_AUDIO:
		return "waiting_audio";
	case STREAM_SEEK_CONVERGE_WAITING_WINDOW:
		return "waiting_window";
	case STREAM_SEEK_CONVERGE_APPLIED:
		return "applied";
	case STREAM_SEEK_CONVERGE_EXPIRED:
		return "expired";
	case STREAM_SEEK_CONVERGE_INACTIVE:
	default:
		return "inactive";
	}
}

static void _stream_seek_converge_set_state( STREAM *s, int state, const char *reason )
{
	if( !s || s->seek_converge_state == state ) {
		return;
	}
	DBG serprintf("seek_converge: state=%s -> %s reason=%s epoch=%d until=%d done=%d anchor=%d audio=%d video=%d\n",
		_stream_seek_converge_state_name( s->seek_converge_state ),
		_stream_seek_converge_state_name( state ),
		reason ? reason : "none",
		s->seek_epoch, s->seek_converge_until_ms, s->seek_converge_done,
		s->seek_converge_anchor_ts, s->audio_time, s->video_time);
	s->seek_converge_state = state;
}

static int _stream_seek_converge_update( STREAM *s, int diff, int passthrough_mode )
{
	int now;

	if( !s || s->seek_epoch <= 0 || !s->put_time_mode || s->audio_time == -1 ) {
		return 0;
	}
	if( passthrough_mode ) {
		return 0;
	}
	if( s->seek_converge_epoch != s->seek_epoch ) {
		s->seek_converge_epoch = s->seek_epoch;
		s->seek_converge_until_ms = atime() + STREAM_SEEK_CONVERGE_WINDOW_MS;
		s->seek_converge_done = 0;
		s->seek_converge_anchor_ts = STREAM_NO_PTS_VALUE;
		_stream_seek_converge_set_state( s, STREAM_SEEK_CONVERGE_ARMED, "new_epoch" );
	}
	if( s->audio_start_pending || s->audio_resume_pending ) {
		_stream_seek_converge_set_state( s, STREAM_SEEK_CONVERGE_WAITING_AUDIO, "audio_pending" );
		return 0;
	}
	now = atime();
	if( s->seek_converge_state == STREAM_SEEK_CONVERGE_WAITING_AUDIO ) {
		s->seek_converge_until_ms = now + STREAM_SEEK_CONVERGE_WINDOW_MS;
		_stream_seek_converge_set_state( s, STREAM_SEEK_CONVERGE_WAITING_WINDOW, "audio_ready" );
		return 0;
	}
	if( s->seek_converge_done ) {
		if( s->seek_converge_state != STREAM_SEEK_CONVERGE_EXPIRED ) {
			_stream_seek_converge_set_state( s, STREAM_SEEK_CONVERGE_APPLIED, "already_done" );
		}
		return 1;
	}
	if( now < s->seek_converge_until_ms && ABS(diff) > STREAM_SEEK_CONVERGE_APPLY_DIFF_MS ) {
		_stream_seek_converge_set_state( s, STREAM_SEEK_CONVERGE_WAITING_WINDOW, "settle_window" );
		return 0;
	}
	if( now < s->seek_converge_until_ms ) {
DBGY		serprintf("seek_converge: early_apply diff=%d limit=%d epoch=%d audio=%d video=%d\n",
			diff, STREAM_SEEK_CONVERGE_APPLY_DIFF_MS,
			s->seek_epoch, s->audio_time, s->video_time);
	}
	if( ABS(diff) > STREAM_SEEK_CONVERGE_APPLY_DIFF_MS ) {
		if( now < s->seek_converge_until_ms + STREAM_SEEK_CONVERGE_MAX_WAIT_MS - STREAM_SEEK_CONVERGE_WINDOW_MS ) {
DBGY			serprintf("seek_converge: wait diff=%d limit=%d epoch=%d audio=%d video=%d\n",
				diff, STREAM_SEEK_CONVERGE_APPLY_DIFF_MS,
				s->seek_epoch, s->audio_time, s->video_time);
			_stream_seek_converge_set_state( s, STREAM_SEEK_CONVERGE_WAITING_WINDOW, "diff_unstable" );
			return 0;
		}
DBGY		serprintf("seek_converge: expire diff=%d limit=%d epoch=%d audio=%d video=%d\n",
			diff, STREAM_SEEK_CONVERGE_APPLY_DIFF_MS,
			s->seek_epoch, s->audio_time, s->video_time);
		s->seek_converge_done = 1;
		_stream_seek_converge_set_state( s, STREAM_SEEK_CONVERGE_EXPIRED, "diff_unstable" );
		return 1;
	}
	if( _stream_is_sink_driven(s) ) {
		int anchor_ts = stream_get_heard_audio_ts( s, s->audio_time );
DBGY		serprintf("post-seek converge anchor: diff=%d anchor_ts=%d\n",
			diff, anchor_ts);
		anchor_ts = _apply_user_av_delay_ts( s, anchor_ts );
		// Seek convergence is an explicit discontinuity; refresh once here,
		// not from steady-state mode-2 timing heuristics.
		sfdec2_refresh_sched_anchor( s );
		s->video_sink->put_time( s->video_sink, anchor_ts );
		s->sink_ref_time = anchor_ts;
		s->vid_ref_time = s->video_time;
		s->seek_converge_anchor_ts = anchor_ts;
	}
	s->seek_converge_done = 1;
	_stream_seek_converge_set_state( s, STREAM_SEEK_CONVERGE_APPLIED, "applied" );
	return 1;
}

int stream_sync_pcm_seek_converge_audio_gate( STREAM *s )
{
	int passthrough_mode;
	int heard_ts;
	int diff;

	if( !s || !s->put_time_mode || s->seek_epoch <= 0 || s->seek_converge_done ||
		s->audio_time == -1 || s->sync_v_time == -1 ) {
		return 0;
	}
	passthrough_mode = s->audio_sink ? s->audio_sink->get_passthrough( s ) : 0;
	if( passthrough_mode ) {
		return 0;
	}
	heard_ts = stream_get_heard_audio_ts( s, s->audio_time );
	if( heard_ts == -1 ) {
		return 0;
	}
	diff = _stream_av_diff( s, s->sync_v_time, heard_ts );
	if( diff < -STREAM_SEEK_CONVERGE_APPLY_DIFF_MS ) {
DBGY		serprintf("seek_converge: audio_gate diff=%d limit=%d epoch=%d audio=%d video=%d sync_v=%d heard=%d\n",
			diff, STREAM_SEEK_CONVERGE_APPLY_DIFF_MS, s->seek_epoch,
			s->audio_time, s->video_time, s->sync_v_time, heard_ts);
		return 1;
	}
	return 0;
}

int stream_sync_pcm_audio_lead_gate( STREAM *s, int ac3_recoding )
{
	int passthrough_mode;
	int heard_ts;
	int diff;

	if( !s || !s->put_time_mode || !s->audio_sink || ac3_recoding ||
		s->sync_v_time == STREAM_NO_PTS_VALUE || s->audio_time == -1 ) {
		if( s ) {
			s->pcm_audio_lead_hold_count = 0;
			s->pcm_audio_lead_last_diff = 0;
		}
		return 0;
	}
	passthrough_mode = s->audio_sink->get_passthrough ?
		s->audio_sink->get_passthrough( s ) : 0;
	if( passthrough_mode ) {
		s->pcm_audio_lead_hold_count = 0;
		s->pcm_audio_lead_last_diff = 0;
		return 0;
	}
	heard_ts = stream_get_heard_audio_ts( s, s->audio_time );
	if( heard_ts == STREAM_NO_PTS_VALUE ) {
		s->pcm_audio_lead_hold_count = 0;
		s->pcm_audio_lead_last_diff = 0;
		return 0;
	}

	diff = s->sync_v_time - heard_ts;
	s->pcm_audio_lead_last_diff = diff;
	if( diff >= -STREAM_PCM_AUDIO_LEAD_HOLD_THRESHOLD_MS ) {
		if( s->pcm_audio_lead_hold_count > 0 ) {
DBGY			serprintf("pcm_audio_lead_hold: release sync_v=%d audio=%d heard=%d diff=%d held=%d\n",
				s->sync_v_time, s->audio_time, heard_ts, diff,
				s->pcm_audio_lead_hold_count);
		}
		s->pcm_audio_lead_hold_count = 0;
		return 0;
	}

	s->pcm_audio_lead_hold_count++;
	if( (s->pcm_audio_lead_hold_count % STREAM_PCM_AUDIO_LEAD_HOLD_LOG_INTERVAL) == 1 ) {
		DBG serprintf("pcm_audio_lead_hold: sync_v=%d audio=%d heard=%d diff=%d count=%d threshold=%d\n",
			s->sync_v_time, s->audio_time, heard_ts, diff,
			s->pcm_audio_lead_hold_count,
			STREAM_PCM_AUDIO_LEAD_HOLD_THRESHOLD_MS);
	}
	return 1;
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
	int delay_valid = delay_status.is_delay_valid;
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
		int current_atempo_delay = stream_get_atempo_delay( s );
		int sensitive_phase = _stream_pcm_delay_sensitive_phase( s, delay_valid );
		int allow_update = _stream_pcm_should_update_delay_cache( sensitive_phase, delay_streak );

		if( !allow_update ) {
			DBG serprintf( "stream_sync_audio: defer last_good update (delay_streak=%d current=%d sensitive=%d)\n",
				delay_streak, current_av_delay, sensitive_phase );
		} else {
			_stream_pcm_update_delay_cache( s, current_av_delay, delay_streak, sensitive_phase );
		}
		_stream_pcm_update_smoothed_hw_delay( s, current_av_delay, current_atempo_delay, delay_streak );
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
			// Centralized heard_ts calculation.
			int anchor_ts = stream_get_heard_audio_ts( s, audio_time );

			int force_passthrough_reanchor =
				(passthrough_mode > 0) && 
				s->video_sink &&
				s->video_sink->put_time &&
				(s->sink_ref_time == -1 || s->sync_a_time == -1);

			if (diag_log) {
				DBGY2 serprintf("anchor_ts: audio_time=%d smoothed=%d current=%d source=%s tag=%s av_delay=%d anchor=%d\n",
					audio_time, s->smoothed_av_delay, current_av_delay,
					_stream_delay_source_name(delay_status.source),
					delay_status.source_tag ? delay_status.source_tag : "none",
					s->av_delay, anchor_ts);
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
		int passthrough_mode = s->audio_sink ? s->audio_sink->get_passthrough( s ) : 0;
		if( passthrough_mode >= 2 ) {
			if( s->audio_start_pending || s->audio_resume_pending ) {
				return 0;
			}
			// Mode 2 video scheduling uses the wall-clock playout estimate to
			// smooth compressed burst cadence. Gate audio writes against that
			// same heard timeline so the producer does not run from a competing
			// raw packet frontier.
			audio_time_for_diff = stream_get_heard_audio_ts( s, s->audio_time );
		} else {
			// PCM and mode 1 compare against the same heard time used for sink anchoring.
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
		int delay_valid = delay_status.is_delay_valid;

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
	if( _stream_seek_converge_update( s, diff, passthrough_mode ) ) {
		// After convergence, stop gating to avoid stutter.
		return 0;
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
DECLARE_DEBUG_PARAM("mode2_dyn_delay", stream_mode2_dynamic_delay);
DECLARE_DEBUG_PARAM("mode2_dyn_max", stream_mode2_dynamic_max_ms);
DECLARE_DEBUG_PARAM("mode2_dyn_slew", stream_mode2_dynamic_slew_ms);
DECLARE_DEBUG_PARAM("mode2_dyn_streak", stream_mode2_dynamic_streak);

static void _stream_toggle_xbmc( int argc, char *argv[] )
{
	stream_use_xbmc_smoothing = !stream_use_xbmc_smoothing;
	serprintf("stream_use_xbmc_smoothing: %d\n", stream_use_xbmc_smoothing );
}
DECLARE_DEBUG_COMMAND("sxbmc", _stream_toggle_xbmc );
#endif

#endif
