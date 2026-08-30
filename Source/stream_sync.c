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
#include "ac3_recode.h"
#include "stream.h"
#include "stream_sync.h"

#include <string.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

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
#define DBGA	DBG_IF(Debug[DBG_AUD])

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
static int sync_diag_count = 0;
static int sync_diag_last_seek_epoch = -1;
static int sync_diag_last_speed_x100 = -1;
static int sync_diag_last_pause_state = -1;
static int sync_diag_last_state = -1;
static int sync_diag_last_reanchor_pending = -1;

static int stream_use_xbmc_smoothing = 1;

void stream_sync_anchor_reset( STREAM *s )
{
	if( !s )
		return;
	pthread_mutex_lock( &s->anchor_mutex );
	__atomic_store_n( &s->sink_ref_time, -1, __ATOMIC_RELEASE );
	__atomic_store_n( &s->vid_ref_time, -1, __ATOMIC_RELEASE );
	pthread_mutex_unlock( &s->anchor_mutex );
}

void stream_sync_anchor_snapshot( STREAM *s, int *sink_ref_time, int *vid_ref_time )
{
	int sink_ref = -1;
	int vid_ref = -1;
	if( s ) {
		pthread_mutex_lock( &s->anchor_mutex );
		sink_ref = __atomic_load_n( &s->sink_ref_time, __ATOMIC_ACQUIRE );
		vid_ref = __atomic_load_n( &s->vid_ref_time, __ATOMIC_ACQUIRE );
		pthread_mutex_unlock( &s->anchor_mutex );
	}
	if( sink_ref_time )
		*sink_ref_time = sink_ref;
	if( vid_ref_time )
		*vid_ref_time = vid_ref;
}

int stream_sync_anchor_get_sink( STREAM *s )
{
	// sfdec2 can query heard time while holding its codec mutex. Keep this
	// validity read atomic so that path never reverses anchor_mutex -> sfdec2.
	return s ? __atomic_load_n( &s->sink_ref_time, __ATOMIC_ACQUIRE ) : -1;
}

int stream_sync_anchor_get_video( STREAM *s )
{
	int vid_ref_time;
	stream_sync_anchor_snapshot( s, NULL, &vid_ref_time );
	return vid_ref_time;
}

int stream_sync_anchor_publish( STREAM *s, int sink_ref_time, int vid_ref_time,
	int only_if_unset, int refresh_sink )
{
	if( !s )
		return 0;

	pthread_mutex_lock( &s->anchor_mutex );
	if( only_if_unset &&
		__atomic_load_n( &s->sink_ref_time, __ATOMIC_ACQUIRE ) != -1 ) {
		pthread_mutex_unlock( &s->anchor_mutex );
		return 0;
	}
	if( refresh_sink )
		sfdec2_refresh_sched_anchor( s );
	/*
	 * The audio decoder thread publishes this anchor while decoder recovery or
	 * resize can close the renderer on another thread.  Keep the method call
	 * under the close/delete lock so sink_close() cannot destroy the renderer's
	 * private mutex between this check and put_time().
	 */
	pthread_mutex_lock( &s->video_sink_mutex );
	if( s->video_sink && s->video_sink->is_open && s->video_sink->put_time )
		s->video_sink->put_time( s->video_sink, sink_ref_time );
	pthread_mutex_unlock( &s->video_sink_mutex );
	__atomic_store_n( &s->vid_ref_time, vid_ref_time, __ATOMIC_RELEASE );
	__atomic_store_n( &s->sink_ref_time, sink_ref_time, __ATOMIC_RELEASE );
	pthread_mutex_unlock( &s->anchor_mutex );
	return 1;
}

int stream_sync_anchor_seed_from_sink( STREAM *s, int vid_ref_time )
{
	if( !s )
		return 0;

	pthread_mutex_lock( &s->anchor_mutex );
	pthread_mutex_lock( &s->video_sink_mutex );
	if( __atomic_load_n( &s->sink_ref_time, __ATOMIC_ACQUIRE ) != -1 ) {
		pthread_mutex_unlock( &s->video_sink_mutex );
		pthread_mutex_unlock( &s->anchor_mutex );
		return 0;
	}
	if( !s->video_sink || !s->video_sink->is_open || !s->video_sink->get_time ) {
		pthread_mutex_unlock( &s->video_sink_mutex );
		pthread_mutex_unlock( &s->anchor_mutex );
		return 0;
	}
	int sink_time = s->video_sink->get_time( s->video_sink );
	pthread_mutex_unlock( &s->video_sink_mutex );
	__atomic_store_n( &s->vid_ref_time, vid_ref_time, __ATOMIC_RELEASE );
	__atomic_store_n( &s->sink_ref_time, sink_time - vid_ref_time, __ATOMIC_RELEASE );
	pthread_mutex_unlock( &s->anchor_mutex );
	return 1;
}

int stream_sync_anchor_adjust_sink( STREAM *s, int delta )
{
	int sink_ref_time = -1;
	if( !s )
		return sink_ref_time;
	pthread_mutex_lock( &s->anchor_mutex );
	sink_ref_time = __atomic_load_n( &s->sink_ref_time, __ATOMIC_ACQUIRE ) + delta;
	__atomic_store_n( &s->sink_ref_time, sink_ref_time, __ATOMIC_RELEASE );
	pthread_mutex_unlock( &s->anchor_mutex );
	return sink_ref_time;
}

#define STREAM_MODE1_STARTUP_CLAMP_MS        50
// Simple audio-lead gate threshold (Phase 1B baseline).
// Phase 4 hysteresis constants removed in Commit C.
#define STREAM_PCM_AUDIO_LEAD_GATE_MS        200
// Mode2 heard interpolator: max lead over the submitted frontier heard point.
// Between accepted write batches the buffer drains while playback continues, so
// the physical presentation position legitimately runs ahead of
// audio_time - selected_delay by up to one HAL batch quantum (~192-350ms on
// EAC3, ~660-700ms on low-bitrate AC3 2.0 routes, avos-443). The cap must
// exceed the largest batch quantum or the clock pins mid-interval; it only
// bounds true starvation, where the producer stops and the interpolator must
// not extrapolate past drained coverage.
#define STREAM_MODE2_HEARD_INTERP_MAX_LEAD_MS 800
#define STREAM_MODE2_DIRECT_RATE_STREAK        3
#define STREAM_MODE2_DIRECT_STABLE_STREAK      3
#define STREAM_MODE2_DIRECT_STABLE_BAND_MS   100
#define STREAM_MODE2_DIRECT_FRESH_MS          250
#define STREAM_MODE2_DIRECT_GRACE_MS          750
#define STREAM_MODE2_DIRECT_MAX_DELAY_MS     5000
static int stream_mode2_dynamic_all = 1;
#define STREAM_PCM_DELAY_DRIFT_CORRECT_MS    60
// Evidence stability filter: prevents AT burst/drain oscillation from overwriting last_good.
// delta <= COMMIT_DELTA: direct commit (normal slow drift).
// delta 12..60ms (medium zone): ignored — EAC3 burst/drain amplitude; not a valid new baseline.
// delta >= DRIFT_CORRECT_MS: candidate path — require CANDIDATE_COUNT samples within CANDIDATE_BAND.
#define STREAM_PCM_EVIDENCE_COMMIT_DELTA_MS   12
#define STREAM_PCM_EVIDENCE_CANDIDATE_BAND_MS  6
#define STREAM_PCM_EVIDENCE_CANDIDATE_COUNT    3
#define STREAM_PCM_STARTUP_PIPELINE_EXTRA_MAX_MS 500
#define STREAM_PCM_STARTUP_PIPELINE_MAX_MS      1000
#define STREAM_PCM_STARTUP_CORRECTION_MIN_MS      16
#define STREAM_PCM_STARTUP_CORRECTION_MAX_MS     500
#define STREAM_PCM_STARTUP_DIRECT_STREAK           10
typedef enum {
	STREAM_DELAY_SOURCE_NONE = 0,
	STREAM_DELAY_SOURCE_DYNAMIC,
	STREAM_DELAY_SOURCE_LAST_GOOD,
	STREAM_DELAY_SOURCE_STATIC,
	STREAM_DELAY_SOURCE_UNKNOWN,
} stream_delay_source_t;

typedef enum {
	PCM_REANCHOR_SOURCE_NONE = 0,
	PCM_REANCHOR_SOURCE_DYNAMIC,
	PCM_REANCHOR_SOURCE_LAST_GOOD,
	PCM_REANCHOR_SOURCE_STATIC,
} stream_pcm_reanchor_source_t;

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
	// Dynamic timing evidence preserved when last_good is selected for stability.
	// _stream_get_delay_status() may override source to LAST_GOOD while a valid
	// dynamic reading exists; these fields carry that reading to stream_sync_audio()
	// so it can keep last_good current without changing the selected effective delay.
	int has_dynamic_evidence;
	int dynamic_evidence_ms;
	int dynamic_evidence_streak;
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

static void _stream_pcm_delay_memory_reset( STREAM *s )
{
	if( !s ) {
		return;
	}
	s->delay_valid = 0;
	s->last_good_delay_ms = 0;
	s->last_good_delay_valid = 0;
	s->last_good_atempo_delay_ms = 0;
	s->last_good_candidate_ms = 0;
	s->last_good_candidate_count = 0;
	s->delay_history_count = 0;
	s->av_delay_history_count = 0;
	s->manual_audio_delay_target_ms = (s->av_delay < 0) ? -s->av_delay : 0;
	s->manual_audio_delay_applied_ms = 0;
	s->manual_audio_hold_pending_ms = 0;
	s->ac3_recode_next_write_wall_ms = 0;
	s->ac3_recode_pacer_valid = 0;
	s->ac3_recode_pacer_max_lead_ms = 0;
	s->at_speed_epoch_active = 0;
	s->atempo_ledger_active = 0;
	s->atempo_ledger_count = 0;
	s->atempo_ledger_write = 0;
	s->atempo_ledger_output_frames = 0;
	s->atempo_ledger_base_written_frames = 0;
	s->atempo_ledger_next_ts_us = 0;
	s->atempo_ledger_last_log_ms = 0;
	s->atempo_ledger_dense_until_ms = 0;
	s->atempo_ledger_next_rst_us = 0;
	s->atempo_ledger_media_cursor = 0;
	s->atempo_ledger_media_valid = 0;
	s->pcm_startup_seed_delay_ms = 0;
	s->pcm_startup_correction_pending = 0;
	s->pcm_startup_correction_seek_epoch = -1;
	s->pcm_startup_correction_speed_epoch = -1;
	// Pending commits reference the old ledger frame domain; on seek/flush the
	// boundaries AND their RST anchor source are invalid.  The target speeds are
	// already live in the filter, so the video-side commit must still happen or the
	// timelines run at different rates forever.  Zeroing the boundaries would drain
	// the whole queue at the next poll against the just-reset (empty) ledger and the
	// stale TS_TO_RST_TIME() map, anchoring video to a wildly wrong RST.  Instead,
	// collapse the queue to the single latest target speed and mark it DEFER, so the
	// poll applies it once the new ledger is active and resolves state==0 (a fresh,
	// correct RST anchor), not before.
	if( s->atempo_commit_count > 0 ) {
		int last = ( s->atempo_commit_head + s->atempo_commit_count - 1 ) % STREAM_ATEMPO_COMMIT_MAX;
		STREAM_ATEMPO_COMMIT collapsed = s->atempo_commit_q[last];
		collapsed.boundary = STREAM_ATEMPO_COMMIT_BOUNDARY_DEFER;
		collapsed.wall_ms = atime();
		s->atempo_commit_head = 0;
		s->atempo_commit_count = 1;
		s->atempo_commit_q[0] = collapsed;
	}
}

static void _stream_pcm_reanchor_reset( STREAM *s )
{
	if( !s ) {
		return;
	}
	s->audio_resume_valid_pending = 0;
	s->pcm_reanchor_state = STREAM_PCM_REANCHOR_INACTIVE;
	s->pcm_reanchor_seek_epoch = -1;
	s->pcm_reanchor_source = 0;
	s->pcm_reanchor_delay_ms = 0;
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

static const char *_stream_pcm_reanchor_state_name( int state )
{
	switch( state ) {
	case STREAM_PCM_REANCHOR_ARMED:
		return "armed";
	case STREAM_PCM_REANCHOR_APPLIED:
		return "applied";
	case STREAM_PCM_REANCHOR_EXPIRED:
		return "expired";
	case STREAM_PCM_REANCHOR_INACTIVE:
	default:
		return "inactive";
	}
}

static const char *_stream_pcm_reanchor_source_name( int source )
{
	switch( source ) {
	case PCM_REANCHOR_SOURCE_DYNAMIC:
		return "dynamic";
	case PCM_REANCHOR_SOURCE_LAST_GOOD:
		return "last_good";
	case PCM_REANCHOR_SOURCE_STATIC:
		return "static";
	case PCM_REANCHOR_SOURCE_NONE:
	default:
		return "none";
	}
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
	int sink_driven = 0;

	if( !s )
		return 0;

	/* The audio thread uses this predicate while codec recovery may delete the sink. */
	pthread_mutex_lock( &s->video_sink_mutex );
	sink_driven = s->video_sink && s->video_sink->is_open && s->video_sink->put_time;
	pthread_mutex_unlock( &s->video_sink_mutex );
	return sink_driven;
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
			"sync_a=%d sync_v=%d seek_epoch=%d\n",
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
			s->seek_epoch);
	}
	sync_diag_last_state = state;
	sync_diag_last_reanchor_pending = reanchor_pending;
}

