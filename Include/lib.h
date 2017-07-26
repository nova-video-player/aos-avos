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

#ifndef _LIB_H
#define _LIB_H

#include "browse.h"
#include "id3tag.h"
#include <time.h>

#define Q_AUD		0x40000000
#define Q_PIC		0x20000000
#define Q_VID		0x10000000

#define Q_GENRE		0x08000000
#define	Q_YEAR		0x04000000
#define	Q_ARTIST	0x02000000
#define Q_ALBUM		0x01000000
#define Q_ALB_ART	0x00800000
#define Q_RATING	0x00400000
#define Q_TITLE		0x00200000
#define Q_PLAYLIST	0x00100000
#define Q_RADIO		0x00080000
#define Q_PODCAST	0x00040000

// Photo
#define Q_P_YEAR	0x08000000
#define Q_P_MONTH	0x04000000
#define Q_PHOTO		0x02000000
#define Q_PHOTO_N	0x01000000

// Video
#define Q_V_YEAR	0x08000000
#define Q_V_MONTH	0x04000000
#define Q_VIDEO		0x02000000
#define Q_VIDEO_N	0x01000000

#define Q_QUERY_MASK	0xFFFF0000
#define Q_FILE_MASK	0x0000FFFF

#define LIB_GET_QUERY( lba ) (lba & Q_QUERY_MASK)
#define LIB_GET_FILE( lba )  (lba & Q_FILE_MASK)

#define ARCLIB_OFF	0
#define ARCLIB_ON	1
#define ARCLIB_AUTO	2

extern int ARCLIB_mode;
extern int ARCLIB_dirty;

#define LIB_M3U_SAVE_DEFAULT	"Playlists"

#define ARCLIB_FILENAME		"NDX8.AVX"

#define ARCLIB_PATH 		HDD_SYSTEM""ARCLIB_FILENAME

// these GAAT_xxx have an index in the library
#define GAAT_GENRE		0
#define GAAT_DATE		1
#define GAAT_ARTIST		2
#define GAAT_ALBUM		3
#define GAAT_TITLE		4
#define GAAT_TRACK		5

#define GAAT_NUM		6

#define GAAT_RATING		10

#define FLG_UNKNOWN_ALBUM	0x01
#define FLG_UNKNOWN_ARTIST	0x02

#define PATH_NUM		6

typedef struct lib_file_str {
	int 		path[PATH_NUM];
	char		ext[4];
	int 		gaat[GAAT_NUM];
	
	unsigned int 	type		:8;
	unsigned int	etype		:8;	

	unsigned int 	rating		:7;
	unsigned int 	level		:4;

	unsigned int 	flags		:2;
	unsigned int	used		:1;
	unsigned int 	deleted		:1;
	unsigned int 	path_track	:1;
	unsigned int 	on_card		:1;
	unsigned int 	purchase	:1;
	unsigned int    adult		:1;

	unsigned int	mhandle;

	time_t 		date;
	int 		size;
	int 		duration;
	
	unsigned short int idx_gaat[GAAT_NUM];
} LIB_FILE;

typedef struct hdr_str {
	char 			magic[8];
	unsigned int 		version;
	unsigned int 		num_files;
	unsigned int 		offset_files;
	unsigned int 		offset_strings;
	unsigned int 		offset_private_data;

	char			reserved[484];
} LIB_HEAD;

#define HEAD_SIZE 	sizeof( struct hdr_str )

#define ARCLIB_VERSION	0x00000503

#define STRING_CHUNK	4

#define LIB_MARGIN	4096

extern LIB_FILE		*lib_files;
extern int 		lib_file_num;

extern char 		*lib_strings;
extern int 		lib_string_size;

extern int		gaat_num[GAAT_NUM];

extern int 		LIB_loaded;
extern int 		LIB_present;
extern int 		LIB_too_big;

typedef USHORT* LIB_INDEX;

typedef struct LIB_Q {
	int query;
	int file;
	int type;
	int root;
	int browser;
	int allfiles;
	int total_duration;
	BROWSE_ENTRY br;
	LIB_INDEX index;
	char filter[MAX_NAME_LEN + 1];
	int filter_mask;
} LIB_Q;

typedef int (*LIB_CALLBACK)( void* listener, const char* msg );

void LIB_ForceType( int type );

void LIB_Index( int rebuild, LIB_CALLBACK callback, void* listener );
int LIB_Check( int *update );
int  LIB_Load( void );
int  LIB_GetSize( void );
void LIB_UnLoad( void );
void LIB_Delete( void );
void LIB_CheckAndUpdate( void );

LIB_Q		*LIB_AllocQuery( int browser );
void		LIB_FreeQuery(   LIB_Q *lib_q );

int		LIB_GetDir_myQ(    int level, int lba, LIB_Q *lib_q );
BROWSE_ENTRY	*LIB_DirEntry_myQ( int pos, LIB_Q *lib_q );
BROWSE_ENTRY	LIB_GetEntry_myQ(  int pos, const LIB_Q *lib_q );
void		LIB_GetPath_myQ (  int pos, const LIB_Q *lib_q, char *path );
char		*LIB_GetGAAT_myQ(  int pos, const LIB_Q *lib_q, int gaat, char *string );

int		LIB_set_filter( LIB_Q *lib_q, char *name, int mask );
int	        LIB_HasFiles(   int lba );
int		LIB_urlToLba(   const char *url );

int		LIB_get_real_path( char *full_path, int handle );
void		LIB_get_virtual_path( char *virtual_path, const char *url );
void		LIB_get_handle_path( int type, char *handle_path, const char *virtual_path );
LIB_FILE	*LIB_check_file(   const char *path, int *dir, int *level, int *lba );

int  LIB_get_file_tag( int file, ID3_TAG *tag );
int  LIB_get_query_etype( int query );

void LIB_Update( int rebuild, LIB_CALLBACK callback, void* listener );
int  LIB_UpdateAllowed( void );

void LIB_CheckDirty( const char *full_path );
void LIB_ForceDirty( void );

#define LIB_FLAG_DELETED	0x0001
#define LIB_FLAG_ADULT		0x0002

void LIB_Flag(        const char *full_path, int flag, int value ); 
void LIB_AddPlaylist( const char *full_path, const char *name );
int  LIB_SetRating  ( const char *full_path, int rating );
int  LIB_GetRating  ( const char *full_path );
void LIB_DumpEntry( int i );


#endif	// _LIB_H
