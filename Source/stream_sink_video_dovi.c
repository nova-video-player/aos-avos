/*
 * Copyright (C) 2026 The Nova Video Player Project
 *
 * Video sink rendering Dolby Vision content through the libplacebo GPU
 * tone-mapping path (mpv style).
 *
 * Frame contract: the sink OWNS the frame pool (allocates_frames = 1), like
 * the android2 sink. Frames carry no pixel buffers (data[0] == NULL); the
 * decoder delivers cloned AVFrames in frame->priv (+ optional paired
 * enhancement-layer AVFrame in frame->handle[1]) via its zero-copy branch,
 * and the sink renders them through dovi_gl on the venc thread.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#include "global.h"
#include "debug.h"
#include "av.h"
#include "stream.h"
#include "stream_sink_video.h"
#include "frame_q.h"
#include "astdlib.h"

#include "dovi_gl.h"

#include <libavutil/frame.h>
#include <pthread.h>
#include <errno.h>

#define DOVI_SINK_MAX_FRAMES 64

typedef struct {
	void        *gl;          /* dovi_gl renderer context */
	void        *surface_handle;
	VIDEO_FRAME *frames[DOVI_SINK_MAX_FRAMES];
	int          num_frames;
	FRAME_Q      get_q;       /* rendered frames waiting to be reclaimed */
	int          venc_time;
	int          venc_put_time;
	int          venc_ref_time;    /* atime() when venc_put_time was set (WC anchor) */
	int          dropped;
	int          stat_forced_late;	/* presented with deadline missed >8 durations, no fresher frame */
	/* frame rendered but not yet presented (mpv draw_frame/flip_page
	 * pipelining); held by the venc thread between iterations */
	VIDEO_FRAME *pending_frame;
	/* render thread (android2 venc_thread parity): pacing must not run on
	 * the player engine thread, which also drives the decoder */
	FRAME_Q      venc_q;      /* frames waiting to be rendered */
	pthread_t    venc_thread_handle;
	pthread_mutex_t venc_mutex;
	pthread_cond_t  venc_cond;
	volatile int venc_run;
	volatile int venc_flushing;
	volatile int venc_busy;	/* 1: the thread holds a frame (dequeue..put) */
	volatile int venc_flush_gen;	/* bumped by sink_flush under the lock; the
				 * venc thread re-checks it after every unlock window
				 * (render/present run unclocked) and abandons its
				 * held frame if a flush rebuilt the pool underneath
				 * it - the rebuild owns every pool frame from that
				 * point, so a recycled put would double-list it
				 * (the measured 'frame_q_put FATAL already in
				 * [dec]' wedge). */
	/* 1Hz pacing diagnostics */
	int stat_render, stat_present;
	int stat_put;
	int64_t stat_render_ns, stat_present_ns;
	/* present-deadline wait: iterations + total ns (separates "queue
	 * starved" from "paced on the blit deadline") */
	int stat_poll_iters;
	int64_t stat_wait_ns;
	int stat_time0, stat_time_last;	/* frame->time span at put (spacing check) */
	int stat_late_skips;	/* dropped by the vsync-skip catch-up drain */
	int64_t stat_idle_ns;	/* cond_wait time: queue-starved vs paced */
	/* content fps hint state: the MKV parser leaves frame_rate_num/den 0
	 * when avg != r_frame_rate, so the open-time hint is a fallback; the
	 * venc thread refreshes it from frame->duration once (mpv/ExoPlayer
	 * declare the rate on the Surface, SF then latches each frame on its
	 * own vsync - measured: without the hint the panel stays 120Hz, the
	 * swap fence is unpredictable and presents cap at ~25-40/s on 48fps
	 * content). */
	int framerate_set;
	/* --- presentation feedback (VideoClock hooks, JRiver/mpv parity) ---
	 * present_at: each queued buffer carries its desired CLOCK_MONOTONIC
	 * ns latch time (content blit_time mapped through the anchor pair),
	 * set via eglPresentationTimeANDROID before the swap. fb_active is a
	 * tri-state capability probed on the first present: -1 unknown (try
	 * the query once), 0 unsupported (free-run, identical to pre-hook
	 * behavior), 1 active (every present re-anchors this clock from the
	 * ACTUAL latch: venc_put_time = presented frame's blit_time,
	 * venc_ref_time = wall ms at the physical latch - dovi_sink_get_time
	 * then advances from physical presentation, closing the loop the way
	 * mpv's update_vsync_timing_after_swap and JRiver's VideoClock do).
	 * The audio-driven put_time soft-blend keeps running: both anchors
	 * are now physical (heard audio vs seen frame), their small disagreement
	 * is the true A/V skew, corrected by the rate-limited blend instead of
	 * accumulating. */
	int fb_active;		/* -1 probe / 0 unsupported / 1 active */
	int fb_locked;		/* 1 after the ONE-TIME sync-acquisition jump:
					 * on the first valid latch the banked startup
					 * lead (A/V offset, measured ~650ms audio-ahead)
					 * is closed with a single hard correction -
					 * after that, rate-only trimming (the audio
					 * put_time anchor owns clock POSITION: measured
					 * two-master standoff with the old per-sample
					 * ±1ms re-anchor - heard anchor held ~200ms
					 * ahead of the clock for an entire file while
					 * the two rate caps canceled each other).
					 * One jump = mpv's seek-epoch re-anchor semantics. */
	int fb_rate_ppm;		/* clock RATE trim from latch feedback, in
					 * ns-per-ms (ppm): positive = the wall clock
					 * runs fast vs content (TrueHD accumulator
					 * over-count measured ~4%), negative = slow.
					 * Applied inside dovi_sink_get_time between
					 * anchor pairs so put_time's position writes
					 * are never fought (single-writer). Clamped
					 * to ±5000 ppm (0.5%). */
	int sched_pace;			/* eglPresentationTimeANDROID pacing master.
					 * FINAL default 0 = present_at disabled: SF latches
					 * as-available and the userspace cadence-locked
					 * wait loop (see the !sched_ok block) paces
					 * presents. Measured A/B (Phase 13): scheduled
					 * presents on this Samsung panel lose the 24Hz mode
					 * fight (yanked back to 120Hz 83ms in), held-
					 * buffer latches run 50-80ms and acquire blocks
					 * (rend 43-74ms, pres 17.6-18.7/s on 24fps
					 * content); userspace pacing holds pres=24.0/s
					 * on 24fps AND pres=46/s (44-51) on 48fps Charles
					 * with late=0 skip=0 in both - the present-late
					 * regime covers the 48fps decode-deficit windows.
					 * Kept as a runtime-selectable knob (A/B and any
					 * panel where SF scheduling behaves). */
	int fb_sched_mode;		/* 1 while eglPresentationTimeANDROID targets are
					 * accepted: SurfaceFlinger owns the latch pacing
					 * (the userspace deadline wait is skipped - it
					 * duplicated SF's hold and serialized the next
					 * render behind it, capping 24fps at 17-18/s
					 * measured). 0 = userspace wait fallback. */
	int64_t fb_last_latch_ns;	/* CLOCK_MONOTONIC ns of the most recent
					 * ACTUAL latch (EGL_DISPLAY_PRESENT) - the
					 * physical presentation timeline. Scheduled
					 * targets derive from THIS, not the audio-
					 * anchored clock: the clock carries the A/V
					 * offset (~535ms audio-ahead measured), so
					 * clock-derived targets land half a second
					 * early, SF latches 'when available', the
					 * feedback poll saturates at 28ms PENDING
					 * per present (measured: presents pinned at
					 * 30/s = 33ms each) and the physical loop
					 * never converges. */
	int fb_phys_blit;		/* content TS that ACTUALLY latched at
					 * fb_last_latch_ns - written together with
					 * it in the feedback block (the coherent
					 * anchor pair for dovi_phys_time). Pacing
					 * (deadline wait + late/drain policy) runs
					 * on this PHYSICAL timeline, not the sink
					 * clock: during compositor stalls the audio
					 * anchor wobbles 100-300ms ahead of real
					 * latches and clock-based lateness misjudges
					 * every frame (measured: poll_it pinned at 0,
					 * fblate accumulated to 280ms, late/skip shed
					 * 20+/s for 90s after a 2s Samsung HRR touch
					 * stall; the same stall on the physical
					 * timeline is a constant, invisible ~100ms
					 * presentation delay that self-re-anchors
					 * at the next latch). */
	int fb_prev_blit_time;	/* blit_time of the ONE-BEHIND frame: the
					 * feedback query answers for the frame the
					 * PREVIOUS present queued (the just-queued
					 * one latches 2-3 vsyncs out on a depth-3
					 * swapchain; polling it blocked 28ms/present).
					 * Rotated at pending-frame hand-off, used as
					 * the re-anchor's content position. */
	int stat_fb_late_us;	/* 1Hz: cumulative scheduled-target vs actual-latch delta (us) */
	int64_t pres_wall_ns;	/* CLOCK_MONOTONIC ns of the last eglSwapBuffers - the
				 * PRESENT cadence the cadence-lock caps its wait against
				 * (mpv display-sync: the next vsync slot is one period
				 * after the previous present, not one period after the
				 * banked content lead). 0 before the first present. */
	int64_t pres_chain_ns;	/* FREE-RUN chain (sched_pace==2): CLOCK_MONOTONIC ns of
				 * the next planned swap - the last actual swap + exactly
				 * one content period. Never derived from the frame's
				 * blit deadline and never corrected by latch feedback:
				 * the chain is a uniform grid (swap at content fps,
				 * "no sync" free-run, mpv free-running-video model) and
				 * the compositor latches wherever it latches. */
	int64_t pres_period_ns;	/* FREE-RUN: exact per-frame period in ns, measured from
				 * consecutive blit_time deltas (double math; the int
				 * frame->duration cannot express 23.976fps = 41.708ms
				 * and an integer-ms grid drifts +1.7%/s into periodic
				 * catch-up hiccups). 0 until the first delta is seen. */
	int fr_mode_latched;	/* free-run engaged at first frame (pref may arrive after open) */
	int fr_last_blit;	/* FREE-RUN measurement state: blit_time of the last
				 * dequeued frame (the delta source above). */
	int64_t fr_period_sum_ns;	/* FREE-RUN windowed period estimator: cumulative
				 * ns over sampled blit deltas (numerator). */
	int64_t fr_period_last_blit_ms;	/* FREE-RUN estimator: blit_time (ms) of the
				 * estimator's previous sample frame. */
	int fr_period_pairs;	/* FREE-RUN estimator: frame PAIRS spanned by the
				 * cumulative sum (denominator) - a 24-frame window
				 * averages the 41/42 integer-TS mix into 41.708ms
				 * (23.976fps), which ONE delta (41.0ms measured on
				 * GoT: frjag period_us=41000) can never express. */
	int period_refined;	/* FREE-RUN: the windowed estimator has committed a
				 * period (the pre-refine single-delta fallback stays
				 * active until the first window closes). */
	int64_t fr_grid_sum_ns;	/* FREE-RUN grid estimator: cumulative ns of
				 * consecutive ACTUAL latch deltas (the panel's true
				 * scan cadence, e.g. Samsung's 24.000Hz video-refresh
				 * grid while the display server still reports 120Hz). */
	int64_t fr_grid_last_ns;	/* FREE-RUN grid estimator: previous latch ns. */
	int fr_grid_pairs;	/* FREE-RUN grid estimator: deltas accumulated. */
	int64_t pres_grid_ns;	/* FREE-RUN: the LOCKED panel latch cadence (ns).
				 * Zero until the grid estimator closes its window. */
	int fr_resample_published;/* FREE-RUN: the display-resample hint has been
				 * published to the engine exactly once for this
				 * playback (recompute would fight the settled loop). */
	int fr_chain_on_grid;	/* FREE-RUN: the swap chain now paces on the panel
				 * grid (pres_grid_ns) instead of the TS-derived
				 * period - flipped when the grid locks and the
				 * resample hint is published. */
	int stat_fb_samples;	/* 1Hz: latch samples for the average */
	int64_t fb_prev_latch_ns;	/* CLOCK_MONOTONIC ns of the latch before
				 * fb_last_latch_ns - free-run display-cadence
				 * diagnosis: consecutive latch deltas expose what
				 * the panel actually shows (a clean 41.7ms period on
				 * a 120Hz grid buckets at 5 vsync slots). */
	int fb_slot_hist[12];	/* 1Hz: latch-delta histogram in 120Hz vsync
				 * slots (index 0 = 1 slot, i.e. 8.33ms). */
	int stat_fr_over_us;	/* 1Hz free-run: us the swap entry overshoots
				 * the chain slot (sum, with the count below = mean) */
	int stat_fr_over_n;
	int stat_gate_timeout;	/* 1Hz free-run: depth-2 inflight gate hit its
				 * one-period bound without seeing frame N latch
				 * (swap proceeded with 2 frames in flight). */
	int64_t fr_last_gate_wait_ns;	/* free-run: ns the LAST inflight-gate spin
				 * waited before releasing the swap (0 when the
				 * previous latch had already landed). */
	int stat_fb_used;	/* 1Hz: re-anchors performed */
	int64_t stat_pres_swap_ns;	/* 1Hz: eglSwapBuffers wall (fence+BufferQueue) */
	int64_t stat_pres_fbpoll_ns;	/* 1Hz: feedback poll wall incl. its bounded sleep */
	int64_t stat_pres_sched_ns;	/* 1Hz: present_at call wall */
} priv_t;

