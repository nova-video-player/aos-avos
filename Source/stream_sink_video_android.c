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
static int do_copy  = 1;
static int do_fake  = 0;
static int num_undequeued = 0;
static int force_blit  = 0;

static int work_num = 0;

typedef struct priv {
	void		*surface_handle;
	android_surface_t *as;

	int		num_frames;
	VIDEO_FRAME	*frames[STREAM_MAX_FRAMES];
	VIDEO_FRAME	*frame_out;
	int		frames_state[STREAM_MAX_FRAMES];
	FRAME_Q		get_q;
	int		frames_num_owned;
	int		frames_max_owned;
	pthread_mutex_t	venc_mutex;
	pthread_cond_t	venc_cond;

	int		num_user_frames;
	VIDEO_FRAME	*user_frames[STREAM_MAX_FRAMES];
	FRAME_Q		user_q;
	pthread_mutex_t	user_mutex;

	FRAME_Q		copy_q;
	pthread_t	copy_thread_handle;
	int		copy_run;
	pthread_mutex_t	copy_mutex;
	pthread_cond_t	copy_cond;

	int 		work_num;
	void		*mt_ctx;
	
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
	struct timespec	venc_ref_time;
	int		venc_run;
	int		venc_flushing;
	
	int		copy_buffers;
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
	return (ts->tv_sec  - p->venc_ref_time.tv_sec ) * 1000 +
	       (ts->tv_nsec - p->venc_ref_time.tv_nsec) / 1000000;
}

static inline int get_venc_time(priv_t *p)
{
	struct timespec ts;
	return get_venc_time_and_ts(p, &ts);
}

static void *venc_thread(void *ctx)
{
	STREAM_SINK_VIDEO *sink = (STREAM_SINK_VIDEO *)ctx;
	priv_t *p = sink->priv;
	STREAM *s = (STREAM *)sink->ctx;
	int blit_duration;
	int venc_time;
	struct timespec ts;
	VIDEO_FRAME *frame = NULL;

	pthread_mutex_lock(&p->venc_mutex);
	while (p->venc_run) {
		while (p->venc_run && !(frame = frame_q_get(&p->venc_q))) {
			pthread_cond_wait(&p->venc_cond, &p->venc_mutex);
		}

		if (!frame)
			continue;
		p->frame_out = frame;
		p->frames_state[frame->index] = FRAME_STATE_OUT;

		venc_time = get_venc_time_and_ts(p, &ts);
		p->venc_flushing = 0;

		blit_duration = force_blit ? 0 : frame->blit_time - venc_time;

DBGSI serprintf("[%2d]%3d[%2d|%4d]", frame_q_count( &p->venc_q ), blit_duration, frame->index, frame->decode_time );
		if( s && s->paused ) {
DBGSI serprintf(" paused\n");
		} else if (blit_duration > 0) {
			int rc = 0;

			// do not hang on one pic forever
			if( blit_duration > frame->duration + 100 )
				blit_duration = frame->duration + 100;
DBGSI serprintf("\n");
			//DBGLOGV("sleep @ %10d.%3d  dur %3d  f(%2d)  blit %8d  venc %8d)", ts.tv_sec, ts.tv_nsec/1000000, blit_duration, frame->index, frame->blit_time, venc_time);

			timespec_add_ms(&ts, blit_duration);
			while (p->venc_run && rc == 0 && !p->venc_flushing) {
				rc = pthread_cond_timedwait(&p->venc_cond, &p->venc_mutex, &ts);
			}
		} else if (blit_duration < -10) {
//			LOG("drop[%2d]  blit %8d  venc %8d  diff %d", frame->index, frame->blit_time, venc_time, blit_duration);
serprintf(" DROP(%3d)\n", blit_duration);
			frame_q_put(&p->get_q, frame);
			p->frames_state[frame->index] = FRAME_STATE_DEQUEUED;
			goto endloop;
		} else {
DBGSI serprintf(" ok\n");
		}
		if( do_fake == 1 ) {
			frame_q_put(&p->get_q, frame);
			p->frames_state[frame->index] = FRAME_STATE_DEQUEUED;
		} else {
			if (android_buffer_queue(p->as, frame->handle[0]) != 0) {
				LOG("WARNING: frame(%d): android_buffer_queue failed",frame->index);
				goto endloop;
			}
			p->frames_state[frame->index] = FRAME_STATE_QUEUED;
			p->frames_num_owned--;
		}
endloop:
		p->frame_out = NULL;
		pthread_cond_broadcast(&p->venc_cond);
	}
	pthread_mutex_unlock(&p->venc_mutex);
	return NULL;
}

