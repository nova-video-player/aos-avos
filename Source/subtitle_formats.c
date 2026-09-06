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
#include "astdlib.h"
#include "ctype.h"
#include "debug.h"
#include "iso639.h"
#include "util.h"
#include "i18n.h"
#include "browse.h"
#include "device_config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>		// for some reason atoi() not from astdlib :(

#ifdef CONFIG_SUBTITLES

#include "subtitle_format.h"

#define result_t int
#define state_t int
#define DBG  if(Debug[DBG_SUB])
#define DBG2 if(Debug[DBG_SUB] > 1)

static SUBTITLE_REG_FORMAT *_reg = NULL;

// ************************************************************
//
//	subtitle_register_format
//
// ************************************************************
int subtitle_register_format( SUBTITLE_REG_FORMAT *reg )
{
	if( !_reg ) {
		_reg = reg;
	} else {
		SUBTITLE_REG_FORMAT *head = _reg;
		while( head->next ) {
			head = head->next;
		}
		head->next = reg;
	}
	reg->next = NULL;
	return 0;
}

// Maps a file extension straight to the format that owns it (no file I/O); a hint only, still confirmed by detect(). ".sub" is deliberately ambiguous (MicroDVD text vs VobSub bitmap) and always falls through to brute-force, like an unrecognized extension.
static const SUBTITLE_FORMAT *_format_for_extension( const char *ext )
{
	if( !ext || !ext[0] ) return NULL;

	SUBTITLE_REG_FORMAT *f = _reg;
	while( f ) {
		const char *name = f->format->name;
		if( !strcasecmp( ext, "srt" ) && !strcmp( name, "SubRip" ) ) return f->format;
		if( !strcasecmp( ext, "vtt" ) && !strcmp( name, "WebVTT" ) ) return f->format;
		if( (!strcasecmp( ext, "ass" ) || !strcasecmp( ext, "ssa" )) && !strcmp( name, "ASS/SSA" ) ) return f->format;
		if( (!strcasecmp( ext, "smi" ) || !strcasecmp( ext, "sami" )) && !strcmp( name, "SAMI" ) ) return f->format;
		if( (!strcasecmp( ext, "mpl" ) || !strcasecmp( ext, "txt" )) && !strcmp( name, "MPL2" ) ) return f->format;
		if( !strcasecmp( ext, "sup" ) && !strcmp( name, "PGS" ) ) return f->format;
		if( !strcasecmp( ext, "idx" ) && !strcmp( name, "IDX/SUB" ) ) return f->format;
		f = f->next;
	}
	return NULL;
}

// When non-zero (default): an extension hint is still confirmed by detect(). When 0: an unambiguous hint (srt/vtt/ass/ssa/smi/sami/mpl/sup/idx) is trusted outright with no detect() I/O -- a mismatched file just fails later at parse/feed time instead. Ambiguous extensions (.sub, missing, unrecognized) always go through brute-force detect() regardless. Runtime-toggleable via the "subdt" debug command.
static int USE_DETECT = 1;
DECLARE_DEBUG_PARAM( "subdt", USE_DETECT );

/*********
 * Executes detect function of every subtitle formats and tries to figure
 * out which format the given file is.
 *
 * An unambiguous extension hint (see _format_for_extension) is tried first --
 * one fopen-worth of I/O for the common case -- falling back to the full
 * brute-force scan if the hint is missing, ambiguous, or unconfirmed. With
 * USE_DETECT == 0 an unambiguous hint is trusted with no confirm-detect() at all.
 *
 * input: open file handle to file, and the file's extension (may be NULL)
 * output: matching format, or NULL if no format is detected
 *  * ********/
static SUBTITLE_FORMAT *subtitle_find_format( FILE * file, const char *ext )
{
	const SUBTITLE_FORMAT *hint = _format_for_extension( ext );
	if( hint ) {
		if( !USE_DETECT ) {
			// Unambiguous extension, confirm-detect() disabled -- trust it
			// outright. See USE_DETECT comment above.
			return (SUBTITLE_FORMAT*)hint;
		}
		if( hint->detect ) {
			fseek( file, 0, SEEK_SET );
			if( !hint->detect( file ) ) {
				return (SUBTITLE_FORMAT*)hint;
			}
		}
	}

	SUBTITLE_REG_FORMAT *f = _reg;
	while( f ) {
		if( (const SUBTITLE_FORMAT*)f->format == hint ) {
			// already tried above as the hinted format -- don't pay for
			// its detect() twice
			f = f->next;
			continue;
		}
		if( f->format->detect ) {
			if( !f->format->detect( file ) ) {
				return (SUBTITLE_FORMAT*)f->format;
			}
		}
		f = f->next;
	}
	return NULL;
}

#ifdef DEBUG_MSG
static void _dump_formats( void )
{
	SUBTITLE_REG_FORMAT *f = _reg;
serprintf("subtitle formats:\r\n");
	while( f ) {
serprintf("\t[%s]\r\n", f->format->name );
		f = f->next;
	}
serprintf("\r\n");
}

DECLARE_DEBUG_COMMAND_VOID( "subd", _dump_formats );
#endif

