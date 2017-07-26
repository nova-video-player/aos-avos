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

#ifndef _STREAM_AVG_H_
#define _STREAM_AVG_H_

#include "atime.h"

typedef struct TAVG {
	int last_time;
	int time;
	int time_sum;
	int data_sum;
	int count;
	int time_avg;
	int time_max;
	int data_avg;
	int samples;
} STREAM_AVG;

static void UNUSED clear_avg( STREAM_AVG *avg )
{
	memset( avg, 0, sizeof( STREAM_AVG ) ); 
	avg->last_time = time_update_time();
}

static void UNUSED do_avg( STREAM_AVG *avg, int time, int data )
{
	if( time == -1 ) {
		// do automatic time
		int now = time_update_time();
		time = now - avg->last_time;
		avg->last_time = now;
	}
	avg->time      = time;
	avg->time_sum += time;
	avg->data_sum += data;
	avg->count ++;
	
	if ( avg->count >= 100 ) { 
		avg->time_avg = avg->time_sum / 100; 
		avg->time_sum = 0; 
		avg->data_avg = avg->data_sum / 100; 
		avg->data_sum = 0; 
		avg->count = 0; 
	}
}

static void UNUSED do_avg_audio( STREAM_AVG *avg, int time, int samples, AUDIO_PROPERTIES *audio )
{
	#define SAMPLE_TIME 8	
	avg->time      = time;
	avg->time_sum += time;
	avg->samples  += samples;
	
	if( avg->samples > SAMPLE_TIME * audio->samplesPerSec ) {
		avg->time_avg = ((UINT64)avg->time_sum * (UINT64)audio->samplesPerSec) / avg->samples;
		avg->time_avg /= SAMPLE_TIME;
		if( avg->time_avg > avg->time_max )
			avg->time_max = avg->time_avg;
		avg->samples  = 0;
		avg->time_sum = 0;
	}
}

#endif
