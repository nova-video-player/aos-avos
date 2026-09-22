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

#ifndef _STREAM_FILTER_AUDIO_H
#define _STREAM_FILTER_AUDIO_H

#include "av.h"

struct STREAM_FILTER_AUDIO;

typedef int (*FILTER_AUDIO_DELETE) ( struct STREAM_FILTER_AUDIO *f );
typedef int (*FILTER_AUDIO_OPEN )  ( struct STREAM_FILTER_AUDIO *f, AUDIO_PROPERTIES *audio );
typedef int (*FILTER_AUDIO_CLOSE)  ( struct STREAM_FILTER_AUDIO *f );
typedef int (*FILTER_AUDIO_FILTER) ( struct STREAM_FILTER_AUDIO *f, AUDIO_FRAME *frame );
typedef int (*FILTER_AUDIO_FLUSH)  ( struct STREAM_FILTER_AUDIO *f );
typedef int (*FILTER_AUDIO_PARAM)  ( struct STREAM_FILTER_AUDIO *f, void *params , void *night_on);
typedef int (*FILTER_AUDIO_DELAY)  ( struct STREAM_FILTER_AUDIO *f );
// Optional bounded output drain: end=0 emits queued complete frames, end=1
// finishes the filter. Returns <0 on error, otherwise frame->size=0 when empty.
// Consume each returned frame before calling filter/drain again.
typedef int (*FILTER_AUDIO_DRAIN)  ( struct STREAM_FILTER_AUDIO *f, AUDIO_FRAME *frame, int end );

typedef struct STREAM_FILTER_AUDIO {
	const char	     *name;
	FILTER_AUDIO_DELETE  delete;
	FILTER_AUDIO_OPEN    open;
	FILTER_AUDIO_CLOSE   close;
	FILTER_AUDIO_FILTER  filter;
	FILTER_AUDIO_FLUSH   flush;
	FILTER_AUDIO_PARAM   set_param;
	FILTER_AUDIO_DELAY   delay;
	FILTER_AUDIO_DRAIN   drain;
	// Common acoustic reference delay in microseconds at the PCM sample rate.
	// Distinct from queued programme duration returned by delay().
	int (*signal_delay_us)(struct STREAM_FILTER_AUDIO *f);

	// Optional software-speed timing interface. Indices count interleaved PCM
	// frames, not bytes or channel samples. Consume the returned PCM and its
	// checkpoint before the next filter/drain call. Sink writes may split it.
	int (*get_output_state)(struct STREAM_FILTER_AUDIO *f, UINT64 *frames,
	                        int *queued, int *rate);
	int (*take_speed_commit)(struct STREAM_FILTER_AUDIO *f, float *speed,
	                         UINT64 *boundary);
	int (*lookup_output_media)(struct STREAM_FILTER_AUDIO *f, UINT64 start,
	                           int frames, INT64 *media_frames, int *rate);

	void		*priv;
} STREAM_FILTER_AUDIO;

#endif
