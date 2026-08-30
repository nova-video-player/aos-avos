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
#include <string.h>
#include "codec_utils.h"
#include "stream.h"
#include "stream_sync.h"
#include "stream_alloc.h"
#include "audio_interface.h"
#include "astdlib.h"
#include "athread.h"
#include "rc_clocks.h"
#include "h264.h"
#include "wmv.h"
#include "device_config.h"
#include "xdm_utils.h"
#include "pts_reorder.h"
#include "sfdec.h"
#include "android_window.h"
#include "android_codec.h"

#include <time.h>
#include <limits.h>
#ifdef CONFIG_ANDROID
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#ifdef CONFIG_STREAM

#define DBGS	DBG_IF(0||Debug[DBG_STREAM])
#define DBGCV   DBG_IF(0||Debug[DBG_CV])
#define DBGCV2  DBG_IF(0||Debug[DBG_CV] > 1)
#define DBGCV3  DBG_IF(0||Debug[DBG_CV] > 2)
#define DBGSI   DBG_IF(0||Debug[DBG_SINK])
#define DBGSI2  DBG_IF(0||Debug[DBG_SINK] > 1)

#define MAXW VIDEO_MAX_WIDTH
#define MAXH VIDEO_MAX_HEIGHT

#define SFDEC_MAX_FRAMES 16

#define NSEC_PER_SEC 1000000000L
#define NSEC_PER_MSEC 1000000L

#define CLOG(fmt, ...) serprintf("%s: " fmt "\n", __FUNCTION__, ##__VA_ARGS__)

static int sfdec_max_frames = 2;
static int sfdec_force_hw   = -1;
static int sfdec_force_blit = 0;
static int sfdec_no_drop    = 0;
static int sfdec_threshold  = 200;
DECLARE_DEBUG_PARAM ("sfmf", sfdec_max_frames );
DECLARE_DEBUG_PARAM ("sfhw", sfdec_force_hw );
DECLARE_DEBUG_TOGGLE("sffb", sfdec_force_blit );
DECLARE_DEBUG_TOGGLE("sfnd", sfdec_no_drop );
DECLARE_DEBUG_PARAM ("sfth", sfdec_threshold );

// Helper: detect whether audio passthrough (IEC/encoded) is active
static inline int _is_passthrough(STREAM *s) {
	if (s && s->audio_sink && s->audio_sink->get_passthrough) {
		return s->audio_sink->get_passthrough(s);
	}
	return 0;
}

enum {
	THREAD_STATE_READING	= 0x01,
	THREAD_STATE_RENDERING	= 0x02,
	THREAD_STATE_FLUSHING	= 0x04,
	THREAD_STATE_WRITING	= 0x08,
};

enum {
	SEEK_STATE_NONE = 0,
	SEEK_STATE_INPUT_SENDING,
	SEEK_STATE_INPUT_SENT,
	SEEK_STATE_OUTPUT_SENDING,
	SEEK_STATE_OUTPUT_SENT,
};

typedef struct priv {
	sfdec_t *sfdec;
	void *surface_handle;

	pthread_t dec_thread;
	pthread_t sink_thread;

	VIDEO_FRAME *frames[SFDEC_MAX_FRAMES];
	int num_frames;
	int reorder_pts;
	int pts_reorder_depth;
	int repair_decode_order_pts;
	int pts_input_monotonic;
	int pts_input_seen;
	int pts_input_last;
	int pts_repair_logged;

	struct XDM_ctx XDM_ctx;

	struct {
		pthread_mutex_t mtx;
		pthread_cond_t cond;

		int run;

		FRAME_Q dec_q;
		FRAME_Q out_q;
		FRAME_Q venc_q;
		FRAME_Q get_q;
		int error;
		int width;
		int height;
	int rotation;
	int interlaced;
	int state;
} locked;

	int venc_put_time;
	int venc_ref_time;

	int dropped;
	int video_frame_rate_num;
	int video_frame_rate_den;
	int playback_speed_num;
	int playback_speed_den;
	INT64 sched_start_off_ns;
	INT64 sched_start_mono_ns;
	INT64 sched_last_off_ns;
	INT64 sched_last_mono_ns;
	int sched_late;
	INT64 sched_debt_ns;	// schedule delay accumulated by the late ratchet, recovered by slew
	int prev_paused;
	int pause_start_ms;
	int pause_armed;
	int slew_active;
	int mode2_dynamic_slew;
	int mode2_dynamic_fast_slew;
	int mode2_dynamic_settle_frames;
	int last_mode2_dynamic_active;
	const void *mode2_slew_frame_handle;
	int mode2_slew_frame_time;
	int mode2_slew_frame_epoch;
	int pcm_startup_slew;
	const void *pcm_startup_slew_frame_handle;
	int pcm_startup_slew_frame_time;
	int pcm_startup_slew_frame_epoch;
	int64_t target_offset_ns;
	int pending_reanchor;
	int pending_seek_reanchor;
	int last_seek_epoch;
	int last_audio_resume_pending;
	int snap_origin_time;
	int snap_origin_epoch;
	STREAM_DEC_VIDEO *dec;
	STREAM *s;
	int64_t render_offset_ns;
	int render_offset_from_audio;
	float last_av_speed;
	int passthrough_cached;		// cached passthrough state to avoid repeated sink queries
	int grace_until_ms;
	int last_user_av_delay;
	int effective_av_delay_ms;
	int drift_dir;
	int drift_streak;
	int hold_audio_until_ms;	// passthrough startup hold
	int hold_audio_start_ms;	// wall clock when passthrough startup hold started
	int hold_audio_applied_ms;	// ms held during passthrough startup
} priv_t;

// Caller must hold p->locked.mtx so venc_put_time/venc_ref_time are sampled
// from the same published clock anchor.
static int _get_time_l( priv_t *p )
{
	int diff = atime() - p->venc_ref_time;
	return p->venc_put_time + diff;
}

static inline INT64 _get_monotonic_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (INT64)ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;
}

// Caller holds p->locked.mtx; snapping owns epoch-scoped phase state.
static INT64 _snap_timestamp_ns(priv_t *p, int frame_time, int frame_epoch)
{
	if( frame_time < 0 )
		return 0;
	if( p->snap_origin_epoch != frame_epoch ) {
		p->snap_origin_time = frame_time;
		p->snap_origin_epoch = frame_epoch;
		DBGSI serprintf("android_sync: snap origin frame=%d epoch=%d\n",
			frame_time, frame_epoch);
	}
	INT64 timestamp_us = (INT64)frame_time * 1000LL;
	if( p->video_frame_rate_den && p->playback_speed_den ) {
		INT64 rendering_num = (INT64)p->video_frame_rate_num * p->playback_speed_num;
		INT64 rendering_den = (INT64)p->video_frame_rate_den * p->playback_speed_den;
		if( rendering_num && rendering_den ) {
			// Preserve the stream's timestamp phase. Absolute snapping around zero
			// aliases a half-frame stream offset into duplicate/skipped deadlines.
			double frame_length = (double)rendering_num / (double)rendering_den;
			INT64 origin_us = (INT64)p->snap_origin_time * 1000LL;
			double relative_us = (double)(timestamp_us - origin_us);
			double frame_index = relative_us * frame_length / 1000000.0;
			INT64 snapped_index = (INT64)llround(frame_index);
			double snapped_delta =
				(double)snapped_index * 1000.0 * 1000.0 / frame_length;
			timestamp_us = origin_us + (INT64)snapped_delta;
		}
	}
	return timestamp_us * 1000LL;
}

static int64_t _get_render_heard_ts(priv_t *p, STREAM *s, int allow_put_time,
	int *used_put_time, int *put_age_ms)
{
	const int k_put_time_fresh_ms = 100;
	int use_put = 0;
	int age_ms = 0;
	int64_t heard_ts = 0;

	if (allow_put_time && p && s && s->audio_time > 0 &&
		p->venc_put_time > 0 && p->venc_ref_time > 0) {
		age_ms = atime() - p->venc_ref_time;
		if (age_ms >= 0 && age_ms <= k_put_time_fresh_ms) {
			heard_ts = p->venc_put_time;
			use_put = 1;
		}
	}
	if (!use_put) {
		heard_ts = s ? (int64_t)stream_get_heard_audio_ts(s, s->audio_time) : 0;
	}
	if (used_put_time) {
		*used_put_time = use_put;
	}
	if (put_age_ms) {
		*put_age_ms = age_ms;
	}
	return heard_ts;
}

static int _update_effective_av_delay_ts(priv_t *p, STREAM *s)
{
	// Video can only be delayed; negative user delay is realized by holding
	// audio, so the sfdec2 video-delay state must never slew below zero.
	int target_av_delay = (s && s->av_delay > 0) ? s->av_delay : 0;
	int effective_av_delay = p->effective_av_delay_ms;
	const int av_delay_slew_step_ms = 40;
	if( effective_av_delay < 0 )
		effective_av_delay = 0;

	if( effective_av_delay < target_av_delay ) {
		int step = target_av_delay - effective_av_delay;
		if( step > av_delay_slew_step_ms )
			step = av_delay_slew_step_ms;
		effective_av_delay += step;
	} else if( effective_av_delay > target_av_delay ) {
		int step = effective_av_delay - target_av_delay;
		if( step > av_delay_slew_step_ms )
			step = av_delay_slew_step_ms;
		effective_av_delay -= step;
	}

	p->effective_av_delay_ms = effective_av_delay;
	return RST_TO_TS_DELTA( effective_av_delay, int );
}

// Reproduce MediaCodec-style WC pacing locally so reordered frames still map to
// the correct deadline.
static int _compute_blit_wait_ms(priv_t *p, VIDEO_FRAME *f, int av_delay_ts)
{
	int frame_time = f ? f->time : -1;
	// Video can only be delayed; negative user delay is realized by holding audio.
	int video_av_delay_ts = av_delay_ts > 0 ? av_delay_ts : 0;
	INT64 timestamp_ns =
		_snap_timestamp_ns(p, frame_time, f ? f->epoch : INT_MIN);
	if( frame_time >= 0 )
		frame_time += video_av_delay_ts;
	timestamp_ns += (INT64)video_av_delay_ts * 1000000LL;
	INT64 now_ns = _get_monotonic_ns();
	INT64 start_off = p->sched_start_off_ns;
	INT64 start_mono = p->sched_start_mono_ns;
	INT64 target_ns = timestamp_ns - start_off + start_mono;
	INT64 delta = target_ns - now_ns;
	int asap = 0;
	int reset_sched = 0;
	int late_before = p->sched_late;

	if( !start_off ||
	    (now_ns - p->sched_last_mono_ns) > 500 * NSEC_PER_MSEC ||
	    (delta < -500 * NSEC_PER_MSEC || delta > 500 * NSEC_PER_MSEC) ) {
		p->sched_start_mono_ns = now_ns + 100 * NSEC_PER_MSEC;
		p->sched_start_off_ns = timestamp_ns;
		p->sched_debt_ns = 0;
		asap = 1;
		reset_sched = 1;
	}

	target_ns = timestamp_ns - p->sched_start_off_ns + p->sched_start_mono_ns;
	delta = target_ns - now_ns;

	// Only count a frame as "late" for the ratchet when it misses by more
	// than half a frame duration (min 10ms).  At high playback speeds the
	// frame cadence shrinks (~21ms at 1.45x) and ordinary jitter makes
	// 3 consecutive small misses (1-6ms) common; with zero tolerance the
	// ratchet then fires every ~1s, holding video 25-55ms behind audio —
	// a real, audible A/V offset invisible to every clock-domain diff.
	int frame_ms = (f && f->duration > 0) ? f->duration : 16;
	INT64 late_tol_ns = (INT64)(frame_ms > 20 ? frame_ms / 2 : 10) * NSEC_PER_MSEC;
	if( !asap && delta < -late_tol_ns ) {
		p->sched_late++;
		if( p->sched_late >= 3 ) {
			int step_ms = frame_ms * 2;
			p->sched_start_mono_ns += (INT64)step_ms * NSEC_PER_MSEC;
			// Remember the push so it can be slewed back once frames are
			// on time again; otherwise each ratchet is a permanent A/V
			// offset below the put_time reanchor threshold.
			p->sched_debt_ns += (INT64)step_ms * NSEC_PER_MSEC;
		}
	} else {
		p->sched_late = 0;
		if( !asap && p->sched_debt_ns > 0 && delta > 0 ) {
			// Recover ratchet debt with a bounded slew (max 2ms per frame),
			// never making the current frame late (step <= delta).
			INT64 step = 2 * NSEC_PER_MSEC;
			if( step > p->sched_debt_ns )
				step = p->sched_debt_ns;
			if( step > delta )
				step = delta;
			p->sched_start_mono_ns -= step;
			p->sched_debt_ns -= step;
		}
	}

	target_ns = timestamp_ns - p->sched_start_off_ns + p->sched_start_mono_ns;
	p->sched_last_mono_ns = now_ns;
	p->sched_last_off_ns = timestamp_ns;

	if( asap ) {
		DBGSI serprintf(
			"sink_wait_calc: frame=%d base=%d ts_ms=%d now_mono_ms=%lld target_mono_ms=%lld wait_ms=0 asap=1 reset=%d late=%d->%d start_off_ms=%lld start_mono_ms=%lld av_ts=%d debt_ms=%lld\n",
			f ? f->index : -1,
			f ? f->time : -1,
			frame_time,
			(long long)(now_ns / NSEC_PER_MSEC),
			(long long)(target_ns / NSEC_PER_MSEC),
			reset_sched,
			late_before,
			p->sched_late,
			(long long)(p->sched_start_off_ns / NSEC_PER_MSEC),
			(long long)(p->sched_start_mono_ns / NSEC_PER_MSEC),
			av_delay_ts,
			(long long)(p->sched_debt_ns / NSEC_PER_MSEC));
		return 0;
	}

	delta = target_ns - now_ns;
	if( delta > (INT64)INT_MAX * NSEC_PER_MSEC )
		delta = (INT64)INT_MAX * NSEC_PER_MSEC;
	if( delta < (INT64)INT_MIN * NSEC_PER_MSEC )
		delta = (INT64)INT_MIN * NSEC_PER_MSEC;

	{
		int wait_ms = (int)( delta / 1000000LL );
		DBGSI serprintf(
			"sink_wait_calc: frame=%d base=%d ts_ms=%d now_mono_ms=%lld target_mono_ms=%lld wait_ms=%d asap=0 reset=%d late=%d->%d start_off_ms=%lld start_mono_ms=%lld av_ts=%d debt_ms=%lld\n",
			f ? f->index : -1,
			f ? f->time : -1,
			frame_time,
			(long long)(now_ns / NSEC_PER_MSEC),
			(long long)(target_ns / NSEC_PER_MSEC),
			wait_ms,
			reset_sched,
			late_before,
			p->sched_late,
			(long long)(p->sched_start_off_ns / NSEC_PER_MSEC),
			(long long)(p->sched_start_mono_ns / NSEC_PER_MSEC),
			av_delay_ts,
			(long long)(p->sched_debt_ns / NSEC_PER_MSEC));
		return wait_ms;
	}
}

