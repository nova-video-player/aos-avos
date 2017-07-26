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
#include "stream.h"
#include "stream_alloc.h"
#include "astdlib.h"
#include "athread.h"
#include "rc_clocks.h"
#include "h264.h"
#include "device_config.h"
#include "xdm_utils.h"
#include "pts_reorder.h"
#include "sfdec.h"

#include <time.h>
#ifdef CONFIG_STREAM

#define DBGS	if(0||Debug[DBG_STREAM])
#define DBGCV   if(0||Debug[DBG_CV])
#define DBGCV2  if(0||Debug[DBG_CV] > 1 )
#define DBGCV3  if(0||Debug[DBG_CV] > 2 )
#define DBGSI   if(0||Debug[DBG_SINK] )
#define DBGSI2  if(0||Debug[DBG_SINK] > 1 )

#define MAXW VIDEO_MAX_WIDTH
#define MAXH VIDEO_MAX_HEIGHT

#define SFDEC_MAX_FRAMES 2

#define NSEC_PER_SEC 1000000000L

#define CLOG(fmt, ...) serprintf("%s: " fmt "\n", __FUNCTION__, ##__VA_ARGS__)

static int sfdec_force_hw   = -1;
static int sfdec_force_blit = 0;
static int sfdec_no_drop    = 0;

DECLARE_DEBUG_PARAM ("sfhw", sfdec_force_hw );
DECLARE_DEBUG_TOGGLE("sffb", sfdec_force_blit );
DECLARE_DEBUG_TOGGLE("sfnd", sfdec_no_drop );

enum {
	THREAD_STATE_READING	= 0x01,
	THREAD_STATE_RENDERING	= 0x02,
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
		struct timespec	venc_ref_time;
		int width;
		int height;
		int interlaced;
		int state;
		int seek_time;
	} locked;
	
	STREAM_DEC_VIDEO *dec;
} priv_t;

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

static inline int get_venc_time_and_ts(priv_t *p, struct timespec *ts)
{
	clock_gettime(CLOCK_REALTIME, ts);
	return (ts->tv_sec  - p->locked.venc_ref_time.tv_sec ) * 1000 +
	       (ts->tv_nsec - p->locked.venc_ref_time.tv_nsec) / 1000000;
}

static inline int get_venc_time(priv_t *p)
{
	struct timespec ts;
	return get_venc_time_and_ts(p, &ts);
}