// ************************************************************
//
//	stream_sync_restart
//
// ************************************************************
static void _stream_sync_mode2_heard_reset_locked( STREAM *s, int clear_frontier,
	int reset_compressed_ledger )
{
	s->mode2_heard_epoch++;
	if( reset_compressed_ledger ) {
		memset( &s->compressed_ledger, 0, sizeof(s->compressed_ledger) );
		s->compressed_ledger.epoch = s->mode2_heard_epoch;
	}
	memset( &s->presentation_observation, 0, sizeof(s->presentation_observation) );
	s->presentation_observation.epoch = s->mode2_heard_epoch;
	s->presentation_observation.state = STREAM_PRESENTATION_UNOBSERVED;
	s->mode2_shadow_last_log_ms = 0;
	s->mode2_heard_interp_valid = 0;
	s->mode2_heard_interp_ts = STREAM_NO_PTS_VALUE;
	s->mode2_heard_interp_wall_ms = 0;
	s->mode2_heard_interp_raw_ts = STREAM_NO_PTS_VALUE;
	s->mode2_heard_interp_delay_ms = -1;
	s->mode2_heard_interp_last_log_ms = 0;
	s->mode2_heard_prevideo_phase_active = 0;
	s->mode2_dynamic_clock_active = 0;
	s->mode2_dynamic_clock_ready = 0;
	s->mode2_dynamic_clock_ts = STREAM_NO_PTS_VALUE;
	s->mode2_dynamic_clock_wall_ms = 0;
	s->mode2_dynamic_clock_last_delay_ms = -1;
	s->mode2_dynamic_clock_grace_until_wall_ms = 0;
	s->mode2_dynamic_clock_last_log_ms = 0;
	if( clear_frontier ) {
		s->mode2_heard_frontier_seed_pending = 0;
	}
}

void stream_sync_mode2_heard_reset( STREAM *s, int clear_frontier )
{
	if( !s ) {
		return;
	}
	pthread_mutex_lock( &s->mode2_heard_mutex );
	_stream_sync_mode2_heard_reset_locked( s, clear_frontier, 1 );
	pthread_mutex_unlock( &s->mode2_heard_mutex );
}

void stream_sync_mode2_heard_frontier_arm( STREAM *s )
{
	if( !s ) {
		return;
	}
	pthread_mutex_lock( &s->mode2_heard_mutex );
	s->mode2_heard_frontier_seed_pending = 1;
	pthread_mutex_unlock( &s->mode2_heard_mutex );
}

int stream_sync_mode2_heard_frontier_pending( STREAM *s )
{
	int pending;
	if( !s ) {
		return 0;
	}
	pthread_mutex_lock( &s->mode2_heard_mutex );
	pending = s->mode2_heard_frontier_seed_pending;
	pthread_mutex_unlock( &s->mode2_heard_mutex );
	return pending;
}

void stream_sync_compressed_unit_commit( STREAM *s, int encoded_bytes,
	int logical_samples, int logical_sample_rate, int codec, int framing )
{
	STREAM_COMPRESSED_LEDGER *ledger;
	STREAM_COMPRESSED_LEDGER_ENTRY *entry;
	int index;

	if( !s || encoded_bytes <= 0 || logical_samples <= 0 || logical_sample_rate <= 0 ) {
		return;
	}

	pthread_mutex_lock( &s->mode2_heard_mutex );
	ledger = &s->compressed_ledger;
	if( ledger->count == STREAM_COMPRESSED_LEDGER_SIZE ) {
		entry = &ledger->entries[ledger->head];
		ledger->discarded_encoded_bytes += entry->encoded_bytes;
		ledger->discarded_logical_samples += entry->logical_samples;
		ledger->head = (ledger->head + 1) % STREAM_COMPRESSED_LEDGER_SIZE;
		ledger->count--;
	}
	index = (ledger->head + ledger->count) % STREAM_COMPRESSED_LEDGER_SIZE;
	entry = &ledger->entries[index];
	entry->epoch = ledger->epoch;
	entry->sequence = ledger->next_sequence++;
	entry->encoded_byte_start = ledger->total_encoded_bytes;
	entry->logical_sample_start = ledger->total_logical_samples;
	entry->encoded_bytes = encoded_bytes;
	entry->logical_samples = logical_samples;
	entry->logical_sample_rate = logical_sample_rate;
	entry->codec = codec;
	entry->framing = framing;
	entry->submitted_wall_ms = atime();
	ledger->total_encoded_bytes += encoded_bytes;
	ledger->total_logical_samples += logical_samples;
	ledger->count++;
	pthread_mutex_unlock( &s->mode2_heard_mutex );
}

// Map an encoded-byte position through complete ledger units. A position inside
// the current unit is interpolated only for shadow diagnostics; no partial unit
// is ever published to the production clock.
static int _stream_sync_ledger_bytes_to_samples_locked(
	const STREAM_COMPRESSED_LEDGER *ledger, UINT64 encoded_position,
	UINT64 *logical_position )
{
	UINT64 logical;
	int i;

	if( !ledger || !logical_position ||
		encoded_position < ledger->discarded_encoded_bytes ) {
		return 0;
	}
	logical = ledger->discarded_logical_samples;
	for( i = 0; i < ledger->count; i++ ) {
		const STREAM_COMPRESSED_LEDGER_ENTRY *entry =
			&ledger->entries[(ledger->head + i) % STREAM_COMPRESSED_LEDGER_SIZE];
		UINT64 entry_end = entry->encoded_byte_start + entry->encoded_bytes;
		if( encoded_position >= entry_end ) {
			logical = entry->logical_sample_start + entry->logical_samples;
			continue;
		}
		if( encoded_position > entry->encoded_byte_start ) {
			logical = entry->logical_sample_start +
				(UINT64)((double)(encoded_position - entry->encoded_byte_start) *
				(double)entry->logical_samples / (double)entry->encoded_bytes);
		}
		*logical_position = logical;
		return 1;
	}
	*logical_position = logical;
	return encoded_position <= ledger->total_encoded_bytes;
}

void stream_sync_compressed_shadow_observe( STREAM *s )
{
	AUDIO_PRESENTATION_SNAPSHOT sample;
	STREAM_COMPRESSED_LEDGER *ledger;
	STREAM_PRESENTATION_OBSERVATION *observation;
	UINT64 presented;
	INT64 direct_remaining;
	INT64 byte_remaining;
	INT64 frame_remaining;
	UINT64 byte_presented_samples;
	UINT64 frame_presented_samples;
	int direct_ms;
	int byte_ms;
	int frame_ms;
	int capacity_ms;
	int direct_capacity_delta;
	int byte_capacity_delta;
	int frame_capacity_delta;
	int direct_selected_delta;
	int byte_selected_delta;
	int frame_selected_delta;
	int candidate_residual_ms;
	int selected_heard;
	int direct_heard;
	int byte_heard;
	int frame_heard;
	int counter_advancing;
	int logical_rate;
	int now_ms;
	UINT64 old_generation;
	int old_underrun_count;
	int old_sample_wall_ms;
	int old_direct_delay_ms;
	int old_direct_stable_streak;
	int new_sample;
	int direct_plausible;
	int framing;
	INT64 timestamp_age_ns;
	struct timespec monotonic_now;

	if( !s || !s->audio_ctx ||
		!audio_interface_get_presentation_snapshot( s->audio_ctx, &sample ) ) {
		return;
	}
	now_ms = atime();
	if( sample.passthrough < 1 || sample.rate <= 0 ||
		now_ms - sample.observed_wall_ms > 500 ) {
		return;
	}

	pthread_mutex_lock( &s->mode2_heard_mutex );
	ledger = &s->compressed_ledger;
	observation = &s->presentation_observation;
	old_generation = observation->generation;
	old_underrun_count = observation->underrun_count;
	old_sample_wall_ms = observation->direct_last_sample_wall_ms;
	old_direct_delay_ms = observation->direct_delay_ms;
	old_direct_stable_streak = observation->direct_stable_streak;
	observation->epoch = s->mode2_heard_epoch;
	observation->generation = sample.generation;
	observation->state = sample.state;
	observation->timestamp_frames = sample.timestamp_frames;
	observation->timestamp_ns = sample.timestamp_ns;
	observation->playback_head_frames = sample.playback_head_frames;
	observation->source = sample.source;
	observation->rate = sample.rate;
	observation->frame_size = sample.frame_size;
	observation->buffer_size = sample.buffer_size;
	observation->format = sample.format;
	observation->logical_samples = sample.logical_samples;
	observation->encoded_bytes = sample.encoded_bytes;
	observation->latency_ms = sample.latency_ms;
	observation->fixed_latency_ms = sample.fixed_latency_ms;
	observation->underrun_count = sample.underrun_count;
	observation->observed_wall_ms = sample.observed_wall_ms;
	observation->last_advance_wall_ms = sample.last_advance_wall_ms;
	observation->direct_rate_hz = sample.direct_rate_hz;
	observation->direct_rate_streak = sample.direct_rate_streak;

	// A stream reset and AudioTrack reset can be observed on adjacent polls.
	// Do not compare counters until both submission domains describe the same
	// complete-unit frontier.
	if( sample.logical_samples > ledger->total_logical_samples ||
		sample.encoded_bytes > ledger->total_encoded_bytes ) {
		observation->state = STREAM_PRESENTATION_REJECTED;
		observation->direct_delay_ms = -1;
		observation->direct_stable_streak = 0;
		observation->direct_trusted = 0;
		pthread_mutex_unlock( &s->mode2_heard_mutex );
		return;
	}

	presented = sample.source == AT_PRESENTED_FRAMES_SRC_TIMESTAMP ?
		sample.timestamp_frames : sample.playback_head_frames;
	timestamp_age_ns = -1;
	if( sample.source == AT_PRESENTED_FRAMES_SRC_TIMESTAMP && sample.timestamp_ns > 0 &&
		clock_gettime( CLOCK_MONOTONIC, &monotonic_now ) == 0 ) {
		INT64 now_ns = (INT64)monotonic_now.tv_sec * 1000000000LL +
			(INT64)monotonic_now.tv_nsec;
		timestamp_age_ns = now_ns - sample.timestamp_ns;
		if( timestamp_age_ns >= 0 &&
			timestamp_age_ns <= STREAM_MODE2_DIRECT_FRESH_MS * 1000000LL ) {
			presented += (UINT64)(timestamp_age_ns * sample.rate / 1000000000LL);
		}
	}
	counter_advancing = sample.state == AT_PRESENTATION_ADVANCING && presented > 0;
	logical_rate = ledger->count > 0 ?
		(int)ledger->entries[(ledger->head + ledger->count - 1) %
			STREAM_COMPRESSED_LEDGER_SIZE].logical_sample_rate : sample.rate;
	framing = ledger->count > 0 ?
		ledger->entries[(ledger->head + ledger->count - 1) %
			STREAM_COMPRESSED_LEDGER_SIZE].framing : STREAM_COMPRESSED_FRAMING_UNKNOWN;
	direct_ms = -1;
	byte_ms = -1;
	frame_ms = -1;
	if( counter_advancing ) {
		direct_remaining = (INT64)ledger->total_logical_samples -
			(INT64)((double)presented * (double)logical_rate / (double)sample.rate);
		byte_remaining = -1;
		frame_remaining = -1;
		if( _stream_sync_ledger_bytes_to_samples_locked( ledger, presented,
			&byte_presented_samples ) ) {
			byte_remaining = (INT64)ledger->total_logical_samples -
				(INT64)byte_presented_samples;
		}
		if( _stream_sync_ledger_bytes_to_samples_locked( ledger,
			presented * (UINT64)MAX(sample.frame_size, 1),
			&frame_presented_samples ) ) {
			frame_remaining = (INT64)ledger->total_logical_samples -
				(INT64)frame_presented_samples;
		}
		direct_ms = (int)(direct_remaining * 1000 / logical_rate);
		byte_ms = byte_remaining >= 0 ?
			(int)(byte_remaining * 1000 / logical_rate) : -1;
		frame_ms = frame_remaining >= 0 ?
			(int)(frame_remaining * 1000 / logical_rate) : -1;
	}
	capacity_ms = -1;
	if( sample.buffer_size > 0 && sample.encoded_bytes > 0 ) {
		capacity_ms = (int)((double)sample.buffer_size *
			(double)sample.logical_samples * 1000.0 /
			((double)sample.encoded_bytes * (double)logical_rate));
	}
	direct_capacity_delta = direct_ms >= 0 && capacity_ms >= 0 ?
		direct_ms - capacity_ms : -1;
	byte_capacity_delta = byte_ms >= 0 && capacity_ms >= 0 ?
		byte_ms - capacity_ms : -1;
	frame_capacity_delta = frame_ms >= 0 && capacity_ms >= 0 ?
		frame_ms - capacity_ms : -1;
	direct_selected_delta = direct_ms >= 0 ? direct_ms - sample.latency_ms : -1;
	byte_selected_delta = byte_ms >= 0 ? byte_ms - sample.latency_ms : -1;
	frame_selected_delta = frame_ms >= 0 ? frame_ms - sample.latency_ms : -1;
	// AudioTimestamp is the platform presentation position, so submitted minus
	// timestamp already contains Android-visible queue and output-pipeline delay.
	// Playback head stops earlier and still needs the static residual fallback.
	candidate_residual_ms = sample.source == AT_PRESENTED_FRAMES_SRC_TIMESTAMP ?
		0 : sample.fixed_latency_ms;
	selected_heard = s->audio_time - sample.latency_ms;
	direct_heard = direct_ms >= 0 ?
		s->audio_time - direct_ms - candidate_residual_ms : STREAM_NO_PTS_VALUE;
	byte_heard = byte_ms >= 0 ?
		s->audio_time - byte_ms - candidate_residual_ms : STREAM_NO_PTS_VALUE;
	frame_heard = frame_ms >= 0 ?
		s->audio_time - frame_ms - candidate_residual_ms : STREAM_NO_PTS_VALUE;

	new_sample = sample.observed_wall_ms != old_sample_wall_ms;
	direct_plausible = sample.source == AT_PRESENTED_FRAMES_SRC_TIMESTAMP &&
		sample.state == AT_PRESENTATION_ADVANCING &&
		sample.direct_rate_streak >= STREAM_MODE2_DIRECT_RATE_STREAK &&
		sample.rate == logical_rate && timestamp_age_ns >= 0 &&
		timestamp_age_ns <= STREAM_MODE2_DIRECT_FRESH_MS * 1000000LL &&
		direct_ms >= 0 && direct_ms <= STREAM_MODE2_DIRECT_MAX_DELAY_MS &&
		presented <= ledger->total_logical_samples &&
		!(old_generation == sample.generation && old_underrun_count >= 0 &&
			sample.underrun_count > old_underrun_count);
	if( new_sample ) {
		if( direct_plausible ) {
			if( old_generation == sample.generation && old_direct_delay_ms >= 0 &&
				abs(direct_ms - old_direct_delay_ms) <= STREAM_MODE2_DIRECT_STABLE_BAND_MS ) {
				observation->direct_stable_streak = old_direct_stable_streak + 1;
			} else {
				observation->direct_stable_streak = 1;
			}
		} else {
			observation->direct_stable_streak = 0;
		}
		observation->direct_last_sample_wall_ms = sample.observed_wall_ms;
	}
	observation->direct_delay_ms = direct_plausible ? direct_ms : -1;
	observation->direct_heard_ts = direct_plausible ?
		direct_heard : STREAM_NO_PTS_VALUE;
	observation->direct_heard_wall_ms = direct_plausible ? now_ms : 0;
	observation->direct_trusted = direct_plausible &&
		observation->direct_stable_streak >= STREAM_MODE2_DIRECT_STABLE_STREAK;

	if( now_ms - s->mode2_shadow_last_log_ms >= 500 ) {
		const char *tag = sample.passthrough == 1 ?
			"mode1_iec_occupancy_shadow" : "mode2_occupancy_shadow";
		s->mode2_shadow_last_log_ms = now_ms;
		DBG serprintf("%s: epoch=%llu generation=%llu pt=%d framing=%d state=%d src=%d age=%d ts_age=%d advance_age=%d counter_advancing=%d rate_hz=%d rate_streak=%d stable_streak=%d trusted=%d fmt=%04X track_rate=%d logical_rate=%d frame_size=%d buffer=%d ledger_count=%d logical=%llu encoded=%llu presented=%llu direct_ms=%d byte_ms=%d frame_ms=%d capacity_ms=%d direct_minus_capacity=%d byte_minus_capacity=%d frame_minus_capacity=%d direct_minus_selected=%d byte_minus_selected=%d frame_minus_selected=%d static_residual_ms=%d candidate_residual_ms=%d selected_latency=%d selected_heard=%d direct_heard=%d byte_heard=%d frame_heard=%d underruns=%d\n",
			tag,
			(unsigned long long)s->mode2_heard_epoch,
			(unsigned long long)sample.generation,
			sample.passthrough, framing, observation->state, sample.source,
			now_ms - sample.observed_wall_ms,
			timestamp_age_ns >= 0 ? (int)(timestamp_age_ns / 1000000LL) : -1,
			sample.last_advance_wall_ms > 0 ? now_ms - sample.last_advance_wall_ms : -1,
			counter_advancing, sample.direct_rate_hz, sample.direct_rate_streak,
			observation->direct_stable_streak, observation->direct_trusted,
			sample.format, sample.rate, logical_rate, sample.frame_size,
			sample.buffer_size, ledger->count,
			(unsigned long long)ledger->total_logical_samples,
			(unsigned long long)ledger->total_encoded_bytes,
			(unsigned long long)presented,
			direct_ms, byte_ms, frame_ms, capacity_ms,
			direct_capacity_delta, byte_capacity_delta, frame_capacity_delta,
			direct_selected_delta, byte_selected_delta, frame_selected_delta,
			sample.fixed_latency_ms, candidate_residual_ms,
			sample.latency_ms, selected_heard,
			direct_heard, byte_heard, frame_heard,
			sample.underrun_count);
	}
	pthread_mutex_unlock( &s->mode2_heard_mutex );
}

