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

#ifndef _ASTDLIB_H
#define _ASTDLIB_H

#include <unistd.h>

#ifndef UCLIBC
#include <malloc.h>
#endif

#ifdef STANDALONE

#include <stdlib.h>

#define amalloc( a )            malloc( (a) )
#define acalloc( a, b )         calloc( (a), (b) )
#define arealloc( a, b )        realloc( (a), (b) )
#define afree( a )              free( (a) )

#define amalloc_dma( a )        malloc( (a) )
#define afree_dma( a, b )       free( (a) )

#define amalloc_dma_cached( a ) malloc( (a) )
#define afree_dma_cached( a )   free( (a) )

#define amalloc_bitmap( a )     malloc( (a) )
#define afree_bitmap( a )       free( (a) )

static inline void *stream_malloc_dma( size_t size )
{
	return amalloc_dma( size );
}

static inline void stream_free_dma( UCHAR **ptr, size_t size )
{
	if( ptr ) {
		afree_dma( *ptr, size );
		*ptr = NULL;
	}
}

#else // STANDALONE

#define ALIGN_16

#include <stdlib.h>
#include <string.h>

static inline void *amalloc_align( size_t alignment, size_t size )
{
#ifdef UCLIBC
	void *ptr;
	return posix_memalign( &ptr, alignment, size ) == 0 ? ptr : NULL;
#else
	return memalign( alignment, size );
#endif
}

#ifdef ALIGN_16
static inline void *amalloc( size_t size )
{
	return amalloc_align( 16, size );
}
static inline void *acalloc( size_t nmemb, size_t size )
{
	void *p = amalloc_align( 16, size * nmemb );
	return memset( p, '\0', size * nmemb );
}
#else
#define amalloc( a )            malloc( (a) )
#define acalloc( a, b )         calloc( (a), (b) )
#endif

#define arealloc( a, b )        realloc( (a), (b) )
#define afree( a )              free( (a) )

#define amalloc_dma( a )        amalloc( (a) )
#define afree_dma( a, b )       afree( (a) )
#define acalloc_named( a, b, c) acalloc( (a), (b) )

#define amalloc_dma_cached( a ) amalloc( (a) )
#define afree_dma_cached( a, b ) afree( (a) )

#define amalloc_bitmap( a )     amalloc( (a) )
#define afree_bitmap( a )       afree( (a) )
#define async_dma_cached( a, b )
#define astrdup( a )            strdup( a )
#define arand( )                rand()
#define asrand( a )             srand( a )

#define afree_align( a )        afree( a )

#define async_dma_cached( a, b )

#include <stdlib.h>
#define asystem_init()
#define asystem_exit()
#define asystem( a ) system( a )

#endif // STANDALONE

#endif