static inline void set_state_l(priv_t *p, int state, int mask)
{
	int prev_state = p->locked.state;
	p->locked.state = (p->locked.state&~mask) | (state&mask);

	if (prev_state != p->locked.state) {
		DBGCV3 CLOG("%s|%s",
		    p->locked.state & THREAD_STATE_READING ? "reading" : "!reading",
		    p->locked.state & THREAD_STATE_RENDERING ? "rendering" : "!rendering");
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

static inline int has_state_l(priv_t *p, int state, int mask)
{
	return (p->locked.state & mask) == state;
}
/*
static inline void seek_state_set_l(priv_t *p, int state)
{
	if (state == SEEK_STATE_OUTPUT_SENT) {
		if (p->locked.seek.state == SEEK_STATE_OUTPUT_SENDING)
			p->locked.seek.state = SEEK_STATE_NONE;
	} else {
		p->locked.seek.state = state;
	}
DBGCV CLOG("new state: %d", p->locked.seek.state);
}*/

static int videosink_open(STREAM_SINK_VIDEO *sink, VIDEO_PROPERTIES *video, void *ctx, int num_frames, STREAM_RC *rc)
{
	sink->ctx = ctx;
	priv_t *p = (priv_t *) sink->priv;

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
	int venc_time;

	if (!sink->is_open)
		return 0;

	pthread_mutex_lock(&p->locked.mtx);
	venc_time = get_venc_time(p);
	frame_q_put(&p->locked.venc_q, frame);
	pthread_cond_broadcast(&p->locked.cond);
	pthread_mutex_unlock(&p->locked.mtx);

DBGSI2 CLOG("frame %2d/%8d handle %p venc/blit %8d/%8d", frame->index, frame->time, frame->android_handle, venc_time, frame->blit_time);
	return venc_time;
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
	priv_t *p = (priv_t *) sink->priv;

DBGSI2 CLOG("->");
	pthread_mutex_lock(&p->locked.mtx);
	clock_gettime(CLOCK_REALTIME, &p->locked.venc_ref_time);
	pthread_mutex_unlock(&p->locked.mtx);
DBGSI2 CLOG("<-");
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

static int videosink_delay(STREAM_SINK_VIDEO *sink)
{
	return 0;
}

static VIDEO_FRAME *videosink_get_frame(STREAM_SINK_VIDEO *sink, int index)
{
	if (!sink || !sink->is_open)
		return NULL;
		
	priv_t *p = (priv_t *) sink->priv;
	return index < p->num_frames ? p->frames[index] : NULL;
}

static int videosink_get_time(STREAM_SINK_VIDEO *sink)
{
	priv_t *p = (priv_t *) sink->priv;
	int venc_time;


	pthread_mutex_lock(&p->locked.mtx);
	venc_time = get_venc_time(p);
	pthread_mutex_unlock(&p->locked.mtx);

	return venc_time;
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

	sink->name      = "sfdec";
	sink->open	= videosink_open;
	sink->close	= videosink_close;
	sink->delete	= videosink_delete;
	sink->put	= videosink_put;
	sink->get	= videosink_get;
	sink->flush	= videosink_flush_and_enqueue;
	sink->end	= videosink_end;
	sink->syncable	= videosink_syncable;
	sink->delay	= videosink_delay;
	sink->get_frame = videosink_get_frame;
	sink->get_time	= videosink_get_time;
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

		VIDEO_FRAME *f = NULL;
		while (p->locked.run && !(f = frame_q_get(&p->locked.venc_q))) {
			rm_state_l(p, THREAD_STATE_RENDERING);
			pthread_cond_wait(&p->locked.cond, &p->locked.mtx);
		}
		add_state_l(p, THREAD_STATE_RENDERING);

		if (!p->locked.run) {
DBGCV CLOG("stop thread");
			pthread_mutex_unlock(&p->locked.mtx);
		} else if (f) {
			struct timespec ts;
			int venc_time = get_venc_time_and_ts(p, &ts);
			int blit_duration = sfdec_force_blit ? 0 : f->blit_time - venc_time;
			int do_render = 1;
			if( s && s->paused ) {
DBGCV2 serprintf(" paused\n");
			} else {
				if (blit_duration > 10) {
					int rc = 0;

					// do not hang on one pic forever
					if( blit_duration > f->duration + 100 )
						blit_duration = f->duration + 100;

DBGCV2 CLOG("sleep @ %10ld.%3ld  dur %3d  f(%2d)  blit %8d  venc %8d)",
	ts.tv_sec, ts.tv_nsec/1000000, blit_duration, f->index, f->blit_time, venc_time);

					timespec_add_ms(&ts, blit_duration);
					while (p->locked.run && rc == 0)
						rc = pthread_cond_timedwait(&p->locked.cond, &p->locked.mtx, &ts);

					if (!p->locked.run) {
DBGCV CLOG("stop thread 2");
					}

				}
				// drop frames if thread is stopped or we are too late (more than 40ms) but
				// don't drop 2 consecutives frames
				do_render = p->locked.run && blit_duration >= -40;

				if (!do_render) {
CLOG("dropping frame(%d): %d ms late, blit_time: %d, venc_time: %d, f->time: %d", f->index, (venc_time - f->blit_time), f->blit_time, venc_time, f->time);
				}
			
			}
			pthread_mutex_unlock(&p->locked.mtx);

			if (do_render) {
DBGCV3 CLOG("render ->");
				int start = time_update_time();
				sfdec_buf_render(p->sfdec, (sfbuf_t *)f->android_handle, 1);
				int took = time_update_time() - start;
DBGCV CLOG("\t\t\t\t\t\t\trender %8d/%8d  took %3d", f->time, f->blit_time, took );
DBGCV3 CLOG("render <-");
			}
		}

		pthread_mutex_lock(&p->locked.mtx);
		if (f)
			frame_q_put(&p->locked.get_q, f);
	}
	rm_state_l(p, THREAD_STATE_RENDERING);
	pthread_mutex_unlock(&p->locked.mtx);
	CLOG("terminated");
	return NULL;
}

static void *videodec_thread(void *ctx)
{
	priv_t *p = (priv_t*) ctx;
	sfdec_read_out_t read_out;

	pthread_mutex_lock(&p->locked.mtx);

	while (p->locked.run && !p->locked.error) {
		int time;
		int seek_time = -1;

		sfbuf_t *sfbuf = NULL;
		VIDEO_FRAME *f = NULL;
		// decode frame
		while (p->locked.run && !p->locked.error &&!(f = frame_q_get(&p->locked.dec_q))) {
			rm_state_l(p, THREAD_STATE_READING);
			pthread_cond_wait(&p->locked.cond, &p->locked.mtx);
		}
		add_state_l(p, THREAD_STATE_READING);

		if (!f) {
DBGCV CLOG("stop thread");
			continue;
		}

		if (p->locked.seek_time != -1) {
			// wait until the thread owns all the frames.
			// p->num_frames - 1 since we already own one (f)
			while (p->locked.run &&
			    frame_q_count(&p->locked.dec_q) != (p->num_frames - 1))
				pthread_cond_wait(&p->locked.cond, &p->locked.mtx);
			seek_time = p->locked.seek_time;
		}

		pthread_mutex_unlock(&p->locked.mtx);

		if (seek_time != -1) {
			int i = 0;

			for (i = 0; i < p->num_frames; ++i)
				frame_release(p->sfdec, p->frames[i]);
		} else {
			frame_release(p->sfdec, f);
		}

DBGCV3 CLOG("sfdec_read, seek: %d->", seek_time);
		static int last;
		int start = time_update_time();
		int wait  = start - last;
		     last = start;

		int ret = sfdec_read(p->sfdec, seek_time, &read_out);

		int took = time_update_time() - start;

		pthread_mutex_lock(&p->locked.mtx);

		if (p->locked.seek_time != -1 && seek_time == p->locked.seek_time)
			p->locked.seek_time = -1;

		if (ret == -1) {
			p->locked.error = 1;
DBGCV3 CLOG("sfdec_read <- invalid");
		}
		if (read_out.flag & SFDEC_READ_BUF) {
			sfbuf = read_out.buf.sfbuf;
			time = read_out.buf.time_us / 1000;
DBGCV3 CLOG("sfdec_read <- sfbuf: %p, time_us: %lld", sfbuf, read_out.buf.time_us);
		}
		if (read_out.flag & SFDEC_READ_SIZE) {
			if (read_out.size.width >= 0 && read_out.size.height >= 0) {
				p->locked.width = read_out.size.width;
				p->locked.height = read_out.size.height;
				p->locked.interlaced = read_out.size.interlaced;
			}
DBGCV3 CLOG("sfdec_read <- size %dx%d (%d)", read_out.size.width, read_out.size.height, read_out.size.interlaced);
		}

		if (!sfbuf) {
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

		f->time = out_time;
		f->user_ID = out_ID;
		f->type = out_type;

		f->width = p->locked.width;
		f->height = p->locked.height;
		f->interlaced = p->locked.interlaced;

		f->android_handle = sfbuf;
		if (p->locked.seek_time != -1 || seek_time != -1 || f->time < 0) {
			/*
			 * buffer may be invalid when seeking, so drop it.
			 */
			frame_q_put_head(&p->locked.dec_q, f);
		} else {
DBGCV CLOG("\t\t\tout %8d  tim %3d  wait %3d", f->time, took, wait );
			frame_q_put(&p->locked.out_q, f);
		}
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
	void *surface_handle = NULL;
	void *extradata = NULL;
	size_t extradata_size = 0;
	int pts_reorder = 0;

	int hw_type = device_get_hw_type();
	if (video->format == VIDEO_FORMAT_H264 && video->sps.valid && video->profile >= H264_PROFILE_HIGH10) {
		CLOG("sf can't do Hi10P, abort");
		return 1;
	}

	dec->ctx = ctx;
	p->dec = dec;

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
	switch (video->format) {
		case VIDEO_FORMAT_H264:
			sfdec_codec = SFDEC_VIDEO_AVC;
			break;
		case VIDEO_FORMAT_HEVC:
			sfdec_codec = SFDEC_VIDEO_HEVC;
			break;
		case VIDEO_FORMAT_MPG4: {
			if (hw_type == HW_TYPE_QCOM_S1 || hw_type == HW_TYPE_MTK) {
				CLOG("that codec doesn't work on that device: abort");
				return 1;
			}
			sfdec_codec = SFDEC_VIDEO_MPEG4;
			break;
		}
		case VIDEO_FORMAT_H263:
			sfdec_codec = SFDEC_VIDEO_H263;
			break;
#if 0
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
#endif
		default:
			return 1;
	}

	// force AVCDecoder usage, better perf than rk OMX decoder
	if ((hw_type == HW_TYPE_RK30 ||
	     hw_type == HW_TYPE_RK29) && video->rotation == 0)
		flags |= SFDEC_FLAG_SWDEC;
	if (ctx)
		surface_handle = stream_get_surface_handle((STREAM *)ctx);

	if (sfdec_force_hw == 0)
		flags |= SFDEC_FLAG_SWDEC;

	width = video->width;
	height = video->height;
	p->sfdec = sfdec_new(SFDEC_TYPE_OMXCODEC,
			sfdec_codec,
			flags,
			&width, &height, video->rotation,
			video->duration * 1000, 768 * 1024 /* input_size */,
			surface_handle,
			extradata, extradata_size,
			&pts_reorder);

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

	p->num_frames = SFDEC_MAX_FRAMES;
	if (stream_alloc_frames( &p->frames, video->width, video->height, video->colorspace, STREAM_MEM_ANDROID, &p->num_frames) != 0) {
		CLOG("stream_alloc_frames failed");
		goto err;
	}
	frame_q_init(&p->locked.dec_q, "dec_q");
	frame_q_init(&p->locked.out_q, "out_q");
	frame_q_init(&p->locked.venc_q, "venc_q");
	frame_q_init(&p->locked.get_q, "get_q");

	pthread_mutex_init(&p->locked.mtx, NULL);
	pthread_cond_init(&p->locked.cond, NULL);
	p->locked.width = width;
	p->locked.height = height;
	p->locked.seek_time = -1;
	p->locked.run = 1;

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

	ret = sfdec_send_input(p->sfdec,
	    d->data[0], d->size,
	    (int64_t)d->time * 1000,
	    d->type == I_VOP ? 1 : 0,
	    0);
	if( ret > 0 ) {
DBGCV CLOG("%c %8d", frame_type(d->type), d->time);
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

static int videodec_seek(STREAM_DEC_VIDEO *dec, int time)
{
	priv_t *p = (priv_t*)dec->priv;

	CLOG("%d", time);

	pthread_mutex_lock(&p->locked.mtx);
	XDM_id_flush( &p->XDM_ctx );
	XDM_ts_flush( &p->XDM_ctx );
	p->locked.seek_time = time;
	pthread_mutex_unlock(&p->locked.mtx);

	sfdec_flush(p->sfdec);

	return 0;
}

static int videodec_flush(STREAM_DEC_VIDEO *dec)
{
	CLOG();

	return 0;
}

static int videodec_get_rc(STREAM_DEC_VIDEO *dec, STREAM_RC *rc)
{
	if( !rc )
		return 1;
	priv_t *p = (priv_t*)dec->priv;

	memset(rc, 0, sizeof(STREAM_RC));
	
	rc->num_frames = p->num_frames;

	rc->cpu_type   = SFDEC_OMXCODEC;
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


static STREAM_DEC_VIDEO *new_dec(void)
{ 
	static char name[] = "sfdec";
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
	dec->seek       = videodec_seek;
	dec->flush	= videodec_flush;
	dec->get_rc	= videodec_get_rc;
	dec->get_sink	= videodec_get_sink;
	dec->async	= 1;

	if (!(dec->priv = acalloc(1, sizeof(priv_t)))) {
		CLOG("cannot alloc priv");
		afree(dec);
		return NULL;
	}

	return dec;
}

#define OMXC_REGISTER( format, mangler ) \
STREAM_REGISTER_DEC_VIDEO( format, 0, MAXW, MAXH, SFDEC_OMXCODEC, new_dec, "sfdec", mangler );

#ifdef CONFIG_OMX_MPEG2
OMXC_REGISTER( VIDEO_FORMAT_MPEG, &stream_video_mangler_MPEG2 );
#endif
#ifdef CONFIG_OMX_MPEG4
OMXC_REGISTER( VIDEO_FORMAT_MPG4, NULL );
#endif
#ifdef CONFIG_OMX_H264
OMXC_REGISTER( VIDEO_FORMAT_H264, &stream_video_mangler_H264 );
#endif
#ifdef CONFIG_OMX_HEVC
OMXC_REGISTER( VIDEO_FORMAT_HEVC, NULL );
#endif
#ifdef CONFIG_OMX_WMV
OMXC_REGISTER( VIDEO_FORMAT_WMV3, NULL );
OMXC_REGISTER( VIDEO_FORMAT_VC1, NULL );
#endif
#ifdef CONFIG_OMX_RV3040
OMXC_REGISTER( VIDEO_FORMAT_RV40, NULL );
OMXC_REGISTER( VIDEO_FORMAT_RV30, NULL );
#endif
#ifdef CONFIG_OMX_VP6
OMXC_REGISTER( VIDEO_FORMAT_VP6, NULL );
#endif
#ifdef CONFIG_OMX_VP7
OMXC_REGISTER( VIDEO_FORMAT_VP7, NULL );
#endif

#ifdef DEBUG_MSG
static STREAM_REG_DEC_VIDEO reg_mpeg2 = { VIDEO_FORMAT_MPEG, 0, MAXW, MAXH, 0, DSP2, new_dec, "sfdec", &stream_video_mangler_MPEG2 };
static STREAM_REG_DEC_VIDEO reg_mpeg4 = { VIDEO_FORMAT_MPG4, 0, MAXW, MAXH, 0, DSP2, new_dec, "sfdec", NULL };
static STREAM_REG_DEC_VIDEO reg_h264  = { VIDEO_FORMAT_H264, 0, MAXW, MAXH, 0, DSP2, new_dec, "sfdec", &stream_video_mangler_H264 };
static STREAM_REG_DEC_VIDEO reg_hevc  = { VIDEO_FORMAT_HEVC, 0, MAXW, MAXH, 0, DSP2, new_dec, "sfdec", NULL };
static STREAM_REG_DEC_VIDEO reg_wmv3  = { VIDEO_FORMAT_WMV3, 0, MAXW, MAXH, 0, DSP2, new_dec, "sfdec", NULL };
static STREAM_REG_DEC_VIDEO reg_vc1   = { VIDEO_FORMAT_VC1,  0, MAXW, MAXH, 0, DSP2, new_dec, "sfdec", NULL };
static STREAM_REG_DEC_VIDEO reg_rv4   = { VIDEO_FORMAT_RV40, 0, MAXW, MAXH, 0, DSP2, new_dec, "sfdec", NULL };
static STREAM_REG_DEC_VIDEO reg_rv3   = { VIDEO_FORMAT_RV30, 0, MAXW, MAXH, 0, DSP2, new_dec, "sfdec", NULL };

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
}

DECLARE_DEBUG_COMMAND_VOID( "regsfc", _reg_sfc );
#endif

#endif