int stream_sync_mode2_dynamic_active( STREAM *s )
{
	int active;
	if( !s ) {
		return 0;
	}
	pthread_mutex_lock( &s->mode2_heard_mutex );
	// Do not expose a newly adopted presentation clock to the renderer while
	// its monotonic phase is holding for a lower measured frontier. Retargeting
	// MediaCodec during that hold stacks a moving render-offset correction on
	// top of the heard-clock convergence and prolongs startup freezes.
	active = s->mode2_dynamic_clock_active && s->mode2_dynamic_clock_ready;
	pthread_mutex_unlock( &s->mode2_heard_mutex );
	return active;
}

// Caller owns anchor_mutex followed by mode2_heard_mutex so the renderer anchor
// and Mode 2 heard-clock epoch reset as one transaction.
static int _stream_sync_restart_locked( STREAM *s, int reset_compressed_ledger )
{
	s->delay         = 0;
	_stream_pcm_delay_memory_reset( s );
	_stream_pcm_reanchor_reset( s );
	s->drop          = 0;
	s->drop_P        = 0;
	s->drop_B        = 0;
	
	__atomic_store_n( &s->sink_ref_time, -1, __ATOMIC_RELEASE );
	__atomic_store_n( &s->vid_ref_time, -1, __ATOMIC_RELEASE );
	s->sync_v_time = -1;
	s->sync_a_time = -1;
	_stream_sync_mode2_heard_reset_locked( s, 0, reset_compressed_ledger );

	_sync_diag_reset();

	return 0;
}

int stream_sync_restart( STREAM *s )
{
	int ret;
	pthread_mutex_lock( &s->anchor_mutex );
	pthread_mutex_lock( &s->mode2_heard_mutex );
	ret = _stream_sync_restart_locked( s, 1 );
	pthread_mutex_unlock( &s->mode2_heard_mutex );
	pthread_mutex_unlock( &s->anchor_mutex );
	return ret;
}

int stream_sync_restart_with_mode2_frontier( STREAM *s )
{
	int ret;
	pthread_mutex_lock( &s->anchor_mutex );
	pthread_mutex_lock( &s->mode2_heard_mutex );
	ret = _stream_sync_restart_locked( s, 1 );
	s->mode2_heard_frontier_seed_pending = 1;
	pthread_mutex_unlock( &s->mode2_heard_mutex );
	pthread_mutex_unlock( &s->anchor_mutex );
	return ret;
}

// Pause keeps the compressed AudioTrack and its buffered media intact. Preserve
// the interpolated heard phase, but move its wall epoch to now so paused wall
// time is not credited as audio progress. Seeks and sink recreation continue to
// use stream_sync_restart(), which deliberately starts a new Mode 2 epoch.
int stream_sync_restart_after_pause( STREAM *s )
{
	int passthrough_mode = (s->audio_sink && s->audio_sink->get_passthrough) ?
		s->audio_sink->get_passthrough( s ) : 0;
	int keep_mode2_phase;
	int keep_dynamic_phase;
	int interp_ts;
	int interp_raw_ts;
	int interp_delay_ms;
	int interp_last_log_ms;
	int dynamic_active;
	int dynamic_ready;
	int dynamic_ts;
	int dynamic_last_delay_ms;
	int dynamic_last_log_ms;

	pthread_mutex_lock( &s->anchor_mutex );
	pthread_mutex_lock( &s->mode2_heard_mutex );
	keep_mode2_phase = passthrough_mode >= 2 &&
		!libavos_get_ac3_recoding_enabled() && s->mode2_heard_interp_valid;
	keep_dynamic_phase = passthrough_mode >= 2 &&
		s->mode2_dynamic_clock_active;
	interp_ts = s->mode2_heard_interp_ts;
	interp_raw_ts = s->mode2_heard_interp_raw_ts;
	interp_delay_ms = s->mode2_heard_interp_delay_ms;
	interp_last_log_ms = s->mode2_heard_interp_last_log_ms;
	dynamic_active = s->mode2_dynamic_clock_active;
	dynamic_ready = s->mode2_dynamic_clock_ready;
	dynamic_ts = s->mode2_dynamic_clock_ts;
	dynamic_last_delay_ms = s->mode2_dynamic_clock_last_delay_ms;
	dynamic_last_log_ms = s->mode2_dynamic_clock_last_log_ms;

	// AudioTrack.pause() preserves compressed queue occupancy. Start a new
	// presentation-observation epoch, but retain the submitted-unit ledger so
	// the next observer can remap the still-buffered media after resume.
	_stream_sync_restart_locked( s, 0 );
	if( keep_mode2_phase ) {
		s->mode2_heard_interp_valid = 1;
		s->mode2_heard_interp_ts = interp_ts;
		s->mode2_heard_interp_wall_ms = atime();
		s->mode2_heard_interp_raw_ts = interp_raw_ts;
		s->mode2_heard_interp_delay_ms = interp_delay_ms;
		s->mode2_heard_interp_last_log_ms = interp_last_log_ms;
		s->mode2_heard_prevideo_phase_active = 1;
	}
	if( keep_dynamic_phase ) {
		s->mode2_dynamic_clock_active = 1;
		s->mode2_dynamic_clock_ready = dynamic_ready;
		s->mode2_dynamic_clock_ts = dynamic_ts;
		s->mode2_dynamic_clock_wall_ms = atime();
		s->mode2_dynamic_clock_last_delay_ms = dynamic_last_delay_ms;
		s->mode2_dynamic_clock_grace_until_wall_ms = atime() +
			STREAM_MODE2_DIRECT_GRACE_MS;
		s->mode2_dynamic_clock_last_log_ms = dynamic_last_log_ms;
		s->mode2_heard_prevideo_phase_active = 1;
	}
	if( keep_mode2_phase || keep_dynamic_phase ) {
		DBG serprintf("mode2_pause_phase_restore: interp=%d raw=%d delay=%d dynamic=%d ready=%d dynamic_ts=%d audio=%d video=%d recode=%d\n",
			interp_ts, interp_raw_ts, interp_delay_ms, dynamic_active, dynamic_ready,
			dynamic_ts, s->audio_time, s->video_time,
			libavos_get_ac3_recoding_enabled());
	}
	pthread_mutex_unlock( &s->mode2_heard_mutex );
	pthread_mutex_unlock( &s->anchor_mutex );

	return 0;
}

// Returns 1 if a tag from audio_interface_get_delay_source() represents a
// dynamic-derived measurement that can be used to refresh last_good.
// Only "dynamic*" (direct HW measurement) and exactly "cached(throttle)"
// (throttle-stabilised form of dynamic) qualify.  "playhead*" captures
// instantaneous AT queue troughs during burst/drain cycles and must NOT be
// used; "last_good(throttle)" is circular (last_good refreshing last_good).
static int _stream_is_dynamic_evidence_tag(const char *tag)
{
	if( !tag || !tag[0] )
		return 0;
	if( !strncmp( tag, "dynamic", 7 ) )
		return 1;
	if( !strcmp( tag, "cached(throttle)" ) )
		return 1;
	return 0;
}

int stream_get_pcm_startup_seed_delay_ms( STREAM *s )
{
#ifdef CONFIG_ANDROID
	if( !s || !s->audio_ctx ) {
		return 0;
	}
	int app_latency = audio_interface_get_latency( s->audio_ctx );
	int pipeline_latency = audio_interface_get_pipeline_latency( s->audio_ctx );
	if( app_latency < 0 ) {
		app_latency = 0;
	}
	if( pipeline_latency < app_latency ) {
		pipeline_latency = app_latency;
	}
	// Some HDMI HALs publish implausibly large route latency. This estimate is
	// only a cold PCM seed, so bound it relative to the known local buffer and
	// let direct AudioTimestamp evidence own the subsequent correction.
	int max_pipeline = app_latency + STREAM_PCM_STARTUP_PIPELINE_EXTRA_MAX_MS;
	if( max_pipeline > STREAM_PCM_STARTUP_PIPELINE_MAX_MS ) {
		max_pipeline = STREAM_PCM_STARTUP_PIPELINE_MAX_MS;
	}
	if( pipeline_latency > max_pipeline ) {
		pipeline_latency = max_pipeline;
	}
	return pipeline_latency;
#else
	(void)s;
	return 0;
#endif
}

