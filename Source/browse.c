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

#include "global.h"
#include "debug.h"		
#include "atime.h"
#include "file.h"
#include "browse.h"
#include "file_type.h"
#include "astdlib.h"
#include "av.h"
#include "util.h"
#include "fs.h"
#include "lib.h"
#include "atf.h"
#include "dirname.h"

#include <stdlib.h> // for rand()
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h> // for snprintf

#define ERR if(1)
#define DBG if(0)

// ************************************************
//
//	is_dot_file
//
//	return whether file is "." or "..""
//
// ************************************************
int is_dot_file( const char *name )
{
	return( !strcmp( name, "." ) || !strcmp( name, ".." ) );
}

// ************************************************
//
//	is_hidden_dir_or_file
//
//	return whether file is "hidden" or a system directory
//
// ************************************************
int is_hidden_file_or_dir( BROWSE_ENTRY* br, int fs, int level)
{
	if ( br->type == TYPE_DIR ) {
		if ( CheckHiddenDir( br->name, level, fs ) )
			return 1;
	} else { // skip "hidden" files
		if ( CheckHiddenFile( br->name, fs ) )
			return 1;
	}
	return 0;
}


// ************************************************
//
//	is_local_file
//
//	return whether file is from local drive or
//	somewhere else (USB/NET)
//
// ************************************************
int is_local_file( const char *name )
{
	struct stat buf;
	dev_t file_dev;

	// does name start with fd:// ?
	if (strncasecmp(name, FD_ROOT, strlen(FD_ROOT)) == 0)
		return 1;

	if (stat(name, &buf) != 0)
		return 0;
	file_dev = buf.st_dev;

	// is file in main storage ?
	if (stat(HDD_ROOT, &buf) != 0)
		return 0;
	if (file_dev == buf.st_dev)
		return 1;

	// is file in sdcard ?
	if (stat(CARD_ROOT, &buf) != 0)
		return 0;
	if (file_dev == buf.st_dev)
		return 1;

	return 0;
}

// ************************************************
//
//	check_extension
//
//	check if a given path/nam has a given extension
//
// ************************************************
int check_extension( const char *name, const char *ext )
{
	int name_len;
	int ext_len;

	if( !name || !ext )
		return 0;
		
	name_len = strlen( name );
	ext_len  = strlen( ext );
	
	if( name_len < ext_len + 2 )
		return 0;
	if( name[name_len - ext_len - 1] == '.' && !strcmpNC( name + name_len - ext_len, ext ) )
		return 1;
	return 0;	
}

// ************************************************
//
//	cut_extension
//
//	remove extension (if present) from a name with extension
//
// ************************************************
const char *cut_n_extension_r( const char *name_ext, char *name, const int max )
{
	char *n;
	
	strnZcpy( name, name_ext, max );
	
	n = name + strlen( name ) - 1;
	
	while( n > name && *n != '.' )
		n--;

	if( n > name )
		*n = '\0';
	
	return name;
}

const char *cut_extension_r( const char *name_ext, char *name )
{
	return cut_n_extension_r( name_ext, name, MAX_NAME_LEN );
}

const char *cut_extension( const char *name_ext )
{
	static char name[MAX_NAME_LEN + 1];
	return cut_extension_r( name_ext, name );
}

// ************************************************
//
//	get_extension
//
//	get extension (if present) from a name with extension
//
// ************************************************
const char *get_n_extension_r( const char *name_ext, char *ext, const int max )
{
	const char *n = name_ext + strlen( name_ext ) - 1;

	while( n > name_ext && *n != '.' )
		n--;

	if( n == name_ext )
		strcpy( ext, "" );
	else 
		strncpy( ext, n + 1, max );

	return ext;
}

const char *get_extension_r( const char *name_ext, char *ext )
{
	return get_n_extension_r( name_ext, ext, MAX_NAME_LEN );
}

const char *get_extension( const char *name_ext )
{
	const char *n = name_ext + strlen( name_ext ) - 1;

	while( n > name_ext && *n != '.' )
		n--;

	if( n == name_ext ) // no extension found, return the '\0'
		n = name_ext + strlen( name_ext );
	else
		n++;

	return n;
}

