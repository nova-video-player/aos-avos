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
#include "debug.h"
#include "atime.h"
#include "astdlib.h"
#include "util.h"
#include "file.h"

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>

#ifdef CONFIG_STREAM

#define DBGV  if(Debug[DBG_VID])
#define DBGH  if(Debug[DBG_HD])
#define DBGH2 if(Debug[DBG_HD] & 4)
#define DBGS  if(Debug[DBG_STREAM])

int stream_drive_wake_sleep = 5000;	// start buffering again at this number of seconds worth of
					// data in the buffer - in sleep mode

int stream_drive_wake_no_sleep = 2000;	// start buffering again at this number of seconds worth of
					// data in the buffer - in no sleep case

#define MIN_VIDEO_RATE 		(250000/8)	// minimum rate, 250kbit/s

int stream_buffer_time = 1;
static int stream_buffer_priority = 0;
static int stream_buffer_o_direct = 0;
static int stream_clear_buffer    = 0;

// ************************************************************
//
//	_calc_buffer_threshold
//
// ************************************************************
static int _calc_buffer_threshold( STREAM_BUFFER *buffer, int time_to_wake ) 
{
	STREAM *s = buffer->s;
	
	int time_parsed;
	int rate;
	
	if( buffer == s->buffer2 ) {
		// we have a separate audio buffer
		time_parsed = s->atime_parsed;
		rate        = s->acurrent_rate;
	} else {
		if ( s->audio->valid ) {
			time_parsed = MIN( s->vtime_parsed,  s->atime_parsed  ); 
			rate        = MAX( s->vcurrent_rate, s->acurrent_rate ); 
		} else {
			time_parsed = s->vtime_parsed;
			rate        = s->vcurrent_rate;
		}
	}
	
	if ( time_parsed > time_to_wake ) {
		// no need to wake the drive
	} else {
		// we have to substract the overlap size, as the parser will not eat into it!
		int head = stream_buffer_get_head( buffer ) - buffer->overlap_size; 
		
		if ( head < 0 ) {
			// we cannot parse further, we need to wake the drive
DBGH serprintf("\r\nWAKE: getHead %d  time parsed %d/%d  rate %d\r\n", head, time_parsed, time_to_wake, rate );			
			return 1;
		} else {
			// we have not yet parsed all the data, predict how much there is left in the buffer
			if ( rate ) {
				int time_in_head   = (UINT64)1000 * (UINT64)head / (UINT64)rate;
				int time_in_buffer = time_parsed + time_in_head;		
				
				if ( time_in_buffer <= time_to_wake ) {
DBGH serprintf("\r\nWAKE: head %d  rate %d  time: head %d  parsed %d  buffer %d/%d\r\n", head, rate, time_in_head, time_parsed, time_in_buffer, time_to_wake);			
					return 1;
				}
			} else {
				// rate is 0, but only for a short time, so no need to panic!
			}
		}
	}	
	
	// no need to wake up
	return 0;
}	

// ************************************************************
//
//	stream_buffer_drive_needs_to_wake_up
//
// ************************************************************
int stream_buffer_drive_needs_to_wake_up( STREAM_BUFFER *buffer ) 
{
	int time_to_wake;
	
	if ( buffer->no_sleep ) {
		time_to_wake = stream_drive_wake_no_sleep;
	} else {
		time_to_wake = stream_drive_wake_sleep;
	}
	
	return _calc_buffer_threshold( buffer, time_to_wake );
}

// ************************************************************
//
//	stream_buffer_flush
//
// ************************************************************
int stream_buffer_flush( STREAM_BUFFER *buffer )
{
	if( !buffer )
		return 1;
		
	if( buffer->s )
		pthread_mutex_lock( &buffer->s->parser_buffer_mutex );

	buffer->buf_write     = 0;
	buffer->buf_write_pos = buffer->data_start;
	buffer->buf_write_cls = 0;

	buffer->buf_scan      = 0;
	buffer->buf_scan_pos  = buffer->data_start;
	
	buffer->buf_read      = 0;
	buffer->buf_tail      = 0;
	buffer->buf_wrap      = 0;
	buffer->buf_end       = 0;

	stream_buffer_free_all_data( buffer, 0, 0 );
	
	if( buffer->s )
		pthread_mutex_unlock( &buffer->s->parser_buffer_mutex );

	return 0;
}