/*************
 * Opens a given filename and if subtitle format is found
 * allocates room for new. Also extract metadata information if such
 * exists for format
 *  * ********************/
static subt_orig *subtitle_parse_file( const char *filename, const char *org_name, const char *ext, const char *lang, int utf8, int delete )
{
	subt_orig *new_title = NULL;

	if ( !filename ) {
		DBG serprintf( "NULL FILENAME!\n" );
		return 0;
	}
	if ( isdigit( *ext ) ) {
		serprintf( "part file: %s\n", filename );
		return 0;
	}

	FILE *file = fopen( filename, "r" );
	if ( !file ) {
		DBG serprintf( "could not open file! %s\n", filename );
		return NULL;
	}

	SUBTITLE_FORMAT *ff = subtitle_find_format( file, ext );
// TEMP DIAGNOSTIC -- remove once the non-SRT detection issue is found
serprintf( "SUBDIAG: file=%s ext=%s -> format=%s\n", filename, ext ? ext : "(null)", ff ? ff->name : "(NONE)" );
	if ( ff != NULL ) {
		new_title = acalloc(1, sizeof( subt_orig ) );
		new_title->filename = amalloc( strlen( filename ) + 1 );
		new_title->org_name = amalloc( strlen( org_name ) + 1 );
		strcpy( new_title->filename, filename );
		strcpy( new_title->org_name, org_name );
		strcpy( new_title->ext,  ext );
		strcpy( new_title->lang, lang );

		new_title->format    = ff;
		new_title->utf8      = utf8;
		new_title->delete    = delete;
		new_title->next      = 0;
		new_title->lan_count = 0;

		// Classify by format identity now, for free, so stream_sub_ext.c can pick the engine backend without running format->parse() first.
		if( !strcmp( ff->name, "IDX/SUB" ) )      new_title->sub_class = SUBT_CLASS_VOBSUB;
		else if( !strcmp( ff->name, "PGS" ) )     new_title->sub_class = SUBT_CLASS_PGS;
		else if( !strcmp( ff->name, "ASS/SSA" ) ) new_title->sub_class = SUBT_CLASS_SSA;
		else                                      new_title->sub_class = SUBT_CLASS_TEXT;

		//Only some titles have metadata
		//try to extract different languages from headers
		if( ff->info ) {
			new_title->title_langs = ff->info( file, &new_title->lan_count, new_title->palette, &new_title->has_palette );
DBG serprintf("lang count %d\r\n", new_title->lan_count );

			// if info is defined and fails, we fail for this title!
			if(!new_title->title_langs){
				DBG serprintf("Error parsing INFO\n");
				afree(new_title->filename);
				afree(new_title->org_name);
				afree(new_title);
				new_title = 0;
			}
		}
	}
	fclose( file );
	return new_title;
}

// Builds an unparsed placeholder uni_sub for `title` (language index `lang_idx` for multi-language files) without calling format->parse() -- all fields the track-menu/engine-selection code needs are set from title->sub_class/metadata, known since detect()/info() time. parse_state stays NOT_QUEUED; the real parse happens later on the background worker, via subtitle_ensure_parsed_async(), eagerly or on first use.
static uni_sub *_stub_uni_sub( subt_orig *title, int lang_idx, int clean_tags )
{
	uni_sub *sub = acalloc( 1, sizeof( uni_sub ) );
	if( !sub ) return NULL;

	sub->spex         = title;
	sub->format       = NULL; // set for real by parse() on gfx-style formats;
	                          // subtitle_get_format_for_sub() resolves through
	                          // spex->format instead, so NULL here is safe.
	sub->is_streaming = (title->format->feed != NULL) ? 1 : 0;
	sub->clean_tags   = clean_tags;
	sub->lang_index   = lang_idx;
	sub->parse_state  = SUBT_PARSE_NOT_QUEUED;
	pthread_mutex_init( &sub->parse_mutex, NULL );

	switch( title->sub_class ) {
	case SUBT_CLASS_VOBSUB:
		sub->vobsub = 1;
		break;
	case SUBT_CLASS_PGS:
		sub->is_pgs = 1;
		break;
	case SUBT_CLASS_SSA:
		sub->is_ssa = 1;
		break;
	default:
		break;
	}

	if( title->has_palette ) {
		sub->has_palette = title->has_palette;
		memcpy( sub->palette, title->palette, sizeof( sub->palette ) );
	}

	if( lang_idx < 0 ) {
		// no languages defined. Use ending of file
		if( title->ext[0] ) {
			const char *code;
			if( title->lang[0] && ( code = map_ISO639_code( title->lang ) ) != title->lang && code[0] != '\0' ) {
				sub->identifier = astrdup( code );
			} else {
				sub->identifier = astrdup( title->ext );
				char *i = sub->identifier;
				while( *i ) { *i = toupper(*i); i++; }
			}
		} else {
			sub->identifier = astrdup( "Unknown" );
		}
DBG serprintf("ext [%s]  lang [%s] -> [%s]\n", title->ext, title->lang, sub->identifier );
	} else {
		sub->identifier = astrdup( title->title_langs[lang_idx]->name );
	}

	return sub;
}

