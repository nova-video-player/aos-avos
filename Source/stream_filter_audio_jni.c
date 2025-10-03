/*
 * Copyright 2025 phh
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
#include "astdlib.h"
#include <limits.h>

extern int (*libavos_transform_audio)(float* buf, int nsamples);

// WARNING: as of 2025-10-06, this function is never called
int stream_filter_audio_jni_delete( STREAM_FILTER_AUDIO *f )
{
	afree(f);
	return 0;
}

// WARNING: as of 2025-10-06, this function is never called
int stream_filter_audio_jni_open( STREAM_FILTER_AUDIO *f, AUDIO_PROPERTIES *audio )
{
	return 0;
}

// WARNING: as of 2025-10-06, this function is never called
int stream_filter_audio_jni_close( STREAM_FILTER_AUDIO *f )
{
	return 0;
}

int stream_filter_audio_jni_filter( STREAM_FILTER_AUDIO *f, AUDIO_FRAME *frame )
{
	if(!libavos_transform_audio) return 0;
	//Yes that's ugly but I'm too lazy
static float buf[96000];
	int samples = frame->size / 2;
	short *data = (short*) frame->data;
	for(int i=0; i < samples; i++) {
		buf[i] = data[i] / 32768.0;
	}
	libavos_transform_audio(buf, samples);
	for(int i=0; i < samples; i++) {
		int a = 32768 * buf[i];
		if (a < SHRT_MIN) a = SHRT_MIN;
		if (a > SHRT_MAX) a = SHRT_MAX;
		data[i] = a;
	}

	return 0;
}

// WARNING: as of 2025-10-06, this function is never called
int stream_filter_audio_jni_flush( STREAM_FILTER_AUDIO *f )
{
	return 0;
}

// WARNING: as of 2025-10-06, this function is never called
int stream_filter_audio_jni_delay( STREAM_FILTER_AUDIO *f )
{
	return 0;
}

STREAM_FILTER_AUDIO *stream_filter_audio_jni_new( void )
{
	STREAM_FILTER_AUDIO *f = acalloc( 1, sizeof( STREAM_FILTER_AUDIO ) );

	if( !f )
		return NULL;

	static char name[] = "stream_filter_audio_jni";
	f->name    = name;
	f->delete  = stream_filter_audio_jni_delete;
	f->open    = stream_filter_audio_jni_open;
	f->close   = stream_filter_audio_jni_close;
	f->filter  = stream_filter_audio_jni_filter;
	f->flush   = stream_filter_audio_jni_flush;
	f->delay   = stream_filter_audio_jni_delay;

	return f;
}