static inline void timespec_add_ns(struct timespec *a, INT64 ns)
{
	long sec = (a->tv_nsec + ns) / NSEC_PER_SEC;
	a->tv_sec += sec;
	a->tv_nsec = a->tv_nsec + ns - sec * NSEC_PER_SEC;
}

static inline void timespec_add_ms(struct timespec *a, int ms)
{
	timespec_add_ns(a, (INT64)ms * 1000000L);
}

static inline void set_state_l(priv_t *p, int state, int mask)
{
	int prev_state = p->locked.state;
	p->locked.state = (p->locked.state&~mask) | (state&mask);

	if (prev_state != p->locked.state) {
		DBGCV3 CLOG("%s|%s|%s|%s",
		    p->locked.state & THREAD_STATE_READING ? "reading" : "!reading",
		    p->locked.state & THREAD_STATE_RENDERING ? "rendering" : "!rendering",
		    p->locked.state & THREAD_STATE_FLUSHING ? "flushing" : "!flushing",
		    p->locked.state & THREAD_STATE_WRITING ? "writing" : "!writing");
		pthread_cond_broadcast(&p->locked.cond);
	}
}

static inline void add_state_l(priv_t *p, int state)
{
	set_state_l(p, state, state);
}

static inline void rm_state_l(priv_t *p, int state)
{
	set_state_l(p, 0, state);
}

static inline int has_state_l(priv_t *p, int state)
{
	return (p->locked.state & state) == state;
}

static int videosink_open(STREAM_SINK_VIDEO *sink, VIDEO_PROPERTIES *video, void *ctx, int num_frames, STREAM_RC *rc)
{
	sink->ctx = ctx;
	priv_t *p = (priv_t *) sink->priv;
	STREAM *s = (STREAM *)ctx;
	p->last_av_speed = 1.0f;
	p->grace_until_ms = 0;
	p->last_user_av_delay = s ? s->av_delay : 0;

	pthread_mutex_lock(&p->locked.mtx);
	int i;
	for (i = 0; i < p->num_frames; ++i)
		frame_q_put(&p->locked.get_q, p->frames[i]);
	pthread_mutex_unlock(&p->locked.mtx);
	sink->is_open = 1;
	CLOG("opened");
	return sink->flush(sink);
}

static int videosink_close(STREAM_SINK_VIDEO *sink)
{
	if (!sink->is_open)
		return 1;

	CLOG();

	sink->is_open = 0;
	return 0;
}

static int videosink_delete(STREAM_SINK_VIDEO *sink)
{
	CLOG();

	afree(sink);
	return 0;
}

static int videosink_put(STREAM_SINK_VIDEO *sink, VIDEO_FRAME *frame)
{
	priv_t *p = (priv_t *) sink->priv;
	int time;

	if (!sink->is_open)
		return 0;

	pthread_mutex_lock(&p->locked.mtx);
	frame_q_put(&p->locked.venc_q, frame);
	pthread_cond_broadcast(&p->locked.cond);
	time = _get_time_l(p);
	pthread_mutex_unlock(&p->locked.mtx);

DBGSI2 CLOG("frame %2d/%8d handle %p", frame->index, frame->time, frame->android_handle);
	return time;
}

static int videosink_get(STREAM_SINK_VIDEO *sink, VIDEO_FRAME **pframe)
{
	priv_t *p = (priv_t *) sink->priv;

	pthread_mutex_lock(&p->locked.mtx);
	*pframe = frame_q_get(&p->locked.get_q);
	pthread_mutex_unlock(&p->locked.mtx);

	return *pframe ? 0 : 1;
}

static int videosink_flush_and_enqueue(STREAM_SINK_VIDEO *sink)
{
	return 0;
}

static int videosink_end(STREAM_SINK_VIDEO *sink)
{
	return 0;
}

static int videosink_syncable(STREAM_SINK_VIDEO *sink)
{
	return 1;
}

static VIDEO_FRAME *videosink_get_frame(STREAM_SINK_VIDEO *sink, int index)
{
	if (!sink || !sink->is_open)
		return NULL;
		
	priv_t *p = (priv_t *) sink->priv;
	return index < p->num_frames ? p->frames[index] : NULL;
}

static int videosink_put_time( STREAM_SINK_VIDEO *sink, int time )
{
	priv_t *p = (priv_t *) sink->priv;
	STREAM *s = (STREAM*)sink->ctx;
	if( !s ) {
		pthread_mutex_lock(&p->locked.mtx);
		s = p->s;
		pthread_mutex_unlock(&p->locked.mtx);
	}

	// Query other subsystems before taking the codec-private lock. The lock
	// serializes this sink's clock/scheduler state only.
	float current_speed = audio_interface_get_audio_speed();
	int passthrough_mode = (s && s->audio_sink && s->audio_sink->get_passthrough) ?
		s->audio_sink->get_passthrough( s ) : 0;

	pthread_mutex_lock(&p->locked.mtx);
	if (!p->s && s)
		p->s = s;

	int now_ms = atime();
	int dt = time    - p->venc_put_time;
	int dr = now_ms - p->venc_ref_time;

	// Detect speed change (explicit discontinuity)
	int speed_changed = 0;
	if (fabsf(current_speed - p->last_av_speed) > 0.001f) {
		speed_changed = 1;
		p->last_av_speed = current_speed;
		DBGSI serprintf("videosink_put_time: speed changed to %.2f\n", current_speed);
		p->grace_until_ms = now_ms + 1000;
	}

	// Detect seek epoch changes to force reanchor
	int epoch_changed = 0;
	int resume_started = 0;
	if (s) {
		if (s->seek_epoch != p->last_seek_epoch) {
			epoch_changed = 1;
			p->last_seek_epoch = s->seek_epoch;
		}
		if (s->audio_resume_pending && !p->last_audio_resume_pending) {
			resume_started = 1;
		}
		p->last_audio_resume_pending = s->audio_resume_pending;
	}

	int expected = p->venc_put_time + dr;
	int diff = time - expected;
	int abs_diff = diff < 0 ? -diff : diff;
	int drift_threshold_ms = 200;
	int manual_hold_ms = 0;
	if( s && s->manual_audio_hold_pending_ms > 0 ) {
		// A manual negative A/V delay just inserted an intentional audio hold
		// (PCM silence). The next put_time sees that gap as grown dr; subtract it
		// so the gap is not mistaken for clock drift and reanchored away, which
		// would cancel the user delay.
		manual_hold_ms = s->manual_audio_hold_pending_ms;
		s->manual_audio_hold_pending_ms = 0;
	}
	int dr_for_sync = dr - manual_hold_ms;
	if( dr_for_sync < 0 )
		dr_for_sync = 0;
	if( s && s->put_time_mode && !passthrough_mode ) {
		int frame_ms = (s->video && s->video->msPerFrame > 0) ?
			s->video->msPerFrame : 33;
		drift_threshold_ms = MAX( 160, frame_ms * 4 );
	}
	expected = p->venc_put_time + dr_for_sync;
	diff = time - expected;
	abs_diff = diff < 0 ? -diff : diff;
	int discontinuity = p->venc_put_time && abs_diff >= drift_threshold_ms;
	int reanchor_discontinuity = discontinuity;
	int smooth_burst_mode = s && s->put_time_mode &&
		(!passthrough_mode || passthrough_mode >= 2) && !speed_changed;
	if( discontinuity && smooth_burst_mode ) {
		int frame_ms = (s->video && s->video->msPerFrame > 0) ?
			s->video->msPerFrame : 33;
		int hard_drift_ms = (passthrough_mode >= 2) ?
			MAX( 1500, frame_ms * 16 ) : MAX( 350, frame_ms * 8 );
		if( !passthrough_mode ) {
			p->drift_streak++;
		}
		// PCM and mode-2 passthrough heard time can legitimately move in write
		// bursts while the physical sink clock remains continuous.  Keep the
		// existing scheduler anchor unless this is hard drift.
		reanchor_discontinuity = (abs_diff >= hard_drift_ms);
	} else if( !discontinuity ) {
		p->drift_streak = 0;
	}
	if( discontinuity ) {
		if( passthrough_mode ) {
			if( !smooth_burst_mode || reanchor_discontinuity ) {
				p->grace_until_ms = now_ms + 1000;
			}
		} else if( reanchor_discontinuity && !speed_changed ) {
			p->grace_until_ms = 0;
		}
	}

	if( !speed_changed && !discontinuity && p->venc_put_time && time < p->venc_put_time ) {
		time = p->venc_put_time;
		diff = time - expected;
		abs_diff = diff < 0 ? -diff : diff;
	}

	if( !p->venc_put_time ) {
		p->grace_until_ms = now_ms + 1000;
	}
	int in_grace = (p->grace_until_ms > 0 && now_ms < p->grace_until_ms);

	int no_sched_anchor = (p->sched_start_off_ns == 0 || p->sched_start_mono_ns == 0);
	// sfdec2_android_sync_on_pause() already shifts a valid Mode 2 render
	// offset by the paused wall duration. Reanchoring it again from the
	// submitted-frontier heard clock can discard the established device/route
	// phase when that frontier is ahead of physical presentation. Missing
	// anchors and real discontinuities still take the normal reanchor path.
	int resume_reanchor = resume_started &&
		(passthrough_mode < 2 || p->render_offset_ns == -1);
	int allow_reanchor = speed_changed || reanchor_discontinuity || no_sched_anchor ||
		epoch_changed || resume_reanchor;
	if( in_grace && !speed_changed && !discontinuity && !no_sched_anchor &&
		!epoch_changed && !resume_reanchor ) {
		allow_reanchor = 0;
	}
	DBGSI serprintf(
		"put_time_calc: req=%d now=%d old_put=%d old_ref=%d dt=%d dr=%d expected=%d diff=%d abs=%d thresh=%d streak=%d speed=%.3f speed_changed=%d disc=%d reanchor_disc=%d grace=%d no_sched=%d resume=%d allow_reanchor=%d\n",
		time,
		now_ms,
		p->venc_put_time,
		p->venc_ref_time,
		dt,
		dr,
		expected,
		diff,
		abs_diff,
		drift_threshold_ms,
		p->drift_streak,
		current_speed,
		speed_changed,
		discontinuity,
		reanchor_discontinuity,
		in_grace,
		no_sched_anchor,
		resume_started,
		allow_reanchor);
	p->venc_put_time = time;
	p->venc_ref_time = atime();
	if (allow_reanchor) {
		if( epoch_changed ) {
			p->pending_seek_reanchor = 1;
		}
		p->sched_start_off_ns  = (INT64)time * 1000000LL;
		p->sched_start_mono_ns = _get_monotonic_ns();
		p->sched_last_off_ns   = p->sched_start_off_ns;
		p->sched_last_mono_ns  = p->sched_start_mono_ns;
		p->sched_late          = 0;
		p->sched_debt_ns       = 0;
		p->render_offset_ns    = -1; // Force immediate re-anchor on seek/resume/discontinuity/speed change.
		p->render_offset_from_audio = 0;
		p->pending_reanchor    = 1;
		DBGSI serprintf("videosink_put_time: reset sched and render anchors at time=%d, diff=%d (speed_changed=%d disc=%d no_sched=%d resume=%d)\n",
			time, diff, speed_changed, discontinuity, no_sched_anchor, resume_started);
	}

DBGSI2 serprintf("[[put %8d|%4d|%4d]]", time, dt, dr );
	pthread_mutex_unlock(&p->locked.mtx);
	return 0;
}

