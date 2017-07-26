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
#include "debug.h"
#include "stream_sink_video.h"
#include "stream.h"
#include "types.h"
#include "stream_sink_video.h"
#include "stream_resizer.h"
#include "stream_alloc.h"

#include "device_config.h"
#include "android_config.h"
#include "android_buffer.h"

#include "frame_q.h"

#include "codec_utils.h"

#include <pthread.h>


#define NSEC_PER_SEC 1000000000L

#define DBGS	if(Debug[DBG_STREAM])
#define DBGSI   if(Debug[DBG_SINK] )
#define DBGSI2  if(Debug[DBG_SINK] > 1)

#define LOG(fmt, ...) serprintf("%s: " fmt "\n", __FUNCTION__, ##__VA_ARGS__)
#define DBGLOG  DBGS LOG

static int num_sink_frames = 1;
static int do_fake  = 0;
static int force_blit  = 0;

typedef struct priv {
	void		*surface_handle;
	android_surface_t *as;

	int		num_frames;
	VIDEO_FRAME	*frames[STREAM_MAX_FRAMES];
	VIDEO_FRAME	*frame_out;
	FRAME_Q		get_q;
	pthread_mutex_t	venc_mutex;
	pthread_cond_t	venc_cond;
	
	int		num_blit_frames;
	VIDEO_FRAME	*blit_frames[1];
	int		blit_frames_state[1];

	int		width;
	int		height;
	int 		padded_width;
	int 		padded_height;
	int		ofs_x;
	int		ofs_y;
	int		interlaced;
	int		aspect_n;
	int		aspect_d;
	float		crop_w;
	float		crop_h;

	FRAME_Q		venc_q;
	pthread_t	venc_thread_handle;
	int		venc_run;
	int		venc_flushing;
	
	int 		venc_put_time;
	int 		venc_ref_time;
	int 		venc_time;
	
	int		out_time;
	int		out_index;
	
	int 		dropped;
	int 		total_dropped;

} priv_t;
enum {
	FRAME_STATE_QUEUED = 0,
	FRAME_STATE_DEQUEUED,
	FRAME_STATE_OUT,
};

static inline int align(int x, int y)
{
    // y must be a power of 2.
    return (x + y - 1) & ~(y - 1);
}

