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

#ifndef _CB_H_
#define _CB_H_

#include "astdlib.h"
#include <string.h>

// *********************************************************************
// create, destroy
// *********************************************************************
typedef struct CIRCULAR_BUFFER 
{
	unsigned char 	*buffer;
	unsigned int 	size;
	unsigned int	write;
	unsigned int	read;
	unsigned int	end;
	int		dma;
} CIRCULAR_BUFFER;

static inline void cb_flush( CIRCULAR_BUFFER *cb )
{
	cb->read  = 0;
	cb->write = 0;
	cb->end   = 0;
}

static inline int cb_malloc( CIRCULAR_BUFFER *cb, int size )
{
	if (!cb || !size )
		return 1;
	
	cb->size = size;
	if( !(cb->buffer = (unsigned char*)amalloc( cb->size ) ) ) {
		cb->size = 0;
		return 1;
	}
	cb->dma = 0;
	cb_flush( cb );
	
	return 0;
}

static inline int cb_malloc_dma( CIRCULAR_BUFFER *cb, int size )
{
	if (!cb || !size )
		return 1;
	
	cb->size = size;
	if( !(cb->buffer = (unsigned char*)amalloc_dma( cb->size ) ) ) {
		cb->size = 0;
		return 1;
	}
	cb->dma = 1;
	cb_flush( cb );
	
	return 0;
}

static inline void cb_free( CIRCULAR_BUFFER *cb )
{
	if( cb ) {
		if( cb->buffer ) {
			if( cb->dma )
				afree_dma( cb->buffer, cb->size );
			else
				afree( cb->buffer );
		}	
		cb->buffer = NULL;
	}
}

// *********************************************************************
// stats of the circular buffer
// *********************************************************************
static inline int cb_get_used( CIRCULAR_BUFFER *cb )
{
	int used = cb->write - cb->read;
	if( used < 0 )
		used += cb->size;
		
	return used;
}

static inline int cb_get_end( CIRCULAR_BUFFER *cb )
{
	return cb->end;
			
}

static inline int cb_get_used_continuous( CIRCULAR_BUFFER *cb )
{
	if ( cb->read <= cb->write )
		return cb->write - cb->read;

	return cb->size - cb->read;
}

static inline int cb_get_free( CIRCULAR_BUFFER *cb )
{
	int free = cb->read - cb->write;
	if( free <= 0 )
		free += cb->size;

	// we cannot fill the buffer to the end!
	free -= 1;
	if( free <0 )
		free = 0;
			
	return free;
}

static inline int cb_get_free_continuous( CIRCULAR_BUFFER *cb )
{
	int free;

	if ( cb->read <= cb->write ){
		free = cb->size - cb->write;

		if( cb->read == 0 ){
//serprintf("cannot fill full buffer\r\n");
			free -= 1;
		}
	} else {
		free = cb->read - cb->write - 1;
	}

	if( free < 0 )
		free = 0;

	return free;
}

// *********************************************************************
// read operations to the circular buffer
// *********************************************************************
static inline unsigned char * cb_get_read_pointer( CIRCULAR_BUFFER *cb )
{
	return cb->buffer + cb->read;
}

static inline int cb_read( CIRCULAR_BUFFER *cb, unsigned char *buffer, int count )
{
	int rest = cb->size - cb->read;
	
	if( cb_get_used( cb ) < count )
		return 1;
	
	if( rest >= count ) {
		memcpy( buffer, cb->buffer + cb->read, count );
		cb->read += count;
		if( cb->read >= cb->size )
			cb->read -= cb->size;
	} else {
		memcpy( buffer,        cb->buffer + cb->read, rest );
		memcpy( buffer + rest, cb->buffer,            count - rest );
		cb->read = count - rest;
	}	
	return 0;
}

static inline unsigned char cb_peek( CIRCULAR_BUFFER *cb, int offset )
{
	int read = cb->read + offset;
	
	if( read >= cb->size )
		read -= cb->size;

//serprintf("[%02X]", cb->buffer[read] );
	return cb->buffer[read];
}

static inline int cb_skip_read( CIRCULAR_BUFFER *cb, int count )
{
//serprintf("skip_read  %d- used %8d   [R %8d][W %8d]\r\n", count, cb_get_used( cb ), cb->read, cb->write);

	if( cb_get_used( cb ) < count )
		return 1;

	cb->read += count;
	if( cb->read >= cb->size )
		cb->read -= cb->size;

	return 0;
}

// *********************************************************************
// write operations to the circular buffer
// *********************************************************************
static inline unsigned char * cb_get_write_pointer( CIRCULAR_BUFFER *cb )
{
	return cb->buffer + cb->write;
}

static inline int cb_write( CIRCULAR_BUFFER *cb, const unsigned char *buffer, int count )
{
	int rest = cb->size - cb->write;
	
	if( cb_get_free( cb ) < count )
		return 1;
	
	if( rest >= count ) {
		memcpy( cb->buffer + cb->write, buffer, count );
		cb->write += count;
		if( cb->write >= cb->size )
			cb->write -= cb->size;
	} else {
		memcpy( cb->buffer + cb->write, buffer,        rest );
		memcpy( cb->buffer,             buffer + rest, count - rest );
		cb->write = count - rest;
	}	
	return 0;
}

static inline int cb_skip_write( CIRCULAR_BUFFER *cb, int count )
{
//serprintf("skip_write %d- free %8d   [R %8d][W %8d]\r\n", count, cb_get_free( cb ), cb->read, cb->write);

	if( cb_get_free( cb ) < count )
		return 1;
	
	cb->write += count;
	if( cb->write >= cb->size )
		cb->write -= cb->size;
	
	return 0;
}

static inline void cb_set_end( CIRCULAR_BUFFER *cb )
{
	cb->end = 1;
}


#endif
