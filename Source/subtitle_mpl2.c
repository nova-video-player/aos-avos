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
#include "subtitle_format.h"
#include "i18n.h"
#include "util.h"
#include "astdlib.h"

#include <ctype.h>
#include <string.h>

#define DBG if(Debug[DBG_SUB])

#define LINE_BREAK_CH_NUM	3

char *subtitle_get_next_line( char *start, int len, FILE *fd );

static const char *LineBreakCh[LINE_BREAK_CH_NUM] =
{
	"|",
	"<br>",
	"&nbsr"
};

/************************
 * Function: subtitle_get_sub_time
 * input: timeline of sub-formatted subtitle
 * output: array of start and endtime in int
 * 
 * Converts the given string to start end endtime.
 * The string must be in "{XX}{YY}ZTY" where XX and YY are int
 * *********************/
static int *subtitle_get_sub_time( char *line )
{
	int *times = amalloc( 2 * sizeof( int ) );
	if( 2 != sscanf( line, "[%d][%d]", &times[0], &times[1] ) ) {
		afree(times);		
		return 0;
	}
	times[0] *= 100;
	times[1] *= 100;
	
	return times;
}

static int detect_MPL( FILE * file )
{
	//make sure that read starts at the beginning of file
	fseek( file, 0, SEEK_SET );

	char _line[ LINE_LEN + 1 ];
	char* line = _line;
	line = subtitle_get_next_line( line, LINE_LEN, file );
	//minimum line len for SUB
	if ( line && strlen( line ) > 6 ) {
		int start;
		int stop;

		if( 2 != sscanf( line, "[%d][%d]", &start, &stop ) ) {
			goto ErrorExit;
		}
DBG serprintf( "MPL: found! %d %d\n", start, stop );
		return 0;
	}
ErrorExit:
DBG printf( "MPL: not MPL\n" );
	return 1;

}

static void store_line(char *line, sub_line *sub, int utf8, int clean_tags)
{
#ifdef CONFIG_I18N
	if( !utf8 ) {
//serprintf("LINE: %s\r\n", line );
		wchar unicode[ LINE_LEN];
		memset(unicode, 0, LINE_LEN * sizeof(wchar));
		wchar *uc = unicode;
		char *c = line;
		while( *c ) {
			// take care about wide codepage chars!
			c += I18N_codepage_to_unicode( c, uc );
			uc++; 
		}
		// convert to utf8
		utf16_to_utf8( line, unicode, LINE_LEN );
//serprintf("LINE: %s\r\n", line );
	}
#endif

	line = subtitle_clean_formatter(line, clean_tags);
	int i;
	for(i = 0; i < LINE_BREAK_CH_NUM;++i){
		char *top = line;
		char *bot = strstr(line, LineBreakCh[i]);
		if(bot){
			*bot = '\0';
			if( top[0] == '/' )
				top++;
			sub->top = acalloc(strlen(top) + 1, 1);
			strncpy(sub->top, top, strlen(top));
			bot += strlen(LineBreakCh[i]);
			if( bot[0] == '/' )
				bot++;
			sub->bottom = acalloc(strlen(bot) + 1, 1);
			strncpy(sub->bottom, bot, strlen(bot));
			break;
		}
	}
	if(!sub->top){
		char *top = line;
		if( top[0] == '/' )
			top++;
		sub->top = acalloc(strlen(top) + 1, 1);
		strcpy(sub->top, top);
	}
	afree(line); //this is different than the parameter. thus free it
}

static uni_sub *parse_MPL( subt_orig *spex, int clean_tags )
{
	if ( !spex ) {
		DBG serprintf( "Subtitle:parseSUB: Invalid parameter\n" );
		return 0;
	}

	if ( !spex->filename ) {
		DBG serprintf( "Subtitle:parseSUB: Invalid filename\n" );
		return 0;
	}
	
	FILE *fd = fopen( spex->filename, "r" );
	if ( !fd ) {
		DBG serprintf( "Subtitle:parseSUB: could not read file %s\n", spex->filename );
		return 0;
	}

	uni_sub *sub_record = acalloc(1, sizeof( uni_sub ) );
	int cnt = 0;
	char _line[LINE_LEN+1];
	memset(_line,0,LINE_LEN+1);
	char* line = _line;
	line = subtitle_get_next_line( line, LINE_LEN, fd );
	while ( line ) {
		int *times = subtitle_get_sub_time( line );
		if ( !times ) {
			DBG serprintf( "could not get timing from line %i\n", cnt );
			cnt++;
			line = subtitle_get_next_line( line, LINE_LEN, fd );
			continue;
		}
		char *tmp = strrchr( line, ']' );
		if ( tmp ) {
			tmp ++;
			//erase CR and LF
			char *cut = strchr( line, '\n' );
			if ( cut ) {
				*cut = '\0';
			}
			cut = strchr( line, '\r' );
			if ( cut ) {
				*cut = '\0';
			}

			sub_line *new_line = acalloc(1, sizeof( sub_line ) );
			new_line->start = times[0];
			new_line->end   = times[1];

			store_line(tmp, new_line, spex->utf8, clean_tags);

			if ( sub_record->first == 0 ) {
				sub_record->first = new_line;
				sub_record->last = new_line;
			} else {
				sub_record->last->next = new_line;
				new_line->prev = sub_record->last;
				sub_record->last = new_line;
			}
		}
		cnt++;
		line = subtitle_get_next_line( line,  LINE_LEN , fd );
		afree( times );
	}
	fclose(fd);
	return sub_record;
}

static struct SUBTITLE_FORMAT MPL = {
	"MPL2",
	detect_MPL,
	NULL,		// no info
	parse_MPL,
};

SUBTITLE_REGISTER_FORMAT( MPL );

