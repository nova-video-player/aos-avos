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

int dummy_delete( STREAM_FILTER_AUDIO *f )
{
serprintf("fadummy: delete\n" );
	afree(f);
	return 0;
}

int dummy_open( STREAM_FILTER_AUDIO *f, AUDIO_PROPERTIES *audio )
{
serprintf("fadummy: open %d\n", audio->samplesPerSec );

	return 0;
}

int dummy_close( STREAM_FILTER_AUDIO *f ) 
{
serprintf("fadummy: close\n" );
	return 0;
}

int dummy_filter( STREAM_FILTER_AUDIO *f, AUDIO_FRAME *frame )
{
	return 0;
}

int dummy_flush( STREAM_FILTER_AUDIO *f )
{
serprintf("fadummy: flush\n" );
	return 0;
}

int dummy_delay( STREAM_FILTER_AUDIO *f )
{
	return 0;
}

STREAM_FILTER_AUDIO *stream_filter_audio_dummy_new( void ) 
{ 
	STREAM_FILTER_AUDIO *f = acalloc( 1, sizeof( STREAM_FILTER_AUDIO ) );
	
	if( !f )
		return NULL;

	static char name[] = "dummy";
	f->name    = name;
	f->delete  = dummy_delete;
	f->open    = dummy_open;
	f->close   = dummy_close;
	f->filter  = dummy_filter;
	f->flush   = dummy_flush;
	f->delay   = dummy_delay;

	return f;
}
