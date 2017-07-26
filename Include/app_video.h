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

#ifndef _APP_VIDEO_H_
#define _APP_VIDEO_H_

#include "types.h"

extern int video_play_mode;
extern int video_nrs_mode;
extern int video_display_format;

enum {
	VIDEO_OSD_OFF = 0,
	VIDEO_OSD_STATUSBAR,
	VIDEO_OSD_STATUSBAR_ONLY,
	VIDEO_OSD_PROGRESSBAR,
	VIDEO_OSD_VOLUMEBAR,
	VIDEO_OSD_MENUBAR,
	VIDEO_OSD_MENUBAR_OFF,
	VIDEO_OSD_FORMATBAR,
	VIDEO_OSD_FORMATBAR_OFF,
	VIDEO_OSD_SPEEDBAR,
	VIDEO_OSD_SPEEDBAR_OFF,
	VIDEO_OSD_FULL
};

#define VIDEO_CHAPTER_NICE_DVIEW

// ************************************************************
// FUNCTIONS
// ************************************************************

void Video_SetAudioStream( int stream );
void Video_SetSubtitleStream( int stream );
void Video_SetChapter( int chapter );

void Video_SetOSDState( int mode, BOOL fade );

struct STREAM;

#include "stream_resizer.h"
#define DEFAULT_VIDEO_DISPLAY_FORMAT DISPFMT_FULL_SCREEN

#endif	// _APP_VIDEO_H_
