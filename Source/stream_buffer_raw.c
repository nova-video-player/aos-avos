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

#ifdef CONFIG_STREAM

#define DBGH  if(Debug[DBG_HD])
#define DBGH2 if(Debug[DBG_HD] & 2)
#define DBGS  if(Debug[DBG_STREAM])

#define MAX_READ_CLUSTER 		4	// maximum number of sectors per read transfer
	
#define MIN_FREE_VIDEO 			2	// stop buffering at this number of clusters empty in buffer

int stream_buffer_drive_needs_to_wake_up( STREAM_BUFFER *buffer );
int stream_buffer_close( STREAM_BUFFER *buffer );

// ************************************************************
//
//	_rewind_buf_pos()
//
// ***********************************************************
static void _rewind_buf_pos( STREAM_BUFFER *buffer, int offset )
{
	buffer->buf_scan -= offset;

	while (buffer->buf_scan < 0)
		buffer->buf_scan += buffer->buffer_size;
}

// ************************************************************
//
//	_rewind_pos()
//
// ***********************************************************
static void _rewind_pos( STREAM_BUFFER *buffer, INT64 offset )
{
	buffer->buf_scan_pos -= offset;
}

// ************************************************************
//
//	__set_pos_blocked
//
// ************************************************************
static UINT64 __set_pos_blocked( STREAM_BUFFER *buffer, UINT64 pos )
{
	UINT 	new_cls = pos / stream_buffer_chunk( buffer->s );
	UINT64 	new_pos = (UINT64)new_cls * stream_buffer_chunk( buffer->s );
	
	buffer->buf_write_cls = new_cls;
	buffer->buf_write_pos = new_pos;
	buffer->buf_file_pos  = new_pos;
	buffer->bite_file_pos = new_pos;
DBGH serprintf("stream_buffer_set_pos_blocked: pos %lld -> cls %d  pos %lld\r\n", pos, new_cls, new_pos );
	
	buffer->bite_pos     = 0;
	buffer->buf_write    = 0;
	buffer->buf_scan     = 0;
	buffer->buf_scan_pos = new_pos;
	buffer->buf_wrap     = 0;
	
	buffer->buf_tail     = buffer->buf_write;
	buffer->buf_end      = 0;

	stream_buffer_free_all_data( buffer, 0, 0 );

	// make sure the drive is awake
	buffer->state = BUFFER_RDWR;
	buffer->state_changed = 1;
		
	// read in some clusters until we fill the overlap or mindata size:
	while (!buffer->buf_end ) {
		if( buffer->buffer( buffer, 2 ) ) {
			break;
		}
		int head = stream_buffer_get_head( buffer );
DBGH serprintf("readSome: %8d   head  %8d\r\n", buffer->buf_write, head );
		if( head > buffer->s->parser_mindata_size ) {
			break;
		}
		if( buffer->buf_write >= buffer->overlap_size ) {
			break;
		}
	}

	return new_pos;	
}

// ************************************************************
//
//	__set_pos_non_blocked
//
// ************************************************************
static UINT64 __set_pos_non_blocked( STREAM_BUFFER *buffer, UINT64 pos )
{
	buffer->buf_write_pos = pos;
	buffer->buf_file_pos  = pos;
	buffer->bite_file_pos = pos;
DBGH serprintf("stream_buffer_set_pos_non_blocked: pos %lld\r\n", pos );
	
	buffer->bite_pos     = 0;
	buffer->buf_write    = 0;
	buffer->buf_scan     = 0;
	buffer->buf_scan_pos = pos;
	buffer->buf_wrap     = 0;
	
	buffer->buf_tail     = buffer->buf_write;
	buffer->buf_end      = 0;

	stream_buffer_free_all_data( buffer, 0, 0 );

	// make sure the drive is awake
	buffer->state = BUFFER_RDWR;
	buffer->state_changed = 1;
		
	// read in some clusters until we fill the overlap or mindata size:
	while (!buffer->buf_end ) {
		if( buffer->buffer( buffer, 2 ) ) {
			break;
		}
		int head = stream_buffer_get_head( buffer );
DBGH serprintf("readSome: %8d   head  %8d\r\n", buffer->buf_write, head );
		if( head > buffer->s->parser_mindata_size ) {
			break;
		}
		if( buffer->buf_write >= buffer->overlap_size ) {
			break;
		}
	}

	return pos;	
}

