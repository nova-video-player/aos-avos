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

#ifndef _FILEINFO_H
#define _FILEINFO_H

#include "file_info_priv.h"
#include "browse.h"
#include "app_state.h"
#include "atf.h"

#include <time.h>
#include <string.h>

typedef struct folderinfo_str {
	UINT64		total_size;
	int 		photo;
	int 		video;
	int 		audio;
	int 		playlist;
	int 		otherfiles;
	int 		dir;
} FOLDER_INFO;

extern const KEYB_STATE INFO_actions;

BOOL InfoScreen_isVisible( void );
const char *InfoScreen_getParentName( void );

void Info_Exit( void );

void ExtractDurationInfo( int duration, unsigned int *days, unsigned int *hours, unsigned int *minutes, unsigned int *seconds );

void clear_info( FILE_INFO *info );

int  get_url_info         ( STREAM_URL *src, int type, int etype, FILE_INFO *info, APIC *apic, FILE_INFO_ABORT abort );
int  get_file_info         ( const char *path, int type, int etype, FILE_INFO *info, APIC *apic, FILE_INFO_ABORT abort );
int  get_file_info_clean   ( const char *path, int type, int etype, FILE_INFO *info, APIC *apic, FILE_INFO_ABORT abort );
int  get_file_info_no_crash( const char *path, int type, int etype, FILE_INFO *info );
int  get_file_info_server_launch( void );
void get_file_info_server_stop( void );
void file_info_dump_for_path( const char *path, int verbose );

#define FOLDER_INFO_OK		0
#define FOLDER_INFO_ERROR	1
#define FOLDER_INFO_ABORT	2

void AC_FilesizeToString( char *string, UINT64 size, int full );
void AC_DateTimeToString( char *buf, size_t n, time_t date, BOOL short_date, BOOL short_time );
void AC_DateToString( char *buf, size_t n, time_t _date );

void File_info_legacy_enter( void );
void File_info_enter( int fs, BROWSE_ENTRY *br, const char *full_path );
void File_info_video_enter( const char *full_path, VIDEO_ENTRY *vid );
void File_info_audio_enter( const char *full_path, BROWSE_ENTRY *br );

void File_info_box_draw( const char *name, const char *full_path, size_t mdate, int type, int etype, const FILE_INFO *info );

void FileInfoScreen_create( const char *name, const char *full_path, size_t mdate, int type, int etype, const FILE_INFO *info );
struct DView_str;
void FileInfoScreen_createFromDView( struct DView_str *view );

BOOL FileInfo_IsDisplayCompact( void );
int  FileInfo_LineToPixel( int line );
int  FileInfo_TagLineToPixel( int first_tag_line, int tag_line );
void FileInfo_DrawSeparator( int line );
int  FileInfo_CloseButtonPosX( void );
void FileInfo_getFormattedFilePath( const char *full_path, char *buffer, int buffer_size );

#endif	// _FILEINFO_H