// Zero the scheduler anchors so the next put_time() call sees no_sched_anchor=1
// and is forced to reanchor even if the heard timestamp hasn't changed.
// Used by the post-seek converge path when the normal put_time() would be a no-op.
void sfdec2_refresh_sched_anchor( STREAM *s )
{
	if( !s || !s->video_sink || !s->video_sink->priv )
		return;
	if( !s->video_sink->name || strcmp( s->video_sink->name, "sfdec2" ) != 0 )
		return;
	priv_t *p = (priv_t*) s->video_sink->priv;
	pthread_mutex_lock(&p->locked.mtx);
	p->s = (STREAM*)s->video_sink->ctx;
	p->sched_start_off_ns  = 0;
	p->sched_start_mono_ns = 0;
	p->sched_last_off_ns   = 0;
	p->sched_last_mono_ns  = 0;
	p->sched_late          = 0;
	p->sched_debt_ns       = 0;
	pthread_mutex_unlock(&p->locked.mtx);
}

void sfdec2_request_pcm_startup_correction( STREAM *s )
{
	if( !s || !s->video_sink || !s->video_sink->priv )
		return;
	if( !s->video_sink->name || strcmp( s->video_sink->name, "sfdec2" ) != 0 )
		return;
	priv_t *p = (priv_t*)s->video_sink->priv;
	pthread_mutex_lock(&p->locked.mtx);
	if( p->render_offset_ns != -1 && p->render_offset_from_audio ) {
		p->pending_reanchor = 1;
		p->pcm_startup_slew = 1;
		p->pcm_startup_slew_frame_handle = NULL;
		p->pcm_startup_slew_frame_time = INT_MIN;
		p->pcm_startup_slew_frame_epoch = INT_MIN;
		pthread_cond_broadcast(&p->locked.cond);
		DBGSI serprintf("android_sync: PCM startup correction armed offset=%lld\n",
			(long long)p->render_offset_ns);
	} else {
		DBGSI serprintf("android_sync: PCM startup correction already covered by pending anchor offset=%lld from_audio=%d\n",
			(long long)p->render_offset_ns, p->render_offset_from_audio);
	}
	pthread_mutex_unlock(&p->locked.mtx);
}

static int videosink_get_time( STREAM_SINK_VIDEO *sink )
{
	priv_t *p = (priv_t *) sink->priv;
	int time;

	pthread_mutex_lock(&p->locked.mtx);
	time = _get_time_l( p );
	pthread_mutex_unlock(&p->locked.mtx);
	return time;
}

static int videosink_clear(STREAM_SINK_VIDEO *sink)
{
	return 0;
}

static int videosink_resize(STREAM_SINK_VIDEO *sink, VIDEO_PROPERTIES *video)
{
	return 0;
}

static STREAM_SINK_VIDEO *videosink_new(priv_t *p)
{
	STREAM_SINK_VIDEO *sink = (STREAM_SINK_VIDEO *) acalloc(1, sizeof(STREAM_SINK_VIDEO));

	if (!sink)
		goto err;

	sink->name      = "sfdec2";
	sink->open	= videosink_open;
	sink->close	= videosink_close;
	sink->delete	= videosink_delete;
	sink->put	= videosink_put;
	sink->get	= videosink_get;
	sink->flush	= videosink_flush_and_enqueue;
	sink->end	= videosink_end;
	sink->syncable	= videosink_syncable;
	sink->get_frame = videosink_get_frame;
	sink->get_time	= videosink_get_time;
	sink->put_time	= videosink_put_time;
	sink->clear	= videosink_clear;
	sink->resize	= videosink_resize;

	sink->primary	= STREAM_SINK_DEFAULT_SCREEN;

	sink->allocates_frames = 1;

	sink->priv = p;

	return sink;
err:
	if (sink)
		afree(sink);
	return NULL;
}

static int videodec_get_error(priv_t *p)
{
	int error;

	pthread_mutex_lock(&p->locked.mtx);
	error = p->locked.error;
	pthread_mutex_unlock(&p->locked.mtx);
	return error;
}

static void frame_release(sfdec_t *sfdec, VIDEO_FRAME *f)
{
	if (f->android_handle) {
DBGCV3 CLOG("release f(%d) %p ->", f->index, f->android_handle);
		sfdec_buf_release(sfdec, (sfbuf_t *)f->android_handle);
		f->android_handle = NULL;
DBGCV3 CLOG("release <-");
	}
}

static void frame_discard_after_flush(sfdec_t *sfdec, VIDEO_FRAME *f)
{
	if (f->android_handle) {
		sfdec_buf_discard(sfdec, (sfbuf_t *)f->android_handle);
		f->android_handle = NULL;
	}
}

