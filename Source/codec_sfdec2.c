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

#define DBGS	if(0||Debug[DBG_STREAM])
#define DBGCV   if(0||Debug[DBG_CV])
#define DBGCV2  if(0||Debug[DBG_CV] > 1 )
#define DBGCV3  if(0||Debug[DBG_CV] > 2 )
#define DBGSI   if(0||Debug[DBG_SINK] )
#define DBGSI2  if(0||Debug[DBG_SINK] > 1 )

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
static int android_sync = 1;

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
	int prev_paused;

	STREAM_DEC_VIDEO *dec;
	int64_t render_offset_ns;
	float last_av_speed;
	int passthrough_cached;		// cached passthrough state to avoid repeated sink queries
	int post_speed_grace_frames;
} priv_t;

static int _get_time( priv_t *p )
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

static INT64 _snap_timestamp_ns(priv_t *p, int frame_time)
{
	if( frame_time < 0 )
		return 0;
	INT64 timestamp_us = (INT64)frame_time * 1000LL;
	if( p->video_frame_rate_den && p->playback_speed_den ) {
		INT64 rendering_num = (INT64)p->video_frame_rate_num * p->playback_speed_num;
		INT64 rendering_den = (INT64)p->video_frame_rate_den * p->playback_speed_den;
		if( rendering_num && rendering_den ) {
			double frame_length = (double)rendering_num / (double)rendering_den;
			double tus = (double)timestamp_us;
			double half_frame = 0.5 * 1000.0 * 1000.0 / frame_length;
			tus += half_frame;
			double frame_index = (double)timestamp_us * frame_length / 1000000.0;
			int snapped_index = (int)(frame_index + 0.5);
			double snapped = (double)snapped_index * 1000.0 * 1000.0 / frame_length;
			timestamp_us = (INT64)snapped;
		}
	}
	return timestamp_us * 1000LL;
}

