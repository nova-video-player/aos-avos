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
#include "stream.h"
#include "file.h"
#include "audio_interface.h"

#ifdef CONFIG_STREAM

#define DBGS 	if(Debug[DBG_STREAM])
#define DBGA2	if(Debug[DBG_AUD] > 1)

static int _open( STREAM *s )
{
	serprintf("stream_sink_audio_open\n");
	s->audio_ctx = audio_interface_open(AUDIO_OUTPUT_MODE);
	return s->audio_ctx == NULL;
}

static int _close( STREAM *s )
{
	serprintf("stream_sink_audio_close\n");
	if( s->audio_ctx ) {
		return audio_interface_close( &s->audio_ctx );
	} else {
		return 1;
	}
}	

static int start( STREAM *s )
{
	if( audio_interface_set_output_params( s->audio_ctx, s->audio->samplesPerSec, s->audio->channels, s->audio->bitsPerSample, s->audio->format ) ) {
serprintf("stream_sink_audio_start: cannot set params: fs %d  ch %d  bits %d\r\n", s->audio->samplesPerSec, s->audio->channels, s->audio->bitsPerSample );
		return 1;
	}

	audio_interface_start( s->audio_ctx);

	s->audio_sink_open = 1;
	s->audio_sink->set_vol(s);

	return 0;
}

static int stop( STREAM *s )
{
DBGS serprintf("stream_sink_audio_stop\r\n");

	if( !s->audio_sink_open ) {
serprintf("ssa not open!\r\n");
		return 1;
	}
	audio_interface_stop( s->audio_ctx );
	s->audio_sink_open = 0;
	return 0;
}

static int flush( STREAM *s )
{
DBGS serprintf("stream_sink_audio_flush\r\n");

	audio_interface_flush_output( s->audio_ctx );

	return 0;
}

static int end( STREAM *s )
{
	return 0;
}

static int syncable( STREAM *s )
{
	return 1;
}

static int _write( STREAM *s, AUDIO_FRAME *frame )
{
DBGA2 serprintf("\r\n[%8d] size %5d  ", atime(), frame->size );

	return audio_interface_write( s->audio_ctx, frame->data, frame->size );
}

static int preload( STREAM *s )
{
	// NOTE: Not sure we actually need to do this
	// The audiodevice preloading is done by the audiomixer
	// and it probably doesn't help to calculate the latency
	return audio_interface_preload( s->audio_ctx );
}

static int _delay( STREAM *s )
{
	return audio_interface_get_delay( s->audio_ctx );
}

static int can_write( STREAM *s, int len )
{
	return audio_interface_can_write( s->audio_ctx, len );
}

static int set_passthrough( STREAM *s, int pass )
{
	return audio_interface_set_passthrough(s->audio_ctx, pass);
}

static int get_passthrough( STREAM *s )
{
	return audio_interface_get_passthrough(s->audio_ctx);
}

static int mute( STREAM *s, int mute )
{
	if( mute ) {
		audio_interface_mute( s->audio_ctx, TRUE );
	} else {
		audio_interface_unmute( s->audio_ctx, TRUE, FALSE );
	}
	return 0;
}

static int set_vol( STREAM *s )
{
	if (s->audio_sink_open && s->vol_l != -1 && s->vol_r != -1)
		audio_interface_set_output_volume_l_r( s->audio_ctx, s->vol_l, s->vol_r );
	return 0;
}

static int get_session_id( STREAM *s )
{
	return audio_interface_get_session_id( s->audio_ctx );
}

STREAM_SINK_AUDIO stream_sink_audio =
{
	"audio",
	_open,
	_close,
	start,
	stop,
	_write,
	preload,
	flush,
	end,
	syncable,
	_delay,
	can_write,
	set_passthrough,
	get_passthrough,
	mute,
	set_vol,
	get_session_id,
};
#endif