static void *videosink_thread(void *ctx)
{
	priv_t *p = (priv_t*) ctx;
	STREAM *s = (STREAM *)p->dec->ctx;

	pthread_mutex_lock(&p->locked.mtx);
	while (p->locked.run && !p->locked.error) {
		if (has_state_l(p, THREAD_STATE_FLUSHING)) {
			rm_state_l(p, THREAD_STATE_RENDERING);
			pthread_cond_wait(&p->locked.cond, &p->locked.mtx);
			continue;
		}

		if( s ) {
			if( s->paused && !p->prev_paused ) {
				p->prev_paused = 1;
			} else if( !s->paused && p->prev_paused ) {
				p->prev_paused = 0;
			}
		}

		VIDEO_FRAME *f = NULL;
		INT64 render_ts_ns = 0;
		int consumed = 0;
		int stale_epoch_drop = 0;
		while (p->locked.run && !p->locked.error && !has_state_l(p, THREAD_STATE_FLUSHING)) {
			// Peek at the frame at the head of the queue without consuming it
			f = frame_q_peek(&p->locked.venc_q);
			if (!f) {
				rm_state_l(p, THREAD_STATE_RENDERING);
				pthread_cond_wait(&p->locked.cond, &p->locked.mtx);
				continue;
			}
			if (!f->android_handle) {
				// Invalid frame, consume it and let normal flow handle it
				f = frame_q_get(&p->locked.venc_q);
				consumed = 1;
				break;
			}
			// A rapid seek can leave the asynchronous renderer owning a buffer
			// from the previous generation. Reject it before it can consume the
			// one-shot seek reanchor and anchor on a stale PTS.
			if( s && f->epoch != s->seek_epoch ) {
				f = frame_q_get(&p->locked.venc_q);
				consumed = 1;
				stale_epoch_drop = 1;
				DBGSI serprintf("android_sync: stale epoch frame drop f_time=%d frame_epoch=%d seek_epoch=%d\n",
					f->time, f->epoch, s->seek_epoch);
				break;
			}

			if( s && s->audio_ctx ) {
				int delta_ms = audio_interface_get_and_clear_latency_delta( s->audio_ctx );
				if( delta_ms != 0 ) {
					// The centralized heard clock already owns normalization phase:
					// initial_latency adopts the corrected raw phase, while a
					// mid-restart delay_monotonic latch preserves the empty-buffer
					// phase. Moving render_offset here applies the same correction a
					// second time and makes the result depend on whether the renderer
					// anchor existed when this notification was consumed.
					DBGSI serprintf("android_sync: observed mode2 latency correction delta=%d ms (heard clock owns phase)\n",
						delta_ms);
				}
			}

			// Under android_sync=1 timed rendering, check lookahead pre-release wait
			INT64 now_ns = _get_monotonic_ns();

			// Establish or update timeline anchor render_offset_ns
			int have_audio_time = (s && s->audio_time >= 0);
			int passthrough = (s && s->audio_sink && s->audio_sink->get_passthrough) ?
				s->audio_sink->get_passthrough( s ) : 0;
			int mode2_dynamic_active = passthrough == 2 ?
				stream_sync_mode2_dynamic_active( s ) : 0;
			int mode2_dynamic_changed = mode2_dynamic_active !=
				p->last_mode2_dynamic_active;
			if( mode2_dynamic_changed ) {
				p->last_mode2_dynamic_active = mode2_dynamic_active;
				if( mode2_dynamic_active ) {
					// The heard clock has already completed its monotonic catch-up.
					// Rebuild the audio-owned anchor once at this explicit boundary;
					// slewing from the provisional static anchor would apply a second,
					// multi-second correction to the same phase change.
					p->render_offset_ns = -1;
					p->render_offset_from_audio = 0;
					p->target_offset_ns = -1;
					p->slew_active = 0;
					p->pending_reanchor = 0;
				} else {
					p->pending_reanchor = 1;
				}
				// Keep both entry and exit corrections limited to one step per
				// distinct video frame. Entry immediately rebuilds its anchor below;
				// exit still uses the slow bounded fallback slew.
				p->mode2_dynamic_slew = 1;
				p->mode2_dynamic_fast_slew = 0;
				p->mode2_dynamic_settle_frames = 0;
				p->mode2_slew_frame_handle = NULL;
				p->mode2_slew_frame_time = INT_MIN;
				p->mode2_slew_frame_epoch = INT_MIN;
				DBGSI serprintf("android_sync: mode2 dynamic clock transition active=%d hard_reanchor=%d\n",
					mode2_dynamic_active, mode2_dynamic_active);
			}
			int mode2_new_slew_frame = p->mode2_dynamic_slew &&
				(f->android_handle != p->mode2_slew_frame_handle ||
				 f->time != p->mode2_slew_frame_time ||
				 f->epoch != p->mode2_slew_frame_epoch);
			int pcm_startup_new_slew_frame = p->pcm_startup_slew &&
				(f->android_handle != p->pcm_startup_slew_frame_handle ||
				 f->time != p->pcm_startup_slew_frame_time ||
				 f->epoch != p->pcm_startup_slew_frame_epoch);
			int hold_passthrough = 0;

			// Passthrough mode 2 startup hold. Seek preview has no audio producer;
			// holding here would outlive _stream_play_n_frames()'s deadline.
			int seek_preview = s && (s->seek_paused || s->play_n_video_frames > 0);
			if (seek_preview) {
				p->hold_audio_until_ms = 0;
				p->hold_audio_start_ms = 0;
				p->hold_audio_applied_ms = 0;
			}
			if (passthrough == 2 && s && s->audio && s->audio->valid &&
			    !seek_preview &&
			    (s->audio_time < 0 || p->hold_audio_until_ms != 0)) {
				if (p->hold_audio_until_ms == 0) {
					// stream_get_anchor_delay_ms() can lock s->video_sink_mutex.
					// Other threads lock video_sink_mutex first and then call back
					// into this decoder (e.g. stream_sync_anchor_publish() ->
					// videosink_put_time()), which locks p->locked.mtx. Drop
					// p->locked.mtx here to avoid an ABBA lock-order inversion
					// between the two mutexes.
					pthread_mutex_unlock(&p->locked.mtx);
					int anchor_delay_ms = stream_get_anchor_delay_ms(s, 1);
					pthread_mutex_lock(&p->locked.mtx);
					int hold_ms = anchor_delay_ms > 0 ? anchor_delay_ms + 200 : 500;
					int hold_start_ms = atime();
					p->hold_audio_start_ms = hold_start_ms;
					p->hold_audio_until_ms = hold_start_ms + hold_ms;
					p->hold_audio_applied_ms = 0;
					DBGSI serprintf("android_sync: hold video for passthrough start (%d ms)\n", hold_ms);
				}
				while (p->locked.run && !has_state_l(p, THREAD_STATE_FLUSHING) &&
				       s->audio_time < 0 && atime() < p->hold_audio_until_ms) {
					struct timespec ts_wait;
					clock_gettime(CLOCK_MONOTONIC, &ts_wait);
					timespec_add_ms(&ts_wait, 10);
					pthread_cond_timedwait(&p->locked.cond, &p->locked.mtx, &ts_wait);
				}
				if (!p->locked.run || has_state_l(p, THREAD_STATE_FLUSHING)) {
					// f was only peeked and still belongs to venc_q.
					f = NULL;
					goto endloop;
				} else if (s->audio_time >= 0) {
					int hold_end_ms = atime();
					if (p->hold_audio_start_ms > 0 && hold_end_ms > p->hold_audio_start_ms) {
						p->hold_audio_applied_ms = hold_end_ms - p->hold_audio_start_ms;
					} else {
						p->hold_audio_applied_ms = 0;
					}
					p->hold_audio_until_ms = 0;
					p->hold_audio_start_ms = 0;
					DBGSI serprintf("android_sync: passthrough hold done applied=%d ms\n",
						p->hold_audio_applied_ms);
				} else {
					// Timeout reached: keep video blocked until audio becomes available.
					hold_passthrough = 1;
				}
				// Audio time may have become valid while we were waiting.
				have_audio_time = (s && s->audio_time >= 0);
				now_ns = _get_monotonic_ns(); // re-read time after hold
			}

			if (hold_passthrough) {
				// Startup hold timeout: drop this frame immediately to avoid blocking composition queue
				f = frame_q_get(&p->locked.venc_q);
				consumed = 1;
				if (!f || !f->android_handle) {
					goto endloop;
				}
				p->dropped++;
				// Keep flush from invalidating the MediaCodec buffer while the
				// timeout path releases it outside the queue lock.
				add_state_l(p, THREAD_STATE_RENDERING);
				pthread_mutex_unlock(&p->locked.mtx);
				sfdec_buf_render(p->sfdec, (sfbuf_t *)f->android_handle, 0, 0, 0);
				pthread_mutex_lock(&p->locked.mtx);
				goto endloop;
			}

			if (p->render_offset_ns == -1) {
				// Anchor Initialization
				if (passthrough == 2 && have_audio_time) {
					// See lock-order note above: drop p->locked.mtx around the call.
					pthread_mutex_unlock(&p->locked.mtx);
					int delay_for_pt = stream_get_anchor_delay_ms(s, 1);
					pthread_mutex_lock(&p->locked.mtx);
					int max_forward_lead_ms = delay_for_pt + 300;
					if (max_forward_lead_ms < 500) {
						max_forward_lead_ms = 500;
					}
					int used_put_time = 0;
					int put_age_ms = 0;
					// Mode 2 writes arrive in bursts, so audio_time-delay is only the
					// accepted-buffer frontier. Before dynamic evidence is trusted, a
					// fresh audio-thread put_time remains the best startup phase. Once
					// dynamic, use the continuous centralized heard clock instead.
					int64_t heard_ts = _get_render_heard_ts(p, s,
						!mode2_dynamic_active, &used_put_time, &put_age_ms);
					if (p->pending_seek_reanchor && s && s->video_time > 0 && heard_ts > f->time) {
						// Backward seek: avoid anchoring behind the current video frame.
						heard_ts = f->time;
					}
					if (p->pending_seek_reanchor && s && heard_ts >= 0) {
						int64_t forward_lead = (int64_t)f->time - heard_ts;
						if (forward_lead > max_forward_lead_ms) {
							DBGSI serprintf("android_sync: clamp passthrough forward lead %lld -> %d ms (f=%d heard=%lld)\n",
								(long long)forward_lead, max_forward_lead_ms, f->time, (long long)heard_ts);
							heard_ts = (int64_t)f->time - max_forward_lead_ms;
						}
					}
					p->render_offset_ns = now_ns - heard_ts * 1000000LL;
					p->render_offset_from_audio = 1;
					DBGSI serprintf("android_sync: init render_offset from audio_time=%d heard_ts=%lld src=%s put_age=%d delay=%d passthrough=2 seek_reanchor=%d offset=%lld\n",
						s->audio_time, (long long)heard_ts,
						used_put_time ? "put_time" : "interpolated",
						put_age_ms, delay_for_pt, p->pending_seek_reanchor,
						(long long)p->render_offset_ns);
				} else if (have_audio_time) {
					int used_put_time = 0;
					int put_age_ms = 0;
					INT64 heard_ts = _get_render_heard_ts(p, s, 1,
						&used_put_time, &put_age_ms);
					if (p->pending_seek_reanchor && s && s->video_time > 0 && heard_ts > f->time) {
						// Backward seek: avoid anchoring behind the current video frame.
						heard_ts = f->time;
					}
					p->render_offset_ns = now_ns - heard_ts * 1000000LL;
					p->render_offset_from_audio = 1;
					// stream_get_anchor_delay_ms()/stream_sync_av_delay() can lock
					// s->video_sink_mutex; compute them (only when this debug print
					// actually fires) with p->locked.mtx dropped, see lock-order note
					// above. Guarded by the same condition as DBGSI2 so this has no
					// effect when the debug flag is off.
					int dbg_raw_delay = -1, dbg_smooth_delay = -1;
					DBGSI2 {
						pthread_mutex_unlock(&p->locked.mtx);
						dbg_raw_delay = s ? stream_get_anchor_delay_ms(s, 1) : -1;
						dbg_smooth_delay = s ? stream_sync_av_delay(s) : -1;
						pthread_mutex_lock(&p->locked.mtx);
					}
					DBGSI2 serprintf("android_sync anchor_diag(init): a_time=%d heard_ts=%lld src=%s put_ts=%d put_age=%d raw_delay=%d smooth_delay=%d off=%lld\n",
						s ? s->audio_time : -1, (long long)heard_ts,
						used_put_time ? "put_time" : "recompute",
						p->venc_put_time, put_age_ms,
						dbg_raw_delay,
						dbg_smooth_delay, (long long)p->render_offset_ns);
					DBGSI serprintf("android_sync: init render_offset from audio_time=%d heard_ts=%lld offset=%lld\n",
						s->audio_time, (long long)heard_ts, (long long)p->render_offset_ns);
				} else {
					// See lock-order note above: drop p->locked.mtx around the call.
					pthread_mutex_unlock(&p->locked.mtx);
					int anchor_delay_ms = s ? stream_get_anchor_delay_ms(s, 1) : 0;
					pthread_mutex_lock(&p->locked.mtx);
					p->render_offset_ns = now_ns + (INT64)anchor_delay_ms * 1000000LL - (INT64)f->time * 1000000LL;
					p->render_offset_from_audio = 0;
					DBGSI serprintf("android_sync: init render_offset static fallback=%d offset=%lld\n",
						anchor_delay_ms, (long long)p->render_offset_ns);
				}
				p->target_offset_ns = p->render_offset_ns;
				p->slew_active = 0;
				p->mode2_dynamic_slew = mode2_dynamic_active;
				p->mode2_dynamic_fast_slew = mode2_dynamic_active;
				p->mode2_dynamic_settle_frames = 0;
				p->mode2_slew_frame_handle = mode2_dynamic_active ?
					f->android_handle : NULL;
				p->mode2_slew_frame_time = mode2_dynamic_active ? f->time : INT_MIN;
				p->mode2_slew_frame_epoch = mode2_dynamic_active ? f->epoch : INT_MIN;
				p->pending_reanchor = 0;
				// A static fallback has not consumed the seek-specific audio
				// clamp; preserve it for the later audio-based reanchor.
				if( have_audio_time ) {
					p->pending_seek_reanchor = 0;
				}
			} else if (have_audio_time && p->render_offset_from_audio == 0) {
				// Audio time became valid after static init: re-anchor once to heard audio.
				int used_put_time = 0;
				int put_age_ms = 0;
				INT64 heard_ts = _get_render_heard_ts(p, s,
					!mode2_dynamic_active, &used_put_time, &put_age_ms);
				if (p->pending_seek_reanchor && s && s->video_time > 0 && heard_ts > f->time) {
					heard_ts = f->time;
				}
				p->render_offset_ns = now_ns - heard_ts * 1000000LL;
				p->render_offset_from_audio = 1;
				p->target_offset_ns = p->render_offset_ns;
				p->slew_active = 0;
				p->mode2_dynamic_slew = mode2_dynamic_active;
				p->mode2_dynamic_fast_slew = mode2_dynamic_active;
				p->mode2_dynamic_settle_frames = 0;
				p->pending_seek_reanchor = 0;
				{
					// See lock-order note above: compute with p->locked.mtx dropped,
					// only when this debug print actually fires.
					int dbg_raw_delay = -1, dbg_smooth_delay = -1;
					DBGSI2 {
						pthread_mutex_unlock(&p->locked.mtx);
						dbg_raw_delay = s ? stream_get_anchor_delay_ms(s, 1) : -1;
						dbg_smooth_delay = s ? stream_sync_av_delay(s) : -1;
						pthread_mutex_lock(&p->locked.mtx);
					}
					DBGSI2 serprintf("android_sync anchor_diag(reanchor): a_time=%d heard_ts=%lld src=%s put_ts=%d put_age=%d raw_delay=%d smooth_delay=%d off=%lld\n",
						s ? s->audio_time : -1, (long long)heard_ts,
						used_put_time ? "put_time" : "recompute",
						p->venc_put_time, put_age_ms,
						dbg_raw_delay,
						dbg_smooth_delay, (long long)p->render_offset_ns);
				}
				DBGSI serprintf("android_sync: reanchor from audio_time=%d heard_ts=%lld offset=%lld\n",
					s->audio_time, (long long)heard_ts, (long long)p->render_offset_ns);
			} else if (have_audio_time &&
				(passthrough != 2 || mode2_dynamic_active || mode2_dynamic_changed)) {
				// Slew toward a new anchor only on explicit events (seek/resume/speed/discontinuity)
				if (p->pending_reanchor ||
					(mode2_dynamic_active && mode2_new_slew_frame)) {
					int used_put_time = 0;
					int put_age_ms = 0;
					// Dynamic presentation is continuous; the cached put_time remains
					// quantized to compressed writes and would make this target oscillate.
					INT64 heard_ts = _get_render_heard_ts(p, s,
						!mode2_dynamic_active, &used_put_time, &put_age_ms);
					if (p->pending_seek_reanchor && s && s->video_time > 0 && heard_ts > f->time) {
						heard_ts = f->time;
					}
					p->target_offset_ns = now_ns - heard_ts * 1000000LL;
					// See lock-order note above: compute with p->locked.mtx dropped,
					// only when this debug print actually fires.
					int dbg_raw_delay = -1, dbg_smooth_delay = -1;
					DBGSI2 {
						pthread_mutex_unlock(&p->locked.mtx);
						dbg_raw_delay = s ? stream_get_anchor_delay_ms(s, 1) : -1;
						dbg_smooth_delay = s ? stream_sync_av_delay(s) : -1;
						pthread_mutex_lock(&p->locked.mtx);
					}
					DBGSI2 serprintf("android_sync anchor_diag(slew_target): a_time=%d heard_ts=%lld src=%s put_ts=%d put_age=%d raw_delay=%d smooth_delay=%d off=%lld target=%lld fast=%d settle=%d\n",
						s ? s->audio_time : -1, (long long)heard_ts,
						used_put_time ? "put_time" : "recompute",
						p->venc_put_time, put_age_ms,
						dbg_raw_delay,
						dbg_smooth_delay,
						(long long)p->render_offset_ns, (long long)p->target_offset_ns,
						p->mode2_dynamic_fast_slew,
						p->mode2_dynamic_settle_frames);
					// Timestamp sampling and millisecond clock quantization can leave
					// a small phase error after convergence. Do not turn that noise
					// into a recurring video cadence correction.
					// Converge a real transition closely, then tolerate less than one
					// compressed AC3 frame of steady-state phase noise. An 8ms steady
					// threshold made the renderer correct 0.2ms on hundreds of frames
					// after resume, producing a visible cadence beat every few seconds.
					const INT64 mode2_deadband_ns =
						p->mode2_dynamic_fast_slew ? 8000000LL : 24000000LL;
					if (mode2_dynamic_active &&
						llabs(p->target_offset_ns - p->render_offset_ns) <= mode2_deadband_ns) {
						p->target_offset_ns = p->render_offset_ns;
						p->slew_active = 0;
						if (p->mode2_dynamic_fast_slew) {
							// A single early timestamp can briefly look converged while
							// the presentation clock is still settling. Require a short
							// run of stable video samples before switching to the slow
							// steady-state correction rate.
							if (++p->mode2_dynamic_settle_frames >= 4) {
								p->mode2_dynamic_fast_slew = 0;
								p->mode2_dynamic_settle_frames = 0;
							}
						}
					} else if( p->pcm_startup_slew &&
						llabs(p->target_offset_ns - p->render_offset_ns) <= 8000000LL ) {
						// Do not turn millisecond timestamp noise into a recurring PCM
						// cadence adjustment once the cold-start phase is close enough.
						p->target_offset_ns = p->render_offset_ns;
						p->slew_active = 0;
						p->pcm_startup_slew = 0;
						p->pcm_startup_slew_frame_handle = NULL;
						p->pcm_startup_slew_frame_time = INT_MIN;
						p->pcm_startup_slew_frame_epoch = INT_MIN;
					} else if (p->target_offset_ns != p->render_offset_ns) {
						p->mode2_dynamic_settle_frames = 0;
						p->slew_active = 1;
					}
					p->pending_reanchor = 0;
					p->pending_seek_reanchor = 0;
				}
			}

			if (p->slew_active &&
				(!p->mode2_dynamic_slew || mode2_new_slew_frame) &&
				(!p->pcm_startup_slew || pcm_startup_new_slew_frame)) {
				INT64 delta = p->target_offset_ns - p->render_offset_ns;
				// A Mode 2 clock transition can correct hundreds of milliseconds, so
				// converge it at 5ms/frame. PCM startup uses 1ms per distinct frame to
				// remove its bounded residual without a snap. Once established, track
				// ordinary clock drift at 0.2ms/frame: compressed presentation samples
				// can move by a whole codec frame and then hold, and chasing that
				// temporary phase at 5ms/frame produces visible cadence reversals.
				INT64 step = p->mode2_dynamic_fast_slew ? 5000000 :
					(p->pcm_startup_slew ? 1000000 : 200000);
				if (delta > step) {
					delta = step;
				} else if (delta < -step) {
					delta = -step;
				}
				p->render_offset_ns += delta;
				if (llabs(p->target_offset_ns - p->render_offset_ns) <= step) {
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
					if( p->pcm_startup_slew ) {
						p->pcm_startup_slew = 0;
						p->pcm_startup_slew_frame_handle = NULL;
						p->pcm_startup_slew_frame_time = INT_MIN;
						p->pcm_startup_slew_frame_epoch = INT_MIN;
					}
				}
			}
			if( p->mode2_dynamic_slew && mode2_new_slew_frame ) {
				p->mode2_slew_frame_handle = f->android_handle;
				p->mode2_slew_frame_time = f->time;
				p->mode2_slew_frame_epoch = f->epoch;
			}
			if( p->pcm_startup_slew && pcm_startup_new_slew_frame ) {
				p->pcm_startup_slew_frame_handle = f->android_handle;
				p->pcm_startup_slew_frame_time = f->time;
				p->pcm_startup_slew_frame_epoch = f->epoch;
			}

			INT64 av_delay_ns = (INT64)RST_TO_TS_DELTA(p->effective_av_delay_ms, int) * 1000000LL;
			render_ts_ns = _snap_timestamp_ns(p, f->time, f->epoch) +
				p->render_offset_ns + av_delay_ns;
			INT64 delta_ns = render_ts_ns - now_ns;
			const INT64 k_max_lookahead_ns = 200 * 1000000LL; // 200ms lookahead

			if (p->render_offset_ns != -1 && delta_ns > k_max_lookahead_ns) {
				// Frame is too far in the future; timed-wait until it enters the safe lookahead window
				int64_t wait_ms = (delta_ns - k_max_lookahead_ns) / 1000000LL;
				if (wait_ms > f->duration + 100) wait_ms = f->duration + 100;
				if (wait_ms < 2) wait_ms = 2; // avoid spinning

				struct timespec ts_wait;
				clock_gettime(CLOCK_MONOTONIC, &ts_wait);
				timespec_add_ms(&ts_wait, wait_ms);

				pthread_cond_timedwait(&p->locked.cond, &p->locked.mtx, &ts_wait);
				continue;
			}

			// Target render window reached; consume it from queue
			f = frame_q_get(&p->locked.venc_q);
			consumed = 1;

			if( s ) {
				av_delay_ns = (INT64)_update_effective_av_delay_ts( p, s ) * 1000000LL;
			}
			render_ts_ns = _snap_timestamp_ns(p, f->time, f->epoch) +
				p->render_offset_ns + av_delay_ns;
			break;
		}
		if (!consumed) {
			f = NULL;
		}
		add_state_l(p, THREAD_STATE_RENDERING);

		if (!p->locked.run || !f || !f->android_handle) {
			goto endloop;
		}

		int do_render = 1;
		if( !p->locked.run || has_state_l(p, THREAD_STATE_FLUSHING)) {
			do_render = 0;
		}

		pthread_mutex_unlock(&p->locked.mtx);

		if (do_render && !stale_epoch_drop) {
			INT64 now_ns = _get_monotonic_ns();
			INT64 lateness_ns = now_ns - render_ts_ns;
			const INT64 k_late_drop_threshold_ns = 200 * 1000000LL; // 200ms lateness drop limit

			if (lateness_ns > k_late_drop_threshold_ns) {
				// Drop/release buffer immediately without rendering to catch up
				sfdec_buf_render(p->sfdec, (sfbuf_t *)f->android_handle, 0, 0, 0);
				p->dropped++;
				DBGSI serprintf("android_sync: late frame drop f_time=%d lateness=%lldms dropped=%d\n",
					f->time, lateness_ns / 1000000LL, p->dropped);
			} else {
				DBGCV3 CLOG("render ->");
				int start = time_update_time();
				// Timed rendering (asap=0) using render_ts_ns computed under the lock
				sfdec_buf_render(p->sfdec, (sfbuf_t *)f->android_handle, 1, 0, render_ts_ns);
				int took = time_update_time() - start;
				p->dropped = 0;
				DBGCV CLOG("\t\t\t\t\t\t\trender %8d/%8d  took %3d", f->time, f->blit_time, took );
				DBGCV3 CLOG("render <-");
			}
		} else {
			sfdec_buf_render(p->sfdec, (sfbuf_t *)f->android_handle, 0, 0, 0);
		}

		pthread_mutex_lock(&p->locked.mtx);
	endloop:
		if (f) {
			frame_q_put(&p->locked.get_q, f);
		}
	}
	rm_state_l(p, THREAD_STATE_RENDERING);
	pthread_mutex_unlock(&p->locked.mtx);
	CLOG("terminated");
	return NULL;
}

