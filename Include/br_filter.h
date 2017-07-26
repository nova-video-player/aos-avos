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

#ifndef BR_FILTER_H
#define BR_FILTER_H

#include "global.h"
#include "browse.h"

typedef int (*BR_FILTER_EXTRA)( BROWSE_ENTRY *br );

typedef enum {
	br_filter_none = 0,
	br_filter_audio,
	br_filter_audio_only,
	br_filter_audio_nodir,
	br_filter_photo,
	br_filter_photo_nodir,
	br_filter_video,
	br_filter_video_nodir,
	br_filter_info,
	br_filter_info_nodir,
	br_filter_pdf,
	br_filter_flash,
} br_filter_type_t;

int br_filter( BROWSE_ENTRY *br, br_filter_type_t f_type, BR_FILTER_EXTRA extra );

#endif // BR_FILTER_H