static int _get_time( priv_t *p )
{
	int diff = atime() - p->venc_ref_time;
	p->venc_time = p->venc_put_time + diff;
//serprintf("get %8d + %8d = %8d\n", p->venc_put_time, diff, p->venc_time );	
	
	return p->venc_time;
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

static VIDEO_FRAME *blit_frame_get(priv_t *p, void *handle)
{
	int i;
	for (i = 0; i < p->num_blit_frames; i++) {
		VIDEO_FRAME *frame = p->blit_frames[i];
		if (frame->handle[0] == handle)
			return frame;
	}
	return NULL;
}

static int update_frame_pointers(VIDEO_FRAME * frame, int colorspace, android_buffer_t android_buffer)
{
	frame->handle[0]  = android_buffer.handle;
#ifdef CONFIG_OMX_IOMX
	frame->android_handle = android_buffer.handle;
#else
	frame->android_handle = android_buffer.img_handle;
#endif
	if (android_buffer.type == BUFFER_TYPE_HW) {
//DBGLOG("android_buffer(hw) %d x %d / %d  siz %d", android_buffer.width, android_buffer.height, android_buffer.stride, android_buffer.size);
		frame->data[0]      = NULL;
		frame->linestep[0]  = android_buffer.stride;
		frame->data_size[0] = android_buffer.size;
		frame->size         = android_buffer.size;
	} else {
//DBGLOG("android_buffer(sw) %p %d x %d / %d", android_buffer.data, android_buffer.width, android_buffer.height, android_buffer.stride);

		switch (colorspace) {
			case AV_IMAGE_BGRA_32:
			case AV_IMAGE_RGBX_32:
				frame->data[0]      = android_buffer.data;
				frame->linestep[0]  = android_buffer.stride;
				frame->data_size[0] = android_buffer.stride * android_buffer.height;
				break;
			case AV_IMAGE_NV12:
				frame->data[0]      = android_buffer.data;
				frame->linestep[0]  = android_buffer.stride;
				frame->data_size[0] = android_buffer.stride * android_buffer.height;

				frame->linestep[1]  = android_buffer.stride;
				frame->data_size[1] = frame->linestep[1] * android_buffer.height / 2;
				frame->data[1]      = frame->data[0] + frame->data_size[0];
				break;
			case AV_IMAGE_YV12:
			default:
				frame->data[0]      = android_buffer.data;
				frame->linestep[0]  = android_buffer.stride;
				frame->data_size[0] = android_buffer.stride * android_buffer.height;
				frame->linestep[1]  = frame->linestep[2]  = align(android_buffer.stride / 2, 16);
				frame->data_size[1] = frame->data_size[2] = frame->linestep[1] * android_buffer.height / 2;
				frame->data[1]      = frame->data[0] + frame->data_size[0];
				frame->data[2]      = frame->data[1] + frame->data_size[1];
				break;
		}
		frame->size = frame->data_size[0] + frame->data_size[1] + frame->data_size[2];
	}
	return 0;
}

static VIDEO_FRAME *dequeue_blit_frame( priv_t *p ) 
{
	VIDEO_FRAME *frame;
	void *handle = NULL;
	android_buffer_t android_buffer = { 0 };

	if (android_buffer_dequeue_with_buffer(p->as, &handle, &android_buffer) != 0)
		return NULL;
	frame = blit_frame_get(p, handle);
	if (!frame) {
		LOG("WARNING: unknown handle %08X after dequeue, shouldn't happen", handle);
		return NULL;
	}
	if (android_buffer.data != NULL) {
		int colorspace =  android_get_av_color(android_buffer.type);
		update_frame_pointers(frame, colorspace, android_buffer);
	}

	p->out_time  = frame->blit_time;
	p->out_index = frame->index;
	p->blit_frames_state[frame->index] = FRAME_STATE_DEQUEUED;

	return frame;
}

static void render_or_drop( VIDEO_FRAME *src, VIDEO_FRAME *dst )
{		
	if( src->dec ) {
		STREAM_DEC_VIDEO *dec = (STREAM_DEC_VIDEO*)src->dec;
	 	if( dec->render ) {
			dec->render( dec, dst, src );
		}
	}
}

static void *venc_thread(void *ctx)
{
	STREAM_SINK_VIDEO *sink = (STREAM_SINK_VIDEO *)ctx;
	priv_t *p = sink->priv;
	STREAM *s = (STREAM *)sink->ctx;
	VIDEO_FRAME *frame = NULL;

	pthread_mutex_lock(&p->venc_mutex);
	while (p->venc_run) {
		while (p->venc_run && !(frame = frame_q_get(&p->venc_q))) {
			pthread_cond_wait(&p->venc_cond, &p->venc_mutex);
		}

		if (!frame)
			continue;
		p->frame_out = frame;
//		p->frames_state[frame->index] = FRAME_STATE_OUT;

		int venc_time = _get_time(p);
		
		p->venc_flushing = 0;

		int delay = 4 * 40;	// assume 4 frames at 25fps

		int blit_duration = force_blit ? 0 : frame->blit_time - venc_time - delay;

DBGSI serprintf("[%2d]%3d[%2d|%4d](%3d)", frame_q_count( &p->venc_q ), blit_duration, frame->index, frame->decode_time, frame->blit_time - p->out_time );
		if( s && s->paused ) {
DBGSI serprintf(" paused\n");
		} else if (blit_duration > 0) {
			if( blit_duration > 50 ) {
				// do not hang on one pic forever
				if( blit_duration > frame->duration + 100 ) {
DBGSI serprintf(" wait");
					blit_duration = frame->duration + 100;
				} 
			} 
//DBGLOG("wait %3d  f %08d  v %08d", blit_duration, frame->blit_time, venc_time);
			struct timespec ts;
			clock_gettime(CLOCK_REALTIME, &ts);
			timespec_add_ms(&ts, blit_duration - 10);
			
			int rc = 0;
			while (p->venc_run && rc == 0 && !p->venc_flushing) {
				rc = pthread_cond_timedwait(&p->venc_cond, &p->venc_mutex, &ts);
			}
		} else if (blit_duration < -10 && p->dropped < 5) {
//			LOG("drop[%2d]  blit %8d  venc %8d  diff %d", frame->index, frame->blit_time, venc_time, blit_duration);
serprintf(" DROP(%3d)", blit_duration);
			p->dropped ++;
			p->total_dropped ++;
			frame->blit_time = -1;

			render_or_drop( frame, NULL );
			frame_q_put(&p->get_q, frame);

			goto endloop;
		} else {
DBGSI serprintf(" ok");
		}
		
		p->dropped = 0;
		
		if( do_fake == 1 ) {
			render_or_drop( frame, NULL );
			frame_q_put(&p->get_q, frame);
		} else {
serprintf("<BLIT %8d >", frame->time);
			// get frame from Android
			VIDEO_FRAME *blit_frame = dequeue_blit_frame( p );

			// render
 			render_or_drop( frame, blit_frame );

			// and queue the frame back to Android
			if (android_buffer_queue(p->as, blit_frame->handle[0]) != 0) {
				LOG("WARNING: frame(%d): android_buffer_queue failed", blit_frame->index);
				goto endloop;
			}
			p->blit_frames_state[frame->index] = FRAME_STATE_QUEUED;

			frame_q_put(&p->get_q, frame);
		}
endloop:
DBGSI serprintf("\n");
		p->frame_out = NULL;
		pthread_cond_broadcast(&p->venc_cond);
	}
	pthread_mutex_unlock(&p->venc_mutex);
	return NULL;
}

__attribute__((unused))
static int vr_round(float val)
{
	int ret = (int) val;
	float tmp = (val - (float) ret);
	if (tmp >= 0.5f)
		ret += 1;
	return ret;
}
/*
static int setcrop(priv_t *p, int ofs_x, int ofs_y, int width, int height)
{
	int x;
	int y;
	int w;
	int non_aligned_w;
	int h;
	float alignement_ratio;

	if (p->ofs_x != ofs_x || p->ofs_y != ofs_y ||
	    p->width != width || p->height != height) {
		p->ofs_x = ofs_x;
		p->ofs_y = ofs_y;
		p->width = width;
		p->height = height;
	} else {
		return 0;
	}

	non_aligned_w = p->width;
	w = non_aligned_w &~15; // lower align of 16
	alignement_ratio = (float)w/(float)non_aligned_w;
	h = p->height * alignement_ratio;
	x = (int)p->ofs_x + ((int)p->width - w) / 2;
	y = (int)p->ofs_y + ((int)p->height - h) / 2;
	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
//LOG("Set Crop to %dx%d - %dx%d", x, y, w, h);
//	return android_buffer_setcrop(p->as, x, y, w, h);
	return 0;
}
*/
static VIDEO_FRAME *sink_get_frame( STREAM_SINK_VIDEO *sink, int index )
{
	if (!sink || !sink->is_open)
		return NULL;
		
	priv_t *p = sink->priv;

	return index < p->num_frames ? p->frames[index] : NULL;
}

static int allocate_buffers(STREAM_SINK_VIDEO *sink, VIDEO_FRAME *(*frames)[], int *num_frames, int width, int height, int colorspace, android_surface_t *as)
{
	int i;

	if (stream_alloc_frames( frames, width, height, colorspace, as ? STREAM_MEM_ANDROID : STREAM_MEM_BYO, num_frames) != 0) {
		LOG("stream_alloc_frames failed");
		return 1;
	}

	DBGLOG("num_frames: %d", *num_frames);

	if( as ) {
		for (i = 0; i < *num_frames; i++) {
			VIDEO_FRAME *frame = (*frames)[i];

			android_buffer_t android_buffer = { 0 };
			if (android_buffer_alloc(as, &android_buffer) != 0) {
				LOG("android_buffer_alloc failed, even for %d frames!", *num_frames);
				return 1;
			}
			android_buffer.type = BUFFER_TYPE_HW;

			frame->index = i;
			update_frame_pointers(frame, colorspace, android_buffer);
			serprintf("as %08X  linestep %d/%d/%d  data_size %d/%d/%d\n", as, frame->linestep[0], frame->linestep[1], frame->linestep[2 ], frame->data_size[0], frame->data_size[1], frame->data_size[2]);
		}
	}
	return 0;
}

static int sink_open(STREAM_SINK_VIDEO *sink, VIDEO_PROPERTIES *video, void *ctx, int num_frames, STREAM_RC *rc)
{
	int hal_format = 0;
	int buffer_type;
	int colorspace = video->colorspace;

	sink->ctx = ctx;
	priv_t *p = sink->priv;
DBGS serprintf("stream_sink_video_android: open  num_frames %d  cpu_type %d\n", num_frames, rc->cpu_type);

	p->as = android_surface_create(p->surface_handle, device_has_archos_enhancement());
	if (!p->as) {
		LOG("android_surface_create failed");
		goto err;
	}

	buffer_type = BUFFER_TYPE_SW;
	colorspace  = android_get_av_color(buffer_type);
	hal_format  = android_get_hal_format(colorspace, buffer_type);
	if (hal_format == 0) {
		colorspace  = android_get_av_color_unknown(buffer_type);
		hal_format  = android_get_hal_format_unknown(colorspace, buffer_type);
	}
	LOG("using SW, colorspace: %d hal_format: 0x%X", colorspace, hal_format);
	if (!color_conversion_supported(colorspace, avimage2pixfmt(video->colorspace)) && (colorspace != video->colorspace)) {
		LOG("copy HW to SW: color conversion not supported: %d to %d", colorspace, video->colorspace);
		goto err;
	}

	if (hal_format == 0) {
		LOG("hal_format not found");
		goto err;
	}

	if (!video->padded_width) {
		p->padded_width  = (video->width + 0x0f) & ~0x0f;
		p->padded_height = pad_height(video->height);
	} else {
		p->padded_width  = video->padded_width;
		p->padded_height = video->padded_height;
	}

	LOG("hal_format: 0x%X, buffer_type: %d hw_usage: 0x%X, size: %dx%d", hal_format, buffer_type, rc->hw_usage, p->padded_width, p->padded_height);

	p->num_blit_frames = 1;
	p->num_frames = num_frames;
	
	int min_undequeued = 0;
	if (android_buffer_setup(p->as, p->padded_width, p->padded_height, buffer_type, hal_format, rc->hw_usage, &p->num_blit_frames, &min_undequeued) != 0) {
		LOG("android_buffer_setup failed");
		android_buffer_close(p->as);
		p->num_blit_frames = 0;
		goto err;
	}

	if (p->num_frames > STREAM_MAX_FRAMES)
		p->num_frames = STREAM_MAX_FRAMES;

	// allocate the Android buffer(s) that we render into
	if( allocate_buffers( sink, &p->blit_frames, &p->num_blit_frames, video->width, video->height, colorspace, p->as ) ) {
		goto err;
	}

	// allocate buffers for codec (dummy alloc, codec will have it's own)
	if( allocate_buffers( sink, &p->frames, &p->num_frames, video->width, video->height, colorspace, NULL ) ) {
		goto err;
	}

	sink->is_open = 1;

	frame_q_init(&p->get_q,  "get_q");
	frame_q_init(&p->venc_q, "venc_q");

	pthread_mutex_init(&p->venc_mutex, NULL);
	pthread_cond_init(&p->venc_cond, NULL);

	LOG("num_frames %d", p->num_frames);

	int i;
	for (i = 0; i < p->num_frames; ++i) {
		frame_q_put(&p->get_q, p->frames[i]);
		DBGLOG("put frame(%d)", i);
	}

	// queue all blit frames to Android...
	for (i = 0; i < p->num_blit_frames; ++i) {
		if (android_buffer_queue(p->as, p->blit_frames[0]->handle[0]) != 0) {
LOG("WARNING: frame(%d): android_buffer_queue failed", p->blit_frames[0]->index);
		}
		p->blit_frames_state[i] = FRAME_STATE_QUEUED;
		DBGLOG("cancel blit_frame(%d)", i);
	}

	p->venc_run = 1;
	pthread_create(&p->venc_thread_handle, 0, venc_thread, (void*)sink);

	return sink->flush(sink);
err:
LOG("sink_open failed");
	sink->close(sink);
	return -1;
}

static int sink_close(STREAM_SINK_VIDEO *sink)
{
	int i;
	priv_t *p = sink->priv;

DBGS serprintf("stream_sink_video_android: close\n");

	if (p->venc_run) {
		pthread_mutex_lock(&p->venc_mutex);
		p->venc_run = 0;
		pthread_cond_signal(&p->venc_cond);
		pthread_mutex_unlock(&p->venc_mutex);
		pthread_join(p->venc_thread_handle, NULL);
	}

	if (p->num_blit_frames) {
		for (i = 0; i < p->num_blit_frames; ++i) {
			if (p->blit_frames_state[i]) {
				DBGLOG("cancel blit_frame(%d)", i);
				android_buffer_cancel(p->as, p->blit_frames[i]->handle[0]);
				p->blit_frames_state[i] = FRAME_STATE_QUEUED;
			}
		}
		stream_free_frames(&(p->blit_frames), p->num_blit_frames);
		android_buffer_close(p->as);
	}

	stream_free_frames(&(p->frames), p->num_frames);

	if (sink->is_open) {
		pthread_mutex_destroy(&p->venc_mutex);
		pthread_cond_destroy (&p->venc_cond);

		frame_q_flush(&p->get_q);
		frame_q_flush(&p->venc_q);
	}

	if (p->as) {
		android_surface_destroy(&p->as);
	}
	
	p->num_frames       = 0;
	p->num_blit_frames  = 0;
	p->frame_out        = NULL;
	p->interlaced       = 0;
	p->width            = 0;
	p->height           = 0;
	p->padded_width     = 0;
	p->padded_height    = 0;
	p->ofs_x            = 0;
	p->ofs_y            = 0;
	p->aspect_n         = 0;
	p->aspect_d         = 0;
	p->venc_flushing    = 0;
	p->venc_run         = 0;
	memset(&p->venc_ref_time, 0, sizeof(p->venc_ref_time));

	sink->is_open = 0;

serprintf("SINK DROPPED: %d\r\n", p->total_dropped );

	DBGLOG("end");
	return 0;
}

static int sink_delete(STREAM_SINK_VIDEO *sink)
{
	afree(sink->priv);
	afree(sink);
	return 0;
}

static int sink_put(STREAM_SINK_VIDEO *sink, VIDEO_FRAME *frame)
{
	priv_t *p = sink->priv;

	if (!sink->is_open)
		return 0;

//	setcrop(p, frame->ofs_x, frame->ofs_y, frame->width, frame->height);

	if( do_fake == 2 ) {
		frame_q_put(&p->get_q, frame);
	} else {
		pthread_mutex_lock(&p->venc_mutex);
		frame_q_put(&p->venc_q, frame);
		pthread_cond_signal(&p->venc_cond);
		pthread_mutex_unlock(&p->venc_mutex);
	}

	DBGSI2 LOG("frame %2d/%8d  handle %08X", frame->index, frame->time, frame->handle[0]);

	return _get_time(p);
}

static int sink_get(STREAM_SINK_VIDEO *sink, VIDEO_FRAME **pframe)
{
	priv_t *p = sink->priv;

	if (!sink->is_open) {
		*pframe = NULL;
		return 1;
	}
	
	VIDEO_FRAME *frame = frame_q_get(&p->get_q);

	*pframe = frame;
	if( frame ) {
		DBGSI2 LOG("frame %2d/%8d", frame->index, frame->time);
	}
	return frame ? 0 : 1;
}

static int sink_flush_and_enqueue(STREAM_SINK_VIDEO *sink)
{
	priv_t *p = sink->priv;

	if (!sink->is_open)
		return 0;

	LOG();

	pthread_mutex_lock(&p->venc_mutex);
	frame_q_flush(&p->venc_q);

	p->venc_flushing = 1;
	pthread_cond_signal(&p->venc_cond);

	while (p->frame_out != NULL)
		pthread_cond_wait(&p->venc_cond, &p->venc_mutex);

	int i;
	for (i = 0; i < p->num_blit_frames; ++i) {
		if (p->blit_frames_state[i]) {
			DBGLOG("cancel blit_frame(%d)", i);
			android_buffer_cancel(p->as, p->blit_frames[i]->handle[0]);
			p->blit_frames_state[i] = FRAME_STATE_QUEUED;
		}
	}

	frame_q_flush(&p->get_q);
	for (i = 0; i < p->num_frames; ++i) {
		frame_q_put(&p->get_q, p->frames[i]);
		p->frames[i]->time = -1;
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
	return 1;
}

static int sink_delay(STREAM_SINK_VIDEO *sink)
{
	return 0;
}

static int sink_put_time( STREAM_SINK_VIDEO *sink, int time )
{
	priv_t *p = (priv_t *) sink->priv;
	
	p->venc_put_time = time;
	p->venc_ref_time = atime();

DBGSI2 serprintf("[[put %8d]]", time );	
	return 0;
}

static int sink_get_time( STREAM_SINK_VIDEO *sink )
{
	priv_t *p = (priv_t *) sink->priv;
	
	return _get_time( p );
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

	frame_q_dump2( &p->venc_q );
	frame_q_dump2( &p->get_q ); 

	return 0;
}

STREAM_SINK_VIDEO *stream_sink_video_android3_new(void *surface_handle) 
{
	STREAM_SINK_VIDEO *sink = (STREAM_SINK_VIDEO *) acalloc(1, sizeof(STREAM_SINK_VIDEO));
	priv_t *p = (priv_t *) acalloc(1, sizeof(priv_t));

	if (!sink || !p)
		goto err;

	sink->name      = "android3";
	sink->open	= sink_open;
	sink->close	= sink_close;
	sink->delete	= sink_delete;
	sink->put	= sink_put;
	sink->get	= sink_get;
	sink->flush	= sink_flush_and_enqueue;
	sink->end	= sink_end;
	sink->syncable	= sink_syncable;
	sink->delay	= sink_delay;
	sink->get_frame = sink_get_frame;
	sink->get_time	= sink_get_time;
	sink->put_time	= sink_put_time;
	sink->clear	= sink_clear;
	sink->resize	= sink_resize;
	sink->dump	= sink_dump;

	sink->primary	= STREAM_SINK_DEFAULT_SCREEN;

	sink->allocates_frames = 1;

	sink->priv = p;
	p->surface_handle = surface_handle;
	return sink;
err:
	LOG("stream_sink_video_android_new failed: sink: %p, p: %p", sink, p);
	if (sink)
		afree(sink);
	if (p)
		afree(p);
	return NULL;
}

#ifdef DEBUG_MSG
DECLARE_DEBUG_PARAM  ("ssdf", do_fake  );
DECLARE_DEBUG_PARAM  ("snsf", num_sink_frames );
DECLARE_DEBUG_TOGGLE ("ssfb", force_blit );
#endif
