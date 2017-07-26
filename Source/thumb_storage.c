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

#include "types.h"
#include "global.h"
#include "astdlib.h"
#include "debug.h"
#include "fs.h"
#include "image.h"
#include "av.h"
#include "atf.h"
#include "thumb.h"
#include "stream.h"
#include "util.h"
#include "app_av.h"

#include <errno.h>
#include <string.h>
#include <stdio.h>

#define ERR  if(1)
#define DBG  if(Debug[DBG_THUMB])
#define DBG2 if(Debug[DBG_THUMB] > 1)

/* Align on android mini thumb width */ 
#define ANDROID_THUMB_WIDTH	512

#define THUMB_TIME (200 * 1000)

#ifdef CONFIG_VIDEO
// ************************************************
//
//	_storage_format_video
//
// ************************************************
static void _storage_format_video( VIDEO_FRAME *frame, IMAGE *vid_img )
{
	int i = 1024;

	vid_img->width = frame->width;
	vid_img->height = frame->height;

	while( vid_img->height * linestep_from_width( vid_img->width ) > vid_img->size ){
		vid_img->width = frame->width * i / 1024;
		vid_img->height = frame->height * i / 1024;
		i--;
	}
	vid_img->width &= ~1;
	vid_img->height &= ~1;

	if( i == 0 ){
ERR serprintf("THUMB: frame too big\r\n");
		return;
	}

	vid_img->linestep[0] = linestep_from_width( vid_img->width );

	frame->window.x = frame->ofs_x;
	frame->window.y = frame->ofs_y;
	frame->window.width  = frame->width;

	frame->window.height = frame->height;
	
	if( frame->interlaced == VIDEO_INTERLACED_ONE_FIELD ) {
		// one field only, half the height
		frame->window.height /= 2;
	}

	vid_img->window.x = 0;
	vid_img->window.y = 0;
	vid_img->window.width = vid_img->width;
	vid_img->window.height = vid_img->height;

	image_software_resize( (IMAGE*)frame, vid_img );
}

//
// define a dummy video sink for the thumb, we just need one frame
// which we copy to out buffer
//
static int _dummy_open( STREAM_SINK_VIDEO *sink, VIDEO_PROPERTIES *video, void *ctx, int num_frames, STREAM_RC *rc )
{
        return 0;
}

static int _dummy( STREAM_SINK_VIDEO *sink )
{
        return 0;
}

static IMAGE *_alloc_img(VIDEO_FRAME *frame)
{
	float scale = (float)ANDROID_THUMB_WIDTH / frame->width;
	return image_alloc( ANDROID_THUMB_WIDTH, (int)(frame->height * scale), AV_IMAGE_YUV_422 );
}

static int _put_thumb( STREAM_SINK_VIDEO *sink, VIDEO_FRAME *frame )
{
	serprintf("_put_thumb\n");
	
	if(!sink->priv) {
       	 	// use only the first frame that we get!
		IMAGE *img = _alloc_img( frame );

		sink->priv = img;

DBG serprintf("%s: %d\r\n", __FUNCTION__, frame->time);
                // Format and save frame as thumbnail
                _storage_format_video( frame, img );
        }
        return frame->blit_time;
}

static int _get_thumb( STREAM_SINK_VIDEO *sink, VIDEO_FRAME **frame  )
{
        *frame = NULL;
        return 1;
}

// ************************************************
//
//      _delete
//
// ************************************************
static int _delete( STREAM_SINK_VIDEO *sink )
{
	afree(sink);
	return 0;
}

// ************************************************
//
//      _thumb_sink_new
//
// ************************************************
static STREAM_SINK_VIDEO *_thumb_sink_new( void )
{
        STREAM_SINK_VIDEO *sink = acalloc(1, sizeof(STREAM_SINK_VIDEO));
        if( !sink )
                return NULL;

	sink->priv = NULL;

        sink->name      = "thumb";
        sink->open      = _dummy_open;
        sink->close     = _dummy;
	sink->delete    = _delete;
        sink->put       = _put_thumb;
        sink->get       = _get_thumb;
        sink->flush     = _dummy;
        sink->syncable  = _dummy;
        sink->get_time  = _dummy;

        return sink;
}


