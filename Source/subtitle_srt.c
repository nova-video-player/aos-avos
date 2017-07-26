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

#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>

#define DBG if(Debug[DBG_SUB])
#define DBG2 if(Debug[DBG_SUB] > 1)

#define SS_TO_MS(x) ((x)*1000)
#define MM_TO_MS(x) ((SS_TO_MS(x))*60)
#define HH_TO_MS(x) ((MM_TO_MS(x))*60)
#define SRT_TIME_LEN 28

#define Xfgets(str,len,fd)\
        while(fgets(str,len,fd)){\
                if(feof(fd)){str = 0;break;}\
                if(*str == '\r' ||*str == '\n' || *str=='\0'){\
                        continue;\
                }\
                break;}

//some states for SRT parser
enum
{
	SRT_NR,
	SRT_TIME,
	SRT_TEXT
};

/************************
 * Function: subtitle_get_srt_time
 * 
 * *********************/
static int subtitle_get_srt_time( char *line, int *start, int *end )
{
	int shh, smm, sss, sms;
	int ehh, emm, ess, ems;
	
	int ret = sscanf(line, "%d:%d:%d,%d --> %d:%d:%d,%d" , &shh, &smm, &sss, &sms, &ehh, &emm, &ess, &ems );
//serprintf("ret %d  %d %d %d %d -> %d %d %d %d \n", ret, shh, smm, sss, sms, ehh, emm, ess, ems );
	
	if( ret != 8 ) {
		return 1;
	}
	
	if( start ) 
		*start = ((shh * 60 + smm) * 60 + sss) * 1000  + sms;
	if( end )
		*end   = ((ehh * 60 + emm) * 60 + ess) * 1000  + ems;
	
	return 0;	
}

static int detect_SRT( FILE * file )
{
	// make sure that read starts at the beginning of file
	fseek( file, 0, SEEK_SET );

	char _line [LINE_LEN + 1 ];
	char* line = _line;

	Xfgets(line, LINE_LEN, file)
	if(feof(file)){
		goto ErrorExit;
	}
	Xfgets(line, LINE_LEN, file)
	if(feof(file)){
		goto ErrorExit;
	}
	if ( !subtitle_get_srt_time( line, NULL, NULL ) ) {
DBG serprintf( "SRT: found!\n" );
		return 0;
	}
ErrorExit:
DBG serprintf( "SRT: not SRT\n" );
	return 1;
}

static void srt_chop( char *line )
{
	char *tmp = strchr( line, NEW_LINE_CH );
	if( tmp ) {
		*tmp = '\0';
	}
	tmp = strchr( line, MS_CURSOR_BEGIN );
	if( tmp ) {
		*tmp = '\0';
	}
}

char *subtitle_get_next_line( char *start, int len, FILE *fd )
{
	char *ret = fgets( start, len, fd );
	if( !ret ) {
		return NULL;
	}
	if( strlen(ret) >= LINE_LEN - 1 ) {
		if( strchr( ret,'\n') ) {
			return ret;
		} else {
			//line did not fit into inputbuffer. Its propably
			//filled with crap. Read until newline so it wont
			//mess the next reads
			char* tmp = NULL;
			do{
				tmp = fgets(start,len,fd);
			}while(tmp && !strchr(tmp,'\n'));
			ret = start;
		}
	}
	return ret;
}

/**************
 * 
 * Parses SRT formatted subtitle text
 * input:
 * spex->filename must exist and point to VALID srt file
 * 
 * returns subtitles, packed into uni_sub*
 * 
 * ***********************/