converted_subs *subtitle_get_converted( subtitle_files *sub_files, int clean_tags )
{
	//count number of subtitles (files & languages)
	int count = 0;
	subt_orig *title = sub_files->files;

	while ( title ) {
		if ( !title->format ) {	//unrecognised file
			title = title->next;
			continue;
		}
		if ( title->title_langs ) {
			count += title->lan_count;
		} else {
			count++;
		}
		title = title->next;
	}

	//reserve space for every title file & lang
	converted_subs *sub_array = acalloc(1, sizeof( converted_subs ) );
	if ( !sub_array ) {
DBG serprintf( "subtitles: cannot alloc sub_array\n" );
		return 0;
	}

	sub_array->converted = acalloc( count, sizeof( uni_sub* ) );
	sub_array->cnt = count;

	// NOTE: this loop used to call format->parse() here, immediately, for
	// every track -- the single biggest cost in the whole external-
	// subtitle startup path, paid synchronously before video could start.
	// It now only builds a cheap stub (see _stub_uni_sub above); the real
	// parse is deferred and ASYNC -- subtitle_ensure_parsed_async() kicks
	// it off on a background worker thread (see stream_sub_ext.c), and in
	// the normal playback path every track gets queued eagerly right after
	// this function returns (see _queue_all_tracks() in stream_sub_ext.c),
	// so parsing for every discovered file starts in parallel with video
	// playback starting, not gated behind it.
	//
	// One consequence: a track stub can no longer fail here (parse() isn't
	// called, so there's nothing to fail) -- a genuinely corrupt file that
	// used to be silently dropped from the menu at this point will now
	// appear in the menu and fail later, once its async parse completes,
	// via SUBT_PARSE_FAILED. stream_sub_ext_feed_engine() and
	// stream_sub_ext_get_gfx_data() both poll for this (non-blockingly)
	// and no-op rather than crash -- see stream_sub_ext.c.
	title = sub_files->files;
	count = 0;
	while ( title ) {
		if( !title->format ) {
			title = title->next;
			continue;
		}
		if( !title->title_langs ) {
			sub_array->converted[count] = _stub_uni_sub( title, -1, clean_tags );
			count++;
		} else {
			int i;
			for( i = 0; i < title->lan_count; ++i ) {
				sub_array->converted[count + i] = _stub_uni_sub( title, i, clean_tags );
			}
			count += i;
		}
		if( title->delete ) {
DBG serprintf("sub: delete %s\n", title->filename );
			file_remove( title->filename );
		}
		title = title->next;
	}

	if(count == 0){ //could not retrieve anything
		afree(sub_array->converted);
		afree(sub_array);
		return 0;
	}
DBG {
	serprintf("count %d\n", sub_array->cnt );
	int i;
	for( i = 0; i < sub_array->cnt; i++ ) {
		serprintf("subs for %d [%s]\n", i, sub_array->converted[i]->identifier );
	}
}
	return sub_array;
}

// Companion to subtitle_check_files_incremental(): grows `existing` in place by stubbing only the ->is_new_scan entries in `sub_files`; every already-known uni_sub* stays at its original index untouched, so a live "new file appeared" update never disturbs the currently-playing track. Returns the number of new entries appended (0 if none).
int subtitle_append_new_converted( subtitle_files *sub_files, converted_subs *existing, int clean_tags )
{
	if( !sub_files || !existing ) return 0;

	int add_count = 0;
	subt_orig *title = sub_files->files;
	while( title ) {
		if( title->format && title->is_new_scan ) {
			add_count += title->title_langs ? title->lan_count : 1;
		}
		title = title->next;
	}
	if( add_count == 0 ) return 0;

	uni_sub **grown = arealloc( existing->converted, (existing->cnt + add_count) * sizeof( uni_sub* ) );
	if( !grown ) return 0; // OOM -- existing->converted is untouched by arealloc on failure
	existing->converted = grown;

	int count = existing->cnt;
	title = sub_files->files;
	while( title ) {
		if( title->format && title->is_new_scan ) {
			if( !title->title_langs ) {
				existing->converted[count] = _stub_uni_sub( title, -1, clean_tags );
				count++;
			} else {
				int i;
				for( i = 0; i < title->lan_count; ++i ) {
					existing->converted[count + i] = _stub_uni_sub( title, i, clean_tags );
				}
				count += i;
			}
		}
		title = title->next;
	}
	int actually_added = count - existing->cnt;
	existing->cnt = count;
	return actually_added;
}