/* current presentation clock: TS anchored by the last put_time call,
 * advanced by wall clock (android2 _get_time parity). The stream engine
 * calls sink->put_time with the audio-heard anchor on every audio packet,
 * so this tracks what the user is currently hearing/seeing. The latch
 * feedback's RATE trim (fb_rate_ppm) is applied on the elapsed wall time:
 * put_time owns the anchor pair (position), the trim only corrects how
 * fast the clock counts between anchors (TrueHD accumulator wobble). */
static int dovi_sink_get_time( priv_t *p )
{
	int diff = atime() - p->venc_ref_time;
	if (diff < 0)
		diff = 0;
	if (p->fb_rate_ppm)
		diff += (int)((long long)diff * p->fb_rate_ppm / 1000000LL);
	p->venc_time = p->venc_put_time + diff;
	return p->venc_time;
}

/* PHYSICAL presentation clock: content TS that the display is showing
 * RIGHT NOW, derived from the last ACTUAL latch pair (content TS that
 * latched, wall ns it latched at) - the same pair the scheduled-present
 * targets come from. Pacing decisions (deadline wait, late/drain policy)
 * must run on THIS timeline: the sink clock is audio-anchored and its
 * position can wander 100-300ms from real latch times during compositor
 * disturbances; against a wandering reference every deadline reads as
 * already-passed and the drain policy misjudges frames that are in fact
 * latching fine (measured 90s shedding after a 2s Samsung HRR touch
 * stall). Sites running OUTSIDE venc_mutex must snapshot the pair under
 * the lock and call dovi_phys_from; the wrapper reads it directly for
 * callers already holding the lock. Falls back to the sink clock before
 * the first latch - the put_time hard anchor is the best pre-feedback
 * estimate. */
static int dovi_phys_from( priv_t *p, int64_t latch_ns, int phys_blit )
{
	if( p->fb_active == 1 && latch_ns > 0 ) {
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		int64_t now_ns = (int64_t) ts.tv_sec * 1000000000L + ts.tv_nsec;
		int64_t elapsed_ns = now_ns - latch_ns;
		if( elapsed_ns < 0 )
			elapsed_ns = 0;
		return phys_blit + (int)(elapsed_ns / 1000000LL);
	}
	return dovi_sink_get_time( p );
}

static int dovi_phys_time( priv_t *p )
{
	return dovi_phys_from( p, p->fb_last_latch_ns, p->fb_phys_blit );
}

/* free the payloads a frame still owns without rendering it (flush/drop) */
static void dovi_frame_free_payloads( VIDEO_FRAME *fr )
{
	if (fr->handle[0]) {
		dovi_hw_frame *hw = (dovi_hw_frame *) fr->handle[0];
		if (hw->release)
			hw->release(hw);
		fr->handle[0] = NULL;
	}
	if (fr->priv)
		av_frame_free((AVFrame **) &fr->priv);
	if (fr->handle[1])
		av_frame_free((AVFrame **) &fr->handle[1]);
}

/* Apply one landed-latch sample to the sink state (rate-trim only,
 * single-writer model: put_time owns clock position). Called with the
 * venc_mutex NOT held; takes the lock to write the coherent anchor pair
 * (fb_last_latch_ns + fb_phys_blit) and the rate trim. Gated on the flush
 * generation: a flush that rebuilt the pool while the sample was in
 * flight already dropped this frame's reference and re-anchored - a stale
 * anchor must not land (same discipline as the post-present recycle). */
static void dovi_fb_apply( priv_t *p, int64_t actual_ns, int my_gen )
{
	int actual_ms = (int)(actual_ns / 1000000LL);
	pthread_mutex_lock( &p->venc_mutex );
	if( p->venc_flush_gen == my_gen && p->pending_frame ) {
		/* clock-at-latch computed inline (venc_put_time + elapsed-since-ref):
		 * calling dovi_sink_get_time() here would evaluate atime() AFTER the
		 * poll, skewing the estimate by the poll latency. */
		int clock_at_latch = p->venc_put_time +
			(actual_ms - p->venc_ref_time);
		int ideal = p->fb_prev_blit_time > 0 ?
			p->fb_prev_blit_time : p->pending_frame->blit_time;
		int step = ideal - clock_at_latch;
		if( !p->fb_locked ) {
			/* SYNC ACQUISITION: the FIRST valid latch closes the banked
			 * startup lead in ONE jump (mpv seek-epoch semantics); after
			 * that, rate-only trimming. */
			p->fb_locked = 1;
			serprintf("dovi sink: sync acquired: jump %+dms\n", step);
			p->venc_put_time = clock_at_latch + step;
			p->venc_ref_time = actual_ms;
			p->fb_rate_ppm = 0;
		} else if( step > 3 || step < -3 ) {
			/* persistent position error: trim the rate. ppm per ms of
			 * step per sample: at 48 samples/s a 1ms steady step
			 * ≈ 0.1%/s convergence. */
			p->fb_rate_ppm += step * 100;
			if( p->fb_rate_ppm > 5000 )
				p->fb_rate_ppm = 5000;
			if( p->fb_rate_ppm < -5000 )
				p->fb_rate_ppm = -5000;
			} else {
				/* inside the 3ms window: hold position via put_time,
				 * stop rate drift accumulation */
				p->fb_rate_ppm = 0;
			}
			/* CAPABILITY PROMOTION: the first landed timestamp sample
			 * proves eglGetFrameTimestamps works on this surface - the
			 * probe (-1) becomes ACTIVE (1). Without this the probe state
			 * only ever transitions to 0 (unsupported) and EVERY consumer
			 * gated on fb_active==1 stays dead: the latch-anchored phys
			 * clock (dovi_phys_from falls back to the audio-anchored sink
			 * clock), the scheduled-mode tightening chain, and the
			 * free-run depth-2 inflight gate (the swapchain banks frames,
			 * SF skips latches to the spare, and the display shows doubled
			 * holds - the 1-per-5-30s judder that survived every earlier
			 * fix because the gate shipped disabled). Measured on GoT
			 * diag6: fb=0 stretches 58/34/45/40s, 16/25/33ms banked-latch
			 * cascades, gate_to=0 forever. */
			if( p->fb_active < 0 )
				p->fb_active = 1;
			p->fb_last_latch_ns = actual_ns;
		p->fb_phys_blit = ideal;	/* coherent pair for dovi_phys_time */
		/* free-run display-cadence diagnosis: bucket the actual
		 * latch-to-latch delta in 120Hz vsync slots (8.33ms) - the
		 * 1Hz print exposes what the PANEL really showed (uniform
		 * 5-slot holds vs occasional 4/6/10). A same-window repeat
		 * is a duplicate sample (ignored). */
		if( p->fb_prev_latch_ns > 0 && actual_ns > p->fb_prev_latch_ns ) {
			int64_t d_ns = actual_ns - p->fb_prev_latch_ns;
			int slots = (int)((d_ns + 4166666LL) / 8333333LL);
			if( slots >= 1 && slots <= 12 )
				p->fb_slot_hist[slots - 1]++;
			/* GRID ESTIMATOR (display-resample basis, both modes): the
			 * steady latch deltas ARE the panel's true scan cadence.
			 * On this Samsung panel video playback drops the scan
			 * into a 24.000Hz-class grid while SF still advertises
			 * 120Hz - the ONLY reliable way to learn the real grid
			 * is to measure it. Same consistency gate as the content
			 * period estimator: deltas within 4ms of the running mean
			 * extend the window (the 41.667ms steady holds), outliers
			 * (the beat doubles / startup noise) flush it. Lock after
			 * 24 consistent pairs; then, ONCE, publish the display-
			 * resample hint: speed = content TS period / grid period
			 * (23.976fps on a 24.000 grid -> 1.001001x) so the engine
			 * can retune audio (atempo) and phase-lock content to the
			 * grid - the mpv display-resample endgame. Mode-0 pacing
			 * keeps its own model (cadence-locked userspace wait); the
			 * audio retune alone removes the 23.976-vs-grid beat.
			 * Measured mode 0 without it: 4+6-slot beat pairs (52/20)
			 * recurring every few seconds. */
			if( 1 ) {
				if( p->fr_grid_last_ns > 0 ) {
					int64_t gd_ns = actual_ns - p->fr_grid_last_ns;
					if( gd_ns > 20000000LL && gd_ns < 100000000LL ) {
						int consistent = 1;
						if( p->fr_grid_pairs > 0 ) {
							int64_t mean_ns = p->fr_grid_sum_ns / p->fr_grid_pairs;
							int64_t dev_ns = gd_ns > mean_ns ?
								gd_ns - mean_ns : mean_ns - gd_ns;
								if( dev_ns > 4000000LL )
									consistent = 0;
							}
						if( consistent ) {
								p->fr_grid_sum_ns += gd_ns;
								p->fr_grid_pairs++;
							} else {
								p->fr_grid_sum_ns = 0;
								p->fr_grid_pairs = 0;
							}
						}
					if( p->fr_grid_pairs >= 24 && p->pres_grid_ns == 0 ) {
						int64_t mean_ns =
							(p->fr_grid_sum_ns + p->fr_grid_pairs / 2) /
							p->fr_grid_pairs;
						/* VALIDITY: the panel's true video-refresh grid must be
						 * near the content cadence (within 10%). A latch stream
						 * at 2x the period is the low-power idle scan (12Hz-class
						 * on 24fps content, before the panel enters video mode) or
						 * a banked/stalled window - locking it as the "grid"
						 * poisoned build 1419 (12.002Hz grid, quarter-speed
						 * video, multi-second stalls). A sane grid also never
						 * exceeds 1.9x content rate. */
						if( mean_ns > 20000000LL && mean_ns < 100000000LL &&
						    p->pres_period_ns > 0 &&
						    mean_ns > p->pres_period_ns - p->pres_period_ns / 10 &&
						    mean_ns < p->pres_period_ns + p->pres_period_ns / 10 ) {
							p->pres_grid_ns = mean_ns;
							serprintf("dovi sink: free-run panel grid locked: %lldus (%.3fHz)\n",
							          (long long)(mean_ns / 1000),
							          1000000000.0 / (double)mean_ns);
						}
					}
				}
				p->fr_grid_last_ns = actual_ns;
			}
			/* per-event latch diagnosis (both modes): every latch
			 * delta that is NOT the clean 5-slot (41.67ms) hold gets a
			 * full-context line - raw delta, both latch timestamps,
			 * the pacing target (free-run: chain slot; mode 0: last
			 * swap wall), the chain period, and the inflight-gate
			 * state at that moment. Ground truth the 1Hz histogram
			 * aggregates: discriminates a real panel double-hold
			 * (latch 83ms after the previous latch, swap on time) from
			 * a lost feedback sample (frame N+1 latched into the
			 * sample slot) from a gate timeout (gate_to>0 with
			 * gate_wait ~= one period). ~1 line per 5-30s. */
			if( slots != 5 ) {
				int64_t phase_ns = actual_ns - p->pres_chain_ns;
				serprintf("dovi frjag: slots=%d d_us=%lld prev_us=%lld latch_us=%lld chain_us=%lld period_us=%lld pres_wall_us=%lld gate_to=%d gate_wait_us=%lld phase_us=%lld blit=%d\n",
				          slots,
				          (long long)(d_ns / 1000),
				          (long long)(p->fb_prev_latch_ns / 1000),
				          (long long)(actual_ns / 1000),
				          (long long)(p->pres_chain_ns / 1000),
				          (long long)(p->pres_period_ns / 1000),
				          (long long)(p->pres_wall_ns / 1000),
				          p->stat_gate_timeout,
				          (long long)(p->fr_last_gate_wait_ns / 1000),
				          (long long)(phase_ns / 1000),
				          ideal);
			}
		}
		p->fb_prev_latch_ns = actual_ns;
		p->stat_fb_used++;
		p->stat_fb_samples++;
	}
	pthread_mutex_unlock( &p->venc_mutex );
}