static uni_sub *parse_SRT( subt_orig *spex, int clean_tags )
{
	uni_sub *sub_record = acalloc(1, sizeof( uni_sub ) );
	char _line[ LINE_LEN + 1];
	memset(_line,0,LINE_LEN);
	char *line = _line;
	FILE *fd = 0;
	sub_line *new_line = 0;
	int srt_state = SRT_NR;
	char* store = 0;
	int next_index = 0;
	//in SRT there is no data for all fields
	if ( !spex ) {
DBG serprintf( "SRT: Invalid parameter\n" );
		goto CLEAR_ERROR;
	}
	if ( !spex->filename ) {
DBG serprintf( "SRT: Invalid filename parameter\n" );
		goto CLEAR_ERROR;
	}
	fd = fopen( spex->filename, "r" );
	if( !fd )
		goto CLEAR_ERROR;
	line = subtitle_get_next_line( line, LINE_LEN, fd );
	while ( line ) {

		switch ( srt_state ) {
		case SRT_NR:{
			//here should be data, but ignore possible blank lines
			srt_chop(line);
			if(*line == '\0'){
				memset(line,0,LINE_LEN);
				line = subtitle_get_next_line( line, LINE_LEN, fd );
				continue;
			}
			if ( next_index == atoi( line ) ) {
				next_index++;
				srt_state = SRT_TIME;
			} else {
DBG2 serprintf( "SRT: missing index %i(%i),line='%s'\n", next_index, atoi( line ), line );
				next_index = atoi( line ) + 1;
				srt_state = SRT_TIME;
			}
			memset(line,0,LINE_LEN);
			line = subtitle_get_next_line( line, LINE_LEN, fd );
			continue;
		}
		case SRT_TIME:{
			int start;
			int end;
			if ( subtitle_get_srt_time( line, &start, &end ) ) {
DBG serprintf( "subtitle: SRT time error near index %i  line %s\n", next_index - 1, line );
				srt_state = SRT_NR;
				memset(line,0,LINE_LEN);
				line = subtitle_get_next_line( line, LINE_LEN, fd );
				continue;
			}
			//new title coming, init new struct for it
			new_line = acalloc(1, sizeof( sub_line ) );
			new_line->start = start;
			new_line->end   = end;

			srt_state = SRT_TEXT;
			memset(line,0,LINE_LEN);
			line = subtitle_get_next_line( line, LINE_LEN, fd );
			continue;
		}
		case SRT_TEXT:{
			//here can be either blank (ends this title) or text.
			//ok. the title ends to this line
			if ( line[0] == NEW_LINE_CH || line[0] == MS_CURSOR_BEGIN ) {
				if ( new_line->top == 0 && new_line->bottom == 0 ) {
DBG serprintf( "no text found for index %i\n", next_index - 1 );
					afree( new_line );
					memset(line,0,LINE_LEN);
					new_line = 0;
					line = subtitle_get_next_line( line, LINE_LEN, fd );
					if( !line ) {
						continue;
					}
				} else {
					//end of this title. Store it
					if ( sub_record->first == 0 ) {
						sub_record->first = new_line;
						sub_record->last = new_line;
					} else {
						if( sub_record->last->end < new_line->end ) {
							sub_record->last->next = new_line;
							new_line->prev   = sub_record->last;
							sub_record->last = new_line;
						}
					}
					new_line = 0;
				}
				srt_state = SRT_NR;
			}
			//IF \n or \r must be erased, do it here!!
			srt_chop(line);
			if ( strlen( line ) ) {
#ifdef CONFIG_I18N
				if( !spex->utf8 ) {
					//serprintf("LINE: %s\r\n", line );
					wchar unicode[ LINE_LEN + 1 ];
					memset(unicode,0, LINE_LEN);
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
				store = subtitle_clean_formatter(line, clean_tags);
				//There may be multiple lines. If true put to top line
				if(new_line){
					if ( new_line->top == 0 ) {
						new_line->top = amalloc( strlen( store ) + 1 );
						strcpy( new_line->top, store );
					} else {	//hopefully the bottom line is empty
						if ( new_line->bottom == 0 ) {
							new_line->bottom = amalloc( strlen( store ) + 1 );
							strcpy( new_line->bottom, store );
						} else {
							//Only allow catting until line is LINE_LEN after that we know that
							//the line is crap
							if((strlen(new_line->bottom) + strlen(store)) < LINE_LEN){

								new_line->bottom = arealloc( new_line->bottom,
										strlen( store ) + strlen( new_line->bottom ) + 2 );
								strcat( new_line->bottom, store );
							}
						}
					}
				}
				else{
					srt_state = SRT_NR;
				}
				afree(store);
				store = 0;
			}
			memset(line,0,LINE_LEN);
			line = subtitle_get_next_line( line, LINE_LEN, fd );
			continue;
		}
		}
	}
	if( new_line ) { //last one was not stored;
		if( sub_record->first == 0 ) {
			sub_record->first = new_line;
			sub_record->last = new_line;
		} else {
			if( sub_record->last->end < new_line->end ) {
				sub_record->last->next = new_line;
				new_line->prev = sub_record->last;
				sub_record->last = new_line;
			}
		}
	}
	
	if(fd){
		fclose(fd);
	}
	if(store){
		afree(store);
	}
	return sub_record;
CLEAR_ERROR:
	if(fd){
		fclose(fd);
	}
	if(store){
		afree(store);
	}
	subtitle_clean_error(sub_record);
		
	afree(sub_record);
	if(new_line){
		free(new_line);
	}
	return 0;
}

__attribute__((unused))
static int subtitle_get_next_time_val(const char **start, int sep, long int *val)
{
	char *endptr;

	errno = 0;
	*val = strtol(*start, &endptr, 10);
	if ((errno == ERANGE && (*val == LONG_MAX || *val == LONG_MIN))
	    || (errno != 0 && *val == 0) || (sep != 0 && endptr != NULL && endptr - *start != sep)) {
		return 1;
	} else {
		*start = endptr +1;
		return 0;
	}
}

static struct SUBTITLE_FORMAT SRT = {
	"SubRip",
	detect_SRT,
	NULL,		// no info
	parse_SRT,
};

SUBTITLE_REGISTER_FORMAT( SRT );