static void apply_rotation(priv_t *p, int rotation, int32_t width, int32_t height, int32_t *out_width, int32_t *out_height) {

	if (android_window_set_buffers_rotation(p->surface_handle, rotation) != 0)
		CLOG("android_window_set_buffers_rotation failed");

	if (rotation == 90 || rotation == 270) {
		*out_width = height;
		*out_height = width;
	} else {
		*out_width = width;
		*out_height = height;
	}
}

static void *videodec_thread(void *ctx)
{
	priv_t *p = (priv_t*) ctx;
	sfdec_read_out_t read_out;

	pthread_mutex_lock(&p->locked.mtx);

	while (p->locked.run && !p->locked.error) {
		int time = -1;

		sfbuf_t *sfbuf = NULL;
		VIDEO_FRAME *f = NULL;
		// decode frame

		// NOTE: run/error must gate the whole wait, not just the "no frame yet"
		// branch: videodec_close() sets FLUSHING together with run=0 and never
		// clears FLUSHING again before joining this thread. If this thread is
		// parked here (e.g. stopped right away, before any frame arrived), a
		// loop condition of the form "FLUSHING || (run && ...)" would keep
		// waiting forever on FLUSHING alone, since nothing broadcasts the cond
		// again once run drops to 0 - deadlocking videodec_close()'s
		// pthread_join() forever (ANR). Requiring run/!error unconditionally
		// lets the close path fall through here with f==NULL, be caught by the
		// "!f" check below, and unwind via the outer run-checked loop.
		while (p->locked.run && !p->locked.error &&
		       (has_state_l(p, THREAD_STATE_FLUSHING) || !(f = frame_q_get(&p->locked.dec_q)))) {
			rm_state_l(p, THREAD_STATE_READING);
			pthread_cond_wait(&p->locked.cond, &p->locked.mtx);
		}
		add_state_l(p, THREAD_STATE_READING);

		if (!f) {
DBGCV CLOG("stop thread");
			continue;
		}

		pthread_mutex_unlock(&p->locked.mtx);

		frame_release(p->sfdec, f);

DBGCV3 CLOG("sfdec_read, ->");
		static int last;
		int start = time_update_time();
		int wait  = start - last;
		     last = start;

		int ret = sfdec_read(p->sfdec, -1, &read_out);

		int took = time_update_time() - start;

		pthread_mutex_lock(&p->locked.mtx);

		if (ret == -1) {
			p->locked.error = 1;
DBGCV3 CLOG("sfdec_read <- error");
		}
		if (read_out.flag & SFDEC_READ_INVALID) {
DBGCV3 CLOG("sfdec_read <- invalid");
		}
		if (read_out.flag & SFDEC_READ_BUF) {
			sfbuf = read_out.buf.sfbuf;
			time = read_out.buf.time_us / 1000;
DBGCV3 CLOG("sfdec_read <- sfbuf: %p, time_ms: %8d", sfbuf, time);
		}
		if (read_out.flag & SFDEC_READ_SIZE) {
			if (read_out.size.width >= 0 && read_out.size.height >= 0) {
				apply_rotation(p, p->locked.rotation,
					read_out.size.width, read_out.size.height,
					&p->locked.width, &p->locked.height);
				p->locked.interlaced = read_out.size.interlaced;
			}
DBGCV3 CLOG("sfdec_read <- size %dx%d (%d)", read_out.size.width, read_out.size.height, read_out.size.interlaced);
		}

		if (!sfbuf || has_state_l(p, THREAD_STATE_FLUSHING)) {
			if( sfbuf ) {
				sfdec_buf_discard(p->sfdec, sfbuf);
			}
			frame_q_put_head(&p->locked.dec_q, f);
			continue;
		}
		int out_time;
		int out_type;
		int out_ID;

		int fifo_time = -1;
		if( p->repair_decode_order_pts ) {
			fifo_time = XDM_ts_get( &p->XDM_ctx );
		}
		if( p->repair_decode_order_pts &&
		    p->pts_input_seen > p->pts_reorder_depth &&
		    p->pts_input_monotonic && fifo_time != -1 ) {
			out_time = fifo_time;
			if( !p->pts_repair_logged ) {
				CLOG("decode-order PTS repair active: depth=%d seen=%d decoder_ts=%d fifo_ts=%d",
					p->pts_reorder_depth,
					p->pts_input_seen, time, fifo_time);
				p->pts_repair_logged = 1;
			}
		} else if( p->reorder_pts ) {
			// get reordered TS
			out_time = time;
		} else {
			// no reordering, just use the ts_ queue
			out_time = p->repair_decode_order_pts ? fifo_time : XDM_ts_get( &p->XDM_ctx );
		}
		ret = XDM_id_get( &p->XDM_ctx, time, &out_type, &out_ID );

		f->time    = out_time;
		f->user_ID = out_ID;
		f->type    = out_type;

		f->width      = p->locked.width;
		f->height     = p->locked.height;
		f->interlaced = p->locked.interlaced;

		f->android_handle = sfbuf;
DBGCV CLOG("\t\t\tout %8d/%8d  tim %3d  wait %3d", time, f->time, took, wait );
		frame_q_put(&p->locked.out_q, f);
	}
	rm_state_l(p, THREAD_STATE_READING);
	pthread_mutex_unlock(&p->locked.mtx);
	CLOG("terminated");
	return NULL;
}

