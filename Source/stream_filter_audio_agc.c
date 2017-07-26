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
#include "stream_filter_audio.h"
#include "debug.h"
#include "atime.h"
#include "pcm_autogain.h"
#include "astdlib.h"

int agc_delete( STREAM_FILTER_AUDIO *f )
{
serprintf("faagc: delete\n" );
	if( f )
		afree( f->priv );
	afree(f);
	return 0;
}

static int agc_open( STREAM_FILTER_AUDIO *f, AUDIO_PROPERTIES *audio )
{
serprintf("faagc: open %d\n", audio->samplesPerSec );
	pcm_set_agc( audio->samplesPerSec );

	return 0;
}

static int agc_close( STREAM_FILTER_AUDIO *f ) 
{
serprintf("faagc: close\n" );
	return 0;
}

#define AGC_ORDER	5
#define AGC_BLOCK	(1<<AGC_ORDER)

static int agc_filter( STREAM_FILTER_AUDIO *f, AUDIO_FRAME *frame )
{
	UCHAR *data = frame->data;
	int    size = frame->size;

	while( size ) {
		if( size < AGC_BLOCK ) {
serprintf("rest %d\n", size );
			break;
		}
		pcm_apply_agc( (SHORT*)data, AGC_ORDER );
		size -= AGC_BLOCK * 2;
		data += AGC_BLOCK * 2;
	}
	return 0;
}

static int agc_flush( STREAM_FILTER_AUDIO *f )
{
serprintf("faagc: flush\n" );
	return 0;
}

int agc_delay( STREAM_FILTER_AUDIO *f )
{
	return 0;
}

STREAM_FILTER_AUDIO *stream_filter_audio_agc_new( void ) 
{ 
	STREAM_FILTER_AUDIO *f = acalloc( 1, sizeof( STREAM_FILTER_AUDIO ) );
	
	if( !f )
		return NULL;

	static char name[] = "agc";
	f->name    = name;
	f->delete  = agc_delete;
	f->open    = agc_open;
	f->close   = agc_close;
	f->filter  = agc_filter;
	f->flush   = agc_flush;
	f->delay   = agc_delay;

	return f;
}
