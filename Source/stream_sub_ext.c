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
#include <limits.h>

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
		if (time == INT_MAX) return INT_MAX; // unknown last DVD cue end
		UINT64 scaled = time * (UINT64)ratio_n / (UINT64)ratio_d;
		return scaled > INT_MAX ? INT_MAX : (int)scaled;
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
			if ( strcmp(files->org_name, new_files->org_name) ) {
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
	for( i = 0; i < p->subs->cnt; i++ ) {
		if( !p->subs->converted[i] ) {
			continue;
		}

		if( s->av.subs_max >= SUB_TRACK_MAX )
			break;
				
		SUB_PROPERTIES *sub = s->av.sub + s->av.subs_max;
		memset(sub, 0, sizeof(*sub));
	
		sub->format         = p->subs->converted[i]->vobsub ? SUB_FORMAT_DVD_GFX : SUB_FORMAT_EXT;
		sub->gfx            = p->subs->converted[i]->vobsub ? 1 : 0;
		sub->ext            = 1;
		sub->stream         = i;
		sub->valid          = 1;
		if (sub->gfx) {
			uni_sub *source = p->subs->converted[i];
			char *extra = (char *)sub->extraData;
			int len = 0;
			if (source->canvas_width > 0 && source->canvas_height > 0)
				len = snprintf(extra, sizeof(sub->extraData), "size: %dx%d\n",
					source->canvas_width, source->canvas_height);
			if (source->has_palette) {
				len += snprintf(extra + len, sizeof(sub->extraData) - len, "palette: ");
				for (int c = 0; c < 16; ++c)
					len += snprintf(extra + len, sizeof(sub->extraData) - len,
						"%06x%s", source->palette[c] & 0xffffff, c == 15 ? "\n" : ", ");
			}
			sub->extraDataSize = len;
		}
		s->av.subs_max ++;
			
		strnZcpy( sub->name, p->subs->converted[i]->identifier, AV_NAME_LEN );
		if (p->subs->converted[i]->source_path)
			strnZcpy(sub->path, p->subs->converted[i]->source_path, MAX_NAME_LEN);
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

// Called with the subtitle worker idle on every successful seek.
void stream_sub_ext_reset(STREAM *s)
{
	SUB_PRIV *p = s->subtitle_priv;
	if (p) {
		p->sub = p->out = NULL;
		p->sub_time = -1;
	}
}

// *************************
//
// stream_sub_ext_get_subtitle_data
//
// *************************
int stream_sub_ext_get_subtitle_data( STREAM *s, VIDEO_FRAME **pframe, int time )
{
	int rst_time = TS_TO_RST_TIME(time, int);
	SUB_PRIV *p = s->subtitle_priv;
	if (!p || !p->subs || s->subtitle->stream < 0 ||
	    s->subtitle->stream >= p->subs->cnt ||
	    !p->subs->converted[s->subtitle->stream] ||
	    !p->subs->converted[s->subtitle->stream]->first ||
	    !p->subs->converted[s->subtitle->stream]->last)
		return 1;
	if( s->subtitle->stream != p->stream ) {
		p->stream = s->subtitle->stream;
		p->sub = NULL;
		p->out = NULL;
DBG serprintf("sub: stream now %d\r\n", p->stream );
	}
	
	// over the end, give up
	if( rst_time > scale_time( s, p->subs->converted[p->stream]->last->end ) ) {
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
	if( !p->out || scale_time( s, p->out->end ) < rst_time ) {
		int start = scale_time( s, p->sub->start );
		int end   = scale_time( s, p->sub->end );
		
		// drop all subs in the past
		while( p->sub ) {
			if( end > rst_time ) {
				break;
			}
DBG3 serprintf("sub: skip [%8d] %8d -> %8d [%s][%s]\r\n", time, start, end, p->sub->top, p->sub->bottom );
			p->sub = p->sub->next;
			if( !p->sub ) {
				break;
			}
			start = scale_time( s, p->sub->start );
			end = scale_time( s, p->sub->end );
		}
		if( !p->sub ) {
			return 1;
		}		
	
		// check if this one is due?
		if( start > rst_time ) {
DBG3 serprintf("sub: wait [%8d] %8d -> %8d [%s][%s]\r\n", time, start, end, p->sub->top, p->sub->bottom );
			return 1;
		}	
		
		// output this one:
		p->out = p->sub;
DBG2 serprintf("sub: out  [%8d] %8d -> %8d TOP[%s] BOT[%s]\r\n", time, start, end, p->out->top, p->out->bottom );
		
		VIDEO_FRAME *frame = *pframe;
		if( s->subtitle->gfx ) {
			frame->valid = frame->size;
			if (subtitle_get_gfx(p->subs->converted[p->stream], p->out->pos,
			    frame->data[0], &frame->valid)) {
				frame->valid = 0;
				p->sub = p->sub->next;
				return 1;
			}
		} else {
			if (!frame->data[0] || frame->size <= 0) return 1;
			int   max = frame->size - 1;
			char *src = p->out->top;
			char *dst = frame->data[0];
			// check if p->out->top is not NULL
			if( !src ) {
				src = "";
			}
			while( *src && max > 0 ) {
				max--;
				*dst++ = *src++;
			}
			if( p->out->bottom && max > 2 ) {
				*dst++ = '\\';
				*dst++ = 'n';
				max -= 2;
				char *src = p->out->bottom;
				while( *src && max > 0 ) {
					max--;
					*dst++ = *src++;
				}
			}
			*dst = '\0';
		}
			
		frame->time      = RST_TO_TS_TIME(start, int);
		frame->duration  = RST_TO_TS_DELTA(end - start, int);

		p->sub = p->sub->next;
		return 0;
	}

	return 1;
}
#endif	// CONFIG_SUBTITLES
#endif  // CONFIG_STREAM
