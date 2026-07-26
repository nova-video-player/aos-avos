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

char *subtitle_get_next_line( char *start, int len, FILE *fd );

static int detect_MPL( FILE * file )
{
	//make sure that read starts at the beginning of file
	fseek( file, 0, SEEK_SET );

	// Skip BOM if present (UTF-8: EF BB BF) -- otherwise it lands as three
	// garbage bytes prefixed onto the first "[start][stop]" line and the
	// sscanf match below silently fails, rejecting an otherwise-valid file.
	int c0 = fgetc(file), c1 = fgetc(file), c2 = fgetc(file);
	if( !((unsigned char)c0 == 0xEF && (unsigned char)c1 == 0xBB && (unsigned char)c2 == 0xBF) ) {
		fseek( file, 0, SEEK_SET );
	}

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

// ---------------------------------------------------------------------------
// mpl_italic_marker_to_ass — MPL2-specific tag layer.
//
// Per the shared/format-specific split (mirrors subtitle_vtt.c's
// vtt_translate_format_tags and subtitle_sub.c's microdvd_style_codes_to_ass):
// MPL2's own italics marker -- a leading '/' at the start of a physical line
// -- has no meaning to the common layer downstream (srt_text_to_ass() in
// sub_format_srt.c), which only knows the shared <b>/<i>/<u>/<font> HTML
// subset. It must be translated here, before the text leaves this file.
//
// Per spec, italics applies per physical line, not to the whole cue: a cue
// with two lines can mark just one of them italic ("Line 1|/Line 2"), so
// this must be called independently on the top and bottom halves after the
// '|' split, same as MicroDVD's per-line marker.
//
// Writes into `out` (caller-provided, must be at least strlen(in) + 11
// bytes: "{\i1}" + text + "{\i0}" + NUL). Returns bytes written, excluding
// NUL. `in` is not modified.
// ---------------------------------------------------------------------------
static int mpl_italic_marker_to_ass( const char *in, char *out, int out_size )
{
	int has_italic = ( in[0] == '/' );
	const char *text = has_italic ? in + 1 : in;
	int text_len = (int)strlen( text );

	if( !has_italic ) {
		int n = text_len < out_size - 1 ? text_len : out_size - 1;
		memcpy( out, text, n );
		out[n] = '\0';
		return n;
	}

	// {\i1} + text + {\i0} + NUL, bounds-checked against out_size.
	int n = snprintf( out, out_size, "{\\i1}%s{\\i0}", text );
	if( n < 0 ) { out[0] = '\0'; return 0; }
	if( n >= out_size ) n = out_size - 1; // snprintf already NUL-terminated at truncation
	return n;
}

static uni_sub *parse_MPL( subt_orig *spex, int clean_tags )
{
	// --- NATIVE LIBASS UPGRADE ---
	// Prevent AVOS from destroying HTML colors/italics!
	clean_tags = 0;

	uni_sub *sub_record = acalloc( 1, sizeof( uni_sub ) );
	char _line[LINE_LEN + 1];
	memset(_line,0,LINE_LEN);
	char *line = _line;
	FILE *fd = 0;
	sub_line *new_line = 0;
	char *store = 0;

	if ( !spex ) {
		goto CLEAR_ERROR;
	}
	if ( !spex->filename ) {
		goto CLEAR_ERROR;
	}
	fd = fopen( spex->filename, "r" );
	if( !fd ) {
		goto CLEAR_ERROR;
	}

	// Skip BOM if present -- see detect_MPL for why this matters even
	// though parse_MPL's own strchr-based scanning is more forgiving than
	// detect_MPL's anchored sscanf.
	{
		int c0 = fgetc(fd), c1 = fgetc(fd), c2 = fgetc(fd);
		if( !((unsigned char)c0 == 0xEF && (unsigned char)c1 == 0xBB && (unsigned char)c2 == 0xBF) )
			fseek( fd, 0, SEEK_SET );
	}

	line = subtitle_get_next_line( line, LINE_LEN, fd );
	while ( line ) {
		char *tmp = strchr( line, NEW_LINE_CH );
		if( tmp ) {
			*tmp = '\0';
		}
		tmp = strchr( line, MS_CURSOR_BEGIN );
		if( tmp ) {
			*tmp = '\0';
		}

		if(*line == '\0'){
			memset(line,0,LINE_LEN);
			line = subtitle_get_next_line( line, LINE_LEN, fd );
			continue;
		}

		char *start_str = strchr(line, '[');
		if(!start_str) {
			memset(line,0,LINE_LEN);
			line = subtitle_get_next_line( line, LINE_LEN, fd );
			continue;
		}
		start_str++;

		char *end_str = strchr(start_str, ']');
		if(!end_str) {
			memset(line,0,LINE_LEN);
			line = subtitle_get_next_line( line, LINE_LEN, fd );
			continue;
		}
		*end_str = '\0';
		end_str++;

		if(*end_str != '[') {
			memset(line,0,LINE_LEN);
			line = subtitle_get_next_line( line, LINE_LEN, fd );
			continue;
		}
		end_str++;

		char *text_str = strchr(end_str, ']');
		if(!text_str) {
			memset(line,0,LINE_LEN);
			line = subtitle_get_next_line( line, LINE_LEN, fd );
			continue;
		}
		*text_str = '\0';
		text_str++;

		new_line = acalloc( 1, sizeof( sub_line ) );
		new_line->start = atoi(start_str) * 100; // MPL is in tenths of a second
		new_line->end = atoi(end_str) * 100;

		char *line_bottom = strchr( text_str, '|' );
		if ( line_bottom ) {
			*line_bottom = '\0';
			line_bottom++;
		}

		#ifdef CONFIG_I18N
		if( !spex->utf8 ) {
			wchar unicode[ LINE_LEN + 1 ];
			memset(unicode,0, LINE_LEN);
			wchar *uc = unicode;
			char *c = text_str;
			while( *c ) {
				c += I18N_codepage_to_unicode( c, uc );
				uc++;
			}
			utf16_to_utf8( text_str, unicode, LINE_LEN );

			if (line_bottom) {
				memset(unicode,0, LINE_LEN);
				uc = unicode;
				c = line_bottom;
				while( *c ) {
					c += I18N_codepage_to_unicode( c, uc );
					uc++;
				}
				utf16_to_utf8( line_bottom, unicode, LINE_LEN );
			}
		}
		#endif

		// MPL2's leading-'/' italic marker must be translated to ASS before
		// subtitle_clean_formatter (a no-op pass-through with clean_tags==0)
		// and before the text reaches the shared common-tag layer
		// downstream, which has no knowledge of MPL2's own marker syntax
		// and would otherwise leak a literal '/' onto the screen.
		{
			char ass_top[ LINE_LEN + 16 ];
			mpl_italic_marker_to_ass( text_str, ass_top, sizeof(ass_top) );
			store = subtitle_clean_formatter(ass_top, clean_tags);
			new_line->top = astrdup( store );
			afree(store);
		}

		if ( line_bottom ) {
			char ass_bottom[ LINE_LEN + 16 ];
			mpl_italic_marker_to_ass( line_bottom, ass_bottom, sizeof(ass_bottom) );
			store = subtitle_clean_formatter(ass_bottom, clean_tags);

			// --- NATIVE LIBASS UPGRADE ---
			// Do not split into bottom! Concatenate using ASS \N line break!
			new_line->top = arealloc(new_line->top, strlen(new_line->top) + strlen(store) + 3);
			strcat(new_line->top, "\\N");
			strcat(new_line->top, store);
			afree(store);
		}

		if ( sub_record->first == 0 ) {
			sub_record->first = new_line;
			sub_record->last = new_line;
		} else {
			sub_record->last->next = new_line;
			new_line->prev = sub_record->last;
			sub_record->last = new_line;
		}

		memset(line,0,LINE_LEN);
		line = subtitle_get_next_line( line, LINE_LEN, fd );
	}

	if(fd) {
		fclose(fd);
	}

	if ( sub_record && sub_record->first ) {
		return sub_record;
	}

	CLEAR_ERROR:
	if(fd) {
		fclose(fd);
	}
	subtitle_clean_error(sub_record);
	afree(sub_record);
	return 0;
}

static struct SUBTITLE_FORMAT MPL = {
	"MPL2",
	detect_MPL,
	NULL,		// no info
	parse_MPL,
};

SUBTITLE_REGISTER_FORMAT( MPL );

