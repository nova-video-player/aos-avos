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

#ifndef STANDALONE
#include "global.h"
#include "debug.h"
#include "frame_q.h"
#include "util.h"
#endif
#include <ctype.h>

#define DBGQ if( 0 )

int frame_q_init( FRAME_Q *q, char *tag )
{
	if( !q )
		return 1;
	strnZcpy( q->tag, tag, sizeof( q->tag ) - 1 );
	frame_q_flush( q );

	return 0;
}

int frame_q_count( FRAME_Q *q )
{
	VIDEO_FRAME *frame = q->head;
	
	int count = 0;
	while( frame ) {
		count ++;
		frame = frame->next;
	}
	return count;
}

VIDEO_FRAME *frame_q_get( FRAME_Q *q )
{
	VIDEO_FRAME *frame = q->head;
	
	if( frame ) {
DBGQ serprintf(" GET( %s %2d ) ", q->tag, frame->index ); 
		q->head = frame->next;
	}
	
	return frame;
}

VIDEO_FRAME *frame_q_get_unlocked( FRAME_Q *q )
{
	VIDEO_FRAME **prev = NULL;
	VIDEO_FRAME *frame = q->head;
	
	while( frame ) {
		if( !(frame->locked) ) {
			if( prev )
				*prev = frame->next;
			else
				q->head = frame->next;
				
			return frame;
		}
		prev = &frame->next;
		frame = frame->next;
	}
	return NULL;
}

VIDEO_FRAME *frame_q_get_index( FRAME_Q *q, int index )
{
	VIDEO_FRAME **prev = NULL;
	VIDEO_FRAME *frame = q->head;
	
	while( frame ) {
		if( frame->index == index ) {
			if( prev )
				*prev = frame->next;
			else
				q->head = frame->next;
				
			return frame;
		}
		prev = &frame->next;
		frame = frame->next;
	}
	return NULL;
}

VIDEO_FRAME *frame_q_peek( FRAME_Q *q )
{
	VIDEO_FRAME *frame = q->head;
	
	if( frame ) {
DBGQ serprintf(" PEEK( %s %2d ) ", q->tag, frame->index ); 
	}
	
	return frame;
}

int frame_q_put( FRAME_Q *q, VIDEO_FRAME *frame )
{
	
	if( !frame )
		return 1;
		
	if( !q->head ) {
		q->head = frame;
		frame->next = NULL;
DBGQ serprintf(" PUT( %s %2d ) ", q->tag, frame->index ); 
		return 0;
	}
DBGQ serprintf(" PUT( %s %2d ) ", q->tag, frame->index ); 

	VIDEO_FRAME *now = q->head;
	
	if( now == frame ) {
serprintf("frame_q_put: FATAL ERROR: %d already in [%s]\r\n", frame->index, q->tag );
		return 1;
	}
	while( now->next ) {
		now = now->next;	
		if( now == frame ) {
serprintf("frame_q_put: FATAL ERROR: %d already in [%s]\r\n", frame->index, q->tag );
			return 1;
		}
	}
	now->next   = frame;
	frame->next = NULL;
	
	return 0;
}

void frame_q_put_head( FRAME_Q *q, VIDEO_FRAME *frame )
{
	if( !frame )
		return;
		
	if( !q->head ) {
		q->head = frame;
		frame->next = NULL;
DBGQ serprintf(" PUT1( %s %2d ) ", q->tag, frame->index ); 
		return;
	}
DBGQ serprintf(" PUT1( %s %2d ) ", q->tag, frame->index ); 

	VIDEO_FRAME *now = q->head;
	q->head = frame;
	frame->next = now;
}

void frame_q_flush( FRAME_Q *q )
{
DBGQ serprintf("FLUSH( %s )\r\n", q->tag ); 
	q->head = NULL;
}

void frame_q_dump( FRAME_Q *q )
{
	if( !q ) {
serprintf("frame_q_dump fail!\n");
		return;
	}
	VIDEO_FRAME *frame = q->head;
	
serprintf("frame_q: [%s]", q->tag );
	while( frame ) {
serprintf(" [%2d|%d]", frame->index, frame->locked );
		frame = frame->next;
	}
serprintf("\n");
}

void frame_q_dump2( FRAME_Q *q )
{
	if( !q ) {
serprintf("frame_q_dump2 fail!\n");
		return;
	}
	VIDEO_FRAME *frame = q->head;
	
serprintf("[%s:", q->tag );
	while( frame ) {
serprintf(" %2d", frame->index);		
		frame = frame->next;
	}
serprintf("] ");
}

#ifdef DEBUG_MSG
#define NUM 10
#include "atime.h"
static void test( void )
{
	FRAME_Q q;
	frame_q_init( &q, "test" );
	
	VIDEO_FRAME f[NUM];
	int i;
	
	for( i = 0; i < NUM; i ++ ) {
		f[i].index = i;
		f[i].locked = (i % 5);
		if( i == 5 )
			frame_q_put_head( &q, f + i );
		else
			frame_q_put( &q, f + i );
	}
	frame_q_dump( &q );
	
	VIDEO_FRAME *l;
	
	while( (l = frame_q_get_unlocked( &q )) ) {
serprintf("unlocked: %d\n", l->index );	
msec_sleep( 10 );
	}
}

DECLARE_DEBUG_COMMAND_VOID( "fqt", test );
#endif
