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

#ifndef _STREAM_SOURCE_H
#define _STREAM_SOURCE_H

#include "astdlib.h"
#include "global.h"
#include "stream_common.h"
#include "stream_error.h"
#include "util.h"

#define STREAM_MAX_PATH_LEN 512

typedef struct {
	char 		*url;
	char		name[STREAM_MAX_PATH_LEN + 1];
	char		**extra_list; /* NULL terminated HTTP header key/value pairs */
} STREAM_URL;

#define STREAM_URL_INITIALIZER { NULL, { 0 }, NULL }

struct STREAM_IO;

//
// STREAM_IO
//
typedef int (*STREAM_IO_DELETE)   ( struct STREAM_IO *src );
typedef int (*STREAM_IO_OPEN)     ( struct STREAM_IO *src, int mode );
typedef int (*STREAM_IO_IS_OPEN)  ( struct STREAM_IO *src );
typedef int (*STREAM_IO_CLOSE)    ( struct STREAM_IO *src );
typedef UINT64 (*STREAM_IO_GET_SIZE) ( struct STREAM_IO *src );
typedef UINT64 (*STREAM_IO_SEEK)  ( struct STREAM_IO *src, UINT64 pos );
typedef int (*STREAM_IO_READ)     ( struct STREAM_IO *src, UCHAR *buffer, UINT count );
typedef int (*STREAM_IO_WRITE)    ( struct STREAM_IO *src, UCHAR *buffer, UINT count );
typedef int (*STREAM_IO_PWRSTATE) ( struct STREAM_IO *src );
typedef int (*STREAM_IO_SEEKABLE) ( struct STREAM_IO *src );
typedef int (*STREAM_IO_SLEEPABLE)( struct STREAM_IO *src, int *sleep_time, int *wake_time );
typedef int (*STREAM_IO_CAN_READ) ( struct STREAM_IO *src, UINT64 pos, UINT64 count );
typedef void*(*STREAM_IO_GET_PRIV)( struct STREAM_IO *src, int *size );
typedef void*(*STREAM_IO_MMAP)    ( struct STREAM_IO *src, UINT64 size, UINT64 offset );
typedef int  (*STREAM_IO_MUNMAP)  ( struct STREAM_IO *src, void *buf, UINT64 size );

typedef int (*STREAM_IO_SETUP)( void *ctx, void *param );
typedef int (*STREAM_IO_META) ( void *ctx, void *param, int len );

typedef struct STREAM_IO {
	// methods
#ifdef __cplusplus
	STREAM_IO_DELETE	_delete;
#else
	STREAM_IO_DELETE	delete;
#endif
	STREAM_IO_OPEN  	open;
	STREAM_IO_IS_OPEN  	is_open;
	STREAM_IO_CLOSE  	close;
	STREAM_IO_GET_SIZE  	get_size;
	STREAM_IO_SEEK  	seek;
	STREAM_IO_READ 		read;
	STREAM_IO_WRITE		write;
	STREAM_IO_PWRSTATE  	power_state;
	STREAM_IO_SEEKABLE	seekable;
	STREAM_IO_SLEEPABLE 	sleepable;
	STREAM_IO_CAN_READ	can_read;
	STREAM_IO_GET_PRIV	get_priv;
	STREAM_IO_MMAP		mmap;
	STREAM_IO_MUNMAP	munmap;
	
	// members
	void		*priv;
	int		_is_open;
	
	STREAM_URL	src;
	
	UINT64		pos;
	UINT64		size;
	int		can_abort;	// the io might sometimes deliver less that what was asked	

	int 		num_parts;
	STREAM_PART 	*parts;
	int		now_part;
	UINT64		now_start;
	int		now_size;
	int		chunk_size;
	
	STREAM_GET_PART_NAME get_part_name;
	
	STREAM_IO_SETUP	setup;	
	void		*setup_ctx;

	STREAM_IO_META	meta;	
	void		*meta_ctx;
	
	ABORT_HANDLER 	abort;
	void		*abort_ctx;
	
} STREAM_IO;

// common methods
static inline void stream_url_free_extra_list( char **extra_list )
{
	int i;
	if( !extra_list )
		return;
	for( i = 0; extra_list[i]; i++ )
		afree( extra_list[i] );
	afree( extra_list );
}

static inline void stream_url_clear( STREAM_URL *src )
{
	if( !src )
		return;
	afree( src->url );
	stream_url_free_extra_list( src->extra_list );
	src->url = NULL;
	src->name[0] = '\0';
	src->extra_list = NULL;
}

