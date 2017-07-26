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
#include "stream_sink_video.h"
#include "stream_resizer.h"
#include "stream.h"
#include "stream_alloc.h"
#include "util.h"
#include "fb.h"
#include "atime.h"
#include "astdlib.h"
#include "frame_q.h"
#include "image.h"
#include "athread.h"

#ifdef CONFIG_STREAM
#ifdef CONFIG_FB_QVFB

#define DBGS 	if(Debug[DBG_STREAM])
#define DBGSI 	if(Debug[DBG_SINK])
#define DBGSI2  if(Debug[DBG_SINK] > 1)
#define DBGQ   	if(Debug[DBG_Q])

static int _sink_force_single_frame = 1;

#ifdef DEBUG_MSG
DECLARE_DEBUG_TOGGLE("sfsf", _sink_force_single_frame );
#endif

typedef struct SINK_PRIV {
	int		width;
	int		height;
	int 		colorspace;
	int		num_frames;
	VIDEO_FRAME	*frames[STREAM_MAX_FRAMES];

	VIDEO_FRAME	*blit_frame;

	STREAM_RSZ_PARAMS rsz;

	FRAME_Q empty;		// empty frames to be filled
	FRAME_Q full;		// full frames  to be displayed

	pthread_t 	venc_thread_handle;
	pthread_mutex_t venc_mutex;
	pthread_cond_t  venc_cond;
	int		venc_run;

	int 		venc_ref_time;
	int		venc_put_time;
	int 		venc_time;
	
	VIDEO_FRAME	*out_frame;
	
	int 		dropped;
	int 		total_dropped;

} SINK_PRIV;

void copy_yuv_image_rotated( int xpos, int ypos, const IMAGE *src, const IMAGE *dst, int fx_mask );

#define VIDEO_IMAGE { \
		.colorspace  = AV_IMAGE_YUV_422, \
		.bpp[0]      = 2, \
		.data[0]     = FB_get_video_data(), \
		.linestep[0] = FB_get_video_linestep(), \
		.width       = FB_get_video_width(), \
		.height      = FB_get_video_height(), \
		.size        = 2 * FB_get_video_width() * FB_get_video_height(), \
		} 

static void blit_video_frame( SINK_PRIV *p, VIDEO_FRAME *frame )
{
	IMAGE dst = VIDEO_IMAGE;
	
	// local data we can tweak
	VIDEO_FRAME src;
	memcpy( &src, frame, sizeof( VIDEO_FRAME ) );

	src.window = p->rsz.src;

	if ( p->rsz.rotation == 0 || p->rsz.rotation == 180 ) {
		dst.window = p->rsz.dst;
		image_resize( (IMAGE*)&src, &dst ); 	
	} else {
		VIDEO_FRAME *rot = frame_alloc( FB_get_video_height(), FB_get_video_width() );
		rot->window = p->rsz.dst;
		
		image_resize( (IMAGE*)&src, (IMAGE*)rot ); 	
		dst.height = rot->height;
		dst.width  = rot->width;
		
		copy_yuv_image_rotated( 0, 0, (IMAGE*)rot, &dst, 0 );
	
		frame_free( rot );
	}
	FB_update( FB_UPDATE_VID );
}

static int _put_time( STREAM_SINK_VIDEO *sink, int time )
{
	SINK_PRIV *p = sink->priv;
	
	p->venc_put_time = time;
	p->venc_ref_time = atime();

DBGSI2 serprintf("[[put %8d]]", time );	
	return 0;
}

static int _get_time( STREAM_SINK_VIDEO *sink )
{
	SINK_PRIV *p = sink->priv;
	
	int diff = atime() - p->venc_ref_time;
	p->venc_time = p->venc_put_time + diff;
//serprintf("get %8d + %8d = %8d\n", p->venc_put_time, diff, p->venc_time );	
	
	return p->venc_time;
}