// ************************************************************
//
//	_buffer_thread
//
// ************************************************************
static void *_buffer_thread( void *data )
{
	STREAM_BUFFER *buffer = (STREAM_BUFFER *)data;
DBGS serprintf("PID[%5d] stream_buffer_thread::Starting %s\r\n", getpid(), buffer->tag );	
	
	while( buffer->run ) {
		if( !pthread_mutex_trylock( &buffer->mutex ) ) {
			if( !buffer->paused )
				buffer->buffer( buffer, 1 );
			pthread_mutex_unlock( &buffer->mutex );
		}
		
		if( buffer->flags & STREAM_BUFFER_NO_YIELD ){
			if( buffer->buf_end ){
				// if we are at the end, yield
				msec_sleep(500);
			}
			continue;
		}

		if( buffer->state == BUFFER_SLEEP ) {
			msec_sleep( 500 );
		} else {
			msec_sleep( buffer->sleep_time );
		}
	}
DBGS serprintf("PID[%5d] stream_buffer_thread::Exiting %s\r\n", getpid(), buffer->tag);	
	return NULL;
}

static int _buffer_abort( STREAM_BUFFER *buffer )
{
	// check internal abort first
	if( buffer->abort ) {
serprintf("stream_buffer: abort: %d\r\n", buffer->abort ); 	
		return buffer->abort;
	}
		 
	// if the stream aborts, assume it's final
	STREAM *s = buffer->s;
	if( stream_abort( s ) ) {
		return STREAM_BUFFER_ABORT_FINAL;
	}
	
	return STREAM_BUFFER_NO_ABORT;
}

static int mmap_file_buffer( STREAM_BUFFER *buffer, int size )
{
	// open the backing store
	buffer->mmap_fd = file_open( buffer->mmap_file, O_RDWR | O_TRUNC | O_CREAT, 0600 );
	if( buffer->mmap_fd == -1 ) {
serprintf("cannot open %s due to %s\n", buffer->mmap_file, strerror( errno ) );
		goto ErrorExit;
	}
	
	// we managed to open it, now see if we can use the full size
	buffer->mmap_size = size;
	int pos = size - 1;
	if( pos != file_seek( buffer->mmap_fd, pos, SEEK_SET ) ) {
serprintf("cannot seek to %d due to %s\n", pos, strerror( errno ) );
		goto ErrorExitClose;
	}
	unsigned char test = 42;
	if( 1 != file_write( buffer->mmap_fd, &test, 1 ) ) {
serprintf("cannot write at %d due to %s\n", size - 1, strerror( errno ) );
		goto ErrorExitClose;
	}

	buffer->data = mmap( 0, buffer->mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, buffer->mmap_fd, 0 );
DBGS serprintf("buffer->data: %08X\n", buffer->data);

	if( buffer->data == MAP_FAILED ) {
serprintf("mmap failed for: size %lld, file %s\n", size, buffer->mmap_file);
		goto ErrorExitClose;
	}

	return 0;
	
ErrorExitClose:
	file_close( buffer->mmap_fd );
	file_remove( buffer->mmap_file );
ErrorExit:
	return 1;
}