// Reproduce MediaCodec's releaseOutputBufferAtTime() pacing logic when we can't
// use android_sync, so reordered frames still map to the correct WC deadline.
static int _compute_blit_wait_ms(priv_t *p, VIDEO_FRAME *f)
{
	INT64 timestamp_ns = _snap_timestamp_ns(p, f ? f->time : -1);
	INT64 now_ns = _get_monotonic_ns();
	INT64 start_off = p->sched_start_off_ns;
	INT64 start_mono = p->sched_start_mono_ns;
	INT64 target_ns = timestamp_ns - start_off + start_mono;
	INT64 delta = target_ns - now_ns;
	int asap = 0;

	if( !start_off ||
	    (now_ns - p->sched_last_mono_ns) > 500 * NSEC_PER_MSEC ||
	    (delta < -500 * NSEC_PER_MSEC || delta > 500 * NSEC_PER_MSEC) ) {
		p->sched_start_mono_ns = now_ns + 100 * NSEC_PER_MSEC;
		p->sched_start_off_ns = timestamp_ns;
		asap = 1;
	}

	if( !asap && delta < 0 ) {
		p->sched_late++;
		p->sched_start_mono_ns += 100 * NSEC_PER_MSEC;
	} else {
		p->sched_late = 0;
	}

	target_ns = timestamp_ns - p->sched_start_off_ns + p->sched_start_mono_ns;
	p->sched_last_mono_ns = now_ns;
	p->sched_last_off_ns = timestamp_ns;

	if( asap )
		return 0;

	delta = target_ns - now_ns;
	if( delta > (INT64)INT_MAX * NSEC_PER_MSEC )
		delta = (INT64)INT_MAX * NSEC_PER_MSEC;
	if( delta < (INT64)INT_MIN * NSEC_PER_MSEC )
		delta = (INT64)INT_MIN * NSEC_PER_MSEC;

	return (int)( delta / 1000000LL );
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
		DBGCV3 CLOG("%s|%s|%s",
		    p->locked.state & THREAD_STATE_READING ? "reading" : "!reading",
		    p->locked.state & THREAD_STATE_RENDERING ? "rendering" : "!rendering",
		    p->locked.state & THREAD_STATE_FLUSHING ? "flushing" : "!flushing");
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
	p->last_av_speed = 1.0f;
	p->post_speed_grace_frames = 0;

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

	if (!sink->is_open)
		return 0;

	pthread_mutex_lock(&p->locked.mtx);
	frame_q_put(&p->locked.venc_q, frame);
	pthread_cond_broadcast(&p->locked.cond);
	pthread_mutex_unlock(&p->locked.mtx);

DBGSI2 CLOG("frame %2d/%8d handle %p", frame->index, frame->time, frame->android_handle);
	return _get_time(p);
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
	STREAM *s = (STREAM *)p->dec->ctx;

	int dt = time    - p->venc_put_time;
	int dr = atime() - p->venc_ref_time;

	// Detect passthrough so we can avoid android_sync re-anchoring noise on encoded outputs.
	int passthrough = _is_passthrough(s);

	// Allow updates if the deviation from the expected time is significant (> 40ms)
	// This handles discontinuities (e.g. from atempo flush) even if updates are frequent.
	int expected = p->venc_put_time + dr;
	int diff = time - expected;
	if (diff < 0) diff = -diff;

	// Detect speed change (explicit discontinuity)
	int speed_changed = 0;
	float current_speed = audio_interface_get_audio_speed();
	if (fabsf(current_speed - p->last_av_speed) > 0.001f) {
		speed_changed = 1;
		p->last_av_speed = current_speed;
		DBGSI serprintf("videosink_put_time: speed changed to %.2f\n", current_speed);
	}

	if( !speed_changed && dr < sfdec_threshold && p->venc_put_time && time > p->venc_put_time && diff < 40 ) {
		return 0;
	}

	p->venc_put_time = time;
	p->venc_ref_time = atime();

	int abs_diff = diff;
	if( abs_diff < 0 ) {
		abs_diff = -abs_diff;
	}

	int using_atempo = (s && s->audio_filter_atempo != NULL);

	// Threshold strategy:
	// - android_sync=1: fixed 500ms
	// - android_sync=0:
	//   * during speed change/grace: tight diff trigger
	//   * steady state (normal): adaptive threshold scaled to latency, clamped [250, 750]
	//   * steady state (atempo): Authoritative tight 50ms threshold (scaled by speed)
	int base_threshold = android_sync ? 500 : 250;
	if( !android_sync && !(speed_changed || p->post_speed_grace_frames > 0) && s && s->audio_ctx ) {
		int audio_latency_ms = audio_interface_get_delay( s->audio_ctx );
		if( audio_latency_ms < 0 ) {
			audio_latency_ms = 0;
		}

		if (using_atempo) {
			// Atempo is authoritative. Use a tight 50ms baseline.
			int adaptive = (int)( 50.0f * current_speed );
			if( adaptive < 50 ) adaptive = 50;
			if( adaptive > 150 ) adaptive = 150;
			base_threshold = adaptive;
		} else {
			int adaptive = audio_latency_ms / 2;
			if( adaptive < 250 ) adaptive = 250;
			if( adaptive > 750 ) adaptive = 750;
			base_threshold = adaptive;
		}
	}

	// Steady state "Trust" logic for android_sync=0:
	// If we are not in a speed transition and drift is within the tight threshold,
	// do NOT perform a Hard Reset of the wall-clock anchors. The soft nudge to 
	// venc_put_time above is enough to track minor jitter.
	if( !android_sync && !speed_changed && p->post_speed_grace_frames <= 0 && 
	    abs_diff < base_threshold && p->sched_start_off_ns != 0 ) {
		pthread_mutex_lock(&p->locked.mtx);
		p->passthrough_cached = passthrough;
		pthread_mutex_unlock(&p->locked.mtx);
		return 0;
	}

	// Grace window after speed change
	if( speed_changed ) {
		p->post_speed_grace_frames = 20;
	}

	// Re-anchor (Hard Reset) if speed changed OR drift exceeds our strict threshold.
	int allow_reanchor = speed_changed || (abs_diff > base_threshold);

	if( p->post_speed_grace_frames > 0 && !speed_changed ) {
		p->post_speed_grace_frames--;
	}

	if (allow_reanchor) {
		pthread_mutex_lock(&p->locked.mtx);
		if ( !android_sync ) {
			p->sched_start_off_ns  = (INT64)time * 1000000LL;
			p->sched_start_mono_ns = _get_monotonic_ns();
			p->sched_last_off_ns   = p->sched_start_off_ns;
			p->sched_last_mono_ns  = p->sched_start_mono_ns;
			p->sched_late          = 0;
			DBGSI serprintf("videosink_put_time: reset sched anchors at time=%d, diff=%d (speed_changed=%d)\n", time, diff, speed_changed);
		} else if (speed_changed || !passthrough) {
			// Reset for PCM always, or passthrough only on speed changes
			p->render_offset_ns = -1;
			p->passthrough_cached = passthrough;
			DBGSI serprintf("videosink_put_time: reset render_offset_ns diff=%d speed=%d pt=%d\n",
				diff, speed_changed, passthrough);
		} else {
			// Passthrough mode with drift (no speed change): skip re-anchoring
			p->passthrough_cached = passthrough;
			DBGSI serprintf("videosink_put_time: SKIP re-anchor (passthrough) diff=%d\n", diff);
		}
		pthread_mutex_unlock(&p->locked.mtx);
	} else {
		pthread_mutex_lock(&p->locked.mtx);
		p->passthrough_cached = passthrough;
		// No reset needed
		pthread_mutex_unlock(&p->locked.mtx);
	}

DBGSI2 serprintf("[[put %8d|%4d|%4d]]", time, dt, dr );
	return 0;
}

