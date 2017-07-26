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


#ifdef CONFIG_STREAM

#define DBGS	if(Debug[DBG_STREAM])
#define DBGV   	if(Debug[DBG_VID])
#define DBGVY	if(Debug[DBG_VID]||Debug[DBG_SYNC])
#define DBGY	if(Debug[DBG_SYNC])
#define DBGV1  	if(Debug[DBG_VID] == 1)
#define DBGV2  	if(Debug[DBG_VID] > 1)
#define DBGV3 	if(Debug[DBG_VID] > 2)

extern int stream_max_delay;
extern int stream_no_sync;
extern int stream_video_paused;
extern int stream_audio_paused;
extern int stream_bdrop_threshold;
extern int stream_pdrop_threshold;

static volatile int	stream_dbg_delay = 0;

// ************************************************************
//
//	stream_sync_restart
//
// ************************************************************
int stream_sync_restart( STREAM *s )
{
	s->delay         = 0;
	s->delay_valid   = 0;
	s->drop          = 0;
	s->drop_P        = 0;
	s->drop_B        = 0;

	return 0;
}

// ************************************************************
//
//	stream_sync_init
//
// ************************************************************
int stream_sync_init( STREAM *s, int time )
{
	s->video_time     = -1;
	s->audio_time     = -1;
	s->audio_ref_time = -1;

	if( s->video->valid ) {
		s->video_time = time;
	} else if( s->sync_mode != STREAM_SYNC_SAMPLES ) {
		s->audio_time = time;
	}

	if( s->video->valid && s->audio->valid && !s->slideshow ) {
DBGS serprintf("sync_init\r\n");
 		s->sync_audio   =  1;
		s->sync_video   =  1;
		s->sync_v_time  = -1;
		s->sync_a_time  = -1;
	}

	stream_sync_restart( s );

	return 0;
}

// *****************************************************************************
//
//	_stream_sync_av_delay
//
// *****************************************************************************
int stream_sync_av_delay( STREAM *s )
{
	if ( !s->audio->valid || !s->video->valid ) {
		// if we have no audio & video
		return 0;
	} 
	
	// audio data passes through decoder and sink
	int codec_delay  = s->audio_dec    ? s->audio_dec->delay( s->audio ) : 0;
	int sink_delay   = s->audio_sink   ? s->audio_sink->delay( s ) : 0;
		
	int video_delay;
	if( s->vtime_post_sink ) {
		// we sample after the sink, so the time stamps are the one we get out of the sink
		video_delay = 0;
	} else {
		// we sample before the sink, so the video frames have to pass through the sink
	  	video_delay = ( s->video_sink && s->video_sink->delay )  ? s->video_sink->delay( s->video_sink ) : 0;
 	}
	
	if( s->sync_mode == STREAM_SYNC_SAMPLES ) {
		return /*codec_delay +*/ sink_delay - video_delay;
	} else {
		return codec_delay + sink_delay - video_delay;
	}
}

// ************************************************************
//
//	_stream_av_diff
//
// ************************************************************
static int _stream_av_diff( STREAM *s, int video_time, int audio_time )
{
	return video_time - audio_time + (stream_sync_av_delay( s ) + s->av_delay + stream_dbg_delay);
}

// ************************************************************
//
//	stream_sync_audio
//
// ************************************************************
int stream_sync_audio( STREAM *s, int audio_time )
{
	if( s->video_sink && s->video_sink->put_time && audio_time != -1 ) {
		if( !stream_no_sync || s->sync_a_time == -1 ) { 
			s->video_sink->put_time( s->video_sink, audio_time - (stream_sync_av_delay(s) +  s->av_delay + stream_dbg_delay) );
		}
	}

	s->sync_a_time = audio_time;
	
	if( !s->sync_audio || s->play_n_audio_frames || stream_no_sync ) {
		return 0;
	}
	
	// let data without timestamp pass!
	if( audio_time == -1 ) {
		return 0;
	}

	// video is already at end, let data pass!
	if( s->video_end ) {
		return 0;
	}

	// video is paused, ignore
	if( stream_video_paused ) {
		return 0;
	}

DBGY serprintf("{SSA %d}} ", audio_time );
	// both audio and video need to have a valid timestamp before we can start
	if( s->sync_v_time == -1 || s->sync_a_time == -1 )
		return 1;
	
	// if audio is in the future, delay it
	int diff = _stream_av_diff( s, s->sync_v_time, s->sync_a_time );
	if ( diff < 0 ) {
DBGY serprintf("{{A %d}} ", diff );
		s->sync_video = 0;
		return 1;
	}
	
	// allow audio to play from now on
	s->sync_audio = 0;
	
	return 0;
}

