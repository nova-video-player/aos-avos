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
#define VTT_TIME_LEN 28

#define Xfgets(str,len,fd)\
        while(fgets(str,len,fd)){\
                if(feof(fd)){str = 0;break;}\
                if(*str == '\r' ||*str == '\n' || *str=='\0'){\
                        continue;\
                }\
                break;}

//some states for VTT parser
enum
{
    VTT_SEARCH, // Searching for next cue (could be ID or time)
    VTT_TIME,   // Expecting time line (if we saw ID) or parsing time
    VTT_TEXT    // Parsing text lines
};

// Defined in subtitle_srt.c, declaring it here to use it
extern char *subtitle_get_next_line( char *start, int len, FILE *fd );

static int subtitle_get_vtt_time( char *line, int *start, int *end )
{
    int shh = 0, smm = 0, sss = 0, sms = 0;
    int ehh = 0, emm = 0, ess = 0, ems = 0;
    
    // VTT times can be MM:SS.mmm or HH:MM:SS.mmm
    // We can try to parse both.
    // And there can be settings after the end time.
    
    // Try HH:MM:SS.mmm
    int ret = sscanf(line, "%d:%d:%d.%d --> %d:%d:%d.%d", &shh, &smm, &sss, &sms, &ehh, &emm, &ess, &ems );
    if (ret != 8) {
        // Try MM:SS.mmm
        shh = 0; ehh = 0;
        ret = sscanf(line, "%d:%d.%d --> %d:%d.%d", &smm, &sss, &sms, &emm, &ess, &ems );
        if (ret != 6) {
            return 1; 
        }
    }

    if( start ) 
        *start = ((shh * 60 + smm) * 60 + sss) * 1000  + sms;
    if( end )
        *end   = ((ehh * 60 + emm) * 60 + ess) * 1000  + ems;
    
    return 0;   
}

static int detect_VTT( FILE * file )
{
    fseek( file, 0, SEEK_SET );
    char _line [LINE_LEN + 1 ];
    char* line = _line;

    Xfgets(line, LINE_LEN, file)
    if(feof(file)){
        return 1;
    }
    // VTT must start with WEBVTT
    if (strncmp(line, "WEBVTT", 6) == 0) {
        DBG serprintf( "VTT: found!\n" );
        return 0;
    }

    return 1;
}

static void vtt_chop( char *line )
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

static uni_sub *parse_VTT( subt_orig *spex, int clean_tags )
{
    uni_sub *sub_record = acalloc(1, sizeof( uni_sub ) );
    char _line[ LINE_LEN + 1];
    memset(_line,0,LINE_LEN);
    char *line = _line;
    FILE *fd = 0;
    sub_line *new_line = 0;
    int vtt_state = VTT_SEARCH;
    char* store = 0;

    if ( !spex || !spex->filename ) {
        goto CLEAR_ERROR;
    }
    fd = fopen( spex->filename, "r" );
    if( !fd )
        goto CLEAR_ERROR;
        
    // Skip Header
    line = subtitle_get_next_line( line, LINE_LEN, fd );
    if (!line || strncmp(line, "WEBVTT", 6) != 0) {
         goto CLEAR_ERROR;
    }

    memset(line,0,LINE_LEN);
    line = subtitle_get_next_line( line, LINE_LEN, fd );

    while ( line ) {
        vtt_chop(line);

        switch ( vtt_state ) {
        case VTT_SEARCH: {
            if (*line == '\0') {
                 // Empty line, continue searching
            } else if (strncmp(line, "NOTE", 4) == 0) {
                // Comment block, skip until empty line
                while(line && *line != '\0') {
                     memset(line,0,LINE_LEN);
                     line = subtitle_get_next_line( line, LINE_LEN, fd );
                     if(line) vtt_chop(line);
                }
            } else if (strstr(line, "-->")) {
                // This is a time line
                // Go to VTT_TIME logic directly (simulated by falling through or goto?)
                // Let's just handle it here to avoid goto or complex state
                int start, end;
                if ( subtitle_get_vtt_time( line, &start, &end ) ) {
                     DBG serprintf( "subtitle: VTT time error line %s\n", line );
                     // maybe it was an ID after all? but it had -->
                     // ignore and reset
                } else {
                     new_line = acalloc(1, sizeof( sub_line ) );
                     new_line->start = start;
                     new_line->end   = end;
                     vtt_state = VTT_TEXT;
                }
            } else {
                // Probably an ID (cue identifier)
                // Next line SHOULD be time
                vtt_state = VTT_TIME;
            }
            break;
        }
        case VTT_TIME: {
             // We expected time line here
             if (strstr(line, "-->")) {
                int start, end;
                if ( subtitle_get_vtt_time( line, &start, &end ) ) {
                     DBG serprintf( "subtitle: VTT time error line %s\n", line );
                     vtt_state = VTT_SEARCH; // Abort this cue
                } else {
                     new_line = acalloc(1, sizeof( sub_line ) );
                     new_line->start = start;
                     new_line->end   = end;
                     vtt_state = VTT_TEXT;
                }
             } else {
                 // We expected time but got something else.
                 // Maybe previous line wasn't an ID but garbage? 
                 // Reset to search
                 vtt_state = VTT_SEARCH;
                 continue; // Re-evaluate this line as SEARCH
             }
             break;
        }
        case VTT_TEXT: {
            if (*line == '\0') {
                // End of cue
                if ( new_line ) {
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
                vtt_state = VTT_SEARCH;
            } else {
                // Text content
#ifdef CONFIG_I18N
				if( !spex->utf8 ) {
                    // ... (utf8 conversion logic same as SRT, reusing code from SRT would be ideal but copying for now)
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
				}
#endif
				store = subtitle_clean_formatter(line, clean_tags);
                if(new_line){
					if ( new_line->top == 0 ) {
						new_line->top = amalloc( strlen( store ) + 1 );
						strcpy( new_line->top, store );
					} else { 
						if ( new_line->bottom == 0 ) {
							new_line->bottom = amalloc( strlen( store ) + 1 );
							strcpy( new_line->bottom, store );
						} else {
							if((strlen(new_line->bottom) + strlen(store)) < LINE_LEN){
								new_line->bottom = arealloc( new_line->bottom,
												strlen( store ) + strlen( new_line->bottom ) + 2 );
								strcat( new_line->bottom, store );
							}
						}
					}
				}
				afree(store);
				store = 0;
            }
            break;
        }
        }
        
        memset(line,0,LINE_LEN);
        line = subtitle_get_next_line( line, LINE_LEN, fd );
    }

    // Handle last entry if file ends without newline
	if( new_line ) { 
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

    if(fd) fclose(fd);
    return sub_record;

CLEAR_ERROR:
    if(fd) fclose(fd);
    if(store) afree(store);
    subtitle_clean_error(sub_record);
    afree(sub_record);
    if(new_line) free(new_line);
    return 0;
}

static struct SUBTITLE_FORMAT VTT = {
	"WebVTT",
	detect_VTT,
	NULL,       // no info
	parse_VTT,
};

SUBTITLE_REGISTER_FORMAT( VTT );
