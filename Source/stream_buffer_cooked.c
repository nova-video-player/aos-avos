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

#include <errno.h>
#include <string.h>

#ifdef CONFIG_STREAM

#define DBGV  if(Debug[DBG_VID])
#define DBGH  if(Debug[DBG_HD])
#define DBGH2 if(Debug[DBG_HD] & 4)
#define DBGS  if(Debug[DBG_STREAM])

int stream_buffer_drive_needs_to_wake_up( STREAM_BUFFER *buffer );
int stream_buffer_close( STREAM_BUFFER *buffer );

// ************************************************************
//
//	_default_cook
//
// ************************************************************
static int _default_cook( void *ctx, UINT64 in_pos, const UCHAR *in_data, int *in_size, UCHAR **out_data, int *out_size ) 
{
	*out_data = (UCHAR*)in_data;
	*out_size = *in_size;
	return 0;
}

#define MIN_FREE 	131072	// stop buffering at this number of bytes empty in buffer
#define DEFAULT_BITE	131072

// ************************************************************
//
//	_buffer
//
//	sleep:	0 - never sleep
//		1 - normal mode, read until buffer full, then sleep
//		2 - read as much as possible, never sleep
//		3 - read as much as possible, fake sleep
//
// ************************************************************
static int _buffer( STREAM_BUFFER *buffer, int sleep )
{
	// no more data to read ?
	if ( buffer->buf_file_pos >= buffer->data_end && !buffer->buf_end ) {
DBGH serprintf("___BUFFER_END____[%s] %lld >= %lld \r\n", buffer->tag, buffer->buf_file_pos, buffer->data_end);
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

	// determine free buffer space in clusters
	int free = stream_buffer_get_free( buffer ) - 1;
	
	if( free <= buffer->bite_size ) {
		// no more space in buffer, return
		return 1;
	}

	int read_now = 0;

	switch ( buffer->state ) {
		case BUFFER_RDWR:
			if ( free > MIN_FREE || sleep == 2 ) {
				read_now = 1;
			} else {
				if( buffer->flags & STREAM_BUFFER_NO_SLEEP ) {
					// we do never sleep!
				} else if ( buffer->io->sleepable( buffer->io, NULL, NULL ) && sleep == 1 ) {
DBGH serprintf("\r\n[%8d] %s gt WAIT_SLEEP, free %8d\r\n", atime(), buffer->tag, free );
					stream_drive_sleep( buffer->s );
					stream_buffer_state_set( buffer, BUFFER_SLEEP );
//serprintf("wcl %d  wr %d  tl %d  rd %d  fr %d  str %d\n", avi_write_cls, buffer->buf_write, buffer->buf_tail, buffer->buf_read, free, SEC_ToRead );			
				}
			}
			break; 

		case BUFFER_SLEEP:

			if( free <= MIN_FREE )
				break;
			
			if( stream_buffer_drive_needs_to_wake_up( buffer ) ) {
DBGH serprintf("\r\n[%8d] %s gt READ, free: %8d\r\n", atime(), buffer->tag, free );
				stream_buffer_state_set( buffer, BUFFER_RDWR );
			}
			break;
	}	

	if ( read_now ) {
		if( !buffer->cook ) {
serprintf("default cook!\r\n");			
			stream_buffer_set_cook( buffer, _default_cook, NULL, DEFAULT_BITE );
		}
		
		UINT64 to_read = MIN( free, buffer->bite_size - buffer->bite_pos );
		
		// do not read over end of file:
		to_read = MIN( to_read, buffer->data_end - buffer->buf_file_pos );
		
		// make sure the io is ready
		if( !buffer->io->can_read( buffer->io, buffer->buf_file_pos, to_read )){
			// file is not downloaded yet => come back later
			msec_sleep( 10 );
			return 1;
		}

		// Read data
		buffer->io->seek( buffer->io, buffer->buf_file_pos );

		int err = 0;
		int ret;
		if( (ret = buffer->io->read( buffer->io, buffer->bite + buffer->bite_pos, to_read )) <= 0 ) {
serprintf("READ ERR: %d\r\n", ret);			
			err = 1;
		}

DBGH2 {
serprintf("%s  fp %9lld  wp %9lld  wr %8d  tl %8d  rd %8d  sc %8d/%8lld  fr %8d  rd %8d\r\n", buffer->tag, buffer->buf_file_pos, buffer->buf_write_pos, 
		buffer->buf_write, buffer->buf_tail, buffer->buf_read, buffer->buf_scan, buffer->buf_scan_pos, free, to_read );			
}

		if ( err ) {
serprintf("SBC_ERR: %s  fp %8lld  wp %8lld  wr %8d  tl %8d  rd %8d  sc %8d/%8lld  fr %8d  rd %8d\r\n", buffer->tag, buffer->buf_file_pos, buffer->buf_write_pos, 
		buffer->buf_write, buffer->buf_tail, buffer->buf_read, buffer->buf_scan, buffer->buf_scan_pos, free, to_read );			
 			// Reading has failed - playing a video can allow no skipped" data, so we have to "die" here !!!
			buffer->buf_end = 1;
			stream_set_error( buffer->s, VE_FILE_ERROR );
			return 1;
		}

		buffer->bite_pos += to_read;
		
		int in_size = buffer->bite_pos;
		UCHAR *cooked;
		int cooked_size;
		buffer->cook( buffer->cook_ctx, buffer->bite_file_pos, buffer->bite, &in_size, &cooked, &cooked_size );

		stream_buffer_write( buffer, cooked, cooked_size );
			
		memmove( buffer->bite, buffer->bite + in_size, buffer->bite_size - in_size );
		
		buffer->bite_pos      -= in_size;
		buffer->bite_file_pos += in_size;
		
		buffer->buf_file_pos += to_read;
			
		// we did read!
		return 0;
	}
	// we did not read!
	return 1;
}

// ************************************************************
//
//	stream_buffer_set_cook
//
// ***********************************************************
int stream_buffer_set_cook( STREAM_BUFFER *buffer, STREAM_BUFFER_COOK cook, void *cook_ctx, int bite_size )
{
	if( !buffer)
		return 1;
	buffer->cook      = cook;
	buffer->cook_ctx  = cook_ctx;
	buffer->bite_size = bite_size;
	buffer->bite_pos  = 0;
	if( !(buffer->bite = amalloc( bite_size ) ) ) {
		return 1;
	}
	return 0;
}

// ************************************************************
//
//	_set_pos
//
// ************************************************************
int _set_pos( STREAM_BUFFER *buffer, UINT64 pos, int force_reload, int resize )
{
	if( resize ) {
		return 1;
	}	
	// inhibit buffering
	pthread_mutex_lock( &buffer->mutex );

	buffer->buf_write_pos = pos;
	buffer->buf_file_pos  = pos;
	buffer->bite_file_pos = pos;
serprintf("_set_pos_cooked: pos %lld\r\n", pos );
	
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
		
	// read in some clusters until we fill the overlap:
	while ( !buffer->buf_end ) {
//DBGV serprintf("readSome: %d\r\n", buffer->buf_write);
		if( buffer->buffer( buffer, 2 ) ) {
			break;
		}
		if( buffer->buf_write >= buffer->overlap_size ) {
			break;
		}
	}

	// allow buffering
	pthread_mutex_unlock( &buffer->mutex );

	return 0;	
}

// ************************************************************
//
//	_delete
//
// ***********************************************************
static int _delete( STREAM_BUFFER *buffer )
{
	if( buffer ) {
		afree( buffer ); 
	}
	return 0;
}

// ************************************************************
//
//	_close
//
// ***********************************************************
static int _close( STREAM_BUFFER *buffer )
{
	if( buffer ) {
		stream_buffer_close( buffer );
		if( buffer->bite )
			afree( buffer->bite );
		buffer->bite = NULL;
	}
	return 0;
}

// ************************************************************
//
//	new_stream_buffer_cooked
//
// ***********************************************************
STREAM_BUFFER *new_stream_buffer_cooked( void ) 
{
	STREAM_BUFFER *buffer = (STREAM_BUFFER *)amalloc( sizeof( STREAM_BUFFER ) );
	
	if( !buffer )
		return NULL;
	memset( buffer, 0, sizeof( STREAM_BUFFER ) );
	
	buffer->close   = _close;
	buffer->delete  = _delete;
	buffer->buffer  = _buffer;
	buffer->set_pos = _set_pos;

	buffer->sleep_time = 1;

	return buffer;	 
}
#endif
