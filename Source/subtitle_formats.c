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
#include "astdlib.h"
#include "ctype.h"
#include "debug.h"
#include "iso639.h"
#include "util.h"
#include "i18n.h"
#include "browse.h"
#include "device_config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>		// for some reason atoi() not from astdlib :(

#ifdef CONFIG_SUBTITLES

#include "subtitle_format.h"

#define result_t int
#define state_t int
#define DBG  if(Debug[DBG_SUB])
#define DBG2 if(Debug[DBG_SUB] > 1)

static SUBTITLE_REG_FORMAT *_reg = NULL;

// ************************************************************
//
//	subtitle_register_format
//
// ************************************************************
int subtitle_register_format( SUBTITLE_REG_FORMAT *reg )
{
	if( !_reg ) {
		_reg = reg;
	} else {
		SUBTITLE_REG_FORMAT *head = _reg;
		while( head->next ) {
			head = head->next;
		} 
		head->next = reg;
	}
	reg->next = NULL;
	return 0;
}

/*********
 * Executes detect function of every subtitle formats and tries to figure
 * out which format the given file is
 *
 * input: open file handle to file
 * output: enum value of format or SUBFORMAT_COUNT if no format is detected
 *  * ********/
static SUBTITLE_FORMAT *subtitle_find_format( FILE * file )
{
	SUBTITLE_REG_FORMAT *f = _reg;
	while( f ) {
		if( f->format->detect ) {
			if( !f->format->detect( file ) ) {
				return (SUBTITLE_FORMAT*)f->format;
			}
		}
		f = f->next;
	}
	return NULL;
}

#ifdef DEBUG_MSG
static void _dump_formats( void )
{
	SUBTITLE_REG_FORMAT *f = _reg;
serprintf("subtitle formats:\r\n");
	while( f ) {
serprintf("\t[%s]\r\n", f->format->name );
		f = f->next;
	}
serprintf("\r\n");
}

DECLARE_DEBUG_COMMAND_VOID( "subd", _dump_formats ); 
#endif

/*************
 * Opens a given filename and if subtitle format is found
 * allocates room for new. Also extract metadata information if such
 * exists for format
 *  * ********************/
static subt_orig *subtitle_parse_file( const char *filename, const char *org_name, const char *ext, const char *lang, int utf8, int delete )
{
	subt_orig *new_title = NULL;

	if ( !filename ) {
		DBG serprintf( "NULL FILENAME!\n" );
		return 0;
	}
	if ( isdigit( *ext ) ) {
		serprintf( "part file: %s\n", filename );
		return 0;
	}
		
	FILE *file = fopen( filename, "r" );
	if ( !file ) {
		DBG serprintf( "could not open file! %s\n", filename );
		return NULL;
	}
	
	SUBTITLE_FORMAT *ff = subtitle_find_format( file );
	if ( ff != NULL ) {
		new_title = acalloc(1, sizeof( subt_orig ) );
		new_title->filename = amalloc( strlen( filename ) + 1 );
		new_title->org_name = amalloc( strlen( org_name ) + 1 );
		strcpy( new_title->filename, filename );
		strcpy( new_title->org_name, org_name );
		strcpy( new_title->ext,  ext );
		strcpy( new_title->lang, lang );
		
		new_title->format    = ff;
		new_title->utf8      = utf8;
		new_title->delete    = delete;
		new_title->next      = 0;
		new_title->lan_count = 0;
		
		//Only some titles have metadata
		//try to extract different languages from headers
		if( ff->info ) {
			new_title->title_langs = ff->info( file, &new_title->lan_count, new_title->palette, &new_title->has_palette );
DBG serprintf("lang count %d\r\n", new_title->lan_count );

			// if info is defined and fails, we fail for this title!
			if(!new_title->title_langs){
				DBG serprintf("Error parsing INFO\n");
				afree(new_title->filename);
				afree(new_title->org_name);
				afree(new_title);
				new_title = 0;
			}
		}
	}
	fclose( file );
	return new_title;
}

