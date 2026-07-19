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
#include "types.h"
#include "stream.h"
#include "stream_subtitle.h"
#include "astdlib.h"
#include "debug.h"
#include "subtitle_format.h"
#include "util.h"
#include "file.h"
#include "browse.h"
#include "sub_engine.h"

#include <ctype.h>		// for isspace
#include <unistd.h>
#include <string.h>

#define DBGS if(Debug[DBG_STREAM])
#define DBG  if(Debug[DBG_SUB])
#define DBG2 if(Debug[DBG_SUB] > 1)
#define DBG3 if(Debug[DBG_SUB] > 2)

#ifdef CONFIG_STREAM
#ifdef CONFIG_SUBTITLES

typedef struct SUB_PRIV {
	subtitle_files *files;
	converted_subs *subs;
	int stream;
	int prev_max;
	// engine_fmt: SUB_FMT_SSA for ASS/SSA tracks, SUB_FMT_SRT for all other
	// text tracks, SUB_FMT_GFX for bitmap tracks. Set per-track in
	// stream_sub_ext_check() and read by stream_subtitle.c at track open.
	int engine_fmt[SUB_TRACK_MAX];
} SUB_PRIV;

static int _get_time_from_frame(VIDEO_PROPERTIES *video, int frame)
{
	if( !video->valid) {
		return -2;
	}
	return (UINT32)( 1000ull * (UINT64)frame * (UINT64)video->scale / (UINT64)video->rate); 
}

// adjusts subtitle timing by multiplying the start and end time of title with framerate
static void _adjust_timing( STREAM *s, converted_subs *subs )
{
	SUB_PRIV *p = s->subtitle_priv;

	int i;
	for(i = 0; i < p->subs->cnt; ++i){
		if(p->subs->converted[i] && p->subs->converted[i]->frame_multiplier ){
			sub_line *line = p->subs->converted[i]->first;
			while(line){
				line->start = _get_time_from_frame(s->video, line->start);
				line->end   = _get_time_from_frame(s->video, line->end  );
				line = line->next;
			}
		}
	}
}

static int scale_time( STREAM *s, int time )
{
	int ratio_n = s->subtitle_ratio_n;
	int ratio_d = s->subtitle_ratio_d;

	if( ratio_n && ratio_d ) {
		return time * (UINT64)ratio_n / (UINT64)ratio_d;
	} 

	return time;
}

static subtitle_files *get_subtitle_files( STREAM *s )
{
	const char *name =  s->src.name[0] == '\0' ? cut_path( s->src.url ) : s->src.name;

	if (!name)
		return NULL;
	// make the current path the 1st entry in the url list
	if( s->sub_url[0] ) {
		afree( s->sub_url[0] );
		s->sub_url[0] = NULL;
	}
	s->sub_url[0] = astrdup( s->src.url );
	return subtitle_check_files( (const char**)s->sub_url, name );
}


// *************************
//
// stream_sub_ext_has_new
//
// *************************
int stream_sub_ext_has_new( STREAM *s )
{
	SUB_PRIV *p = s->subtitle_priv;
	subtitle_files *new_subtitle_files = get_subtitle_files( s );
	int ret = 0;

	if ((!new_subtitle_files && p) || (!p && new_subtitle_files)) {
		ret = 1;
		goto end;
	}
	if (p && new_subtitle_files) {
		subtitle_files *subtitle_files = p->files;
		if (subtitle_files->count != new_subtitle_files->count) {
			ret = 1;
			goto end;
		}
		struct subt_orig_t *files = subtitle_files->files;
		struct subt_orig_t *new_files = new_subtitle_files->files;
		while (files && new_files) {
			if ( strcmp(files->filename, new_files->filename) ) {
				ret = 1;
				goto end;
			}
			files = files->next;
			new_files = new_files->next;
		}
		goto end;
	}

end:
	if( new_subtitle_files )
		subtitle_free_files( new_subtitle_files );

	return ret;
}

