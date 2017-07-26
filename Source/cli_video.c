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
#include "audio.h"
#include "app_av.h"
#include "image.h"
#include "app_video.h"
#include "threadcom.h"
#include "file_type.h"
#include "fs.h"
#include "fb.h"
#include "app_key.h"

#define DBG  if(Debug[DBG_AUD] || Debug[DBG_VID])
#define DBGA if(Debug[DBG_AUD])
#define DBGV if(Debug[DBG_VID])
//
// COMMON
//

static threadcom_link_t *comlink = NULL;
static fpx 		notify_cb;
static void 		*notify_ctx;

static void link_callback(threadcom_link_t* comlink)
{
        int dummy;
	threadcom_get_event(comlink, (char*)&dummy, sizeof(int));
	
DBG serprintf("link_callback: DONE\r\n");
	AV_stop();
	if( notify_cb )
		notify_cb( notify_ctx ); 
}

static void _setup_notify( fpx _notify_cb, void *_notify_ctx )
{
	comlink = threadcom_create( link_callback, &mainloop_events );
	notify_cb  = _notify_cb;
	notify_ctx = _notify_ctx;
}

static void _do_notify( void )
{
	if( comlink ) {
		int dummy = 0;
		threadcom_post_event( comlink, (char*)&dummy, sizeof( int ) );
	}
}

static void _stopped( void *ctx )
{
DBG serprintf("cli: stopped!\r\n");
}

static STREAM *v = NULL;
static const char *url;
static int etype;

static void _video_stop( void *ctx, int resume )
{
	STREAM *s = ctx;
	stream_stop( s );
	stream_delete( &s );
	AV_set_state( AV_STOPPED, 0, 0, NULL, NULL );
	threadcom_destroy(comlink);
	comlink = NULL;
}

static int  _video_pause( void *ctx )                        { return stream_pause( (STREAM*)ctx ); }
static void _video_un_pause( void *ctx, int was_paused )     { return stream_un_pause( (STREAM*)ctx, was_paused ); }
static int  _video_is_paused( void *ctx )                    { return stream_is_paused( (STREAM*)ctx ); }
static int  _video_get_current_time( void *ctx, int *total ) { return stream_get_current_time( (STREAM*)ctx, total ); }

static AV_CALLBACKS _video_cbs  = {
	_video_stop,
	_video_pause,
	_video_un_pause,
	_video_is_paused,
	_video_get_current_time,
};

static void stop_handler( STREAM *s )
{
serprintf("video stop handler called\r\n");
	_do_notify();
}

void cli_play_video( const char *_url, int _etype )
{
	etype = _etype;
	url   = _url;
}

static int video_start_paused = 0;

static void *_start_video( void )
{
	// make sure others are stopped
	AV_stop();
	
	if( !(v = stream_new()) ) {
DBGV serprintf("cannot create stream\r\n");
		return NULL;
	}
	
	int _flags = STREAM_PAUSED;
	if( !is_local_file( url ) ) {
DBGV serprintf("NONLOCAL!\r\n");	
		_flags |= STREAM_FILE_NONLOCAL;
	}

//	_flags |= i_flags;
	
//	stream_set_buffer_size( v, 8 );	// use only 8 MB 

	int type = TYPE_VID;
	if( !etype ) {
		get_file_type( url, &type, &etype );
	}
DBGV serprintf("path: %s\n", url);
DBGV serprintf("type: %d\n", type);
DBGV serprintf("etype: %d\n", etype);

	stream_set_max_video_dimensions( v, VIDEO_MAX_WIDTH, VIDEO_MAX_HEIGHT );
	stream_set_stop_handler( v, stop_handler );
	stream_set_volume( v, -1, -1);

	STREAM_SCREEN_PARAMS screen = {
		{ 0, 0, FB_get_video_width(), FB_get_video_height() },
		1, 1, 0, 1.0f, DISPFMT_ORIGINAL_PICTURE,
	};
	
	STREAM_SINK_VIDEO *video_sink = stream_get_default_video_sink(v);
	stream_sink_video_set_output( video_sink, STREAM_OUTPUT_PRIMARY, &screen, NULL ); 
	stream_set_video_sink( v, video_sink ); 

	STREAM_URL src;
	stream_url_cpy_url( &src, url );
	if( stream_open( v, &src, etype, _flags ) ) {
		stream_delete( &v );
		return NULL;
	}

	if( stream_start( v ) ) {
		stream_delete( &v );
		return NULL;
	}
	
	AV_set_state( AV_PLAYING, type, DEV_HDD, v, &_video_cbs );
	_setup_notify( _stopped, NULL );

	// Erase background ( only needed on SIM )
	IMAGE fb = FB_VIDEO_IMAGE;
	// clear all buffer prevent green background surround in tv mode
	image_full_window( &fb );
	image_fill_window( &fb, YUV_BLACK );

	FB_set_video_active(1);

	if( !video_start_paused ) {
		stream_un_pause( v, 0 );
	}
	
	return v;
}