// Runs format->parse() for `sub` right now (the actual I/O + cue parsing), called once per track from the background parse-worker thread, which owns the parse_state transitions around this call. parse_mutex is only held for the result merge afterward, never during the I/O itself. Reconstructs the transient title->default_language/language_name setup from sub->lang_index for the multi-language case, since format->parse() reads it off subt_orig; the worker processes its queue one job at a time so two stubs sharing a spex never parse concurrently. Returns 1 on success, 0 on failure.
int subtitle_do_parse( uni_sub *sub )
{
	if( !sub ) return 0;

	subt_orig *title = sub->spex;
	if( !title || !title->format || !title->format->parse ) {
		pthread_mutex_lock( &sub->parse_mutex );
		sub->parse_state = SUBT_PARSE_FAILED;
		pthread_mutex_unlock( &sub->parse_mutex );
		return 0;
	}

	uni_sub *result;
	if( sub->lang_index < 0 ) {
		result = title->format->parse( title, sub->clean_tags );
	} else {
		// Reproduce the exact per-language transient state parse() expects
		// -- set immediately before the call, cleared immediately after,
		// since these live on the shared spex, not on sub itself. See the
		// concurrency note above the function for why this is safe only
		// as long as the worker is single-threaded.
		if( title->language_name ) afree( title->language_name );
		title->language_name    = astrdup( title->title_langs[sub->lang_index]->name );
		title->default_language = astrdup( title->title_langs[sub->lang_index]->id );

		result = title->format->parse( title, sub->clean_tags );

		afree( title->default_language );
		title->default_language = 0;
		afree( title->language_name );
		title->language_name = 0;
	}

	pthread_mutex_lock( &sub->parse_mutex );

	if( !result ) {
DBG serprintf( "subtitle_do_parse: parse failed for %s\n", title->filename );
		sub->parse_state = SUBT_PARSE_FAILED;
		pthread_mutex_unlock( &sub->parse_mutex );
		return 0;
	}

	// Merge the freshly parsed content into the existing stub in place --
	// callers elsewhere already hold the `sub` pointer (converted[i]) and
	// must keep seeing the same identifier/vobsub/is_pgs/etc. we set at
	// stub time; only the actual cue-bearing fields come from the parse.
	sub->first            = result->first;
	sub->last             = result->last;
	sub->raw_data         = result->raw_data;
	sub->raw_size         = result->raw_size;
	sub->frame_multiplier = result->frame_multiplier;
	sub->vobsub_fd        = result->vobsub_fd;
	sub->vobsub_data      = result->vobsub_data;
	sub->vobsub_size      = result->vobsub_size;
	sub->vobsub_pos       = result->vobsub_pos;
	sub->sup_fd           = result->sup_fd;
	if( result->format ) sub->format = result->format; // gfx-style formats set this
	if( result->has_palette ) {
		sub->has_palette = result->has_palette;
		memcpy( sub->palette, result->palette, sizeof( sub->palette ) );
	}
	// result->identifier / result->spex / result->is_streaming / result->is_ssa
	// / result->is_pgs / result->vobsub are intentionally NOT copied over --
	// the stub's own values (set in _stub_uni_sub from cheaply-known
	// metadata) are authoritative and callers may already have read them.

	sub->parse_state = SUBT_PARSE_DONE;
	pthread_mutex_unlock( &sub->parse_mutex );

	afree( result ); // shallow container only; ownership of its pointer
	                  // fields transferred above, not duplicated
	return 1;
}

static subtitle_parse_enqueue_fn _parse_enqueue_fn = NULL;

void subtitle_set_parse_enqueue_fn( subtitle_parse_enqueue_fn fn )
{
	_parse_enqueue_fn = fn;
}

// Non-blocking poll of sub's parse_state; if still NOT_QUEUED, atomically flips it to QUEUED and hands it to the registered enqueue callback -- so asking "is it ready?" is what triggers parsing. Falls back to a direct synchronous subtitle_do_parse() if no enqueue fn is registered.
int subtitle_ensure_parsed_async( uni_sub *sub )
{
	if( !sub ) return SUBT_PARSE_FAILED;

	int should_enqueue = 0;

	pthread_mutex_lock( &sub->parse_mutex );
	int state = sub->parse_state;
	if( state == SUBT_PARSE_NOT_QUEUED ) {
		// Flip + "I'm responsible for enqueueing" happen atomically under the lock, so concurrent callers on the same fresh track never both enqueue.
		sub->parse_state = SUBT_PARSE_QUEUED;
		state = SUBT_PARSE_QUEUED;
		should_enqueue = 1;
	}
	pthread_mutex_unlock( &sub->parse_mutex );

	if( should_enqueue ) {
		if( _parse_enqueue_fn ) {
			_parse_enqueue_fn( sub );
		} else {
			// No worker registered -- parse synchronously here instead of leaving the track stuck at QUEUED forever.
			subtitle_do_parse( sub );
			pthread_mutex_lock( &sub->parse_mutex );
			state = sub->parse_state;
			pthread_mutex_unlock( &sub->parse_mutex );
		}
	}

	return state;
}

static subtitle_parse_priority_fn _parse_priority_fn = NULL;

void subtitle_set_parse_priority_fn( subtitle_parse_priority_fn fn )
{
	_parse_priority_fn = fn;
}

