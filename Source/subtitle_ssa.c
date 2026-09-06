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

/*
 * This is a complete rewrite of the external SSA parser.
 *
 * Unlike all other subtitle_*.c parsers, this one does NOT build a uni_sub
 * linked list of cue nodes. ASS/SSA files are fed directly to Libass as a
 * raw buffer via ass_process_data(), so the full script — [Script Info],
 * [V4+ Styles], [Events], embedded fonts — is preserved exactly as the
 * fansubber intended.
 *
 * parse_SSA() reads the entire file into a heap buffer and stores it in
 * uni_sub->raw_data / uni_sub->raw_size. stream_sub_ext_feed_engine() in
 * stream_sub_ext.c detects is_ssa=1 and calls sub_engine_feed_raw() instead
 * of the normal per-cue sub_engine_feed() loop.
 */

#include "global.h"
#include "debug.h"
#include "astdlib.h"
#include "subtitle_format.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define DBG if(Debug[DBG_SUB])

// ---------------------------------------------------------------------------
// detect_SSA
//
// ASS/SSA files always start with [Script Info] as the first non-blank line.
// Both .ass and .ssa use the same header — the version is indicated inside
// the ScriptType field, not the file extension.
// ---------------------------------------------------------------------------
static int detect_SSA( FILE *file )
{
	//make sure that read starts at the beginning of file
	fseek( file, 0, SEEK_SET );

	char line[256];
	while ( fgets( line, sizeof(line), file ) ) {
		char *p = line;
		// Strip UTF-8 BOM (EF BB BF) if present on this line
		if ( (unsigned char)p[0] == 0xEF &&
			(unsigned char)p[1] == 0xBB &&
			(unsigned char)p[2] == 0xBF ) {
			p += 3;
		}
		// Strip trailing whitespace/newlines
		char *end = p + strlen(p) - 1;
		while ( end >= p && (*end == '\r' || *end == '\n' || *end == ' ') ) *end-- = '\0';
		// Skip leading whitespace
		while ( *p == ' ' || *p == '\t' ) p++;
		// Skip blank lines
		if ( *p == '\0' ) continue;

		if ( strncmp( p, "[Script Info]", 13 ) == 0 ) {
			DBG serprintf( "SSA: found!\n" );
			return 0; // detected
		}
		// First non-blank line didn't match — not ASS/SSA
		break;
	}

	DBG serprintf( "SSA: not SSA\n" );
	return 1;
}

// ---------------------------------------------------------------------------
// parse_SSA
//
// Reads the entire file into a single heap buffer. No cue parsing —
// Libass will do that internally when we call ass_process_data().
//
// Sets uni_sub->is_ssa = 1 so stream_sub_ext.c routes it to the SSA
// engine path instead of the SRT bulk-feed path.
// ---------------------------------------------------------------------------
static uni_sub *parse_SSA( subt_orig *spex, int clean_tags )
{
	// ASS styling must NEVER be stripped — override regardless of caller
	(void)clean_tags;

	if ( !spex || !spex->filename ) {
		DBG serprintf( "SSA: invalid params\n" );
		return NULL;
	}

	FILE *fd = fopen( spex->filename, "rb" );
	if ( !fd ) {
		DBG serprintf( "SSA: cannot open %s\n", spex->filename );
		return NULL;
	}

	// Get file size
	fseek( fd, 0, SEEK_END );
	long file_size = ftell( fd );
	fseek( fd, 0, SEEK_SET );

	if ( file_size <= 0 ) {
		fclose( fd );
		return NULL;
	}

	char *buf = amalloc( file_size + 1 );
	if ( !buf ) {
		fclose( fd );
		return NULL;
	}

	long bytes_read = (long)fread( buf, 1, file_size, fd );
	fclose( fd );

	if ( bytes_read <= 0 ) {
		afree( buf );
		return NULL;
	}
	buf[bytes_read] = '\0';

	uni_sub *sub_record = acalloc( 1, sizeof( uni_sub ) );
	if ( !sub_record ) {
		afree( buf );
		return NULL;
	}

	// Store raw buffer — stream_sub_ext_feed_engine() will pass this
	// directly to ass_process_data() / sub_engine_feed_raw()
	sub_record->raw_data  = buf;
	sub_record->raw_size  = (int)bytes_read;
	sub_record->is_ssa    = 1;

	// Provide a dummy first/last so subtitle_get_converted()'s count logic
	// doesn't reject this entry (it checks converted[i] != NULL only)
	sub_record->first = NULL;
	sub_record->last  = NULL;

	DBG serprintf( "SSA: loaded %ld bytes from %s\n", bytes_read, spex->filename );
	return sub_record;
}

// ---------------------------------------------------------------------------
// Format registration
// ---------------------------------------------------------------------------
static struct SUBTITLE_FORMAT SSA = {
	"ASS/SSA",
	detect_SSA,
	NULL,       // no info() — ASS language tracks handled by Libass internally
	parse_SSA,
	NULL,		// no get_gfx
	NULL,		// no close
	NULL,		// no feed — raw buffer path via is_ssa flag, not streaming callback
};

SUBTITLE_REGISTER_FORMAT( SSA );