void cli_handle_key( int key ) 
{
serprintf("key: %2d\n", key );
	switch( key ) {
	case KEY_OK:
		if( AV_get_state() != AV_PLAYING ) {
			if( url ) {
				_start_video();
			}
		} else {
			if ( !v->paused ) {
				stream_pause( v );
			} else {
				stream_un_pause( v, 0 );
			}
		}
		break;
/*
	case KEY_OK_L,
	case KEY_LEFT,
	case KEY_LEFT_L,
	case KEY_RIGHT,
	case KEY_RIGHT_L,
	case KEY_UP,
	case KEY_UP_L,
	case KEY_DOWN,
	case KEY_DOWN_L,
	case KEY_MENU,
	case KEY_MENU_L,
*/
	case KEY_STOP:
		if( AV_get_state() == AV_PLAYING ) {
			AV_stop();
			FB_set_video_active(0);
		}
		break;
/*		
	case KEY_STOP_L,
	case KEY_PGUP,
	case KEY_PGDOWN,
*/
	}
}

#ifdef DEBUG_MSG
#include "file_info.h"

static void _file_info( void )
{
serprintf("file_info: %s\r\n", url );
	file_info_dump_for_path( url, 1 );
}

static void _file_type( void )
{
serprintf("file_type: %s\r\n", url );
	int type;
	int etype;
	get_file_type( url, &type, &etype);
serprintf("type %d  etype %d\n", type, etype );
}

static void _video_set_audio_stream( int argc, char *argv[] )
{
	if( !v || v->av.as_max == 1 )
		return;

	int next = 0;
	if( argc > 1 ) {
		next = atoi( argv[1] );
	} else {
		next = v->av.as + 1;
		if( next == v->av.as_max ) {
			next = 0;
		}
	}
serprintf("video_set_audio_stream: %d\r\n", next );
	stream_set_audio_stream( v, next );
}

static void _video_set_subtitle_stream( int argc, char *argv[] )
{
	if( !v || v->av.subs_max == 1 )
		return;

	int next = 0;
	if( argc > 1 ) {
		next = atoi( argv[1] );
	} else {
		next = v->av.subs + 1;
		if( next == v->av.subs_max ) {
			next = 0;
		}
	}
serprintf("video_set_subtitle_stream: %d\r\n", next );
	stream_set_subtitle_stream( v, next );
}		

DECLARE_DEBUG_COMMAND_VOID("fi", _file_info);
DECLARE_DEBUG_COMMAND_VOID("ft", _file_type);
DECLARE_DEBUG_TOGGLE      ("vidp", video_start_paused );
DECLARE_DEBUG_COMMAND     ("vas", _video_set_audio_stream );
DECLARE_DEBUG_COMMAND     ("vss", _video_set_subtitle_stream );

#endif