// *************************
//
// stream_sub_ext_check
//
// *************************
int stream_sub_ext_check( STREAM *s )
{
	if( !s )
		return 1;

DBGS serprintf("stream_sub_ext_check: [%s]\r\n", s->sub_url[0] ? s->sub_url[0] : "(null)" );

	subtitle_files *files = get_subtitle_files( s );
	if (!files)
		return 1;

	if( !s->subtitle_priv ) {
		s->subtitle_priv = amalloc( sizeof( SUB_PRIV ) );
		if( !s->subtitle_priv ) {
			goto NULL_SUBTITLES;
		}
	}
	SUB_PRIV *p = s->subtitle_priv;
	memset( p, 0, sizeof( SUB_PRIV ) );
	p->prev_max = s->av.subs_max;
	p->files = files;

	// now every file that contains valid subtitles (according to name of file) 
	// has been found. Convert every file to general format. 
	if ( !p->files ) {
		DBG serprintf( "Failed to find subtitles\n" );
		goto NULL_SUBTITLES;
	}
	
	// read all the files now rather than during videoplayback
	p->subs = subtitle_get_converted( p->files, s->flags & STREAM_SUBTITLES_CLEAN_TAGS );
	if(!p->subs){
		goto NULL_SUBTITLES;
	}

	//convert the subtitle time format if necessary
	_adjust_timing( s, p->subs );

	// add them to the sub props:
	int i;
	struct subt_orig_t *sub_files = p->files->files;
	for( i = 0; i < p->subs->cnt; i++ ) {
		if( !p->subs->converted[i] ) {
			if( sub_files ) {
				sub_files = sub_files->next;
			}
			continue;
		}

		if( s->av.subs_max >= SUB_TRACK_MAX )
			break;
				
		SUB_PROPERTIES *sub = s->av.sub + s->av.subs_max;
	
		// Determine format and engine target for this track
		if ( p->subs->converted[i]->vobsub ) {
			sub->format        = SUB_FORMAT_DVD_GFX;
			sub->gfx           = 1;
			p->engine_fmt[s->av.subs_max] = SUB_FMT_GFX;
		} else if ( p->subs->converted[i]->is_ssa ) {
			sub->format        = SUB_FORMAT_SSA;
			sub->gfx           = 0;
			p->engine_fmt[s->av.subs_max] = SUB_FMT_SSA;
		} else {
			sub->format        = SUB_FORMAT_EXT;
			sub->gfx           = 0;
			p->engine_fmt[s->av.subs_max] = SUB_FMT_SRT;
		}
		sub->ext            = 1;
		sub->stream         = i;
		sub->valid          = 1;
		if(p->subs->converted[i]->has_palette) {
DBGS serprintf("has palette!\n");
			sub->extraDataSize = sizeof( p->subs->converted[i]->palette );
			memcpy( sub->extraData, p->subs->converted[i]->palette, sub->extraDataSize );
		}
		s->av.subs_max ++;

		strnZcpy( sub->name, p->subs->converted[i]->identifier, AV_NAME_LEN );
		if (sub_files) {
			if (sub_files->filename) {
				strnZcpy( sub->path, sub_files->filename, MAX_NAME_LEN );
			}
			sub_files = sub_files->next;
		}
	}

	p->stream = -1;
	
	return 0;

NULL_SUBTITLES:
	stream_sub_ext_close( s );
	return 1;
}

// *************************
//
// stream_sub_ext_close
//
// *************************
void stream_sub_ext_close( STREAM *s )
{
DBGS serprintf("stream_sub_ext_close\r\n" );
	SUB_PRIV *p = s->subtitle_priv;
	if( p ) {
		int i;
		for (i = p->prev_max; i < s->av.subs_max; ++i) {
			SUB_PROPERTIES *sub = s->av.sub + i;
			memset(sub, 0, sizeof(SUB_PROPERTIES));
		}
		s->av.subs_max = p->prev_max;
		if( p->files )
			subtitle_free_files( p->files );
		if( p->subs )
			subtitle_free_converted( p->subs );
		afree( s->subtitle_priv );
		s->subtitle_priv = NULL;
	}
}