static VIDEO_FRAME *frame_get(priv_t *p, void *handle)
{
	int i;
	for (i = 0; i < p->num_frames; i++) {
		VIDEO_FRAME *frame = p->frames[i];
		if (frame->handle[0] == handle)
			return frame;
	}
	return NULL;
}

static VIDEO_FRAME *dequeue_frame( priv_t *p ) 
{
	VIDEO_FRAME *frame = frame_q_get(&p->get_q);
	if (!frame) {
		void *handle = NULL;
		if (p->frames_num_owned >= p->frames_max_owned)
			return NULL;
		if (android_buffer_dequeue(p->as, &handle) != 0)
			return NULL;
		frame = frame_get(p, handle);
		if (!frame) {
			LOG("WARNING: unknown handle %08X after dequeue, shouldn't happen", handle);
			return NULL;
		}
		p->frames_num_owned++;
		p->frames_state[frame->index] = FRAME_STATE_DEQUEUED;
	}
	return frame;
}

static void *copy_thread(void *ctx)
{
	STREAM_SINK_VIDEO *sink = (STREAM_SINK_VIDEO *)ctx;
	priv_t *p = sink->priv;
	
	VIDEO_FRAME *user_frame = NULL;
	VIDEO_FRAME *venc_frame = NULL;
	
	while (p->copy_run) {
		pthread_mutex_lock(&p->copy_mutex);
		while (p->copy_run && !(user_frame = frame_q_get(&p->copy_q))) {
			pthread_cond_wait(&p->copy_cond, &p->copy_mutex);
		}
		pthread_mutex_unlock(&p->copy_mutex);

		while(p->copy_run) {
			pthread_mutex_lock(&p->venc_mutex);
			venc_frame = dequeue_frame( p );
			pthread_mutex_unlock(&p->venc_mutex);
			if( venc_frame ) 
				break;
				
			// wait 10ms
			msec_sleep( 10 );
			
		}
		if( !p->copy_run ) {
			break;
		}
		venc_frame->time      = user_frame->time;
		venc_frame->blit_time = user_frame->blit_time;
		venc_frame->ofs_x     = user_frame->ofs_x;
		venc_frame->ofs_y     = user_frame->ofs_y;
		venc_frame->deinterlace = user_frame->deinterlace;

		if( user_frame->priv && do_copy ) {
			VIDEO_FRAME *shadow = user_frame->priv;
			int width = venc_frame->width + venc_frame->ofs_x;
			int height = venc_frame->height + venc_frame->ofs_y;
			
			if( !p->work_num ) {
				codec_convert_pixel_format( avimage2pixfmt(shadow->colorspace), shadow->data, shadow->linestep, width, height, venc_frame );
			} else {
				codec_convert_mt( p->mt_ctx, avimage2pixfmt(shadow->colorspace), shadow->data, shadow->linestep, width, height, venc_frame );
			}
		}

		pthread_mutex_lock(&p->venc_mutex);
		frame_q_put(&p->venc_q, venc_frame);
		pthread_cond_signal(&p->venc_cond);
		pthread_mutex_unlock(&p->venc_mutex);
		
		pthread_mutex_lock(&p->user_mutex);
		frame_q_put(&p->user_q, user_frame);
		pthread_mutex_unlock(&p->user_mutex);
		
		pthread_cond_broadcast(&p->copy_cond);
	}
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
	LOG("Set Crop to %dx%d - %dx%d", x, y, w, h);
	return android_buffer_setcrop(p->as, x, y, w, h);
}

