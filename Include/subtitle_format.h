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

#ifndef __SUBTITLE_FORMAT_H__
#define __SUBTITLE_FORMAT_H__

#define LINE_LEN	600
#define MS_CURSOR_BEGIN '\r'
#define NEW_LINE_CH	'\n'

#include <stdio.h>
#include <stdint.h>

typedef void (*sub_cue_cb)(void *ctx, const char *text, int start_ms, int end_ms);

// Polled periodically by feed()'s single-pass loop (feed_SRT/feed_VTT); non-zero stops it early (NULL = never stop). Policy lives in the caller; feed() doesn't save/resume state, so the caller just retries later.
typedef int (*sub_feed_poll_cb)( void *poll_ctx );

//if the subtitle contains title for multiple languages
//it should fill this field in info_XXX function

//This struct is acquired when determining which files may
//contain valid subtitles for videofile
//and is passed to parse_XXX function

typedef struct sub_coding_style
{
	char *id; //internal describer for language 'ENCC' 'FINCC' etc.
	char *name; //name of language 'English', 'Finnish'
   //this is used to select the language
	char *lang; //codepage
	char *type; //may or may not be used
} sub_coding_style;

struct SUBTITLE_FORMAT;

// Track classification, known cheaply at detect() time (before parse()); used for the track menu and engine backend choice.
enum {
	SUBT_CLASS_TEXT = 0,	// plain-text cue formats: SRT/VTT/SMI/SUB/MPL2
	SUBT_CLASS_SSA,		// ASS/SSA raw-buffer passthrough
	SUBT_CLASS_VOBSUB,	// external VobSub IDX/SUB (bitmap)
	SUBT_CLASS_PGS,		// external PGS .sup (bitmap)
};

typedef struct subt_orig_t
{
	struct SUBTITLE_FORMAT *format;
	char *filename;
	char *org_name;
 	char  ext[4];
 	char lang[4];
        int utf8;
        int delete;

        unsigned int lan_count;
        char *language_name;
	char *default_language; //This must contain the internal describer
                                //   'ENCC' 'FINCC' etc or zero if not
				// supported by subtitle format
	sub_coding_style **title_langs;

	int has_palette;
	uint32_t palette[16];

	int sub_class;		// one of SUBT_CLASS_* above, set at detect time
	int is_new_scan;	// set by subtitle_check_files_incremental() on
				// entries it just created this call (not reused
				// from `prev`) -- lets a caller doing a live,
				// non-destructive track-list append (see
				// subtitle_append_new_converted() and
				// stream_sub_ext_update() in stream_sub_ext.c)
				// find exactly the new files without
				// re-diffing paths itself.

	struct subt_orig_t *next;
} subt_orig;

typedef struct subtitle_files_t
{
	char *set_lan;
	int count;
	subt_orig *files;
} subtitle_files;

typedef struct sub_line_t
{
	char *top;
	char *bottom;
	int start;
	int end;
	uint32_t pos;
	struct sub_line_t *next;
	struct sub_line_t *prev;
} sub_line;

// Async parse state machine, strictly forward: NOT_QUEUED -> QUEUED -> IN_PROGRESS -> DONE|FAILED; all transitions/reads of the parse-result fields below must hold ->parse_mutex.
enum {
	SUBT_PARSE_NOT_QUEUED = 0,
	SUBT_PARSE_QUEUED,
	SUBT_PARSE_IN_PROGRESS,
	SUBT_PARSE_DONE,
	SUBT_PARSE_FAILED,
};

#include <pthread.h>

typedef struct uni_sub_t
{
	int frame_multiplier;
	int vobsub;
	int vobsub_fd;
	unsigned char *vobsub_data;
	int vobsub_size;
	int vobsub_pos;
	int has_palette;
	uint32_t palette[16];
	struct SUBTITLE_FORMAT *format;
        char *identifier;
	sub_line *first;
	sub_line *last;
	int   is_ssa;
	char *raw_data;
	int   raw_size;
	int   is_streaming;
	subt_orig *spex;
	int   is_pgs;
	FILE *sup_fd;

	// Lazy async parse: subtitle_get_converted() just stubs the track (NOT_QUEUED); the background parse worker (stream_sub_ext.c) runs format->parse() later, polled via subtitle_ensure_parsed_async().
	int             parse_state;	// one of SUBT_PARSE_* above
	pthread_mutex_t parse_mutex;	// guards parse_state + every parse-result field listed in the enum comment above
	int   clean_tags;	// saved from subtitle_get_converted() for the deferred format->parse() call in the worker
	int   lang_index;	// title_langs[i] index for multi-language files (SMI/IDX), or -1 for single-language

	// Opaque owner back-pointer, set once by stream_sub_ext.c right after this
	// uni_sub is produced by subtitle_get_converted()/subtitle_append_new_converted()
	// (before it's ever handed to subtitle_ensure_parsed_async()). subtitle_formats.c
	// itself never reads or writes this field -- it exists purely so the
	// registered subtitle_parse_enqueue_fn/subtitle_parse_priority_fn callbacks
	// (which have no STREAM* parameter, by design, so this file stays
	// decoupled from STREAM/SUB_PRIV) can recover the ACTUAL stream this job
	// belongs to instead of guessing via a process-wide "most recently
	// active stream" pointer -- the guess is wrong the moment two STREAMs
	// are ever alive at once. NULL until stream_sub_ext.c stamps it.
	void *owner_ctx;
} uni_sub;