converted_subs *subtitle_get_converted( subtitle_files *sub_files, int clean_tags )
{
	//count number of subtitles (files & languages)
	int count = 0;
	subt_orig *title = sub_files->files;

	while ( title ) {
		if ( !title->format ) {	//unrecognised file
			title = title->next;
			continue;
		}
		if ( title->title_langs ) {
			count += title->lan_count;
		} else {
			count++;
		}
		title = title->next;
	}
	
	//reserve space for every title file & lang
	converted_subs *sub_array = acalloc(1, sizeof( converted_subs ) );
	if ( !sub_array ) {
DBG serprintf( "subtitles: cannot alloc sub_array\n" );
		return 0;
	}

	sub_array->converted = acalloc( count, sizeof( uni_sub* ) );
	sub_array->cnt = count;
	
	title = sub_files->files;
	count = 0;
	while ( title ) {
		if( !title->format ) {
			title = title->next;
			continue;
		}
		if( !title->title_langs ) {
			// no languages defined. Use ending of file
			sub_array->converted[count] = title->format->parse( title, clean_tags );
			if( sub_array->converted[count] == 0 ) {
				sub_array->cnt--;
				title = title->next;
				continue;
			}

			// if there is a . separated part before the extension, treat it as a 
			// language code and try to map a proper language name to it!
			if( title->ext[0] ) {
				const char *code;
				if( title->lang[0] && ( code = map_ISO639_code( title->lang ) ) != title->lang && code[0] != '\0' ) {
					sub_array->converted[count]->identifier = astrdup( code );
				} else {
					sub_array->converted[count]->identifier = astrdup( title->ext );
					char *i = sub_array->converted[count]->identifier;
					while( *i ) {
						*i = toupper(*i);
						i++;
					}
				}
			} else {
				sub_array->converted[count]->identifier = astrdup( "Unknown" );
			}
DBG serprintf("ext [%s]  lang [%s] -> [%s]\n", title->ext, title->lang, sub_array->converted[count]->identifier );			
			count++;
		} else {
			// languages specified in this file
			int i;
			for( i = 0; i < title->lan_count; ++i ) {
				if( title->language_name ) {
					afree( title->language_name );
				}
				title->language_name = astrdup( title->title_langs[i]->name );
				title->default_language = astrdup(title->title_langs[i]->id);
				sub_array->converted[count + i] = title->format->parse( title, clean_tags );
				afree(title->default_language);
				title->default_language = 0;
				afree(title->language_name);
				title->language_name = 0;
				if( sub_array->converted[count + i] == 0 ) {
					// error in file. ignore it
					count--;
					continue;
				}
				sub_array->converted[count + i]->identifier = astrdup( title->title_langs[i]->name );
			}
			count += i;
		}
		if( title->delete ) {
DBG serprintf("sub: delete %s\n", title->filename );		
			file_remove( title->filename );
		}
		title = title->next;
	}

	if(count == 0){ //could not retrieve anything
		afree(sub_array->converted);
		afree(sub_array);
		return 0;
	}
DBG {
	serprintf("count %d\n", sub_array->cnt );
	int i;
	for( i = 0; i < sub_array->cnt; i++ ) {
		serprintf("subs for %d [%s]\n", i, sub_array->converted[i]->identifier );
DBG2 {		
		sub_line* tt = sub_array->converted[i]->first;
		while(tt){
			serprintf("%8d/%8d [%s][%s]\n",tt->start,tt->end, tt->top, tt->bottom);
			tt = tt->next;
		}
}
	} 
}
	return sub_array;
}

int subtitle_get_gfx( uni_sub *subs, uint32_t pos, uint8_t *data, int *size )
{
	if( subs->format->get_gfx ) {
		return subs->format->get_gfx( subs, pos, data, size );
	}
	return 1;
}

char *subtitle_get_description( subt_orig * title )
{
	return ( astrdup( title->format->name ) );
}

