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
#include "types.h"
#include "astdlib.h"
#include "debug.h"
#include "subtitle.h"
#include "app_video.h"
#include "image.h"

#include "rect.h"
#include "stream.h"

#define DBG if(Debug[DBG_SUB])
#define ERR if(1)

#ifdef CONFIG_SUBTITLES

static int sub_num = 0;
static VIDEO_FRAME *current = NULL;
static int remove_time;

static AV_PROPERTIES *av_props = NULL;

static int lang_index = 0;

void subtitles_graphic_set_lang( int index )
{
	if( !av_props ) {
		// No subtitles in stream
ERR serprintf("%s invalid stream\n",__FUNCTION__);
		return;
	}
	lang_index = index;

	// clear old subs
	subtitles_text_update( "", "" );
	subtitles_graphic_update( NULL );

	Video_SetSubtitleStream( lang_index );

	if( av_props->subs != lang_index ) {
DBG serprintf("Subtitle stream change failed\n");
	}
}

int subtitles_graphic_open( AV_PROPERTIES *av )
{
DBG serprintf("%s: %d\r\n", __FUNCTION__, av->subs_max );
	av_props    = av;
	sub_num     = av->subs_max;
	remove_time = -1;

	if( av->subs_max ) {
		return 0;
	}

	return 1;
}

int subtitles_graphic_get_count( void )
{
	if( !av_props ) {
		return 0;
	}
	return av_props->subs_max;
}

const char *subtitles_graphic_get_item( int index, int *text )
{
	if( text )
		*text = 0;
		
	if( !av_props || index > av_props->subs_max ) {
		return "";
	}
	if( av_props->sub[index ].valid ) {
		if( text )
			*text = !av_props->sub[index].gfx;
		return av_props->sub[index].name;
	} else {
		return "";
	}
}

void subtitles_graphic_changed( VIDEO_FRAME *subs )
{
DBG serprintf("subtitles_graphic_changed: ");
	current = subs;
	if( current ) {
DBG		serprintf("%8d/%5d  %d x %d", current->time, current->duration, current->window.width, current->window.height);
	}
DBG	serprintf("\r\n");
}

void subtitles_graphic_close( void )
{
DBG serprintf("subtitles_graphic_close\r\n");
	sub_num = 0;
	current = NULL;
	av_props = NULL;
}

void subtitles_graphic_display( int time )
{
//DBG serprintf("subtitles_graphic_display( %d ) ", time );

	if( time == -1 || ( remove_time != -1 && time > remove_time ) ) {
		// remove all subs
		subtitles_graphic_update( NULL );
		subtitles_text_update( "", "" );

 		remove_time = -1;
	}

	if( !av_props || !sub_num || !current ) {
//DBG serprintf("none %08X %d %08X\r\n", av_props, sub_num, current );
		return;
	}
		
	if( time < current->time ) {
		// too early
//DBG serprintf("early\r\n");
		return;
	} else if( time > current->time + current->duration ) {
		// too late, drop it
		current = NULL;
//DBG serprintf("late\r\n");
		return;
	}

	// show it!

DBG serprintf("SUBTITLE_SHOW: %8d < %8d < %8d  ", current->time, time, current->time + current->duration);
	if( av_props->sub[av_props->subs].gfx ) {
		IMAGE cropped = image_crop( (IMAGE *)current, &current->window );
DBG serprintf("%3d/%3d  %3d/%3d\r\n", current->window.x, current->window.y, current->window.width, current->window.height);
		subtitles_graphic_update( &cropped );
	} else {
DBG serprintf("%s\r\n", current->data[0]);
		// currently, STREAM will export text subs as one single line in data, with \n being an escape code for a line break,
		// so we have to split here for displaying:
		char top[1024];
		char *t = top;
		
		char *d = current->data[0];
		char *bottom = "";
		
		while( *d ) {
			if( *d == '\\' && *(d + 1) == 'n' ) {
				// we found a line break, rest is bottom
				bottom = d + 2;
			
				// replace all further line breaks with spaces
				char *s = bottom;
				char *b = bottom;
				while( *s ) {
					if( *s == '\\' && *(s + 1) == 'n' ) {
						// we found a line break, replace
						*b++ = ' ';
						s += 2;
					} else {
						*b++ = *s++;
					}
				}
				*b = '\0';
				
				break;
			}
			*t++ = *d++;
		}
		*t = '\0';

		if( bottom ) {
		}
		
		subtitles_text_update( top, bottom );
	}
	
	remove_time = current->time + current->duration;

	current = NULL;
}

#endif	// CONFIG_SUBTITLES
