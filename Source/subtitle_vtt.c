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
// In-memory equivalent, for feed_VTT's full-file buffered read -- see its contract in subtitle_srt.c.
extern char *subtitle_get_next_line_from_buffer( const char **cursor, const char *end, char *out, int len );

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

// Translates VTT-only tags in place before the shared tag layer (srt_text_to_ass) sees them: <v Name>text</v> -> "Name: text", <c.class>/<lang...> unwrapped to plain text; <b>/<i>/<u>/<font> are left alone since those are shared tags handled downstream.
static void vtt_translate_format_tags( char *line )
{
    char out[ LINE_LEN * 2 ];
    int  o = 0;
    char *p = line;

    while( *p && o < (int)sizeof(out) - 1 ) {
        if( *p != '<' ) {
            out[o++] = *p++;
            continue;
        }

        char *tag_end = strchr( p, '>' );
        if( !tag_end ) {
            // Unterminated tag — copy the rest verbatim rather than losing it.
            while( *p && o < (int)sizeof(out) - 1 ) out[o++] = *p++;
            break;
        }
        int tag_len = (int)(tag_end - p) + 1;

        // Is this one of the shared tags (owned by the common layer
        // downstream, srt_text_to_ass())? Check the actual boundary char,
        // not just a prefix -- "<b2" or "<big" must NOT match "<b".
        int is_shared =
            (tag_len >= 3 && (p[1]=='b'||p[1]=='B') && p[2]=='>') ||
            (tag_len >= 4 && p[1]=='/' && (p[2]=='b'||p[2]=='B') && p[3]=='>') ||
            (tag_len >= 3 && (p[1]=='i'||p[1]=='I') && p[2]=='>') ||
            (tag_len >= 4 && p[1]=='/' && (p[2]=='i'||p[2]=='I') && p[3]=='>') ||
            (tag_len >= 3 && (p[1]=='u'||p[1]=='U') && p[2]=='>') ||
            (tag_len >= 4 && p[1]=='/' && (p[2]=='u'||p[2]=='U') && p[3]=='>') ||
            (tag_len >= 7 && !strncmpNC(p+1, "font", 4) && (p[5]=='>' || p[5]==' ')) ||
            (tag_len >= 7 && p[1]=='/' && !strncmpNC(p+2, "font", 4) && p[6]=='>');

        if( is_shared ) {
            // Shared tag — not ours to translate. Copy the whole tag through
            // verbatim so the common layer (srt_text_to_ass) sees it intact.
            int n = tag_len;
            if( o + n > (int)sizeof(out) - 1 ) n = (int)sizeof(out) - 1 - o;
            memcpy( out + o, p, n );
            o += n;
            p = tag_end + 1;
            continue;
        }

        if( (p[1] == 'v' || p[1] == 'V') && (p[2] == ' ' || p[2] == '.' || p[2] == '>') ) {
            // <v[.class] Speaker Name> — extract the speaker name (skip any
            // leading .class token(s) and the space separating them from
            // the name) and fold it into the visible text as
            // "Speaker Name: ". The matching </v> just gets unwrapped below.
            char *name_start = p + 2;
            while( *name_start == '.' ) {
                while( *name_start && *name_start != ' ' && *name_start != '>' ) name_start++;
            }
            while( *name_start == ' ' ) name_start++;
            int name_len = (int)(tag_end - name_start);
            if( name_len > 0 && o + name_len + 2 < (int)sizeof(out) - 1 ) {
                memcpy( out + o, name_start, name_len );
                o += name_len;
                out[o++] = ':';
                out[o++] = ' ';
            }
            p = tag_end + 1;
            continue;
        }
        if( tag_len >= 4 && !strncmpNC(p, "</v", 3) && p[3] == '>' ) {
            // Closing </v> carries no text of its own — just unwrap it.
            p = tag_end + 1;
            continue;
        }

        // <c...>, </c>, <lang...>, or any other VTT-only/unknown tag: no
        // shared ASS equivalent and no text worth extracting beyond what's
        // already inside — unwrap (drop the tag, keep the enclosed text
        // which will follow as plain characters on the next loop iterations).
        p = tag_end + 1;
    }
    out[o] = '\0';

    // line's caller buffer is LINE_LEN+1 sized (see feed_VTT's _line[]);
    // out can only be <= the input length since every branch either copies
    // through 1:1 or shortens, so this always fits.
    strcpy( line, out );
}