// ************************************************************
//
//	stream_buffer_alloc
//
// ************************************************************
int stream_buffer_alloc( STREAM_BUFFER *buffer, int size )
{
	if( buffer->flags & STREAM_BUFFER_MMAP_FILE && buffer->mmap_file ) {
		return mmap_file_buffer( buffer, size );
	} else if( buffer->flags & STREAM_BUFFER_MMAP ) {
		buffer->mmap_size = size;
		buffer->data = mmap( 0, buffer->mmap_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0 );	
DBGS serprintf("buffer->data: %08X\n", buffer->data);
		if( buffer->data == MAP_FAILED ) {
serprintf("can't mmap buffer for %d\n", buffer->mmap_size);
			return 1;
		}
	} else {
		if( !( buffer->data = amalloc( size )) ) {
serprintf("no mem for buffer\n");
			return 1;
		}
	}
	return 0;
} 

// ************************************************************
//
//	stream_buffer_free
//
// ************************************************************
void stream_buffer_free( STREAM_BUFFER *buffer )
{
	if( !buffer->data || buffer->virtual )
		return;

	if( buffer->flags & STREAM_BUFFER_MMAP_FILE && buffer->mmap_file && buffer->mmap_fd != -1 ) {
		munmap( buffer->data, buffer->mmap_size );	
		file_close( buffer->mmap_fd );
		file_remove( buffer->mmap_file );
	} else if( buffer->flags & STREAM_BUFFER_MMAP ) {
		munmap( buffer->data, buffer->mmap_size );
	} else {
		afree( buffer->data );
	}
}

// ************************************************************
//
//	stream_buffer_resize
//
// ************************************************************
int stream_buffer_resize( STREAM_BUFFER *buffer, int new_size )
{
DBGS serprintf("stream_buffer_resize(%s  new_size %d)\r\n", buffer->tag, new_size  );
	if( buffer->flags & STREAM_BUFFER_MMAP_FILE && buffer->mmap_file ) {
		return 1;
	} else if( buffer->flags & STREAM_BUFFER_MMAP ) {
		int new_mmap_size = new_size + buffer->overlap_size;
		unsigned char *data = mremap(buffer->data, buffer->mmap_size, new_mmap_size, MREMAP_MAYMOVE);
		if( data == MAP_FAILED ) {
serprintf("mremap failed with %s\n", strerror( errno ) );
			return 1;
		}
DBGS serprintf("mremap to %08X\n", data );
		buffer->data        = data;
		buffer->mmap_size   = new_mmap_size;
		buffer->buffer_size = new_size;

	} else {
		buffer->data = arealloc( buffer->data, new_size + buffer->overlap_size );
		if( !buffer->data ) {
serprintf("realloc failed with %s\n", strerror( errno ) );
			return 1;
		}
		buffer->buffer_size = new_size;
	}
	return 0;
}

// ************************************************************
//
//	stream_buffer_resize_and_rebuffer
//
// ************************************************************
int stream_buffer_resize_and_rebuffer( STREAM_BUFFER *buffer, int new_size )
{
DBGS serprintf("stream_buffer_resize_and_rebuffer(%s  new_size %d)\r\n", buffer->tag, new_size  );
	return buffer->set_pos( buffer, buffer->buf_scan_pos, 1, new_size );
}