static int videodec_open(STREAM_DEC_VIDEO *dec, VIDEO_PROPERTIES *video, void *ctx, int *pneed_flush, int *pneed_reorder)
{
	priv_t *p = (priv_t *) dec->priv;
	sfdec_codec_t sfdec_codec;
	sfdec_flags_t flags = 0;
	int width, height;
	void *extradata = NULL;
	size_t extradata_size = 0;
	int pts_reorder = 0;
	int input_size = -1;
	CLOG("open: format=%d %dx%d subfmt=%d profile=%d level=%d rotation=%d",
		video ? video->format : -1,
		video ? video->width : -1,
		video ? video->height : -1,
		video ? video->subfmt : -1,
		video ? video->profile : -1,
		video ? video->level : -1,
		video ? video->rotation : -1);

	int hw_type = device_get_hw_type();
	if (video->format == VIDEO_FORMAT_H264 && video->sps.valid && video->profile >= H264_PROFILE_HIGH10) {
		// Do not hard-block Hi10 here: some devices can still decode via MediaCodec.
		// If decoder instantiation/start fails, stream_open_video_dec will fall back.
		CLOG("Hi10P input detected (profile=%d): try sfdec2 and fallback on runtime failure", video->profile);
	}

	dec->ctx = ctx;
	p->dec = dec;
	p->video_frame_rate_num = video->frame_rate_num;
	p->video_frame_rate_den = video->frame_rate_den;
	p->playback_speed_num = 100;
	p->playback_speed_den = 100;
	p->sched_start_off_ns = 0;
	p->sched_start_mono_ns = 0;
	p->sched_last_off_ns = 0;
	p->sched_last_mono_ns = 0;
	p->sched_late = 0;
	p->sched_debt_ns = 0;

	p->reorder_pts = video->reorder_pts;

	if (hw_type == HW_TYPE_OMAP4 || hw_type == HW_TYPE_ARCHOS_OMAP4) {
		/*
		 * HACK:
		 * On omap4, sfdec send an error with num_frames > 32
		 * sfdec can't be recovered from that error, so don't try it.
		 */
		int num_frames;
		int ref_by_sps = video->sps.num_ref_frames;
		int ref_by_res = (video->width && video->height)
			? MIN(16, 32768 / ((video->width / 16) * (video->height / 16)))
			: 16;

		if( ref_by_sps ) {
			num_frames = MAX( ref_by_res, 2 * ref_by_sps) + 3;
		} else {
			num_frames = ref_by_res * 2;
		}
		if (num_frames > 32 /* NUM_BUFFER_SLOTS */) {
			CLOG("too much frames for OMXCodec/MediaCodec on TI: abort");
			return 1;
		}
	}

	if (video->extraDataSize) {
		extradata      = video->extraData;
		extradata_size = video->extraDataSize;
	} else if (video->extraData2 && video->extraDataSize2 && video->extraData2) {
		extradata      = video->extraData2;
		extradata_size = video->extraDataSize2;
	}
	if (video->format == VIDEO_FORMAT_WMV3 || video->format == VIDEO_FORMAT_VC1) {
                int rcv_size = 0;
                extradata      = WMV_get_rcv_header(video, &rcv_size);
                extradata_size = rcv_size;
	}

	int effective_format = video->format;
	int effective_fourcc = video->fourcc;
	int tried_hevc_fallback = 0;

retry_decoder_open:
	switch (effective_format) {
		case VIDEO_FORMAT_DOLBY_VISION:
			sfdec_codec = SFDEC_VIDEO_DOLBY_VISION;
			break;
		case VIDEO_FORMAT_H264:
			sfdec_codec = SFDEC_VIDEO_AVC;
			break;
		case VIDEO_FORMAT_HEVC:
			sfdec_codec = SFDEC_VIDEO_HEVC;
			break;
		case VIDEO_FORMAT_MPG4:
			sfdec_codec = SFDEC_VIDEO_MPEG4;
			break;
		case VIDEO_FORMAT_H263:
			sfdec_codec = SFDEC_VIDEO_H263;
			break;
		case VIDEO_FORMAT_VP6:
		case VIDEO_FORMAT_VP7:
		case VIDEO_FORMAT_VP8:
			sfdec_codec = SFDEC_VIDEO_VP8;
			break;
		case VIDEO_FORMAT_VP9:
			sfdec_codec = SFDEC_VIDEO_VP9;
			break;
		case VIDEO_FORMAT_AV1:
			sfdec_codec = SFDEC_VIDEO_AV1;
			break;
		case VIDEO_FORMAT_MPEG:
			sfdec_codec = SFDEC_VIDEO_MPEG2;
			break;
		case VIDEO_FORMAT_WMV1:
		case VIDEO_FORMAT_WMV2:
		case VIDEO_FORMAT_WMV3:
		case VIDEO_FORMAT_WMV3B:
		case VIDEO_FORMAT_VC1:
			sfdec_codec = SFDEC_VIDEO_WMV;
			break;
		default:
			CLOG("open reject: unsupported format=%d subfmt=%d profile=%d", effective_format, video->subfmt, video->profile);
			return 1;
		}

	// force AVCDecoder usage, better perf than rk OMX decoder
	if ((hw_type == HW_TYPE_RK30 ||
	     hw_type == HW_TYPE_RK29) && video->rotation == 0)
		flags |= SFDEC_FLAG_SWDEC;

	if (ctx)
		p->surface_handle = stream_get_surface_handle((STREAM *)ctx);

	if (sfdec_force_hw == 0)
		flags |= SFDEC_FLAG_SWDEC;

    const char *decoder_name = NULL;
    if (effective_format == VIDEO_FORMAT_DOLBY_VISION) {
		DBGCV2 serprintf("dovi profile dtr %s\n", acodecs_get_for_profile("video/dolby-vision", 16));
		DBGCV2 serprintf("dovi profile dth %s\n", acodecs_get_for_profile("video/dolby-vision", 64));
		DBGCV2 serprintf("dovi profile st %s\n", acodecs_get_for_profile("video/dolby-vision", 256));
		DBGCV2 serprintf("dovi profile stn %s\n", acodecs_get_for_profile("video/dolby-vision", 32));
		DBGCV2 serprintf("dovi profile dtb %s\n", acodecs_get_for_profile("video/dolby-vision", 128));
		DBGCV2 serprintf("dovi profile der %s\n", acodecs_get_for_profile("video/dolby-vision", 4));
		DBGCV2 serprintf("dovi profile den %s\n", acodecs_get_for_profile("video/dolby-vision", 8));
		DBGCV serprintf("dovi profile myself %s\n", acodecs_get_for_profile("video/dolby-vision", video->dv_profile));
        decoder_name = acodecs_get_for_profile("video/dolby-vision", video->dv_profile);
		serprintf("Dolby Vision decoder selection: requested_profile=%d resolved_decoder=%s hdr_primaries=%d hdr_trc=%d hdr_space=%d hdr_range=%d\n",
		          video->dv_profile,
		          decoder_name ? decoder_name : "(default)",
		          video->color_primaries,
		          video->color_trc,
		          video->color_space,
		          video->color_range);
    }

	width = video->width;
	height = video->height;
	p->sfdec = sfdec_new(SFDEC_TYPE_MEDIACODEC,
			sfdec_codec,
			flags,
			&width, &height, video->rotation,
			video->duration * 1000, input_size,
			p->surface_handle,
			extradata, extradata_size,
			&pts_reorder, decoder_name, video->frame_rate_den, video->frame_rate_num,
			video->color_primaries, video->color_trc, video->color_space, video->color_range);
	apply_rotation(p, video->rotation, width, height, &width, &height);

	if (!p->sfdec) {
		CLOG("sfdec_new failed codec=%d flags=0x%x decoder_name=%s w=%d h=%d",
			sfdec_codec, flags, decoder_name ? decoder_name : "(default)", width, height);
		if (effective_format == VIDEO_FORMAT_DOLBY_VISION) {
			serprintf("Dolby Vision decoder init failed: requested_profile=%d resolved_decoder=%s codec=%d flags=0x%x\n",
			          video->dv_profile,
			          decoder_name ? decoder_name : "(default)",
			          sfdec_codec,
			          flags);
			if (!tried_hevc_fallback) {
				tried_hevc_fallback = 1;
				effective_format = VIDEO_FORMAT_HEVC;
				effective_fourcc = VIDEO_FOURCC_HEVC;
				serprintf("Dolby Vision fallback retry: reopening as HEVC after decoder init failure\n");
				goto retry_decoder_open;
			}
		}
		goto err;
	}

	if (p->reorder_pts && (pts_force_reorder || pts_reorder)) {
		STREAM *s = dec->ctx;
		if( s && s->ro_ctx) {
			CLOG("pts_reorder");
			pts_ro_run(s->ro_ctx);
			p->reorder_pts = 0;
		}
	}
	// Some malformed HEVC streams declare B-frame reordering but carry decode-order
	// PTS (missing composition offsets). Preserve submitted timestamps in a FIFO so
	// they can replace non-monotonic MediaCodec output timestamps for that case.
	// Correctly muxed B-frame input becomes non-monotonic and keeps decoder PTS.
	p->pts_reorder_depth = video->reorder_depth;
	p->repair_decode_order_pts = effective_format == VIDEO_FORMAT_HEVC &&
		p->pts_reorder_depth > 0;
	p->pts_input_monotonic = 1;
	p->pts_input_seen = 0;
	p->pts_input_last = INT_MIN;
	p->pts_repair_logged = 0;
	if( p->repair_decode_order_pts ) {
		CLOG("decode-order PTS repair armed: depth=%d", p->pts_reorder_depth);
	}

	if (sfdec_start(p->sfdec) != 0) {
		CLOG("sfdec_start failed codec=%d flags=0x%x decoder_name=%s",
			sfdec_codec, flags, decoder_name ? decoder_name : "(default)");
		if (effective_format == VIDEO_FORMAT_DOLBY_VISION && !tried_hevc_fallback) {
			tried_hevc_fallback = 1;
			effective_format = VIDEO_FORMAT_HEVC;
			effective_fourcc = VIDEO_FOURCC_HEVC;
			serprintf("Dolby Vision fallback retry: reopening as HEVC after decoder start failure\n");
			sfdec_delete(p->sfdec);
			p->sfdec = NULL;
			goto retry_decoder_open;
		}
		goto err;
	}

	int requested_frames = sfdec_max_frames;
	if( p->repair_decode_order_pts ) {
		// The read thread holds one frame wrapper while MediaCodec waits for
		// enough input to satisfy its reorder depth. Keep additional wrappers
		// available for those inputs or high-depth HEVC can deadlock before its
		// first output buffer is produced.
		requested_frames = MAX( requested_frames, p->pts_reorder_depth + 2 );
	}
	p->num_frames = MIN( requested_frames, SFDEC_MAX_FRAMES );
	if( p->num_frames != sfdec_max_frames ) {
		CLOG("frame pool expanded: configured=%d reorder_depth=%d allocated=%d",
			sfdec_max_frames, p->pts_reorder_depth, p->num_frames);
	}
	if (stream_alloc_frames( &p->frames, video->width, video->height, video->colorspace, STREAM_MEM_ANDROID, &p->num_frames) != 0) {
		CLOG("stream_alloc_frames failed");
		goto err;
	}
	frame_q_init(&p->locked.dec_q, "dec_q");
	frame_q_init(&p->locked.out_q, "out_q");
	frame_q_init(&p->locked.venc_q, "venc_q");
	frame_q_init(&p->locked.get_q, "get_q");

	pthread_mutex_init(&p->locked.mtx, NULL);
	pthread_condattr_t cond_attr;
	pthread_condattr_init(&cond_attr);
#ifndef __APPLE__
	pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
#endif
	pthread_cond_init(&p->locked.cond, &cond_attr);
	pthread_condattr_destroy(&cond_attr);
	p->locked.width = width;
	p->locked.height = height;
	p->locked.rotation = video->rotation;
	p->locked.run = 1;
	p->prev_paused = 0;
	p->pause_start_ms = 0;
	p->pause_armed = 0;
	p->render_offset_ns = -1;
	p->slew_active = 0;
	p->mode2_dynamic_fast_slew = 0;
	p->mode2_dynamic_settle_frames = 0;
	p->mode2_slew_frame_handle = NULL;
	p->mode2_slew_frame_time = INT_MIN;
	p->mode2_slew_frame_epoch = INT_MIN;
	p->pcm_startup_slew = 0;
	p->pcm_startup_slew_frame_handle = NULL;
	p->pcm_startup_slew_frame_time = INT_MIN;
	p->pcm_startup_slew_frame_epoch = INT_MIN;
	p->target_offset_ns = 0;
	p->pending_reanchor = 0;
	p->pending_seek_reanchor = 0;
	p->last_seek_epoch = 0;
	p->last_audio_resume_pending = 0;
	p->snap_origin_time = 0;
	p->snap_origin_epoch = INT_MIN;
	p->hold_audio_until_ms = 0;
	p->hold_audio_start_ms = 0;
	p->hold_audio_applied_ms = 0;
	STREAM *sctx = (STREAM *)dec->ctx;
	p->passthrough_cached = _is_passthrough(sctx);
	p->grace_until_ms = 0;
	p->last_user_av_delay = sctx ? sctx->av_delay : 0;
	p->effective_av_delay_ms = (sctx && sctx->av_delay > 0) ? sctx->av_delay : 0;

	VIDEO_PROPERTIES opened_video = *video;
	opened_video.format = effective_format;
	opened_video.fourcc = effective_fourcc;
	opened_video.colorspace = AV_IMAGE_HW;
	dec->video = &dec->_video;
	memcpy(dec->video, &opened_video, sizeof(VIDEO_PROPERTIES));

	XDM_id_flush( &p->XDM_ctx );
	XDM_ts_flush( &p->XDM_ctx );

	dec->is_open = 1;

	pthread_create(&p->dec_thread, 0, videodec_thread, p);
	pthread_create(&p->sink_thread, 0, videosink_thread, p);

	if( pneed_flush )
		*pneed_flush = 1;
	if( pneed_reorder )
		*pneed_reorder = 0;

	return 0;
err:
	CLOG("open failed: format=%d %dx%d subfmt=%d profile=%d", video->format, video->width, video->height, video->subfmt, video->profile);
	if (p->sfdec) {
		sfdec_delete(p->sfdec);
		p->sfdec = NULL;
	}
	stream_free_frames(&(p->frames), p->num_frames);
	return 1;
}