// Like subtitle_ensure_parsed_async(), but also promotes an already-QUEUED track to the front of the worker's queue (a track that's actively selected shouldn't wait behind passive warm-up parses); IN_PROGRESS/DONE/FAILED are left alone. Never falls back to the regular enqueue fn for the already-QUEUED case, to avoid a duplicate entry.
int subtitle_ensure_parsed_async_priority( uni_sub *sub )
{
	if( !sub ) return SUBT_PARSE_FAILED;

	int should_enqueue = 0;
	int should_promote = 0;

	pthread_mutex_lock( &sub->parse_mutex );
	int state = sub->parse_state;
	if( state == SUBT_PARSE_NOT_QUEUED ) {
		sub->parse_state = SUBT_PARSE_QUEUED;
		state = SUBT_PARSE_QUEUED;
		should_enqueue = 1;
	} else if( state == SUBT_PARSE_QUEUED ) {
		should_promote = 1;
	}
	pthread_mutex_unlock( &sub->parse_mutex );

	if( should_enqueue ) {
		if( _parse_priority_fn ) {
			_parse_priority_fn( sub );
		} else if( _parse_enqueue_fn ) {
			// No priority-aware worker registered -- regular tail-insert
			// enqueue rather than dropping the request; correctness is
			// unaffected, only the fairness win is lost.
			_parse_enqueue_fn( sub );
		} else {
			subtitle_do_parse( sub );
			pthread_mutex_lock( &sub->parse_mutex );
			state = sub->parse_state;
			pthread_mutex_unlock( &sub->parse_mutex );
		}
	} else if( should_promote && _parse_priority_fn ) {
		_parse_priority_fn( sub );
	}

	return state;
}

int subtitle_get_gfx( uni_sub *subs, uint32_t pos, uint8_t *data, int *size )
{
	// subs->format is only populated by format->parse() (see get_IDX/SUP's
	// sub->format = spex->format assignment) -- with lazy+async parse it's
	// NULL until the track has actually finished parsing. Bitmap tracks
	// (VobSub/PGS) are exactly the ones with get_gfx, so this is the one
	// place that needs to trigger/poll the parse for them; text tracks
	// never call subtitle_get_gfx at all.
	//
	// Non-blocking: if parsing hasn't finished yet (NOT_QUEUED, QUEUED, or
	// IN_PROGRESS), this just enqueues it (if needed) and returns "nothing
	// to show yet" -- the caller (stream_sub_ext_get_gfx_data(), called
	// once per video frame) must treat that as "no subtitle bitmap for
	// this frame" rather than blocking, exactly like a genuinely empty
	// Display Set. The picture keeps playing; the bitmap appears once
	// parsing actually completes on the background worker.
	int state = subtitle_ensure_parsed_async( subs );
	if( state != SUBT_PARSE_DONE || !subs->format ) {
		return 1;
	}
	if( subs->format->get_gfx ) {
		return subs->format->get_gfx( subs, pos, data, size );
	}
	return 1;
}

char *subtitle_get_description( subt_orig * title )
{
	return ( astrdup( title->format->name ) );
}

// Single-allocation, single-pass rewrite (was one-alloc-per-call plus an O(n^2) copy loop). clean_tags==0 (the only value any caller passes, since Libass now handles tags natively) is a straight strdup. clean_tags==1 is kept for completeness and fixes two original bugs: it couldn't handle an embedded '\n', and its <br> stripping condition was inverted (<br> survived as literal text instead of becoming a line break). Returns a freshly allocated string; caller afree()s it; never NULL for non-NULL input.
char *subtitle_clean_formatter( char *line, int clean_tags )
{
	int len = (int)strlen( line );

	if( !clean_tags ) {
		// The only path any current caller takes. Single alloc, single
		// memcpy -- no per-character loop, no repeated strlen().
		char *out = amalloc( len + 1 );
		memcpy( out, line, len );
		out[len] = '\0';
		return out;
	}

	// clean_tags==1: single left-to-right pass. Output can only be <=
	// input length (every branch either copies a char through 1:1 or
	// drops/replaces a whole tag with nothing/one '\n'), so len+1 bytes
	// is always sufficient -- no worst-case-expansion sizing needed here,
	// unlike the ASS-tag-emitting translators elsewhere in this codebase.
	char *out = amalloc( len + 1 );
	int i = 0, o = 0;

	while( i < len ) {
		if( line[i] == '<' ) {
			// <br> / <br/> / <br /> / </br> -> one real line break.
			// Case-insensitive to match how "<br" tags are commonly
			// authored (mirrors this codebase's other tag translators,
			// e.g. subtitle_vtt.c's strncmpNC use for the same reason).
			int is_close = (i + 1 < len && line[i+1] == '/');
			int tag_start = is_close ? i + 2 : i + 1;
			int is_br = (tag_start + 2 <= len) &&
			            (line[tag_start] == 'b' || line[tag_start] == 'B') &&
			            (line[tag_start+1] == 'r' || line[tag_start+1] == 'R');

			char *gt = memchr( line + i, '>', len - i );
			if( !gt ) {
				// Unterminated '<' -- copy the rest verbatim rather than
				// silently dropping it (matches this codebase's other
				// tag-translators' "unterminated tag" handling, e.g.
				// srt_text_to_ass() in sub_format_srt.c).
				while( i < len ) out[o++] = line[i++];
				break;
			}

			if( is_br ) {
				out[o++] = '\n';
			}
			// Any other <...> tag (is_br==0): stripped entirely, nothing
			// emitted -- matches the original's intent of turning
			// non-<br> tags into nothing (the original's bug was doing
			// the opposite of this for specifically <br>, not the
			// stripping of everything else, which this preserves).
			i = (int)(gt - line) + 1;
			continue;
		}

		if( line[i] == '&' && i + 4 <= len && !strncmp( line + i, "&lt;", 4 ) ) {
			// &lt;...&gt; span -- same "ignore" treatment the original
			// gave it (stripped entirely, no line break substitution;
			// unlike literal <br>, an HTML-entity-escaped tag was never
			// going to BE <br> specifically, so there's no equivalent
			// special case here).
			char *gt = strstr( line + i, "&gt;" );
			if( !gt ) {
				out[o++] = line[i++];
				continue;
			}
			i = (int)(gt - line) + 4;
			continue;
		}

		out[o++] = line[i++];
	}
	out[o] = '\0';
	return out;
}