IMAGE *thumb_get_image_from_url(STREAM_URL *src, int etype, int *stream_error, int thumb_time, int option)
{
	IMAGE *img = NULL;
        STREAM *stream;

        STREAM_SINK_VIDEO *sink = NULL;

DBG serprintf( "%s %s\r\n", __FUNCTION__, cut_extension( src->url ) );

        if( ! ( stream  = stream_new() ) ) {
ERR serprintf("%s : cannot create stream\r\n", __FUNCTION__);
                goto ErrorExit;
        }

        if( !( sink = _thumb_sink_new() ) ) {
ERR serprintf("%s : cannot create sink\r\n", __FUNCTION__);
                goto ErrorExit;
        }

        stream_set_buffer_size( stream, 12 );   // use 12 MB for thumb creation 

	stream_set_max_video_dimensions( stream, VIDEO_MAX_WIDTH, VIDEO_MAX_HEIGHT );
	stream_set_video_sink( stream, sink );

        if ( stream_open( stream, src, etype, STREAM_MULTI | STREAM_THUMB ) ) {
serprintf("thumb: ve %d\r\n", stream->video_error );
                *stream_error = stream->video_error;
                goto ErrorExit;
        }
	
	// now lets decide where to get the thumb from:
	int start;
	int duration;
	stream_get_current_time(stream, &duration);
	if( !duration ) {
		int total;
		stream_get_current_pos( stream, &total );
		start = total / 2;
serprintf("get thumb at pos %d\r\n", start );
	} else {
		// set thumb_time only if stream has duration.
		if (thumb_time != -1 && thumb_time <= duration ) {
			start = thumb_time;
		} else {
			start = duration / 2;
			start = MIN( THUMB_TIME, start );
		}
serprintf("get thumb at time %d  duration %d\r\n", start, duration );
	}
		
	stream_set_start_time( stream, start );
	
        if ( stream_start( stream ) ) {
serprintf("thumb: ve %d\r\n", stream->video_error );
                *stream_error = stream->video_error;
                goto ErrorExit;
        }

        if( !sink->priv ) {
serprintf("thumb: no blit!\r\n" );
                // thumb sink was given no frame, so something
                // is wrong with this video, declare an error
                *stream_error = VE_ERROR;
                goto ErrorExit;
        } else {
		img = (IMAGE *)sink->priv;
		*stream_error = VE_NO_ERROR;
	}

ErrorExit:
        stream_stop( stream );
	stream_delete( &stream );
        return img;
}

IMAGE *thumb_get_image_from_frame(VIDEO_FRAME *frame)
{
	IMAGE *vid_img = _alloc_img( frame );;

	if (vid_img) 
		_storage_format_video( frame, vid_img );
        return vid_img;
}

#ifdef DEBUG_MSG
#include "file_type.h"

// ***************************************************************
//
// 	make_thumb
//
// ***************************************************************
static void make_thumb( int argc, char *argv[] )
{
	int time = -1;
	int etype;
	int error;
	STREAM_URL src;
	const char *full_path;
	if (argc <= 1) {
		return;
	}
	full_path = argv[1];
	if( argc > 2 ) {
		time = atoi( argv[2] );
	}

	stream_url_cpy_url( &src, full_path );
	get_url_type( &src, NULL, &etype );
serprintf("make thumb for %s at %d\n", full_path, time );
	IMAGE *img = thumb_get_image_from_url( &src, etype, &error, time, 0 );
	
	if(img) {
		av_dump_video_frame( (VIDEO_FRAME*)img );
		image_free(img);
	}
}
DECLARE_DEBUG_COMMAND("mkt", make_thumb );
#endif

#endif	// CONFIG_VIDEO