static int videodec_close(STREAM_DEC_VIDEO *dec)
{
	if (!dec->is_open)
		return 1;
	CLOG();
	if (dec->priv) {
		int i;
		priv_t *p = (priv_t *) dec->priv;

DBGCV CLOG("sfdec_stop_input");
		sfdec_stop_input(p->sfdec);

DBGCV CLOG("stop thread");
		pthread_mutex_lock(&p->locked.mtx);
		p->locked.run = 0;
		add_state_l(p, THREAD_STATE_FLUSHING);

		pthread_cond_broadcast(&p->locked.cond);
		while (p->locked.state & (THREAD_STATE_READING|THREAD_STATE_WRITING|THREAD_STATE_RENDERING)) {
			pthread_cond_wait(&p->locked.cond, &p->locked.mtx);
		}
		pthread_mutex_unlock(&p->locked.mtx);

		int codec_flushed = sfdec_flush(p->sfdec) == 0;

		pthread_join(p->dec_thread, NULL);
		pthread_join(p->sink_thread, NULL);

DBGCV CLOG("stop thread done");

		for (i = 0; i < p->num_frames; ++i) {
			if( codec_flushed ) {
				frame_discard_after_flush(p->sfdec, p->frames[i]);
			} else {
				frame_release(p->sfdec, p->frames[i]);
			}
		}

		sfdec_stop(p->sfdec);

		sfdec_delete(p->sfdec);
		p->sfdec = NULL;

		pthread_mutex_destroy(&p->locked.mtx);
		pthread_cond_destroy(&p->locked.cond);

		dec->is_open = 0;

		stream_free_frames(&(p->frames), p->num_frames);
		p->num_frames = 0;
	}

	STREAM *s = dec->ctx;
	if( s && s->ro_ctx) {
		pts_ro_stop(s->ro_ctx);
	}

 	return 0;
}

static int videodec_dec_in(STREAM_DEC_VIDEO *dec, VIDEO_FRAME **data_frame, int *pdecoded, int *ptime)
{
	priv_t *p = (priv_t *) dec->priv;
	VIDEO_FRAME *d = *data_frame;
	ssize_t ret;
	int error;

	if (pdecoded)
		*pdecoded = 0;

	if (ptime)
		*ptime = 0;

	error = videodec_get_error(p);
	if (error) {
CLOG("error!");
		return error;
	}

	pthread_mutex_lock(&p->locked.mtx);
	if( !p->locked.run || has_state_l(p, THREAD_STATE_FLUSHING) ) {
		pthread_mutex_unlock(&p->locked.mtx);
		return 0;
	}
	add_state_l(p, THREAD_STATE_WRITING);
	pthread_mutex_unlock(&p->locked.mtx);

	ret = sfdec_send_input(p->sfdec, d->data[0], d->size, (int64_t)d->time * 1000, d->type == I_VOP ? 1 : 0, 0);
	pthread_mutex_lock(&p->locked.mtx);
	if( ret > 0 ) {
DBGCV CLOG("%c %8d: %d/%d", frame_type(d->type), d->time, ret, d->size);
		XDM_id_put( &p->XDM_ctx,  d->time, d->type, d->user_ID );
		if( p->repair_decode_order_pts ) {
			if( p->pts_input_seen > 0 && d->time < p->pts_input_last ) {
				p->pts_input_monotonic = 0;
			}
			p->pts_input_last = d->time;
			p->pts_input_seen++;
		}
		if( !p->reorder_pts || p->repair_decode_order_pts ) {
			XDM_ts_put( &p->XDM_ctx, d->time );
		}
	}
	rm_state_l(p, THREAD_STATE_WRITING);
	pthread_mutex_unlock(&p->locked.mtx);
	if (pdecoded) {
		*pdecoded = ret > 0 ? ret : 0;
	}
//CLOG("<- decoded: %d", ret);
	return 0;
}

static int videodec_put_out(STREAM_DEC_VIDEO *dec, VIDEO_FRAME **pin_frame)
{
	priv_t *p = (priv_t*)dec->priv;
	int error;

	error = videodec_get_error(p);
	if (error)
		return error;
	if (pin_frame) {
		pthread_mutex_lock(&p->locked.mtx);
		frame_q_put(&p->locked.dec_q, *pin_frame);
		pthread_cond_broadcast(&p->locked.cond);
		pthread_mutex_unlock(&p->locked.mtx);
		*pin_frame = NULL;
	}

	return 0;
}

static int videodec_get_out(STREAM_DEC_VIDEO *dec, VIDEO_FRAME **pout_frame)
{
	priv_t *p = (priv_t*)dec->priv;
	int error;

	error = videodec_get_error(p);
	if (error)
		return error;
	pthread_mutex_lock(&p->locked.mtx);
	*pout_frame = frame_q_get(&p->locked.out_q);
	pthread_mutex_unlock(&p->locked.mtx);
	if (*pout_frame)
		(*pout_frame)->valid = 1;
	return 0;
}

static int videodec_flush(STREAM_DEC_VIDEO *dec)
{
	int i;
	priv_t *p = (priv_t*)dec->priv;

DBGCV	CLOG();

	pthread_mutex_lock(&p->locked.mtx);
	p->render_offset_ns = -1;
	p->slew_active = 0;
	p->mode2_dynamic_fast_slew = 0;
	p->mode2_dynamic_settle_frames = 0;
	p->mode2_slew_frame_handle = NULL;
	p->mode2_slew_frame_time = INT_MIN;
	p->mode2_slew_frame_epoch = INT_MIN;
	p->pcm_startup_slew = 0;
	p->pcm_startup_slew_frame_handle = NULL;
	p->pcm_startup_slew_frame_time = INT_MIN;
	p->pcm_startup_slew_frame_epoch = INT_MIN;
	p->target_offset_ns = 0;
	p->pending_reanchor = 0;
	p->pending_seek_reanchor = 0;
	p->last_seek_epoch = 0;
	p->last_audio_resume_pending = 0;
	p->snap_origin_time = 0;
	p->snap_origin_epoch = INT_MIN;
	p->hold_audio_until_ms = 0;
	p->hold_audio_start_ms = 0;
	p->hold_audio_applied_ms = 0;

	add_state_l(p, THREAD_STATE_FLUSHING);
	while (p->locked.state & (THREAD_STATE_READING|THREAD_STATE_WRITING|THREAD_STATE_RENDERING)) {
		pthread_cond_wait(&p->locked.cond, &p->locked.mtx);
	}

	XDM_id_flush( &p->XDM_ctx );
	XDM_ts_flush( &p->XDM_ctx );
	p->pts_input_monotonic = 1;
	p->pts_input_seen = 0;
	p->pts_input_last = INT_MIN;
	p->pts_repair_logged = 0;
	int codec_flushed = sfdec_flush(p->sfdec) == 0;
	sfdec_seek_reset( p->sfdec );
DBGCV CLOG("MediaCodec seek reset");

	for (i = 0; i < p->num_frames; ++i) {
		if( codec_flushed ) {
			frame_discard_after_flush(p->sfdec, p->frames[i]);
		} else {
			frame_release(p->sfdec, p->frames[i]);
		}
	}

	rm_state_l(p, THREAD_STATE_FLUSHING);

	pthread_mutex_unlock(&p->locked.mtx);

	return 0;
}

static int videodec_get_rc(STREAM_DEC_VIDEO *dec, STREAM_RC *rc)
{
	if( !rc )
		return 1;
	priv_t *p = (priv_t*)dec->priv;

	memset(rc, 0, sizeof(STREAM_RC));
	
	rc->num_frames = p->num_frames;

	rc->cpu_type   = SFDEC_MEDIACODEC;
	return 0;
}

static STREAM_SINK_VIDEO *videodec_get_sink(STREAM_DEC_VIDEO *dec)
{
	priv_t *p = (priv_t*)dec->priv;

	return videosink_new(p);
}

static int videodec_destroy(STREAM_DEC_VIDEO *dec)
{
	if (dec) {
		afree(dec->priv);
		afree(dec);
	}
	return 0;
} 

static int videodec_set_playback_speed(struct STREAM_DEC_VIDEO *dec, int den, int num) {
	priv_t *p = (priv_t*)dec->priv;
	int changed = 0;
	pthread_mutex_lock(&p->locked.mtx);
	if( den && den != p->playback_speed_den ) {
		p->playback_speed_den = den;
		changed = 1;
	}
	if( num && num != p->playback_speed_num ) {
		p->playback_speed_num = num;
		changed = 1;
	}
	if( changed ) {
		p->snap_origin_time = 0;
		p->snap_origin_epoch = INT_MIN;
	}
	pthread_mutex_unlock(&p->locked.mtx);

	int rc = sfdec_set_playback_speed(p->sfdec, den, num);
	DBGSI serprintf("sfdec2: set_playback_speed den=%d num=%d rc=%d snap_reset=%d\n",
		den, num, rc, changed);
	return rc;
}


static STREAM_DEC_VIDEO *new_dec(void)
{ 
	static char name[] = "sfdec2";
	STREAM_DEC_VIDEO *dec = (STREAM_DEC_VIDEO *)acalloc(1, sizeof(STREAM_DEC_VIDEO));
	
	if (!dec)
		return NULL;

	dec->name	= name;
	dec->destroy	= videodec_destroy;
	dec->open	= videodec_open;
	dec->close	= videodec_close;
	dec->dec_in	= videodec_dec_in;
	dec->put_out	= videodec_put_out;
	dec->get_out	= videodec_get_out;
	dec->flush	= videodec_flush;
	dec->get_rc	= videodec_get_rc;
	dec->get_sink	= videodec_get_sink;
	dec->async	= 1;
    dec->set_playback_speed = videodec_set_playback_speed;

	if (!(dec->priv = acalloc(1, sizeof(priv_t)))) {
		CLOG("cannot alloc priv");
		afree(dec);
		return NULL;
	}

	return dec;
}

