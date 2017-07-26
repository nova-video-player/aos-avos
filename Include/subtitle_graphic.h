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

#ifndef _SUBTITLE_GRAPHIC_H_
#define _SUBTITLE_GRAPHIC_H_

#include "av.h"

int  subtitles_graphic_open     ( AV_PROPERTIES *av );
void subtitles_graphic_close    ( void );
void subtitles_graphic_display  ( int time );
int  subtitles_graphic_get_count( void );
const char *subtitles_graphic_get_item( int lang, int *text );
void subtitles_graphic_set_lang ( int lang );

#endif