static void *dovi_venc_thread( void *ctx )
{
	STREAM_SINK_VIDEO *sink = (STREAM_SINK_VIDEO *) ctx;
	priv_t *p = sink->priv;
	VIDEO_FRAME *frame;

	pthread_mutex_lock( &p->venc_mutex );
	while( p->venc_run ) {
		struct timespec iw0, iw1;
		clock_gettime(CLOCK_MONOTONIC, &iw0);
		while( p->venc_run && !(frame = frame_q_get( &p->venc_q )) )
			pthread_cond_wait( &p->venc_cond, &p->venc_mutex );
		clock_gettime(CLOCK_MONOTONIC, &iw1);
		p->stat_idle_ns += (int64_t)(iw1.tv_sec - iw0.tv_sec) * 1000000000L +
			           (iw1.tv_nsec - iw0.tv_nsec);

		if( !frame )
			continue;

	/* mpv vsync-skip catch-up: mpv only drops a frame when its
	 * vsync window is GONE (past deadline) AND a fresher frame is
	 * queued behind it. The historical 24-duration threshold (~0.5s)
	 * was calibrated for the ~44/s codec-ceiling regime where the pts
	 * deficit was PERPETUAL and dropping made stutter; that regime is
	 * GONE since the scheduled-presents + latch-feedback landed
	 * (presents sustain 48/s, fblate sub-ms). What 24 durations now
	 * hide is the STANDING VENC BACKLOG: the engine burst-feeds (put
	 * 59/s bursts), the queue banks 15-52 frames (300-1100ms, all
	 * under 24 durations = never drained), and the picture trails the
	 * soundtrack by the queue depth - the audible 'audio slightly
	 * ahead' with every drop counter at zero (measured vencq=51 with
	 * skip=0). Threshold 4 durations (~83ms at 48fps): still ≥2x the
	 * codec dip envelope (a 40/s decode regime leaves frames ≤2
	 * durations stale), but a real backlog drains within ONE present
	 * instead of surviving the whole file.
	 *
	 * CATCH-UP FEED GUARD: 'a fresher frame exists' alone is NOT a
	 * drop condition. During engine catch-up (after any stall) every
	 * fed frame is dated behind the physical anchor - the engine
	 * burst-puts 20 frames in one 50ms window and the first 19 would
	 * each see 'fresher behind' and be eaten at 20+/s (measured:
	 * rend 22-26/s, skip 22-24/s, idle 600ms - the drain starved the
	 * display while eating the very frames sent to fill it, the
	 * returns looped the engine into feeding more, lateness never
	 * closed). A frame is only skippable when the queue is DEEP
	 * enough that skipping it actually reaches fresher content this
	 * instant AND that content is itself already due - otherwise
	 * present late (mpv: a late frame always beats a hole). Requiring
	 * late_by >= 2 durations beyond the freshest frame's remaining
	 * lead keeps real-backlog draining at full rate (deep queue, all
	 * frames overdue) while burst-feed catch-up presents through. */
	while( frame->blit_time > 0 && frame->duration > 0 &&
	       p->venc_run ) {
		int now = dovi_phys_time( p );
		int late_by = now - frame->blit_time;
		if( late_by <= 4 * frame->duration )
			break;			/* still presentable: render it */
		VIDEO_FRAME *next = frame_q_peek( &p->venc_q );
		if( !next )
			break;			/* freshest: present it late (mpv: never drop the last) */
		int next_lead = next->blit_time - now;
		if( next_lead < 0 )
			break;			/* the fresher frame is ALSO past due: the whole feed
					 * trails the anchor (catch-up regime after a stall).
					 * Skipping buys NOTHING here - the next frame is
					 * equally late, so this drop only sheds CONTENT and
					 * widens the A/V gap permanently (measured death
					 * loop: pres 21/s vs skip 27/s for minutes, audio
					 * ahead by the eaten sum, both clocks then advancing
					 * at 1x so the offset never closes). Present every
					 * frame late instead and let the pipeline walk
					 * forward at presentation rate. */
		if( next_lead < frame->duration )
			break;			/* the fresher frame is not due yet: this frame is
					 * the content for RIGHT NOW - present it late */
		p->dropped++;
		p->stat_late_skips++;
		dovi_frame_free_payloads( frame );
		frame->blit_time = -1;
		frame_q_put( &p->get_q, frame );
		frame = frame_q_get( &p->venc_q );
			if( !frame )
				break;
		}

		p->venc_busy = 1;
		/* generation at dequeue: a flush that rebuilds the pool while
		 * this iteration runs (render/present unlock the mutex) must
		 * abandon the frame instead of recycling it - the rebuild
		 * already re-listed every pool frame. */
		int my_gen = p->venc_flush_gen;

		/* FREE-RUN period measurement: refine the exact content
		 * period from consecutive frame blit deltas (double ms->ns;
		 * 23.976fps = 41.708ms cannot ride the int duration field).
		 * WINDOWED ESTIMATOR + CONSISTENCY GATE: the int-ms blit
		 * deltas alternate 41/42 and ONE delta (the original code)
		 * quantizes the whole run to 41.0/42.0ms (measured on GoT:
		 * pres_period_ns=41ms vs real 41.708ms cadence; the chain
		 * then ran ~0.7ms/frame slow, its phase drifted through the
		 * vsync latch grid and forced the doubled 83ms holds - frjag
		 * evidence: d_us=83324, on-time swaps, gate_to=0). A naive
		 * mean window is poisonable by the startup burst (double-
		 * duration 83/84ms head frames averaged 43.4ms into the
		 * first window and the pure-grid chain paced the panel wrong
		 * EVERY second - measured 0049 build). Only deltas within 4ms
		 * of the running mean extend the window; outliers flush it
		 * and the steady 41/42 mix rebuilds it within ~1s. Lock at 24
		 * consistent pairs; a persistent >2ms shift re-locks (real
		 * cadence change, e.g. 24->48fps content mid-file). */
		{
			extern int libavos_get_present_free_run(void);
			if( !p->fr_mode_latched && libavos_get_present_free_run() ) {
				p->fr_mode_latched = 1;
				p->sched_pace = 2;
				serprintf("dovi sink: free-run presents engaged\n");
			}
		}
		/* WINDOWED CONTENT-PERIOD ESTIMATOR (both modes): refines the
		 * exact content period from consecutive frame blit deltas
		 * (double ms->ns; 23.976fps = 41.708ms cannot ride the int
		 * duration field). The refined period feeds the display-
		 * resample ratio in EVERY pacing mode (mode 0 included: its
		 * cadence wait uses the frame deadline, but the beat vs the
		 * panel grid is mode-independent). */
		if( frame->blit_time > 0 &&
		    p->fr_period_last_blit_ms > 0 ) {
			double d_ms = (double)(frame->blit_time - (int)p->fr_period_last_blit_ms);
			if( d_ms > 0.5 && d_ms < 500.0 ) {
				int64_t d_ns = (int64_t)(d_ms * 1000000.0 + 0.5);
				/* CONSISTENCY GATE: a delta only extends the window when it
				 * is within 4ms of the window mean. The startup burst feeds
				 * double-duration frames (83/84ms) and irregular gaps while
				 * sync acquisition runs - a naive window averaged 43.4ms and
				 * the pure-grid chain then paced the panel wrong every
				 * second (measured 0049 build: pres avg 43.4ms, slot-10/s).
				 * The 41/42 steady mix stays in; outliers flush the window
				 * and the steady cadence rebuilds it within ~1s. */
				int consistent = 1;
				if( p->fr_period_pairs > 0 ) {
					int64_t mean_ns = p->fr_period_sum_ns / p->fr_period_pairs;
					int64_t dev_ns = d_ns > mean_ns ?
						d_ns - mean_ns : mean_ns - d_ns;
					if( dev_ns > 4000000LL )
						consistent = 0;
				}
				if( consistent ) {
					p->fr_period_sum_ns += d_ns;
					p->fr_period_pairs++;
				} else {
					p->fr_period_sum_ns = 0;
					p->fr_period_pairs = 0;
				}
			}
			if( p->fr_period_pairs >= 24 ) {
				int64_t mean_ns =
					(p->fr_period_sum_ns + p->fr_period_pairs / 2) /
					p->fr_period_pairs;
				/* lock on first close; re-lock only on a persistent >2ms shift
				 * (real cadence change, e.g. 24->48fps content mid-file) */
				if( mean_ns > 0 &&
				    ( !p->period_refined ||
				      mean_ns > p->pres_period_ns + 2000000LL ||
				      mean_ns < p->pres_period_ns - 2000000LL ) ) {
					serprintf("dovi sink: free-run period %s: %lldus over %d pairs\n",
					          p->period_refined ? "re-refined" : "refined",
					          (long long)(mean_ns / 1000), p->fr_period_pairs);
				p->pres_period_ns = mean_ns;
				p->period_refined = 1;
				}
			}
		}
		/* DISPLAY-RESAMPLE publish (once per playback, both modes):
		 * both rates are locked now - content cadence (TS period)
		 * and the panel's real scan cadence (pres_grid_ns, refined
		 * from actual latches in dovi_fb_apply). If they differ by a
		 * small but non-trivial ratio (the classic 23.976-on-24.000
		 * beat, ~0.1%), publish the audio speed that makes content
		 * wall-cadence EXACTLY the panel grid. In free-run the swap
		 * chain also flips onto the grid; in mode 0 the pacing model
		 * stays as-is - the audio retune alone removes the beat.
		 * Guards: |speed-1| <= 1.5% (covers 23.976->24 and PAL-class
		 * ratios) and >= 0.01% (a 1.0 ratio publishes nothing - the
		 * 48fps-on-48Hz case needs no resample); hints outside the
		 * band are NOT published (the grid estimator misread a mixed
		 * cadence - never resample wild).
		 * The engine applies the hint through stream_set_av_speed
		 * (atempo, timeline mapping) exactly once per generation. */
		if( p->period_refined &&
		    p->pres_grid_ns > 0 && !p->fr_resample_published ) {
			double ratio = (double)p->pres_period_ns / (double)p->pres_grid_ns;
			double dev = ratio > 1.0 ? ratio - 1.0 : 1.0 - ratio;
			p->fr_resample_published = 1;
			if( dev >= 0.0001 && dev <= 0.015 ) {
				extern void libavos_set_display_resample_hint(float);
				serprintf("dovi sink: display-resample engaged: content %lldus on grid %lldus -> audio %.6fx\n",
				          (long long)(p->pres_period_ns / 1000),
				          (long long)(p->pres_grid_ns / 1000),
				          ratio);
				libavos_set_display_resample_hint((float)ratio);
				p->fr_chain_on_grid = 1;
			} else {
				serprintf("dovi sink: display-resample skipped: ratio %.6f (grid %lldus content %lldus)\n",
				          ratio, (long long)(p->pres_grid_ns / 1000),
				          (long long)(p->pres_period_ns / 1000));
			}
		}
		if( frame->blit_time > 0 ) {
			p->fr_last_blit = frame->blit_time;
			p->fr_period_last_blit_ms = (int64_t)frame->blit_time;
		}


		/* lazy content-fps hint: MKV files often leave
		 * video->frame_rate_num/den 0 (avg != r_frame_rate); the frame's
		 * own duration is the ground truth. One shot: once SF accepts the
		 * rate it latches every frame on its own vsync (48Hz mode) - the
		 * deterministic swap cadence the present loop needs to sustain
		 * content rate.
		 *
		 * FREE-RUN MODE 4 sets the hint TOO (the sched_pace!=2 gate is
		 * removed): the hint does NOT change our pacing (the uniform
		 * swap chain stays the pacer) - it only tells Samsung's HRR the
		 * content class so the panel leaves its ~12Hz low-power idle
		 * scan and enters the 24Hz-class video-refresh grid PROMPTLY.
		 * Without it the panel idles at 12Hz for the first seconds of
		 * playback (until its own cadence classifier catches up), the
		 * depth-2 gate starves on 83ms latches, and build 1419 locked
		 * the whole pipeline to 12fps (quarter-speed video, multi-
		 * second stalls). With the hint the panel is in video mode
		 * before the first latches land. */
		if (!p->framerate_set && frame->duration > 0 && p->gl) {
			float fps = 1000.0f / (float) frame->duration;
			if (fps >= 10.0f && fps <= 240.0f &&
			    dovi_gl_set_framerate(p->gl, fps) == 0)
				p->framerate_set = 1;
		}

		/* mpv gpu-next pacing model (draw_frame / flip_page split):
		 * render IMMEDIATELY on arrival (upload + pl_render_image +
		 * submit = all GPU work enqueued early, only glFlush), then
		 * present (eglSwapBuffers, the sole blocking fence-wait with
		 * max_swapchain_depth=3) exactly at the frame's blit_time.
		 * The old order (sleep-then-render) put the whole 4K FEL render
		 * AFTER the deadline wait, presenting every frame render-time
		 * late and tripping the drop policy. */
		int too_late = 0;
		if( frame->blit_time > 0 && frame->duration > 0 ) {
			int now = dovi_phys_time( p );
			/* mpv-strict policy: NEVER pre-empt late frames. The measured
			 * steady state during the bright 4K FEL window is a constant
			 * ~4-5 frame-durations late (GPU-fence backpressure at
			 * max_swapchain_depth=3 + the one-frame pipelined hold), NOT
			 * a growing backlog: forced 719MHz GPU, pool 14 and bilinear
			 * scaler experiments all left DROPPED unchanged - these are
			 * fence cadence artifacts, and rendering them ~170ms late is
			 * one constant presentation delay, invisible to the eye.
			 * Only a run-away backlog (>= 8 frame durations) is worth
			 * dropping. */
			int late_by = now - frame->blit_time;   /* >0 = past deadline. In
			 * free-run (sched_pace==2) lateness reads as ~1-2 pipeline
			 * periods EVERY frame against the audio-anchored phys clock -
			 * that is the constant presentation delay, not backlog: the
			 * forced_late counter ticks per-frame and late/skip stay 0
			 * (guards hold). Only a REAL decode deficit grows lateness. */
			/* 4 durations: mpv drops when the vsync window is GONE and a
			 * fresher frame exists. The 24-duration threshold was a
			 * codec-ceiling-era compromise (see the drain comment above:
			 * that regime is gone); it let the standing venc backlog
			 * (15-52 frames, 300-1100ms) survive the whole file - the
			 * audible 'audio slightly ahead' with all drop counters at
			 * zero. 4 durations drains a real backlog within one present
			 * while staying ≥2x the fence-cadence + decode-dip envelope
			 * (measured steady-state staleness 0-2 durations). */
			if( late_by > 4 * frame->duration ) {
				VIDEO_FRAME *next = frame_q_peek( &p->venc_q );
				int next_lead = next ? (next->blit_time - now) : -1;
				if( next && next_lead < 0 ) {
					/* CATCH-UP REGIME GUARD: the fresher frame is ALSO
					 * past due - the whole feed trails the anchor
					 * (decode-deficit / post-stall catch-up). Skipping
					 * buys NOTHING: the next frame is equally late, so
					 * the drop only sheds CONTENT and widens the A/V
					 * gap. Measured without this guard: an equilibrium
					 * where every engine burst fed past-due frames, the
					 * gate discarded all but the last of each burst
					 * (late 24/s + forced_late 24/s, rend 24/s, idle
					 * 500ms - a self-defeating discard storm). Present
					 * every frame late instead; the audio-side sync gate
					 * (stream_sync_audio) holds the heard position back
					 * to the video head, so lipsync is preserved and the
					 * deficit appears as reduced fps, not desync. */
					p->stat_forced_late++;
				} else if( next ) {
					/* Structural backlog (mpv vsync_skip_detection
					 * analogue): newer frames are queued with deadlines
					 * still in the future - skipping this frame reaches
					 * content that is presentable on time, so this
					 * deadline is gone for good - skip the render and
					 * drain toward the freshest frame; the physical
					 * anchor re-anchors there at the next latch. */
					too_late = 1;
					p->dropped++;
					dovi_frame_free_payloads( frame );
					frame->blit_time = -1;
				} else {
					/* No fresher frame behind it: present this one late
					 * (mpv "Always show the first frame" spirit - a late
					 * frame beats a hole), and note it. */
					p->stat_forced_late++;
				}
			} else {
				p->dropped = 0;
			}
		}

		if( !too_late ) {
			struct timespec r0, r1;
			clock_gettime(CLOCK_MONOTONIC, &r0);
			if( frame->handle[0] && p->gl ) {
				/* hardware path: MediaCodec AHardwareBuffer descriptor */
				dovi_hw_frame *hw = (dovi_hw_frame *) frame->handle[0];
				pthread_mutex_unlock( &p->venc_mutex );
				if( dovi_gl_render_hw(p->gl, hw) != 0 )
					serprintf("dovi sink: hw render failed for frame %d\n", frame->time);
				pthread_mutex_lock( &p->venc_mutex );
				if( hw->release )
					hw->release(hw);
				frame->handle[0] = NULL;
			} else if( frame->priv && p->gl ) {
				pthread_mutex_unlock( &p->venc_mutex );
				if( dovi_gl_render(p->gl, (AVFrame *) frame->priv,
				                   (AVFrame *) frame->handle[1]) != 0 ) {
					serprintf("dovi sink: render failed for frame %d\n", frame->time);
				}
				/* dovi_gl_render consumed the BL/EL AVFrames (SW FEL path):
				 * free them at the same ownership point where the HW path
				 * releases its image - the decoder's render hook is never
				 * called on the dovi pipeline (android3/sim2 only), so the
				 * sink owns the recycle. Leak without this: ~15MB/frame. */
				av_frame_free((AVFrame **) &frame->priv);
				av_frame_free((AVFrame **) &frame->handle[1]);
				pthread_mutex_lock( &p->venc_mutex );
			}
			clock_gettime(CLOCK_MONOTONIC, &r1);
			p->stat_render++;
			p->stat_render_ns += (int64_t)(r1.tv_sec - r0.tv_sec) * 1000000000L + (r1.tv_nsec - r0.tv_nsec);

			/* flip_page, one frame behind (mpv pipelining): present the
			 * PREVIOUS rendered frame at ITS deadline while the current
			 * frame's GPU work is already enqueued. The blocking fence
			 * wait inside swap now overlaps the next render instead of
			 * serializing behind it. Sleep until the pending frame's
			 * blit_time (cap: never wait past the next frame + 100ms);
			 * venc_cond is not signalled on time alone, so poll every
			 * 2ms (flush sets venc_flushing under the lock). */
			if( p->pending_frame && p->gl ) {
				/* --- deadline wait ---
				 * SCHEDULED MODE (eglPresentationTimeANDROID accepted on the
				 * PREVIOUS present): SurfaceFlinger HOLDS each queued buffer
				 * to its target vsync - the compositor IS the pacer (ExoPlayer/
				 * mpv-with-presentation-time model). Sleeping for the same
				 * deadline here would duplicate SF's hold in userspace and
				 * serialize the next render behind it (measured on GoT 4K24:
				 * rend + full-period wait ≈ 48-80ms per iteration > 41.7ms
				 * period, presents sagged to 17.6-18.5/s with every drop
				 * counter at zero). Instead: swap immediately after render,
				 * keep the swapchain fence as the ONLY backpressure (it bounds
				 * in-flight buffers), and let SF latch on the target grid.
				 * UNSCHEDULED FALLBACK (present_at failed/unsupported): the
				 * sleep-to-deadline loop below paces in userspace, cadence-
				 * locked to min(content lead, one period since the last
				 * present). */
			int sched_ok = (p->fb_sched_mode == 1);
			/* Mutex discipline for the whole present section (BOTH modes):
			 * released here, re-taken after the recycle below. The wait
			 * loop, present_at, swap and the feedback poll/apply all run
			 * unlocked - dovi_fb_apply takes the lock itself, so holding
			 * it here self-deadlocks (measured: black screen, emit=0,
			 * outq frozen on vc27 - the first landed latch locked an
			 * already-held mutex). */
			pthread_mutex_unlock( &p->venc_mutex );
			if( p->sched_pace == 2 ) {
				/* --- FREE-RUN "no sync" pacer (GUI mode 4) ---
				 * Swap at a perfectly uniform CONTENT-fps grid: next
				 * swap = last actual swap + exactly one period, the
				 * period measured from consecutive blit_time deltas.
				 * NO vsync chasing: no present_at, no blit-deadline
				 * wait, no latch-feedback pacing correction - the
				 * compositor latches wherever it latches; the constant
				 * pipeline delay is invisible, exactly like a TV driven
				 * by a free-running HDMI source. A late swap (fence
				 * stall) advances the chain from the ACTUAL swap time,
				 * so the grid neither accumulates drift nor jumps to
				 * "catch up". Content lead only decides WHICH frame
				 * (the drop gate above); this block decides WHEN the
				 * swap fires. */
				int64_t period_ns = p->pres_period_ns;
				/* DISPLAY-RESAMPLE: once the panel grid is locked and the
				 * resample hint is published, pace the chain on the MEASURED
				 * panel grid - the audio retune makes content arrive at
				 * exactly this cadence, so swaps and latches stay in one
				 * fixed vsync slot (the 23.976-on-24.000 beat is gone). */
				if( p->fr_chain_on_grid && p->pres_grid_ns > 0 )
					period_ns = p->pres_grid_ns;
				if( period_ns <= 0 && p->pending_frame->duration > 0 )
					period_ns = (int64_t)p->pending_frame->duration * 1000000LL;
				if( period_ns > 0 ) {
					struct timespec fr_now;
					clock_gettime(CLOCK_MONOTONIC, &fr_now);
					int64_t wall_ns = (int64_t)fr_now.tv_sec * 1000000000LL + fr_now.tv_nsec;
					if( p->pres_chain_ns <= 0 ||
					    p->pres_chain_ns < wall_ns - period_ns ) {
						/* first present, or a gap (stall/flush) wider than
						 * one period: re-seed the chain one period out -
						 * never let a backlog of "owed" slots fire a burst */
						p->pres_chain_ns = wall_ns + period_ns;
					} else if( p->pres_chain_ns > wall_ns + 4 * period_ns ) {
						/* chain drifted too far ahead: pull back to one
						 * period out */
						p->pres_chain_ns = wall_ns + period_ns;
					}
					int64_t _t_wait0;
					struct timespec _ts_wait0;
					clock_gettime(CLOCK_MONOTONIC, &_ts_wait0);
					_t_wait0 = (int64_t)_ts_wait0.tv_sec * 1000000 + _ts_wait0.tv_nsec / 1000;
					for( ;; ) {
						clock_gettime(CLOCK_MONOTONIC, &fr_now);
						wall_ns = (int64_t)fr_now.tv_sec * 1000000000LL + fr_now.tv_nsec;
						int64_t remain_ns = p->pres_chain_ns - wall_ns;
						if( remain_ns <= 0 ) {
							p->stat_fr_over_us += (int)((-remain_ns) / 1000);
							p->stat_fr_over_n++;
							break;
						}
						/* keep the latch-feedback anchor alive while we
						 * idle (the audio clock reads phys through
						 * fb_last_latch_ns) */
						if( p->fb_active != 0 && p->gl &&
						    p->pending_frame->blit_time > 0 ) {
							int64_t actual_ns = 0;
							int fbrc = dovi_gl_present_feedback(p->gl, &actual_ns);
							if( p->fb_active < 0 && fbrc == -1 )
								p->fb_active = 0;
							if( fbrc == 0 )
								dovi_fb_apply( p, actual_ns, my_gen );
						}
						/* never hold a frame more than one period past its
						 * chain slot (fence stall safety: swap anyway) */
						if( remain_ns > 2 * period_ns )
							break;
						/* hybrid final approach: a 2ms nap can overshoot
						 * the chain slot by up to 2ms and push the swap
						 * past the vsync boundary (measured 2% doubled
						 * display durations = the visible 2-frame judder).
						 * Under 2.5ms remaining, spin at 100us granularity
						 * instead. */
						if( remain_ns < 2500000LL ) {
							struct timespec spinnap = { 0, 100 * 1000L };
							nanosleep( &spinnap, NULL );
						} else {
							struct timespec nap = { 0, 2 * 1000000L };
							nanosleep( &nap, NULL );
						}
						p->stat_poll_iters++;
						pthread_mutex_lock( &p->venc_mutex );
						int flushing = p->venc_flushing;
						pthread_mutex_unlock( &p->venc_mutex );
						if( flushing )
							break;
					}
					if( p->stat_poll_iters ) {
						struct timespec _ts_wait1;
						clock_gettime(CLOCK_MONOTONIC, &_ts_wait1);
						p->stat_wait_ns += (int64_t)(_ts_wait1.tv_sec - _ts_wait0.tv_sec) * 1000000 +
						                   (_ts_wait1.tv_nsec - _ts_wait0.tv_nsec) / 1000;
					}
					/* EFFECTIVE DEPTH-2 (free-run): the swapchain was created
					 * with depth 6 (the mode global is set after GL open), so
					 * up to 6 frames can sit queued; SurfaceFlinger with
					 * as-available latching can skip a latch to a banked spare
					 * (the measured 0.6% doubled 83ms display holds). Gate the
					 * swap on the PREVIOUS frame having actually LATCHED: at
					 * most one frame is then in flight - the spare is never
					 * available to skip to. Bounded by one period past the
					 * chain slot (a lost feedback sample must not stall the
					 * grid). */
					if( p->fb_active == 1 ) {
						int64_t gwait_t0_ns;
						{
							struct timespec gw0;
							clock_gettime(CLOCK_MONOTONIC, &gw0);
							gwait_t0_ns = (int64_t)gw0.tv_sec * 1000000000LL + gw0.tv_nsec;
						}
						/* previous swap wall = chain - period; a latch at or
						 * after that wall can only be the frame THAT swap
						 * queued (frame N). Wait until such a latch lands:
						 * only then is N on display and N+1 the single
						 * frame in flight. */
					int64_t prev_swap_ns = p->pres_chain_ns - period_ns;
					int64_t last_latch_ns;
					pthread_mutex_lock( &p->venc_mutex );
					last_latch_ns = p->fb_last_latch_ns;
					pthread_mutex_unlock( &p->venc_mutex );
					/* GATE BOUND: ONE 120Hz VSYNC past the chain slot, not one
					 * full period. The old +period bound let a slow-latch window
					 * (Samsung's low-power ~12Hz idle scan for the first seconds
					 * after playback starts, while the panel has not yet entered
					 * its video-refresh mode) push EVERY iteration to
					 * wait(period)+gate(period) = 2 periods = 83ms cadence:
					 * the whole pipeline locked to 12fps, the grid estimator
					 * then measured 83.3ms as the "panel grid" and the video
					 * played at quarter speed with multi-second stalls (measured
					 * build 1419: pres avg 84ms, gate_to=12/s, grid 12.002Hz).
					 * At one vsync past the slot the frame is late anyway - swap
					 * immediately; worst iteration = period + 8.3ms, which the
					 * late-swap re-anchor then folds back into the grid. */
					int64_t gate_bound_ns = 8333333LL;
					if( gate_bound_ns > period_ns / 4 )
						gate_bound_ns = period_ns / 4;
					int64_t inflight_deadline_ns = p->pres_chain_ns + gate_bound_ns;
						while( last_latch_ns < prev_swap_ns ) {
							/* frame N has not latched yet: wait, bounded by
							 * one period past the chain slot (a lost feedback
							 * sample must not stall the grid); 100us spin
							 * keeps the approach phase exact */
							struct timespec gnow;
							clock_gettime(CLOCK_MONOTONIC, &gnow);
							int64_t wall2 = (int64_t)gnow.tv_sec * 1000000000LL + gnow.tv_nsec;
							if( wall2 >= inflight_deadline_ns )
								break;
							int flushing2;
							pthread_mutex_lock( &p->venc_mutex );
							flushing2 = p->venc_flushing;
							pthread_mutex_unlock( &p->venc_mutex );
							if( flushing2 )
								break;
							struct timespec gnap = { 0, 100 * 1000L };
							nanosleep( &gnap, NULL );
							int64_t actual_ns2 = 0;
							if( p->gl && dovi_gl_present_feedback(p->gl, &actual_ns2) == 0 ) {
								dovi_fb_apply( p, actual_ns2, my_gen );
								pthread_mutex_lock( &p->venc_mutex );
								last_latch_ns = p->fb_last_latch_ns;
								pthread_mutex_unlock( &p->venc_mutex );
							}
						}
						/* free-run gate diagnostics: how long this gate spin
						 * waited, and whether it released WITHOUT frame
						 * N's latch (the one-period bound fired - two
						 * frames then went in flight) */
						{
							struct timespec gw;
							int64_t gwait_ns;
							clock_gettime(CLOCK_MONOTONIC, &gw );
							gwait_ns = (int64_t)gw.tv_sec * 1000000000LL + gw.tv_nsec;
							pthread_mutex_lock( &p->venc_mutex );
							p->fr_last_gate_wait_ns = gwait_ns - gwait_t0_ns;
							if( last_latch_ns < prev_swap_ns )
								p->stat_gate_timeout++;
							pthread_mutex_unlock( &p->venc_mutex );
						}
					}
				}
			} else if( !sched_ok ) {
			int _poll_iters = 0;
			int64_t _t_wait0;
			struct timespec _ts_wait0;
			clock_gettime(CLOCK_MONOTONIC, &_ts_wait0);
			_t_wait0 = (int64_t) _ts_wait0.tv_sec * 1000000 + _ts_wait0.tv_nsec / 1000;
			for( ;; ) {
				/* fb_last_latch_ns + fb_phys_blit form a coherent anchor
				 * pair written under venc_mutex (feedback block); snapshot
				 * them together here so dovi_phys_time never mixes halves of
				 * two different latches. */
				int64_t latch_ns;
				int phys_blit;
				pthread_mutex_lock( &p->venc_mutex );
				latch_ns = p->fb_last_latch_ns;
				phys_blit = p->fb_phys_blit;
				pthread_mutex_unlock( &p->venc_mutex );
				int now = dovi_phys_from( p, latch_ns, phys_blit );
				int blit_duration = p->pending_frame->blit_time - now;
				/* CADENCE LOCK (mpv display-sync): never wait past the NEXT
				 * vsync slot on the PRESENT cadence - min(content lead, one
				 * period since the last present). Without this, a banked
				 * queue (vencq 14-60 deep: every frame's blit_time one+
				 * period ahead of phys) makes each iteration wait the FULL
				 * period and the 6-21ms render serializes in front of the
				 * next wait: iteration ≈ 48-63ms > 41.7ms period, presents
				 * sag to 17.6/s on 24fps content while every counter stays
				 * zero (measured vc20-23 on GoT 4K24). Locking the slot
				 * instead lets the render overlap the tail of the wait:
				 * iteration = max(period, rend+overhead) - 24fps holds.
				 * The content lead still selects WHICH frame is due; the
				 * cap only bounds WHEN this present fires (a present can
				 * never be early relative to content: blit_duration <= 2
				 * still breaks first). */
				if( p->pres_wall_ns &&
				    blit_duration > p->pending_frame->duration ) {
					struct timespec cad_ns;
					clock_gettime(CLOCK_MONOTONIC, &cad_ns);
					int64_t wall_ns = (int64_t)cad_ns.tv_sec * 1000000000LL + cad_ns.tv_nsec;
					int64_t since_pres_ns = wall_ns - p->pres_wall_ns;
					int64_t slot_ns =
						(int64_t)p->pending_frame->duration * 1000000LL;
					if( since_pres_ns < slot_ns )
						blit_duration = (int)((slot_ns - since_pres_ns) / 1000000LL);
				}
				if( blit_duration <= 2 )
					break;
				/* OVERLAPPED LATCH FEEDBACK (the serialized post-swap poll
				 * cost 15-50ms serial per present - at 24fps that ate the
				 * next frame's entire wait/render window: measured pres
				 * 13-22/s on 24fps content, pres avg 49-80ms vs the 41.7ms
				 * period, park_drop 5-8/s). The one-behind query is
				 * non-blocking; run it on every wait iteration (2ms cadence,
				 * same detection latency the old poll loop had) and apply
				 * the anchor the moment the latch lands - all inside time
				 * the loop already spends waiting for the deadline. Zero
				 * serial cost; the anchor is fresh BEFORE the next present
				 * target is derived from it. */
				if( p->fb_active != 0 && p->gl &&
				    p->pending_frame->blit_time > 0 ) {
					int64_t actual_ns = 0;
					int fbrc = dovi_gl_present_feedback(p->gl, &actual_ns);
					if( p->fb_active < 0 && fbrc == -1 )
						p->fb_active = 0;	/* probe: only explicit invalid disables (2 = no one-behind frame yet, keep probing) */
					if( fbrc == 0 )
						dovi_fb_apply( p, actual_ns, my_gen );
				}
				/* mpv flip_page semantics: NEVER wait more than ONE frame
				 * period for a deadline. When decode-ahead runs the stream
				 * ahead of the anchor (queue full of FUTURE blit_times),
				 * waiting for the deadline sleeps multiple seconds per
				 * present - measured pres 1-3/s with 0.3-1.3s waits, a
				 * slideshow. The pts selects WHICH frame to show; the vsync
				 * paces WHEN; a future-dated frame shows at the next vsync. */
				{
					int64_t waited_us = 0;
					struct timespec tw1;
					clock_gettime(CLOCK_MONOTONIC, &tw1);
					waited_us = (int64_t)(tw1.tv_sec - _ts_wait0.tv_sec) * 1000000 +
					             (tw1.tv_nsec - _ts_wait0.tv_nsec) / 1000;
					if (waited_us > (int64_t)p->pending_frame->duration * 1000)
						break;
				}
				struct timespec nap = { 0, 2 * 1000000L };
				nanosleep( &nap, NULL );
				_poll_iters++;
				pthread_mutex_lock( &p->venc_mutex );
				int flushing = p->venc_flushing;
				pthread_mutex_unlock( &p->venc_mutex );
				if( flushing )
					break;
			}
				p->stat_poll_iters += _poll_iters;
				if( _poll_iters ) {
					struct timespec _ts_wait1;
					clock_gettime(CLOCK_MONOTONIC, &_ts_wait1);
					p->stat_wait_ns += (int64_t) (_ts_wait1.tv_sec - _ts_wait0.tv_sec) * 1000000000L + (_ts_wait1.tv_nsec - _ts_wait0.tv_nsec);
				}
			} /* !sched_ok: userspace deadline wait */
			/* SCHEDULED MODE latch poll: the wait loop is skipped when SF
			 * owns pacing, so its overlapped poll does not run - poll the
			 * one-behind sample HERE (unlocked section, non-blocking) so
			 * latch samples keep flowing and the tightening-chain target
			 * above always derives from a FRESH latch. Without this the
			 * only poll left was the post-swap check, which runs before
			 * the previous frame's latch lands (always PENDING, measured
			 * fb=0 on vc28) and the physical anchor never updates. */
			if( p->fb_active != 0 && p->gl &&
			    p->pending_frame && p->pending_frame->blit_time > 0 ) {
				int64_t actual_ns = 0;
				int fbrc = dovi_gl_present_feedback(p->gl, &actual_ns);
				/* BOUNDED RE-POLL: the one-behind frame's target can still
				 * be a few ms out when polled once per iteration; the wait
				 * loop's 2ms poll cadence is gone in scheduled mode, and a
				 * single PENDING miss loses that sample FOREVER (prev_frame_id
				 * rotates at the next present - measured fb=0 on vc28/29:
				 * every poll hit PENDING, nothing ever landed). Nap-retry up
				 * to 5x2ms (10ms) - the loop idles 43-51ms in swapchain
				 * acquire in this mode, so the re-poll is effectively free.
				 * FREE-RUN EXCLUSION: never nap here when the uniform swap
				 * chain owns pacing (sched_pace==2) - this block sits between
				 * the chain-slot break and eglSwapBuffers, and up to 10ms of
				 * re-poll naps would shift the swap past its slot per frame
				 * (measured: the residual doubled 83ms display holds + a
				 * weak frov/swap-delay correlation - frov only counted the
				 * wait-break overshoot, not this delay). The free-run wait
				 * loop already polls feedback at its 100us-2ms cadence, so
				 * a single non-blocking sample here keeps the anchor fresh
				 * with zero delay added to the swap path. */
				int fb_tries = 0;
				while( fbrc == 1 && fb_tries < 5 && p->sched_pace != 2 ) {
					struct timespec fbnap = { 0, 2 * 1000000L };
					nanosleep( &fbnap, NULL );
					fb_tries++;
					fbrc = dovi_gl_present_feedback(p->gl, &actual_ns);
				}
				if( p->fb_active < 0 && fbrc == -1 )
					p->fb_active = 0;
				if( fbrc == 0 )
					dovi_fb_apply( p, actual_ns, my_gen );
			}
				/* --- scheduled present: hand SurfaceFlinger the desired latch
				 * time for the pending frame's content deadline (mapped through
				 * the anchor pair: blit_time is content TS ms, the anchor maps it
				 * to CLOCK_MONOTONIC ms; eglPresentationTimeANDROID takes ns).
				 * On success SF paces the latch itself - the fine-grained 2ms
				 * deadline poll above becomes a no-op safety net, and on a 60Hz
				 * panel 48fps content gets SF's own 5:4 vsync allocation instead
				 * of racing the compositor. On -1 (unsupported) the swap runs
				 * unscheduled, exactly the old behavior. */
				int scheduled = -1;
				int64_t target_ns = 0;
				struct timespec tA, tB;
				clock_gettime(CLOCK_MONOTONIC, &tA);
				if( p->pending_frame && p->pending_frame->blit_time > 0 ) {
					/* PHYSICAL-timeline target: last ACTUAL latch + one frame
					 * duration (content cadence), NOT wall-now + (blit_time -
					 * clock): the sink clock carries the A/V offset baked in
					 * from the audio anchor (measured ~535ms ahead of true
					 * presentation), so clock-derived targets land half a
					 * second early, SF latches them 'when available', and the
					 * feedback poll saturates at 28ms PENDING per present
					 * (presents pinned 30/s, skip 18/s). Anchoring to the last
					 * real latch keeps the target within one vsync of physical
					 * presentation, and the CONTENT deadline still selects
					 * WHICH frame shows. Fallback while no latch has landed yet
					 * (first frames): wall-now + lead through the clock, as
					 * before - coarse but only until the first feedback. */
				int dur_ms = p->pending_frame->duration > 0 ?
				             p->pending_frame->duration : 21;
				if( p->fb_active == 1 && p->fb_last_latch_ns > 0 ) {
					/* TIGHTENING CHAIN target (scheduled mode, mpv
					 * vsync_offset): target = max(last ACTUAL latch + one
					 * period, this frame's content-mapped deadline). With no
					 * userspace wait loop in scheduled mode, the only mechanism
					 * that pulls the cadence back after a slow latch is the
					 * TARGET itself: a pure wall+lead content-grid target
					 * accepts whatever rate SF settles into and the swapchain
					 * acquire follows it down (measured: SF drifted to ~65ms
					 * latches, rend blocked 64-80ms on acquire, pres 13-16/s,
					 * fb=0). latched+dur never accepts a late latch as the new
					 * cadence - every target recomputes from the freshest
					 * latch plus exactly one period, so the cadence
					 * re-tightens at the next sample; the max() with the
					 * content deadline keeps a banked frame from presenting
					 * before its content time, and the one-period ceiling
					 * keeps banked queues from pushing targets out. */
					struct timespec now_ns;
					clock_gettime(CLOCK_MONOTONIC, &now_ns);
					int64_t wall_ns = (int64_t)now_ns.tv_sec * 1000000000LL + now_ns.tv_nsec;
					int64_t chain_ns = p->fb_last_latch_ns +
						(int64_t)dur_ms * 1000000LL;
					int phys_now = dovi_phys_time( p );
					int lead_ms = p->pending_frame->blit_time - phys_now;
					if( lead_ms < 0 )
						lead_ms = 0;
					int64_t content_ns = wall_ns + (int64_t)lead_ms * 1000000LL;
					target_ns = chain_ns > content_ns ? chain_ns : content_ns;
					if( target_ns < wall_ns )
						target_ns = wall_ns;
					if( target_ns > content_ns + (int64_t)dur_ms * 1000000LL )
						target_ns = content_ns + (int64_t)dur_ms * 1000000LL;
				} else {
						int lead_ms = p->pending_frame->blit_time - dovi_sink_get_time( p );
						struct timespec now_ns;
						clock_gettime(CLOCK_MONOTONIC, &now_ns);
						int64_t wall_ns = (int64_t)now_ns.tv_sec * 1000000000LL + now_ns.tv_nsec;
						target_ns = wall_ns + (int64_t)lead_ms * 1000000LL;
					}
						/* A/B gate (24fps present-rate defect): present_at makes
						 * SF HOLD each buffer to its target - at 120Hz with
						 * Samsung's HRR votes overriding the 24Hz panel mode, SF's
						 * held-buffer latch cadence runs 50-80ms and starves the
						 * swapchain acquire (rend blocked 43-74ms, pres 18/s
						 * measured on vc28-31). sched_pace=0 keeps SF latching
						 * as-available (instant acquire) and the userspace
						 * cadence-locked wait loop paces presents instead. */
						/* sched_pace==2 (free-run): no present_at at all -
						 * the uniform swap chain paces, SF latches
						 * as-available. */
						if( p->sched_pace == 2 )
							scheduled = -2;
						else if( p->sched_pace )
							scheduled = dovi_gl_present_at(p->gl, target_ns);
						p->fb_sched_mode = (scheduled == 0);
					}
				clock_gettime(CLOCK_MONOTONIC, &tB);
				p->stat_pres_sched_ns += (int64_t)(tB.tv_sec - tA.tv_sec) * 1000000000L + (tB.tv_nsec - tA.tv_nsec);
				/* DEPTH-2 INFLIGHT GATE (mode 0): the swapchain runs depth 6
				 * and SF latches as-available in this mode, so a banked spare
				 * lets the compositor skip a latch to the newest queued buffer
			 * - the user sees one stale frame then a snap forward (mode-0
				 * measured: 154 slot-10 doubles / 8min, 1.67%). Gate this swap
				 * on the PREVIOUS frame having actually LATCHED, same
				 * discipline as the free-run gate: at most one frame in
				 * flight, the spare is never available to skip to. Anchors:
				 * the previous swap wall (pres_wall_ns) and the newest latch
				 * (fb_last_latch_ns). Bounded by one vsync past THIS frame's
				 * wait target (target_ns, computed above; when no latch has
				 * landed yet the gate is a no-op) - a lost feedback sample
				 * must not stall the cadence. */
				if( p->sched_pace != 2 && p->fb_active == 1 &&
				    p->fb_last_latch_ns > 0 && p->pres_wall_ns > 0 &&
				    p->gl ) {
					int flushing_g;
					int64_t prev_swap_ns = p->pres_wall_ns;
					int64_t last_latch_ns;
					pthread_mutex_lock( &p->venc_mutex );
					last_latch_ns = p->fb_last_latch_ns;
					flushing_g = p->venc_flushing;
					pthread_mutex_unlock( &p->venc_mutex );
					while( !flushing_g && last_latch_ns < prev_swap_ns ) {
						struct timespec gnow;
						clock_gettime(CLOCK_MONOTONIC, &gnow);
						int64_t gwall_ns = (int64_t)gnow.tv_sec * 1000000000LL + gnow.tv_nsec;
						int64_t bound_ns = ( target_ns > 0 ?
						                    target_ns : prev_swap_ns ) + 8333333LL;
						if( gwall_ns >= bound_ns )
							break;
						struct timespec gnap = { 0, 100 * 1000L };
						nanosleep( &gnap, NULL );
						int64_t actual_ns_g = 0;
						if( dovi_gl_present_feedback(p->gl, &actual_ns_g) == 0 ) {
							dovi_fb_apply( p, actual_ns_g, my_gen );
							pthread_mutex_lock( &p->venc_mutex );
								last_latch_ns = p->fb_last_latch_ns;
								flushing_g = p->venc_flushing;
							pthread_mutex_unlock( &p->venc_mutex );
						}
					}
				}
				clock_gettime(CLOCK_MONOTONIC, &tA);
				dovi_gl_present(p->gl);
				clock_gettime(CLOCK_MONOTONIC, &tB);
				p->stat_pres_swap_ns += (int64_t)(tB.tv_sec - tA.tv_sec) * 1000000000L + (tB.tv_nsec - tA.tv_nsec);
				p->pres_wall_ns = (int64_t)tB.tv_sec * 1000000000LL + tB.tv_nsec;
				if( p->sched_pace == 2 ) {
					/* FREE-RUN chain advance (PURE GRID): the next slot is
					 * the PREVIOUS slot + exactly one refined period - NOT
					 * swap completion + period. The old wall+period advance
					 * leaked the swap duration (~0.65ms) + frov overshoot
					 * into the grid EVERY frame: with the quantized 41ms
					 * period the two errors nearly cancelled (net ~41.74ms
					 * measured), but they made the grid rate depend on
					 * per-frame overhead jitter - each present's slot-time
					 * wobbled independently and crossed the vsync latch
					 * boundary stochastically (the residual slot-10 doubles:
					 * frjag d_us=83324 with on-time swaps, gate_to=0).
					 * A pure slot+=period grid is exactly the content
					 * cadence; the wait loop re-seeds on any stall/gap wider
					 * than one period (pres_chain_ns < wall - period) and
					 * pulls back if it drifts >4 periods ahead, so a late
					 * swap still shifts the grid (never a catch-up burst).
					 *
					 * LATE-SWAP RE-ANCHOR (oscillation killer): a pure grid
					 * that ignores swap reality is METASTABLE - once one
					 * swap lands late (gate timeout / compositor hiccup),
					 * the NEXT slot's deadline is still nominal, the wait
					 * cannot make up the delay, the next gate times out too,
					 * and the system locks into a 3-frame limit cycle (16.6
					 * -> 25 -> 83ms latch deltas, gate_to climbing 0->12,
					 * measured diag6 13:59: 89 frjag in 5s, the visible
					 * judder). Re-anchor to the ACTUAL swap wall whenever it
					 * completed more than 2ms past the slot (swap jitter under
					 * 2ms stays on the exact grid - no beat reintroduced);
					 * the next slot is then reachable, the next gate sees its
					 * latch, and the cycle collapses back to clean 5-slots
					 * within one frame. */
					int64_t period_ns = p->pres_period_ns;
					if( p->fr_chain_on_grid && p->pres_grid_ns > 0 )
						period_ns = p->pres_grid_ns;
					if( period_ns <= 0 && p->pending_frame &&
					    p->pending_frame->duration > 0 )
						period_ns = (int64_t)p->pending_frame->duration * 1000000LL;
					if( period_ns > 0 ) {
						int64_t next_nominal_ns = ( p->pres_chain_ns > 0 ) ?
							 p->pres_chain_ns + period_ns :
							 p->pres_wall_ns + period_ns;
						if( p->pres_wall_ns > p->pres_chain_ns + 2000000LL )
						next_nominal_ns = p->pres_wall_ns + period_ns;
						p->pres_chain_ns = next_nominal_ns;
					}
					p->fb_sched_mode = 0;
				}
				p->stat_present++;
				/* --- actual-latch feedback: NON-BLOCKING final check. The
				 * one-behind sample is now polled inside the deadline wait
				 * loop (overlapped, zero serial cost) - this single query
				 * covers the case where the wait loop never ran (deadline
				 * already due: burst-fed or behind regime) so the anchor
				 * still lands before the next target is derived from it.
				 * A PENDING result here is fine: the next frame's wait
				 * loop picks the sample up. Capability probe: only an
				 * explicit invalid (-1) disables the loop for good. */
				if( p->fb_active != 0 && p->pending_frame &&
				    p->pending_frame->blit_time > 0 && p->gl ) {
					int64_t actual_ns = 0;
					struct timespec tF0, tF1;
					clock_gettime(CLOCK_MONOTONIC, &tF0);
					int fbrc = dovi_gl_present_feedback(p->gl, &actual_ns);
					if( p->fb_active < 0 && fbrc == -1 )
						p->fb_active = 0;
					clock_gettime(CLOCK_MONOTONIC, &tF1);
					p->stat_pres_fbpoll_ns += (int64_t)(tF1.tv_sec - tF0.tv_sec) * 1000000000L + (tF1.tv_nsec - tF0.tv_nsec);
					if( fbrc == 0 ) {
						dovi_fb_apply( p, actual_ns, my_gen );
						if( scheduled == 0 )
							p->stat_fb_late_us += (int)(actual_ns / 1000 -
								(target_ns / 1000));
					}
				}
				clock_gettime(CLOCK_MONOTONIC, &r1);
				p->stat_present_ns += (int64_t)(r1.tv_sec - r0.tv_sec) * 1000000000L + (r1.tv_nsec - r0.tv_nsec);
				/* re-lock for the 1Hz stats + recycle: frame_q_count on
				 * venc_q races sink_put's frame_q_put when read unlocked */
				pthread_mutex_lock( &p->venc_mutex );
				/* 1Hz pacing stats: render/present counts and mean durations
				 * (µs) + queue backlogs - pinpoints the fps limiter live */
				{
					static int64_t last_us;
					struct timespec s0;
					clock_gettime(CLOCK_MONOTONIC, &s0);
					int64_t now_us = (int64_t) s0.tv_sec * 1000000 + s0.tv_nsec / 1000;
						if (last_us && now_us - last_us >= 1000000) {
						// pi-lens-ignore: typos
						serprintf("dovi sink: put=%d rend=%d (avg %dus) pres=%d (avg %dus) [sw=%dus fb=%dus sch=%dus] poll_it=%d wait=%dms vencq=%d late=%d forced_late=%d skip=%d idle=%dms clock=%d phys=%d span=%dms fb=%d fblate=%dus frov=%d/%dus gate_to=%d latchslots=",
						          p->stat_put,
						          p->stat_render, p->stat_render ? (int)(p->stat_render_ns / p->stat_render / 1000) : 0,
						          p->stat_present, p->stat_present ? (int)(p->stat_present_ns / p->stat_present / 1000) : 0,
						          p->stat_present ? (int)(p->stat_pres_swap_ns / p->stat_present / 1000) : 0,
						          p->stat_present ? (int)(p->stat_pres_fbpoll_ns / p->stat_present / 1000) : 0,
						          p->stat_present ? (int)(p->stat_pres_sched_ns / p->stat_present / 1000) : 0,
						          p->stat_poll_iters, (int)(p->stat_wait_ns / 1000000),
						          frame_q_count(&p->venc_q), p->dropped, p->stat_forced_late,
						          p->stat_late_skips,
						          (int)(p->stat_idle_ns / 1000000),
						          dovi_sink_get_time(p),
						          dovi_phys_time(p),
						          p->stat_time0 >= 0 ? (p->stat_time_last - p->stat_time0) : -1,
						          p->stat_fb_used,
						          p->stat_fb_samples ? (p->stat_fb_late_us / p->stat_fb_samples) : 0,
						          p->stat_fr_over_n,
						          p->stat_fr_over_n ? (p->stat_fr_over_us / p->stat_fr_over_n) : 0,
						          p->stat_gate_timeout);
						for( int _si = 0; _si < 12; _si++ )
							serprintf("%s%d:%d", _si ? "," : "", _si + 1, p->fb_slot_hist[_si]);
						serprintf("\n");
						p->stat_put = p->stat_render = p->stat_present = 0;
						p->stat_forced_late = 0;
						p->stat_poll_iters = 0;
						p->stat_wait_ns = 0;
						p->stat_idle_ns = 0;
						p->stat_late_skips = 0;
						p->stat_render_ns = p->stat_present_ns = 0;
						p->stat_fb_used = 0;
						p->stat_fb_late_us = 0;
						p->stat_fb_samples = 0;
						p->stat_pres_swap_ns = 0;
						p->stat_pres_fbpoll_ns = 0;
						p->stat_pres_sched_ns = 0;
						p->stat_fr_over_us = p->stat_fr_over_n = 0;
						p->stat_gate_timeout = 0;
						for( int _si = 0; _si < 12; _si++ )
							p->fb_slot_hist[_si] = 0;
						p->stat_time0 = p->stat_time_last = -1;
						last_us = now_us;
					} else if (!last_us)
						last_us = now_us;
				}
				/* (mutex already re-taken above) */
				/* flush raced this iteration (present ran unclocked): the
				 * rebuild owns the pending frame - drop the reference,
				 * do not put it back */
				if( p->venc_flush_gen != my_gen ) {
					p->pending_frame = NULL;
					goto flushed_away;
				}
				/* the presented frame returns to the pool for reuse */
				frame_q_put( &p->get_q, p->pending_frame );
				/* one-behind rotation: the feedback query in the NEXT
					 * iteration answers for THIS just-queued frame - keep
					 * its content position for the re-anchor. Captured at
					 * recycle time (the correct one-behind slot): the
					 * incoming frame below is one too early. */
				p->fb_prev_blit_time = p->pending_frame->blit_time;
				p->pending_frame = NULL;
			}
				p->pending_frame = frame;
			goto endloop_skip;	/* frame stays pending; recycled after present */
		}
endloop:
		/* flush raced this iteration (render ran unclocked): the rebuild
		 * owns this frame - its payloads were consumed by the render, so
		 * dropping the reference (not putting it back) is the correct recycle */
		if( p->venc_flush_gen != my_gen )
			goto flushed_away;
		frame_q_put( &p->get_q, frame );
endloop_skip:
flushed_away:
		p->venc_busy = 0;
		pthread_cond_broadcast( &p->venc_cond );
	}
	pthread_mutex_unlock( &p->venc_mutex );
	return NULL;
}

