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

#ifndef CONFIG_SUBTITLE_H
#define CONFIG_SUBTITLE_H

#include "av.h"

#define SUBTITLE_DEFAULT_LANG	"English"

#define SUBTITLE_MENU_STR	"subt_file_sel"
#define SUBTITLE_MENU_FONT_STR	"subt_font_sel"

extern int  subtitles_font_index;
extern int  subtitles_time_offset;

int  subtitles_init      ( void );
int  subtitles_add       ( AV_PROPERTIES *av );
void subtitles_close     ( void );
int  subtitles_get_count ( void );
const char *subtitles_get_name( int lang, int *text );
void subtitles_set_lang  ( int lang );
int  subtitles_get_lang  ( int *text);
void subtitles_set_font_index( int index );
int  subtitles_get_font_index( void );
void subtitles_display   ( int s_time );
void subtitles_redraw    ( void );
Rect subtitles_get_area( void );

void subtitles_graphic_changed( VIDEO_FRAME *subs );

void subtitles_show          ( void );
void subtitles_text_update   ( const char *top, const char *bottom );
void subtitles_graphic_update( const IMAGE *img );
void subtitles_clear         ( void );

#endif // CONFIG_SUBTITLE_EXPORT