// ************************************************
//
//	br_cut_extension
//
//	Cut extension if needed (it depends on the file system type)
//
//	return 1 if the extension was cut else 0
// ************************************************
int br_cut_extension( const BROWSE_ENTRY * br, char * name,  const int max )
{
	if( !br || !name )
		return 0;

	//cut file extension if needed
	if( br->fs == DEV_UPNP || br->fs == DEV_ARCLIB ) {
		//No need to cut file extension
		strnZcpy( name, br->name, max );
		return 0;
	} else {
		//Cut file extension
		cut_n_extension_r( br->name, name, max );
		return 1;
	}
}

// ************************************************
//
//	cut_path
//
//	return just the filename without a leading path
//
// ************************************************
const char *cut_path( const char *path_name )
{
	const char *n = path_name + strlen( path_name ) - 1;

	while( n >= path_name && *n != '/' )
		n--;

	return n + 1;
}

// ************************************************
//
//	cut_path_r
//
//	return just the filename without a leading path
//
// ************************************************
const char *cut_path_r( const char *path_name, char *file, int max )
{
	const char *n = path_name + strlen( path_name ) - 1;

	while( n >= path_name && *n != '/' )
		n--;

	strnZcpy(file, n + 1, max);

	return file;
}

// ************************************************
//
//	cut_file
//
//	return just the path without a trailing file
//
// ************************************************
const char *cut_n_file_r( const char *path_and_file, char *path, const int max )
{
	char *p;
	
	strnZcpy( path, path_and_file, max );
	
	p = path + strlen( path ) - 1;
	
	while( p > path && *p != '/' )
		p--;
	p[1] = '\0';
	
	return path;
}

const char *cut_file_r( const char *path_and_file, char *path )
{
	return cut_n_file_r( path_and_file, path, MAX_NAME_LEN );
}

const char *cut_file( const char *path_and_file )
{
	static char path[MAX_NAME_LEN + 1];
	return cut_n_file_r( path_and_file, path, MAX_NAME_LEN );
}

// ************************************************
//
//	cut_path_and_extension
//
//	return just the filename without path and extension
//
// ************************************************
const char *cut_path_and_extension( const char *path_name_ext )
{
	static char name[MAX_NAME_LEN + 1];
	return cut_extension_r( cut_path( path_name_ext ), name );
}

// ************************************************
//
//	cut_slash
//
//	return path without the trailing slash
//
// ************************************************
const char *cut_slash( const char *_path )
{
	static char path[MAX_NAME_LEN + 1];
	int len;
	// remove possible trailing "/"s
	strnZcpy( path, _path, MAX_NAME_LEN );
	
	len = strlen(path);
	while ( path[len - 1] == '/' ) {
		path[len - 1] = '\0';
		len --;
	}	
	
	return path;
}

// ************************************************
//
//	split_path
//
//	split "full_path" into "path/" and "name"
//
// ************************************************
int split_path( char *path, char *name, const char *full )
{
	int 		path_len;
	int 		len = strlen( full );
	const char 	*n = full + len - 1;
	
	// omit traling slash
	if( *n == '/' ) {
		n--;
		len --;
	}
	
	path_len = len;
	
	while( n > full && *n != '/' ) {
		n--;
		path_len --;
	}
	
	if( path )
		strnZcpy( path, full, path_len );
	if( name )
		strnZcpy( name, full + path_len, len - path_len );
//serprintf("SPLIT %d %d %s -> [%s %s]\r\n", len, path_len, full, path, name );
	return 0;	
}