static int sink_open(STREAM_SINK_VIDEO *sink, VIDEO_PROPERTIES *video, void *ctx, int num_frames, STREAM_RC *rc)
{
	priv_t *p = sink->priv;
	int i;

	if (sink->is_open)
		return 0;   /* resize re-open: renderer and frames stay alive */

	if (!p->surface_handle) {
		serprintf("dovi sink: no surface handle\n");
		return 1;
	}

	/* frame pool owned by the sink; clamp to a sane range */
	p->num_frames = num_frames;
	if (p->num_frames < 2)
		p->num_frames = 2;
	if (p->num_frames > DOVI_SINK_MAX_FRAMES)
		p->num_frames = DOVI_SINK_MAX_FRAMES;

	for (i = 0; i < p->num_frames; i++) {
		p->frames[i] = (VIDEO_FRAME *) acalloc(1, sizeof(VIDEO_FRAME));
		if (!p->frames[i]) {
			serprintf("dovi sink: cannot alloc frame %d\n", i);
			return 1;
		}
		/* no pixel buffers: decoder delivers AVFrames in frame->priv */
		p->frames[i]->index   = i;
		p->frames[i]->user_ID = i;
	}

	frame_q_init(&p->get_q, "dovi_get");
	frame_q_init(&p->venc_q, "dovi_venc");
	pthread_mutex_init(&p->venc_mutex, NULL);
	pthread_cond_init(&p->venc_cond, NULL);
	p->venc_flushing = 0;	/* a close/re-open cycle leaves it set */
	p->venc_flush_gen = 0;
	p->framerate_set = 0;	/* re-hint on re-open (new content fps) */
	p->fb_active = -1;	/* presentation-feedback capability unknown: probe on first present */
	p->fb_locked = 0;	/* sync-acquisition jump re-arms on re-open */
	p->fb_rate_ppm = 0;	/* latch-feedback rate trim starts neutral */
	p->fb_last_latch_ns = 0;
	p->fb_phys_blit = 0;
	p->fb_prev_blit_time = 0;
	p->fb_sched_mode = 0;	/* userspace wait until the first accepted target */
	/* "No sync" free-run mode (GUI refresh-rate sync == 4): the Java
	 * Player sets the libavos global in onPrepared, which can land
	 * AFTER the sink opened - so read it again at the first frame and
	 * lock the pacing mode in for the rest of the stream (per-iteration
	 * reads would let a mid-playback pref flip break the chain math). */
	p->sched_pace = 0;
	p->fr_mode_latched = 0;
	p->pres_wall_ns = 0;	/* present cadence restarts on open */
	p->pres_chain_ns = 0;	/* free-run chain restarts on open */
	p->pres_period_ns = 0;	/* re-measure the content period per stream */
	p->fr_last_blit = 0;
	p->fb_prev_latch_ns = 0;
	p->stat_fb_late_us = p->stat_fb_samples = p->stat_fb_used = 0;
	for( int _si = 0; _si < 12; _si++ )
		p->fb_slot_hist[_si] = 0;

	/* pre-queue all frames as free (mirrors android2's sink_open): the
	 * stream's _queue_sink_frames pulls them via sink->get into decode_q,
	 * so decoding can start before the first rendered frame comes back */
	for (i = 0; i < p->num_frames; i++)
		frame_q_put(&p->get_q, p->frames[i]);

	if (dovi_gl_open(&p->gl, p->surface_handle) != 0) {
		serprintf("dovi sink: dovi_gl_open failed\n");
		return 1;
	}

	/* content frame-rate hint (API 30+, mpv/ExoPlayer parity): ask SF to
	 * switch the panel to content rate. On this device (S24) 48Hz is a
	 * supported seamless mode. Presenting 48fps at 120Hz gives a 2.5:1
	 * vsync cadence (judder) and ~19.5ms unpredictable swap fences; at
	 * 48Hz each frame latches its own vsync (deterministic 20.83ms
	 * cycle). frame_rate_num/den can be 0 (MKV parser sets them only
	 * when avg==r_frame_rate): the venc thread then sets the hint from
	 * frame->duration on the first frame. */
	{
		float fps = 0.0f;
		if (video->frame_rate_num > 0 && video->frame_rate_den > 0)
			fps = (float) video->frame_rate_num / (float) video->frame_rate_den;
		if (fps > 0.0f && dovi_gl_set_framerate(p->gl, fps) == 0)
			p->framerate_set = 1;
	}

	/* start the pacing/render thread (android2 venc_thread parity) */
	p->venc_run = 1;
	if (pthread_create(&p->venc_thread_handle, NULL, dovi_venc_thread, sink) != 0) {
		serprintf("dovi sink: cannot start render thread\n");
		p->venc_run = 0;
		dovi_gl_close(p->gl);
		p->gl = NULL;
		return 1;
	}

	sink->is_open = 1;
	return 0;
}