typedef struct converted_subs_t
{
        uni_sub **converted;
        int cnt;
} converted_subs;

char *subtitle_clean_formatter( char *line, int clean_tags );
void subtitle_clean_error(uni_sub* subs);

typedef struct SUBTITLE_FORMAT {
	const char 		*name;
	int 			(*detect)( FILE * file );
	sub_coding_style**	(*info)( FILE * file, int *cnt, uint32_t *palette, int *has_palette );
	uni_sub*		(*parse)( subt_orig *subs, int clean_tags );
	int			(*get_gfx)( uni_sub *subs, uint32_t pos, uint8_t *data, int *size );
	int			(*close)( uni_sub *subs );
	void		(*feed)( subt_orig *subs, sub_cue_cb cb, void *ctx, sub_feed_poll_cb poll, void *poll_ctx );
} SUBTITLE_FORMAT;

struct SUBTITLE_REG_FORMAT;

typedef struct SUBTITLE_REG_FORMAT {
	const SUBTITLE_FORMAT 	*format;
	struct SUBTITLE_REG_FORMAT *next;
} SUBTITLE_REG_FORMAT;

int subtitle_register_format( SUBTITLE_REG_FORMAT *reg );

#define SUBTITLE_REGISTER_FORMAT( format ) \
	static SUBTITLE_REG_FORMAT _reg_##etype##format = { \
		&format,\
		NULL\
	}; \
	static void _fn_reg_##etype##format( void ) __attribute__((constructor));\
	static void _fn_reg_##etype##format( void )\
	{ \
		subtitle_register_format( &_reg_##etype##format ); \
	}

SUBTITLE_FORMAT *subtitle_get_format_for_sub( uni_sub *subs );
subtitle_files *subtitle_check_files( const char **path_list, const char *filename );
void            subtitle_free_files( subtitle_files *files );
converted_subs *subtitle_get_converted( subtitle_files *sub_files, int clean_tags );
int  		subtitle_get_gfx( uni_sub *subs, uint32_t pos, uint8_t *data, int *size );
void            subtitle_free_converted( converted_subs *subs );
char           *subtitle_get_description( subt_orig *title );

// Runs format->parse() for `sub` synchronously right now; parse_mutex is only held for the final result merge/DONE-FAILED transition, not during the I/O itself. Returns 1 on success, 0 on failure.
int             subtitle_do_parse( uni_sub *sub );

// Registers the fn subtitle_ensure_parsed_async() uses to hand a track to the background parse worker; if never registered, falls back to a direct synchronous subtitle_do_parse() call.
typedef void (*subtitle_parse_enqueue_fn)( uni_sub *sub );
void            subtitle_set_parse_enqueue_fn( subtitle_parse_enqueue_fn fn );

// Non-blocking poll of sub's parse_state; if still NOT_QUEUED, this also enqueues it onto the background parse worker as a side effect. Safe from any thread.
int             subtitle_ensure_parsed_async( uni_sub *sub );

// Priority enqueue fn (patch 7): lets an actively-selected track jump ahead of the worker's queue; optional, and a no-op promotion-wise if never registered.
typedef void (*subtitle_parse_priority_fn)( uni_sub *sub );
void subtitle_set_parse_priority_fn( subtitle_parse_priority_fn fn );

// Like subtitle_ensure_parsed_async(), but for an actively-selected track: head-inserts if unqueued, or promotes to front if already queued (never double-enqueues).
int             subtitle_ensure_parsed_async_priority( uni_sub *sub );

// Incremental subtitle_check_files(): reuses matching entries from `prev` (consumed, do not reuse after the call), only scans genuinely new files; fresh entries get ->is_new_scan set.
subtitle_files *subtitle_check_files_incremental( const char **path_list, const char *filename, subtitle_files *prev );

// Grows `existing` in place by stubbing only the ->is_new_scan entries in `sub_files`; prior uni_sub pointers/indices stay untouched. Returns count appended (0 if none/OOM).
int             subtitle_append_new_converted( subtitle_files *sub_files, converted_subs *existing, int clean_tags );

#endif // __SUBTITLE_FORMAT_H__