void subtitle_clean_error(uni_sub *subs)
{
	if(!subs){
		return;
	}
	if(subs->first){
		sub_line* tmp = subs->first;
		sub_line* line = tmp;
		while(line){
			if(line->top){
				afree(line->top);
			}
			if(line->bottom){
				afree(line->bottom);
			}
			line = line->next;
			afree(tmp);
			tmp = line;
		}
		if(subs->identifier){
			afree(subs->identifier);
		}
	}
}

#define BOM_LE 	 0xFFFE
#define BOM_BE 	 0xFEFF
#define BOM_UTF8 0xBFBBEF

#define BUF_MAX 1024
#define TMP_FILE "/tmp/subXXXXXX"

#define CHECK_MAX 256

static int convert_to_utf8( char **filename, int *delete )
{
	int ret;
	int tmp_fd = -1;
	FILE *tmp_file = NULL;
	FILE *file = NULL;

	file = fopen( *filename, "r");
	if (!file) {
		return 0;
	}
	*delete = 0;

	// check for BOM
	unsigned short bom = 0;
	fread( &bom, 2, 1, file );
	if( bom != BOM_LE && bom != BOM_BE ) {
		unsigned char msb;
		fread( &msb, 1, 1, file );
		if( bom == (BOM_UTF8 & 0xFFFF) && msb == (BOM_UTF8 >> 16) ) {
DBG serprintf("sub: UTF-8 by BOM!\n");
			ret = 1;
			goto end;
		}

		// no UTF8 BOM, we need to look at the whole file:
		void *ctx = I18N_check_encoding_init();
		unsigned char buf[BUF_MAX];
		int len;
		int count = 0;
		while( (len = fread(buf, 1, BUF_MAX, file)) && count ++ < CHECK_MAX ) {
			I18N_check_encoding_update( ctx, buf, len);
		}
		int utf8;
		I18N_check_encoding_finish( ctx, &utf8 );
		if( utf8 ) {
DBG serprintf("sub: UTF-8 by check!\n");
			ret = 1;
			goto end;
		}
DBG serprintf("sub: CODEPAGE!\n");
		ret = 0;
		goto end;
	}
DBG serprintf("sub: UTF-16!\n");

#ifdef CONFIG_ANDROID
	char template[256];
	snprintf(template, 255, "/data/data/%s/files/sub", device_config_get_android_pkg_name());
	tmp_fd = open(template, O_CREAT|O_RDWR, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP);
	chmod(template, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP);
#else
	char template[] = TMP_FILE;

	tmp_fd = mkstemp(template);
#endif

	if (tmp_fd < 0) {
		serprintf("failed to create temporary file: %s\n", template );
		ret = 0;
		goto end;
	}
DBG serprintf("sub: tmpfile: %s\n", template );

	tmp_file = fdopen(tmp_fd, "w+");
	if (!tmp_file) {
		serprintf("failed to open temporary file for writing (%s:%i)\n", __FILE__, __LINE__);
		ret = 0;
		goto end;
	}

	unsigned short utf16[BUF_MAX];
	unsigned char  utf8[BUF_MAX * 3];
	while( 1 ) {
		int utf16_len = fread( utf16, 2, BUF_MAX, file );
		if( !utf16_len ) {
			break;
		}
		if( bom == BOM_LE ) {
			swap16_buf( (unsigned char*)utf16, utf16_len * 2 );
		}
		int utf8_len  = unicode_utf16_to_utf8( utf8, utf16, utf16_len );
//serprintf("in %3d out %3d  %s\n", utf16_len, utf8_len, utf8 );
		fwrite( utf8, 1, utf8_len, tmp_file );
	}

	afree( *filename );
	*filename = astrdup( template );
	*delete   = 1;
	ret = 1;
end:
	if( file )
		fclose( file );
	if( tmp_file )
		fclose( tmp_file );
	if( tmp_fd > -1 )
		close( tmp_fd );
	return ret;
}