static int sink_close(STREAM_SINK_VIDEO *sink)
{
	priv_t *p = sink->priv;

	if (!sink->is_open)
		return 0;

	/* stop the render thread and drain pending frames */
	if (p->venc_run) {
		p->venc_run = 0;
		pthread_mutex_lock(&p->venc_mutex);
		p->venc_flushing = 1;
		pthread_cond_broadcast(&p->venc_cond);
		pthread_mutex_unlock(&p->venc_mutex);
		/* the thread parks with at most one rendered-but-unpresented
		 * frame (pipelined flip_page); its payloads were already
		 * consumed by the render - after the join the pool is torn
		 * down, so just drop the reference (the loop's exit means
		 * the thread will never present it). */
		pthread_join(p->venc_thread_handle, NULL);
		p->pending_frame = NULL;
		/* anything still queued for render never got rendered: free
		 * the payloads it still owns (hw images, BL/EL AVFrames,
		 * ~15MB each) before the pool is discarded below */
		for (;;) {
			VIDEO_FRAME *fr = frame_q_get(&p->venc_q);
			if (!fr)
				break;
			dovi_frame_free_payloads( fr );
		}
	}

	if (p->gl) {
		dovi_gl_close(p->gl);
		p->gl = NULL;
	}

	/* everything the sink holds is in get_q now */
	while (frame_q_get(&p->get_q) != NULL)
		;

	sink->is_open = 0;
	return 0;
}

