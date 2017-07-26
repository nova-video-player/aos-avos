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

#include "astdlib.h"
#include "debug.h"
#include "util.h"

#include "cbe.h"
#include "stream_alloc.h"

#include <string.h>

// *********************************************************************
// create, destroy
// *********************************************************************
CBE *cbe_new( int size, int overlap, int dma )
{
	CBE *cbe;
	
	if( !size || !overlap || !(cbe = amalloc( sizeof( CBE ) ) ) ) {
		return NULL;
	}
	
	cbe->size    = size;
	cbe->overlap = overlap;
	cbe->dma     = dma;
	
	if( cbe->dma ) {
		cbe->data = stream_malloc_dma( size + overlap );
	} else {
		cbe->data = amalloc( size + overlap );
	}
	if( !cbe->data ) {
		afree( cbe );
		return NULL;
	}
	cbe_flush( cbe );
	
	return cbe;
}

void cbe_delete( CBE **cbe )
{
	if( *cbe ) {
		if( (*cbe)->dma ) {
			stream_free_dma( &(*cbe)->data, (*cbe)->size + (*cbe)->overlap );
		} else {
			afree( (*cbe)->data );
		}
		afree( *cbe );
		*cbe = NULL;
	}
}

void cbe_flush( CBE *cbe )
{
//serprintf("cbe flush: \r\n" );
	cbe->read  = 0;
	cbe->write = 0;
}

// *********************************************************************
// stats of the circular buffer
// *********************************************************************
int cbe_get_used( CBE *cbe )
{
	int used = cbe->write - cbe->read;
	if( used < 0 )
		used += cbe->size;
		
	return used;
}

int cbe_get_free( CBE *cbe )
{
	int free = cbe->read - cbe->write;
	if( free <= 0 )
		free += cbe->size;
		
	return free;
}

int cbe_get_size( CBE *cbe )
{
	return cbe->size;
}

// *********************************************************************
// read operations to the cbe
// *********************************************************************
unsigned char *cbe_get_p( CBE *cbe )
{
	return cbe->data + cbe->read;
}

unsigned char *cbe_get_tail_p( CBE *cbe, int size )
{
	int pos = cbe->write - size;
	if( pos < 0 ) {
		pos += cbe->size;
	}
	return cbe->data + pos;
}

unsigned char *cbe_get_write_p( CBE *cbe )
{
	return cbe->data + cbe->write;
}

unsigned int cbe_get_rest_size( CBE *cbe )
{
	return cbe->size + cbe->overlap - cbe->read;
}

void cbe_skip( CBE *cbe, int count )
{
	int used = cbe_get_used( cbe );
	count = MIN( count, used );
	
//serprintf("cbe skip: %d  %d\r\n", cbe->read, count );
	cbe->read += count;
	if( cbe->read >= cbe->size )
		cbe->read -= cbe->size;
}

void cbe_skip_all( CBE *cbe )
{
	cbe->read = cbe->write;
}

// *********************************************************************
// write operations to the CBE
// *********************************************************************
static void _cbe_copy_nowrap( CBE *cbe, unsigned int *write, const unsigned char *buffer, int count )
{
		memcpy( cbe->data + *write, buffer, count );
		
		// copy start to overlap area !
		if ( *write < cbe->overlap ) {
			int copy = MIN( count, cbe->overlap - *write);
			
//serprintf("cbe ovrl:  %d  %d\r\n", cbe->size + cbe->write, size );
			memcpy( cbe->data + cbe->size + *write, buffer, copy );
		}
		*write += count;
		if( *write >= cbe->size )
			*write -= cbe->size;
}

static int _cbe_copy( CBE *cbe, unsigned int *write, const unsigned char *buffer, int count )
{
	int rest = cbe->size - *write;
//serprintf("cbe write: %d  %d\r\n", cbe->write, count );
	if( rest >= count ) {
		_cbe_copy_nowrap( cbe, write, buffer, count );
	} else {
		_cbe_copy_nowrap( cbe, write, buffer, rest );
		_cbe_copy_nowrap( cbe, write, buffer + rest, count - rest );
	}	
	return count;
}

int cbe_write( CBE *cbe, const unsigned char *buffer, int count )
{
	return _cbe_copy( cbe, (unsigned int *)&cbe->write, buffer, count );
}

unsigned char *cbe_get_patch_p( CBE *cbe, int size, unsigned char **copy_p, int *copy_size )
{
	if( cbe->write < cbe->overlap ) {
		int copy = MIN( size, cbe->overlap - cbe->write ); 
		if( copy_size )
			*copy_size = copy;
		if( copy_p )
			*copy_p = cbe->data + cbe->size + cbe->write;
	} else {
		if( copy_size )
			*copy_size = 0;
		if( copy_p )
			*copy_p = NULL;
	}
	return cbe->data + cbe->write;
}

int cbe_patch( CBE *cbe, unsigned char *patch_p, int size )
{
	int write = patch_p - cbe->data;
//serprintf("write: %d\r\n", write );
	if( write < 0 || write >= cbe->size )
		return 1;
	
	if( write < cbe->overlap ) {
		int copy = MIN( size, cbe->overlap - write ); 
//serprintf("copy:  %d\r\n", copy );
		memcpy( cbe->data + cbe->size + write, cbe->data + write, copy );
	}
	return 0;	
}