// ************************************************************
//
//	_set_pos
//
// ************************************************************
static int _set_pos( STREAM_BUFFER *buffer, UINT64 pos, int force_reload, int resize, int non_blocked )
{
	// inhibit buffering
	if( non_blocked ) {
		// in non_blocked case we can tell buffer to abort whatever he is doing
		buffer->abort = STREAM_BUFFER_ABORT_THIS;
		pthread_mutex_lock( &buffer->mutex );
		buffer->abort = STREAM_BUFFER_NO_ABORT;
	} else {
		// in blocked case we have to wait for the lock 
		pthread_mutex_lock( &buffer->mutex );
	}
	
	if( resize ) {
		force_reload = 1;
DBGH serprintf("stream_buffer_set_pos %lld  resize %d\r\n", pos, resize );	
		if( stream_buffer_resize( buffer, resize ) ) {
			pthread_mutex_unlock( &buffer->mutex );
			return 1;
		}
	}

	int   reload = 0;
	INT64 offset = pos - stream_buffer_get_pos( buffer );
DBGH serprintf("stream_buffer_set_pos %lld  bsp %lld  ofs %lld   force %d\r\n", pos, buffer->buf_scan_pos, offset, force_reload );	

	if ( !force_reload ) {
		if ( offset == 0 ) {
			// do nothing
		} else if( offset > 0 ) {
			//
			// we go forward
			//

			// how much offset in the buffer ?
			INT64 buf_offset = pos - stream_buffer_get_pos( buffer );
			// how much do we have in the buffer ?
			int used = stream_buffer_get_head( buffer );
DBGH serprintf(">> buf_ofs: %lld  used %d/%d  bsc %d  bwr %d\r\n", buf_offset, used, used - buffer->overlap_size, buffer->buf_scan, buffer->buf_write  );	
			used -= buffer->overlap_size;
			if( used < 0 )
				used = 0;
			// do we need more data to be buffered ?
			if ( buf_offset > used && !buffer->buf_end ) {
				// OK, we need more data before we can advance further!
				// as we have no idea how far we have to jump, we will
				// restart the whole buffering operation here !!!

				reload = 1;
			} else {
DBGH serprintf("inbuffer!\n" );	
				stream_buffer_skip_pos    ( buffer, pos - stream_buffer_get_pos( buffer ) );
				stream_buffer_skip_buf_pos( buffer, buf_offset );
DBGH serprintf(">> buf_rd: %d  bsc %d  bwr %d\r\n", buffer->buf_read, buffer->buf_scan, buffer->buf_write  );	
			}
		} else {
			//
			// we go backwards ...
			//

			// how much offset in the buffer ?
			INT64 buf_offset = stream_buffer_get_pos( buffer ) - pos;

			// how much is there behind us in the buffer ?		
			int tail = stream_buffer_get_tail( buffer );
DBGH serprintf("<< buf_ofs: %lld  tail %d  bsc %d  btl %d\r\n", buf_offset, tail, buffer->buf_scan, buffer->buf_tail  );	

			if ( buf_offset > tail ) {
				// OK, we need more data before we can go further back
				// as we have no idea how far we have to jump, we will
				// restart the whole buffering operation here !!!

				reload = 1;
			} else {	
DBGH serprintf("inbuffer!\n" );	
				_rewind_pos    ( buffer, stream_buffer_get_pos( buffer ) - pos ); 
				_rewind_buf_pos( buffer, buf_offset ); 
DBGH serprintf("<< buf_rd: %d  bsc %d  bwr %d\r\n", buffer->buf_read, buffer->buf_scan, buffer->buf_write  );	
			}
		}
	}
	
	// so that s->buf_read is updated
	stream_buffer_free_all_data( buffer, 0, 0 );
	
	int err = 0;
	if ( reload || force_reload ) {
		UINT64 new_pos;
DBGH serprintf("reload!\n" );	

		// so that s->buf_read is updated
		stream_buffer_free_all_data( buffer, 0, 0 );

		// advance to this position and read some data
		if( non_blocked ) {
			new_pos = __set_pos_non_blocked( buffer, pos );
		} else {
			new_pos = __set_pos_blocked( buffer, pos );
		}
DBGH serprintf("stream_buffer_set_pos: %lld -> %lld \r\n", pos, new_pos );

		if ( new_pos != -1 ) {
			// set to new position
			buffer->buf_scan_pos = new_pos;
			// now read at least once
			buffer->buffer( buffer, 0 );
		}
		// and goto real position on next chunk
		if ( pos < buffer->buf_scan_pos ) {
serprintf("_____AAARGH____%lld", pos );
serprintf("_____AAARGH____%lld", buffer->buf_scan_pos );
			((STREAM*)buffer->s)->error = 1;
		} 

		if( pos - buffer->buf_scan_pos > stream_buffer_get_head( buffer )) {
serprintf("_____AAARGH2____%lld - %lld > %d\r\n", pos, buffer->buf_scan_pos, stream_buffer_get_head( buffer ));
			err = 1;
			goto ErrorExit;
		}
		
		stream_buffer_skip( buffer, pos - buffer->buf_scan_pos );
	
DBGH serprintf("HEAD: %d\r\n", stream_buffer_get_head( buffer ) );
	
	}

ErrorExit:
	// allow buffering
	pthread_mutex_unlock( &buffer->mutex );

	return err;
}

