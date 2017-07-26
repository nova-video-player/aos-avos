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

#ifndef _PIPE_H_
#define _PIPE_H_

#include "astdlib.h"
#include "debug.h"
#include "util.h"
#include <string.h>
#include "atime.h"

// *********************************************************************
// create, destroy
// *********************************************************************
typedef struct PIPE 
{
	unsigned char 		*buffer;
	unsigned int 		size;
	volatile unsigned int	write;
	volatile unsigned int	read;
	volatile unsigned int	end;
	
	unsigned int		total_write;
	unsigned int		total_read;

} PIPE;

static inline void pipe_flush( PIPE *pipe )
{
	pipe->read  = 0;
	pipe->write = 0;
	pipe->end   = 0;
	pipe->total_read  = 0;
	pipe->total_write = 0;
}

static inline PIPE *pipe_new( int size )
{
	PIPE *pipe;
	
	if( !size || !(pipe = amalloc( sizeof( PIPE ) ) ) ) {
		return NULL;
	}
	
	pipe->size = size;
	if( !(pipe->buffer = (unsigned char*)amalloc( pipe->size ) ) ) {
		afree( pipe );
		return NULL;
	}
	pipe_flush( pipe );
	
	return pipe;
}

static inline void pipe_delete( PIPE **pipe )
{
	if( pipe && *pipe ) {
		afree( (*pipe)->buffer );
		afree( *pipe );
		*pipe = NULL;
	}
}

// *********************************************************************
// stats of the circular buffer
// *********************************************************************
static inline int pipe_get_used( PIPE *pipe )
{
	int used = pipe->write - pipe->read;
	if( used < 0 )
		used += pipe->size;
		
	return used;
}

static inline int pipe_get_end( PIPE *pipe )
{
	return pipe->end;
			
}

static inline int pipe_get_used_continous( PIPE *pipe )
{
	if ( pipe->read <= pipe->write )
		return pipe->write - pipe->read;

	return pipe->size - pipe->read;
}

static inline int pipe_get_free( PIPE *pipe )
{
	int free = pipe->read - pipe->write;
	if( free <= 0 )
		free += pipe->size;
		
	return free;
}

static inline int pipe_get_free_continous( PIPE *pipe )
{
	if ( pipe->read <= pipe->write )
		return pipe->size - pipe->write;

	return pipe->read - pipe->write;
}

static inline int pipe_get_stats( PIPE *pipe, unsigned int *total_write, unsigned int *total_read )
{
	if( total_write )
		*total_write = pipe->total_write;
	if( total_read )
		*total_read = pipe->total_read;
	return 0;
}

// *********************************************************************
// read operations to the pipe
// *********************************************************************
static inline unsigned char *pipe_get_read_pointer( PIPE *pipe )
{
	return pipe->buffer + pipe->read;
}

static inline int _pipe_read( PIPE *pipe, unsigned char *buffer, int count )
{
	int rest = pipe->size - pipe->read; 

	if( rest >= count ) {
		if( buffer )
			memcpy( buffer, pipe->buffer + pipe->read, count );
		pipe->read += count;
		if( pipe->read >= pipe->size )
				pipe->read -= pipe->size;
	} else {
		if( buffer ) {
			memcpy( buffer,        pipe->buffer + pipe->read, rest );
			memcpy( buffer + rest, pipe->buffer,              count - rest );
		}
		pipe->read = count - rest;
	}	
	pipe->total_read += count;
	return count;
}

static inline int pipe_read( PIPE *pipe, unsigned char *buffer, int count )
{
	if( pipe_get_used( pipe ) < count )
		return 0;
	
	return _pipe_read( pipe, buffer, count );
}

static inline int pipe_read_wait( PIPE *pipe, unsigned char *buffer, int count )
{
	int used;
	
	while( (used = pipe_get_used( pipe )) < count && !pipe_get_end( pipe ) ) {
		msec_sleep( 1 );
	}
	count = MIN( count, used );
	if( count ) {
		return _pipe_read( pipe, buffer, count );
	}
	return count;
}

// *********************************************************************
// write operations to the pipe
// *********************************************************************
static inline unsigned char *pipe_get_write_pointer( PIPE *pipe )
{
	return pipe->buffer + pipe->write;
}

static int _pipe_write( PIPE *pipe, unsigned char *buffer, int count )
{
	int rest = pipe->size - pipe->write;

	if( rest >= count ) {
		if( buffer )
			memcpy( pipe->buffer + pipe->write, buffer, count );
			
		pipe->write += count;
		if( pipe->write >= pipe->size )
			pipe->write -= pipe->size;
	} else {
		if( buffer ) {
			memcpy( pipe->buffer + pipe->write, buffer,        rest );
			memcpy( pipe->buffer,             buffer + rest, count - rest );
		}
		pipe->write = count - rest;
	}	
	pipe->total_write += count;
	return count;
}

static inline int pipe_write( PIPE *pipe, unsigned char *buffer, int count )
{
	if( pipe_get_free( pipe ) <= count )
		return 0;
	
	return _pipe_write( pipe, buffer, count);
}

static inline int pipe_write_wait( PIPE *pipe, unsigned char *buffer, int count )
{
	int free; 
	
	while( (free = pipe_get_free( pipe )) <= count ) {
		msec_sleep( 1 );
	}
	
	count = MIN( count, free );
	if( free ) {
		return _pipe_write( pipe, buffer, count );
	}
	
	return 0;
}

static inline void pipe_set_end( PIPE *pipe )
{
	pipe->end = 1;
}
#endif
