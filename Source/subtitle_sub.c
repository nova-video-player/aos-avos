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

static int detect_SUB( FILE * file )
{
	//make sure that read starts at the beginning of file
	fseek( file, 0, SEEK_SET );

	// Skip BOM if present (UTF-8: EF BB BF) -- otherwise the unconditional
	// line++ below skips past a BOM byte instead of the leading '{', and
	// every subsequent offset (strchr for '}', atoi position) is thrown off
	// by however many BOM bytes remain, silently rejecting a valid file.
	int c0 = fgetc(file), c1 = fgetc(file), c2 = fgetc(file);
	if( !((unsigned char)c0 == 0xEF && (unsigned char)c1 == 0xBB && (unsigned char)c2 == 0xBF) ) {
		fseek( file, 0, SEEK_SET );
	}

	char _line[ LINE_LEN + 1 ];
	char* line = _line;
	line = subtitle_get_next_line( line, 83, file );
	//minimum line len for SUB
	if ( line && strlen( line ) > 6 ) {
		line++;
		line = strchr( line, '}' );
		if ( !line ) {
			goto ErrorExit;
		}
		line += 2;
		//sub format is {xx}{yy} so if {
		if ( atoi( line ) != 0 ) {
			line = strchr( line, '}' );
			if ( line ) {
				line++;

				if ( strlen( line ) > 2 ) {
					//At least first line is like in SUB
DBG serprintf( "SUB: found!\n" );
					return 0;
				}
			}
		}
	}
ErrorExit:
DBG printf( "SUB: not SUB\n" );
	return 1;

}

// ---------------------------------------------------------------------------
// microdvd_style_codes_to_ass — MicroDVD-specific tag layer.
//
// Per the shared/format-specific split (mirrors subtitle_vtt.c's
// vtt_translate_format_tags): MicroDVD's OWN control codes -- {y:...},
// {c:$BBGGRR}, {f:name}, {s:size} -- are not shared with any other format
// and have no meaning to the common layer downstream (srt_text_to_ass() in
// sub_format_srt.c, which only knows <b>/<i>/<u>/<font> and passes existing
// "{\...}" ASS blocks through untouched). They must be translated HERE,
// before the text leaves this file.
//
// Handles, in a single left-to-right pass over `in`:
//   {y:i}   -> {\i1}   italic
//   {y:b}   -> {\b1}   bold
//   {y:u}   -> {\u1}   underline
//   {y:s}   -> {\s1}   strikethrough
//   {y:ib}  -> {\i1}{\b1}  (any combination of the above letters)
//   {c:$BBGGRR} or {c:BBGGRR} -> {\c&HBBGGRR&}
//       MicroDVD color is already BGR hex (unlike HTML's #RRGGBB) -- no
//       byte-swap needed, just reformat the delimiters.
//   {f:...} and {s:...} (font name / font size) are DROPPED, not
//       translated: MicroDVD {s:size} is an opaque player-defined unit,
//       not ASS points, and {f:name} may not name an installed/available
//       font -- emitting a guessed {\fnXXX}/{\fsXXX} could look worse than
//       just falling back to the track's default style, so we don't.
//
// Per the documented spec examples, a control code appears once (typically
// at the start of the line) and applies to the rest of that line/cue -- it
// is NOT treated as a paired open/close toggle here, since no source
// confirms toggle semantics and every example shows single use.
//
// `out` must be at least in_size * 4 + 32 bytes (worst case: a run of
// "{y:ibus}" codes at 7 input chars each expanding to 4 flags * 5 chars =
// 20 output chars, ~2.9x per-byte -- 4x plus fixed headroom is comfortable
// margin). The function also bounds-checks internally as defense in depth,
// so a caller passing a slightly undersized buffer degrades to truncated
// output rather than a heap overflow. Returns bytes written, excluding NUL.
// ---------------------------------------------------------------------------
static int microdvd_style_codes_to_ass( const char *in, int in_size, char *out, int out_size )
{
	int i = 0, j = 0;
	// Reserve room for the NUL terminator throughout.
	while( i < in_size && in[i] != '\0' && j < out_size - 1 ) {
		if( in[i] != '{' ) {
			out[j++] = in[i++];
			continue;
		}

		char *close = memchr( in + i, '}', in_size - i );
		if( !close ) {
			out[j++] = in[i++];
			continue;
		}
		int tag_len = (int)(close - (in + i)) + 1;
		const char *tag = in + i;

		if( tag_len >= 5 && ( tag[1] == 'y' || tag[1] == 'Y' ) && tag[2] == ':' ) {
			const char *c;
			for( c = tag + 3; c < close && j < out_size - 6; c++ ) {
				if( *c == 'i' || *c == 'I' )      { memcpy( out + j, "{\\i1}", 5 ); j += 5; }
				else if( *c == 'b' || *c == 'B' ) { memcpy( out + j, "{\\b1}", 5 ); j += 5; }
				else if( *c == 'u' || *c == 'U' ) { memcpy( out + j, "{\\u1}", 5 ); j += 5; }
				else if( *c == 's' || *c == 'S' ) { memcpy( out + j, "{\\s1}", 5 ); j += 5; }
			}
			i += tag_len;
			continue;
		}
		if( tag_len >= 4 && ( tag[1] == 'c' || tag[1] == 'C' ) && tag[2] == ':' ) {
			const char *hexstart = tag + 3;
			if( *hexstart == '$' ) hexstart++;
			unsigned int bbggrr = 0;
			int parsed = ( close - hexstart ) >= 6 &&
			             sscanf( hexstart, "%6x", &bbggrr ) == 1;
			if( parsed && j < out_size - 14 ) {
				j += sprintf( out + j, "{\\c&H%06X&}", bbggrr );
			}
			i += tag_len;
			continue;
		}
		if( tag_len >= 4 && ( tag[1] == 'f' || tag[1] == 'F' || tag[1] == 's' || tag[1] == 'S' ) && tag[2] == ':' ) {
			// {f:name}/{s:size} (also seen uppercase {F:}/{S:} on the
			// {DEFAULT} line) -- intentionally dropped, see comment above.
			i += tag_len;
			continue;
		}
		if( tag_len >= 4 && ( tag[1] == 'h' || tag[1] == 'H' ) && tag[2] == ':' ) {
			// {H:charset} -- valid only on the {DEFAULT} line, selects the
			// codepage/character-set for the whole file. Not a per-cue
			// style and has no ASS equivalent -- drop.
			i += tag_len;
			continue;
		}

		out[j++] = in[i++];
	}
	out[j] = '\0';
	return j;
}