static inline char **stream_url_dup_extra_list( char **extra_list )
{
	char **copy;
	int count = 0;
	int i;

	if( !extra_list )
		return NULL;
	while( extra_list[count] )
		count++;
	copy = (char **)acalloc( count + 1, sizeof(char *) );
	if( !copy )
		return NULL;
	for( i = 0; i < count; i++ ) {
		copy[i] = astrdup( extra_list[i] );
		if( !copy[i] ) {
			stream_url_free_extra_list( copy );
			return NULL;
		}
	}
	return copy;
}

static inline char **stream_url_dup_headers( const char **keys, const char **values )
{
	char **headers;
	int count = 0;
	int i;

	if( !keys || !values )
		return NULL;
	while( keys[count] && values[count] )
		count++;
	if( !count )
		return NULL;
	headers = (char **)acalloc( count * 2 + 1, sizeof(char *) );
	if( !headers )
		return NULL;
	for( i = 0; i < count; i++ ) {
		headers[i * 2] = astrdup( keys[i] );
		headers[i * 2 + 1] = astrdup( values[i] );
		if( !headers[i * 2] || !headers[i * 2 + 1] ) {
			stream_url_free_extra_list( headers );
			return NULL;
		}
	}
	return headers;
}

static inline int stream_url_set( STREAM_URL *dst, const char *url, const char *name, char **extra_list )
{
	char *url_copy;
	char **extra_copy = NULL;

	if( !dst )
		return 1;
	url_copy = astrdup( url ? url : "" );
	if( !url_copy )
		return 1;
	if( extra_list ) {
		extra_copy = stream_url_dup_extra_list( extra_list );
		if( !extra_copy ) {
			afree( url_copy );
			return 1;
		}
	}
	stream_url_clear( dst );
	dst->url = url_copy;
	if( name )
		strnZcpy( dst->name, name, STREAM_MAX_PATH_LEN );
	dst->extra_list = extra_copy;
	return 0;
}

static inline int stream_url_cpy_url( STREAM_URL *dst, const char *url )
{
	return stream_url_set( dst, url, NULL, NULL );
}

static inline int stream_url_cpy_url_name( STREAM_URL *dst, const char *url, const char *name )
{
	return stream_url_set( dst, url, name, NULL );
}

static inline int stream_url_cpy_url_name_headers( STREAM_URL *dst, const char *url, const char *name,
		const char **keys, const char **values )
{
	char **headers = stream_url_dup_headers( keys, values );
	int ret;

	if( keys && values && keys[0] && values[0] && !headers )
		return 1;
	ret = stream_url_set( dst, url, name, headers );
	stream_url_free_extra_list( headers );
	return ret;
}

static inline int stream_url_cpy( STREAM_URL *dst, STREAM_URL *src )
{
	if( !src )
		return stream_url_set( dst, "", NULL, NULL );
	return stream_url_set( dst, src->url, src->name[0] ? src->name : NULL, src->extra_list );
}

static inline STREAM_IO *stream_io_new( STREAM_URL *src )
{
	STREAM_IO *io = (STREAM_IO *)amalloc( sizeof( STREAM_IO ) );

	if( !io )
		 return NULL;
	memset( io, 0, sizeof( STREAM_IO ) );
	if( stream_url_cpy( &io->src, src ) ) {
		afree( io );
		return NULL;
	}
	return io;
}

static inline UINT64 stream_io_get_size( STREAM_IO *io )
{
	return io->size;
}

static inline UINT64 stream_io_can_abort( STREAM_IO *io )
{
	return io->can_abort;
}

static inline void stream_io_set_parts( STREAM_IO *io, int num_parts, STREAM_PART *parts, STREAM_GET_PART_NAME get_part_name )
{
	if( !io )
		return;

	io->num_parts     = num_parts;
	io->parts         = parts;
	io->get_part_name = get_part_name;
}

static inline void stream_io_set_setup( STREAM_IO *io, STREAM_IO_SETUP setup, void *ctx )
{
	if( !io )
		return;

	io->setup     = setup;
	io->setup_ctx = ctx;
}

static inline void stream_io_set_meta( STREAM_IO *io, STREAM_IO_META meta, void *ctx )
{
	if( !io )
		return;

	io->meta     = meta;
	io->meta_ctx = ctx;
}

static inline void stream_io_set_abort( STREAM_IO *io, ABORT_HANDLER abort, void *ctx )
{
	if( !io )
		return;

	io->abort     = abort;
	io->abort_ctx = ctx;
}

static inline void stream_io_set_chunk_size( STREAM_IO *io, int size )
{
	if( !io )
		return;

	io->chunk_size = size;
}

static inline UINT64 stream_io_get_pos( STREAM_IO *io )
{
	return io->pos;
}

#endif
