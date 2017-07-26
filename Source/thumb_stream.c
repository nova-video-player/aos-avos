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
#include "thumb_stream.h"
#include "codec_utils.h"
#include "stream_alloc.h"

#include <errno.h>
#include <string.h>
#include <stdio.h>

#define ERR  if(1)
#define DBG  if(Debug[DBG_THUMB])
#define DBG2 if(Debug[DBG_THUMB] > 1)

#define THUMB_TIME (200 * 1000)

#ifdef CONFIG_VIDEO

struct thumb_stream_t {
	STREAM *s;
	int colorspace;
	int num_frames;
	VIDEO_FRAME *frames[STREAM_MAX_FRAMES];
	int frames_get[STREAM_MAX_FRAMES];
};

static int _open( STREAM_SINK_VIDEO *sink, VIDEO_PROPERTIES *video, void *ctx, int num_frames, STREAM_RC *rc )
{
	int i;
	thumb_stream_t *p = sink->priv;

	video->colorspace = p->colorspace;

	if (stream_alloc_frames(&p->frames, video->width, video->height, video->colorspace, STREAM_MEM_NRM, &num_frames) != 0) {
		return 1;
	}
	p->num_frames = num_frames;

	for( i = 0; i < p->num_frames; i++ ) {
		p->frames[i] = frame_alloc_with_cs_and_mem( video->width, video->height, video->colorspace, STREAM_MEM_NRM );
		if( !p->frames[i] ) {
serprintf("cannot alloc frame %d\r\n", i);
			return 1;
		}
		p->frames[i]->index   = i;
		p->frames[i]->user_ID = i;
		p->frames[i]->locked  = 0;
		p->frames_get[i] = 0;
	}
	sink->is_open = 1;
        return 0;
}

static int _close( STREAM_SINK_VIDEO *sink )
{
	thumb_stream_t *p = sink->priv;

	stream_free_frames(&(p->frames), p->num_frames);
        return 0;
}

static int _dummy( STREAM_SINK_VIDEO *sink )
{
        return 0;
}

static int _flush( STREAM_SINK_VIDEO *sink )
{
	thumb_stream_t *p = sink->priv;
	int i;

	for (i = 0; i < p->num_frames; ++i) {
		p->frames_get[i] = 0;
	}
        return 0;
}

static int _put( STREAM_SINK_VIDEO *sink, VIDEO_FRAME *frame )
{
	thumb_stream_t *p = sink->priv;

	p->frames_get[frame->index] = 0;

        return frame->blit_time;
}

static int _get( STREAM_SINK_VIDEO *sink, VIDEO_FRAME **pframe  )
{
	int i;
	thumb_stream_t *p = sink->priv;

	for (i = 0; i < p->num_frames; ++i) {
		if (p->frames_get[i] == 0) {
			*pframe = p->frames[i];
			p->frames_get[i] = 1;
			return 0;
		}
	}
	*pframe = NULL;
        return 1;
}

static VIDEO_FRAME *_get_frame( STREAM_SINK_VIDEO *sink, int index )
{
	if (!sink || !sink->is_open)
		return NULL;

	thumb_stream_t *p = sink->priv;

	return index < p->num_frames ? p->frames[index] : NULL;
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
static STREAM_SINK_VIDEO *_thumb_sink_new( thumb_stream_t *priv )
{
        STREAM_SINK_VIDEO *sink = acalloc(1, sizeof(STREAM_SINK_VIDEO));
        if( !sink )
                return NULL;

	sink->priv = priv;

        sink->name      = "thumb";
        sink->open      = _open;
        sink->close     = _close;
	sink->delete    = _delete;
        sink->put       = _put;
        sink->get       = _get;
	sink->get_frame = _get_frame;
        sink->flush     = _flush;
        sink->syncable  = _dummy;
        sink->get_time  = _dummy;
	sink->allocates_frames = 1;

        return sink;
}

thumb_stream_t *thumb_stream_create()
{
	return (thumb_stream_t *)acalloc(1, sizeof(thumb_stream_t));
}

IMAGE* thumb_stream_get_frame(thumb_stream_t *thumb_stream, STREAM_URL *src, int etype, int thumb_time, int colorspace, int *rotation)
{
        STREAM *stream;
        STREAM_SINK_VIDEO *sink;

	if (thumb_stream->s) {
		stream_stop(thumb_stream->s);
		stream_delete(&thumb_stream->s);
		thumb_stream->s = NULL;
	}
        if (!(stream = stream_new())) {
		ERR serprintf("%s : cannot create stream\r\n", __FUNCTION__);
		return NULL;
        }
	thumb_stream->s = stream;
	thumb_stream->colorspace = colorspace;

	sink = _thumb_sink_new(thumb_stream);
	if (!sink) {
		return NULL;
	}

	stream_set_max_video_dimensions(stream, VIDEO_MAX_WIDTH, VIDEO_MAX_HEIGHT);
        stream_set_buffer_size(stream, 12);   // use 12 MB for thumb creation 
	stream_set_video_sink(stream, sink);

        if ( stream_open( stream, src, etype, STREAM_MULTI | STREAM_THUMB ) ) {
		serprintf("thumb: ve %d\r\n", stream->video_error );
		return NULL;
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
		return NULL;
        }

	if (rotation)
		*rotation = stream->video->rotation;

	return (IMAGE *) stream->current_frame;
}

void thumb_stream_destroy(thumb_stream_t *thumb_stream)
{
	if (thumb_stream->s) {
		stream_stop(thumb_stream->s);
		stream_delete(&thumb_stream->s);
	}
	afree(thumb_stream);
}

#endif	// CONFIG_VIDEO
