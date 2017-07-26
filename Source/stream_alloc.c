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
#include "stream.h"
#include "stream_alloc.h"
#include "debug.h"

#define DBGS if(Debug[DBG_STREAM])

static int alloc_packed = 1;

// ************************************************************
//
//	malloc_clever_buffer
//
// ***********************************************************
int malloc_clever_buffer( CLEVER_BUFFER *buffer, int size )
{
	if( !buffer )
		return 1;

	memset( buffer, 0, sizeof( CLEVER_BUFFER ) );
	buffer->data = amalloc( size );
	if ( buffer->data ) {
		buffer->size = size;
		return 0;
	}	
	return 1;
}

// ************************************************************
//
//	realloc_clever_buffer
//
// ***********************************************************
int realloc_clever_buffer( CLEVER_BUFFER *buffer, int min_size )
{
	if( !buffer )
		return 1;
	
	int 	size = buffer->size ? buffer->size : 1;
	UCHAR 	*new_data;
	while( size < min_size )
		size *= 2;

	new_data = amalloc( size );	
	if( new_data ) {
//serprintf("realloc %d -> %d\r\n", buffer->size, size );
		// copy old data if any
		if( buffer->data ) {
			memcpy( new_data, buffer->data, buffer->size );
			afree( buffer->data );
		}
		buffer->data = new_data;
		buffer->size = size;
		return 0; 
	}
serprintf("realloc fail! %d\r\n", min_size );
	return 1;
}

// ************************************************************
//
//	free_clever_buffer
//
// ***********************************************************
void free_clever_buffer( CLEVER_BUFFER *buffer )
{
	if( !buffer )
		return;
		
	if( buffer->data ) {
		afree( buffer->data );
	}
	buffer->size = 0;
}


// ************************************************************
//
//	stream_malloc
//
// ***********************************************************
void *stream_malloc( size_t size )
{
	return amalloc( size );
}

// ************************************************************
//
//	stream_malloc_dma
//
// ***********************************************************
void *stream_malloc_dma( size_t size )
{
	return amalloc_dma( size );
}

// ************************************************************
//
//	stream_malloc_dma_cached
//
// ***********************************************************
void *stream_malloc_dma_cached( size_t size )
{
	return amalloc_dma_cached( size );
}

// ************************************************************
//
//	stream_free
//
// ***********************************************************
void stream_free( void *ptr )
{
	afree( ptr );
}

// ************************************************************
//
//	stream_free2
//
// ***********************************************************
static void stream_free2( UCHAR **ptr, size_t size )
{
	if( ptr ) {
		afree( *ptr );
		*ptr = NULL;
	}
}

// ************************************************************
//
//	stream_free_dma
//
// ***********************************************************
void stream_free_dma( UCHAR **ptr, size_t size )
{
	if( ptr ) {
		afree_dma( *ptr, size );
		*ptr = NULL;
	}
}

// ************************************************************
//
//	stream_free_dma_cached
//
// ***********************************************************
void stream_free_dma_cached( UCHAR **ptr, size_t size )
{
	if( ptr ) {
		afree_dma_cached( *ptr, size );
		*ptr = NULL;
	}
}

static void stream_frame_free2( VIDEO_FRAME *f )
{
	int i;
	for (i = 0; f->data[i]; i++)
		stream_free2( &f->data[i], f->data_size[i] );
}

static void stream_frame_free_dma( VIDEO_FRAME *f )
{
	int i;
	for (i = 0; f->data[i]; i++)
		stream_free_dma( &f->data[i], f->data_size[i] );
}

static void stream_frame_free_dma_cached( VIDEO_FRAME *f )
{
	int i;
	for (i = 0; f->data[i]; i++)
		stream_free_dma_cached( &f->data[i], f->data_size[i] );
}