static int sink_delete(STREAM_SINK_VIDEO *sink)
{
	priv_t *p = sink->priv;
	int i;

	if (sink->is_open)
		sink_close(sink);

	if (p) {
		for (i = 0; i < DOVI_SINK_MAX_FRAMES; i++) {
			if (p->frames[i]) {
				/* a frame may still hold an unrendered BL/EL AVFrame
				 * pair (flush raced the render path): free it, the
				 * decoder is gone and will not reclaim it */
				dovi_frame_free_payloads( p->frames[i] );
				afree(p->frames[i]);
			}
		}
		afree(p);
	}
	afree(sink);
	return 0;
}

static VIDEO_FRAME *sink_get_frame(STREAM_SINK_VIDEO *sink, int index)
{
	priv_t *p = sink->priv;

	if (index < 0 || index >= p->num_frames)
		return NULL;
	return p->frames[index];
}

static int sink_put(STREAM_SINK_VIDEO *sink, VIDEO_FRAME *frame)
{
	priv_t *p = sink->priv;

	if (!sink->is_open || !frame)
		return 0;
	p->stat_put++;

	/* android2 venc_thread parity: hand the frame to the render thread,
	 * which paces it against the anchored presentation clock.
	 *
	 * NO sink-side drop here: the engine's own video_sink_count <
	 * stream_sink_video_max(5) gate throttles puts (queue + pending +
	 * busy are all in-flight from its perspective). A sink-side cap at
	 * that same boundary collided with the gate every cycle and
	 * churn-dropped ~19/s of frames the engine believed safely queued
	 * (measured at cap 5: late +19/s, visible skips). Stale frames are
	 * handled where their lateness is known: the venc dequeue drain.
	 * Sanity bound only (pool size) against pool exhaustion. */
	pthread_mutex_lock( &p->venc_mutex );
	while (frame_q_count(&p->venc_q) >= DOVI_SINK_MAX_FRAMES) {
		VIDEO_FRAME *old = frame_q_get(&p->venc_q);
		if (!old)
			break;
		p->dropped++;
		dovi_frame_free_payloads(old);
		old->blit_time = -1;
		frame_q_put(&p->get_q, old);
	}
	frame_q_put( &p->venc_q, frame );
	if (p->stat_time0 == -1)
		p->stat_time0 = frame->time;
	p->stat_time_last = frame->time;
	pthread_cond_signal( &p->venc_cond );
	pthread_mutex_unlock( &p->venc_mutex );
	return dovi_sink_get_time(p);
}