// *************************
//
// stream_sub_ext_feed_engine
//
// Called ONCE at track open from _get_next_ext_sub() in stream_subtitle.c.
// Feeds the entire parsed subtitle track into the C engine in one pass so
// Libass owns the full timeline — identical to how internal embedded tracks
// work. No cursor, no frame-by-frame polling, no ordering constraints.
//
// Returns: engine format (SUB_FMT_SSA / SUB_FMT_SRT / SUB_FMT_GFX)
//          so stream_subtitle.c knows which backend was opened.
//          Returns -1 on error.
// *************************
int stream_sub_ext_feed_engine( STREAM *s )
{
	SUB_PRIV *p = s->subtitle_priv;
	if( !p || !p->subs ) return -1;

	int stream = s->subtitle->stream;
	if( stream < 0 || stream >= p->subs->cnt ) return -1;

	uni_sub *subs = p->subs->converted[stream];
	if( !subs ) return -1;

	int engine_fmt = p->engine_fmt[s->av.subs];

DBG serprintf("sub_ext_feed_engine: stream %d  engine_fmt %d  is_ssa %d\r\n",
              stream, engine_fmt, subs->is_ssa);

	if( engine_fmt == SUB_FMT_SSA && subs->is_ssa ) {
		// ASS/SSA: feed raw file buffer directly — Libass handles everything
		if( subs->raw_data && subs->raw_size > 0 && s->sub_engine ) {
			sub_engine_feed_raw( (SUB_ENGINE*)s->sub_engine,
			                     (const uint8_t*)subs->raw_data,
			                     subs->raw_size );
		}

	} else if( engine_fmt == SUB_FMT_SRT ) {
		// SRT/VTT/SMI/SUB/MPL2: walk the cue list and bulk-feed every node
		sub_line *node = subs->first;
		while( node ) {
			char merged[LINE_LEN * 2 + 4];
			if( node->top && node->bottom ) {
				snprintf( merged, sizeof(merged), "%s\\N%s", node->top, node->bottom );
			} else {
				snprintf( merged, sizeof(merged), "%s", node->top ? node->top : "" );
			}
			if( merged[0] && s->sub_engine ) {
				int duration = scale_time( s, node->end ) - scale_time( s, node->start );
				sub_engine_feed( (SUB_ENGINE*)s->sub_engine,
				                 (uint8_t*)merged, strlen(merged),
				                 scale_time( s, node->start ),
				                 duration );
			}
			node = node->next;
		}
	}
	// SUB_FMT_GFX (VobSub bitmap) is handled by the existing
	// _is_ffdec_bitmap path in stream_subtitle.c — not bulk-fed here.

	return engine_fmt;
}

// stream_sub_ext_get_gfx_data — retained for the external VobSub bitmap
// path only. Called from stream_subtitle.c _get_next_ext_sub when
// _is_ffdec_bitmap() is true.
int stream_sub_ext_get_gfx_data( STREAM *s, VIDEO_FRAME **pframe, int time )
{
	int rst_time = TS_TO_RST_TIME(time, int);
	SUB_PRIV *p = s->subtitle_priv;
	if( !p ) return 1;

	int stream = s->subtitle->stream;
	if( stream != p->stream ) {
		p->stream = stream;
DBG serprintf("sub_ext_gfx: stream now %d\r\n", p->stream);
	}

	uni_sub *subs = p->subs->converted[stream];
	if( !subs || !subs->first ) return 1;

	// Walk to find the cue that covers rst_time
	sub_line *node = subs->first;
	while( node ) {
		int start = scale_time( s, node->start );
		int end   = scale_time( s, node->end );
		if( end > rst_time && start <= rst_time ) {
			VIDEO_FRAME *frame = *pframe;
			frame->valid = frame->size;
			subtitle_get_gfx( subs, node->pos, frame->data[0], &frame->valid );
			frame->time     = RST_TO_TS_TIME(start, int);
			frame->duration = RST_TO_TS_DELTA(end - start, int);
			return 0;
		}
		node = node->next;
	}
	return 1;
}
// *************************
//
// stream_sub_ext_get_engine_fmt
//
// Returns the engine format (SUB_FMT_SSA / SUB_FMT_SRT / SUB_FMT_GFX)
// for the currently active subtitle track. Called from stream_subtitle.c
// _get_next_ext_sub() at track open time.
// *************************
int stream_sub_ext_get_engine_fmt( STREAM *s )
{
	SUB_PRIV *p = s->subtitle_priv;
	if( !p ) return -1;
	int track_idx = s->av.subs;
	if( track_idx < 0 || track_idx >= SUB_TRACK_MAX ) return -1;
	return p->engine_fmt[track_idx];
}

#endif	// CONFIG_SUBTITLES
#endif  // CONFIG_STREAM
