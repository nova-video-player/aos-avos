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

#ifndef _STREAM_SUBTITLE_H
#define _STREAM_SUBTITLE_H

#include "stream.h"

int  stream_sub_ext_has_new( STREAM *s );
int  stream_sub_ext_check( STREAM *s );
void stream_sub_ext_close( STREAM *s );
int  stream_sub_ext_get_subtitle_data( STREAM *s, VIDEO_FRAME **frame, int time );

//int stream_get_num_subtitle( STREAM *s );
//SUB_PROPERTIES *stream_get_subtitle_props( STREAM *s, int num );
VIDEO_FRAME *stream_get_current_subtitle( STREAM *s );

#define SUBTITLE_CHUNK 65536

#endif
