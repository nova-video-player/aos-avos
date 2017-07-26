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

#include "types.h"
#include "global.h"
#include "stream.h"
#include "stream_heap.h"
#include "debug.h"
#include "astdlib.h"

#include <string.h>

#ifdef CONFIG_STREAM

/* this is the allocator */
struct mem_block {
	struct mem_block* prev;
	struct mem_block* next;
	size_t start;
	int    size;
	int    state;
};

#define MEM_BLOCK_USED	1
#define MEM_BLOCK_FREE	0
#define MEM_BLOCK_ROOT	-1;

static struct mem_block *heap;
static size_t heap_start;
static int    heap_size;
static int    heap_used;
static int    shdebug = 0;

#define DBG if( shdebug )
#define DBGS if( Debug[DBG_STREAM] )

pthread_mutex_t heap_lock = PTHREAD_MUTEX_INITIALIZER;

static struct mem_block *split_block(struct mem_block *p, size_t start, int size)
{
DBG serprintf("split_block(%08X(%6d), %08X, %6d)\n", p->start, p->size, start, size);
	
	/* Maybe cut off the start of an existing block */
	if (start > p->start) {
		struct mem_block *newblock = amalloc(sizeof(*newblock));
		if (!newblock) 
			goto out;
		newblock->start = start;
		newblock->size = p->size - (start - p->start);
		newblock->state = MEM_BLOCK_FREE;
		newblock->next = p->next;
		newblock->prev = p;
		p->next->prev = newblock;
		p->next = newblock;
		p->size -= newblock->size;
		p = newblock;
	}

	/* Maybe cut off the end of an existing block */
	if (size < p->size) {
		struct mem_block *newblock = amalloc(sizeof(*newblock));
		if (!newblock)
			goto out;
		newblock->start = start + size;
		newblock->size = p->size - size;
		newblock->state = MEM_BLOCK_FREE;
		newblock->next = p->next;
		newblock->prev = p;
		p->next->prev = newblock;
		p->next = newblock;
		p->size = size;
	}

out:
	 /* Our block is in the middle */
	 p->state = MEM_BLOCK_USED;
	 return p;
}

#define list_for_each(pos, head) for(pos = (head)->next; pos != (head); pos = pos->next)
	
static struct mem_block *alloc_block(int size, int align2)
{
	struct mem_block *p;
	int mask = (1 << align2)-1;

DBG serprintf("alloc_block(%i, %i)\n", size, align2);

	list_for_each(p, heap) {
		size_t start = (p->start + mask) & ~mask;
		if (p->state == MEM_BLOCK_FREE && start + size <= p->start + p->size)
			return split_block( p, start, size);
	}

	return NULL;
}

static struct mem_block *find_block( int start )
{
	struct mem_block *p;

	list_for_each(p, heap) {
		if (p->start == start)
			return p;
	}
	
	return NULL;
}

static void free_block( struct mem_block *p )
{
	p->state = MEM_BLOCK_FREE;

	/* Assumes a single contiguous range.  Needs a special filp in
	* 'heap' to stop it being subsumed.
	*/
	if (p->next->state == MEM_BLOCK_FREE) {
		struct mem_block *q = p->next;
		p->size += q->size;
		p->next = q->next;
		p->next->prev = p;
		afree(q);
	}

	if (p->prev->state == MEM_BLOCK_FREE) {
		struct mem_block *q = p->prev;
		q->size += p->size;
		q->next = p->next;
		q->next->prev = q;
		afree(p);
	}
}

int stream_heap_create(unsigned long size)
{
DBGS serprintf("stream_heap_create: %d\n", size);
	heap_size = size;
	heap_used = 0;
	
	heap = (struct mem_block*)amalloc(sizeof(struct mem_block));
	if (!heap) {
serprintf("stream_heap_create: NO HEAD\n");
		return 1;
	}
	
	struct mem_block *blocks = (struct mem_block*)amalloc(sizeof(struct mem_block));
	if (!blocks) {
serprintf("stream_heap_create: NO BLOCK\n");
		afree(heap);
		return 1;
	}

	heap_start = (size_t)amalloc(heap_size);
	if (!heap_start) {
serprintf("stream_heap_create: NO START\n");
		afree(heap);
		afree(blocks);
		return 1;
	}
	
	blocks->start = heap_start;
	blocks->size  = size;
	blocks->state = MEM_BLOCK_FREE;
	blocks->next  = blocks->prev = heap;

	memset(heap, 0, sizeof(struct mem_block));
	heap->state = MEM_BLOCK_ROOT;
	heap->next = heap->prev = blocks;
	return 0;
}

void stream_heap_destroy(void)
{
DBGS serprintf("stream_heap_destroy: %d/%d\n", heap_used, heap_size);
	struct mem_block *p;

	list_for_each(p, heap) {
		free_block( p );
	}

	afree(heap->next);
	afree(heap);
	
	afree((void*)heap_start);

	heap = NULL;
}

void *stream_heap_alloc(size_t size)
{
	struct mem_block* block;
	
	pthread_mutex_lock(&heap_lock);
	block = alloc_block(size, 4);
	heap_used += block ? block->size : 0;
	pthread_mutex_unlock(&heap_lock);
	
	if (!block) {
serprintf("stream_heap_alloc(%6d): failed to allocate\n", size);
		return 0;
	}
	
DBG serprintf("stream_heap_alloc(%6d) -> %08X\n", size, block->start);

	return (void*)block->start;
}

// (very) unclever implementation
void *stream_heap_realloc( void *ptr, size_t size )
{
	void *newptr = NULL;

	if ( !ptr )
		return stream_heap_alloc( size );
	if ( !size ) {
		stream_heap_free( ptr );
		return stream_heap_alloc( 0 );
	}

	newptr = stream_heap_alloc( size );
	if ( newptr ) {
		size_t old = *( ( size_t * ) ( ( char * ) ptr - sizeof( size_t ) ) );
		size_t copy = old > size ? size : old;
		memcpy( newptr, ptr, copy );
		stream_heap_free( ptr );
	}
	return newptr;
}

void stream_heap_free( void *ptr )
{
	struct mem_block *block;
	
DBG serprintf("stream_heap_free(%08X)\n", ptr);

	pthread_mutex_lock(&heap_lock);
	block = find_block((size_t)ptr);
	if (!block) {
		pthread_mutex_unlock(&heap_lock);
serprintf("stream_heap_free(%08X) CANNOT find block!\n", ptr);
		return;
	}
	
	heap_used -= block->size;
	free_block(block);
	pthread_mutex_unlock(&heap_lock);
	return;
}

#ifdef DEBUG_MSG
#include <stdlib.h>
static void test( int argc, char **argv )
{
	int mb = 4;
	if( argc > 1 )
		mb = atoi( argv[1] );
		
	stream_heap_create( mb * 1024 * 1024 );
	
	void *addr[1024];
	int size[1024];
	int count = 0;
	int total = 0;
	
	int i;
	for( i = 0; i < 1024; i++ ) {
		size[i] = rand() % 32768;
		addr[i] = stream_heap_alloc( size[i] );
		if( !addr[i] )
			break;
		count ++;
		total += size[i];
	}
	
	for( i = 0; i < count; i++ ) {
		stream_heap_free( addr[i] );
	}
	
	stream_heap_destroy();

serprintf("alloced/freed %d bytes of %d\r\n", total, mb * 1024 * 1024 );
}

DECLARE_DEBUG_COMMAND( "shtest", test );
DECLARE_DEBUG_TOGGLE( "dbgsh", shdebug );
#endif
#endif