// ************************************************************
//
//	_open
//
// ************************************************************
static int _open( STREAM_BUFFER *buffer, STREAM *s, STREAM_IO *io, 
			int buffer_size, int overlap_size, UINT64 start, UINT64 end, 
			UINT32 flags, const char *tag, STREAM_BUFFER *src )
{
DBGS serprintf("stream_buffer_open(%s  size %d  over %d  start %llX  end %llX  flags %08X)\r\n", tag, buffer_size, overlap_size, start, end,  flags  );
	buffer->flags = flags;
	buffer->s     = s;
	buffer->io    = io;

	strcpy( buffer->tag, tag );
	
	buffer->buffer_size  = buffer_size;
	buffer->overlap_size = overlap_size;
	buffer->data_start   = start;
	buffer->data_end     = end;

	if( buffer->io ) {
		stream_io_set_parts     ( buffer->io, s->num_parts, s->parts, s->get_part_name );
		stream_io_set_chunk_size( buffer->io, stream_buffer_chunk( s ) );
		stream_io_set_abort     ( buffer->io, (ABORT_HANDLER)_buffer_abort, buffer );
		int mode = O_RDONLY;
#ifndef SIM
		if( stream_buffer_o_direct || (buffer->flags & STREAM_BUFFER_O_DIRECT) ) {
			mode |= O_DIRECT;
		}
#endif
		int err;
		if( ( err = buffer->io->open( buffer->io, mode ) ) ) {
serprintf("cannot open io %s, err %d\r\n", s->src.url, err );
		 	return err;
		}
	}

	// check if the size changed?
	UINT64 io_size = 0;
	if( !buffer->io ) {
		io_size = 0xFFFFFFFFFFFFFFFull; // no io -> assume it is a streaming source with infinite size
	} else if( s->size != buffer->io->size && s->num_parts == 1 ) {
		io_size = buffer->io->size;
	}
	if ( io_size ) {
serprintf("update size! %lld\r\n", io_size );
		s->size = io_size;
		if( !buffer->data_end ) {
serprintf("update end!\r\n" );
			buffer->data_end = s->size;
		}
	}

	if( src ) {
		// we take the mem away from an existing buffer
		src->buffer_size -= buffer_size + overlap_size;
		buffer->data = src->data + src->buffer_size + src->overlap_size;
		buffer->virtual = 1;
	} else {
		if( s->size < buffer_size ) {
			// if the file is small and fits into the buffer, don't do MMAP_FILE
			buffer->mmap_file = NULL;
		}
		if( stream_buffer_alloc( buffer, buffer_size + overlap_size ) ) {
serprintf("could not alloc buffer size %d\n", buffer_size + overlap_size);
			buffer->io->close( buffer->io );
			return 1;
		}
	}
	
	if( stream_clear_buffer ) {
serprintf("stream_clear_buffer\r\n");
		memset( buffer->data, 0, buffer_size + overlap_size );
	}

	stream_buffer_flush( buffer );
	
	stream_buffer_state_set( buffer, BUFFER_RDWR );
	buffer->state_changed = 0;
	
	pthread_mutex_init ( &buffer->mutex, NULL );

	// lock and start the buffer_thread
	if( buffer->io ){
		pthread_mutex_lock( &buffer->mutex );
		buffer->run = 1;
	
		thread_create( &buffer->thread_handle, _buffer_thread, (void*)buffer, stream_buffer_priority, "stream buffer" );
		
		// allow the buffering
		pthread_mutex_unlock( &buffer->mutex );
	}
	
	buffer->is_open = 1;

	return 0;
}

// ************************************************************
//
//	stream_buffer_open
//
// ************************************************************
int stream_buffer_open( STREAM_BUFFER *buffer, STREAM *s, STREAM_IO *io, 
			int buffer_size, int overlap_size, UINT64 start, UINT64 end, 
			UINT32 flags, const char *tag )
{
	return _open( buffer, s, io, buffer_size, overlap_size, start, end, flags, tag, NULL );
}

// ************************************************************
//
//	stream_buffer_split
//
// ************************************************************
int stream_buffer_split( STREAM_BUFFER *buffer, STREAM *s, STREAM_IO *io, 
			int buffer_size, int overlap_size, UINT64 start, UINT64 end, 
			UINT32 flags, const char *tag, STREAM_BUFFER *src )
{
	return _open( buffer, s, io, buffer_size, overlap_size, start, end, flags, tag, src );
}


// ************************************************************
//
//	stream_buffer_close
//
// ************************************************************
int stream_buffer_close( STREAM_BUFFER *buffer )
{
DBGS serprintf("stream_buffer_close\r\n" );
	if( !buffer->is_open ) {
serprintf("sb not open!\r\n");
		return 1;
	}

	pthread_mutex_lock( &buffer->mutex );

	if( buffer->run ) {
		buffer->run = 0;
		apthread_join( buffer->thread_handle, NULL );
DBGS serprintf("buffer_thread joined\r\n");
	}

	pthread_mutex_unlock( &buffer->mutex );
	pthread_mutex_destroy( &buffer->mutex );
	
	stream_buffer_free( buffer );

	buffer->is_open = 0;
	
	if( buffer->io )
		return buffer->io->close( buffer->io );
	
	return 0;
}