// ************************************************************
//
//	venc_thread
//
// ************************************************************
static void *venc_thread( void *data )
{
	STREAM_SINK_VIDEO *sink = (STREAM_SINK_VIDEO *)data;
	SINK_PRIV *p = sink->priv;
DBGS serprintf("venc_thread::Starting\r\n");	
	
	pthread_mutex_lock( &p->venc_mutex );

	while( p->venc_run ) {
		
		// get frame
		while (p->venc_run && !(p->out_frame = frame_q_get( &p->full )) ) {
			pthread_cond_wait(&p->venc_cond, &p->venc_mutex);
		}
 
		if (!p->out_frame) {
serprintf("venc_thread::stop 0\n");
			continue;
		}			
			
		VIDEO_FRAME *f = p->out_frame;
		int venc_time;
		int blit_duration;
RETRY:
		venc_time = _get_time(sink);
		blit_duration = f->blit_time - venc_time;

		int do_render = 1;
		if( p->venc_put_time == 1 ) {
			p->venc_put_time --;
		} else if (blit_duration > 10) {
			// too early, wait
			if( !p->venc_put_time ) {
				// put is zero, better wait a bit and retry
				pthread_mutex_unlock(&p->venc_mutex);
				serprintf("$");
				msec_sleep( 10 );
				pthread_mutex_lock( &p->venc_mutex );
				if( !p->venc_run ) {
					break;
				}
				goto RETRY;
			}
DBGSI serprintf("wait %3d  f %08d  v %08d\n", blit_duration, f->blit_time, venc_time);
			msec_sleep( blit_duration - 10 );
		} else if( blit_duration < -100 && p->dropped < 5 ) {
			// too late, drop it
			p->dropped ++;
			p->total_dropped ++;
			do_render = 0;
DBGSI serprintf("drop %3d  f %08d  v %08d\n", blit_duration, f->blit_time, venc_time);
		}
		
		if( do_render ) {
DBGSI serprintf("blit %3d\n", blit_duration);
			p->dropped = 0;
			VIDEO_FRAME *blit_f = f;
 
 			if( f->dec ) {
				STREAM_DEC_VIDEO *dec = (STREAM_DEC_VIDEO*)f->dec;
			 	if( dec->render ) {
					blit_f = p->blit_frame;
					dec->render( dec, blit_f, f );
				}
			}
 			blit_video_frame( p, blit_f );
		} else {
 			if( f->dec ) {
				STREAM_DEC_VIDEO *dec = (STREAM_DEC_VIDEO*)f->dec;
			 	if( dec->render ) {
					dec->render( dec, NULL, f );
				}
			}
		}

		frame_q_put( &p->empty, f );
		p->out_frame = NULL;
	
	}
	pthread_mutex_unlock(&p->venc_mutex);

DBGS serprintf("venc_thread::stopped\r\n");	
	return NULL;
}

static int _put( STREAM_SINK_VIDEO *sink, VIDEO_FRAME *frame )
{
	SINK_PRIV *p = sink->priv;
//serprintf("put %d %d\r\n", frame->index, frame->blit_time );
	
	pthread_mutex_lock(&p->venc_mutex);
	frame_q_put( &p->full, frame );
	pthread_cond_broadcast(&p->venc_cond);
	pthread_mutex_unlock(&p->venc_mutex);

DBGQ serprintf("SINK[%d|%d] ", frame->index, frame_q_count( &p->full ) );
	return p->venc_time;
}

static int _get( STREAM_SINK_VIDEO *sink, VIDEO_FRAME **frame )
{
	SINK_PRIV *p = sink->priv;
	VIDEO_FRAME *_frame = frame_q_get( &p->empty );
	
	if( !_frame ) {
		return 1;
	}
		
//serprintf("get %d\r\n", _frame->index);
	*frame = _frame;
	return 0; 
}

static int _flush( STREAM_SINK_VIDEO *sink )
{
	SINK_PRIV *p = sink->priv;
DBGS serprintf("stream_sink_video_flush_SIM\r\n");
	pthread_mutex_lock ( &p->venc_mutex );
	
	frame_q_flush( &p->empty );
	frame_q_flush( &p->full  );
	p->out_frame = NULL;
	
	int i;
	for( i = 0; i < p->num_frames; i++ ) {
		p->frames[i]->time = -1;	
		frame_q_put( &p->empty, p->frames[i] );	
	}

	pthread_mutex_unlock ( &p->venc_mutex );
	return 0;
}

static int alloc_frames( SINK_PRIV *p, int cached ) 
{
DBGS serprintf("stream_sink_video: alloc_frames(%d)\r\n", p->num_frames);
	int i;
	for( i = 0; i < STREAM_MAX_FRAMES; i++ ) {
		p->frames[i] = NULL;
	}

	int max = p->num_frames ? p->num_frames : STREAM_MAX_FRAMES;
	for( i = 0; i < max; i++ ) {
		if( !(p->frames[i] = frame_alloc_with_cs_and_mem( p->width, p->height, p->colorspace, _sink_force_single_frame ? STREAM_MEM_BYO : STREAM_MEM_NRM ) ) ) {
			if( p->num_frames ) {
				// we were asked for num_frames
serprintf("stream_sink_video: OOM\r\n");
				return 1;
			} else {
				// that is all we can do, stop
				break;
			}
		}
		p->frames[i]->index = i;
	}
	p->num_frames = i;
DBGS serprintf("stream_sink_video: allocated %d frames\r\n", p->num_frames);

	if( _sink_force_single_frame ) {
		// alloc blit frame:
		if( !(p->blit_frame = frame_alloc_with_cs_and_mem( p->width, p->height, p->colorspace, STREAM_MEM_NRM ) ) ) {
serprintf("stream_sink_video: cant alloc blit frame\r\n");
			return 1;
		}
	}
	
	return 0;
}

static int free_frames( SINK_PRIV *p ) 
{
	int i;
	
	for( i = 0; i < p->num_frames; i++ ) {
		if( p->frames[i] ) {
			frame_free( p->frames[i] );
		}
	}
	if( p->blit_frame ) {
		frame_free( p->blit_frame );
	}
	return 0;
}

