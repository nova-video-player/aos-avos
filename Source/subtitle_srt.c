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

	// Skip BOM if present (UTF-8: EF BB BF)
	int c0 = fgetc(file), c1 = fgetc(file), c2 = fgetc(file);
	if( !((unsigned char)c0 == 0xEF && (unsigned char)c1 == 0xBB && (unsigned char)c2 == 0xBF) ) {
		// Not a BOM — rewind
		fseek( file, 0, SEEK_SET );
	}

	// Read line 1 (cue index number) then line 2 (timestamp)
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

// In-memory equivalent of subtitle_get_next_line(), for callers (feed_SRT, feed_VTT) that read the whole file into a buffer upfront; same fgets()-like contract, but an oversized line is truncated into `out` while `*cursor` still advances past the full physical line so the next call resyncs. Returns NULL at EOF.
char *subtitle_get_next_line_from_buffer( const char **cursor, const char *end, char *out, int len )
{
	if( *cursor >= end ) return NULL;

	const char *p  = *cursor;
	const char *nl = memchr( p, '\n', end - p );
	const char *line_end = nl ? nl + 1 : end; // include the '\n', like fgets()

	int phys_len = (int)(line_end - p);
	int out_len  = phys_len < len ? phys_len : len - 1;

	memcpy( out, p, out_len );
	out[out_len] = '\0';

	*cursor = line_end;
	return out;
}

// Streaming single-pass SRT parser: fires cb() per cue as parsed, no linked list or second pass; this is the only path (SRT.parse is NULL below). `poll` is checked every SRT_POLL_INTERVAL lines -- on interruption the loop just stops (caller retries later) and `interrupted` suppresses flushing a genuinely-incomplete trailing cue.
#define SRT_POLL_INTERVAL 200

static void feed_SRT( subt_orig *spex, sub_cue_cb cb, void *ctx, sub_feed_poll_cb poll, void *poll_ctx )
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

	// Skip BOM
	if( bytes_read >= 3 && (unsigned char)cursor[0] == 0xEF && (unsigned char)cursor[1] == 0xBB && (unsigned char)cursor[2] == 0xBF ) {
		cursor += 3;
	}

	char  _line[ LINE_LEN + 1 ];
	char *line;
	char  cue_text[ LINE_LEN * 2 + 4 ];
	cue_text[0] = '\0';

	int srt_state = SRT_NR;
	int next_index = 0;
	int cue_start = 0, cue_end = 0;
	char *store = 0;
	int line_count = 0;
	int interrupted = 0;

	line = subtitle_get_next_line_from_buffer( &cursor, end, _line, LINE_LEN + 1 );
	while( line ) {
		if( poll && (++line_count % SRT_POLL_INTERVAL) == 0 && poll( poll_ctx ) ) {
			interrupted = 1;
			break;
		}
		switch( srt_state ) {
			case SRT_NR: {
				srt_chop( line );
				if( *line == '\0' ) break;
				next_index = atoi( line ) + 1;
				srt_state  = SRT_TIME;
				break;
			}
			case SRT_TIME: {
				if( subtitle_get_srt_time( line, &cue_start, &cue_end ) ) {
					DBG serprintf( "SRT feed: time error line %s\n", line );
					srt_state = SRT_NR;
					break;
				}
				cue_text[0] = '\0';
				srt_state   = SRT_TEXT;
				break;
			}
			case SRT_TEXT: {
				srt_chop( line );
				if( line[0] == NEW_LINE_CH || line[0] == MS_CURSOR_BEGIN || line[0] == '\0' ) {
					// Blank line — cue is complete, fire callback
					if( cue_text[0] ) {
						cb( ctx, cue_text, cue_start, cue_end );
					}
					cue_text[0] = '\0';
					srt_state   = SRT_NR;
					break;
				}
				#ifdef CONFIG_I18N
				if( !spex->utf8 ) {
					wchar unicode[ LINE_LEN + 1 ];
					memset( unicode, 0, LINE_LEN );
					wchar *uc = unicode;
					char  *c  = line;
					while( *c ) { c += I18N_codepage_to_unicode( c, uc ); uc++; }
					utf16_to_utf8( line, unicode, LINE_LEN );
				}
				#endif
				store = subtitle_clean_formatter( line, 0 ); // clean_tags always 0
				if( store ) {
					if( cue_text[0] ) {
						// Multi-line: append \N separator
						if( strlen(cue_text) + strlen(store) + 3 < sizeof(cue_text) ) {
							strcat( cue_text, "\\N" );
							strcat( cue_text, store );
						}
					} else {
						strncpy( cue_text, store, sizeof(cue_text) - 1 );
						cue_text[sizeof(cue_text)-1] = '\0';
					}
					afree( store );
					store = 0;
				}
				break;
			}
		}
		line = subtitle_get_next_line_from_buffer( &cursor, end, _line, LINE_LEN + 1 );
	}

	// Flush the trailing cue only at true EOF -- on interruption it may still be incomplete, and a full re-parse will happen next pass anyway.
	if( !interrupted && cue_text[0] ) cb( ctx, cue_text, cue_start, cue_end );
	if( store ) afree( store );
	afree( buf );
}

static struct SUBTITLE_FORMAT SRT = {
	"SubRip",
	detect_SRT,
	NULL,		// no info
	NULL,		// no parse -- feed_SRT (streaming) is the only path; NULL .parse is treated as an immediate SUBT_PARSE_FAILED (see subtitle_do_parse())
	NULL,		// no get_gfx
	NULL,		// no close
	feed_SRT,	// streaming single-pass feed — primary path
};

SUBTITLE_REGISTER_FORMAT( SRT );