static int _set_pos_blocked( STREAM_BUFFER *buffer, UINT64 pos, int force_reload, int resize )
{
	return _set_pos( buffer, pos, force_reload, resize, 0 );
}

static int _set_pos_non_blocked( STREAM_BUFFER *buffer, UINT64 pos, int force_reload, int resize )
{
	return _set_pos( buffer, pos, force_reload, resize, 1 );
}

// ************************************************************
//
//	_buffer_blocked
//
//	sleep:	0 - never sleep
//		1 - normal mode, read until buffer full, then sleep
//		2 - read as much as possible, never sleep
//		3 - read as much as possible, fake sleep
//
// ************************************************************
static int _buffer_blocked( STREAM_BUFFER *buffer, int sleep )
{
	// no more data to read ?
	if ( buffer->buf_write_pos >= buffer->data_end && !buffer->buf_end ) {
DBGH serprintf("___BUFFER_END____[%s] %lld >= %lld \r\n", buffer->tag, buffer->buf_write_pos, buffer->data_end);
		buffer->buf_end = 1;
	}	

	// all data in buffer, return
	if ( buffer->buf_end ) {
		if ( buffer->io->sleepable( buffer->io, NULL, NULL ) && sleep > 0 ) {
			if ( buffer->state != BUFFER_SLEEP ) {
DBGH serprintf("[%8d] %s gt END_WAIT_SLEEP", atime(), buffer->tag );
				stream_drive_sleep( buffer->s );
				stream_buffer_state_set( buffer, BUFFER_SLEEP );
			}
		}
		return 1;
	}

	int STREAM_BUFFER_CHUNK = stream_buffer_chunk( buffer->s );
	// determine free buffer space in clusters
	int free_cls = stream_buffer_get_free( buffer ) / STREAM_BUFFER_CHUNK;
	free_cls --;

	if( free_cls <= 0 ) {
		// no more space in buffer, return
		return 1;
	}

//DBGH serprintf("rd %d  wr %d  free %d\r\n", buffer->buf_read, buffer->buf_write, free_cls );					

	int read_now = 0;

	switch ( buffer->state ) {
		case BUFFER_RDWR:
			if ( free_cls > MIN_FREE_VIDEO || sleep == 2 ) {
				read_now = 1;
			} else {
				if( buffer->flags & STREAM_BUFFER_NO_SLEEP ) {
					// we do never sleep!
				} else if ( buffer->io->sleepable( buffer->io, NULL, NULL ) && sleep == 1 ) {
					buffer->stat_time = atime() - buffer->stat_time;
DBGH serprintf("\r\n[%8d] %s gt WAIT_SLEEP, free %d  awake %d  bytes %d  bytes/s %d\r\n", atime(), buffer->tag, free_cls, buffer->stat_time, buffer->stat_bytes, (UINT64)buffer->stat_bytes * 1000 / buffer->stat_time );
					buffer->stat_time = atime();
					stream_drive_sleep( buffer->s );
					stream_buffer_state_set( buffer, BUFFER_SLEEP );
//serprintf("wcl %d  wr %d  tl %d  rd %d  fr %d  str %d\n", avi_write_cls, buffer->buf_write, buffer->buf_tail, buffer->buf_read, free, SEC_ToRead );			
				}
			}
			break; 

		case BUFFER_SLEEP:
			if( free_cls <= MIN_FREE_VIDEO )
				break;
		
			if( stream_buffer_drive_needs_to_wake_up( buffer ) ) {	
				buffer->stat_time = atime() - buffer->stat_time;
DBGH serprintf("\r\n[%8d] %s gt READ, free %d  slept %d\r\n", atime(), buffer->tag, free_cls, buffer->stat_time );
				buffer->stat_time  = atime();
				buffer->stat_bytes = 0;
				stream_buffer_state_set( buffer, BUFFER_RDWR );
			}
			break;
	}	

	if ( read_now ) {
		UINT64 max_cls  = free_cls;
		UINT64 rest_cls = pad_to_buffer_chunk( buffer->s, buffer->data_end - buffer->buf_write_pos ) / STREAM_BUFFER_CHUNK; 
			
		// make sure we do not read over end of buffer
		if ( max_cls > ( ( buffer->buffer_size - buffer->buf_write ) / STREAM_BUFFER_CHUNK ) )
			max_cls = ( buffer->buffer_size - buffer->buf_write ) / STREAM_BUFFER_CHUNK;			
		
		// make sure we do not read over end of file
		if ( max_cls > rest_cls )
			max_cls = rest_cls;			
	
		if ( max_cls >= MAX_READ_CLUSTER )	// maximum number of clusters at a time
			max_cls = MAX_READ_CLUSTER;
			
//serprintf("max %lld  rest %lld\r\n", max_cls, rest_cls );
		int num_cls = 0;
		int write_cls = buffer->buf_write_cls;
		while( max_cls-- ) {
			num_cls ++;
			write_cls ++;
		}

		// do not read over end of file
		int to_read = MIN( num_cls * STREAM_BUFFER_CHUNK, buffer->data_end - buffer->buf_write_pos );
//serprintf("toread %d  num %d\r\n", to_read, num_cls );

		while( num_cls > 0 ){
			if( buffer->io->can_read( buffer->io, buffer->buf_write_pos, to_read )) {
				break;
			}

			num_cls--;
			to_read = MIN( num_cls * STREAM_BUFFER_CHUNK, buffer->data_end - buffer->buf_write_pos );
		}

		// io can not read anything --> come back later
		if( !num_cls ){
			msec_sleep(10);
			return 1;
		}

		// Read data from the hard drive (or memory card or whatever ....)
		buffer->io->seek( buffer->io, buffer->buf_write_pos );

		int err = 0;
		int ret;
		if( (ret = buffer->io->read( buffer->io, buffer->data + buffer->buf_write, to_read )) < 0 ) {
serprintf("stream_buffer: read err: %d\r\n", ret);			
			int abort = -1 * ret;
			if( abort == STREAM_BUFFER_ABORT_FINAL ) {
				// final abort, give up
				err = 1;
			} else {
				// just this operation, return
				return 1;
			}
		} else if( ret != to_read ) {
serprintf("stream_buffer: got only %d of %d -> at end! \r\n", ret, to_read );			
			// we did not get all data, we must be at the end
			buffer->buf_end = 1;
			
			// add only this many bytes!
			to_read = ret;
		}

DBGH2 {
serprintf("[%2lld%%] ", (UINT64)stream_buffer_get_used( buffer ) * 100 / (UINT64)buffer->buffer_size );
serprintf("%s  wcl %8d  wp %8lld  wr %8d  tl %8d  rd %8d  sc %8d/%8lld  fr %8d  nm %d\r\n", buffer->tag, buffer->buf_write_cls, 
	buffer->buf_write_pos,  buffer->buf_write,  buffer->buf_tail, buffer->buf_read, buffer->buf_scan, buffer->buf_scan_pos, free_cls, num_cls );			
}

		if ( err ) {
serprintf("stream_buffer: fail!\n");		
 			// Reading has failed !!!
			// playing a video can allow no
			// "skipped" data, so we have to "die" here !!!
				
			// signal no more reads!
			buffer->buf_end = 1;
			stream_set_error( buffer->s, VE_FILE_ERROR );
			return 1;
		}

		// Command has ENDed successfully!!!
			
		// copy start to overlap area !
		if ( buffer->buf_write < buffer->overlap_size ) {
			int copy_size = num_cls * STREAM_BUFFER_CHUNK;
			if ( ( buffer->buf_write + copy_size) > buffer->overlap_size )				
				copy_size = buffer->overlap_size - buffer->buf_write;
			memcpy( buffer->data + buffer->buffer_size + buffer->buf_write, buffer->data + buffer->buf_write, copy_size );	
DBGH2 serprintf("COPY: src %8d  dst %8d  siz %8d\r\n", buffer->buf_write, buffer->buffer_size + buffer->buf_write, copy_size );
		}

		// update counters and buffer pointers
		buffer->buf_write_cls += num_cls;
		buffer->buf_write_pos += to_read;
		buffer->buf_write     += to_read;
		buffer->stat_bytes    += to_read;
		
		if (buffer->buf_write == buffer->buffer_size) {
			if( buffer->flags & STREAM_BUFFER_NO_WRAP ) {
serprintf("__BUFFER_NO_WRAP__%d %lld\r\n", buffer->buf_scan, buffer->buf_scan_pos );
				// we are not allowed to wrap, in this case we have to clear the overlap area!
				memset( buffer->data + buffer->buffer_size, 0, buffer->overlap_size );
				buffer->buf_end = 1;
			} else {
serprintf("__BUFFER_WRAP__\r\n");
				buffer->buf_wrap  = 1;
				buffer->buf_write = 0;
			}
		}

		if ( buffer->buf_wrap ) {
			buffer->buf_tail = buffer->buf_write;
		}
		// we did read!
		return 0;
	}
	// we did not read!
	return 1;
}

