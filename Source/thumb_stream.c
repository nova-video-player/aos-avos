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
	int (*abort)(void *);
	void *abort_opaque;
	int num_frames;
	VIDEO_FRAME *frames[STREAM_MAX_FRAMES];
	int frames_get[STREAM_MAX_FRAMES];
};

static int _open(STREAM_SINK_VIDEO *sink, VIDEO_PROPERTIES *video, void *ctx, int num_frames, STREAM_RC *rc)
{
	thumb_stream_t *p = sink->priv;
	if (num_frames <= 0 || num_frames > STREAM_MAX_FRAMES ||
	    video->width <= 0 || video->width > VIDEO_MAX_WIDTH ||
	    video->height <= 0 || video->height > VIDEO_MAX_HEIGHT) return 1;
	video->colorspace = p->colorspace;
	int wanted = num_frames;
	stream_alloc_frames(&p->frames, video->width, video->height, video->colorspace,
		STREAM_MEM_NRM, &num_frames);
	if (num_frames != wanted) {
		stream_free_frames(&p->frames, num_frames);
		memset(p->frames, 0, sizeof(p->frames));
		p->num_frames = 0;
		return 1;
	}
	p->num_frames = num_frames;
	for (int i = 0; i < p->num_frames; i++) {
		p->frames[i]->user_ID = i;
		p->frames_get[i] = 0;
	}
	sink->is_open = 1;
	return 0;
}

static int _close( STREAM_SINK_VIDEO *sink )
{
	thumb_stream_t *p = sink->priv;

	stream_free_frames(&(p->frames), p->num_frames);
	memset(p->frames, 0, sizeof(p->frames));
	p->num_frames = 0;
	sink->is_open = 0;
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

	if (!frame || frame->index < 0 || frame->index >= p->num_frames) return 1;
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

	return index >= 0 && index < p->num_frames ? p->frames[index] : NULL;
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

static int thumb_aborted(void *opaque)
{
	STREAM *s = opaque;
	thumb_stream_t *p = stream_get_user_ctx(s);
	return p->abort && p->abort(p->abort_opaque);
}

static void thumb_stream_clear(thumb_stream_t *p)
{
	if (!p->s) return;
	if (p->s->open) stream_stop(p->s);
	// Failure before stream_init/open never reaches stream_stop's sink cleanup.
	if (p->s->video_sink) {
		if (p->s->video_sink->is_open) _close(p->s->video_sink);
		_delete(p->s->video_sink);
		p->s->video_sink = NULL;
	}
	stream_delete(&p->s);
}

IMAGE* thumb_stream_get_frame(thumb_stream_t *p, STREAM_URL *src, int etype,
	int thumb_time, int colorspace, int *rotation, int (*abort)(void *), void *opaque)
{
	int old_identity = timeline_set_local_identity(1);
	IMAGE *image = NULL;
	thumb_stream_clear(p);
	p->abort = abort;
	p->abort_opaque = opaque;
	if (abort && abort(opaque)) goto out;
	STREAM *s = p->s = stream_new();
	if (!s) goto out;
	p->colorspace = colorspace;
	STREAM_SINK_VIDEO *sink = _thumb_sink_new(p);
	if (!sink) goto out;
	stream_set_video_sink(s, sink);
	stream_set_user_ctx(s, p);
	stream_set_abort_handler(s, thumb_aborted);
	stream_set_max_video_dimensions(s, VIDEO_MAX_WIDTH, VIDEO_MAX_HEIGHT);
	stream_set_buffer_size(s, 12);
	if (stream_open(s, src, etype, STREAM_MULTI | STREAM_THUMB)) goto out;
	int duration;
	stream_get_current_time(s, &duration);
	int start;
	if (!duration) {
		int total;
		stream_get_current_pos(s, &total);
		start = total / 2;
	} else {
		start = thumb_time >= 0 && thumb_time <= duration ? thumb_time : MIN(THUMB_TIME, duration / 2);
	}
	stream_set_start_time(s, start);
	if (stream_start(s) || (abort && abort(opaque)) || s->video_error) goto out;
	if (rotation) *rotation = s->video->rotation;
	image = (IMAGE *)s->current_frame;
out:
	if (!image) thumb_stream_clear(p);
	timeline_set_local_identity(old_identity);
	return image;
}

void thumb_stream_destroy(thumb_stream_t *p)
{
	if (!p) return;
	int old_identity = timeline_set_local_identity(1);
	thumb_stream_clear(p);
	timeline_set_local_identity(old_identity);
	afree(p);
}

#endif	// CONFIG_VIDEO
