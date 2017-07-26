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

#ifndef __SUBTITLE_FORMAT_H__
#define __SUBTITLE_FORMAT_H__

#define LINE_LEN	300
#define MS_CURSOR_BEGIN '\r'
#define NEW_LINE_CH	'\n'

#include <stdio.h>
#include <stdint.h>

//if the subtitle contains title for multiple languages
//it should fill this field in info_XXX function

//This struct is acquired when determining which files may
//contain valid subtitles for videofile 
//and is passed to parse_XXX function

typedef struct sub_coding_style
{
	char *id; //internal describer for language 'ENCC' 'FINCC' etc.
	char *name; //name of language 'English', 'Finnish'
   //this is used to select the language
	char *lang; //codepage
	char *type; //may or may not be used
} sub_coding_style;

struct SUBTITLE_FORMAT;

typedef struct subt_orig_t
{
	struct SUBTITLE_FORMAT *format;
	char *filename;
	char *org_name;
 	char  ext[4];
 	char lang[4];
        int utf8;
        int delete;

        unsigned int lan_count;
        char *language_name;
	char *default_language; //This must contain the internal describer
                                //   'ENCC' 'FINCC' etc or zero if not 
				// supported by subtitle format
	sub_coding_style **title_langs;

	int has_palette;
	uint32_t palette[16];

	struct subt_orig_t *next;
} subt_orig;

typedef struct subtitle_files_t
{
	char *set_lan;
	int count;
	subt_orig *files;
} subtitle_files;

typedef struct sub_line_t
{
	char *top;
	char *bottom;
	int start;
	int end;
	uint32_t pos;
	struct sub_line_t *next;
	struct sub_line_t *prev;
} sub_line;

typedef struct uni_sub_t
{
	int frame_multiplier;
	int vobsub;
	int vobsub_fd;
	unsigned char *vobsub_data;
	int vobsub_size;
	int vobsub_pos;
	int has_palette;
	uint32_t palette[16];
	struct SUBTITLE_FORMAT *format;
        char *identifier;
	sub_line *first;
	sub_line *last;
} uni_sub;

typedef struct converted_subs_t
{
        uni_sub **converted;
        int cnt;
} converted_subs;

char *subtitle_clean_formatter( char *line, int clean_tags );
void subtitle_clean_error(uni_sub* subs);

typedef struct SUBTITLE_FORMAT {
	const char 		*name;
	int 			(*detect)( FILE * file );
	sub_coding_style**	(*info)( FILE * file, int *cnt, uint32_t *palette, int *has_palette );
	uni_sub*		(*parse)( subt_orig *subs, int clean_tags );
	int			(*get_gfx)( uni_sub *subs, uint32_t pos, uint8_t *data, int *size );
	int			(*close)( uni_sub *subs );
} SUBTITLE_FORMAT;

struct SUBTITLE_REG_FORMAT;

typedef struct SUBTITLE_REG_FORMAT {
	const SUBTITLE_FORMAT 	*format;
	struct SUBTITLE_REG_FORMAT *next;
} SUBTITLE_REG_FORMAT;

int subtitle_register_format( SUBTITLE_REG_FORMAT *reg );

#define SUBTITLE_REGISTER_FORMAT( format ) \
	static SUBTITLE_REG_FORMAT _reg_##etype##format = { \
		&format,\
		NULL\
	}; \
	static void _fn_reg_##etype##format( void ) __attribute__((constructor));\
	static void _fn_reg_##etype##format( void )\
	{ \
		subtitle_register_format( &_reg_##etype##format ); \
	}

subtitle_files *subtitle_check_files( const char **path_list, const char *filename );
void            subtitle_free_files( subtitle_files *files );
converted_subs *subtitle_get_converted( subtitle_files *sub_files, int clean_tags );
int  		subtitle_get_gfx( uni_sub *subs, uint32_t pos, uint8_t *data, int *size );
void            subtitle_free_converted( converted_subs *subs );
char           *subtitle_get_description( subt_orig *title );

#endif // __SUBTITLE_FORMAT_H__