static VIDEO_FRAME *_get_frame( STREAM_SINK_VIDEO *sink, int index )
{
	if(!sink || !sink->is_open)
		return NULL;
		
	SINK_PRIV *p = sink->priv;
	return index < p->num_frames ? p->frames[index] : NULL;
}

static int _clear( STREAM_SINK_VIDEO *sink )
{
	SINK_PRIV *p = sink->priv;
	int i;
	for( i = 0; i < p->num_frames; i++ ) {
		if( p->frames[i]->data[0] ) {
			image_full_window( (IMAGE*)p->frames[i] );
			image_fill_window( (IMAGE*)p->frames[i], YUV_BLACK );
		}
	}

	return 0;
}

static int _open( STREAM_SINK_VIDEO *sink, VIDEO_PROPERTIES *video, void *ctx, int num_frames, STREAM_RC *rc )
{
	if (sink->is_open)
		return 1;
	
	sink->ctx = ctx;
	SINK_PRIV *p = sink->priv;
	p->num_frames = num_frames + 3;

	if( p->num_frames > STREAM_MAX_FRAMES )
		return 1;
	
	sink->resize( sink, video );
DBGS serprintf("stream_sink_video_open: WxH %d x %d  num_frames %d  cached %d\r\n", p->width, p->height, p->num_frames, rc->output_cached);
	
	// allocate out frames
	if( alloc_frames( p, rc->output_cached ) ) {
		free_frames( p );
		return 1;
	}
	
	frame_q_init( &p->empty, "empty" );
	frame_q_init( &p->full,  "full"  );
	pthread_mutex_init( &p->venc_mutex, NULL );
	pthread_cond_init ( &p->venc_cond, NULL );
	
	_clear( sink );
	_flush( sink );
	
	// lock and start the venc_thread
	pthread_mutex_lock( &p->venc_mutex );
	p->venc_run = 1;
	apthread_create( &p->venc_thread_handle, 0, venc_thread, (void*)sink, "video sink venc" );

	pthread_mutex_unlock( &p->venc_mutex );

	sink->is_open = 1;

	return 0;
}

static int _close( STREAM_SINK_VIDEO *sink )
{
	SINK_PRIV *p = sink->priv;

DBGS serprintf("stream_sink_video_close_SIM, dropped %d\n", p->total_dropped);
	if( !sink->is_open ) {
serprintf("ssv not open!\r\n");
		return 1;
	}

	if( p->venc_run ) {
DBGS serprintf("waiting for venc thread to join\r\n");
		pthread_mutex_lock( &p->venc_mutex );
		p->venc_run = 0;
		pthread_cond_broadcast(&p->venc_cond);
		pthread_mutex_unlock( &p->venc_mutex );
		apthread_join( p->venc_thread_handle, NULL );
DBGS serprintf("venc_thread joined\r\n");
	}
	
	pthread_mutex_destroy( &p->venc_mutex );
	pthread_cond_destroy(&p->venc_cond);

	// free the out frames
	free_frames( p );

	sink->is_open = 0;
	return 0;
}

static int _delete( STREAM_SINK_VIDEO *sink )
{
DBGS serprintf("ssv: delete\r\n");
	// free the priv data
	afree( sink->priv );
	
	afree( sink );
	return 0;
}

static int _end( STREAM_SINK_VIDEO *sink )
{
	return 0;
}

static int _syncable( STREAM_SINK_VIDEO *sink )
{
	return 1;
}

static int _resize( STREAM_SINK_VIDEO *sink, VIDEO_PROPERTIES *video )
{
	SINK_PRIV *p = sink->priv;
	
	video_resizer_DSS.setup( &video_resizer_DSS, &sink->primary, video, &p->rsz );

DBGS serprintf("ssv4l2: resize: rotation: %d\r\n", p->rsz.rotation);
DBGS Rect_Dump( &p->rsz.src, "src" );
DBGS Rect_Dump( &p->rsz.dst, "dst" );
	
	p->width      = video->width;
	p->height     = video->height;
	p->colorspace = video->colorspace;

	return 0;
}

STREAM_SINK_VIDEO *stream_sink_video_SIM2_new( void ) 
{
	STREAM_SINK_VIDEO *sink = acalloc( 1, sizeof( STREAM_SINK_VIDEO ) );
	if( sink ) {
		sink->name        = "SIM2";
		sink->open        = _open;
		sink->close 	  = _close;
		sink->delete 	  = _delete;
		sink->put	  = _put;
		sink->get	  = _get;
		sink->flush	  = _flush;
		sink->end	  = _end;
		sink->syncable	  = _syncable;
		sink->get_frame   = _get_frame;
		sink->get_time	  = _get_time;
		sink->put_time	  = _put_time;
		sink->clear	  = _clear;
		sink->resize	  = _resize;
		
		sink->primary     = STREAM_SINK_DEFAULT_SCREEN;

		sink->allocates_frames = 1;
				
		// allocate private data
		if( !(sink->priv = acalloc( 1, sizeof( SINK_PRIV ) ) ) ) {
			afree( sink );
			return NULL;
		}
	}
	return sink;	
}

#endif
#endif