// ************************************************
//
//	break_path
//
//	break_down an absolute "path" into its' folder and filename components
//	thereby ignoring the "root" part of the path
//
//	return pointer to array of char pointers with the components
//	and "num"ber of components found
//
// ************************************************
#define MAX_ITEMS 32
char **break_path( const char *root, const char *_path, int *_num )
{
	static char	path[MAX_NAME_LEN + 1];
	static char	*comp[MAX_ITEMS];	
	char		*p = path;
	int		num = 0;

	memset( comp, 0, sizeof( comp ) );

	strnZcpy( path, _path, MAX_NAME_LEN );

	if( _num )
		*_num = 0;
		
	// skip the root part of the absolute path
	while( *root == *p ) {
		root++;
		p++;	
	}
	
	if( *root ) {
		//we still have root chars -> error!
serprintf("still root: %s\r\n", root );
		return NULL;
	}
	
	while( *p ) {
		while( *p && *p == '/' ) {
			// skip all /
			*p = '\0';
			p++;
		}
		if( *p ) {
			if( num < MAX_ITEMS ) {
				// add the new component
				comp[num++] = p;
			} else {
				return NULL;	
			}
		}
		while( *p && *p != '/' )
			p++;
	}
	
	if( _num )
		*_num = num;
	
	return comp;
}

// ************************************************
//
//	path_add_path
//
//	add a "path" with "/" to a path
//
// ************************************************
int path_add_path( char *path, const char *add, int max )
{
	int len = strlen( path );
	char *p = path + len;
	
	while( len++ < max - 1 && *add ) {
		*p++ = *add++;
	}
	// if there is no "/" at the end, add one!
	if( p > path && *(p - 1) != '/' )
		*p++ = '/';
	*p++ = '\0';
		
	return (*add != 0);
	
}

// ************************************************
//
//	path_add_file
//
//	add a "file" to a "path"
//
// ************************************************
int path_add_file( char *path, const char *file, int max )
{
	int len = strlen( path );
	char *p = path + len;

	// if path is not empty, it should end with a '/'
	if (len && *(p-1) != '/'){
		*p = '/';
		++p;
		++len;
	}
	
	while( len++ < max && *file ) {
		*p++ = *file++;
	}
	*p++ = '\0';
		
	return (*file != 0);
}

// ************************************************
//
//	path_from_PATH
//
//	create a full "path" from a PATH struct
//
// ************************************************
int path_from_PATH( char *path, const PATH *p, int max )
{
	int ret = 0;
	int i;

	strcpy( path, p->root );

	for (i = 0; i < p->level; i++) {
		ret += path_add_path(path, p->path[i].br.name, max );
	}
	return ret;
}

// ************************************************
//
//	path_from_url
//
//	return a path from a url ( adultdir:/mnt/data -> /mnt/data )
//
// ************************************************
const char *path_from_url( const char *url )
{
	while ( *url != ':' && *url != '\0' ) {
		url++;
	}

	if ( *url == ':' && *(++url) == '/' ) {
		return url;
	} else {
		return NULL;
	}
}

// ************************************************
//
//	check_root
//
//	make sure that a path starts with HDD_ROOT
//
// ************************************************
int check_root( const char *path )
{
	return strncmp( path, HDD_ROOT, strlen( HDD_ROOT ) );
}

// ************************************************
//
//	full_path_from_PATH
//
//	create a full "path" from a PATH struct and a BROWSE_ENTRY
//
// ************************************************
int full_path_from_PATH( char *full_path, const PATH *p, const BROWSE_ENTRY *br, int max )
{
	if( !br )
		return 1;
		
#ifdef CONFIG_ARCLIB
	if( p->fs == DEV_ARCLIB ) {
		if( br->type == TYPE_DIR ) {
serprintf("LIB: no real path for %s\r\n", br->name );		
			return 1;
		}
		if( LIB_get_real_path( full_path, br->handle ) ) {
			return 1;
		}
	} else 
#endif
	{
		path_from_PATH( full_path, p, max );
		if( path_add_file( full_path, br->name, max ) ) {
			return 1;
		}
	}
	return 0;
}

// ************************************************
//
//	parent_path_from_PATH
//
//	create a limited "path" from a PATH struct
//
// ************************************************
int parent_path_from_PATH( char *path, const PATH *p, int max, int level )
{
	int ret = 0;
	int i;
	
#ifdef CONFIG_ARCLIB
	if ( p->fs == DEV_ARCLIB ) {
		// TODO get the actual file path from the HDD root
	} else 
#endif
	{
		strcpy( path, p->root );
		for (i = 0; i < MIN( p->level, level ); i++) {
			ret += path_add_path( path, p->path[i].br.name, max );
		}
	}
	return ret;
}

