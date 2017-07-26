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

#ifndef _STREAM_COMMON_H
#define _STREAM_COMMON_H

#include "drm.h"

typedef int  (*ABORT_HANDLER)    ( void *ctx );

typedef void (*STREAM_GET_PART_NAME) ( char *part_name, const char *full_path, int part_num );

typedef struct stream_part {
	UINT32	pad_size;		// padded size of this section
	UINT32	real_size;		// real size of this section
	UINT32	start_skip;		// number of bytes to skip at start
} STREAM_PART;

#endif