// ************************************************************
//
//	stream_buffer_pause
//
// ************************************************************
void stream_buffer_pause( STREAM_BUFFER *buffer )
{
DBGS serprintf("stream_buffer_pause\n");
	pthread_mutex_lock( &buffer->mutex );

	buffer->paused ++;
	
	pthread_mutex_unlock( &buffer->mutex );
}

// ************************************************************
//
//	stream_buffer_un_pause
//
// ************************************************************
void stream_buffer_un_pause( STREAM_BUFFER *buffer )
{
DBGS serprintf("stream_buffer_un_pause\n");
	pthread_mutex_lock( &buffer->mutex );
	
	if( buffer->paused > 0 )
		buffer->paused --;
	
	pthread_mutex_unlock( &buffer->mutex );
}

// ************************************************************
//
//	stream_buffer_peek_byte()
//
// ***********************************************************
unsigned char stream_buffer_peek_byte( STREAM_BUFFER *buffer, int offset )
{
	int peek = buffer->buf_scan + offset;
	
	if (peek >= buffer->buffer_size)
		 peek -= buffer->buffer_size;
	
	return buffer->data[peek];
}

// ************************************************************
//
//	stream_buffer_read_byte()
//
// ***********************************************************
unsigned char stream_buffer_read_byte( STREAM_BUFFER *buffer )
{
	unsigned char b = buffer->data[buffer->buf_scan];

	buffer->buf_scan ++;
	if (buffer->buf_scan == buffer->buffer_size)
		 buffer->buf_scan = 0;

	buffer->buf_scan_pos ++;

	return b;
}

// ************************************************************
//
//	stream_buffer_read()
//
// ***********************************************************
void stream_buffer_read( STREAM_BUFFER *buffer, unsigned char * b, int size )
{
	while ( size-- > 0 ) {
		*(b++) = stream_buffer_read_byte( buffer );
	}
}

// ************************************************************
//
//	stream_buffer_get_pos()
//
// ***********************************************************
UINT64 stream_buffer_get_pos( STREAM_BUFFER *buffer )
{
	return buffer->buf_scan_pos;
}

// ************************************************************
//
//	stream_buffer_get_buf_pos()
//
// ***********************************************************
UINT stream_buffer_get_buf_pos( STREAM_BUFFER *buffer )
{
	return buffer->buf_scan;
}

// ************************************************************
//
//	stream_buffer_get_pointer()
//
// ***********************************************************
UCHAR *stream_buffer_get_pointer( STREAM_BUFFER *buffer )
{
	return buffer->data + buffer->buf_scan;
}

// ************************************************************
//
//	stream_buffer_EOF()
//
// ***********************************************************
int stream_buffer_EOF( STREAM_BUFFER *buffer )
{
	return ( buffer->buf_scan_pos >= buffer->data_end );
}

// ************************************************************
//
//	stream_buffer_skip_buf_pos()
//
// ***********************************************************
void stream_buffer_skip_buf_pos( STREAM_BUFFER *buffer, int offset )
{
	buffer->buf_scan += offset;

	while (buffer->buf_scan >= buffer->buffer_size)
		 buffer->buf_scan -= buffer->buffer_size;
}

// ************************************************************
//
//	stream_buffer_skip_pos()
//
// ***********************************************************
void stream_buffer_skip_pos( STREAM_BUFFER *buffer, INT64 offset )
{
	buffer->buf_scan_pos += offset;
}

// ************************************************************
//
//	stream_buffer_skip()
//
// ***********************************************************
UINT64 stream_buffer_skip( STREAM_BUFFER *buffer, int offset )
{
	stream_buffer_skip_buf_pos( buffer, offset );
	stream_buffer_skip_pos    ( buffer, offset );
	return buffer->buf_scan_pos;
}

