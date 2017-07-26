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

// *****************************************************
//
// 	BROWSE.H
//
//	all browser related stuff
//
// *****************************************************

#ifndef _BROWSE_H
#define _BROWSE_H

#include "global.h"
#include "file.h"

#ifndef HDD_ROOT
	#define HDD_ROOT	"/"
#endif

// ************************************************************
// BROWSER
// ************************************************************
#define STRING_MAX		MAX_NAME_LEN

typedef struct browse_entry
{
	char 		name[MAX_NAME_LEN + 1];	// Fancy user oriented name (could also be VFAT long name)
	
	unsigned int 	type;		// type and extended type for GUI application
	unsigned int 	etype;		// ...

	unsigned long 	handle;		// handle for no path oriented FS, ARCLIB, PTP etc..
	unsigned long long size;	// size in bytes of files

	unsigned long	mhandle;	// MTP Handle of the file
	
	unsigned int	fs;		// file system of this file

	unsigned int 	drm	 : 1;	// drm protected
	unsigned int	typed	 : 1;	// this entry has type and etype set
	unsigned int	playable : 1;	// this file/folder is playable or can be added to a playlist
	unsigned int	purchase : 1;	// this file has been purchased
	unsigned int	showtime : 1;	// enough data to play this file (progressive playback)
	unsigned int	dl_status: 4;	// download status for purchased files
	unsigned int	adult    : 1;
	unsigned int	resume   : 1;
	
	time_t		mdate;		// modification date/time
	time_t		cdate;		// creation     date/time

} BROWSE_ENTRY;

typedef struct video_entry
{
	BROWSE_ENTRY	br;

	char		simple_name[MAX_NAME_LEN + 1];
	int  		dl_progress;
	int  		dl_time_left;
} VIDEO_ENTRY;

typedef struct BROWSE_DIR {
	DIR 		*dir;
	int 		fd;
	BROWSE_ENTRY	current;
	int 		verbose;
	char		path[MAX_NAME_LEN + 1];
	int		plen;
} BROWSE_DIR;

struct path_entry
{
	BROWSE_ENTRY br;	// use full BROWSE_ENTRY so we get type/etype etc..
	int	handle;		// for ARCLIB and other virtual filesystems
	int	offset;		// which folder in this level did we enter?
};

#define MAX_PATH_DEPTH		10	// mamximum depth of path on HD
typedef struct path_str
{
	int			fs;			// The filesystem the path refers to
	char			root[MAX_NAME_LEN + 1];	// Name of root directory
	int			level;			// Where we are in the path : 0 = root
	struct path_entry	path[MAX_PATH_DEPTH];
	BROWSE_ENTRY		current;
	unsigned long int	handle;			// this is for left right
} PATH;

int  		check_extension( const char *name, const char *ext );
const char 	*cut_n_extension_r( const char *name_ext, char *name, const int max );
const char 	*cut_extension_r( const char *name_ext, char *name );
const char 	*cut_extension( const char *name_ext );
const char 	*get_n_extension_r( const char *name_ext, char *ext, const int max );
const char 	*get_extension_r( const char *name_ext, char *ext );
const char 	*get_extension( const char *name_ext );
int 		br_cut_extension( const BROWSE_ENTRY * br, char * name,  const int max );
const char 	*cut_path( const char *path_name );
const char 	*cut_path_r( const char *path_name, char *file, int max );
const char 	*cut_n_file_r( const char *path_and_file, char *path, const int max );
const char 	*cut_file_r( const char *path_and_file, char *path );
const char 	*cut_file( const char *path_and_file );
const char 	*cut_path_and_extension( const char *path_name_ext );
int  		split_path( char *path, char *name, const char *full );
char 		**break_path( const char *root, const char *_path, int *_num );
int  		path_add_path( char *path, const char *add, int max );
int  		path_add_file( char *path, const char *add, int max );
int  		make_path_unique(const char * path, char * name);
int  		is_local_file( const char *name );
int  		is_dot_file( const char *name );
int 		is_hidden_file_or_dir( BROWSE_ENTRY* br, int fs, int level);
const char 	*cut_slash( const char *path );
int 		check_root( const char *path );
int  		browse_entry_from_path( BROWSE_ENTRY *br, const char *full_path, const char *file_name );
void 		browse_entry_from_stat( BROWSE_ENTRY *br, STAT *st, const char *name );
void 		browse_set_file_type( BROWSE_ENTRY *entry, const char *path  );

int		full_path_from_PATH( char *full_path, const PATH *p, const BROWSE_ENTRY *br, int max );
int		path_from_PATH( char *path, const PATH *p, int max );
const char	*path_from_url( const char *url );
int		parent_path_from_PATH( char *path, const PATH *p, int max, int level );
int		parent_path_from_PATH_CARD( char *path, const PATH *p, int max, int level );
int		browse_get_url( char *url, const PATH *path, const BROWSE_ENTRY *br, int max );
int		browse_calc_level( const char *path );

int 		CheckHiddenDir( char *name, int level, int fs );
int 		CheckHiddenFile( char *name, int fs );


#endif	// _BROWSE_H_
