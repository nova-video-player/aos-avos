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
#include "pipe.h"

#include <sys/time.h>
#include <pthread.h>

typedef struct STREAM_Q_STR {
	PIPE *pipe;
	int entry_size;
	int length;

	pthread_mutex_t mutex;
	pthread_cond_t  cond;
} STREAM_Q;

STREAM_Q *stream_q_new( int length, int entry_size ) 
{
	STREAM_Q *q;
	
	if( !length || !entry_size || !(q = amalloc( sizeof( STREAM_Q ) ) ) ) {
		return NULL;
	}
	
	q->length = length;
	q->entry_size = entry_size;
	
	if( !(q->pipe = pipe_new( entry_size * (length + 1)) ) ) {
		afree( q );
		return NULL;
	}
	
	q->length = length;
	q->entry_size = entry_size;

	pthread_cond_init (  &q->cond,  0);
	pthread_mutex_init(  &q->mutex, NULL);
	
	return q;
}

void stream_q_delete( STREAM_Q **q ) 
{
	if( !q || !*q )
		return;
		
	if( (*q)->pipe )
		pipe_delete( &(*q)->pipe );
	
	afree( *q );
	*q = NULL;
}

int stream_q_put( STREAM_Q *q, void *entry ) 
{
	if( !q || !entry )
		return 1;
		
	pthread_mutex_lock( &q->mutex );
	if( pipe_write( q->pipe, entry, q->entry_size ) != q->entry_size ) {
		pthread_mutex_unlock( &q->mutex );
		return 1;
	}
	
	pthread_cond_signal( &q->cond );
	pthread_mutex_unlock( &q->mutex );
	return 0;
}

int stream_q_get( STREAM_Q *q, void *entry )
{
	if( !q || !entry )
		return 1;
	
	pthread_mutex_lock( &q->mutex );
	if( pipe_read( q->pipe, entry, q->entry_size ) != q->entry_size ) {
		pthread_mutex_unlock( &q->mutex );
		return 1;
	}
	pthread_mutex_unlock( &q->mutex );
	return 0;
} 

int stream_q_get_wait( STREAM_Q *q, void *entry, int timeout )
{
	if( !stream_q_get( q, entry ) )
		return 0;
		
	struct timespec ts;
	struct timeval  tp;
	gettimeofday(&tp, NULL);

	ts.tv_sec  = tp.tv_sec;
	ts.tv_nsec = tp.tv_usec * 1000 + timeout * 1000000;
	if( ts.tv_nsec > 1000000000 ) {
		ts.tv_nsec -= 1000000000;
		ts.tv_sec  += 1;
	}
	
	pthread_mutex_lock( &q->mutex );
	pthread_cond_timedwait( &q->cond, &q->mutex, &ts );
	pthread_mutex_unlock( &q->mutex );
	
	return stream_q_get( q, entry );
}

int stream_q_flush( STREAM_Q *q )
{
	if( !q )
		return 1;
	
	pipe_flush( q->pipe );
	
	return 0;
} 


#ifdef DEBUG_MSG

static const int entries = 10;

static void *writer( void *_q )
{
	STREAM_Q *q = _q;
	int i;
	for( i = 0; i < entries; i++ ) {	
		do {
			unsigned int sleep = 10 * (rand() & 0x0F);
serprintf("sleep %d\r\n", sleep );
			msec_sleep( sleep );
		} while( stream_q_put( q, &i ) );
serprintf("write %d\r\n", i );
	}
	return NULL;
}	

static void UNUSED *reader( void *_q )
{
	STREAM_Q *q = _q;
	int i;
	for( i = 0; i < entries; i++ ) {	
		int entry;
		do {
			unsigned int sleep = 10 * (rand() & 0x0F);
serprintf("\t\tsleep %d\r\n", sleep );
			msec_sleep( sleep );
		} while( stream_q_get( q, &entry ) );
serprintf("\t\tread  %d\r\n", entry );
	}
	return NULL;
}

static void *reader2( void *_q )
{
	STREAM_Q *q = _q;
	int i;
	for( i = 0; i < entries; i++ ) {	
		int entry;
		while( stream_q_get_wait( q, &entry, 100 ) ) {
serprintf("\t\tTIMEOUT!\r\n");
		}
serprintf("\t\tread  %d\r\n", entry );
	}
	return NULL;
}

static void _test( void )
{
	const int max = 5;
	
	STREAM_Q *q = stream_q_new( max, sizeof( int ) );
	
	int i;
	for( i = 0; i < max; i++ ) {
		if( stream_q_put( q, &i ) ) {
serprintf("failed to put %d\r\n", i );		
			break;
		}
	}
	
	for( i = 0; i < max; i++ ) {
		int entry;
		if( stream_q_get( q, &entry ) ) {
serprintf("failed to get %d\r\n", i );		
			break;
		}
serprintf("got %d: %d\r\n", i, entry );		
		
	}
	
	pthread_t    writer_handle;
	pthread_t    reader_handle;
	
	thread_create( &writer_handle, writer, (void*)q, 0, "writer");
	thread_create( &reader_handle, reader2, (void*)q, 0, "reader");

	apthread_join( writer_handle, NULL );
	apthread_join( reader_handle, NULL );
	
	stream_q_delete( &q );
	
}

DECLARE_DEBUG_COMMAND_VOID( "sqt", _test );
#endif