// ************************************************************
//
//	stream_buffer_set_pos
//
// ************************************************************
int stream_buffer_set_pos( STREAM_BUFFER *buffer, UINT64 pos, int force_reload )
{
	return buffer->set_pos( buffer, pos, force_reload, 0 );
}

// ************************************************************
//
//	stream_buffer_set_pos_seek
//
//	used when seeking, will change pos AND free up all past
//	data even when not reloading!
//
// ************************************************************
int stream_buffer_set_pos_seek( STREAM_BUFFER *buffer, UINT64 pos, int force_reload )
{
	if( buffer->set_pos( buffer, pos, force_reload, 0 ) ) {
		return 1;
	}
	stream_buffer_free_all_data( buffer, buffer->buf_scan_pos, buffer->buf_scan );
	return 0;
}

// ************************************************************
//
//	stream_buffer_get_free
//
// ***********************************************************
int stream_buffer_get_free( STREAM_BUFFER *buffer )
{	
	if( !buffer )
		return 0;
		
	int free = buffer->buf_read - buffer->buf_write;
	if (free <= 0) 
		free += buffer->buffer_size;
	return free;
}	

// ************************************************************
//
//	stream_buffer_get_used
//
// ***********************************************************
int stream_buffer_get_used( STREAM_BUFFER *buffer )
{	
	if( !buffer )
		return 0;

	int used = buffer->buf_write - buffer->buf_read;
	if (used < 0) 
		used += buffer->buffer_size;
	return used;
}	

// ************************************************************
//
//	stream_buffer_get_head
//
// ***********************************************************
int stream_buffer_get_head( STREAM_BUFFER *buffer )
{	
	int used = buffer->buf_write - buffer->buf_scan;
	if (used < 0) 
		used += buffer->buffer_size;
	return used;
}	

// ************************************************************
//
//	stream_buffer_get_tail
//
// ***********************************************************
int stream_buffer_get_tail( STREAM_BUFFER *buffer )
{	
	int used = buffer->buf_scan - buffer->buf_tail;
	if (used < 0) 
		used += buffer->buffer_size;
	return used;
}	

// ************************************************************
//
//	_copy_nowrap
//
// ***********************************************************
static void _copy_nowrap( STREAM_BUFFER *buffer, const unsigned char *data, int count )
{
	memcpy( buffer->data + buffer->buf_write, data, count );
		
	// copy start to overlap area !
	if ( buffer->buf_write < buffer->overlap_size ) {
		int copy = MIN( count, buffer->overlap_size - buffer->buf_write);
		
//serprintf("cbe ovrl:  %d  %d\r\n", buffer->buffer_size + buffer->buf_write, size );
		memcpy( buffer->data + buffer->buffer_size + buffer->buf_write, data, copy );
	}
	buffer->buf_write_pos += count;
	buffer->buf_write     += count;	

	if( buffer->buf_write >= buffer->buffer_size )
		buffer->buf_write -= buffer->buffer_size;
}

// ************************************************************
//
//	stream_buffer_write
//
// ***********************************************************
int stream_buffer_write( STREAM_BUFFER *buffer, const UCHAR *data, int size )
{
	if( !buffer ) {
		return 0;
	}

	if( buffer->s )
		pthread_mutex_lock( &buffer->s->parser_buffer_mutex );
		
	int free = stream_buffer_get_free( buffer );
DBGH2 serprintf("%s                              wr %8d               rd %8d  sc %8d/%8lld  fr %8d  sz %8d\r\n", buffer->tag, buffer->buf_write, 
		buffer->buf_read, buffer->buf_scan, buffer->buf_scan_pos, free, size );			
	if( free < size ) {
		// we cannot store this data, return
		if( buffer->s )
			pthread_mutex_unlock( &buffer->s->parser_buffer_mutex );
		return 0;
	}
		
	int rest = buffer->buffer_size - buffer->buf_write;
//serprintf("write: %d  %d\r\n", buffer->buf_write, count );
	if( rest >= size ) {
		_copy_nowrap( buffer, data, size );
	} else {
		_copy_nowrap( buffer, data,        rest );
		_copy_nowrap( buffer, data + rest, size - rest );
		
		// we wrapped around the buffer end, buf_tail is now buf_write
serprintf("__BUFFER_WRAP__\r\n");
		buffer->buf_wrap  = 1;
	}	

	if ( buffer->buf_wrap ) {
		// buf_tail moves with buf_write when we wrapped!
		buffer->buf_tail = buffer->buf_write;
	}
	
	if( buffer->s )
		pthread_mutex_unlock( &buffer->s->parser_buffer_mutex );
	return size;
}

