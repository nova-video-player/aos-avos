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
#include "stream.h"
#include "stream_alloc.h"
#include "stream_subtitle.h"
#include "debug.h"
#include "atime.h"
#include "util.h"

#include <string.h>
#include "sub_engine.h"
extern SUB_ENGINE *g_sub_engine;
// --- NATIVE ENGINE CLOCK ---
static int64_t g_player_time = 0;
static int64_t engine_clock(void *ctx) { return g_player_time; }

#define DBGS if(Debug[DBG_STREAM])
#define DBG  if(Debug[DBG_SUB])

#ifdef CONFIG_STREAM

// *****************************************************************************
//
//	stream_open_sub_dec
//
// *****************************************************************************
static int stream_open_sub_dec( STREAM *s )
{
	if( s->sub_dec) {
DBGS serprintf("stream_open_sub_dec\r\n");
		// open the decoder
		if( s->sub_dec->open( s->sub_dec, s->subtitle, s ) ) {
serprintf("error opening sub_dec!\r\n");
			s->sub_dec->destroy( s->sub_dec );
			s->sub_dec = NULL;
			s->cdata_sub.valid = 0;
			return 1;
		}
		return 0;
	} 
serprintf("no sub_dec found!\r\n");
	
	return 1;
}

// *****************************************************************************
//
//	stream_close_sub_dec
//
// *****************************************************************************
void stream_close_sub_dec( STREAM *s )
{
	if( s->sub_dec) {
		s->sub_dec->close( s->sub_dec );
		s->sub_dec->destroy( s->sub_dec );
		s->sub_dec = NULL;
		s->cdata_sub.valid = 0;
	}
}

// *****************************************************************************
//
//	stream_drop_subtitles
//
// *****************************************************************************
void stream_drop_subtitles( STREAM *s )
{
DBGS serprintf("stream_drop_subtitles\r\n");
	s->av.subs_max     = 0;
	s->subtitle->valid = 0;
	if( s->buffer )
		s->buffer->subtitle = 0;
	stream_parser_clear_subtitle_chunks( s );
	s->flags |= STREAM_NO_SUBTITLES;
}

void stream_buffer_fix_subs( STREAM_BUFFER *buffer );

// *****************************************************************************
//
//	alloc_sub_frame
//
// *****************************************************************************
static void alloc_sub_frame( STREAM *s )
{
	if( !s->subtitle_frame ) {
		int cs = AV_IMAGE_BGRA_32;
		if( s->subtitle->gfx ) {
			int w = MAX( 720, s->video->width  );
			int h = MAX( 576, s->video->height );
DBG serprintf("stream_subtitle: alloc_sub_frame: %dx%d\n", w, h);
			s->subtitle_frame = frame_alloc_with_cs_and_mem( w, h, cs, STREAM_MEM_NRM, 0);
		} else {
			// this is for text subs, so just allocate a large enuf buffer
			s->subtitle_frame = frame_alloc_with_cs_and_mem( 128, 8, cs, STREAM_MEM_NRM, 1);
		}
	}					 
}

static void _output_sub( STREAM *s, VIDEO_FRAME *f, uint64_t pos )
{
	// do we need to get a user time stamp from the parser?
	if( pos && s->parser->get_time_for_pos ) {
		int t = s->parser->get_time_for_pos( s, pos );
		if( t != -1 ) {
DBG serprintf("[diff %4d]  ", f->time - t );
			f->time = t;
		}
	}
	
	// we need to adjust the time the users sees for the delay:
	f->time += RST_TO_TS_DELTA(s->subtitle_offset, int);
	
	if( s->subtitle->gfx ) {
DBG serprintf("sub int GFX: video %8d  start %8d  dur %8d  [%dx%d]\r\n", s->video_time, f->time, f->duration, f->window.width, f->window.height );
	} else {
DBG serprintf("new int TXT: video %8d  start %8d  dur %8d  [%s]\r\n", s->video_time, f->time, f->duration, f->data );
	}
	// tell the user
	if( s->message_cb ) {
		s->message_cb( s, STREAM_SUBTITLE_CHANGED );
	}
}