char *subtitle_clean_formatter( char *line, int clean_tags )
{
	char *newli = acalloc(strlen(line) + 2, 1);
	char *end = NULL;
	if (clean_tags) {
		char *start = strchr(line,'<');
		if(start)
			end = strchr(start,'>');
		//replace all to-be-ignored characters with \n
		while(start && end){
			if(strncmp(start,"<br",3)){
				memset(start,'\n',end - start + 1);
			}
			start = strchr(end,'<');
			if(!start){
				break;
			}
			end = strchr(start,'>');
		}

		//ignore &lt;i&gt sequences too
		start = strstr(line,"&lt;");
		if( !start ) {
			end = 0;
		} else {
			end = strstr(line, "&gt;");
		}

		while( start && end ) {
			memset(start,'\n',end - start + 4);
			start = strstr(line,"&lt;");
			if(!start){
				break;
			}
			end = strstr(start,"&gt;");
		}
	}

	int i = 0;
	int b = 0;
	//copy string with execption of '\n's
	while(i < strlen(line)){
		if(line[i] != '\n'){
			newli[b++] = line[i];
		}
		i++;
	}
	newli[b] = '\0'; //make sure the string ends 
	return (newli);
}

void subtitle_clean_error(uni_sub *subs)
{
	if(!subs){
		return;
	}
	if(subs->first){
		sub_line* tmp = subs->first;
		sub_line* line = tmp;
		while(line){
			if(line->top){
				afree(line->top);
			}
			if(line->bottom){
				afree(line->bottom);
			}
			line = line->next;
			afree(tmp);
			tmp = line;
		}
		if(subs->identifier){
			afree(subs->identifier);
		}
	}
}

#define BOM_LE 	 0xFFFE
#define BOM_BE 	 0xFEFF
#define BOM_UTF8 0xBFBBEF

#define BUF_MAX 1024
#define TMP_FILE "/tmp/subXXXXXX"

#define CHECK_MAX 256

static int convert_to_utf8( char **filename, int *delete )
{
	int ret;
	int tmp_fd = -1;
	FILE *tmp_file = NULL;
	FILE *file = NULL;

	file = fopen( *filename, "r");
	if (!file) {
		return 0;
	}
	*delete = 0;
	
	// check for BOM
	unsigned short bom = 0;
	fread( &bom, 2, 1, file );
	if( bom != BOM_LE && bom != BOM_BE ) {
		unsigned char msb;
		fread( &msb, 1, 1, file );
		if( bom == (BOM_UTF8 & 0xFFFF) && msb == (BOM_UTF8 >> 16) ) {
DBG serprintf("sub: UTF-8 by BOM!\n");
			ret = 1;
			goto end;
		}

		// no UTF8 BOM, we need to look at the whole file:
		void *ctx = I18N_check_encoding_init();
		unsigned char buf[BUF_MAX];
		int len;
		int count = 0;
		while( (len = fread(buf, 1, BUF_MAX, file)) && count ++ < CHECK_MAX ) {
			I18N_check_encoding_update( ctx, buf, len);
		}
		int utf8;
		I18N_check_encoding_finish( ctx, &utf8 );
		if( utf8 ) {
DBG serprintf("sub: UTF-8 by check!\n");
			ret = 1;
			goto end;
		}
DBG serprintf("sub: CODEPAGE!\n");
		ret = 0;
		goto end;
	}
DBG serprintf("sub: UTF-16!\n");

#ifdef CONFIG_ANDROID
	char template[256];
	snprintf(template, 255, "/data/data/%s/files/sub", device_config_get_android_pkg_name());
	tmp_fd = open(template, O_CREAT|O_RDWR);
	chmod(template, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP);
#else
	char template[] = TMP_FILE;
	
	tmp_fd = mkstemp(template);
#endif

	if (tmp_fd < 0) {
		serprintf("failed to create temporary file: %s\n", template );
		ret = 0;
		goto end;
	}
DBG serprintf("sub: tmpfile: %s\n", template );

	tmp_file = fdopen(tmp_fd, "w+");
	if (!tmp_file) {
		serprintf("failed to open temporary file for writing (%s:%i)\n", __FILE__, __LINE__);
		ret = 0;
		goto end;
	}

	unsigned short utf16[BUF_MAX];
	unsigned char  utf8[BUF_MAX * 3];
	while( 1 ) {
		int utf16_len = fread( utf16, 2, BUF_MAX, file );
		if( !utf16_len ) {
			break;
		}
		if( bom == BOM_LE ) {
			swap16_buf( (unsigned char*)utf16, utf16_len * 2 );
		}
		int utf8_len  = unicode_utf16_to_utf8( utf8, utf16, utf16_len );
//serprintf("in %3d out %3d  %s\n", utf16_len, utf8_len, utf8 );	
		fwrite( utf8, 1, utf8_len, tmp_file );
	}
	
	afree( *filename );
	*filename = astrdup( template );
	*delete   = 1;
	ret = 1;
end:
	if( file )
		fclose( file );
	if( tmp_file )
		fclose( tmp_file );
	if( tmp_fd > -1 )
		close( tmp_fd );
	return ret;
}

