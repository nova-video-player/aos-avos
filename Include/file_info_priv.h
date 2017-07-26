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

#ifndef _FILE_INFO_PRIV_H
#define _FILE_INFO_PRIV_H

#include "id3tag.h"
#include "drm.h"
#include "av.h"
#include "image.h"
#include "stream_io.h"

#include <time.h>
#include <string.h>

typedef int (*FILE_INFO_ABORT) ( void * );

typedef struct FILE_INFO {
	int 		type;
	int 		etype;
	
	AUDIO_PROPERTIES *audio;
	VIDEO_PROPERTIES *video;
	SUB_PROPERTIES   *subtitle;

	AV_PROPERTIES	av;

	int 		duration;	// now in ms!
	int 		clippable;
	
	DRM_PROPERTIES	drm;
	
	ID3_TAG 	id3_tag;

	UINT64		size;
	time_t		date;
	
	char		full_path[MAX_NAME_LEN + 1];
} FILE_INFO;

typedef int (*FILE_INFO_PATH)( const char *path,           struct FILE_INFO *info, APIC *apic, FILE_INFO_ABORT abort );
typedef int (*FILE_INFO_IO)  ( STREAM_IO *io,              struct FILE_INFO *info, APIC *apic, FILE_INFO_ABORT abort );
typedef int (*FILE_INFO_MMAP)( UCHAR *buffer, UINT64 size, struct FILE_INFO *info, APIC *apic, FILE_INFO_ABORT abort );

#define __stringify_1(x)        #x
#define __stringify(x)          __stringify_1(x)

typedef struct FILE_INFO_REG {
	int 			type;
	int			etype;
	FILE_INFO_PATH		info_path;
	const char		*info_path_name;
	FILE_INFO_IO		info_io;
	const char		*info_io_name;
	FILE_INFO_MMAP		info_mmap;
	const char		*info_mmap_name;
	struct FILE_INFO_REG	*next;
} FILE_INFO_REG;

void file_info_register( FILE_INFO_REG *reg );
int  file_info_unregister( int type, int etype );

#define FILE_INFO_REGISTER_PATH( type, etype, path ) \
	static FILE_INFO_REG _fi_reg##type##etype = { \
		type,\
		etype,\
		path,\
		__stringify(path),\
		NULL,\
		"NULL",\
		NULL,\
		"NULL",\
		NULL\
	}; \
	static void _fi_reg_fn##type##etype( void ) __attribute__((constructor));\
	static void _fi_reg_fn##type##etype( void )\
	{ \
		file_info_register( &_fi_reg##type##etype ); \
	}

#define FILE_INFO_REGISTER_IO( type, etype, io ) \
	static FILE_INFO_REG _fi_reg##type##etype = { \
		type,\
		etype,\
		NULL,\
		"NULL",\
		io,\
		__stringify(io),\
		NULL,\
		"NULL",\
		NULL\
	}; \
	static void _fi_reg_fn##type##etype( void ) __attribute__((constructor));\
	static void _fi_reg_fn##type##etype( void )\
	{ \
		file_info_register( &_fi_reg##type##etype ); \
	}

#define FILE_INFO_REGISTER_MMAP( type, etype, mmap ) \
	static FILE_INFO_REG _fi_reg##type##etype = { \
		type,\
		etype,\
		NULL,\
		"NULL",\
		NULL,\
		"NULL",\
		mmap,\
		__stringify(mmap),\
		NULL\
	}; \
	static void _fi_reg_fn##type##etype( void ) __attribute__((constructor));\
	static void _fi_reg_fn##type##etype( void )\
	{ \
		file_info_register( &_fi_reg##type##etype ); \
	}



#endif
