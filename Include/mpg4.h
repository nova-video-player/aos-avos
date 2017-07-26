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

#ifndef _MPG4_H
#define _MPG4_H

#include "av.h"

int  MPG4_find_start_code( UCHAR *data, int size, int *type );
int  MPG4_findFrame  ( UCHAR *data, int ofs, int max );
int  MPG4_checkIFrame( UCHAR *data, int max, int *new );
void MPG4_fix_vol_header( UCHAR *data, int size );
int  MPG4_get_video_props( VIDEO_PROPERTIES *video, const UCHAR *data, int size );
int  MPG4_get_VOL_len( UCHAR *data, int size );
int  MPG4_get_frame_size( UCHAR *data, int size, int fix_user_data);
int  MPG4_get_extradata( VIDEO_PROPERTIES *video, UCHAR *data, int size );
uint8_t *MPG4_find_video_sc( uint8_t *p, int len );
#endif