// ************************************************************
//
//	stream_get_heard_audio_ts
//
// ************************************************************
static stream_delay_status_t _stream_get_delay_status(STREAM *s, int allow_static)
{
	stream_delay_status_t status = { 0 };
	int passthrough_mode = s && s->audio_sink ? s->audio_sink->get_passthrough( s ) : 0;
	int ac3_recoding = 0;
#ifdef CONFIG_AUDIO_AC3
	ac3_recoding = libavos_get_ac3_recoding_enabled();
#endif

#ifdef CONFIG_ANDROID
	if( s && s->audio_ctx ) {
		int delay_valid;
		int measured_delay;
		const char *source_tag;

		// Refresh AudioTrack timing evidence once, then select one delay.
		// Phase 2 allows dynamic delay for PCM only.  Passthrough/mode2 and
		// AC3 recoding stay on static timing until their dedicated phases.
		measured_delay = audio_interface_get_delay( s->audio_ctx );
		delay_valid = audio_interface_is_delay_valid( s->audio_ctx );
		source_tag = audio_interface_get_delay_source( s->audio_ctx );

		status.source_tag = source_tag;
		status.source = _classify_audio_delay_source( source_tag, delay_valid );
		status.streak = audio_interface_get_delay_valid_streak( s->audio_ctx );

		// Capture dynamic timing evidence before any stability-based override.
		// Covers "dynamic*" (direct HW) and exactly "cached(throttle)" (throttle-
		// stabilised form of dynamic). Both map to LAST_GOOD after classification,
		// so the DYNAMIC-only path below never sees them. Evidence is the full AV
		// delay so atempo/filter/sink accounting stays consistent with the normal
		// last_good update path.
		if( !passthrough_mode && !ac3_recoding && delay_valid && measured_delay > 0 &&
			_stream_is_dynamic_evidence_tag( source_tag ) &&
			status.streak >= STREAM_PCM_DELAY_STABLE_STREAK ) {
			status.has_dynamic_evidence = 1;
			status.dynamic_evidence_ms = stream_sync_av_delay( s );
			status.dynamic_evidence_streak = status.streak;
		}

		if( !passthrough_mode && !ac3_recoding && delay_valid &&
			status.source == STREAM_DELAY_SOURCE_DYNAMIC &&
			measured_delay > 0 ) {
			int dynamic_delay = stream_sync_av_delay( s );
			if( status.streak >= STREAM_PCM_DELAY_STABLE_STREAK ) {
				if( !s->last_good_delay_valid ) {
					status.effective_delay_ms = dynamic_delay;
					status.is_anchorable = 1;
					status.is_delay_valid = 1;
					return status;
				}

				int last_good_delay = s->last_good_delay_ms + stream_get_atempo_delay( s );
				int drift = dynamic_delay - last_good_delay;
				if( ABS( drift ) >= STREAM_PCM_DELAY_DRIFT_CORRECT_MS ) {
					DBG serprintf("pcm_delay_select: large stable drift dynamic=%d last_good=%d drift=%d streak=%d threshold=%d\n",
						dynamic_delay, last_good_delay, drift, status.streak,
						STREAM_PCM_DELAY_DRIFT_CORRECT_MS);
					status.effective_delay_ms = dynamic_delay;
					status.is_anchorable = 1;
					status.is_delay_valid = 1;
					return status;
				}

				status.effective_delay_ms = last_good_delay;
				status.is_anchorable = 1;
				status.is_fallback = 1;
				status.source = STREAM_DELAY_SOURCE_LAST_GOOD;
				status.source_tag = "last_good(stable)";
				// has_dynamic_evidence was already populated by the up-front
				// capture above; no need to repeat it here.
				return status;
			}
		}

		if( !passthrough_mode && !ac3_recoding && s->last_good_delay_valid ) {
			status.effective_delay_ms = s->last_good_delay_ms + stream_get_atempo_delay( s );
			status.is_anchorable = 1;
			status.is_fallback = 1;
			status.source = STREAM_DELAY_SOURCE_LAST_GOOD;
			status.source_tag = "last_good(stream)";
			return status;
		}
	}

	if( s && allow_static && s->audio_ctx ) {
		int static_latency = audio_interface_get_latency(s->audio_ctx);
		if( !passthrough_mode && !ac3_recoding &&
			audio_interface_is_startup_hold_active(s->audio_ctx) ) {
			int startup_seed = stream_get_pcm_startup_seed_delay_ms( s );
			if( startup_seed > static_latency ) {
				static_latency = startup_seed;
			}
		}
		if( static_latency > 0 ) {
			status.effective_delay_ms = static_latency;
			status.is_anchorable = 1;
			status.is_delay_valid = 1;
			status.is_fallback = 1;
			status.source = STREAM_DELAY_SOURCE_STATIC;
			status.source_tag = "static(phase1)";
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

static int _stream_mode2_heard_delay( STREAM *s, int static_latency, int fallback_delay )
{
	(void)s;
	return static_latency > 0 ? static_latency : fallback_delay;
}

static int _stream_ac3_recode_pacer_lead_ms( STREAM *s )
{
	if( !s || !s->ac3_recode_pacer_valid || s->ac3_recode_pacer_max_lead_ms <= 0 ) {
		return 0;
	}
	int lead_ms = s->ac3_recode_next_write_wall_ms - atime();
	if( lead_ms < 0 ) {
		return 0;
	}
	if( lead_ms > s->ac3_recode_pacer_max_lead_ms ) {
		lead_ms = s->ac3_recode_pacer_max_lead_ms;
	}
	return lead_ms;
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
	return delay_status->effective_delay_ms;
}

// Map an output-frame playhead position to the media TS of the sample at that
// position using the atempo output ledger.
// state: 0 = inside a ledger block, -1 = before oldest block (stale/startup),
// +1 = past newest block (extrapolated at the newest block rate).
typedef struct {
	int	heard;		// STREAM_NO_PTS_VALUE if not resolvable
	int	heard_rst;	// media/RST interpolation (STREAM_NO_PTS_VALUE if invalid)
	int	state;
	UINT64	block_start;
	int	block_ts;
	int	block_nframes;
} ATEMPO_LEDGER_LOOKUP;

static ATEMPO_LEDGER_LOOKUP _stream_atempo_ledger_lookup( STREAM *s, UINT64 playhead, int playhead_rate )
{
	ATEMPO_LEDGER_LOOKUP r = { STREAM_NO_PTS_VALUE, STREAM_NO_PTS_VALUE, 0, 0, 0, 0 };
	int oldest = (s->atempo_ledger_write - s->atempo_ledger_count + STREAM_ATEMPO_LEDGER_SIZE) %
		STREAM_ATEMPO_LEDGER_SIZE;
	int newest = (s->atempo_ledger_write - 1 + STREAM_ATEMPO_LEDGER_SIZE) %
		STREAM_ATEMPO_LEDGER_SIZE;
	for( int i = 0; i < s->atempo_ledger_count; ++i ) {
		int idx = (oldest + i) % STREAM_ATEMPO_LEDGER_SIZE;
		STREAM_ATEMPO_LEDGER_ENTRY *entry = &s->atempo_ledger[idx];
		UINT64 block_end = entry->output_frames_start + (UINT64)entry->block_nframes;
		if( playhead >= entry->output_frames_start && playhead < block_end ) {
			UINT64 delta_frames = playhead - entry->output_frames_start;
			int rate = entry->rate > 0 ? entry->rate : playhead_rate;
			if( entry->block_is_hold ) {
				// Output-only hold, used for negative manual A/V delay.  The
				// playhead crosses inserted silence, but heard media time must
				// stay frozen at the block's start TS.
				r.heard = entry->block_ts_start;
			} else {
				r.heard = entry->block_ts_start + (int)((delta_frames * 1000) / (UINT64)rate);
			}
			// Media/RST interpolation: prorate the block's media span across its
			// output frames (RST slope differs from the 1:1 TS slope by tempo).
			if( entry->block_nframes > 0 ) {
				int64_t rst_us = entry->block_rst_start_us +
					((int64_t)delta_frames * entry->block_rst_span_us) / entry->block_nframes;
				r.heard_rst = (int)(rst_us / 1000);
			} else {
				r.heard_rst = (int)(entry->block_rst_start_us / 1000);
			}
			r.block_start = entry->output_frames_start;
			r.block_ts = entry->block_ts_start;
			r.block_nframes = entry->block_nframes;
			r.state = 0;
			return r;
		}
	}
	STREAM_ATEMPO_LEDGER_ENTRY *oldest_entry = &s->atempo_ledger[oldest];
	STREAM_ATEMPO_LEDGER_ENTRY *newest_entry = &s->atempo_ledger[newest];
	UINT64 newest_end = newest_entry->output_frames_start + (UINT64)newest_entry->block_nframes;
	if( playhead < oldest_entry->output_frames_start ) {
		r.heard = oldest_entry->block_ts_start;
		r.heard_rst = (int)(oldest_entry->block_rst_start_us / 1000);
		r.block_start = oldest_entry->output_frames_start;
		r.block_ts = oldest_entry->block_ts_start;
		r.block_nframes = oldest_entry->block_nframes;
		r.state = -1;
	} else if( playhead >= newest_end ) {
		int rate = newest_entry->rate > 0 ? newest_entry->rate : playhead_rate;
		int block_end_ts = newest_entry->block_ts_start +
			(int)(((UINT64)newest_entry->block_nframes * 1000) / (UINT64)rate);
		UINT64 delta_after = playhead - newest_end;
		r.heard = block_end_ts + (int)((delta_after * 1000) / (UINT64)rate);
		// Extend the media/RST clock at the newest block's RST slope.
		int64_t rst_us = newest_entry->block_rst_start_us + newest_entry->block_rst_span_us;
		if( newest_entry->block_nframes > 0 ) {
			rst_us += ((int64_t)delta_after * newest_entry->block_rst_span_us) /
				newest_entry->block_nframes;
		}
		r.heard_rst = (int)(rst_us / 1000);
		r.block_start = newest_entry->output_frames_start;
		r.block_ts = newest_entry->block_ts_start;
		r.block_nframes = newest_entry->block_nframes;
		r.state = 1;
	}
	return r;
}

// Public accessor for the ledger's media/RST interpolation at a given output-frame
// playhead.  Returns the heard media (RST) ms, or STREAM_NO_PTS_VALUE if the ledger
// is inactive/unresolvable.  *state (optional) gets the lookup state (-1 stale /
// 0 inside / +1 extrapolated).
int stream_atempo_ledger_lookup_rst( STREAM *s, UINT64 playhead, int playhead_rate, int *state )
{
	if( !s || !s->atempo_ledger_active || s->atempo_ledger_count <= 0 ) {
		if( state ) {
			*state = 0;
		}
		return STREAM_NO_PTS_VALUE;
	}
	ATEMPO_LEDGER_LOOKUP r = _stream_atempo_ledger_lookup( s, playhead, playhead_rate );
	if( state ) {
		*state = r.state;
	}
	return r.heard_rst;
}

// Caller holds mode2_heard_mutex. The static clock remains live as fallback;
// trusted AudioTimestamp evidence replaces it only after proving a stable
// submitted-minus-presented frontier. For AC3 recode, that frontier already
// includes the bursts admitted by the wall-clock pacer, so pacer lead must not
// be subtracted from the dynamic target a second time.
static int _stream_apply_mode2_dynamic_clock_locked( STREAM *s, int wall_now,
	int static_heard_ts, int ac3_recoding )
{
	STREAM_PRESENTATION_OBSERVATION *observation = &s->presentation_observation;
	int validated_profile = observation->format == WAVE_FORMAT_AC3 &&
		observation->rate == 44100;
	int recode_profile = observation->format == WAVE_FORMAT_AC3 &&
		observation->rate == AC3_RECODE_SAMPLE_RATE;
	int profile_enabled = ac3_recoding ? recode_profile :
		(validated_profile || stream_mode2_dynamic_all);
	int direct_valid = observation->epoch == s->mode2_heard_epoch &&
		observation->direct_trusted &&
		profile_enabled &&
		observation->direct_delay_ms >= 0 &&
		observation->direct_heard_ts != STREAM_NO_PTS_VALUE &&
		wall_now - observation->direct_heard_wall_ms >= 0 &&
		wall_now - observation->direct_heard_wall_ms <= STREAM_MODE2_DIRECT_FRESH_MS &&
		wall_now - observation->observed_wall_ms >= 0 &&
		wall_now - observation->observed_wall_ms <= STREAM_MODE2_DIRECT_FRESH_MS &&
		wall_now - observation->last_advance_wall_ms >= 0 &&
		wall_now - observation->last_advance_wall_ms <= STREAM_MODE2_DIRECT_FRESH_MS;
	int dynamic_source = 0;
	int dynamic_target = STREAM_NO_PTS_VALUE;
	int dynamic_delay = -1;

	if( direct_valid && !s->mode2_dynamic_clock_active ) {
		s->mode2_dynamic_clock_active = 1;
		s->mode2_dynamic_clock_ready = 0;
		s->mode2_dynamic_clock_ts = static_heard_ts;
		s->mode2_dynamic_clock_wall_ms = wall_now;
		s->mode2_dynamic_clock_grace_until_wall_ms = 0;
		DBG serprintf("mode2_dynamic_clock_enter: epoch=%llu heard=%d target=%d delay=%d rate_streak=%d stable_streak=%d fmt=%04X rate=%d forced=%d recode=%d\n",
			(unsigned long long)s->mode2_heard_epoch, static_heard_ts,
			observation->direct_heard_ts,
			observation->direct_delay_ms, observation->direct_rate_streak,
			observation->direct_stable_streak, observation->format,
			observation->rate, !validated_profile, ac3_recoding);
	}
	if( !s->mode2_dynamic_clock_active ) {
		return static_heard_ts;
	}

	int elapsed_ms = wall_now - s->mode2_dynamic_clock_wall_ms;
	if( elapsed_ms < 0 ) {
		elapsed_ms = 0;
	}
	if( direct_valid ) {
		dynamic_source = 1;
		dynamic_delay = observation->direct_delay_ms;
		// The presentation frontier and its wall epoch are an atomic observation.
		// Recombining a new submitted frontier with an old occupancy sample makes
		// compressed write bursts appear as heard progress.
		dynamic_target = observation->direct_heard_ts +
			(wall_now - observation->direct_heard_wall_ms);
		s->mode2_dynamic_clock_last_delay_ms = dynamic_delay;
		s->mode2_dynamic_clock_grace_until_wall_ms = 0;
	} else if( s->mode2_dynamic_clock_last_delay_ms >= 0 &&
		s->mode2_dynamic_clock_grace_until_wall_ms > 0 &&
		wall_now <= s->mode2_dynamic_clock_grace_until_wall_ms ) {
		// Only a non-flushing pause gets a grace window while the observer
		// re-establishes confidence over the preserved AudioTrack queue.
		dynamic_source = 2;
		dynamic_delay = s->mode2_dynamic_clock_last_delay_ms;
		dynamic_target = s->audio_time - dynamic_delay;
	} else {
		// Catch the maintained static fallback gradually so loss of evidence cannot
		// create a visible forward jump. AC3 recode's fallback includes pacer lead.
		dynamic_source = 3;
		dynamic_target = static_heard_ts;
		s->mode2_dynamic_clock_grace_until_wall_ms = 0;
	}

	if( s->paused || s->paused_internal ) {
		s->mode2_dynamic_clock_wall_ms = wall_now;
	} else if( dynamic_target > s->mode2_dynamic_clock_ts ) {
		int extra_ms = dynamic_source == 3 ? MAX(elapsed_ms / 4, 1) : 25;
		int maximum_advance = elapsed_ms + extra_ms;
		int gap = dynamic_target - s->mode2_dynamic_clock_ts;
		s->mode2_dynamic_clock_ts += MIN(gap, maximum_advance);
		s->mode2_dynamic_clock_wall_ms = wall_now;
	} else {
		// A larger measured delay would move heard time backward. Hold phase until
		// physical presentation catches up.
		s->mode2_dynamic_clock_wall_ms = wall_now;
	}
	if( direct_valid && !s->mode2_dynamic_clock_ready &&
		dynamic_target == s->mode2_dynamic_clock_ts ) {
		s->mode2_dynamic_clock_ready = 1;
		DBG serprintf("mode2_dynamic_clock_ready: epoch=%llu heard=%d target=%d delay=%d recode=%d\n",
			(unsigned long long)s->mode2_heard_epoch,
			s->mode2_dynamic_clock_ts, dynamic_target, dynamic_delay,
			ac3_recoding);
	}

	int heard_ts = s->mode2_dynamic_clock_ts;
	if( dynamic_source == 3 && heard_ts == dynamic_target ) {
		s->mode2_dynamic_clock_active = 0;
		s->mode2_dynamic_clock_ready = 0;
		heard_ts = dynamic_target;
		DBG serprintf("mode2_dynamic_clock_fallback: epoch=%llu heard=%d static=%d recode=%d\n",
			(unsigned long long)s->mode2_heard_epoch, heard_ts,
			dynamic_target, ac3_recoding);
	}
	if( wall_now - s->mode2_dynamic_clock_last_log_ms >= 500 ) {
		s->mode2_dynamic_clock_last_log_ms = wall_now;
		DBG serprintf("mode2_dynamic_clock: wall=%d source=%d target=%d heard=%d gap=%d delay=%d static=%d audio=%d paused=%d recode=%d\n",
			wall_now, dynamic_source, dynamic_target, heard_ts,
			dynamic_target == STREAM_NO_PTS_VALUE ? 0 : dynamic_target - heard_ts,
			dynamic_delay, static_heard_ts, s->audio_time,
			s->paused || s->paused_internal, ac3_recoding);
	}
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
	int delay_valid = delay_status.is_delay_valid;
	int passthrough_mode = s->audio_sink ? s->audio_sink->get_passthrough( s ) : 0;
	int ac3_recoding = libavos_get_ac3_recoding_enabled();
	int is_mode2_sync = (passthrough_mode >= 2) || ac3_recoding;
	int static_latency = s->audio_ctx ? audio_interface_get_latency( s->audio_ctx ) : 0;
	int heard_delay = _stream_current_heard_delay( s, &delay_status, is_mode2_sync );
	int ac3_pacer_lead = 0;
	if( ac3_recoding ) {
		ac3_pacer_lead = _stream_ac3_recode_pacer_lead_ms( s );
		heard_delay += ac3_pacer_lead;
	}

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

	int heard_ts = s->audio_time - heard_delay;

	// Direct mode2 AudioTrack writes are accepted in coarse compressed-buffer
	// quanta (for example six 32ms EAC3 packets at once). The submitted endpoint
	// remains the accounting ceiling, but it is not a continuous presentation
	// clock. Interpolate heard time from CLOCK_MONOTONIC between accepted batches
	// and clamp it to that endpoint. AC3 recode is excluded because its dedicated
	// wall-clock burst pacer already supplies smooth write timing.
	if( passthrough_mode >= 2 && !ac3_recoding ) {
		int raw_heard_ts = heard_ts;
		int reset_interp = 0;
		int do_log = 0;

		pthread_mutex_lock( &s->mode2_heard_mutex );
		if( s->sync_v_time >= 0 || s->mode2_heard_prevideo_phase_active ||
			s->mode2_heard_frontier_seed_pending ) {
			int fixed_latency = s->audio_ctx ?
				audio_interface_get_fixed_latency( s->audio_ctx ) : 0;
			if( fixed_latency < 0 || fixed_latency > heard_delay ) {
				fixed_latency = 0;
			}

			// Epoch resets. Each cause needs a different seed because the buffer
			// fill state differs (avos-444: seeding every reset at raw pushed video
			// back ~360ms per mid-playback track change, accumulating):
			//
			// 1. First start (!valid): seed at raw. heard <= 0 then gates video
			//    until audio becomes audible; the pre-audible phase is discarded by
			//    the delay-change reset once the normalized latency freezes
			//    (avos-429 showed a permanent ~270ms deficit when it was carried).
			// 2. Mid-playback sink recreation (track change): the track was torn
			//    down and reopened, so its compressed buffer is EMPTY. Remove that
			//    capacity from the seed, but retain the fixed downstream route delay:
			//    the first accepted frame still traverses AudioFlinger/HAL/HDMI before
			//    it is heard. Raw assumes a full buffer and understates presentation
			//    by the capacity during refill, making the sync gate hold video against
			//    a phantom deficit and push it permanently late. Raw races up under
			//    the frozen-phase clock as blocking writes refill; the normal
			//    envelope resumes once it catches up. This case is signaled
			//    explicitly by the reconfigure path (frontier_seed_pending): it is
			//    not inferable here, because audio_time is continuous across a
			//    track change so raw does not jump backward (avos-446). The
			//    backward-raw check remains as a fallback for flush paths that
			//    do rewind the timeline.
			// 3. Selected-delay change with a continuous frontier (normalized
			//    latency freezing shortly after a restart): keep the clock
			//    monotonic. Seeding at the new raw would snap the frontier-seeded
			//    clock back down mid-refill and reintroduce the deficit of case 2.
			int first_start = !s->mode2_heard_interp_valid;
			int delay_change = !first_start &&
				heard_delay != s->mode2_heard_interp_delay_ms;
			// A larger normalized latency moves raw heard time backward without
			// emptying the sink. Do not let that expected clock recalculation take
			// the legacy backward-raw restart path and seed at audio_time.
			int frontier_restart = s->mode2_heard_frontier_seed_pending ||
				(!first_start && !delay_change &&
				 raw_heard_ts < s->mode2_heard_interp_raw_ts);
			reset_interp = first_start || frontier_restart || delay_change;
			s->mode2_heard_frontier_seed_pending = 0;

			if( reset_interp ) {
				int seed;
				const char *seed_cause;
				if( frontier_restart ) {
					seed = raw_heard_ts + heard_delay - fixed_latency;
					seed_cause = "restart_frontier";
				} else if( delay_change && stream_sync_anchor_get_sink( s ) < 0 ) {
					// Initial normalization replaces a provisional latency before an
					// authoritative playback epoch exists. Adopt its physical phase;
					// preserving the provisional phase would retain the full delta.
					seed = raw_heard_ts;
					seed_cause = "initial_latency";
				} else if( delay_change && s->mode2_heard_interp_ts > raw_heard_ts ) {
					seed = s->mode2_heard_interp_ts;	// keep phase, stay monotonic
					seed_cause = "delay_monotonic";
				} else {
					seed = raw_heard_ts;
					seed_cause = first_start ? "initial_raw" :
						(delay_change ? "delay_raw" : "raw_discontinuity");
				}
				s->mode2_heard_interp_valid = 1;
				s->mode2_heard_interp_ts = seed;
				s->mode2_heard_interp_wall_ms = wall_now;
				if( frontier_restart && s->sync_v_time < 0 ) {
					// A seek/reopen can deliver audio before the first video sync sample.
					// Keep the empty-buffer phase authoritative instead of falling back
					// to raw audio_time-delay on the following audio-side calls.
					s->mode2_heard_prevideo_phase_active = 1;
				}
				DBG serprintf("mode2_epoch_seed: cause=%s audio=%d raw=%d seed=%d delay=%d fixed=%d sink_ref=%d seek_epoch=%d\n",
					seed_cause, s->audio_time, raw_heard_ts, seed, heard_delay,
					fixed_latency, stream_sync_anchor_get_sink( s ), s->seek_epoch);
			} else {
				if( s->paused || s->paused_internal ) {
					// Playback is paused: physical presentation is frozen, so hold the
					// interpolated phase and keep the wall epoch current so resume does
					// not credit the pause duration as elapsed audio.
					s->mode2_heard_interp_wall_ms = wall_now;
				} else {
					int elapsed_ms = wall_now - s->mode2_heard_interp_wall_ms;
					if( elapsed_ms < 0 ) {
						// atime() wrap or an invalid epoch: preserve phase and restart.
						elapsed_ms = 0;
					}
					int candidate = s->mode2_heard_interp_ts + elapsed_ms;
					// Free-run ahead of the frontier: between accepted batches the
					// buffer drains while playback continues, so physical presentation
					// legitimately exceeds raw. The physical bound is the buffered
					// amount itself (selected delay minus fixed route latency): presentation can never
					// be more than one full buffer ahead of the full-buffer model.
					// This also keeps the empty-buffer frontier seed (raw + delay,
					// track-change restart) inside the envelope during refill.
					// MAX_LEAD_MS remains a floor for routes whose HAL batch quantum
					// exceeds a small capacity; the cap only bounds true starvation.
					int capacity_lead = heard_delay - fixed_latency;
					int max_lead = capacity_lead > STREAM_MODE2_HEARD_INTERP_MAX_LEAD_MS ?
						capacity_lead : STREAM_MODE2_HEARD_INTERP_MAX_LEAD_MS;
					if( candidate > raw_heard_ts + max_lead ) {
						candidate = raw_heard_ts + max_lead;
					}
					if( candidate > s->mode2_heard_interp_ts ) {
						s->mode2_heard_interp_ts = candidate;
					}
					s->mode2_heard_interp_wall_ms = wall_now;
				}
				// Frontier re-anchor: when a write batch is accepted, the buffer has
				// just refilled to capacity, so at that instant physical presentation
				// equals audio_time - selected_delay exactly. If the frontier heard
				// point jumps above the interpolated clock (buffer fill after
				// start/seek/track change, or HAL batch jitter), snap up to it. This
				// restores the raw clock's self-correction during buffer fill instead
				// of carrying a permanent heard deficit (avos-432 track-change desync).
				if( raw_heard_ts > s->mode2_heard_interp_ts ) {
					s->mode2_heard_interp_ts = raw_heard_ts;
				}
			}

			s->mode2_heard_interp_raw_ts = raw_heard_ts;
			s->mode2_heard_interp_delay_ms = heard_delay;
			heard_ts = s->mode2_heard_interp_ts;

			// The direct-mode2 interpolator supplies the static fallback phase. A
			// trusted timestamp may then replace it with measured presentation.
			heard_ts = _stream_apply_mode2_dynamic_clock_locked( s, wall_now,
				heard_ts, 0 );
			// Audio can publish put_time before the video thread establishes its new
			// sync sample. Keep using the explicit pause/seek phase during that window;
			// once video is live, the ordinary sync_v_time condition owns continuity.
			if( s->sync_v_time >= 0 ) {
				s->mode2_heard_prevideo_phase_active = 0;
			}

			if( wall_now - s->mode2_heard_interp_last_log_ms >= 500 ) {
				s->mode2_heard_interp_last_log_ms = wall_now;
				do_log = 1;
			}
		}

		pthread_mutex_unlock( &s->mode2_heard_mutex );

		if( do_log ) {
			DBG serprintf("mode2_heard_interp: wall=%d raw=%d interp=%d ceiling_gap=%d audio=%d delay=%d reset=%d paused=%d\n",
				wall_now, raw_heard_ts, heard_ts, raw_heard_ts - heard_ts,
				s->audio_time, heard_delay, reset_interp,
				s->paused || s->paused_internal);
		}
	}

	// AC3 recode retains its dedicated wall-clock writer pacer, but when that
	// encoded output resolves to raw Android Mode 2, trusted AudioTimestamp
	// occupancy is authoritative for presentation. The measured queue already
	// contains pacer write-ahead; the static fallback above keeps adding pacer
	// lead only while presentation evidence is unavailable.
	if( passthrough_mode >= 2 && ac3_recoding &&
		(s->sync_v_time >= 0 || s->mode2_heard_prevideo_phase_active) ) {
		pthread_mutex_lock( &s->mode2_heard_mutex );
		heard_ts = _stream_apply_mode2_dynamic_clock_locked( s, wall_now,
			heard_ts, 1 );
		if( s->sync_v_time >= 0 ) {
			s->mode2_heard_prevideo_phase_active = 0;
		}
		pthread_mutex_unlock( &s->mode2_heard_mutex );
	}

	// 3. STARTUP CLAMP (Non-Mode 2 only)
	if( !is_mode2_sync && passthrough_mode && stream_sync_anchor_get_sink( s ) == -1 &&
		heard_ts < s->audio_time - STREAM_MODE1_STARTUP_CLAMP_MS && s->audio_time > s->video_time ) {
		int clamped = s->audio_time - STREAM_MODE1_STARTUP_CLAMP_MS;
		DBG serprintf( "stream_get_heard_audio_ts: mode1 startup hold clamp: %d->%d (audio=%d video=%d)\n",
			heard_ts, clamped, s->audio_time, s->video_time );
		heard_ts = clamped;
	}

	// Preserves full physical delay offset for passthrough startup.
	// For PCM in put_time mode, allow negative heard_ts during the buffer-fill phase
	// (sink_ref_time <= 0) so sfdec2 can schedule frames relative to when audio is heard.
	if( heard_ts < 0 && !passthrough_mode && stream_sync_anchor_get_sink( s ) > 0 ) {
		heard_ts = 0;
	}

	if (_sync_diag_should_log(s)) {
		DBGY2 serprintf("heard_ts_calc: audio_time=%d heard_ts=%d\n", s->audio_time, heard_ts);
	}

	// AudioTrack speed-epoch: use a playhead-derived heard clock after any AT speed change.
	// Replaces the write-burst clock (audio_time - last_good) with a presentation-
	// position clock anchored at the speed-change moment.
	// Active until cleared by _stream_pcm_delay_memory_reset (seek/flush/stop).
	if( s->at_speed_epoch_active && !passthrough_mode && !is_mode2_sync ) {
		UINT64 ep_frames = s->at_speed_epoch_presented_frames;
		int ep_rate = s->at_speed_epoch_rate;

		// Refresh the playhead on every epoch query.
		// A stale cached playhead introduces stair-step jitter perceptible at speed.
		// Optimization (linear interpolation between samples) deferred to a
		// separate commit after correctness is established.
		{
			UINT64 frames_fresh = 0;
			int rate_fresh = 0, src_fresh = 0, age_fresh = 0;
			if( audio_interface_get_presented_frames( s->audio_ctx, &frames_fresh, &rate_fresh, &src_fresh, &age_fresh, 1 )
				&& frames_fresh >= ep_frames ) {
				s->at_speed_epoch_frames_cached = frames_fresh;
				s->at_speed_epoch_cache_wall_ms = wall_now;
			}
		}

		UINT64 frames_now = s->at_speed_epoch_frames_cached;
		if( ep_frames > 0 && ep_rate > 0 && frames_now >= ep_frames ) {
			// frames_delta is in RST/media-sample domain; convert to TS via RST_TO_TS_DELTA.
			UINT64 frames_delta = frames_now - ep_frames;
			int delta_media_ms = (int)((frames_delta * 1000) / (UINT64)ep_rate);
			int delta_ts = RST_TO_TS_DELTA( delta_media_ms, int );
			int checkpoint_heard = s->at_speed_epoch_heard_ts + delta_ts;
			DBG {
				static int last_epoch_log_ms = 0;
				if( wall_now - last_epoch_log_ms >= 500 ) {
					last_epoch_log_ms = wall_now;
					serprintf( "at_epoch_clock: checkpoint=%d old=%d diff=%d audio=%d delta_media=%d speed=%.3f epoch_age=%d\n",
						checkpoint_heard, heard_ts, checkpoint_heard - heard_ts,
						s->audio_time, delta_media_ms, s->at_speed_epoch_speed,
						wall_now - s->at_speed_epoch_wall_ms );
				}
			}
			heard_ts = checkpoint_heard;
		}
	}

	if( s->atempo_ledger_active && s->atempo_ledger_count > 0 &&
		audio_interface_is_audio_speed_enabled() && audio_interface_is_using_atempo() &&
		!passthrough_mode && !is_mode2_sync && s->audio_ctx ) {
		UINT64 playhead = 0;
		int playhead_rate = 0, playhead_src = 0, playhead_age = 0;
		if( audio_interface_get_presented_frames( s->audio_ctx, &playhead, &playhead_rate,
				&playhead_src, &playhead_age, 1 ) && playhead_rate > 0 ) {
			ATEMPO_LEDGER_LOOKUP raw = _stream_atempo_ledger_lookup( s, playhead, playhead_rate );
			float cur_speed = audio_interface_get_audio_speed();
			// SRC_TIMESTAMP = extrapolated AudioTrack getTimestamp framePosition:
			// the frame at the DAC, latency-free, so the raw ledger lookup IS the
			// heard clock.  SRC_PLAYHEAD = getPlaybackHeadPosition (mixer hand-off),
			// which needs the calibrated post-playhead latency subtracted first.
			int playhead_is_dac = (playhead_src == AT_PRESENTED_FRAMES_SRC_TIMESTAMP);

			// Calibrate the post-playhead latency (in output frames) at 1.0x against
			// the legacy heard model.  Only meaningful for the mixer playhead source.
			if( !playhead_is_dac &&
				raw.heard != STREAM_NO_PTS_VALUE && raw.state == 0 &&
				delay_valid && cur_speed > 0.999f && cur_speed < 1.001f &&
				playhead_age <= 100 &&
				wall_now - s->atempo_ledger_lat_last_ms >= 250 ) {
				s->atempo_ledger_lat_last_ms = wall_now;
				int bias_ms = raw.heard - heard_ts;
				int64_t target = ((int64_t)bias_ms * playhead_rate) / 1000;
				if( target < 0 )
					target = 0;
				if( s->atempo_ledger_lat_samples == 0 )
					s->atempo_ledger_lat_frames = target;
				else
					s->atempo_ledger_lat_frames += (target - s->atempo_ledger_lat_frames) / 4;
				if( s->atempo_ledger_lat_samples < 1000 )
					s->atempo_ledger_lat_samples++;
				if( s->atempo_ledger_lat_samples >= 4 )
					s->atempo_ledger_lat_valid = 1;
			}

			// Use the atempo ledger heard clock in place of the legacy delay model.
			int ledger_applied = 0;
			int eff_heard = STREAM_NO_PTS_VALUE;
			if( playhead_is_dac ) {
				if( raw.heard != STREAM_NO_PTS_VALUE && raw.state >= 0 ) {
					eff_heard = raw.heard;
					ledger_applied = 1;
				}
			} else if( s->atempo_ledger_lat_valid ) {
				UINT64 lat = (UINT64)s->atempo_ledger_lat_frames;
				UINT64 playhead_eff = playhead > lat ? playhead - lat : 0;
				ATEMPO_LEDGER_LOOKUP eff = _stream_atempo_ledger_lookup( s, playhead_eff, playhead_rate );
				if( eff.heard != STREAM_NO_PTS_VALUE && eff.state >= 0 ) {
					eff_heard = eff.heard;
					ledger_applied = 1;
				}
			}

			// Reject stale playhead samples (e.g. cached from before/during pause)
			if( ledger_applied && playhead_age > 100 ) {
				ledger_applied = 0;
			}

			if( raw.heard != STREAM_NO_PTS_VALUE ) {
				int now_ms = wall_now;
				int dense = s->atempo_ledger_dense_until_ms > 0 &&
					now_ms <= s->atempo_ledger_dense_until_ms;
				if( dense || now_ms - s->atempo_ledger_last_log_ms >= 500 ) {
					UINT64 flt_out = 0;
					int flt_fifo = 0, flt_rate = 0;
					stream_filter_audio_atempo_get_ledger_stats( s->audio_filter_atempo,
						&flt_out, &flt_fifo, &flt_rate );
					s->atempo_ledger_last_log_ms = now_ms;
					DBG serprintf("at_ledger: ledger_heard=%d heard=%d diff=%d eff_heard=%d lat_frames=%lld applied=%d playhead=%llu out_written=%llu state=%d block_start=%llu block_ts=%d block_nframes=%d q_fifo=%d flt_out=%llu flt_rate=%d src=%d age=%d speed=%.3f dense=%d\n",
						raw.heard, heard_ts, raw.heard - heard_ts,
						eff_heard, (long long)s->atempo_ledger_lat_frames, ledger_applied,
						(unsigned long long)playhead,
						(unsigned long long)s->atempo_ledger_output_frames,
						raw.state, (unsigned long long)raw.block_start,
						raw.block_ts, raw.block_nframes, flt_fifo,
						(unsigned long long)flt_out, flt_rate,
						playhead_src, playhead_age, cur_speed, dense);
				}
			}

			if( ledger_applied ) {
				heard_ts = eff_heard;
			}
		}
	}

	static int last_diag_wall = 0;
	if (wall_now > last_diag_wall + 2000) {
		last_diag_wall = wall_now;
		DBG serprintf("heard_ts_diag: wall=%d audio=%d heard=%d h_delay=%d eff=%d delay_valid=%d source=%s tag=%s\n",
			wall_now, s->audio_time, heard_ts, heard_delay,
			delay_status.effective_delay_ms, delay_status.is_delay_valid,
			_stream_delay_source_name(delay_status.source),
			delay_status.source_tag ? delay_status.source_tag : "none");
		if( passthrough_mode >= 2 || ac3_recoding ) {
			int user_av_delay = s->av_delay + stream_dbg_delay;
			int diff = STREAM_NO_PTS_VALUE;
			int heard_no_pacer = s->audio_time - (heard_delay - ac3_pacer_lead);
			if( s->sync_v_time != STREAM_NO_PTS_VALUE ) {
				diff = (s->sync_v_time - heard_ts) + RST_TO_TS_DELTA( user_av_delay, int );
			}
			DBG serprintf("mode2_timeline: wall=%d fmt=%04X passthrough=%d ac3=%d audio=%d heard=%d heard_no_pacer=%d pacer_lead=%d video=%d sync_v=%d diff=%d latency=%d latency_no_pacer=%d source=%s tag=%s\n",
				wall_now, s->audio ? s->audio->format : 0, passthrough_mode,
				ac3_recoding, s->audio_time, heard_ts, heard_no_pacer,
				ac3_pacer_lead, s->video_time, s->sync_v_time, diff, heard_delay,
				heard_delay - ac3_pacer_lead, _stream_delay_source_name(delay_status.source),
				delay_status.source_tag ? delay_status.source_tag : "none");
		}
	}

	return heard_ts;
}

int stream_get_heard_audio_ts( STREAM *s, int fallback_ts )
{
	int heard_ts = _stream_get_heard_audio_ts_internal( s, fallback_ts );
	// Consume only the observer's cached sample. This keeps JNI off the
	// scheduler thread and lets shadow diagnostics continue while the compressed
	// writer is idle or paused.
	stream_sync_compressed_shadow_observe( s );
	return heard_ts;
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
	s->audio_start_gap_hold = 0;
	_stream_pcm_delay_memory_reset( s );
	s->warmup_video_frames = 0;

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
	pthread_mutex_lock( &s->video_sink_mutex );
	int video_sink_not_open = s->video_sink && !s->video_sink->is_open;
	pthread_mutex_unlock( &s->video_sink_mutex );
	if (video_sink_not_open) {
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
	if (fabsf(audio_interface_get_audio_speed() - 1.0f) <= 1e-6f) {
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
	int ac3_pacer_lead = 0;
	if( ac3_recoding ) {
		ac3_pacer_lead = _stream_ac3_recode_pacer_lead_ms( s );
		sink_delay += ac3_pacer_lead;
	}
	// wold time video sink delay not dependant on audio speed
	int video_delay;
	if( s->vtime_post_sink ) {
		// we sample after the sink, so the time stamps are the one we get out of the sink
		video_delay = 0;
	} else {
		// we sample before the sink, so the video frames have to pass through the sink
		pthread_mutex_lock( &s->video_sink_mutex );
		video_delay = ( s->video_sink && s->video_sink->is_open && s->video_sink->delay ) ?
			s->video_sink->delay( s->video_sink ) : 0;
		pthread_mutex_unlock( &s->video_sink_mutex );
 	}
	if( s->sync_mode == STREAM_SYNC_SAMPLES ) {
		// In sample-based sync, the audio sink's sample counter is the master clock.
		// The codec_delay is upstream from the sink and not part of this clock,
		// so it's excluded to prevent an incorrect sync bias.
		int total_delay = /*codec_delay +*/ filter_delay + sink_delay - video_delay;
		if (diag_log) {
			DBGY2 serprintf("stream_sync_av_delay: samples mode codec=%d filter=%d (atempo=%d) sink=%d sink_no_pacer=%d pacer_lead=%d video=%d total=%d total_no_pacer=%d speed=%.3f using_atempo=%d passthrough=%d ac3=%d\n",
				codec_delay, filter_delay, atempo_delay, sink_delay,
				sink_delay - ac3_pacer_lead, ac3_pacer_lead, video_delay, total_delay,
				total_delay - ac3_pacer_lead,
				audio_interface_get_audio_speed(), s->audio_filter_atempo != NULL, passthrough, ac3_recoding);
		}
		return total_delay;
	} else {
		int total_delay = codec_delay + filter_delay + sink_delay - video_delay;
		if (diag_log) {
			DBGY2 serprintf("stream_sync_av_delay: codec=%d filter=%d (atempo=%d) sink=%d sink_no_pacer=%d pacer_lead=%d video=%d total=%d total_no_pacer=%d speed=%.3f using_atempo=%d passthrough=%d ac3=%d\n",
				codec_delay, filter_delay, atempo_delay, sink_delay,
				sink_delay - ac3_pacer_lead, ac3_pacer_lead, video_delay, total_delay,
				total_delay - ac3_pacer_lead,
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

	if( !use_heard_time && sync_delay <= 0 ) {
		int delay_valid = s->audio_ctx ? audio_interface_is_delay_valid( s->audio_ctx ) : 1;
		if( delay_valid && s->audio_ctx ) {
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
	if( fabsf(audio_interface_get_audio_speed() - 1.0f) <= 1e-6f ) {
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
	// Post-seek sensitive until the first audio frame is committed (sync_a_time set).
	// The stateful lead-gate contribution (pcm_audio_lead_state == HOLDING) was removed
	// in Commit C; the seek window naturally expires on first committed audio output.
	int post_seek_pending = s->seek_epoch > 0 && s->sync_a_time == -1;
	return (startup_hold_active && !delay_valid) ||
		s->audio_start_pending ||
		s->audio_resume_pending ||
		post_seek_pending;
}

static int _stream_pcm_should_update_delay_cache( int sensitive_phase, int delay_streak,
	stream_delay_source_t source )
{
	// Keep last_good HW-only and based on real timing evidence; static/startup
	// fallbacks are safe to use transiently but must not poison the cache.
	if( source != STREAM_DELAY_SOURCE_DYNAMIC &&
		source != STREAM_DELAY_SOURCE_LAST_GOOD ) {
		return 0;
	}
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

static void _stream_pcm_reanchor_disarm( STREAM *s, const char *reason )
{
	if( !s ) {
		return;
	}
	if( s->pcm_reanchor_state != STREAM_PCM_REANCHOR_INACTIVE ||
		s->audio_resume_valid_pending ) {
		DBG serprintf("pcm_reanchor: state=%s -> inactive reason=%s source=%s delay=%d seek_epoch=%d\n",
			_stream_pcm_reanchor_state_name( s->pcm_reanchor_state ),
			reason ? reason : "none",
			_stream_pcm_reanchor_source_name( s->pcm_reanchor_source ),
			s->pcm_reanchor_delay_ms, s->seek_epoch);
	}
	_stream_pcm_reanchor_reset( s );
}

void stream_sync_pcm_reanchor_arm( STREAM *s, int passthrough_active )
{
	if( !s ) {
		return;
	}
	if( passthrough_active ) {
		_stream_pcm_reanchor_disarm( s, "passthrough" );
		return;
	}
	if( s->audio_start_pending ) {
		_stream_pcm_reanchor_disarm( s, "startup" );
		return;
	}
	if( s->put_time_mode ) {
		_stream_pcm_reanchor_disarm( s, "put_time" );
		return;
	}
	s->pcm_reanchor_state = STREAM_PCM_REANCHOR_ARMED;
	s->pcm_reanchor_seek_epoch = s->seek_epoch;
	s->pcm_reanchor_source = PCM_REANCHOR_SOURCE_NONE;
	s->pcm_reanchor_delay_ms = 0;
	s->audio_resume_valid_pending = 1;
	DBG serprintf("pcm_reanchor: state=armed video=%d sync_v=%d audio=%d seek_epoch=%d\n",
		s->video_time, s->sync_v_time, s->audio_time, s->seek_epoch);
}

static int _stream_pcm_reanchor_select_delay( STREAM *s, int *delay_ms,
	int *source, const char **tag )
{
	const char *delay_source = audio_interface_get_delay_source( s->audio_ctx );

	if( tag ) {
		*tag = delay_source;
	}
	// Priority: last_good → static latency → skip.
	// Do not wait for dynamic delay stability here. Dynamic delay is a
	// selected-delay provider during normal playback; the Phase 2 drift gate
	// handles later correction if last_good is stale.
	if( s->last_good_delay_valid ) {
		int atempo_delay   = stream_get_atempo_delay( s );
		int lg_total       = s->last_good_delay_ms + atempo_delay;
		int static_lat     = audio_interface_get_latency( s->audio_ctx );
		int static_total   = (static_lat > 0) ? (static_lat + atempo_delay) : 0;
		// Clamp last_good up to static latency: after AudioTrack flush+preload the
		// queue starts at at least static_lat, so anchoring on a low drain-state
		// last_good value (e.g. 15ms) would place audio_time too early and cause
		// audio-leads-video desync visible immediately after resume.
		if( static_total > 0 && lg_total < static_total ) {
			DBG serprintf("pcm_reanchor_delay: last_good_clamped lg=%d static=%d selected=%d\n",
				lg_total, static_total, static_total);
			*delay_ms = static_total;
		} else {
			*delay_ms = lg_total;
		}
		*source = PCM_REANCHOR_SOURCE_LAST_GOOD;
		return 1;
	}
	*delay_ms = audio_interface_get_latency( s->audio_ctx );
	if( *delay_ms > 0 ) {
		// Static is only a cold resume fallback, but it still needs the filter
		// delay so heard_ts lands on sync_v_time after the reanchor.
		*delay_ms += stream_get_atempo_delay( s );
		*source = PCM_REANCHOR_SOURCE_STATIC;
		return 1;
	}
	return 0;
}

int stream_sync_pcm_reanchor_update( STREAM *s, int passthrough_active )
{
	if( !s || s->pcm_reanchor_state == STREAM_PCM_REANCHOR_INACTIVE ||
		s->pcm_reanchor_state == STREAM_PCM_REANCHOR_APPLIED ||
		s->pcm_reanchor_state == STREAM_PCM_REANCHOR_EXPIRED ) {
		return 0;
	}
	if( passthrough_active ) {
		_stream_pcm_reanchor_disarm( s, "passthrough" );
		return 0;
	}
	if( s->audio_start_pending ) {
		_stream_pcm_reanchor_disarm( s, "startup" );
		return 0;
	}
	if( s->pcm_reanchor_seek_epoch != s->seek_epoch ) {
		DBG serprintf("pcm_reanchor: state=expired reason=seek_epoch_changed armed=%d current=%d\n",
			s->pcm_reanchor_seek_epoch, s->seek_epoch);
		_stream_pcm_reanchor_disarm( s, "expired" );
		return 0;
	}

	// One-shot apply: attempt to select a delay and apply immediately.
	// Clear the latch whether the reanchor applies or is skipped — no WAITING_STABLE retry.
	int delay = 0;
	int source = PCM_REANCHOR_SOURCE_NONE;
	const char *tag = NULL;
	if( !s->audio_ctx || s->video_time < 0 || s->audio_time < 0 ||
		!_stream_pcm_reanchor_select_delay( s, &delay, &source, &tag ) ) {
		DBG serprintf("pcm_reanchor: skipped reason=no_delay video=%d audio=%d ctx=%d seek_epoch=%d\n",
			s->video_time, s->audio_time, s->audio_ctx != NULL, s->seek_epoch);
		_stream_pcm_reanchor_disarm( s, "skip" );
		return 0;
	}

	int old_audio_time = s->audio_time;
	int rebase_video_time = (s->sync_v_time >= 0) ? s->sync_v_time : s->video_time;
	int new_audio_time = rebase_video_time + delay;
	s->pcm_reanchor_state = STREAM_PCM_REANCHOR_APPLIED;
	s->pcm_reanchor_source = source;
	s->pcm_reanchor_delay_ms = delay;
	s->audio_resume_valid_pending = 0;
	s->audio_time = new_audio_time;
DBGA	serprintf(" <<%d>> ", s->audio_time);
	stream_sync_audio( s, s->audio_time );
	DBG serprintf("pcm_reanchor: state=applied audio_time %d -> %d video=%d sync_v=%d delay=%d source=%s tag=%s seek_epoch=%d\n",
		old_audio_time, s->audio_time, s->video_time, s->sync_v_time,
		delay, _stream_pcm_reanchor_source_name( source ),
		tag ? tag : "none",
		s->seek_epoch);
	return 1;
}

int stream_sync_pcm_audio_lead_gate( STREAM *s, int ac3_recoding )
{
	// Hold the audio producer when heard audio is materially ahead of video.
	// This is a PCM-only scheduler guard. Compressed passthrough is exempt:
	// mode1 is paced by IEC writes, while mode2 relies on blocking
	// AudioTrack.write() for real buffer backpressure. Gating mode2 from its
	// packet-quantized logical clock creates a write/hold limit cycle and visible
	// video stutter. AC3 recoding is paced separately.
	// No persistent state, no hysteresis.
	if( !s || !s->put_time_mode || !s->audio_sink || ac3_recoding ||
		s->sync_v_time == -1 || s->sync_v_time == STREAM_NO_PTS_VALUE ||
		s->audio_time == -1 ||
		s->video_hold_for_resume_audio ) {
		return 0;
	}
	int passthrough_mode = s->audio_sink->get_passthrough ?
		s->audio_sink->get_passthrough( s ) : 0;
	if( passthrough_mode >= 1 ) {
		return 0;
	}
	int heard_ts = stream_get_heard_audio_ts( s, s->audio_time );
	if( heard_ts == STREAM_NO_PTS_VALUE ) {
		return 0;
	}
	int gate_ms = STREAM_PCM_AUDIO_LEAD_GATE_MS;
	int user_av_delay = s->av_delay + stream_dbg_delay;
	if( user_av_delay < 0 ) {
		// The lead gate prevents producer runaway; it is not the realizer for
		// negative manual A/V delay.  Negative delay is applied once in the
		// output domain by _wait().  If the gate also includes the negative
		// target, it continuously holds audio and can deadlock startup before
		// the one-shot hold has a stable anchor.
		user_av_delay = 0;
	}
	// Positive manual delay intentionally holds video behind audio, so audio
	// legitimately leads sync_v by up to user_av_delay.  Fold that into the
	// gate threshold; otherwise the gate reads the intended lead as runaway
	// and stalls the producer, deadlocking video and audio together.
	int raw_diff = s->sync_v_time - heard_ts;
	int diff = raw_diff + RST_TO_TS_DELTA( user_av_delay, int );
	if( diff < -gate_ms ) {
		DBG serprintf("pcm_audio_lead_gate: hold sync_v=%d heard=%d diff=%d raw_diff=%d av_delay=%d dbg_delay=%d gate=%d\n",
			s->sync_v_time, heard_ts, diff, raw_diff, s->av_delay, stream_dbg_delay, gate_ms);
		return 1;
	}
	return 0;
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

	// Apply a deferred atempo video commit once the playhead crosses the boundary
	// where new-speed content starts playing.
	stream_atempo_commit_poll( s );

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
	int pcm_startup_request_correction = 0;

	_sync_diag_log_state(s, "audio", &delay_status);

	if( !delay_valid && !anchor_valid ) {
		// No usable timing (no dynamic and no static/last-good); disable delay compensation.
		current_av_delay = 0;
		anchor_delay = 0;
	}
	if( delay_valid && delay_status.source == STREAM_DELAY_SOURCE_DYNAMIC ) {
		int delay_streak = delay_status.streak;
		int sensitive_phase = _stream_pcm_delay_sensitive_phase( s, delay_valid );
		int allow_update = _stream_pcm_should_update_delay_cache( sensitive_phase,
			delay_streak, delay_status.source );

		if( !allow_update ) {
			static int last_good_skip_count = 0;
			if( last_good_skip_count < 5 ) {
				DBG serprintf( "last_good_skip_gate[%d]: source=%s tag=%s streak=%d delay=%d sensitive=%d\n",
					last_good_skip_count,
					_stream_delay_source_name( delay_status.source ),
					delay_status.source_tag ? delay_status.source_tag : "none",
					delay_streak, current_av_delay, sensitive_phase );
				last_good_skip_count++;
			}
		} else {
			_stream_pcm_update_delay_cache( s, current_av_delay, delay_streak, sensitive_phase );
		}
	}
	// Log when delay is valid but source is not DYNAMIC and no evidence carried —
	// these are true non-dynamic sources (static, fallback) that cannot refresh last_good.
	if( delay_valid && delay_status.source != STREAM_DELAY_SOURCE_DYNAMIC &&
		!delay_status.has_dynamic_evidence ) {
		static int last_good_skip_nd_count = 0;
		if( last_good_skip_nd_count < 5 ) {
			int sensitive_phase = _stream_pcm_delay_sensitive_phase( s, delay_valid );
			DBG serprintf( "last_good_skip_nd[%d]: source=%s tag=%s streak=%d delay=%d sensitive=%d\n",
				last_good_skip_nd_count,
				_stream_delay_source_name( delay_status.source ),
				delay_status.source_tag ? delay_status.source_tag : "none",
				delay_status.streak, current_av_delay, sensitive_phase );
			last_good_skip_nd_count++;
		}
	}
	// Refresh last_good from dynamic evidence using a candidate stability filter.
	// last_good is a one-time snapshot unless we do this: _stream_get_delay_status()
	// converts valid dynamic timing to LAST_GOOD source for stability, so the DYNAMIC
	// block above never fires again after the first write.
	//
	// Filter rules (prevents AT burst/drain oscillation poisoning last_good):
	//   !sensitive_phase required — no bypass; warmup/resume spikes must not leak in.
	//   delta ≤ COMMIT_DELTA (12ms):    commit directly (normal slow drift).
	//   delta 12–60ms (medium zone):    ignore + reset candidate; this is the EAC3
	//                                   burst/drain amplitude; neither extreme is a
	//                                   valid new baseline for resume anchoring.
	//   delta ≥ DRIFT_CORRECT (60ms):   candidate accumulation — route-change / HW
	//                                   reset scale; requires CANDIDATE_COUNT stable
	//                                   samples within CANDIDATE_BAND before commit.
	//   sensitive_phase active:         reset candidate accumulator, do nothing.
	if( delay_status.has_dynamic_evidence && delay_status.dynamic_evidence_ms > 0 ) {
		int sensitive_phase = _stream_pcm_delay_sensitive_phase( s, delay_valid );
		if( sensitive_phase ) {
			// Discard any candidate in progress — don't accumulate during warmup/seek/resume.
			s->last_good_candidate_ms    = 0;
			s->last_good_candidate_count = 0;
		} else {
			int evidence_ms = delay_status.dynamic_evidence_ms;
			int evidence_streak = delay_status.dynamic_evidence_streak;
			if( !s->last_good_delay_valid ) {
				// No baseline yet — commit immediately.
				_stream_pcm_update_delay_cache( s, evidence_ms, evidence_streak, sensitive_phase );
				s->last_good_candidate_ms    = 0;
				s->last_good_candidate_count = 0;
			} else {
				int last_good_total = s->last_good_delay_ms + stream_get_atempo_delay( s );
				int delta = evidence_ms - last_good_total;
				if( delta < 0 ) delta = -delta;
				if( delta <= STREAM_PCM_EVIDENCE_COMMIT_DELTA_MS ) {
					// Close enough — commit directly, discard any stale candidate.
					_stream_pcm_update_delay_cache( s, evidence_ms, evidence_streak, sensitive_phase );
					s->last_good_candidate_ms    = 0;
					s->last_good_candidate_count = 0;
				} else if( delta >= STREAM_PCM_DELAY_DRIFT_CORRECT_MS ) {
					// Very large drift (≥60ms): route-change or major HW reset —
					// allow candidate accumulation to commit.
					if( s->last_good_candidate_count == 0 ) {
						// Start fresh candidate.
						s->last_good_candidate_ms    = evidence_ms;
						s->last_good_candidate_count = 1;
					} else {
						int cand_delta = evidence_ms - s->last_good_candidate_ms;
						if( cand_delta < 0 ) cand_delta = -cand_delta;
						if( cand_delta <= STREAM_PCM_EVIDENCE_CANDIDATE_BAND_MS ) {
							// Still within band — accumulate running average.
							s->last_good_candidate_ms = ( s->last_good_candidate_ms *
								s->last_good_candidate_count + evidence_ms ) /
								( s->last_good_candidate_count + 1 );
							s->last_good_candidate_count++;
							if( s->last_good_candidate_count >= STREAM_PCM_EVIDENCE_CANDIDATE_COUNT ) {
								DBG serprintf( "evidence_candidate_commit: candidate=%d count=%d last_good=%d delta=%d streak=%d\n",
									s->last_good_candidate_ms, s->last_good_candidate_count,
									last_good_total, delta, evidence_streak );
								_stream_pcm_update_delay_cache( s, s->last_good_candidate_ms,
									evidence_streak, sensitive_phase );
								s->last_good_candidate_ms    = 0;
								s->last_good_candidate_count = 0;
							}
						} else {
							// Jumped outside band — restart candidate.
							s->last_good_candidate_ms    = evidence_ms;
							s->last_good_candidate_count = 1;
						}
					}
				} else {
					// Medium drift (12–60ms): EAC3 burst/drain oscillation zone —
					// ignore and discard any candidate to prevent phase-locked flipping.
					s->last_good_candidate_ms    = 0;
					s->last_good_candidate_count = 0;
				}
			}
		}
	}
	if( s->pcm_startup_correction_pending ) {
		int passthrough_mode = s->audio_sink && s->audio_sink->get_passthrough ?
			s->audio_sink->get_passthrough( s ) : 0;
		int ac3_recoding = 0;
#ifdef CONFIG_AUDIO_AC3
		ac3_recoding = libavos_get_ac3_recoding_enabled();
#endif
		if( passthrough_mode || ac3_recoding ||
			s->pcm_startup_correction_seek_epoch != s->seek_epoch ) {
			s->pcm_startup_correction_pending = 0;
		} else if( s->pcm_startup_correction_speed_epoch != s->audio_speed_diag_epoch ) {
			// A speed change invalidates the old phase comparison. Keep the feature
			// armed for headphones at non-1.0x, but wait for evidence from the new
			// atempo/PlaybackParams epoch.
			s->pcm_startup_seed_delay_ms = stream_get_pcm_startup_seed_delay_ms( s ) +
				stream_get_atempo_delay( s );
			s->pcm_startup_correction_speed_epoch = s->audio_speed_diag_epoch;
			DBG serprintf("pcm_startup_correction: rearm speed_epoch=%d seed=%d speed=%.3f\n",
				s->audio_speed_diag_epoch, s->pcm_startup_seed_delay_ms,
				audio_interface_get_audio_speed());
		} else if( delay_status.has_dynamic_evidence &&
			delay_status.dynamic_evidence_streak >= STREAM_PCM_STARTUP_DIRECT_STREAK ) {
			int delta = delay_status.dynamic_evidence_ms - s->pcm_startup_seed_delay_ms;
			int abs_delta = ABS( delta );
			if( abs_delta >= STREAM_PCM_STARTUP_CORRECTION_MIN_MS &&
				abs_delta <= STREAM_PCM_STARTUP_CORRECTION_MAX_MS ) {
				pcm_startup_request_correction = 1;
				DBG serprintf("pcm_startup_correction: request seed=%d dynamic=%d delta=%d streak=%d speed=%.3f epoch=%d\n",
					s->pcm_startup_seed_delay_ms, delay_status.dynamic_evidence_ms,
					delta, delay_status.dynamic_evidence_streak,
					audio_interface_get_audio_speed(), s->audio_speed_diag_epoch);
			} else {
				DBG serprintf("pcm_startup_correction: complete without slew seed=%d dynamic=%d delta=%d streak=%d\n",
					s->pcm_startup_seed_delay_ms, delay_status.dynamic_evidence_ms,
					delta, delay_status.dynamic_evidence_streak);
			}
			s->pcm_startup_correction_pending = 0;
		}
	}
	// Check if passthrough mode is active - static delay is immediately valid
	int passthrough_mode = s->audio_sink ? s->audio_sink->get_passthrough( s ) : 0;
	int sink_ref_time = stream_sync_anchor_get_sink( s );
	if( passthrough_mode == 1 ) {
		DBG serprintf("pt_mode1_sync_audio: in=%d stored=%d sync_a=%d video=%d seek_epoch=%d anchor_valid=%d delay_valid=%d source=%s tag=%s anchor_delay=%d current_delay=%d sink_ref=%d\n",
			audio_time, s->audio_time, s->sync_a_time, s->video_time, s->seek_epoch,
			anchor_valid, delay_valid, _stream_delay_source_name(delay_status.source),
			delay_status.source_tag ? delay_status.source_tag : "none",
			anchor_delay, current_av_delay, sink_ref_time);
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
				(passthrough_mode > 0) && _stream_is_sink_driven( s ) &&
				(sink_ref_time == -1 || s->sync_a_time == -1);

			if (diag_log) {
				DBGY2 serprintf("anchor_ts: audio_time=%d current=%d source=%s tag=%s av_delay=%d anchor=%d\n",
					audio_time, current_av_delay,
					_stream_delay_source_name(delay_status.source),
					delay_status.source_tag ? delay_status.source_tag : "none",
					s->av_delay, anchor_ts);
			}
			anchor_ts = _apply_user_av_delay_ts( s, anchor_ts );
			anchor_ts -= RST_TO_TS_DELTA( stream_dbg_delay, int );

			if( force_passthrough_reanchor && sink_ref_time != -1 ) {
				DBG serprintf("stream_sync_audio: passthrough explicit reanchor: old_sink=%d audio=%d video=%d seek_epoch=%d resume_pending=%d sync_a=%d\n",
					sink_ref_time, audio_time, s->video_time, s->seek_epoch,
					s->audio_resume_pending, s->sync_a_time);
			}
			
			// In put_time mode, audio writes own heard-time anchoring.  Keep
			// this on the audio path; the video path must not continuously
			// rewrite the scheduler anchor from its sync loop.
			// Skip pre-commit negative anchors: heard_ts is negative because
			// static/fallback latency exceeds the first audio PTS. Anchoring the
			// scheduler there creates a stale reference that normal mode-2
			// reanchor heuristics may not replace. Leave sink_ref_time=-1 until
			// audible time reaches zero.
			if( anchor_ts >= 0 ) {
				stream_sync_anchor_publish( s, anchor_ts, s->video_time, 0,
					force_passthrough_reanchor && sink_ref_time != -1 );
			} else {
				DBG serprintf("stream_sync_audio: defer negative anchor audio=%d anchor=%d video=%d seek_epoch=%d pt=%d\n",
					audio_time, anchor_ts, s->video_time, s->seek_epoch, passthrough_mode);
			}
		}
	}
	if( pcm_startup_request_correction ) {
		sfdec2_request_pcm_startup_correction( s );
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
		if (s->put_time_mode && s->sync_v_time == -1) {
			return 0; // Don't block audio writes during startup/resume when video hasn't outputted yet
		}
		return 1;
	}
	
	// if audio is in the future, delay it (but only if significantly ahead)
	int diff = _stream_av_diff( s, s->sync_v_time, audio_time_for_diff );
	if( diff < 0 && s->av_delay < 0 ) {
		// Negative manual delay is realized by the one-shot audio hold
		// (_wait), not by continuous producer blocking. Keep _stream_av_diff()
		// signed for diagnostics/accounting, but do not let the negative
		// user-delay term deadlock this gate.
		diff -= RST_TO_TS_DELTA( s->av_delay, int );
	}

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
DBGY			serprintf("sync_video: timing unavailable, free-run video (anchor_valid=0 audio_time=%d sync_a=%d vtime=%d put_time=%d delay_valid=%d source=%s tag=%s)\n",
				s->audio_time, s->sync_a_time, s->sync_v_time,
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
DBGY			serprintf("sync_video: timing unavailable, free-run video (anchor_valid=0 audio_time=%d sync_a=%d vtime=%d put_time=%d)\n",
				s->audio_time, s->sync_a_time, s->sync_v_time,
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
	// PCM and an explicitly delayed Mode 1 start hold their first audio write
	// until video reaches audio_start_target_ts. Admit the forward frame that
	// closes that gap before requiring an audio clock, otherwise both threads wait.
	int startup_audio_gap_hold = !passthrough_mode ||
		(passthrough_mode == 1 && s->audio_start_gap_hold);
	if( s->put_time_mode && startup_audio_gap_hold && s->audio_start_pending &&
		s->audio_start_target_ts != STREAM_NO_PTS_VALUE &&
		s->video_time < s->audio_start_target_ts && video_time >= s->video_time ) {
		return 0;
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

	// Under put_time mode (android_sync=1), the video sink paces frames using precise
	// timed release to V-Sync with a lookahead of up to 200ms in the TS domain.
	// To avoid choking this pipeline and forcing ASAP releases via coarse thread sleeps,
	// we allow the video thread to lead by up to 300ms in the TS domain directly.
	// Legacy post-sink non-put_time pipelines use real-time scale (RST) allowance.
	int max_wait;
	if (s->put_time_mode) {
		max_wait = 300;
	} else {
		int max_rst = s->vtime_post_sink ? 500 : 0;
		max_wait = RST_TO_TS_DELTA( max_rst, int );
	}
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
	STREAM *s = AV_get_ctx();
	if( s ) {
		int delta = argc > 1 ? atoi( argv[1] ) : 20;
		stream_set_av_delay( s, s->av_delay + delta );
		stream_dbg_delay = 0;
serprintf("user_delay %5d\n", s->av_delay );
serprintf("av_delay   %5d\n", stream_sync_av_delay( s ) );
	} else {
		if( argc > 1 ) {
			stream_dbg_delay += atoi( argv[1] );
		} else {
			stream_dbg_delay += 20;
		}
	}
serprintf("dbg_delay %5d\n", stream_dbg_delay );
}

static void _stream_delay_minus( int argc, char *argv[] )
{
	STREAM *s = AV_get_ctx();
	if( s ) {
		int delta = argc > 1 ? atoi( argv[1] ) : 20;
		stream_set_av_delay( s, s->av_delay - delta );
		stream_dbg_delay = 0;
serprintf("user_delay %5d\n", s->av_delay );
serprintf("av_delay   %5d\n", stream_sync_av_delay( s ) );
	} else {
		if( argc > 1 ) {
			stream_dbg_delay -= atoi( argv[1] );
		} else {
			stream_dbg_delay -= 20;
		}
	}
serprintf("dbg_delay %5d\n", stream_dbg_delay );
}

static void _stream_delay_set( int argc, char *argv[] )
{
	STREAM *s = AV_get_ctx();
	if( s ) {
		int delay = argc > 1 ? atoi( argv[1] ) : 0;
		stream_set_av_delay( s, delay );
		stream_dbg_delay = 0;
serprintf("user_delay %5d\n", s->av_delay );
serprintf("av_delay   %5d\n", stream_sync_av_delay( s ) );
	} else if( argc > 1 ) {
		stream_dbg_delay = atoi( argv[1] );
	}
serprintf("dbg_delay %5d\n", stream_dbg_delay );
}

DECLARE_DEBUG_COMMAND("sep", 	_stream_delay_plus   );
DECLARE_DEBUG_COMMAND("sem", 	_stream_delay_minus  );
DECLARE_DEBUG_COMMAND("ses", 	_stream_delay_set    );
DECLARE_DEBUG_PARAM("mode2_dynamic_all", stream_mode2_dynamic_all );

static void _stream_toggle_xbmc( int argc, char *argv[] )
{
	stream_use_xbmc_smoothing = !stream_use_xbmc_smoothing;
	serprintf("stream_use_xbmc_smoothing: %d\n", stream_use_xbmc_smoothing );
}
DECLARE_DEBUG_COMMAND("sxbmc", _stream_toggle_xbmc );
#endif

#endif