static int sink_get(STREAM_SINK_VIDEO *sink, VIDEO_FRAME **pframe)
{
	priv_t *p = sink->priv;
	VIDEO_FRAME *frame;

	if (!sink->is_open) {
		*pframe = NULL;
		return 1;
	}

	/* only frames already rendered (or dropped) by the venc thread are
	 * handed back for reuse (android2 dequeue parity). The queues are
	 * plain frame_q lists with no internal locking: every access must be
	 * serialized against the venc thread, which also puts to get_q. */
	pthread_mutex_lock(&p->venc_mutex);
	frame = frame_q_get(&p->get_q);
	pthread_mutex_unlock(&p->venc_mutex);
	*pframe = frame;
	return frame ? 0 : 1;
}

static int sink_flush(STREAM_SINK_VIDEO *sink)
{
	priv_t *p = sink->priv;
	int i;

	if (!sink->is_open)
		return 0;

	/* _free_all_frames contract: the stream has flushed all its queues
	 * (frame_q_flush on decode_q/disp_q/locked_q/codec_q) and cleared
	 * every ->locked flag before calling us, so no pool frame is held by
	 * the stream anymore. Reclaim the entire pool, like android2 reclaims
	 * its buffers from the display pipeline (sink_flush_and_enqueue).
	 *
	 * The venc thread must be parked first: wake it out of any pace-wait
	 * (venc_flushing breaks the poll loop) and wait until venc_busy
	 * clears, so the frame it holds is back in a queue and cannot be
	 * double-put by the rebuild below (measured: 'frame_q_put FATAL
	 * already in [dec]' wedging the whole pool). */
	pthread_mutex_lock(&p->venc_mutex);
	p->venc_flushing = 1;
	/* bump the generation FIRST: if the thread is mid-iteration (its
	 * render/present run with the mutex released, and a 4K FEL present
	 * can legitimately block >50ms on the swapchain fence), it will see
	 * the new generation when it re-locks and abandon its held frame
	 * instead of recycling it into the freshly rebuilt pool - that put
	 * would double-list the frame (the measured 'frame_q_put FATAL
	 * already in [dec]' wedge). The generation makes the bounded wait
	 * below purely an optimization: even if it times out, the thread's
	 * late puts are now harmless no-op references, never queue writes. */
	p->venc_flush_gen++;
	/* SEEK EPOCH RE-ARM: sync acquisition is per-EPOCH, not per-open.
	 * The engine does NOT re-open the sink on a seek - it only flushes
	 * - so without this the one-time jump flag stays set from the
	 * pre-seek stream and the FIRST latch after the seek goes through
	 * the rate-trim path (clamped ±0.5%/s) instead of re-anchoring: the
	 * phys clock stays on the OLD timeline while the engine's media
	 * clock jumps to the seek target, every present deadline computes
	 * hundreds of seconds into the future, and the sink presents one
	 * frame per ~2s (measured 12:07 run: pres avg 1.9s, idle=1789ms,
	 * phys frozen at the pre-seek position while clock=+548s; the
	 * venc pipeline then parks 12 BLs, el_fed/el_out wedge, and
	 * playback freezes). fb_locked re-arms here so dovi_fb_apply's
	 * next landed latch re-anchors in ONE jump, mpv seek-epoch
	 * semantics - same as the open path (fb_locked=0). The rate trim
	 * and the latch anchor also reset: both are epoch-local. */
	p->fb_locked = 0;
	p->fb_rate_ppm = 0;
	p->fb_last_latch_ns = 0;
	p->fb_prev_latch_ns = 0;
	pthread_cond_broadcast(&p->venc_cond);
	while (p->venc_busy && p->venc_run) {
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 50 * 1000000L;
		if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
		if (pthread_cond_timedwait(&p->venc_cond, &p->venc_mutex, &ts) == ETIMEDOUT)
			break;	// thread stuck: safe now - the generation check keeps
				// its late recycles out of the rebuilt pool
	}

	/* the thread parks with at most one rendered-but-unpresented frame
	 * (pipelined flip_page): drop our reference WITHOUT putting it back -
	 * the rebuild below re-lists every pool frame exactly once, and a
	 * put here would double-queue it. Its payloads were already consumed
	 * by the render, so only the reference needs clearing. */
	p->pending_frame = NULL;

	/* drain the render queue: these frames were never rendered; free
	 * the payloads they still own (hw image / BL+EL AVFrames) or every
	 * flush leaks ~15MB per frame, then discard - the rebuild re-lists
	 * each pool frame once. */
	for (;;) {
		VIDEO_FRAME *fr = frame_q_get(&p->venc_q);
		if (!fr)
			break;
		dovi_frame_free_payloads( fr );
	}

	/* rebuild the free pool from scratch, still under the lock so the
	 * parked thread cannot interleave a put: empty get_q first (it holds
	 * a mix of rendered and reclaimed frames at this point), then enqueue
	 * every pool frame exactly once. */
	frame_q_flush(&p->get_q);
	for (i = 0; i < p->num_frames; i++) {
		p->frames[i]->time = -1;
		p->frames[i]->locked = 0;
		frame_q_put(&p->get_q, p->frames[i]);
	}
	pthread_mutex_unlock(&p->venc_mutex);
	return 0;
}

