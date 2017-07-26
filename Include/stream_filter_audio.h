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

typedef struct STREAM_FILTER_AUDIO {
	const char	     *name;
	FILTER_AUDIO_DELETE  delete;
	FILTER_AUDIO_OPEN    open;
	FILTER_AUDIO_CLOSE   close;
	FILTER_AUDIO_FILTER  filter;
	FILTER_AUDIO_FLUSH   flush;
	FILTER_AUDIO_PARAM   set_param;
	FILTER_AUDIO_DELAY   delay;

	void		*priv;
} STREAM_FILTER_AUDIO;

#endif
