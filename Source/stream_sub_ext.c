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
	sub_line *sub;
	sub_line *out;
	int sub_time;
	int prev_max;
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
		if(p->subs->converted[i]->frame_multiplier ){
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
	if( s->sub_url[0] )
		afree( s->sub_url[0] );
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

DBGS serprintf("stream_sub_ext_check: [%s]\r\n", s->sub_url[0] );

	subtitle_files *files = get_subtitle_files( s );
	if (!files)
		return 1;

	if( !s->subtitle_priv ) {
		s->subtitle_priv = amalloc( sizeof( SUB_PRIV ) );
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
		if( s->av.subs_max >= SUB_STREAM_MAX )
			break;
				
		SUB_PROPERTIES *sub = s->av.sub + s->av.subs_max;
	
		sub->format         = p->subs->converted[i]->vobsub ? SUB_FORMAT_DVD_GFX : SUB_FORMAT_EXT;
		sub->gfx            = p->subs->converted[i]->vobsub ? 1 : 0;
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
	p->sub    = NULL;
	p->out    = NULL;
	p->sub_time = -1;
	
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
// stream_sub_ext_get_subtitle_data
//
// *************************
int stream_sub_ext_get_subtitle_data( STREAM *s, VIDEO_FRAME **pframe, int time )
{
	SUB_PRIV *p = s->subtitle_priv;
	if( s->subtitle->stream != p->stream ) {
		p->stream = s->subtitle->stream;
		p->sub = NULL;
		p->out = NULL;
DBG serprintf("sub: stream now %d\r\n", p->stream );
	}
	
	// over the end, give up
	if( time > scale_time( s, p->subs->converted[p->stream]->last->end ) ) {
		return 1;
	} else if( p->sub_time == -1 || time < p->sub_time ) {
		p->sub = NULL;
		p->out = NULL;
	}

	p->sub_time = time;
	
	if( !p->sub ) {
DBG serprintf("sub: rewind at %d\r\n", time);
		// start from 1st sub
		p->sub = p->subs->converted[p->stream]->first;
		if( !p->sub ) {
DBG serprintf("sub: no 1st\r\n");
			return 1;
		}
	}
	
	if( !p->sub ) {
		return 1;
	}
	
	// no current or current is done?
	if( !p->out || scale_time( s, p->out->end ) < time ) {
		int start = scale_time( s, p->sub->start );
		int end   = scale_time( s, p->sub->end );
		
		// drop all subs in the past
		while( p->sub ) {
			if( end > time ) {
				break;
			}
DBG3 serprintf("sub: skip [%8d] %8d -> %8d [%s][%s]\r\n", time, start, end, p->sub->top, p->sub->bottom );
			p->sub = p->sub->next;
			start = scale_time( s, p->sub->start );
			end   = scale_time( s, p->sub->end );
		}
		if( !p->sub ) {
			return 1;
		}		
	
		// check if this one is due?
		if( start > time ) {
DBG3 serprintf("sub: wait [%8d] %8d -> %8d [%s][%s]\r\n", time, start, end, p->sub->top, p->sub->bottom );
			return 1;
		}	
		
		// output this one:
		p->out = p->sub;
DBG2 serprintf("sub: out  [%8d] %8d -> %8d TOP[%s] BOT[%s]\r\n", time, start, end, p->out->top, p->out->bottom );
		
		VIDEO_FRAME *frame = *pframe;
		if( s->subtitle->gfx ) {
			frame->valid = frame->size;
			subtitle_get_gfx( p->subs->converted[p->stream], p->out->pos, frame->data[0], &frame->valid );
		} else {
			int   max = frame->size - 1;
			char *src = p->out->top;
			char *dst = frame->data[0];
			while( *src && max-- ) {
				*dst++ = *src++;
			}
			if( p->out->bottom && max > 2 ) {
				*dst++ = '\\';
				*dst++ = 'n';
				char *src = p->out->bottom;
				while( *src && max-- ) {
					*dst++ = *src++;
				}
			}
			*dst = '\0';
		}
			
		frame->time      = start;
		frame->duration  = end - start; 

		p->sub = p->sub->next;
		return 0;
	}

	return 1;
}
#endif	// CONFIG_SUBTITLES
#endif  // CONFIG_STREAM
