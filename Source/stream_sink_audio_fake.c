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
#include "pipe.h"

#include <stdint.h>

#ifdef CONFIG_STREAM
static int debug_fas = 0;

DECLARE_DEBUG_TOGGLE( "dbgfas", debug_fas );

#define DBGS 	if(Debug[DBG_STREAM])
#define DBG	if(debug_fas)

static int 		delay = 0;
static int 		rate;
static int 		block_size = 8192;
static int 		block_num  = 8;
static PIPE 		*out_pipe = NULL;
static pthread_mutex_t  pipe_mutex;

static pthread_t 	out_thread_handle;
static pthread_mutex_t  out_mutex;
static int		out_run;

static int 		ref_time;
static uint64_t		ref_bytes;
static int 		last_time;

static int _flush( STREAM *s );

static void output( STREAM *s )
{
	UCHAR data[block_size];
	int used = pipe_get_used( out_pipe );
	
	if( used == 0 ) {
		// we have an underrun!
		if( ref_bytes != 0 ) {
serprintf("ssaf: UNDERRUN!\n");	
		}
		ref_time  = atime();
		ref_bytes = 0;
		msec_sleep(20);	
		return;
	}
	
	// where should we be now:
	int now_time = atime() - ref_time;
	uint64_t now_bytes = (uint64_t )now_time * rate * 2 * 2 / 1000;
	
	uint64_t diff = now_bytes - ref_bytes;
	
	int out  = MIN( block_size, diff );
	out = MIN( out, used );
//DBG2 serprintf("ssaf: time %8d  diff %6lld  used %6d  out %6d\n", now_time, diff, used, out );
DBG serprintf("ssaf: %2d  used %6d  out %6d\n", now_time - last_time, used, out );

	pipe_read( out_pipe, data, out );

	ref_bytes += out;
	last_time = now_time;
	
	msec_sleep(20);	
}

// ************************************************************
//
//	out_thread
//
// ************************************************************
static void *out_thread( void *data )
{
	STREAM *s = (STREAM*)data;
DBGS serprintf("out_thread::Starting\r\n");	
	
	while( out_run ) {
		if( !pthread_mutex_trylock( &out_mutex ) ) {
			
			output( s );
			
			pthread_mutex_unlock( &out_mutex );
		}
		
		msec_sleep( 1 );
	}
DBGS serprintf("out_thread::Exiting\r\n");	
	return NULL;
}

static int _open( STREAM *s )
{
	return 0;
}

static int _close( STREAM *s )
{
	return 0;
}

static int _start( STREAM *s )
{
	rate  = s->audio->samplesPerSec;
	delay = (UINT64)1000 * (UINT64)(block_size * block_num) / (rate * 2 * 2);
DBGS serprintf("stream_sink_audio_open_fake: rate %d  delay %d\r\n", rate, delay );

	if( !(out_pipe = pipe_new( block_size * block_num ) ) ) {
		return 1;
	}

	pthread_mutex_init ( &out_mutex,  NULL );
	pthread_mutex_init ( &pipe_mutex, NULL );
	
	_flush( s );
	
	// lock and start the out_thread
	pthread_mutex_lock( &out_mutex );
	out_run = 1;
	apthread_create( &out_thread_handle, 0, out_thread, (void*)s, "audio fake out" );

	ref_time  = atime();
	ref_bytes = 0;

	pthread_mutex_unlock( &out_mutex );

	s->audio_sink_open = 1;

	return 0;
}

static int _stop( STREAM *s )
{
DBGS serprintf("stream_sink_audio_close_fake\r\n");
	if( !s->audio_sink_open ) {
serprintf("ssaf not open!\r\n");
		return 1;
	}

	pthread_mutex_lock( &out_mutex );
	
DBGS serprintf("waiting for out thread to join\r\n");
	if( out_run ) {
		out_run = 0;
		apthread_join( out_thread_handle, NULL );
DBGS serprintf("out_thread joined\r\n");
	}
	
	pthread_mutex_unlock( &out_mutex );
	pthread_mutex_destroy( &out_mutex );
	
	pthread_mutex_destroy( &pipe_mutex );

	if( out_pipe ) {
		pipe_delete( &out_pipe );
	}
	s->audio_sink_open = 0;
	return 0;
}

static int _flush( STREAM *s )
{
DBGS serprintf("stream_sink_audio_flush_fake\r\n");
	pthread_mutex_lock( &pipe_mutex );
	pipe_flush(out_pipe);
	pthread_mutex_unlock( &pipe_mutex );

	return 0;
}

static int _end( STREAM *s )
{
	return 0;
}

static int _syncable( STREAM *s )
{
	return 1;
}

static int _write( STREAM *s, AUDIO_FRAME *frame )
{
	UCHAR *data = frame->data;
	int    size = frame->size;
	
	while( size ) {
		int copy = MIN( size, block_size );
//DBGS serprintf("ssaf: free %5d  copy: %5d\r\n", pipe_get_free( out_pipe ), copy );
		pipe_write_wait( out_pipe, data, copy );
		data += copy;
		size -= copy;
	}

	return frame->size;
}

static int _preload( STREAM *s )
{
	return 0;
}

static int _delay( STREAM *s )
{
	return delay;
}

static int _can_write( STREAM *s, int len )
{
	return pipe_get_free( out_pipe ) >= block_num;
}

static int _set_passthrough( STREAM *s, int pass )
{
	return 0;
}

static int _get_passthrough( STREAM *s )
{
	return 0;
}

static int _mute( STREAM *s, int mute )
{
	return 0;
}

STREAM_SINK_AUDIO stream_sink_audio_FAKE =
{
	"audio fake",
	_open,
	_close,
	_start,
	_stop,
	_write,
	_preload,
	_flush,
	_end,
	_syncable,
	_delay,
	_can_write,
	_set_passthrough,
	_get_passthrough,
	_mute,
};
#endif
