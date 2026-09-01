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

#define DOVI_SINK_MAX_FRAMES 14

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
} priv_t;

/* current presentation clock: TS anchored by the last put_time call,
 * advanced by wall clock (android2 _get_time parity). The stream engine
 * calls sink->put_time with the audio-heard anchor on every audio packet,
 * so this tracks what the user is currently hearing/seeing. */
static int dovi_sink_get_time( priv_t *p )
{
	int diff = atime() - p->venc_ref_time;
	p->venc_time = p->venc_put_time + diff;
	return p->venc_time;
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

static void *dovi_venc_thread( void *ctx )
{
	STREAM_SINK_VIDEO *sink = (STREAM_SINK_VIDEO *) ctx;
	priv_t *p = sink->priv;
	VIDEO_FRAME *frame;

	pthread_mutex_lock( &p->venc_mutex );
	while( p->venc_run ) {
		while( p->venc_run && !(frame = frame_q_get( &p->venc_q )) )
			pthread_cond_wait( &p->venc_cond, &p->venc_mutex );

		if( !frame )
			continue;

		p->venc_busy = 1;

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
			int now = dovi_sink_get_time( p );
			/* mpv-strict policy: NEVER pre-empt late frames. The measured
			 * steady state during the bright 4K FEL window is a constant
			 * ~4-5 frame-durations late (GPU-fence backpressure at
			 * max_swapchain_depth=3 + the one-frame pipelined hold), NOT
			 * a growing backlog: forced 719MHz GPU, pool 14 and bilinear
			 * scaler experiments all left DROPPED unchanged - these are
			 * fence cadence artifacts, and rendering them ~170ms late is
			 * one constant presentation delay, invisible to the eye.
			 * Only a run-away backlog (>= 8 frame durations) is worth
			 * dropping; the 5-drop cap bounds a misjudgment. */
			if( frame->blit_time - now < -8 * frame->duration && p->dropped < 5 ) {
				/* too late to be worth presenting: skip the render,
				 * reclaim via endloop's single put. The unrendered
				 * frame still owns its SW FEL BL/EL AVFrames: free
				 * them here or every dropped frame leaks ~15MB. */
				too_late = 1;
				p->dropped++;
				dovi_frame_free_payloads( frame );
				frame->blit_time = -1;
			} else {
				p->dropped = 0;
			}
		}

		if( !too_late ) {
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

			/* flip_page, one frame behind (mpv pipelining): present the
			 * PREVIOUS rendered frame at ITS deadline while the current
			 * frame's GPU work is already enqueued. The blocking fence
			 * wait inside swap now overlaps the next render instead of
			 * serializing behind it. Sleep until the pending frame's
			 * blit_time (cap: never wait past the next frame + 100ms);
			 * venc_cond is not signalled on time alone, so poll every
			 * 2ms (flush sets venc_flushing under the lock). */
			if( p->pending_frame && p->gl ) {
				pthread_mutex_unlock( &p->venc_mutex );
				for( ;; ) {
					int now = dovi_sink_get_time( p );
					int blit_duration = p->pending_frame->blit_time - now;
					if( blit_duration <= 2 )
						break;
					if( blit_duration > p->pending_frame->duration + 100 )
						blit_duration = p->pending_frame->duration + 100;
					struct timespec nap = { 0, 2 * 1000000L };
					nanosleep( &nap, NULL );
					pthread_mutex_lock( &p->venc_mutex );
					int flushing = p->venc_flushing;
					pthread_mutex_unlock( &p->venc_mutex );
					if( flushing )
						break;
				}
				dovi_gl_present(p->gl);
				pthread_mutex_lock( &p->venc_mutex );
				/* the presented frame returns to the pool for reuse */
				frame_q_put( &p->get_q, p->pending_frame );
				p->pending_frame = NULL;
			}
			p->pending_frame = frame;
			goto endloop_skip;	/* frame stays pending; recycled after present */
		}
endloop:
		frame_q_put( &p->get_q, frame );
endloop_skip:
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

	/* pre-queue all frames as free (mirrors android2's sink_open): the
	 * stream's _queue_sink_frames pulls them via sink->get into decode_q,
	 * so decoding can start before the first rendered frame comes back */
	for (i = 0; i < p->num_frames; i++)
		frame_q_put(&p->get_q, p->frames[i]);

	if (dovi_gl_open(&p->gl, p->surface_handle) != 0) {
		serprintf("dovi sink: dovi_gl_open failed\n");
		return 1;
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

	/* android2 venc_thread parity: hand the frame to the render thread,
	 * which paces it against the anchored presentation clock. The player
	 * engine thread (also the decoder driver) must never sleep here. */
	pthread_mutex_lock( &p->venc_mutex );
	frame_q_put( &p->venc_q, frame );
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
	pthread_cond_broadcast(&p->venc_cond);
	while (p->venc_busy && p->venc_run) {
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 50 * 1000000L;
		if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
		if (pthread_cond_timedwait(&p->venc_cond, &p->venc_mutex, &ts) == ETIMEDOUT)
			break;	// thread stuck: proceed rather than deadlock
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
	p->venc_put_time = time;
	p->venc_ref_time = atime();
	/* re-anchor also lifts any flush hold so pacing resumes from the new
	 * anchor (seek: engine re-anchors after flushing) */
	p->venc_flushing = 0;
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
