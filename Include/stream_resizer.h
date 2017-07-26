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

#ifndef _STREAM_RESIZER_H
#define _STREAM_RESIZER_H

#include "av.h"
#include "rect.h"

struct STREAM_RESIZER;

enum {
	DISPFMT_ORIGINAL_PICTURE,
	DISPFMT_FULL_SCREEN,
};

typedef struct STREAM_SCREEN_PARAMS
{
	RECT	screen;
	int     aspect_n; 
	int     aspect_d; 
	int     rotation; 
	float   zoom;
	int 	format;
	
} STREAM_SCREEN_PARAMS;

typedef struct STREAM_RSZ_PARAMS
{
	int     rotation; 
	
	RECT	src;
	RECT	dst;
	
} STREAM_RSZ_PARAMS;

typedef int (*STREAM_RESIZER_SETUP )  ( struct STREAM_RESIZER *r, STREAM_SCREEN_PARAMS *screen, VIDEO_PROPERTIES *video, STREAM_RSZ_PARAMS *rsz );

typedef struct STREAM_RESIZER {
	const char	       *name;
	STREAM_RESIZER_SETUP    setup;
} STREAM_RESIZER;

extern STREAM_RESIZER video_resizer_DSS;

#endif
