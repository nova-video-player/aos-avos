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

#ifndef _STREAM_ERROR_H
#define _STREAM_ERROR_H

/*
 * in sync with:
 *	avos/public/avos_mp.h
 */

typedef enum {
	VE_NO_ERROR            = 0,
	VE_FILE_ERROR          = 1,	// this is set to 1, as it also applies to the legacy "return 1" in ios, buffers and parsers...
	VE_ERROR               = 2,	
	VE_CONNECTION_ERROR    = 3,
	VE_USER_ABORT          = 4,
	VE_VIDEO_NOT_SUPPORTED = 5,
	VE_AUDIO_NOT_SUPPORTED = 6,
	VE_VIDEO_NOT_ALLOWED   = 7,
	VE_AUDIO_NOT_ALLOWED   = 8,
	VE_TOO_BIG_FOR_STREAM  = 9,
	VE_TOO_BIG_FOR_CODEC   = 10,
	VE_NOT_INTERLEAVED     = 11,
	VE_CRYPTED             = 12,
	VE_VIDEO_CODEC_ERROR   = 13,
} VIDEO_ERROR;

typedef enum {
	VEQ_NONE                                = 0,
	VEQ_MPG4_UNSUPPORTED                    = 1,
	VEQ_PROFILE_AND_LEVEL_UNSUPPORTED       = 2,
	VEQ_AUDIO_PROFILE_AND_LEVEL_UNSUPPORTED = 3,
	VEQ_INTERLACED_NOT_SUPPORTED            = 4,
	VEQ_SEE_DESCRIPTION                     = 5,
} VIDEO_ERROR_QUAL;

#endif