// ************************************************
//
//	parent_path_from_PATH_CARD
//
//	create a limited "path" from a PATH struct
//	used with a SD/MMC file structure
//
// ************************************************
int parent_path_from_PATH_CARD( char *path, const PATH *p, int max, int level )
{
	int ret = 0;
	int i;
	
#ifdef CONFIG_ARCLIB
	if ( p->fs == DEV_ARCLIB ) {
		// TODO get the actual file path from the HDD root
	} else 
#endif
	{
		strcpy( path, CARD_ROOT );
		for (i = 1; i < MIN( p->level, level ); i++) {
			ret += path_add_path( path, p->path[i].br.name, max );
		}
	}
	return ret;
}

// ************************************************
//
//	browse_calc_level
//
// ************************************************
int browse_calc_level( const char *path )
{
	int cnt = 0;
	while ( *path ) {
		if ( *path == '/' )
			cnt++;
		path++;
	}
	return cnt ? cnt - 1 : 0;	// ignore the first '/'
}

// ************************************************
//
//	CheckHiddenDir
//
//	Directories which should always be hidden in a browser
// ************************************************
//	returns 1 when directory should be hidden, or 0 if is ok
//	returns 0 for all external filesystems (cfc, usb host)
// *************************************************************************
int CheckHiddenDir( char *name, int level, int fs )
{
	if( ( fs != DEV_HDD ) && ( fs != DEV_NET ) )
		return 0;

	if ( strcmpNC( name, "RECYCLED") == 0 )
		return 1;

	if ( strcmpNC( name, "_RESTORE") == 0 )
		return 1;

	if ( strcmpNC( name, "RESOURCE.FRK") == 0 )
		return 1;

	if ( strcmpNC( name, "THEFINDBYCONTENTFOLDER") == 0)
		return 1;

	if ( strcmpNC( name, "THEVOLUMESETTINGSFOLDER") == 0)
		return 1;

	if ( strcmpNC( name, "SYSTEM VOLUME INFORMATION") == 0)
		return 1;

	if ( strcmpNC( name, "DESKTOP FOLDER") == 0)
		return 1;

	if ( strcmpNC( name, "$RECYCLE.BIN" ) == 0 )
		return 1;

	if ( strcmpNC( name, "TRASH") == 0 )
		return 1;

	if ( strcmpNC( name, "lost+found") == 0 )
		return 1;

	if ( strcmpNC( name, "ringtones") == 0 )
		return 1;

	if ( level == 0 && strcmpNC( name, "sdcard") == 0 )
		return 1;

	if ( level == 0 && strcmpNC( name, "usb_host") == 0 )
		return 1;

#ifndef SHOW_ARCHOS_FOLDERS
	if ( ( level == 0 ) && ( strcmpNC( name, ARCHOS_SYSTEM_DIRNAME ) == 0 ) )
		return 1;

	if ( strcmpNC( name, ARCHOS_THUMB_DIRNAME ) == 0 )
		return 1;
#endif
	// Hide dirs starting with a dot
	if ( name[0] == '.' )
		return 1;

	return 0;
}

// **************************************************************
//
//	CheckHiddenFile
//
//	Files which should always be hidden in a browser
// **************************************************************
//	returns 1 when file should be hidden, or 0 if is ok
//	returns 0 for all external filesystems (cfc, usb host)
// **************************************************************
int CheckHiddenFile( char * name, int fs )
{
	if ( ( fs != DEV_HDD ) && ( fs != DEV_NET ) )
		return 0;

	if ( strcmpNC( name, "FINDER.DAT") == 0 )
		return 1;

	if ( strcmpNC( name, "DEVICON.FIL") == 0 )
		return 1;

	if ( strcmpNC( name, "DEVLOGO.FIL") == 0 )
		return 1;

	if ( strcmpNC( name, "WMPINFO.XML") == 0 )
		return 1;

	// Hide files starting with a dot
	if ( name[0] == '.' )
		return 1;
	//Hide Windows specific files
	if ( strcmpNC( name, "DESKTOP.INI") == 0)
		return 1;
	if( strcmpNC( name, "THUMBS.DB") == 0)
		return 1;

	return 0;
}
