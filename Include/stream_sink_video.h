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

#ifndef _STREAM_SINK_VIDEO_H
#define _STREAM_SINK_VIDEO_H

#include "av.h"
#include "stream_resizer.h"
#include "stream_rc.h"

//
// SINK_VIDEO
//
enum {
	STREAM_OUTPUT_NONE = -1,
	STREAM_OUTPUT_PRIMARY,
	STREAM_OUTPUT_SECONDARY,
	STREAM_OUTPUT_ALL,
	STREAM_OUTPUT_ANDROID
};

struct STREAM_SINK_VIDEO;
 
typedef int (*SINK_VIDEO_OPEN )  ( struct STREAM_SINK_VIDEO *sink, VIDEO_PROPERTIES *video, void *ctx, int num_frames, STREAM_RC *rc );
typedef int (*SINK_VIDEO_CLOSE)  ( struct STREAM_SINK_VIDEO *sink );
typedef int (*SINK_VIDEO_DELETE) ( struct STREAM_SINK_VIDEO *sink );
typedef int (*SINK_VIDEO_PUT)    ( struct STREAM_SINK_VIDEO *sink, VIDEO_FRAME *frame  );
typedef int (*SINK_VIDEO_GET)    ( struct STREAM_SINK_VIDEO *sink, VIDEO_FRAME **frame );
typedef int (*SINK_VIDEO_FLUSH)  ( struct STREAM_SINK_VIDEO *sink );
typedef int (*SINK_VIDEO_END)    ( struct STREAM_SINK_VIDEO *sink );
typedef int (*SINK_VIDEO_SYNC)   ( struct STREAM_SINK_VIDEO *sink );
typedef int (*SINK_VIDEO_DELAY)  ( struct STREAM_SINK_VIDEO *sink );
typedef VIDEO_FRAME *(*SINK_VIDEO_GETFRAME)( struct STREAM_SINK_VIDEO *sink, int index );
typedef int (*SINK_VIDEO_GETTIME)( struct STREAM_SINK_VIDEO *sink );
typedef int (*SINK_VIDEO_PUTTIME)( struct STREAM_SINK_VIDEO *sink, int time );
typedef int (*SINK_VIDEO_CLEAR)	 ( struct STREAM_SINK_VIDEO *sink );
typedef int (*SINK_VIDEO_RESIZE) ( struct STREAM_SINK_VIDEO *sink, VIDEO_PROPERTIES *video );
typedef int (*SINK_VIDEO_DUMP)   ( struct STREAM_SINK_VIDEO *sink );

typedef struct STREAM_SINK_VIDEO {
	const char	   *name;
	SINK_VIDEO_OPEN    open;
	SINK_VIDEO_CLOSE   close;
	SINK_VIDEO_DELETE  delete;
	SINK_VIDEO_PUT     put;
	SINK_VIDEO_GET     get;
	SINK_VIDEO_FLUSH   flush;
	SINK_VIDEO_END     end;
	SINK_VIDEO_SYNC    syncable;
	SINK_VIDEO_DELAY   delay;
	SINK_VIDEO_GETFRAME get_frame;
	SINK_VIDEO_GETTIME get_time;
	SINK_VIDEO_PUTTIME put_time;
	SINK_VIDEO_CLEAR   clear;
	SINK_VIDEO_RESIZE  resize;
	SINK_VIDEO_DUMP    dump;
	int                is_open;
	int		   allocates_frames;
	int                output;
	STREAM_SCREEN_PARAMS primary;
	STREAM_SCREEN_PARAMS secondary;
	void		   *ctx;
	void               *priv;
} STREAM_SINK_VIDEO;

#define STREAM_SINK_DEFAULT_SCREEN ((STREAM_SCREEN_PARAMS){ { 0, 0, 320, 240 }, 1, 1, 0, 1.0f, DISPFMT_ORIGINAL_PICTURE })

static inline int stream_sink_video_set_output( struct STREAM_SINK_VIDEO *sink, int output, STREAM_SCREEN_PARAMS *primary, STREAM_SCREEN_PARAMS *secondary ) 
{
	if( !sink ) 
		return 1;
	
	sink->output = output;
	
	if( primary ) {
		sink->primary = *primary;
	}
	if ( secondary ) {
		sink->secondary = *secondary;
	} 

	return 0;
}

static inline int stream_sink_video_allocates_frames( struct STREAM_SINK_VIDEO *sink )
{
	if( !sink || !sink->allocates_frames ) 
		return 0;
	return 1;
} 

#endif