static int videosink_get_time( STREAM_SINK_VIDEO *sink )
{
	priv_t *p = (priv_t *) sink->priv;

	return _get_time( p );
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

static void *videosink_thread(void *ctx)
{
	priv_t *p = (priv_t*) ctx;
	STREAM *s = (STREAM *)p->dec->ctx;

	pthread_mutex_lock(&p->locked.mtx);
	while (p->locked.run && !p->locked.error) {

		// Detect pause/resume transitions to keep codec anchors aligned
		if( s ) {
			if( s->paused && !p->prev_paused ) {
				if( android_sync ) {
					sfdec_pause( p->sfdec );
DBGSI serprintf("MediaCodec pause\n");
				}
				p->prev_paused = 1;
				p->render_offset_ns = -1;
			} else if( !s->paused && p->prev_paused ) {
				if( android_sync ) {
					sfdec_resume( p->sfdec );
DBGSI serprintf("MediaCodec resume\n");
				}
				p->prev_paused = 0;
				p->render_offset_ns = -1;
			}
		}

		VIDEO_FRAME *f = NULL;
		while (has_state_l(p, THREAD_STATE_FLUSHING) || (p->locked.run && !(f = frame_q_get(&p->locked.venc_q)))) {
			rm_state_l(p, THREAD_STATE_RENDERING);
			pthread_cond_wait(&p->locked.cond, &p->locked.mtx);
		}
		add_state_l(p, THREAD_STATE_RENDERING);

		if (!p->locked.run || !f || !f->android_handle) {
			goto endloop;
		}

 		int do_render = 1;
 		int venc_time = _get_time(p);
 		int blit_duration;

		// Use MediaCodec's projection (nanosecond snapping) by not providing an absolute timestamp here.
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		int64_t render_ts_ns = 0;
		int passthrough = p->passthrough_cached;

		if( android_sync ) {
			// For android_sync, use the traditional blit_time calculation
			blit_duration = sfdec_force_blit ? 0 : f->blit_time - venc_time;
		} else if( sfdec_force_blit ) {
			blit_duration = 0;
		} else {
			// For non-android_sync, use the pacing logic that mimics MediaCodec's behavior
			blit_duration = _compute_blit_wait_ms( p, f );
		}

DBGSI serprintf("[%3d|%2d] : f->time: %8d|%d | %d", blit_duration, f->index, f->time, f->duration, f->blit_time);
		if( s && s->paused ) {
DBGSI serprintf(" paused\n");
		} else if (blit_duration > 0) {
DBGSI serprintf(" wait\n");
//DBGSI2 CLOG("sleep @ %10ld.%3ld  dur %3d  f(%2d)  blit %8d  venc %8d)", ts.tv_sec, ts.tv_nsec/1000000, blit_duration, f->index, f->blit_time, venc_time);

			// do not hang on one pic forever
			if( blit_duration > f->duration + 100 )
				blit_duration = f->duration + 100;

			if( !android_sync ) {
				struct timespec ts_wait = ts;
				timespec_add_ms(&ts_wait, blit_duration);

				int rc = 0;
				while (p->locked.run && rc == 0 && !has_state_l(p, THREAD_STATE_FLUSHING)) {
					rc = pthread_cond_timedwait(&p->locked.cond, &p->locked.mtx, &ts_wait);
				}
				if (!p->locked.run) {
DBGCV CLOG("stop thread 2");
				}
			}
		// Android sync will happily drop the frames for us
		} else if ( !android_sync ) {
			int audio_delay = 0;
			int smoothed_delay = 0;
			if( s ) {
				smoothed_delay = s->smoothed_av_delay;
				if( s->audio_ctx ) {
					audio_delay = audio_interface_get_delay( s->audio_ctx );
				}
			}
			if( audio_delay < 0 ) {
				audio_delay = 0;
			}
			if( smoothed_delay < 0 ) {
				smoothed_delay = 0;
			}
			int latency_ms = MAX( audio_delay, smoothed_delay );
			int late_threshold = -40;
			if( latency_ms > 200 ) {
				late_threshold = -MAX( 40, latency_ms );
			}
			if( blit_duration < late_threshold && p->dropped < 5 ) {
			if( 1 ) {
				// drop frames if thread is stopped or we are too late (more than 40ms) 
				// but don't drop too many consecutives frames
				do_render = 0;
				p->dropped ++;
				f->blit_time = -1;
			}
			int audio_time = -1;
			int video_time = -1;
			float speed = audio_interface_get_audio_speed();
			if( s ) {
				audio_time = s->audio_time;
				video_time = s->video_time;
			}
DBGSI serprintf(" DROP blit=%d f=%d time=%d venc=%d audio_time=%d video_time=%d smoothed=%d audio_delay=%d latency=%d threshold=%d speed=%.3f dropped=%d\n",
	blit_duration, f->index, f->time, venc_time, audio_time, video_time, smoothed_delay, audio_delay, latency_ms, late_threshold, speed, p->dropped);
//CLOG("dropping frame(%d): %d ms late, blit_time: %d, venc_time: %d, f->time: %d", f->index, (venc_time - f->blit_time), f->blit_time, venc_time, f->time);
			}
		} else {
DBGSI serprintf(" ok\n");
		}

		if( android_sync ) {
			// Calculate raw blit duration (unclamped)
			int raw_blit_duration = f->blit_time - venc_time;
			int64_t now_ns = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;

			// If we are within a reasonable sync window (e.g. -1s to +5s), use the player clock
			// We allow a large positive window because buffering (frames ahead of time) is good and shouldn't be squashed.
			// We allow a negative window to let the player catch up (fast forward) if slightly behind.
			if (raw_blit_duration > -1000 && raw_blit_duration < 5000) {
				int64_t current_offset_ns = now_ns - (int64_t)venc_time * 1000000LL;
				if (p->render_offset_ns == -1) {
					p->render_offset_ns = current_offset_ns;
					DBGSI serprintf("android_sync: initial anchor established offset=%lld ms pt=%d\n",
						current_offset_ns/1000000LL, passthrough);
				} else if (!passthrough) {
					int64_t diff = current_offset_ns - p->render_offset_ns;
					// Adapt drift tolerance to device latency: base 50ms, loosen on high latency up to 200ms
					int drift_tol_ms = 50;
					if( s && s->audio_ctx ) {
						int lat_ms = audio_interface_get_delay( s->audio_ctx );
						if( lat_ms < 0 ) {
							lat_ms = 0;
						}
						// Allow larger jitter on high-latency devices: use half the latency,
						// clamped to [200ms, 500ms]
						int adaptive = lat_ms / 2;
						if( adaptive < 200 ) {
							adaptive = 200;
						} else if( adaptive > 500 ) {
							adaptive = 500;
						}
						drift_tol_ms = adaptive;
					}
					int64_t drift_tol_ns = (int64_t)drift_tol_ms * 1000000LL;
					if (diff > drift_tol_ns || diff < -drift_tol_ns) {
						p->render_offset_ns = current_offset_ns;
						DBGSI serprintf("android_sync: offset drift %lld ms (tol=%d ms), resetting\n", diff/1000000LL, drift_tol_ms);
					}
				} else {
					int64_t diff = current_offset_ns - p->render_offset_ns;
					DBGSI serprintf("android_sync: offset drift %lld ms SKIPPED (passthrough)\n", diff/1000000LL);
				}
				render_ts_ns = (int64_t)f->time * 1000000LL + p->render_offset_ns;
			} else {
				// Large desync detected (e.g. clock reset). Establish a local anchor.
				if (p->render_offset_ns == -1) {
					// Anchor the current frame to play in 50ms
					p->render_offset_ns = now_ns - (int64_t)f->time * 1000000LL + 50000000LL;
					DBGSI serprintf("android_sync: re-anchoring. raw_dur=%d, offset=%lld\n", raw_blit_duration, p->render_offset_ns);
				}
				render_ts_ns = (int64_t)f->time * 1000000LL + p->render_offset_ns;
			}
		}

		if( !p->locked.run || has_state_l(p, THREAD_STATE_FLUSHING)) {
			do_render = 0;
		}

		pthread_mutex_unlock(&p->locked.mtx);

		if (do_render) {
DBGCV3 CLOG("render ->");
			int start = time_update_time();
			// For android_sync we rely on MediaCodec projection; only non-android_sync uses ASAP.
			int asap = !android_sync;
			sfdec_buf_render(p->sfdec, (sfbuf_t *)f->android_handle, 1, asap, render_ts_ns);
			int took = time_update_time() - start;
			p->dropped = 0;
DBGCV CLOG("\t\t\t\t\t\t\trender %8d/%8d  took %3d", f->time, f->blit_time, took );
DBGCV3 CLOG("render <-");
		} else {
			sfdec_buf_render(p->sfdec, (sfbuf_t *)f->android_handle, 0, 0, 0);
		}
		// "Paused" here usually mean "seek"
		if( s && s->paused ) {
			sfdec_reset_ts(p->sfdec);
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

		while (has_state_l(p, THREAD_STATE_FLUSHING) || (p->locked.run && !p->locked.error &&!(f = frame_q_get(&p->locked.dec_q)))) {
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
			frame_q_put_head(&p->locked.dec_q, f);
			continue;
		}
		int out_time;
		int out_type;
		int out_ID;

		if( p->reorder_pts ) {
			// get reordered TS
			out_time = time;
		} else {
			// no reordering, just use the ts_ queue
			out_time = XDM_ts_get( &p->XDM_ctx );
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

	int hw_type = device_get_hw_type();
	if (video->format == VIDEO_FORMAT_H264 && video->sps.valid && video->profile >= H264_PROFILE_HIGH10) {
		CLOG("sf can't do Hi10P, abort");
		return 1;
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

	switch (video->format) {
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
    if (video->format == VIDEO_FORMAT_DOLBY_VISION) {
		DBGCV2 serprintf("dovi profile dtr %s\n", acodecs_get_for_profile("video/dolby-vision", 16));
		DBGCV2 serprintf("dovi profile dth %s\n", acodecs_get_for_profile("video/dolby-vision", 64));
		DBGCV2 serprintf("dovi profile st %s\n", acodecs_get_for_profile("video/dolby-vision", 256));
		DBGCV2 serprintf("dovi profile stn %s\n", acodecs_get_for_profile("video/dolby-vision", 32));
		DBGCV2 serprintf("dovi profile dtb %s\n", acodecs_get_for_profile("video/dolby-vision", 128));
		DBGCV2 serprintf("dovi profile der %s\n", acodecs_get_for_profile("video/dolby-vision", 4));
		DBGCV2 serprintf("dovi profile den %s\n", acodecs_get_for_profile("video/dolby-vision", 8));
		DBGCV serprintf("dovi profile myself %s\n", acodecs_get_for_profile("video/dolby-vision", video->dv_profile));
        decoder_name = acodecs_get_for_profile("video/dolby-vision", video->dv_profile);
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
			&pts_reorder, decoder_name, video->frame_rate_den, video->frame_rate_num);
	apply_rotation(p, video->rotation, width, height, &width, &height);

	if (!p->sfdec) {
		CLOG("sfdec_new failed");
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

	if (sfdec_start(p->sfdec) != 0) {
		CLOG("sfdec_start failed");
		goto err;
	}

	p->num_frames = sfdec_max_frames;
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
	p->render_offset_ns = -1;
	p->passthrough_cached = _is_passthrough((STREAM *)dec->ctx);
	p->post_speed_grace_frames = 0;

	video->colorspace = AV_IMAGE_HW;
	dec->video = &dec->_video;
	memcpy(dec->video, video, sizeof(VIDEO_PROPERTIES));

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

		pthread_cond_broadcast(&p->locked.cond);
		pthread_mutex_unlock(&p->locked.mtx);

		sfdec_flush(p->sfdec);

		pthread_join(p->dec_thread, NULL);
		pthread_join(p->sink_thread, NULL);

DBGCV CLOG("stop thread done");

		for (i = 0; i < p->num_frames; ++i)
			frame_release(p->sfdec, p->frames[i]);

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

	ret = sfdec_send_input(p->sfdec, d->data[0], d->size, (int64_t)d->time * 1000, d->type == I_VOP ? 1 : 0, 0);
	if( ret > 0 ) {
DBGCV CLOG("%c %8d: %d/%d", frame_type(d->type), d->time, ret, d->size);
		pthread_mutex_lock(&p->locked.mtx);
		XDM_id_put( &p->XDM_ctx,  d->time, d->type, d->user_ID );
		if( !p->reorder_pts ) {
			XDM_ts_put( &p->XDM_ctx, d->time );
		}

		pthread_mutex_unlock(&p->locked.mtx);
	}
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

	add_state_l(p, THREAD_STATE_FLUSHING);

	XDM_id_flush( &p->XDM_ctx );
	XDM_ts_flush( &p->XDM_ctx );
	sfdec_flush(p->sfdec);
	if( android_sync ) {
		sfdec_seek_reset( p->sfdec );
DBGCV CLOG("MediaCodec seek reset");
	}

	while (p->locked.state & (THREAD_STATE_READING|THREAD_STATE_RENDERING)) {
		pthread_cond_wait(&p->locked.cond, &p->locked.mtx);
	}

	for (i = 0; i < p->num_frames; ++i)
		frame_release(p->sfdec, p->frames[i]);

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
	    if( den )
	    	p->playback_speed_den = den;
	    if( num )
	    	p->playback_speed_num = num;
	    int rc = sfdec_set_playback_speed(p->sfdec, den, num);
	    DBGSI serprintf("sfdec2: set_playback_speed den=%d num=%d rc=%d\n", den, num, rc);
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

void set_android_sync(int sync)
{
	DBGSI serprintf("set_android_sync: %d\n", sync);
	android_sync = sync;
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
