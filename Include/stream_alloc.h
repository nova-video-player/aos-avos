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

#ifndef _STREAM_MEM_H
#define _STREAM_MEM_H

#include <unistd.h>

#include "stream.h"
#include "av.h"

VIDEO_FRAME *frame_alloc( int width, int height );
VIDEO_FRAME *frame_alloc_cached( int width, int height );
VIDEO_FRAME *frame_flush_cached( VIDEO_FRAME* frame );
VIDEO_FRAME *frame_alloc_with_cs( int width, int height, int colorspace );
VIDEO_FRAME *frame_alloc_with_cs_and_mem( int width, int height, int colorspace, int mem_type );
void	     frame_free( VIDEO_FRAME *f );

void *stream_malloc           ( size_t size );
void *stream_malloc_dma       ( size_t size );
void *stream_malloc_dma_cached( size_t size );
void stream_free           ( void *ptr );
void stream_free_dma       ( unsigned char **ptr, size_t size );
void stream_free_dma_cached( unsigned char **ptr, size_t size );

int stream_alloc_frames( VIDEO_FRAME *(*frames)[], int width, int height, int colorspace, int mem_type, int *count );
int stream_free_frames ( VIDEO_FRAME *(*frames)[], int count );

static inline int stream_get_mem_type( void ) 
{
	return STREAM_MEM_DMA;
}

#endif