// Streaming single-pass VTT parser, mirroring feed_SRT() in subtitle_srt.c: fires cb() per cue, no linked list; `poll` is checked every VTT_POLL_INTERVAL lines.
#define VTT_POLL_INTERVAL 200

static void feed_VTT( subt_orig *spex, sub_cue_cb cb, void *ctx, sub_feed_poll_cb poll, void *poll_ctx )
{
    if( !spex || !spex->filename || !cb ) return;

    // Full-file buffered read: one read() instead of many fgets() round-trips (costly on network shares); parse from RAM instead.
    FILE *fd = fopen( spex->filename, "rb" );
    if( !fd ) return;

    fseek( fd, 0, SEEK_END );
    long file_size = ftell( fd );
    fseek( fd, 0, SEEK_SET );
    if( file_size <= 0 ) { fclose( fd ); return; }

    char *buf = amalloc( file_size + 1 );
    if( !buf ) { fclose( fd ); return; }

    long bytes_read = (long)fread( buf, 1, file_size, fd );
    fclose( fd );
    if( bytes_read <= 0 ) { afree( buf ); return; }
    buf[bytes_read] = '\0';

    const char *cursor = buf;
    const char *end     = buf + bytes_read;

    char _line[ LINE_LEN + 1 ];
    char *line;
    char  cue_text[ LINE_LEN * 2 + 4 ];
    cue_text[0] = '\0';
    char *store = 0;
    int   cue_start = 0, cue_end = 0;
    int   vtt_state = VTT_SEARCH;
    int   line_count = 0;
    int   interrupted = 0;

    // Skip WEBVTT header line (including optional BOM)
    line = subtitle_get_next_line_from_buffer( &cursor, end, _line, LINE_LEN + 1 );
    if( !line ) { afree( buf ); return; }
    {
        char *p = line;
        if( (unsigned char)p[0]==0xEF && (unsigned char)p[1]==0xBB && (unsigned char)p[2]==0xBF ) p += 3;
        if( strncmp(p, "WEBVTT", 6) != 0 ) { afree( buf ); return; }
    }

    line = subtitle_get_next_line_from_buffer( &cursor, end, _line, LINE_LEN + 1 );

    while( line ) {
        if( poll && (++line_count % VTT_POLL_INTERVAL) == 0 && poll( poll_ctx ) ) {
            interrupted = 1;
            break;
        }
        vtt_chop( line );

        switch( vtt_state ) {
            case VTT_SEARCH: {
                if( *line == '\0' ) break;
                if( strncmp(line, "NOTE", 4) == 0 ) {
                    // Skip comment block
                    while( line && *line != '\0' ) {
                        line = subtitle_get_next_line_from_buffer( &cursor, end, _line, LINE_LEN + 1 );
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
                // VTT-only tags must be translated here -- the shared tag layer downstream only knows <b>/<i>/<u>/<font> and silently drops anything else.
                vtt_translate_format_tags( line );
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

        line = subtitle_get_next_line_from_buffer( &cursor, end, _line, LINE_LEN + 1 );
    }

    // Flush the trailing cue only at true EOF -- see feed_SRT()'s matching comment in subtitle_srt.c.
    if( !interrupted && cue_text[0] ) cb( ctx, cue_text, cue_start, cue_end );
    if( store ) afree(store);
    afree( buf );
}

static struct SUBTITLE_FORMAT VTT = {
	"WebVTT",
	detect_VTT,
	NULL,		// no info
	NULL,		// no parse -- feed_VTT (streaming) is the only path; NULL .parse is treated as an immediate SUBT_PARSE_FAILED (see subtitle_do_parse())
	NULL,		// no get_gfx
	NULL,		// no close
	feed_VTT,	// streaming single-pass feed — primary path
};

SUBTITLE_REGISTER_FORMAT( VTT );
