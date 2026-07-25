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

    // VTT must start with WEBVTT, optionally preceded by a UTF-8 BOM (EF BB BF).
    // The Xfgets macro skips blank/CR/LF lines but does NOT strip the BOM,
    // so we must check for it explicitly before the WEBVTT signature.
    char *p = line;
    if( (unsigned char)p[0] == 0xEF &&
        (unsigned char)p[1] == 0xBB &&
        (unsigned char)p[2] == 0xBF ) {
        p += 3; // skip BOM
    }
    if (strncmp(p, "WEBVTT", 6) == 0) {
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

// ---------------------------------------------------------------------------
// feed_VTT — streaming single-pass path
//
// Parses the VTT file and fires cb(ctx, text, start_ms, end_ms) for every
// cue as it is parsed. No sub_line allocation, no linked list, no second
// pass. The callback converts each cue to an ASS Dialogue event and feeds
// it directly to sub_engine_feed().
// ---------------------------------------------------------------------------
static void feed_VTT( subt_orig *spex, sub_cue_cb cb, void *ctx )
{
    if( !spex || !spex->filename || !cb ) return;

    char _line[ LINE_LEN + 1 ];
    memset( _line, 0, LINE_LEN );
    char *line = _line;
    char  cue_text[ LINE_LEN * 2 + 4 ];
    cue_text[0] = '\0';
    char *store = 0;
    int   cue_start = 0, cue_end = 0;
    int   vtt_state = VTT_SEARCH;

    FILE *fd = fopen( spex->filename, "r" );
    if( !fd ) return;

    // Skip WEBVTT header line (including optional BOM)
    line = subtitle_get_next_line( line, LINE_LEN, fd );
    if( !line ) { fclose(fd); return; }
    {
        char *p = line;
        if( (unsigned char)p[0]==0xEF && (unsigned char)p[1]==0xBB && (unsigned char)p[2]==0xBF ) p += 3;
        if( strncmp(p, "WEBVTT", 6) != 0 ) { fclose(fd); return; }
    }

    memset( line, 0, LINE_LEN );
    line = subtitle_get_next_line( line, LINE_LEN, fd );

    while( line ) {
        vtt_chop( line );

        switch( vtt_state ) {
            case VTT_SEARCH: {
                if( *line == '\0' ) break;
                if( strncmp(line, "NOTE", 4) == 0 ) {
                    // Skip comment block
                    while( line && *line != '\0' ) {
                        memset(line,0,LINE_LEN);
                        line = subtitle_get_next_line(line,LINE_LEN,fd);
                        if(line) vtt_chop(line);
                    }
                    break;
                }
                if( strstr(line, "-->") ) {
                    if( subtitle_get_vtt_time(line, &cue_start, &cue_end) ) {
                        DBG serprintf("VTT feed: time error %s\n", line);
                    } else {
                        cue_text[0] = '\0';
                        vtt_state = VTT_TEXT;
                    }
                } else {
                    vtt_state = VTT_TIME; // line is a cue ID
                }
                break;
            }
            case VTT_TIME: {
                if( strstr(line, "-->") ) {
                    if( subtitle_get_vtt_time(line, &cue_start, &cue_end) ) {
                        DBG serprintf("VTT feed: time error %s\n", line);
                        vtt_state = VTT_SEARCH;
                    } else {
                        cue_text[0] = '\0';
                        vtt_state = VTT_TEXT;
                    }
                } else {
                    vtt_state = VTT_SEARCH;
                }
                break;
            }
            case VTT_TEXT: {
                if( *line == '\0' ) {
                    // Blank line — cue complete, fire callback
                    if( cue_text[0] ) cb( ctx, cue_text, cue_start, cue_end );
                    cue_text[0] = '\0';
                    vtt_state = VTT_SEARCH;
                    break;
                }
                #ifdef CONFIG_I18N
                if( !spex->utf8 ) {
                    wchar unicode[ LINE_LEN + 1 ];
                    memset(unicode, 0, LINE_LEN);
                    wchar *uc = unicode;
                    char  *c  = line;
                    while(*c) { c += I18N_codepage_to_unicode(c, uc); uc++; }
                    utf16_to_utf8(line, unicode, LINE_LEN);
                }
                #endif
                store = subtitle_clean_formatter(line, 0); // clean_tags always 0
                if( store ) {
                    if( cue_text[0] ) {
                        if( strlen(cue_text) + strlen(store) + 3 < sizeof(cue_text) ) {
                            strcat(cue_text, "\\N");
                            strcat(cue_text, store);
                        }
                    } else {
                        strncpy(cue_text, store, sizeof(cue_text)-1);
                        cue_text[sizeof(cue_text)-1] = '\0';
                    }
                    afree(store);
                    store = 0;
                }
                break;
            }
        }

        memset(line, 0, LINE_LEN);
        line = subtitle_get_next_line(line, LINE_LEN, fd);
    }

    // Last cue if file doesn't end with blank line
    if( cue_text[0] ) cb( ctx, cue_text, cue_start, cue_end );
    if( store ) afree(store);
    fclose(fd);
}

// parse_VTT — stub kept so subtitle_get_converted() accepts the track.
// Returns an empty but valid uni_sub. Actual data delivered via feed_VTT.
static uni_sub *parse_VTT( subt_orig *spex, int clean_tags )
{
    (void)clean_tags;
    if( !spex || !spex->filename ) return NULL;
    uni_sub *sub_record = acalloc(1, sizeof(uni_sub));
    return sub_record;
}

static struct SUBTITLE_FORMAT VTT = {
	"WebVTT",
	detect_VTT,
	NULL,		// no info
	parse_VTT,
	NULL,		// no get_gfx
	NULL,		// no close
	feed_VTT,	// streaming single-pass feed — primary path
};

SUBTITLE_REGISTER_FORMAT( VTT );