static char **subtitle_get_files( char **sub_files, const char *full_path, const char *file_name, int *count )
{
	if ( !full_path || !file_name ) {
		DBG serprintf( "subtitle_get_files: path or filename error\n" );
		return NULL;
	}
	
	int sub_n = *count;
	
	// trickster. Compare only name not the ending in name.end
	char *path = astrdup( full_path );
	char *name = astrdup( file_name );
	char *tmp  = strrchr( name, '.' );
	if ( tmp ) {
		*( tmp /*+ 1*/ ) = '\0';	// allow for substrings by terminating before the "."
	}
	// dig the names from current directory
	// use the name of parameter to find out subtitle files. Only ending should 
	// be different
	DIR *dp = dir_open( path );
	if ( !dp ) {		//path could point directly to file. Remove everything after last /
		tmp = strrchr( path, '/' );
		if ( tmp ) {
			*( tmp + 1 ) = '\0';
			dp = dir_open( path );
		}
	}
	if ( dp ) {
		DIRENT *ep = dir_read( dp );
		// search the subtitle with following rules
		// 1. everything until the '.' must be similar
		// 2. length of filename must be equal to videofile
		// 3. subtitle file can not be the same file as videofile
		while ( ep ) {
			if ( !strncmpNC( name, ep->d_name, strlen( name ) ) ) {
				if ( strcmpNC( ep->d_name, file_name ) ) {
					if ( sub_files ) {
						sub_files = arealloc( sub_files, ( sub_n + 1 ) * sizeof( *sub_files ) );
						if ( !sub_files ) {
							dir_close( dp );
							goto ERROREXIT;
						}
					} else {
						sub_files = amalloc( sizeof( sub_files ) );
						if ( !sub_files ) {
							dir_close( dp );
							goto ERROREXIT;
						}
					}
					sub_files[sub_n] = amalloc( strlen( ep->d_name ) + strlen( path ) + 1 );
					strcpy( sub_files[sub_n], path );
					strcat( sub_files[sub_n], ep->d_name );
DBG serprintf("%d: %s\r\n", sub_n, sub_files[sub_n] );

					++sub_n;
				}
			}
			//next file
			ep = dir_read( dp );
		}
		
		dir_close( dp );
	} else {
		DBG serprintf( "subtitle_get_files:Error opening directory:%s\n", path );
		//goto ERROREXIT;
	}

	afree( name );
	afree( path );
	*count = sub_n;
	return sub_files;

ERROREXIT:
	if ( sub_files ) {
		int i;
		for ( i = 0; i < sub_n; ++i ) {
			afree( sub_files[i] );
		}
		afree( sub_files );
	}
	afree( path );
	afree( name );
	if ( dp )
		dir_close( dp );
	return NULL;
}

/******************
 * Browses through given PATH and searches for files 
 * that have same name as filename, but different ending
 * send those files to be detected and groups detected files 
 * into one array in subtitle_files*
 *  * ***************/