static char **subtitle_get_files( char **sub_files, const char *full_path, const char *file_name, int *count )
{
	if ( !full_path || !file_name ) {
		DBG serprintf( "subtitle_get_files: path or filename error\n" );
		return NULL;
	}

	int sub_n = *count;

	// trickster. Compare only name not the ending in name.end
	char *path = astrdup( full_path );
	char *name = astrdup( file_name );
	char *tmp  = strrchr( name, '.' );
	if ( tmp ) {
		*( tmp /*+ 1*/ ) = '\0';	// allow for substrings by terminating before the "."
	}
	// dig the names from current directory
	// use the name of parameter to find out subtitle files. Only ending should
	// be different
	DIR *dp = dir_open( path );
	if ( !dp ) {		//path could point directly to file. Remove everything after last /
		tmp = strrchr( path, '/' );
		if ( tmp ) {
			*( tmp + 1 ) = '\0';
			dp = dir_open( path );
		}
	}
	if ( dp ) {
		DIRENT *ep = dir_read( dp );
		// search the subtitle with following rules
		// 1. everything until the '.' must be similar
		// 2. length of filename must be equal to videofile
		// 3. subtitle file can not be the same file as videofile
		while ( ep ) {
			if ( !strncmpNC( name, ep->d_name, strlen( name ) ) ) {
				if ( strcmpNC( ep->d_name, file_name ) ) {
					if ( sub_files ) {
						sub_files = arealloc( sub_files, ( sub_n + 1 ) * sizeof( *sub_files ) );
						if ( !sub_files ) {
							dir_close( dp );
							goto ERROREXIT;
						}
					} else {
						sub_files = amalloc( sizeof( sub_files ) );
						if ( !sub_files ) {
							dir_close( dp );
							goto ERROREXIT;
						}
					}
					sub_files[sub_n] = amalloc( strlen( ep->d_name ) + strlen( path ) + 1 );
					strcpy( sub_files[sub_n], path );
					strcat( sub_files[sub_n], ep->d_name );
DBG serprintf("%d: %s\r\n", sub_n, sub_files[sub_n] );

					++sub_n;
				}
			}
			//next file
			ep = dir_read( dp );
		}

		dir_close( dp );
	} else {
		DBG serprintf( "subtitle_get_files:Error opening directory:%s\n", path );
		//goto ERROREXIT;
	}

	afree( name );
	afree( path );
	*count = sub_n;
	return sub_files;

ERROREXIT:
	if ( sub_files ) {
		int i;
		for ( i = 0; i < sub_n; ++i ) {
			afree( sub_files[i] );
		}
		afree( sub_files );
	}
	afree( path );
	afree( name );
	if ( dp )
		dir_close( dp );
	return NULL;
}

// Finds a subt_orig in `*prev_head` whose ->org_name matches `path` (stable across re-scans, unlike ->filename); if found, unlinks and returns it for reuse with no fopen/detect/info(). Returns NULL if genuinely new.
static subt_orig *_find_and_unlink_prev( subt_orig **prev_head, const char *path )
{
	subt_orig *cur  = *prev_head;
	subt_orig *prev = NULL;
	while( cur ) {
		if( cur->org_name && !strcmp( cur->org_name, path ) ) {
			if( prev ) prev->next = cur->next;
			else       *prev_head = cur->next;
			cur->next = NULL;
			return cur;
		}
		prev = cur;
		cur  = cur->next;
	}
	return NULL;
}

// Shared implementation behind subtitle_check_files() and subtitle_check_files_incremental(): if `prev_head` is given, any entry whose path still matches on disk is reused as-is (no fopen/detect/info()); anything unmatched (file disappeared) is freed via free_subs().
static void free_subs( subt_orig * fd );
static subtitle_files *_check_files_common( const char **path, const char *filename, subt_orig *prev_head )
{
	int count = 0;
	char **sub_files = NULL;

	while( *path ) {
DBG serprintf("checking path: %s\n", *path );
		sub_files = subtitle_get_files( sub_files, *path, filename, &count );
		path++;
	}

	subt_orig *subtitle_file     = NULL;
	subtitle_files *usable_files = NULL;

	int i;
	for ( i = 0; i < count; ++i ) {
DBG serprintf("sub: check: %s\r\n", sub_files[i] );

		subtitle_file = prev_head ? _find_and_unlink_prev( &prev_head, sub_files[i] ) : NULL;

		if( subtitle_file ) {
DBG serprintf("sub: reused (already known): %s\r\n", sub_files[i] );
			// Nothing to do -- subtitle_file already has format/sub_class/
			// title_langs/etc. from the previous scan, and its own
			// ->filename (possibly a converted tempfile) is still valid on
			// disk since nothing touched it.
			subtitle_file->is_new_scan = 0;
		} else {
			// get ext and lang here as the filename could be mangled by utf16 conversion!
			char ext[4]  = { 0 };
			char lang[4] = { 0 };
			strnZcpy( ext,  get_extension( sub_files[i] ), 3 );
			strnZcpy( lang, get_extension( cut_extension( sub_files[i] )), 3 );

			int delete = 0;
			char *org_name = astrdup( sub_files[i] );
			int utf8 = convert_to_utf8( &(sub_files[i]), &delete );
			subtitle_file = subtitle_parse_file( sub_files[i], org_name, ext, lang, utf8, delete );
			afree( org_name );
			if( subtitle_file ) {
				subtitle_file->is_new_scan = 1;
			} else if( delete ) {
DBG serprintf("sub: delete %s\n", sub_files[i] );
				file_remove( sub_files[i] );
			}
		}

		if ( subtitle_file ) {
			if ( !usable_files ) {
				usable_files = acalloc(1, sizeof( subtitle_files ) );
				usable_files->set_lan = 0;
				usable_files->files = subtitle_file;
				usable_files->files->next = 0;
				usable_files->count = 0;
			} else {
				subtitle_file->next = usable_files->files;
				usable_files->files = subtitle_file;
			}
			usable_files->count++;
		}
		afree( sub_files[i] );
	}
	afree( sub_files );

	// Anything left in prev_head no longer exists on disk (or no longer
	// matches) -- free it now rather than leaking it.
	if( prev_head ) {
		free_subs( prev_head );
	}

	return usable_files;
}