static uni_sub *parse_SUB( subt_orig *spex, int clean_tags )
{
	// --- NATIVE LIBASS UPGRADE ---
	// Prevent AVOS from destroying HTML colors/italics!
	clean_tags = 0;

	uni_sub *sub_record = acalloc(1, sizeof( uni_sub ) );
	char _line[ LINE_LEN + 1 ];
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

	// Skip BOM if present -- see detect_SUB for why this matters.
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

		char *start_str = strchr(line, '{');
		if(!start_str) {
			memset(line,0,LINE_LEN);
			line = subtitle_get_next_line( line, LINE_LEN, fd );
			continue;
		}
		start_str++;

		char *end_str = strchr(start_str, '}');
		if(!end_str) {
			memset(line,0,LINE_LEN);
			line = subtitle_get_next_line( line, LINE_LEN, fd );
			continue;
		}
		*end_str = '\0';
		end_str++;

		if(*end_str != '{') {
			memset(line,0,LINE_LEN);
			line = subtitle_get_next_line( line, LINE_LEN, fd );
			continue;
		}
		end_str++;

		char *text_str = strchr(end_str, '}');
		if(!text_str) {
			memset(line,0,LINE_LEN);
			line = subtitle_get_next_line( line, LINE_LEN, fd );
			continue;
		}
		*text_str = '\0';
		text_str++;

		new_line = acalloc(1, sizeof( sub_line ) );
		new_line->start = atoi(start_str);
		new_line->end = atoi(end_str);

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

		// MicroDVD-specific {y:}/{c:}/{f:}/{s:} control codes must be
		// translated to ASS override syntax before subtitle_clean_formatter
		// (which is a no-op pass-through with clean_tags==0) and before the
		// text reaches the shared common-tag layer downstream, which has no
		// knowledge of MicroDVD's own tag vocabulary and would otherwise
		// either leak "{y:i}" onto the screen as literal text or -- since
		// that layer treats any "{...}" block as trusted pre-existing ASS
		// and passes it through untouched -- forward invalid ASS syntax
		// straight to libass.
		{
			// microdvd_style_codes_to_ass' worst case is a run of {y:ibus}
			// (4 flags) at 7 input chars -> 20 output chars, ~2.9x. Sized
			// generously at 4x + fixed headroom; the function also bounds-
			// checks internally regardless.
			char ass_top[ LINE_LEN * 4 + 32 ];
			microdvd_style_codes_to_ass( text_str, (int)strlen(text_str), ass_top, sizeof(ass_top) );
			store = subtitle_clean_formatter(ass_top, clean_tags);
			new_line->top = astrdup( store );
			afree(store);
		}

		if( line_bottom ) {
			char ass_bottom[ LINE_LEN * 4 + 32 ];
			microdvd_style_codes_to_ass( line_bottom, (int)strlen(line_bottom), ass_bottom, sizeof(ass_bottom) );
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
		sub_record->frame_multiplier = 1; // MicroDVD is frame-based
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

static struct SUBTITLE_FORMAT SUB = {
	"MicroDVD",
	detect_SUB,
	NULL,		// no info
	parse_SUB,
};

SUBTITLE_REGISTER_FORMAT( SUB );

