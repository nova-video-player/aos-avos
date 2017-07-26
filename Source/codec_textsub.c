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
#include "debug.h"
#include "astdlib.h"
#include "stream.h"
#include "util.h"

#include <string.h>
#include <ctype.h>
#include <stdio.h>

#ifdef CONFIG_STREAM

#define DBGS	if(Debug[DBG_STREAM])
#define DBG 	if(Debug[DBG_SUB])
#define DBG2 	if(Debug[DBG_SUB] > 1)

static int _open( STREAM_DEC_SUB *dec, SUB_PROPERTIES *sub, void *ctx )
{
DBGS serprintf("sub_dec_open_TEXT\r\n");
	// copy the props locally
	dec->subtitle = &dec->_subtitle;
	*dec->subtitle = *sub;
	
	dec->ctx = ctx;
	dec->is_open = 1;
	
	return 0;
}

static int _close( STREAM_DEC_SUB *dec )
{
DBGS serprintf( "sub_dec_close_TEXT\r\n");
	if( !dec->is_open ) {
serprintf("SSA: not open!\r\n");
		return 1;
	}
	
	dec->is_open = 0;
 	return 0;
}

static int _decode( STREAM_DEC_SUB *dec, UCHAR *data, int size, int time, VIDEO_FRAME **pframe )
{
DBG serprintf("TEXT_decode: size %5d  time %d  [%s]\r\n", size, time, data );	
DBG2 Dump( data, size );
	int start;
	int end;
	if( sscanf( data, "%d:%d,", &start, &end ) != 2 ) {
		return 1;
	}
	
	UCHAR *text = strstr( data, "," );
	if( !text )
		return 1;
	text ++;
	size -= text - data;
	
	// this is supposed to be utf-8, so just copy:
	VIDEO_FRAME *frame = *pframe;
	int   max = frame->size - 1;
	char *dst = frame->data[0];
	while( *text && size-- && max-- ) {
		*dst++ = *text++;
	}
	*dst = '\0';
	
	frame->time          = start;
	frame->duration      = end - start; 

	return 0;
}

static int _flush( STREAM_DEC_SUB *dec )
{	
	return 0;
} 

static int _destroy( STREAM_DEC_SUB *dec )
{
	if( dec	) {
		afree( dec );
	}
	return 0;
} 

static STREAM_DEC_SUB *_new_dec_text( void ) 
{ 
	STREAM_DEC_SUB *dec = (STREAM_DEC_SUB *)amalloc( sizeof( STREAM_DEC_SUB ) );
	
	if( !dec )
		return NULL;
	memset( dec, 0, sizeof( STREAM_DEC_SUB ) );

	static char name[] = "TEXT";
	dec->name    = name;
	dec->destroy = _destroy;
	dec->open    = _open;
	dec->close   = _close;
	dec->decode  = _decode;
	dec->flush   = _flush;
	
	return dec;
}

STREAM_REGISTER_DEC_SUB( SUB_FORMAT_TEXT, _new_dec_text, "TEXT" );

#endif