// ************************************************************
//
//	stream_buffer_get_write_pointer
//
// ***********************************************************
int stream_buffer_get_write_pointer( STREAM_BUFFER *buffer, UCHAR **data )
{
	if( !buffer ) {
		return 0;
	}

	if( buffer->s )
		pthread_mutex_lock( &buffer->s->parser_buffer_mutex );

	int free = stream_buffer_get_free( buffer );
	
	// at the buffer end the size of the chunk to write is limited by the overlap_size!
	int rest = buffer->buffer_size + buffer->overlap_size - buffer->buf_write;
	if ( free > rest ) {
		free = rest;	
	}
DBGH2 serprintf("%s                              wr %8d               rd %8d  sc %8d/%8lld  fr %8d\r\n", 
		buffer->tag, buffer->buf_write, buffer->buf_read, buffer->buf_scan, buffer->buf_scan_pos, free );			

	if( data )
		*data = buffer->data + buffer->buf_write;
	return free;
}
	
// ************************************************************
//
//	stream_buffer_update_write_pointer
//
// ***********************************************************
int stream_buffer_update_write_pointer( STREAM_BUFFER *buffer, int count )
{
	if( !buffer ) {
		return 1;
	}

	if( buffer->buf_write < buffer->overlap_size ) {
		// start of buffer, copy to overlap area
		int copy = MIN( count, buffer->overlap_size - buffer->buf_write);
		
//serprintf("cbe ovrl:  %d  %d\r\n", buffer->buffer_size + buffer->buf_write, count );
		memcpy( buffer->data + buffer->buffer_size + buffer->buf_write, buffer->data + buffer->buf_write, copy );
	} 
	
	if( buffer->buf_write + count > buffer->buffer_size ) {
		// overlap area, copy to start of buffer
		memcpy( buffer->data, buffer->data + buffer->buffer_size, buffer->buf_write + count - buffer->buffer_size );
				
	}

	// update pointers
	buffer->buf_write_pos += count;
	buffer->buf_write     += count;	

	if( buffer->buf_write >= buffer->buffer_size ) {
		buffer->buf_write -= buffer->buffer_size;
		buffer->buf_wrap   = 1;
	}
	
	if ( buffer->buf_wrap ) {
		// buf_tail moves with buf_write when we wrapped!
		buffer->buf_tail = buffer->buf_write;
	}
	
	if( buffer->s )
		pthread_mutex_unlock( &buffer->s->parser_buffer_mutex );
	
	return count;
}

#undef DBG
#define DBG if(0)

// ************************************************************
//
//	_free_data
//
// ***********************************************************
static void _free_data( STREAM_BUFFER *buffer ) 
{
	static int last_read;	
	
DBG { if( buffer == ((STREAM*)buffer->s)->buffer2 ) serprintf("<A"); else serprintf("<V"); }

	// determine the position of the last used byte
	// in the buffer
	UINT64 last_pos  = (UINT64)-1;
	int buf_read = buffer->buf_read;
	
	if( buffer->video && buffer->vid_last_pos < last_pos ) {
DBG serprintf("v");
		buf_read = buffer->vid_last_buf;
		last_pos = buffer->vid_last_pos;
	} 
	if( buffer->audio && buffer->aud_last_pos < last_pos ) {
DBG serprintf("a");
		buf_read = buffer->aud_last_buf;
		last_pos = buffer->aud_last_pos;
	} 
	if( buffer->subtitle && buffer->sub_last_pos < last_pos ) {
DBG serprintf("s");
		buf_read = buffer->sub_last_buf;
		last_pos = buffer->sub_last_pos;
	} 
	
	buffer->buf_read = buf_read;
	
	if ( buffer->buf_read != last_read ) {
		last_read = buffer->buf_read;
DBG serprintf("(%d)", last_read );
	}
}