subtitle_files *subtitle_check_files( const char **path, const char *filename )
{
	int count = 0;
	char **sub_files = NULL;

	while( *path ) {
DBG serprintf("checking path: %s\n", *path );
		sub_files = subtitle_get_files( sub_files, *path, filename, &count );
		path++;
	}
	
	subt_orig *subtitle_file     = NULL;
	subtitle_files *usable_files = NULL;
	
	// for every found file, run detection routines to find out if they are
	// in some recognised format
	int i;
	for ( i = 0; i < count; ++i ) {
DBG serprintf("sub: check: %s\r\n", sub_files[i] );
		// get ext and lang here as the filename could be mangled by utf16 conversion!
		char ext[4]  = { 0 };
		char lang[4] = { 0 };
		strnZcpy( ext,  get_extension( sub_files[i] ), 3 );
		strnZcpy( lang, get_extension( cut_extension( sub_files[i] )), 3 );

		int delete = 0;
		char *org_name = astrdup( sub_files[i] );
		int utf8 = convert_to_utf8( &(sub_files[i]), &delete );
		subtitle_file = subtitle_parse_file( sub_files[i], org_name, ext, lang, utf8, delete );
		afree( org_name );
		if ( subtitle_file ) {
			if ( !usable_files ) {
				usable_files = acalloc(1, sizeof( subtitle_files ) );
				usable_files->set_lan = 0;
				usable_files->files = subtitle_file;
				usable_files->files->next = 0;
				usable_files->count = 0;
			} else {
				subtitle_file->next = usable_files->files;
				usable_files->files = subtitle_file;
			}
			usable_files->count++;
		} else {
			if( delete ) {
DBG serprintf("sub: delete %s\n", sub_files[i] );		
				file_remove( sub_files[i] );
			}
		}
		afree( sub_files[i] );
	}
	afree( sub_files );
	
	return usable_files;
}

// cleans the coding style struct
static void free_coding( sub_coding_style ** code, int count )
{
	int i;
	if ( !code )
		return;
	for ( i = 0; i < count; ++i ) {
		if ( !code[i] ) {
			return;
		}
		if ( code[i]->id )
			afree( code[i]->id );
		if ( code[i]->name )
			afree( code[i]->name );
		if ( code[i]->lang )
			afree( code[i]->lang );
		if ( code[i]->type )
			afree( code[i]->type );
		afree( code[i] );
	}
	afree( code );
}

// cleans the per-subtitle-file structs
static void free_subs( subt_orig * fd )
{
	if ( !fd )
		return;
	subt_orig *tmp = 0;
	while ( fd ) {
		afree( fd->filename );
		afree( fd->org_name );
		if ( fd->title_langs )
			free_coding( fd->title_langs, fd->lan_count );
		afree( fd->default_language );
		tmp = fd;
		fd = fd->next;
		afree( tmp );
	}
}

// cleans the struct that contains all the files that contain subtitles 
// for played videofile
void subtitle_free_files( subtitle_files *files )
{
	if ( !files )
		return;
	if ( files->set_lan )
		afree( files->set_lan );

	free_subs( files->files );
	afree( files );
}

// cleans the linked list of converted subtitles
static void free_subline( sub_line * sub )
{
	sub_line *tmp;
	if ( !sub )
		return;
	while ( sub ) {
		if ( sub->top )
			afree( sub->top );
		if ( sub->bottom )
			afree( sub->bottom );
		tmp = sub;
		sub = sub->next;
		afree( tmp );
	}
}

void subtitle_free_converted( converted_subs *subs )
{
	int i = 0;
	for ( i = 0; i < subs->cnt; ++i ) {
		// call a format specific cleanup if needed
		if( subs->converted[i]->format && subs->converted[i]->format->close ) {
			subs->converted[i]->format->close( subs->converted[i] );
		}
		free_subline( subs->converted[i]->first );
		afree(subs->converted[i]->identifier);
		afree( subs->converted[i] );
	}
	afree( subs->converted );
	afree( subs );
}

#endif	// CONFIG_SUBTITLES