// ************************************************************
//
//	_buffer_non_blocked
//
//	sleep:	0 - never sleep
//		1 - normal mode, read until buffer full, then sleep
//		2 - read as much as possible, never sleep
//		3 - read as much as possible, fake sleep
//
// ************************************************************
static int _buffer_non_blocked( STREAM_BUFFER *buffer, int sleep )
{
	// no more data to read ?
	if ( buffer->buf_write_pos >= buffer->data_end && !buffer->buf_end ) {
DBGH serprintf("___BUFFER_END____[%s] %lld >= %lld \r\n", buffer->tag, buffer->buf_write_pos, buffer->data_end);
		buffer->buf_end = 1;
	}	

	// all data in buffer, return
	if ( buffer->buf_end ) {
		if ( buffer->io->sleepable( buffer->io, NULL, NULL ) && sleep > 0 ) {
			if ( buffer->state != BUFFER_SLEEP ) {
DBGH serprintf("[%8d] %s gt END_WAIT_SLEEP", atime(), buffer->tag );
				stream_drive_sleep( buffer->s );
				stream_buffer_state_set( buffer, BUFFER_SLEEP );
			}
		}
		return 1;
	}

	int STREAM_BUFFER_CHUNK = stream_buffer_chunk( buffer->s );
	// determine free buffer space in bytes
	int free = stream_buffer_get_free( buffer );
	free -= STREAM_BUFFER_CHUNK;
	
	if( free <= 0 ) {
		// no more space in buffer, return
		return 1;
	}

//DBGH serprintf("rd %d  wr %d  free %d\r\n", buffer->buf_read, buffer->buf_write, free );					

	int read_now = 0;

	switch ( buffer->state ) {
		case BUFFER_RDWR:
			if ( free > STREAM_BUFFER_CHUNK || sleep == 2 ) {
				read_now = 1;
			} else {
				if( buffer->flags & STREAM_BUFFER_NO_SLEEP ) {
					// we do never sleep!
				} else if ( buffer->io->sleepable( buffer->io, NULL, NULL ) && sleep == 1 ) {
DBGH serprintf("\r\n[%8d] %s gt WAIT_SLEEP, free %d\r\n", atime(), buffer->tag, free );
					stream_drive_sleep( buffer->s );
					stream_buffer_state_set( buffer, BUFFER_SLEEP );
//serprintf("wcl %d  wr %d  tl %d  rd %d  fr %d  str %d\n", avi_write_cls, buffer->buf_write, buffer->buf_tail, buffer->buf_read, free, SEC_ToRead );			
				}
			}
			break; 

		case BUFFER_SLEEP:
			if( free <= STREAM_BUFFER_CHUNK )
				break;
		
			if( stream_buffer_drive_needs_to_wake_up( buffer ) ) {	
DBGH serprintf("\r\n[%8d] %s gt READ, free: %d\r\n", atime(), buffer->tag, free );
				stream_buffer_state_set( buffer, BUFFER_RDWR );
			}
			break;
	}	

	if ( read_now ) {
		int to_read = free;
		
		// do not read more than one chunk
		to_read = MIN( to_read, STREAM_BUFFER_CHUNK );
		
		// make sure we do not read over end of buffer
		to_read = MIN( to_read, buffer->buffer_size - buffer->buf_write );
		
		// make sure we do not read over end of file
		to_read = MIN( to_read, buffer->data_end - buffer->buf_write_pos ); 
			
		// io can not read anything --> come back later
		if( !buffer->io->can_read( buffer->io, buffer->buf_write_pos, to_read )) {
			to_read = 0;
		}

		if( !to_read ){
			msec_sleep(10);
			return 1;
		}

		// Read data from the hard drive (or memory card or whatever ....)
		int err = 0;
		buffer->io->seek( buffer->io, buffer->buf_write_pos );
		int bytes = buffer->io->read( buffer->io, buffer->data + buffer->buf_write, to_read );
		if( bytes < 0 ) {
serprintf("stream_buffer: read err: %d\r\n", bytes);			
			int abort = -1 * bytes;
			if( abort == STREAM_BUFFER_ABORT_FINAL ) {
				// final abort, give up
				err = 1;
			} else {
				// just this operation, return
				return 1;
			}
		}

DBGH2 {
serprintf("[%2d%%] ", stream_buffer_get_used( buffer ) * 100 / buffer->buffer_size );
serprintf("%s  wp %8lld  wr %8d  tl %8d  rd %8d  sc %8d/%8lld  fr %8d  rd %8d/%8d\r\n", buffer->tag, buffer->buf_write_pos, buffer->buf_write, buffer->buf_tail,
						 buffer->buf_read, buffer->buf_scan, buffer->buf_scan_pos, free, to_read, bytes );			
}

		if ( err ) {
serprintf("stream_buffer: fail!\n");		
 			// Reading has failed !!!
			// playing a video can allow no
			// "skipped" data, so we have to "die" here !!!
				
			// signal no more reads!
			buffer->buf_end = 1;
			stream_set_error( buffer->s, VE_FILE_ERROR );
			return 1;
		}

		// Command has ENDed successfully!!!
			
		// copy start to overlap area !
		if ( buffer->buf_write < buffer->overlap_size ) {
			int copy = MIN( bytes, buffer->overlap_size - buffer->buf_write );
			memcpy( buffer->data + buffer->buffer_size + buffer->buf_write, buffer->data + buffer->buf_write, copy );	
DBGH2 serprintf("COPY: src %8d  dst %8d  cpy %8d\r\n", buffer->buf_write, buffer->buffer_size + buffer->buf_write, copy );
		}

		// update counters and buffer pointers
		buffer->buf_write_pos += bytes;
		buffer->buf_write     += bytes;
		
		if (buffer->buf_write == buffer->buffer_size) {
			if( buffer->flags & STREAM_BUFFER_NO_WRAP ) {
serprintf("__BUFFER_NO_WRAP__%d %lld\r\n", buffer->buf_scan, buffer->buf_scan_pos );
				// we are not allowed to wrap, in this case we have to clear the overlap area!
				memset( buffer->data + buffer->buffer_size, 0, buffer->overlap_size );
				buffer->buf_end = 1;
			} else {
serprintf("__BUFFER_WRAP__\r\n");
				buffer->buf_wrap  = 1;
				buffer->buf_write = 0;
			}
		}

		if ( buffer->buf_wrap ) {
			buffer->buf_tail = buffer->buf_write;
		}

		// are we at the end of the io?
		if( buffer->buf_write_pos >= stream_io_get_size( buffer->io ) ) {
serprintf("stream_buffer: end!\r\n");		
			buffer->buf_end = 1;
		}

		// we did read!
		return 0;
	}
	// we did not read!
	return 1;
}