/******************
 * Browses through given PATH and searches for files
 * that have same name as filename, but different ending
 * send those files to be detected and groups detected files
 * into one array in subtitle_files*
 *  * ***************/
subtitle_files *subtitle_check_files( const char **path, const char *filename )
{
	return _check_files_common( path, filename, NULL );
}

// Same result as subtitle_check_files(), but reuses every subt_orig from `prev` still present on disk instead of re-running fopen+detect(+info()) for it -- cheap enough to call again as soon as a new file might have appeared, without a full stream restart. `prev` is consumed: the caller must not free it itself, just use the return value.
subtitle_files *subtitle_check_files_incremental( const char **path, const char *filename, subtitle_files *prev )
{
	subt_orig *prev_head = NULL;
	if( prev ) {
		prev_head = prev->files;
		if( prev->set_lan ) afree( prev->set_lan );
		afree( prev ); // just the container -- its ->files list lives on in prev_head
	}
	return _check_files_common( path, filename, prev_head );
}

// cleans the coding style struct
static void free_coding( sub_coding_style ** code, int count )
{
	int i;
	if ( !code )
		return;
	for ( i = 0; i < count; ++i ) {
		if ( !code[i] ) {
			return;
		}
		if ( code[i]->id )
			afree( code[i]->id );
		if ( code[i]->name )
			afree( code[i]->name );
		if ( code[i]->lang )
			afree( code[i]->lang );
		if ( code[i]->type )
			afree( code[i]->type );
		afree( code[i] );
	}
	afree( code );
}

// cleans the per-subtitle-file structs
static void free_subs( subt_orig * fd )
{
	if ( !fd )
		return;
	subt_orig *tmp = 0;
	while ( fd ) {
		afree( fd->filename );
		afree( fd->org_name );
		if ( fd->title_langs )
			free_coding( fd->title_langs, fd->lan_count );
		afree( fd->default_language );
		tmp = fd;
		fd = fd->next;
		afree( tmp );
	}
}

// cleans the struct that contains all the files that contain subtitles
// for played videofile
void subtitle_free_files( subtitle_files *files )
{
	if ( !files )
		return;
	if ( files->set_lan )
		afree( files->set_lan );

	free_subs( files->files );
	afree( files );
}

// cleans the linked list of converted subtitles
static void free_subline( sub_line * sub )
{
	sub_line *tmp;
	if ( !sub )
		return;
	while ( sub ) {
		if ( sub->top )
			afree( sub->top );
		if ( sub->bottom )
			afree( sub->bottom );
		tmp = sub;
		sub = sub->next;
		afree( tmp );
	}
}

SUBTITLE_FORMAT *subtitle_get_format_for_sub( uni_sub *subs )
{
    // uni_sub->format is never populated for SRT/VTT (they register .parse
    // as NULL -- there used to be a parse_SRT()/parse_VTT() stub, removed;
    // see subtitle_srt.c/subtitle_vtt.c), so reading it here would always
    // yield NULL and silently force every external SRT/VTT track onto the
    // fallback sub_line list-walk in stream_sub_ext_feed_engine() -- a list
    // that's always empty for them since there's no parse() to populate it.
    // Resolve through spex instead:
    // subt_orig->format is unconditionally set for every track in
    // subtitle_parse_file(), so this reliably returns the streaming feed()
    // backend and lets cues reach libass as they're parsed -- unordered
    // and overlapping cues included, since feed() no longer filters them
    // through the old "only keep it if end time increases" list insertion.
    if (!subs || !subs->spex) return NULL;
    return (SUBTITLE_FORMAT *)subs->spex->format;
}

void subtitle_free_converted( converted_subs *subs )
{
	int i = 0;
	for ( i = 0; i < subs->cnt; ++i ) {
		if( !subs->converted[i] ) continue;
		// call a format specific cleanup if needed
		if( subs->converted[i]->format && subs->converted[i]->format->close ) {
			subs->converted[i]->format->close( subs->converted[i] );
		}
		free_subline( subs->converted[i]->first );
		afree(subs->converted[i]->identifier);
		if (subs->converted[i]->raw_data) {
			afree(subs->converted[i]->raw_data);
		}
		// Matches the pthread_mutex_init() in _stub_uni_sub(). Safe to call
		// even if a parse never ran (glibc pthread_mutex_destroy on an
		// initialized-but-never-locked mutex is well-defined) and safe
		// even right after stream_sub_ext_close()'s worker join, since
		// that join guarantees no thread can still be holding this mutex.
		pthread_mutex_destroy( &subs->converted[i]->parse_mutex );
		afree( subs->converted[i] );
	}
	afree( subs->converted );
	afree( subs );
}

#endif	// CONFIG_SUBTITLES
