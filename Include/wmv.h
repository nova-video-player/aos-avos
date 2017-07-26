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

#ifndef _WMV_H
#define _WMV_H

#include "av.h"
#include "cbe.h"

int WMV_get_video_props( VIDEO_PROPERTIES *video );

enum {
	WMV_PROFILE_SIMPLE 	= 0,
	WMV_PROFILE_MAIN 	= 1,
	WMV_PROFILE_COMPLEX 	= 2,
	WMV_PROFILE_ADVANCED 	= 3,
};	

unsigned char *WMV_get_rcv_header( VIDEO_PROPERTIES *video, int *size );
void VC1_add_sync( CBE *cbe, int *out_size );

#endif
