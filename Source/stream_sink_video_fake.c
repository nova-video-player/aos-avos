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
#include "stream.h"
#include "stream_alloc.h"
#include "util.h"
#include "fb.h"
#include "atime.h"
#include "image.h"
#include "astdlib.h"
#include "frame_q.h"

#include "athread.h"

#ifdef CONFIG_STREAM

#define DBGS 	if(Debug[DBG_STREAM])
#define DBGSI 	if(Debug[DBG_SINK])
#define DBGQ   	if(Debug[DBG_Q])

typedef struct SINK_PRIV {
	VIDEO_FRAME *frames[STREAM_MAX_FRAMES];

	int num_frames;

	int width;
	int height;
	int padded_width;
	int padded_height;

	int put_num;
	int get_num;

	FRAME_Q q;

} SINK_PRIV;

static int _get_time( STREAM_SINK_VIDEO *sink )
{
	return 0;
}

static int _put( STREAM_SINK_VIDEO *sink, VIDEO_FRAME *frame )
{
	SINK_PRIV *p = sink->priv;
//serprintf("put %d %d\r\n", frame->index, frame->blit_time );
	frame_q_put( &p->q, frame );
DBGQ serprintf("SINK[%d|%d] ", frame->index, frame_q_count( &p->q ) );
	return 0;
}

static int _get( STREAM_SINK_VIDEO *sink, VIDEO_FRAME **frame )
{
	SINK_PRIV *p = sink->priv;
	VIDEO_FRAME *_frame = frame_q_get( &p->q );
	
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

	frame_q_flush( &p->q );
	
	int i;
	for( i = 0; i < p->num_frames; i++ ) {
		p->frames[i]->time = -1;	
		frame_q_put( &p->q, p->frames[i] );	
	}
	return 0;
}

static int alloc_frames( SINK_PRIV *p, VIDEO_PROPERTIES *video ) 
{
DBGS serprintf("ssvf: alloc_frames(%d)\r\n", p->num_frames);
	int i;
	for( i = 0; i < STREAM_MAX_FRAMES; i++ ) {
		p->frames[i] = NULL;
	}

	if (p->num_frames)
		p->num_frames += 3;
	else
		p->num_frames = STREAM_MAX_FRAMES;
	int max = p->num_frames;
	int memtype = STREAM_MEM_NRM;

	stream_alloc_frames( &(p->frames), p->width, p->height, video->colorspace, memtype, &max );
	
	if( p->num_frames && max != p->num_frames ) {
		// we were asked for exactly num_frames
serprintf("stream_sink_video: OOM: %d\r\n", max);
		return 1;
	}
	p->num_frames = max;
	
	return 0;
}

static int free_frames( SINK_PRIV *p ) 
{
	return stream_free_frames( &(p->frames), p->num_frames );
}

static VIDEO_FRAME *_get_frame( STREAM_SINK_VIDEO *sink, int index )
{
	if(!sink || !sink->is_open)
		return NULL;
		
	SINK_PRIV *p = sink->priv;
	return index < p->num_frames ? p->frames[index] : NULL;
}

static int _open( STREAM_SINK_VIDEO *sink, VIDEO_PROPERTIES *video, void *ctx, int num_frames, STREAM_RC *rc )
{
	sink->ctx = ctx;
	SINK_PRIV *p = sink->priv;

	sink->resize( sink, video );
	p->num_frames = num_frames;
		
DBGS serprintf("ssvf: open: WxH %d x %d  num_frames %d  cached %d\r\n", p->width, p->height, p->num_frames, rc->output_cached);
	if (sink->is_open)
		return 1;

	// allocate out frames
	if( alloc_frames( p, video ) ) {
		free_frames( p );
		return 1;
	}
	
	frame_q_init( &p->q, "q" );
	
	_flush( sink );

	sink->is_open = 1;

	return 0;
}

static int _close( STREAM_SINK_VIDEO *sink )
{
DBGS serprintf("ssvf: close\r\n");
	SINK_PRIV *p = sink->priv;

	// free the out frames
	free_frames( p );

	sink->is_open = 0;
	return 0;
}

static int _delete( STREAM_SINK_VIDEO *sink )
{
DBGS serprintf("ssvf: delete\r\n");
	afree( sink );
	return 0;
}

static int _clear( STREAM_SINK_VIDEO *sink )
{
	return 0;
}

static int _end( STREAM_SINK_VIDEO *sink )
{
	return 0;
}

static int _syncable( STREAM_SINK_VIDEO *sink )
{
	return 0;
}

static int _delay( STREAM_SINK_VIDEO *sink )
{
	return 0;
}

static int _resize( STREAM_SINK_VIDEO *sink, VIDEO_PROPERTIES *video )
{
	SINK_PRIV *p = sink->priv;
	p->width = video->width;
	p->height = video->height;
	if (!video->padded_width) {
		p->padded_width = (video->width + 0x0f) & ~0x0f;
		p->padded_height = pad_height(video->height);
	} else {
		p->padded_width = video->padded_width;
		p->padded_height = video->padded_height;
	}
	return 0;
}

STREAM_SINK_VIDEO *stream_sink_video_FAKE_new( void ) 
{
	STREAM_SINK_VIDEO *sink = acalloc( 1, sizeof( STREAM_SINK_VIDEO ) );
	if( sink ) {
		sink->name     = "FAKE";
		sink->open     = _open;
		sink->close    = _close;
		sink->delete   = _delete;
		sink->put      = _put;
		sink->get      = _get;
		sink->flush    = _flush;
		sink->end      = _end;
		sink->syncable = _syncable;
		sink->delay    = _delay;
		sink->get_frame = _get_frame;
		sink->get_time = _get_time;
		sink->clear    = _clear;
		sink->resize   = _resize;
		
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