static int sink_end(STREAM_SINK_VIDEO *sink)
{
	return 0;
}

static int sink_syncable(STREAM_SINK_VIDEO *sink)
{
	return 0;
}

static int sink_delay(STREAM_SINK_VIDEO *sink)
{
	return 0;
}

static int sink_get_time(STREAM_SINK_VIDEO *sink)
{
	priv_t *p = sink->priv;
	return dovi_sink_get_time(p);
}

static int sink_put_time(STREAM_SINK_VIDEO *sink, int time)
{
	priv_t *p = sink->priv;
	/* under the mutex: venc_put_time + venc_ref_time form one anchor
	 * pair; the venc thread reads both unlocked, so a torn update would
	 * pace one present off the new time against the old wall base.
	 *
	 * SOFT re-anchor (mpv av_desync/ao-clocks model): a hard re-anchor
	 * on every audio packet teleports the clock whenever the AudioTrack
	 * latency estimate wobbles (measured ±100-400ms on TrueHD) - every
	 * queued frame's lateness jumps at once and the late-drop policy
	 * fires in bursts (the audio-coupled drop/desync; video-only plays
	 * perfectly). Instead: after the FIRST anchor (or a seek flush, when
	 * venc_flushing was set and >500ms off), nudge the clock toward the
	 * new estimate at ≤5ms per second of wall time - continuous
	 * correction like every known-good player; a hard reset only when
	 * the discrepancy is huge (seek / stream restart). */
	pthread_mutex_lock(&p->venc_mutex);
	int hard = p->venc_flushing ||
	           !p->venc_ref_time ||
	           (p->venc_put_time == 0);
	if (!hard) {
		int drift = time - dovi_sink_get_time(p);
		if (drift > 500 || drift < -500)
			hard = 1;
	}
	if (hard) {
		p->venc_put_time = time;
		p->venc_ref_time = atime();
	} else {
		/* AUDIO ANCHOR OWNS POSITION (single-writer model, mpv parity).
	 * The historical blend here had a 200ms-wide deadband (integer
	 * division by 200 clamped to ±2ms: at drift≈200 the audio side
	 * contributed +52ms/s while the latch-feedback ±1ms/sample blend
	 * contributed −48ms/s - measured standoff: the heard anchor sat
	 * 197-203ms ahead of the clock for the entire file, the audible
	 * 'audio ahead' offset that never closed). The latch feedback no
	 * longer writes this clock's position (it trims RATE only, in the
	 * venc thread), so this anchor is free to close position error at
	 * a real rate: proportional slew, 1/8th of the remaining drift
	 * per call, capped at 12ms so a 200ms offset closes in ~2s (52
	 * calls/s) without ever teleporting a live deadline, and never
	 * overshooting the anchor. Small drifts (AudioTrack latency wobble)
	 * close gently: a 40ms wobble moves ≤5ms/call. */
		int now = dovi_sink_get_time(p);
		int step = (time - now) / 8;
		if (step > 12)
			step = 12;
		if (step < -12)
			step = -12;
		if (step > 0 && step > (time - now))
			step = time - now;	/* never overshoot the anchor */
		if (step < 0 && step < (time - now))
			step = time - now;
		p->venc_put_time = now + step;
		p->venc_ref_time = atime();
	}
	/* re-anchor also lifts any flush hold so pacing resumes from the new
	 * anchor (seek: engine re-anchors after flushing) */
	p->venc_flushing = 0;
	pthread_cond_broadcast(&p->venc_cond);
	pthread_mutex_unlock(&p->venc_mutex);
	return 0;
}

static int sink_clear(STREAM_SINK_VIDEO *sink)
{
	return 0;
}

static int sink_resize(STREAM_SINK_VIDEO *sink, VIDEO_PROPERTIES *video)
{
	return 0;
}

static int sink_dump(STREAM_SINK_VIDEO *sink)
{
	priv_t *p = sink->priv;
	frame_q_dump2(&p->get_q);
	return 0;
}

STREAM_SINK_VIDEO *stream_sink_video_dovi_new(void *surface_handle)
{
	STREAM_SINK_VIDEO *sink = (STREAM_SINK_VIDEO *) acalloc(1, sizeof(STREAM_SINK_VIDEO));
	priv_t *p = (priv_t *) acalloc(1, sizeof(priv_t));

	if (!sink || !p)
		goto err;

	sink->name       = "dovi";
	sink->open       = sink_open;
	sink->close      = sink_close;
	sink->delete     = sink_delete;
	sink->put        = sink_put;
	sink->get        = sink_get;
	sink->flush      = sink_flush;
	sink->end        = sink_end;
	sink->syncable   = sink_syncable;
	sink->delay      = sink_delay;
	sink->get_frame  = sink_get_frame;
	sink->get_time   = sink_get_time;
	sink->put_time   = sink_put_time;
	sink->clear      = sink_clear;
	sink->resize     = sink_resize;
	p->stat_time0 = p->stat_time_last = -1;
	sink->dump       = sink_dump;

	sink->primary    = STREAM_SINK_DEFAULT_SCREEN;

	/* the sink owns the frame pool; frames carry AVFrames in priv, no pixel
	 * buffers. This selects the stream's use_sink_frames path, which drives
	 * sink->get reclamation and video_sink_count accounting. */
	sink->allocates_frames = 1;

	sink->priv = p;
	p->surface_handle = surface_handle;
	return sink;
err:
	serprintf("stream_sink_video_dovi_new failed: sink %p p %p\n", sink, p);
	if (sink)
		afree(sink);
	if (p)
		afree(p);
	return NULL;
}