static VIDEO_FRAME *sink_get_frame( STREAM_SINK_VIDEO *sink, int index )
{
	if (!sink || !sink->is_open)
		return NULL;
		
	priv_t *p = sink->priv;
	if( p->copy_buffers ) {
		return index < p->num_user_frames ? p->user_frames[index] : NULL;
	} else {
		return index < p->num_frames ? p->frames[index] : NULL;
	}
}

static int allocate_buffers(STREAM_SINK_VIDEO *sink, VIDEO_FRAME *(*frames)[], int *num_frames, int width, int height, int colorspace, android_surface_t *as)
{
	int i;
	priv_t *p = sink->priv;

	if (stream_alloc_frames( frames, width, height, colorspace, STREAM_MEM_ANDROID, num_frames) != 0) {
		LOG("stream_alloc_frames failed");
		return 1;
	}

	DBGLOG("num_frames: %d", p->num_frames);
	for (i = 0; i < *num_frames; i++) {
		VIDEO_FRAME *frame = (*frames)[i];

		android_buffer_t android_buffer = { 0 };

		if( as ) {
			if (android_buffer_alloc(as, &android_buffer) != 0) {
				int y;
				LOG("android_buffer_alloc failed, trying with %d frames instead of %d", i, *num_frames);
				for( y = i; y < *num_frames; y++ ) {
					if( (*frames)[y] ) {
						frame_free( (*frames)[y] );
					}
				}
				*num_frames = i;
				if (i == 0)
					return 1;
				break;
			}
		} else {
			android_buffer.type = BUFFER_TYPE_HW;
		}

		frame->index = i;
		frame->handle[0]  = android_buffer.handle;
#ifdef CONFIG_OMX_IOMX
		frame->android_handle = android_buffer.handle;
#else
		frame->android_handle = android_buffer.img_handle;
#endif
		if (android_buffer.type == BUFFER_TYPE_HW) {
			DBGLOG("android_buffer(hw) %dx%d/%d  siz %d", android_buffer.width, android_buffer.height, android_buffer.stride, android_buffer.size);
			frame->data[0]      = NULL;
			frame->linestep[0]  = android_buffer.stride;
			frame->data_size[0] = android_buffer.size;
			frame->size         = android_buffer.size;
		} else {
			DBGLOG("android_buffer(sw) %p %dx%d/%d", android_buffer.data, android_buffer.width, android_buffer.height, android_buffer.stride);

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
serprintf("as %08X  linestep %d/%d/%d  data_size %d/%d/%d\n", as, frame->linestep[0], frame->linestep[1], frame->linestep[2 ], frame->data_size[0], frame->data_size[1], frame->data_size[2]);
	}

	return 0;
}

static int sink_open(STREAM_SINK_VIDEO *sink, VIDEO_PROPERTIES *video, void *ctx, int num_frames, STREAM_RC *rc)
{
	int min_undequeued = num_undequeued;
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

	if (rc->cpu_type == OMXHW) {
		buffer_type = BUFFER_TYPE_HW;
		hal_format = android_get_hal_format(colorspace, buffer_type);
		LOG("using HW, colorspace: %d hal_format: 0x%X", colorspace, hal_format);
	} else {
		buffer_type = BUFFER_TYPE_SW;
		if (rc->cpu_type == OMXSW) {
			p->copy_buffers = 1;
			colorspace  = android_get_av_color(buffer_type);
			hal_format  = android_get_hal_format(colorspace, buffer_type);
 			if (hal_format == 0) {
 				colorspace  = android_get_av_color_unknown(buffer_type);
 				hal_format  = android_get_hal_format_unknown(colorspace, buffer_type);
 			}
			LOG("copy HW to SW, colorspace: %d hal_format: 0x%X", colorspace, hal_format);
		} else {
			colorspace  = android_get_av_color(buffer_type);
			hal_format  = android_get_hal_format(colorspace, buffer_type);
			if (hal_format == 0) {
				colorspace  = android_get_av_color_unknown(buffer_type);
				hal_format  = android_get_hal_format_unknown(colorspace, buffer_type);
			}
			LOG("using SW, colorspace: %d hal_format: 0x%X", colorspace, hal_format);
		}
		if (!color_conversion_supported(colorspace, avimage2pixfmt(video->colorspace)) && (colorspace != video->colorspace)) {
			LOG("copy HW to SW: color conversion not supported: %d to %d", colorspace, video->colorspace);
			goto err;
		}
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

	/*
	 * rk30 on jb wants output to be 32 aligned
	 */
	if (buffer_type == BUFFER_TYPE_HW &&
	    device_get_hw_type() == HW_TYPE_RK30 &&
	    device_get_android_version() == ANDROID_VERSION_JB) {
		int aligned_width = ALIGN(p->padded_width, 32);
		int aligned_height = ALIGN(p->padded_height, 32);
		p->padded_width = aligned_width;
		p->padded_height = aligned_height;
	}

	LOG("hal_format: 0x%X, buffer_type: %d hw_usage: 0x%X, size: %dx%d", hal_format, buffer_type, rc->hw_usage, p->padded_width, p->padded_height);

	if( p->copy_buffers ) {
		p->num_user_frames = num_frames;
		p->num_frames = num_sink_frames;
		
		if (android_buffer_setup(p->as, p->padded_width, p->padded_height, buffer_type, hal_format, rc->hw_usage, &p->num_frames, &min_undequeued) != 0) {
			LOG("android_buffer_setup failed");
			android_buffer_close(p->as);
			p->num_frames = 0;
			goto err;
		}
		if (p->num_frames > STREAM_MAX_FRAMES)
			p->num_frames = STREAM_MAX_FRAMES;

		// allocate the internal sink buffers
		if( allocate_buffers( sink, &p->frames, &p->num_frames, video->width, video->height, colorspace, p->as ) ) {
			goto err;
		}

		// allocate user buffers (dummy alloc, OMX will allocate actual memory)
		if( allocate_buffers( sink, &p->user_frames, &p->num_user_frames, video->width, video->height, colorspace, NULL ) ) {
			goto err;
		}

	} else {
		p->num_frames = num_frames;

		if (android_buffer_setup(p->as, p->padded_width, p->padded_height, buffer_type, hal_format, rc->hw_usage, &p->num_frames, &min_undequeued) != 0) {
			LOG("android_buffer_setup failed");
			android_buffer_close(p->as);
			p->num_frames = 0;
			goto err;
		}
		if (p->num_frames > STREAM_MAX_FRAMES)
			p->num_frames = STREAM_MAX_FRAMES;

		if( allocate_buffers( sink, &p->frames, &p->num_frames, video->width, video->height, colorspace, p->as ) ) {
			goto err;
		}

		if ((rc->cpu_type == STREAM_CPU_DSP3) && (rc->min_available_buffer < min_undequeued) && !num_undequeued) {
			LOG("Not enough minimum OMX buffer for rendering required %d, get %d", min_undequeued, rc->min_available_buffer);
			goto err;
		}

		// serve frames as user frames
		p->num_user_frames = p->num_frames;
	}

	sink->is_open = 1;

	frame_q_init(&p->get_q,  "get_q");
	frame_q_init(&p->venc_q, "venc_q");
	frame_q_init(&p->copy_q, "copy_q");
	frame_q_init(&p->user_q, "user_q");

	pthread_mutex_init(&p->venc_mutex, NULL);
	pthread_cond_init(&p->venc_cond, NULL);
	pthread_mutex_init(&p->copy_mutex, NULL);
	pthread_cond_init(&p->copy_cond, NULL);
	pthread_mutex_init(&p->user_mutex, NULL);


	LOG("num_frames %d  num_user_frames %d  min_undequeued %d", p->num_frames, p->num_user_frames, min_undequeued);

	p->frames_max_owned = p->num_frames - min_undequeued;

	int i;
	for (i = 0; i < p->frames_max_owned; ++i) {
		VIDEO_FRAME *frame = p->frames[i];

		frame_q_put(&p->get_q, frame);
		p->frames_num_owned++;
		p->frames_state[i] = FRAME_STATE_DEQUEUED;
		DBGLOG("put frame(%d)", i);
	}
	for (; i < p->num_frames; ++i) {
		VIDEO_FRAME *frame = p->frames[i];

		android_buffer_cancel(p->as, frame->handle[0]);
		p->frames_state[i] = FRAME_STATE_QUEUED;
		DBGLOG("cancel frame(%d)", i);
	}
	
	if( p->copy_buffers ) {
		for (i = 0; i < p->num_user_frames; ++i) {
			VIDEO_FRAME *frame = p->user_frames[i];
			frame_q_put(&p->user_q, frame);
		}
	}
	
	p->venc_run = 1;
	pthread_create(&p->venc_thread_handle, 0, venc_thread, (void*)sink);

	if( p->copy_buffers ) {
		p->copy_run = 1;

		pthread_mutex_init(&p->copy_mutex, NULL);
		pthread_cond_init(&p->copy_cond, NULL);
		pthread_mutex_init(&p->user_mutex, NULL);

		pthread_create(&p->copy_thread_handle, 0, copy_thread, (void*)sink);
		p->work_num = work_num;
		if( p->work_num ) {
			p->mt_ctx = codec_convert_mt_init( p->work_num );
		}
	}

	return sink->flush(sink);
err:
LOG("sink_open failed");
	// if there are some frames allocated, they need to be canceled (in close)
	for (i = 0; i < p->num_frames; ++i) {
		p->frames_state[i] = FRAME_STATE_DEQUEUED;
	}
	sink->close(sink);
	return -1;
}

static int sink_close(STREAM_SINK_VIDEO *sink)
{
	int i;
	priv_t *p = sink->priv;

DBGS serprintf("stream_sink_video_android: close\n");

	if( p->copy_buffers && p->copy_run ) {
		pthread_mutex_lock(&p->copy_mutex);
		p->copy_run = 0;
		pthread_cond_signal(&p->copy_cond);
		pthread_mutex_unlock(&p->copy_mutex);

		pthread_join(p->copy_thread_handle, NULL);

		if( p->mt_ctx ) {
			codec_convert_mt_exit( p->mt_ctx );
			p->mt_ctx = NULL;
		}
	}

	if (p->venc_run) {
		pthread_mutex_lock(&p->venc_mutex);
		p->venc_run = 0;
		pthread_cond_signal(&p->venc_cond);
		pthread_mutex_unlock(&p->venc_mutex);
		pthread_join(p->venc_thread_handle, NULL);
	}

	if (p->num_frames) {
		for (i = 0; i < p->num_frames; ++i) {
			if (p->frames_state[i]) {
				DBGLOG("cancel frame(%d)", i);
				android_buffer_cancel(p->as, p->frames[i]->handle[0]);
				p->frames_state[i] = FRAME_STATE_QUEUED;
			}
		}
		stream_free_frames(&(p->frames), p->num_frames);
		android_buffer_close(p->as);
	}
	if (p->copy_buffers && p->num_user_frames) {
		stream_free_frames(&(p->user_frames), p->num_user_frames);
	}
	if (sink->is_open) {
		pthread_mutex_destroy(&p->venc_mutex);
		pthread_cond_destroy(&p->venc_cond);

		pthread_mutex_destroy(&p->copy_mutex);
		pthread_cond_destroy(&p->copy_cond);
		pthread_mutex_destroy(&p->user_mutex);

		frame_q_flush(&p->get_q);
		frame_q_flush(&p->venc_q);
		frame_q_flush(&p->copy_q);
		frame_q_flush(&p->user_q);
	}

	if (p->as)
		android_surface_destroy(&p->as);

	p->num_frames       = 0;
	p->num_user_frames  = 0;
	p->frame_out        = NULL;
	p->frames_num_owned = p->frames_max_owned = 0;
	p->interlaced       = 0;
	p->width = p->height = p->padded_width = p->padded_height = 0;
	p->ofs_x = p->ofs_y  = 0;
	p->aspect_n  = p->aspect_d      = 0;
	p->venc_flushing = 0;
	p->copy_run = p->venc_run = 0;
	p->copy_buffers = 0;
	memset(&p->venc_ref_time, 0, sizeof(p->venc_ref_time));

	sink->is_open = 0;

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

	setcrop(p, frame->ofs_x, frame->ofs_y, frame->width, frame->height);

	int venc_time = get_venc_time(p);
	if( do_fake == 2 ) {
		frame_q_put(&p->get_q, frame);
	
	} else if( p->copy_buffers ) {
		pthread_mutex_lock(&p->copy_mutex);
		frame_q_put(&p->copy_q, frame);
		pthread_cond_signal(&p->copy_cond);
		pthread_mutex_unlock(&p->copy_mutex);
	} else {
		pthread_mutex_lock(&p->venc_mutex);
		frame_q_put(&p->venc_q, frame);
		pthread_cond_signal(&p->venc_cond);
		pthread_mutex_unlock(&p->venc_mutex);
	}

	DBGSI2 LOG("frame %2d/%8d  handle %08X  own %2d  venc/blit %8d/%8d", frame->index, frame->time, frame->handle[0], p->frames_num_owned, venc_time, frame->blit_time);

	return venc_time;
}

static int sink_get(STREAM_SINK_VIDEO *sink, VIDEO_FRAME **pframe)
{
	priv_t *p = sink->priv;
	VIDEO_FRAME *frame;

	if (!sink->is_open) {
		*pframe = NULL;
		return 1;
	}
	
	if( p->copy_buffers ) {
		pthread_mutex_lock(&p->user_mutex);
		frame = frame_q_get(&p->user_q);
		pthread_mutex_unlock(&p->user_mutex);
	} else {
		pthread_mutex_lock(&p->venc_mutex);
		frame = dequeue_frame( p );
		pthread_mutex_unlock(&p->venc_mutex);
	}
	
	*pframe = frame;
	if( frame ) {
		DBGSI2 LOG("frame %2d/%8d  own %2d", frame->index, frame->time, p->frames_num_owned);
	}
	return frame ? 0 : 1;
}

static int sink_flush_and_enqueue(STREAM_SINK_VIDEO *sink)
{
	priv_t *p = sink->priv;
	int i, max;

	if (!sink->is_open)
		return 0;

	LOG();

	pthread_mutex_lock(&p->venc_mutex);
	frame_q_flush(&p->venc_q);

	p->venc_flushing = 1;
	pthread_cond_signal(&p->venc_cond);

	while (p->frame_out != NULL)
		pthread_cond_wait(&p->venc_cond, &p->venc_mutex);

	max = p->frames_max_owned - p->frames_num_owned;
	for (i = 0; i < max; i++) {
		VIDEO_FRAME *frame;
		void *handle;

		if (android_buffer_dequeue(p->as, &handle) != 0)
			break;
		frame = frame_get(p, handle);
		if (!frame) {
			LOG("WARNING: unknow frame after dequeue, shouldn't happen");
			break;
		}
		p->frames_state[frame->index] = FRAME_STATE_DEQUEUED;
		p->frames_num_owned++;
	}

	frame_q_flush(&p->get_q);
	max = p->num_frames;
	for (i = 0; i < max; ++i) {
		if (p->frames_state[i] == FRAME_STATE_DEQUEUED)
			frame_q_put(&p->get_q, p->frames[i]);
		p->frames[i]->time = -1;
	}

	clock_gettime(CLOCK_REALTIME, &p->venc_ref_time);

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

static int sink_get_time(STREAM_SINK_VIDEO *sink)
{
	priv_t *p = sink->priv;
	int venc_time;

	if (!sink->is_open)
		return 0;

	venc_time = get_venc_time(p);

	return venc_time;
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

STREAM_SINK_VIDEO *stream_sink_video_android_new(void *surface_handle) 
{
	STREAM_SINK_VIDEO *sink = (STREAM_SINK_VIDEO *) acalloc(1, sizeof(STREAM_SINK_VIDEO));
	priv_t *p = (priv_t *) acalloc(1, sizeof(priv_t));

	if (!sink || !p)
		goto err;

	sink->name      = "android";
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
DECLARE_DEBUG_TOGGLE ("ssdc", do_copy  );
DECLARE_DEBUG_PARAM  ("ssdf", do_fake  );
DECLARE_DEBUG_PARAM  ("sswn", work_num );
DECLARE_DEBUG_PARAM  ("snsf", num_sink_frames );
DECLARE_DEBUG_PARAM  ("snud", num_undequeued );
DECLARE_DEBUG_TOGGLE ("ssfb", force_blit );
#endif
