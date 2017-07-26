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

// ****************************************************
//
// 	ATF functions : parse & decode
//
// ****************************************************
#ifndef _ATF_H
#define _ATF_H

#include "image.h"
#include "id3tag.h"

#define ARCHOS_THUMB_DIRNAME		".arcthumb"
#define ARCHOS_USB_THUMB_DIRNAME	".usb_thumbs"
#define ARCTHUMB_FILE_EXTENSION		"ATF"
#define ARCTHUMB_BMP_FILE_EXTENSION	"BMP"

// increase here if you want to force ATF creation
// The reason this magic is so strange is that 0x4154460a == ATF in Ascii
#define ATF_MAGIC		0x41544612

#define ATF_PHOTO_VERSION	0x00000002
#define ATF_AUDIO_VERSION	0x00000002
#define ATF_VIDEO_VERSION	0x00000002

#define ATF_MAX_IDX 64

typedef struct atf_video_infos_str {
	/* WARNING: increase ATF_VIDEO_VERSION if you change something here */
	int		version;
	int		valid;
	UINT32		drm;
	int		duration;
	int		width;
	int		height;
	UINT32	 	vid_format;
	int		vid_kbps;
	int		aud_format;
	int		aud_kbps;
	int		aspect_n;
	int		aspect_d;
	int		idx_size;		// size of index data entry
	unsigned char 	idx_data[ATF_MAX_IDX];	// for videos, index entry data to start stream from
	UINT32		start_time;		// For animated video thumbs
	UINT32		stop_time;		// For animated video thumbs
	int 		purchase;
} ATF_VIDEO_INFOS;

typedef struct atf_photo_infos_str {
	/* WARNING: increase ATF_PHOTO_VERSION if you change something here */
	int		version;
	int		valid;
	int		width;
	int		height;
	int		exif_valid;
	ULONG		exposure_time_num;	
	ULONG		exposure_time_den;
	ULONG		aperture;
	UINT16		rotation;		// 0 / 90 / 180 / 270
} ATF_PHOTO_INFOS;

typedef struct atf_audio_infos_str {
	/* WARNING: increase ATF_AUDIO_VERSION if you change something here */
	int		version;
	int		valid;
	UINT32		drm;
	int		duration;
	int		vbr;
	int		bytes_per_sec;
	int		bits_per_sample;
	int		samples_per_sec;
	int		id3_tag;
	char 		title[MAX_TAG_LENGTH + 1];
	char 		artist[MAX_TAG_LENGTH + 1];
	char 		album[MAX_TAG_LENGTH + 1];
	char 		year[ID3_YEAR_LENGTH + 1];
} ATF_AUDIO_INFOS;

typedef struct atf_infos_str {
	UINT32		magic;
	UINT16		error_count;	// Error counter
	UINT32		parent_size;	// size of parent image
	time_t		parent_date;	// creation date of parent image
	UINT16		width;		// Width of thumbnail
	UINT16		height;		// Height of thumbnail
	ATF_VIDEO_INFOS	video;		// specific video informations
	ATF_PHOTO_INFOS	photo;		// specific photo informations
	ATF_AUDIO_INFOS	audio;		// specific audio informations
} ATF_INFOS;

#define ATF_INFOS_SIZE    sizeof( ATF_INFOS )

int atf_save( const char * full_path, ATF_INFOS * infos, IMAGE *image, int sync );

int atf_get_infos_from_file( const char *full_path, ATF_INFOS * infos );

int atf_build_full_path( char *atf_path, const char *image_path, int max );
int atf_bmp_build_full_path( char *atf_bmp_path, const char *image_path, int max );

#endif // _ATF_H