// *****************************************************************************
//
//	_get_next_int_sub
//
// *****************************************************************************
static void _get_next_int_sub( STREAM *s, int time )
{
	g_player_time = time; // CAPTURE AVOS TIME!

	if( !s->seek ) {
		// CRITICAL FIX: Check subtitle_frame so this block ONLY RUNS ONCE!
		if( !s->sub_dec && !s->subtitle_frame ) {

			// --- NEW ROUTING LOGIC FOR INTERNAL SUBS ---
			if (s->subtitle->format == SUB_FORMAT_SSA || s->subtitle->format == SUB_FORMAT_TEXT) {
				if (g_sub_engine) {
					int engine_fmt = (s->subtitle->format == SUB_FORMAT_TEXT) ? SUB_FMT_SRT : SUB_FMT_SSA;
					sub_engine_open_track(g_sub_engine, engine_fmt, s->video ? s->video->width : 0, s->video ? s->video->height : 0, s->subtitle->extraData2, s->subtitle->extraDataSize2);
					sub_engine_start(g_sub_engine, engine_clock, NULL);
				}
			} else {
				// try to get a sub decoder
				s->sub_dec = stream_get_new_dec_sub( s->subtitle->format );
				if( stream_open_sub_dec( s ) ) {
					stream_drop_subtitles( s );
					return;
				}
			}

			// Allocates the frame so this initialization block NEVER runs again!
			alloc_sub_frame( s );
			
			if( !s->subtitle_frame ) {
serprintf("cannot allocate subtitle frame!\r\n");
				stream_close_sub_dec( s );
				// no subs, disable it
				stream_drop_subtitles( s );
				return;
			}
		}
		if( !s->cdata_sub.valid ) {
			if ( !s->parser->get_subtitle_cdata ) { 
				return;
			}
			if( s->parser->get_subtitle_cdata( s, &s->sub_buffer, &s->cdata_sub ) ) {
				// no subs, need to advance the sub last pos....
				stream_buffer_fix_subs( s->buffer );
				return;
			}
		}
		
		if( s->cdata_sub.valid ) {
			if( time == -1 ) return;

			if( s->cdata_sub.time == -1 || s->cdata_sub.time <= time ) {
				if (g_sub_engine) {
					int duration = 0;
					uint8_t *payload = s->sub_buffer.data;
					int payload_size = s->cdata_sub.size;

					if ((s->subtitle->format == SUB_FORMAT_SSA || s->subtitle->format == SUB_FORMAT_TEXT) && payload_size >= (int)sizeof(int)) {
						duration = *(int*)payload;
						payload += sizeof(int);
						payload_size -= sizeof(int);
					}
					sub_engine_feed(g_sub_engine, payload, payload_size, s->cdata_sub.time, duration);
				}

				if (s->subtitle->format == SUB_FORMAT_SSA || s->subtitle->format == SUB_FORMAT_TEXT) {
					s->cdata_sub.valid = 0;
				} else {
					VIDEO_FRAME *f = s->subtitle_frame;
					s->sub_dec->decode( s->sub_dec, s->sub_buffer.data, s->cdata_sub.size, s->cdata_sub.time, &f );
					s->cdata_sub.valid = 0;
					if( f ) _output_sub( s, f, s->cdata_sub.pos );
				}
			}
		}
	}
}