void sfdec2_reset_sync_state_on_seek( STREAM *s )
{
	if( !s || !s->video_sink || !s->video_sink->priv )
		return;
	if( !s->video_sink->name || strcmp( s->video_sink->name, "sfdec2" ) != 0 )
		return;

	priv_t *p = (priv_t*) s->video_sink->priv;
	pthread_mutex_lock( &p->locked.mtx );
	p->venc_put_time = 0;
	p->venc_ref_time = 0;
	p->sched_start_off_ns = 0;
	p->sched_start_mono_ns = 0;
	p->sched_last_off_ns = 0;
	p->sched_last_mono_ns = 0;
	p->sched_late = 0;
	p->sched_debt_ns = 0;

	// Reset android_sync timeline offsets
	p->render_offset_ns = -1;
	p->render_offset_from_audio = 0;
	p->slew_active = 0;
	p->mode2_dynamic_fast_slew = 0;
	p->mode2_dynamic_settle_frames = 0;
	p->mode2_slew_frame_handle = NULL;
	p->mode2_slew_frame_time = INT_MIN;
	p->mode2_slew_frame_epoch = INT_MIN;
	p->pcm_startup_slew = 0;
	p->pcm_startup_slew_frame_handle = NULL;
	p->pcm_startup_slew_frame_time = INT_MIN;
	p->pcm_startup_slew_frame_epoch = INT_MIN;
	p->target_offset_ns = 0;
	p->pending_reanchor = 0;
	p->pending_seek_reanchor = 1;
	p->last_seek_epoch = 0;
	p->last_audio_resume_pending = 0;
	p->snap_origin_time = 0;
	p->snap_origin_epoch = INT_MIN;
	p->grace_until_ms = 0;
	p->hold_audio_until_ms = 0;
	p->hold_audio_start_ms = 0;
	p->hold_audio_applied_ms = 0;

	p->last_user_av_delay = s->av_delay;
	p->effective_av_delay_ms = s->av_delay > 0 ? s->av_delay : 0;
	pthread_mutex_unlock( &p->locked.mtx );
}

void sfdec2_android_sync_on_pause( STREAM *s, int paused )
{
	if( !s || !s->video_sink || !s->video_sink->priv )
		return;
	if( !s->video_sink->name || strcmp( s->video_sink->name, "sfdec2" ) != 0 )
		return;
	// The sink's private state is owned by the decoder. An open-error cleanup
	// must not let a stale sink reach this hook after that decoder was destroyed.
	if( !s->video_dec || !s->video_dec->is_open ||
	    !s->video_dec->name || strcmp( s->video_dec->name, "sfdec2" ) != 0 ||
	    s->video_dec->priv != s->video_sink->priv )
		return;

	priv_t *p = (priv_t*) s->video_sink->priv;
	pthread_mutex_lock( &p->locked.mtx );
	if( paused ) {
		p->pcm_startup_slew = 0;
		p->pcm_startup_slew_frame_handle = NULL;
		p->pcm_startup_slew_frame_time = INT_MIN;
		p->pcm_startup_slew_frame_epoch = INT_MIN;
		int active = (s->audio_time > 0 || s->video_time > 0);
		if( !active ) {
			p->pause_start_ms = 0;
			p->pause_armed = 0;
			pthread_mutex_unlock( &p->locked.mtx );
			return;
		}
		p->pause_start_ms = atime();
		p->pause_armed = 1;
		DBGSI serprintf("android_sync: pause start at %d\n", p->pause_start_ms);
		pthread_mutex_unlock( &p->locked.mtx );
		return;
	}

	if( !p->pause_armed ) {
		DBGSI serprintf("android_sync: resume shift skipped (pause not armed)\n");
		p->pause_start_ms = 0;
		pthread_mutex_unlock( &p->locked.mtx );
		return;
	}

	DBGSI serprintf("android_sync: resume check audio_time=%d video_time=%d diff=%d\n",
		s->audio_time, s->video_time, s->video_time - s->audio_time);
	DBGSI serprintf("android_sync: resume state offset=%lld pending=%d seek_epoch=%d\n",
		(long long)p->render_offset_ns, p->pending_reanchor, s->seek_epoch);

	// If audio_time is stale vs video_time, invalidate audio timing and re-anchor on first post-resume audio.
	if( s->audio_time >= 0 && s->video_time >= 0 && (s->video_time - s->audio_time) > 500 ) {
		DBGSI serprintf("android_sync: resume invalidates stale audio_time (a=%d v=%d)\n",
			s->audio_time, s->video_time);
		s->audio_time = -1;
		s->sync_a_time = -1;
		p->render_offset_ns = -1;
		p->render_offset_from_audio = 0;
		p->pause_start_ms = 0;
		p->pause_armed = 0;
		pthread_mutex_unlock( &p->locked.mtx );
		return;
	}

	// Shift render_offset_ns by paused duration to avoid fast catch-up on resume.
	if( p->pause_start_ms > 0 && p->render_offset_ns != -1 ) {
		int pause_ms = atime() - p->pause_start_ms;
		if( pause_ms > 0 ) {
			p->render_offset_ns += (int64_t)pause_ms * 1000000LL;
			if( p->last_mode2_dynamic_active ) {
				// The first compressed writes after play refill AudioTrack and can
				// move heard time by tens of milliseconds. Finish that explicit
				// resume transition promptly instead of leaving the steady 0.2ms
				// path to modulate video cadence for several seconds.
				p->mode2_dynamic_slew = 1;
				p->mode2_dynamic_fast_slew = 1;
				p->mode2_dynamic_settle_frames = 0;
				p->mode2_slew_frame_handle = NULL;
				p->mode2_slew_frame_time = INT_MIN;
				p->mode2_slew_frame_epoch = INT_MIN;
			}
			DBGSI serprintf("android_sync: resume shift offset by %dms -> %lld\n",
				pause_ms, p->render_offset_ns);
		}
	} else {
		DBGSI serprintf("android_sync: resume shift skipped (pause_start=%d offset=%lld)\n",
			p->pause_start_ms, (long long)p->render_offset_ns);
	}
	p->pause_start_ms = 0;
	p->pause_armed = 0;
	pthread_mutex_unlock( &p->locked.mtx );
}

#define OMXC_REGISTER( format, mangler ) \
STREAM_REGISTER_DEC_VIDEO( format, 0, MAXW, MAXH, SFDEC_MEDIACODEC, new_dec, "sfdec2", mangler );

#ifdef CONFIG_OMX_MPEG2
//OMXC_REGISTER( VIDEO_FORMAT_MPEG, &stream_video_mangler_MPEG2 );
#endif
#ifdef CONFIG_OMX_MPEG4
OMXC_REGISTER( VIDEO_FORMAT_MPG4, NULL );
#endif
#ifdef CONFIG_OMX_H264
OMXC_REGISTER( VIDEO_FORMAT_H264, &stream_video_mangler_H264 );
#endif
#ifdef CONFIG_OMX_HEVC
OMXC_REGISTER( VIDEO_FORMAT_HEVC, NULL );
OMXC_REGISTER( VIDEO_FORMAT_DOLBY_VISION, NULL );
#endif
#ifdef CONFIG_OMX_WMV
OMXC_REGISTER( VIDEO_FORMAT_WMV3, NULL );
OMXC_REGISTER( VIDEO_FORMAT_VC1, NULL );
#endif
#ifdef CONFIG_OMX_RV3040
//OMXC_REGISTER( VIDEO_FORMAT_RV40, NULL );
//OMXC_REGISTER( VIDEO_FORMAT_RV30, NULL );
#endif
#ifdef CONFIG_OMX_VP6
//OMXC_REGISTER( VIDEO_FORMAT_VP6, NULL );
#endif
#ifdef CONFIG_OMX_VP7
//OMXC_REGISTER( VIDEO_FORMAT_VP7, NULL );
#endif
#ifdef CONFIG_OMX_VP8
OMXC_REGISTER( VIDEO_FORMAT_VP8, NULL );
#endif
#ifdef CONFIG_OMX_VP9
OMXC_REGISTER( VIDEO_FORMAT_VP9, NULL );
OMXC_REGISTER( VIDEO_FORMAT_AV1, NULL );
#endif

#ifdef DEBUG_MSG
static STREAM_REG_DEC_VIDEO reg_mpeg2 = { VIDEO_FORMAT_MPEG, 0, MAXW, MAXH, 0, DSP3, new_dec, "sfdec", &stream_video_mangler_MPEG2 };
static STREAM_REG_DEC_VIDEO reg_mpeg4 = { VIDEO_FORMAT_MPG4, 0, MAXW, MAXH, 0, DSP3, new_dec, "sfdec", NULL };
static STREAM_REG_DEC_VIDEO reg_h264  = { VIDEO_FORMAT_H264, 0, MAXW, MAXH, 0, DSP3, new_dec, "sfdec", &stream_video_mangler_H264 };
static STREAM_REG_DEC_VIDEO reg_hevc  = { VIDEO_FORMAT_HEVC, 0, MAXW, MAXH, 0, DSP3, new_dec, "sfdec", NULL };
static STREAM_REG_DEC_VIDEO reg_wmv3  = { VIDEO_FORMAT_WMV3, 0, MAXW, MAXH, 0, DSP3, new_dec, "sfdec", NULL };
static STREAM_REG_DEC_VIDEO reg_vc1   = { VIDEO_FORMAT_VC1,  0, MAXW, MAXH, 0, DSP3, new_dec, "sfdec", NULL };
static STREAM_REG_DEC_VIDEO reg_rv4   = { VIDEO_FORMAT_RV40, 0, MAXW, MAXH, 0, DSP3, new_dec, "sfdec", NULL };
static STREAM_REG_DEC_VIDEO reg_rv3   = { VIDEO_FORMAT_RV30, 0, MAXW, MAXH, 0, DSP3, new_dec, "sfdec", NULL };
static STREAM_REG_DEC_VIDEO reg_vp8   = { VIDEO_FORMAT_VP8, 0, MAXW, MAXH, 0, DSP3, new_dec, "sfdec", NULL };
static STREAM_REG_DEC_VIDEO reg_vp9   = { VIDEO_FORMAT_VP9, 0, MAXW, MAXH, 0, DSP3, new_dec, "sfdec", NULL };
static STREAM_REG_DEC_VIDEO reg_av1   = { VIDEO_FORMAT_AV1, 0, MAXW, MAXH, 0, DSP3, new_dec, "sfdec", NULL };

static void _reg_sfc() 
{
serprintf("register OMX for VIDEO_FORMAT_MPEG\r\n");
	stream_unregister_dec_video( VIDEO_FORMAT_MPEG );
	stream_register_dec_video( &reg_mpeg2 );
serprintf("register OMX for VIDEO_FORMAT_MPG4\r\n");
	stream_unregister_dec_video( VIDEO_FORMAT_MPG4 );
	stream_register_dec_video( &reg_mpeg4 );
serprintf("register OMX for VIDEO_FORMAT_H264\r\n");
	stream_unregister_dec_video( VIDEO_FORMAT_H264 );
	stream_register_dec_video( &reg_h264 );
serprintf("register OMX for VIDEO_FORMAT_HEVC\r\n");
	stream_unregister_dec_video( VIDEO_FORMAT_HEVC );
	stream_register_dec_video( &reg_hevc );
serprintf("register OMX for VIDEO_FORMAT_WMV3\r\n");
	stream_unregister_dec_video( VIDEO_FORMAT_WMV3 );
	stream_register_dec_video( &reg_wmv3 );
serprintf("register OMX for VIDEO_FORMAT_VC1\r\n");
	stream_unregister_dec_video( VIDEO_FORMAT_VC1 );
	stream_register_dec_video( &reg_vc1 );
serprintf("register OMX for VIDEO_FORMAT_RV40\r\n");
	stream_unregister_dec_video( VIDEO_FORMAT_RV40 );
	stream_register_dec_video( &reg_rv4 );
serprintf("register OMX for VIDEO_FORMAT_RV30\r\n");
	stream_unregister_dec_video( VIDEO_FORMAT_RV30 );
	stream_register_dec_video( &reg_rv3 );
serprintf("register OMX for VIDEO_FORMAT_VP8\r\n");
	stream_unregister_dec_video( VIDEO_FORMAT_VP8 );
	stream_register_dec_video( &reg_vp8 );
serprintf("register OMX for VIDEO_FORMAT_VP9\r\n");
	stream_unregister_dec_video( VIDEO_FORMAT_VP9 );
	stream_register_dec_video( &reg_vp9 );
serprintf("register OMX for VIDEO_FORMAT_AV1\r\n");
	stream_unregister_dec_video( VIDEO_FORMAT_AV1 );
	stream_register_dec_video( &reg_av1 );
}

DECLARE_DEBUG_COMMAND_VOID( "regsfcc", _reg_sfc );
#endif

#endif