// ************************************************************
//
//	frame_alloc
//
// ***********************************************************
VIDEO_FRAME *_frame_alloc( int width, int height, int colorspace, int mem_type )
{
	VIDEO_FRAME *frame = amalloc( sizeof( VIDEO_FRAME ) );
	int i;

	if( !frame )
		return NULL;

	memset( frame, 0, sizeof( VIDEO_FRAME ) );
	frame->map_fd[0] = frame->map_fd[1] = frame->map_fd[2] = -1;
	switch( colorspace ) {
	case AV_IMAGE_BGRA_32:
	case AV_IMAGE_RGBX_32:
		frame->linestep[0] = linestep_from_width( width ) / 2;
		frame->bpp[0]      = 4;
		break;

	case AV_IMAGE_YUV_422:
		frame->linestep[0] = linestep_from_width( width );
		frame->bpp[0]      = 2;
		break;
	case AV_IMAGE_GRAYSCALE:
		frame->linestep[0] = linestep_from_width( width ) / 2;
		frame->bpp[0]      = 1;
		break;
	case AV_IMAGE_NV12:
	case AV_IMAGE_YV12:
	case AV_IMAGE_TEGRA3_YV12:
	case AV_IMAGE_EXYNOS_NV12_TILED:
	case AV_IMAGE_QCOM_NV12_TILED:
		frame->linestep[0] = linestep_from_width( width );
		frame->linestep[1] = frame->linestep[0];
		frame->bpp[0] = frame->bpp[1] = 1;
		break;
	case AV_IMAGE_HW:
		break;
	default:
serprintf("frame_alloc: unknown color space: %d\r\n", colorspace );
		afree( frame );
		return NULL;
	}
	
	frame->colorspace = colorspace;
	for (i = 0; frame->linestep[i]; i++) {
		frame->data_size[i] = frame->linestep[i] * frame->bpp[i] * pad_height( height );
		frame->size += frame->data_size[i];
	}

	switch( mem_type ) {
	case STREAM_MEM_NRM:
		for (i = 0; frame->linestep[i]; i++)
			frame->data[i] = stream_malloc( frame->data_size[i] );
		frame->destroy = stream_frame_free2;
		break;

	case STREAM_MEM_DMA:
		for (i = 0; frame->linestep[i]; i++)
			frame->data[i] =  stream_malloc_dma( frame->data_size[i] );
		frame->destroy = stream_frame_free_dma;
		break;

	case STREAM_MEM_DMA_CACHED:
		for (i = 0; frame->linestep[i]; i++)
			frame->data[i] = stream_malloc_dma_cached( frame->data_size[i] );
		frame->destroy = stream_frame_free_dma_cached;
		break;
	}
	
	if ( !frame->data[0] && mem_type != STREAM_MEM_BYO && mem_type != STREAM_MEM_ANDROID ) {
serprintf("frame_alloc: error allocating %d x %d frame!\r\n", width, height );
		afree( frame );
		return NULL;
	}

	frame->width    = width;
	frame->height   = height;

	return frame;
}

VIDEO_FRAME *frame_alloc( int width, int height )
{
	return _frame_alloc( width, height, AV_IMAGE_YUV_422, STREAM_MEM_DMA );
}

VIDEO_FRAME *frame_alloc_cached( int width, int height )
{
	return _frame_alloc( width, height, AV_IMAGE_YUV_422, STREAM_MEM_DMA_CACHED );
}

VIDEO_FRAME *frame_alloc_with_cs( int width, int height, int colorspace )
{
	return _frame_alloc( width, height, colorspace, STREAM_MEM_NRM );
}

VIDEO_FRAME *frame_alloc_with_cs_and_mem( int width, int height, int colorspace, int mem_type  )
{
	return _frame_alloc( width, height, colorspace, mem_type );
}

// ************************************************************
//
//	frame_free
//
// ***********************************************************
void frame_free( VIDEO_FRAME *f )
{
	if( f ) {
		if ( f->destroy )
			f->destroy( f );
		afree( f );	
	}
}

// ************************************************************
//
//	stream_alloc_frames
//
// ***********************************************************
int stream_alloc_frames( VIDEO_FRAME *(*frames)[], int width, int height, int colorspace, int mem_type, int *count )
{
	int i;
	for( i = 0; i < *count; i++ ) {
		if( !( (*frames)[i] = frame_alloc_with_cs_and_mem( width, height, colorspace, mem_type ) ) ) {
			// that is all we can do, stop
			break;
		}
		(*frames)[i]->index = i;
	}
	*count = i;
DBGS serprintf("stream_alloc: allocated %d frames\r\n", *count);
	return 0;
}

// ************************************************************
//
//	stream_free_frames
//
// ***********************************************************
int stream_free_frames( VIDEO_FRAME *(*frames)[], int count ) 
{
	int i;
	for( i = 0; i < count; i++ ) {
		if( (*frames)[i] ) {
			frame_free( (*frames)[i] );
		}
	}
	return 0;
}

#ifdef DEBUG_MSG
static int dbg_f_num;
static VIDEO_FRAME *dbg_f[32];

static void dbg_f_alloc( int argc, char *argv[] )
{
	if( argc < 2 ) {
		return;
	}
	dbg_f_num = atoi(argv[1]);
	
	stream_alloc_frames( &dbg_f, 1920, 1080, AV_IMAGE_NV12, STREAM_MEM_ION, &dbg_f_num );
	
	int i;
	for( i = 0; i < dbg_f_num; i++ ) {
serprintf("alloced: [%d] addr %08X\n", i, dbg_f[i]->data[0] );
	}
}

static void dbg_f_free( int argc, char *argv[] )
{
	stream_free_frames( &dbg_f, dbg_f_num );
}

DECLARE_DEBUG_COMMAND("sfal", dbg_f_alloc );
DECLARE_DEBUG_COMMAND("sffr", dbg_f_free  );
DECLARE_DEBUG_TOGGLE ("salp", alloc_packed  );
#endif