// *****************************************************************************
//
//	_get_next_ext_sub
//
// *****************************************************************************
static void _get_next_ext_sub( STREAM *s, int time )
{
	g_player_time = time; // CAPTURE AVOS TIME!

	if( !s->seek ) {

		// CRITICAL FIX: Only initialize ONCE per track!
		if( !s->sub_dec && !s->subtitle_frame ) {
			// --- NEW ROUTING LOGIC FOR EXTERNAL SUBS ---
			if (s->subtitle->format == SUB_FORMAT_SSA || s->subtitle->format == SUB_FORMAT_TEXT) {
				if (g_sub_engine) {
					int engine_fmt = (s->subtitle->format == SUB_FORMAT_TEXT) ? SUB_FMT_SRT : SUB_FMT_SSA;
					sub_engine_open_track(g_sub_engine, engine_fmt, s->video ? s->video->width : 0, s->video ? s->video->height : 0, s->subtitle->extraData2, s->subtitle->extraDataSize2);
					sub_engine_start(g_sub_engine, engine_clock, NULL);
				}
			} else if( s->subtitle->format == SUB_FORMAT_DVD_GFX ) {
				s->sub_dec = stream_get_new_dec_sub( s->subtitle->format );
				if( s->sub_dec && stream_open_sub_dec( s ) ) {
					stream_drop_subtitles( s );
					return;
				}
			}

			alloc_sub_frame( s );

			if( !s->subtitle_frame ) {
				serprintf("cannot allocate subtitle frame!\r\n");
				stream_close_sub_dec( s );
				stream_drop_subtitles( s );
				return;
			}
		}

		if( time == -1 ) return;
		if( s->sub_dec ) {
			VIDEO_FRAME f_;
			VIDEO_FRAME *f = &f_;
			UCHAR data[SUBTITLE_CHUNK];
			f->data[0] = data;
			f->size = sizeof( data );

			if( stream_sub_ext_get_subtitle_data( s, &f, time ) ) return;

			if( f && f->valid ) {
				if (g_sub_engine) {
					sub_engine_feed(g_sub_engine, f->data[0], f->valid, f->time, f->duration);
				}
				if (s->subtitle->format == SUB_FORMAT_SSA || s->subtitle->format == SUB_FORMAT_TEXT) {
					// Bypass old renderer
				} else {
					VIDEO_FRAME *f2 = s->subtitle_frame;
					s->sub_dec->decode( s->sub_dec, f->data[0], f->valid, f->time, &f2 );
					if( f2 ) {
						f2->time = f->time;
						_output_sub( s, f2, 0 );
					}
				}
			}
		} else {
			VIDEO_FRAME *f = s->subtitle_frame;
			if( stream_sub_ext_get_subtitle_data( s, &f, time ) ) return;

			if( f ) {
				if (g_sub_engine && f->data[0] && (s->subtitle->format == SUB_FORMAT_SSA || s->subtitle->format == SUB_FORMAT_TEXT)) {
					int text_len = strlen((char*)f->data[0]);
					sub_engine_feed(g_sub_engine, f->data[0], text_len, f->time, f->duration);
				}

				if (s->subtitle->format == SUB_FORMAT_SSA || s->subtitle->format == SUB_FORMAT_TEXT) {
					// Bypass old renderer
				} else {
					_output_sub( s, f, 0 );
				}
			}
		}
	}
}

// ************************************************************
//
//	_sub_decode
//
// ************************************************************
void _sub_decode( STREAM *s )
{
	if( s->subtitle_changed ) {
		s->subtitle_changed = 0;
		// tell the user
		if( s->message_cb ) {
			s->message_cb( s, STREAM_SUB_PROPS_CHANGED );
		}
	}
	if( s->subtitle->valid && !s->paused ) {
		int time = s->video_time;
		if( time != -1 ) {
			// apply correction
			time -= s->subtitle_offset;
			if( time < 0 )
				time = 0;
		}
		if( s->subtitle->ext ) {
			_get_next_ext_sub( s, time );
		} else {
			_get_next_int_sub( s, time );
		}
	}
}

// ************************************************************
//
//	stream_sub_dec_thread
//
// ************************************************************
void *stream_sub_dec_thread( void *data )
{
	STREAM *s = (STREAM *)data;
DBGS serprintf("PID[%5d] stream_sub_dec_thread::Starting\r\n", getpid() );	
	
	while( thread_state_get( &s->sub_tstate ) != THREAD_EXIT ) {
		thread_state_ack( &s->sub_tstate );
		if( thread_state_get( &s->sub_tstate ) == THREAD_RUNNING ) {
			_sub_decode( s );
		}
		stream_yield_RT();
	}
DBGS serprintf("PID[%5d] stream_sub_dec_thread::Exiting\r\n", getpid() );	
 	return NULL;
}

