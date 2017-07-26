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

#ifndef _SUBTITLE_FILES_H_
#define _SUBTITLE_FILES_H_

int  subtitles_files_check    ( const char *path, const char *name, const char *lan );
void subtitles_files_close    ( void );
int  subtitles_files_get_count( void );
const char *subtitles_files_get_item( int lang );
void subtitles_files_set_lang ( int lang );
void subtitles_files_display  ( int s_time );
void subtitles_files_redraw   ( void );
#endif
