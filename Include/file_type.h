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

#ifndef _FILE_TYPE_H
#define _FILE_TYPE_H

#include "browse.h"
#include "stream_io.h"

#define __stringify_1(x)        #x
#define __stringify(x)          __stringify_1(x)

typedef int (*FILETYPE_ACTION_PATH) ( BROWSE_ENTRY *br, const char *url );

struct FILETYPE_REG;

typedef struct FILETYPE_REG {
	int				type;
	const FILETYPE_ACTION_PATH 	action;
	const char			*name;
	const char			*default_dir;
	struct FILETYPE_REG	*next;
} FILETYPE_REG;

int filetype_register( FILETYPE_REG *reg );
FILETYPE_REG* filetype_get_reg( int type );

#define FILETYPE_REGISTER( type, action, default_dir ) \
		static FILETYPE_REG _reg_path##type##action = { \
			type, \
			&action,\
			__stringify(action), \
			default_dir,\
			NULL\
		}; \
		static void _fn_reg_##type##action( void ) __attribute__((constructor));\
		static void _fn_reg_##type##action( void )\
		{ \
			filetype_register( &_reg_path##type##action ); \
		}

int 		get_file_type         ( const char *name, int *_type, int *_etype ); 
int 		get_file_type_no_probe( const char *name, int *_type, int *_etype ); 
int 		get_file_type_from_ext( const char *ext, int *_type, int *_etype, const char **_mime, int *probe ); 
int 		get_file_type_and_mime( const char *name, int *_type, int *_etype, const char **_mime ); 
int 		get_url_type  ( STREAM_URL *src, int *_type, int *_etype ); 
int 		get_url_type_and_mime( STREAM_URL *src, int *_type, int *_etype, const char **_mime ); 
int 		get_file_type_from_mime_type(const char *mime_type, int *type, int *etype);
int 		get_mime_type_from_file_type(int type, int etype, const char **mime);
int		detect_file_type(const char *name, const char *mime_type, int *_type, int *_etype);

const char 	*get_type_name( int type, int etype );

enum {
	FILE_NO_ERROR = 0,
	FILE_ERROR,
	FILE_EMPTY,
	FILE_NOT_PLAYABLE,
	FILE_PLAYLIST_NOT_LOCAL,
	FILE_UPDATE_NOT_LOCAL,
	FILE_UPDATE_NETWORK_STORAGE,
	FILE_PTP_VID,
	FILE_UNKNOWN
};

#endif