// ************************************************************
//
//	stream_sync_video
//
// ************************************************************
int stream_sync_video( STREAM *s, int video_time )
{
	s->sync_v_time = video_time;

	if( !s->sync_video || s->speed != STREAM_SPEED_NORMAL || s->play_n_video_frames || stream_no_sync ) {
		return 0;
	}
	// let data without timestamp pass!
	if( video_time == -1 ) {
		return 0;
	}
	
	// audio is already at end, let data pass!
	if( s->audio_end ) {
		return 0;
	}
		
	// audio is paused, ignore
	if( stream_audio_paused ) {
		return 0;
	}
	
DBGY serprintf("{SSV %d}} ", video_time );
	// both audio and video need to have a valid timestamp before we can start
	if( s->sync_v_time == -1 || s->sync_a_time == -1 )
		return 1;

	// if video is in the future, delay it
	int diff = _stream_av_diff( s, s->sync_v_time, s->sync_a_time );
	// if we sample post sink, allow us to start 500ms early
	int max = s->vtime_post_sink ? 500 : 0;
	if ( diff > max ) {
DBGY serprintf("{{V %d}} ", diff );
		s->sync_audio = 0;
		return 1;
	}
	
	// allow video to play from now on
	s->sync_video = 0;
	
	return 0;
}

// ************************************************************
//
//	stream_sync
//
// ************************************************************
void stream_sync( STREAM *s )
{
	// if we have audio ...
	if ( !s->audio->valid || !s->video->valid )
		goto EXIT;
		
	if ( s->speed )
		goto EXIT;

	if ( s->slideshow )
		goto EXIT;
	
	// sync machine still working?
	if( (s->sync_audio || s->sync_video) && !stream_no_sync ) 
		goto EXIT;

	// audio and video time known?
	if ( s->audio_time <= 0 || s->video_time <= 0 )
		goto EXIT;
	
	// audio done?
	if ( s->audio_end )
		goto EXIT;
	
	// ... we calc the delay between audio and video frames and
	// try adjust it to zero
	int rdiff = _stream_av_diff( s, s->video_time, s->audio_time );
	int diff  = MAX( MIN( rdiff,  250 ), -250 );
	
 	if( !s->delay_valid ) {
//serprintf("(D %d)", diff );	
		s->delay = diff;
	} else {
		s->delay = (s->delay * s->delay_fb + diff * (1000 - s->delay_fb)) / 1000;
	}
	s->delay_valid = 1;
	
DBGVY serprintf("(%3d|%3d|%3d)", rdiff, diff, s->delay );
		
	if ( stream_no_sync || s->video_sink->put_time )
		goto EXIT;
		 
	s->drop_B = 0;

	if ( s->delay > stream_max_delay * s->video->msPerFrame ) {
		// video is too fast, we have to slow down
		s->drop = -1;
		s->delay -= stream_max_delay * s->video->msPerFrame;
DBGVY serprintf("_S(%3d)_", s->delay );
	} else if ( s->delay < (-1 * stream_max_delay * s->video->msPerFrame)  ) {
		
		if ( stream_pdrop_threshold && rdiff < (-1 * stream_pdrop_threshold) ) {
			// we are totally late, see if we can skip to next key frame
			int max_time = s->video_time - rdiff + 500;
			int key_time;
			int num;
			if( (num = stream_parser_find_key_frame( s, max_time, &key_time )) ) {
				int dropped = stream_parser_drop_video( s, key_time );
DBGVY serprintf("XX(%d %d %d) ", num, key_time, dropped );
				// set drop_P to 1 which means drop all you have and restart
				s->drop_P = 1;
				return;
			}
		}

		// video is late, we have to hurry up
		if( stream_bdrop_threshold && s->delay < (-1 * stream_bdrop_threshold  * s->video->msPerFrame) ) {
			s->drop_B = 1;
		}
		s->drop = 1;
		s->delay += stream_max_delay * s->video->msPerFrame;
DBGVY serprintf("_%s(%3d)_", s->drop_B ? "B" : "F", s->delay );
	} else {
DBGVY serprintf("  (   ) " );
	}
	return;
EXIT:
DBGV serprintf("  (---) " );
}

#ifdef DEBUG_MSG
void *AV_get_ctx( void );

static void _stream_delay_plus( int argc, char *argv[] )
{
	if( argc > 1 ) {
		stream_dbg_delay += atoi( argv[1] );
	} else {
		stream_dbg_delay += 20;
	}
	
	STREAM *s = AV_get_ctx();
	if( s ) {
serprintf("av_delay  %5d\n", stream_sync_av_delay( s ) );
	}
serprintf("dbg_delay %5d\n", stream_dbg_delay );
}

static void _stream_delay_minus( int argc, char *argv[] )
{
	if( argc > 1 ) {
		stream_dbg_delay -= atoi( argv[1] );
	} else {
		stream_dbg_delay -= 20;
	}
	STREAM *s = AV_get_ctx();
	if( s ) {
serprintf("av_delay  %5d\n", stream_sync_av_delay( s ) );
	}
serprintf("dbg_delay %5d\n", stream_dbg_delay );
}

static void _stream_delay_set( int argc, char *argv[] )
{
	if( argc > 1 ) {
		stream_dbg_delay = atoi( argv[1] );
	}
	STREAM *s = AV_get_ctx();
	if( s ) {
serprintf("av_delay  %5d\n", stream_sync_av_delay( s ) );
	}
serprintf("dbg_delay %5d\n", stream_dbg_delay );
}

DECLARE_DEBUG_COMMAND("sep", 	_stream_delay_plus   );
DECLARE_DEBUG_COMMAND("sem", 	_stream_delay_minus  );
DECLARE_DEBUG_COMMAND("ses", 	_stream_delay_set    );
#endif

#endif