// ************************************************************
//
//	stream_buffer_free_data
//
// ***********************************************************
void stream_buffer_free_data( STREAM_BUFFER *buffer, STREAM_CHUNK *sc )
{
	if( sc->type == TYPE_VID ) {
		buffer->vid_last_pos = sc->pos + sc->size;
		buffer->vid_last_buf = sc->buf + sc->size;
		if( buffer->vid_last_buf > buffer->buffer_size )
			buffer->vid_last_buf -= buffer->buffer_size;
		_free_data( buffer );
	} else if ( sc->type == TYPE_AUD ) {
		buffer->aud_last_pos = sc->pos + sc->size;
		buffer->aud_last_buf = sc->buf + sc->size;
		if( buffer->aud_last_buf > buffer->buffer_size )
			buffer->aud_last_buf -= buffer->buffer_size;
		_free_data( buffer );
	} else if ( sc->type == TYPE_SUBT ) {
		buffer->sub_last_pos = sc->pos + sc->size;
		buffer->sub_last_buf = sc->buf + sc->size;
		if( buffer->sub_last_buf > buffer->buffer_size )
			buffer->sub_last_buf -= buffer->buffer_size;
		_free_data( buffer );
	}
}

void stream_buffer_fix_subs( STREAM_BUFFER *buffer )
{
	if( !buffer )
		return;
	buffer->sub_last_pos = buffer->vid_last_pos;
	buffer->sub_last_buf = buffer->vid_last_buf;
}

// ************************************************************
//
//	stream_buffer_free_all_data
//
// ***********************************************************
void stream_buffer_free_all_data( STREAM_BUFFER *buffer, UINT64 pos, UINT buf )
{
	buffer->vid_last_pos = pos;
	buffer->vid_last_buf = buf;
	
	buffer->aud_last_pos = pos;
	buffer->aud_last_buf = buf;

	buffer->sub_last_pos = pos;
	buffer->sub_last_buf = buf;
	
	_free_data( buffer );
}

#ifdef DEBUG_MSG
extern int stream_buffer_time;

static void _stream_buffer_time( int argc, char *argv[] )
{ 
	if( argc > 1 ) {
		stream_buffer_time = atoi( argv[1] );
	} else {
		stream_buffer_time = 10;
	}
serprintf("stream_buffer_time: %d\r\n", stream_buffer_time ); 
}

static void _stream_buffer_o_direct( int argc, char *argv[] )
{ 
	stream_buffer_o_direct = 1 - stream_buffer_o_direct;
serprintf("stream_buffer_o_direct: %d\r\n", stream_buffer_o_direct );
}

static void _stream_clear_buffer( int argc, char *argv[] )
{ 
	stream_clear_buffer = 1 - stream_clear_buffer;
serprintf("stream_clear_buffer: %d\r\n", stream_clear_buffer ); 
}

DECLARE_DEBUG_COMMAND("sbt", 	_stream_buffer_time     );
DECLARE_DEBUG_COMMAND("sbo", 	_stream_buffer_o_direct );
DECLARE_DEBUG_COMMAND("scb", 	_stream_clear_buffer    );
DECLARE_DEBUG_PARAM( "sdws", stream_drive_wake_sleep );
DECLARE_DEBUG_PARAM( "sdwns", stream_drive_wake_no_sleep );
#endif

#endif