// *****************************************************************************
//
//	stream_check_subtitles
//
// *****************************************************************************
int stream_check_subtitles( STREAM *s )
{
	if( !s->open ) {
serprintf("ScS: not open!\r\n");
		return 1;
	}

	// We don't want to pause / unpause if there is no new subtitles, so check it first.
	if( !stream_sub_ext_has_new( s ) ) {
DBGS serprintf("stream_check_subtitles, no new ext subtitles\r\n");
		return 0;
	} else {
DBGS serprintf("stream_check_subtitles, has new ext subtitles\r\n");
	}

	char prev_extsub[MAX_NAME_LEN + 1];
	int has_prev_extsub = 0;
	int prev_sub = s->av.subs;
	if (s->subtitle && s->subtitle->valid && s->subtitle->ext) {
		strncpy(prev_extsub, s->subtitle->path, MAX_NAME_LEN);
		has_prev_extsub = 1;
	}
	int was_paused = stream_pause( s );

	// idle threads to make sure they are at a known state
	thread_state_set( &s->engine_tstate, THREAD_IDLE );
	thread_state_set( &s->sub_tstate,    THREAD_IDLE );

	// close old subtitle decoder
	stream_close_sub_dec( s );

	// free subtitle frame
	frame_free( s->subtitle_frame );
	s->subtitle_frame = NULL;

	stream_sub_ext_close( s );

	stream_sub_ext_check( s );

	if (has_prev_extsub) {
		int i;

		prev_sub = 0;
		for (i = 0; i < s->av.subs_max; ++i) {
			if (strcmp(s->av.sub[i].path, prev_extsub) == 0) {
				prev_sub = i;
				break;
			}
		}
	}
	s->av.subs = prev_sub;
	if (s->av.subs >= s->av.subs_max)
		s->av.subs = 0;
	s->subtitle = s->av.sub + s->av.subs;

	// notify the app too
	s->subtitle_changed = 1;

	// run threads again
	thread_state_set( &s->engine_tstate, THREAD_RUNNING );
	thread_state_set( &s->sub_tstate,    THREAD_RUNNING );

	stream_un_pause( s, was_paused );
	return 0;
}

// *****************************************************************************
//
//	stream_set_subtitle_stream
//
// *****************************************************************************
int stream_set_subtitle_stream( STREAM *s, int sub_stream )
{	
serprintf("stream_set_subtitle_stream( %d )\r\n", sub_stream );
 
	if( !s->open ) {
serprintf("SsS: not open!\r\n");
		return 1;
	}
	
	if( !s->subtitle->valid ) {
serprintf("SsS: not sub!\r\n");
		return 1;
	}

	if( sub_stream >= s->av.subs_max ) {
serprintf("SsS: sub_stream > av.subs_max\n");	
		return 1;
	}
	if( sub_stream == s->av.subs ) {
serprintf("SsS: sub_stream already set\n");	
//		return 0;
	}

	int was_paused = stream_pause( s );

	// idle threads to make sure they are at a known state
	thread_state_set( &s->engine_tstate, THREAD_IDLE );
	thread_state_set( &s->sub_tstate,    THREAD_IDLE );
	
	// close old subtitle decoder
	stream_close_sub_dec( s );

	// free subtitle frame
	frame_free( s->subtitle_frame );
	s->subtitle_frame = NULL;

	s->av.subs  = sub_stream;
	s->subtitle = s->av.sub + s->av.subs;
	
	// run threads again
	thread_state_set( &s->engine_tstate, THREAD_RUNNING );
	thread_state_set( &s->sub_tstate,    THREAD_RUNNING );

	stream_un_pause( s, was_paused );

	// FIXME: there should be a better way without seek jumping
	int current_time = stream_get_current_time( s, NULL );
	if( current_time > 0 && thread_state_get( &s->parser_tstate ) != THREAD_EXIT && s->parser->seekable && s->parser->seekable( s ) ) {
		// reseek to current time to get internal subtitle decoder to reinitialize
		stream_seek_time( s, current_time - 1, STREAM_SEEK_BACKWARD, 0 );
	}
	return 0;
}

// *****************************************************************************
//
//	stream_get_current_subtitle
//
// *****************************************************************************
VIDEO_FRAME *stream_get_current_subtitle( STREAM *s )
{
	return s ? s->subtitle_frame : NULL;
}

// *****************************************************************************
//
//	stream_set_subtitle_offset
//
// *****************************************************************************
void stream_set_subtitle_offset( STREAM *s, int offset )
{
	if( s ) { 
		s->subtitle_offset = offset;
	}
}

// *****************************************************************************
//
//	stream_set_subtitle_ratio
//
// *****************************************************************************
void stream_set_subtitle_ratio( STREAM *s, int n, int d )
{
	if( s ) {
		s->subtitle_ratio_n = n;
		s->subtitle_ratio_d = d;
	}
}

// ************************************************************
//
//	stream_set_subtitle_url
//
// ************************************************************
void stream_set_subtitle_url( STREAM *s, const char **url_list )
{
DBGS serprintf("stream_set_subtitle_url\n");		
	if( s && url_list ) {
		int i;
		for( i = 0; i < SUB_TRACK_MAX && url_list[i]; i++ ) {
DBGS serprintf("sub_url: %s\n", url_list[i] );
			if ( s->sub_url[i + 1] != NULL )
				afree( s->sub_url[i + 1] );
			s->sub_url[i + 1] = astrdup( url_list[i] );		
		}
	}
}
#endif
