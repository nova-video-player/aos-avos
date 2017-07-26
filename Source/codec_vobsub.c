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
#include "vobsub.h"
#include "astdlib.h"
#include "stream.h"

#include <string.h>

#ifdef CONFIG_STREAM
#ifdef CONFIG_VOBSUB

#define DBGS	if(Debug[DBG_STREAM])
#define DBG1 	if(Debug[DBG_SUB])
#define DBG2 	if(Debug[DBG_SUB] > 1)

typedef struct PRIV {
	VOBSUB *vs;
} PRIV;

static int _open( STREAM_DEC_SUB *dec, SUB_PROPERTIES *sub, void *ctx )
{
DBGS serprintf("sub_dec_open_VOBSUB\r\n");

	PRIV *p = (PRIV*)dec->priv;
	p->vs = VOBSUB_create( sub->extraDataSize ? sub->extraData : NULL );
	if( !p->vs )
		return 1;
	dec->ctx = ctx;
	dec->is_open = 1;

	if( sub->extraData2 && sub->extraDataSize2 ) {
DBG1		Dump( sub->extraData2, sub->extraDataSize2 );
	} else if( sub->extraDataSize ) { 
DBG1		Dump( sub->extraData, sub->extraDataSize );
	}
	
	return 0;
}

static int _close( STREAM_DEC_SUB *dec )
{
DBGS serprintf( "sub_dec_close_VOBSUB\r\n");
	if( !dec->is_open ) {
serprintf("not open!\r\n");
		return 1;
	}
	
	PRIV *p = (PRIV*)dec->priv;
	VOBSUB_destroy( p->vs );
	dec->is_open = 0;
 	return 0;
}

static int _decode( STREAM_DEC_SUB *dec, UCHAR *data, int size, int time, VIDEO_FRAME **pframe )
{
DBG1 serprintf("vobsub_decode( size %5d, time %d )\r\n", size, time );	
	PRIV *p = (PRIV*)dec->priv;
	return VOBSUB_parse_chunk( p->vs, data, size, time, pframe );
}

static int _flush( STREAM_DEC_SUB *dec )
{	
	PRIV *p = (PRIV*)dec->priv;
	VOBSUB_reset( p->vs );
	return 0;
} 

static int _destroy( STREAM_DEC_SUB *dec )
{
	if( dec	) {
		afree( dec->priv );
		afree( dec );
	}
	return 0;
} 

static STREAM_DEC_SUB *_new_dec_vobsub( void ) 
{ 
	STREAM_DEC_SUB *dec = (STREAM_DEC_SUB *)amalloc( sizeof( STREAM_DEC_SUB ) );
	
	if( !dec )
		return NULL;
	memset( dec, 0, sizeof( STREAM_DEC_SUB ) );

	static char name[] = "vobsub";
	dec->name    = name;
	dec->destroy = _destroy;
	dec->open    = _open;
	dec->close   = _close;
	dec->decode  = _decode;
	dec->flush   = _flush;
	
	if( !(dec->priv = acalloc( 1, sizeof( PRIV ) ) ) ) {
serprintf("vobsub: cannot alloc context\n");
		afree(dec);
		return NULL;
	}
	
	return dec;
}

STREAM_REGISTER_DEC_SUB( SUB_FORMAT_DVD_GFX, _new_dec_vobsub, "vobsub" );

#endif
#endif