// ************************************************************
//
//	_delete
//
// ***********************************************************
static int _delete( STREAM_BUFFER *buffer )
{
	if( buffer ) 
		afree( buffer ); 
	return 0;
}

extern int stream_buffer_time;

// ************************************************************
//
//	new_stream_buffer_raw_blocked
//
// ***********************************************************
STREAM_BUFFER *new_stream_buffer_raw_blocked( void ) 
{
DBGS serprintf("new_stream_buffer_raw_blocked\r\n" );
	STREAM_BUFFER *buffer = (STREAM_BUFFER *)amalloc( sizeof( STREAM_BUFFER ) );
	
	if( !buffer )
		return NULL;
	memset( buffer, 0, sizeof( STREAM_BUFFER ) );
	
	buffer->close   = stream_buffer_close;
	buffer->delete  = _delete;
	buffer->buffer  = _buffer_blocked;
	buffer->set_pos = _set_pos_blocked;

	buffer->sleep_time = stream_buffer_time;
	return buffer;	 
}

STREAM_BUFFER *new_stream_buffer_raw( void ) 
{
	return new_stream_buffer_raw_blocked();
}

// ************************************************************
//
//	new_stream_buffer_raw_non_blocked
//
// ***********************************************************
STREAM_BUFFER *new_stream_buffer_raw_non_blocked( void ) 
{
DBGS serprintf("new_stream_buffer_raw_non_blocked\r\n" );
	STREAM_BUFFER *buffer = (STREAM_BUFFER *)amalloc( sizeof( STREAM_BUFFER ) );
	
	if( !buffer )
		return NULL;
	memset( buffer, 0, sizeof( STREAM_BUFFER ) );
	
	buffer->close   = stream_buffer_close;
	buffer->delete  = _delete;
	buffer->buffer  = _buffer_non_blocked;
	buffer->set_pos = _set_pos_non_blocked;

	buffer->sleep_time = stream_buffer_time;
	return buffer;	 
}


#endif
