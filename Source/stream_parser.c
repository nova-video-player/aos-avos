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
#include "stream_parser.h"
#include "debug.h"
#include "astdlib.h"
#include "atime.h"
#include "cbe.h"
#include "util.h"
#include "h264.h"
#include "wmv.h"
#include "pts_reorder.h"

#include <string.h>
#include <limits.h>

#ifdef CONFIG_STREAM

#define DBGC(x) if(Debug[DBG_CHU] & (x))

#define DBGS  if(Debug[DBG_STREAM])
#define DBGS2 if(Debug[DBG_STREAM] == 2)
#define DBGP  if(Debug[DBG_PARSER])
#define DBGP2 if(Debug[DBG_PARSER] > 1)
#define DBGP3 if(Debug[DBG_PARSER] > 2)
#define DBGD  if(Debug[DBG_DRM])
#define DBGCV if(Debug[DBG_CV])

#define CHUNK_MAX_AUD	2048
#define CHUNK_MAX_VID	4096
#define CHUNK_MAX_SUB	4096

static int _stream_parser_can_add_audio( STREAM *s );
static int _stream_parser_can_add_video( STREAM *s );
static int _stream_parser_can_add_subtitle( STREAM *s );

STREAM_IO *stream_io_timeshift_new( STREAM *s );

int ignore_chunks = 0;

// ***********************************************************
//
//	stream_parser_prebuffer
//
// ***********************************************************
int stream_parser_prebuffer( STREAM *s, STREAM_BUFFER *buffer, int min_data )
{
	if( !s || !buffer )
		return 1;
DBGS serprintf("stream_parser_prebuffer: %d\r\n", min_data );
	int last_progress = 0;
	int do_progress   = 0;
	if( (s->flags & STREAM_FILE_NONLOCAL ) && s->do_progress && s->progress ) {
		do_progress = 1;
		s->progress( s, 128, last_progress );
	}
	int head;
	while (    (head = stream_buffer_get_head( buffer )) < min_data
		&& !stream_buffer_end( buffer )
		&& stream_buffer_get_free( buffer ) > (256 * 1024)
	 ) {
DBGS {
serprintf(".");
	static int last;
	if( atime() - last > 1000 ) {
		last = atime();
		serprintf("\r\n[%2d%%]", head * 100 / min_data );
	}
}
		if( do_progress ) {
			int progress = head * 128 / min_data;
			if ( progress != last_progress ) {
				last_progress = progress;
				s->progress( s, 128, progress );
			}
		}

		// check for abort
		if( stream_abort( s ) ) {
serprintf("\r\nuser abort in parser prebuffer!\n");
			return 1;
		}

		msec_sleep( 100 );
	}
	if( do_progress ) {
		s->progress( s, 128, 128 );
	}
DBGS serprintf("\r\n");
	return 0;
}

// ***********************************************************
//
//	_alloc_chunk_store
//
// ***********************************************************
static int _alloc_chunk_store( STREAM_CHUNK_STORE *scs, int max )
{
	scs->max = max;
		
	if( !(scs->c = amalloc( scs->max * sizeof( STREAM_CHUNK ) ) ) ) {
		return 1;
	}
	return 0;
}

// ***********************************************************
//
//	_free_chunk_store
//
// ***********************************************************
static void _free_chunk_store( STREAM_CHUNK_STORE *scs )
{
	if( scs->c ) {
		afree( scs->c );
		scs->c = NULL;
	}
}

// ***********************************************************
//
//	stream_parser_open
//
// ***********************************************************
int stream_parser_open( STREAM *s, int buffer_size, int flags )
{
DBGS serprintf("stream_parser_open: %d  flags %04x\r\n", buffer_size, flags );
	
	pthread_mutex_init( &s->parser_buffer_mutex, NULL );
	
	// declare parser open here so that we can free memory on close
	s->parser_open = 1;

	if( !s->parser_mindata_size ) {
		s->parser_mindata_size = VIDEO_MINDATA_SIZE;
	}
	s->parser_flags = flags;
	
	// new STREAM_IO
	s->io = stream_get_new_io( &s->src );
	if( !s->io ) {
		// not an error, some input methods might want to write into the buffer directly
serprintf("warning no io!\r\n");
	}

	// new STREAM_BUFFER
	if( s->io && stream_io_can_abort( s->io ) ) {
		s->buffer = new_stream_buffer_raw_non_blocked();
	} else {
		s->buffer = new_stream_buffer_raw();
	} 
	
	if( !s->buffer ) {
serprintf("no mem for buffer!\r\n");
		return 1;
	}
	
	int buffer_flags = 0;
	
	if( flags & STREAM_PARSER_FILE_NONLOCAL ) {
		buffer_flags |= STREAM_BUFFER_NO_SLEEP;
	}
	if( s->drm_ctx.setup ) {
		stream_io_set_setup( s->io, (STREAM_IO_SETUP)s->drm_ctx.setup, s->drm_ctx.handle );
	}
	if( s->io_setup ) {
		stream_io_set_setup( s->io, s->io_setup, s->io_setup_ctx );
	}
	if( s->io_meta ) {
		stream_io_set_meta( s->io, s->io_meta, s->io_meta_ctx );
	}

	buffer_flags |= s->buffer_flags;
	if( s->buffer_flags & STREAM_BUFFER_MMAP_FILE ) {
		stream_buffer_set_mmap_file( s->buffer, (const char*)s->buffer_opaque );
	}

	int err;
	if ( ( err = stream_buffer_open( s->buffer, s, s->io, buffer_size, VIDEO_OVERLAP_SIZE, 0, s->size, buffer_flags, "VID" ) ) ) {
		stream_set_error( s, err );
		return 1;
	}

	if( flags & STREAM_PARSER_TIMESHIFT ){
DBGS serprintf("stream_parser_open: timeshift is not supported!!!\r\n");
		return 1;
	} else {
	}

	// alloc chunks
	if( _alloc_chunk_store( &s->aud, CHUNK_MAX_AUD ) ) {
serprintf("no mem for aud chunks!\r\n");
		return 1;
	}
	if( _alloc_chunk_store( &s->vid, CHUNK_MAX_AUD ) ) {
serprintf("no mem for vid chunks!\r\n");
		return 1;
	}
	if( _alloc_chunk_store( &s->sub, CHUNK_MAX_AUD ) ) {
serprintf("no mem for sub chunks!\r\n");
		return 1;
	}

	// alloc a reorder context
	s->ro_ctx = pts_ro_alloc( CHUNK_MAX_AUD );
	
	stream_parser_clear_chunks( s );
	
	if( !( flags & STREAM_PARSER_NO_PREBUFFER ) ) {
		// read mindata + 50% into buffer so that we can parse file header
		if( stream_parser_prebuffer( s, s->buffer, s->parser_mindata_size * 3 / 2 ) ) {
serprintf("user abort in parser open!\n");
			stream_parser_close( s );
			return 1;
		}
	}

	return 0;
}

// ***********************************************************
//
//	stream_parser_close
//
// ***********************************************************
int stream_parser_close( STREAM *s )
{
	int ret = 0;
DBGS serprintf("stream_parser_close\r\n" );

	if( !s->parser_open ) {
serprintf("sp not open!\r\n");
		return 1;
	}

	_free_chunk_store( &s->aud );
	_free_chunk_store( &s->vid );
	_free_chunk_store( &s->sub );
	
	if( s->ro_ctx ) {
		pts_ro_free(s->ro_ctx);
		s->ro_ctx = NULL;
	}
	
	s->parser_open = 0;

	if( s->buffer2 ) {
		pthread_mutex_lock( &s->parser_buffer_mutex );
		
		ret = s->buffer2->close( s->buffer2 );

		s->buffer2->delete( s->buffer2 );
		s->buffer2 = NULL;
		
		pthread_mutex_unlock( &s->parser_buffer_mutex );
	}
	if( s->io2 )
		s->io2->delete( s->io2 );

	if( s->buffer ) {
		pthread_mutex_lock( &s->parser_buffer_mutex );
		
		ret = s->buffer->close( s->buffer );

		s->buffer->delete( s->buffer );
		s->buffer = NULL;
		
		pthread_mutex_unlock( &s->parser_buffer_mutex );
	}
	if( s->io )
		s->io->delete( s->io );
	
	return ret;
}

// ***********************************************************
//
//	stream_parser_pause
//
// ***********************************************************
int stream_parser_pause( STREAM *s, int paused )
{
	if( !s )
		return 1;
DBGS serprintf("stream_parser_pause( %d )\r\n", paused );

	s->parser_paused = paused;

	return 0;
}

// ***********************************************************
//
//	stream_parser_peek_n_audio_chunk
//
// ***********************************************************
STREAM_CHUNK *stream_parser_peek_n_audio_chunk(STREAM *s, int n, UCHAR **data )
{
	if( s && s->parser && s->parser->peek_n_audio_chunk ) {
		return s->parser->peek_n_audio_chunk(s, n, data);
	}
	int aud_read = s->aud.read + n;
	if (aud_read >= s->aud.max)
		aud_read -= s->aud.max;

	STREAM_CHUNK *sc = s->aud.c + aud_read;
	if( data ) 
		*data = s->buffer->data + sc->buf;
	return s->aud.c + aud_read;
}

// ***********************************************************
//
//	stream_parser_peek_n_video_chunk
//
// ***********************************************************
static STREAM_CHUNK *stream_parser_peek_n_video_chunk( STREAM *s, STREAM_CHUNK *c, int n )
{
	int vid_read = s->vid.read + n;
	if (vid_read >= s->vid.max)
		vid_read -= s->vid.max;
	if( c ) 
		memcpy( c, s->vid.c + vid_read, sizeof( STREAM_CHUNK ) );
	return s->vid.c + vid_read;
}

// ***********************************************************
//
//	stream_parser_clear_rate
//
// ***********************************************************
int stream_parser_clear_rate( STREAM *s )
{
	s->vtime_parsed  = 0;
	s->acurrent_rate = 0;
	s->vtime_parsed  = 0;
	s->vcurrent_rate = 0;

	return 0;
} 

// ***********************************************************
//
//	stream_parser_preparse
//
// ***********************************************************
int stream_parser_preparse(STREAM *s)
{
	if( !s ) {
		return 1;
	}
	
	int i;
	for( i = 0; i < 100; i++ ) {
		s->parser->parse( s );
	}
	return 0;
}

// ***********************************************************
//
//	stream_parser_guess_msPerFrame
//
// ***********************************************************
int stream_parser_guess_msPerFrame(STREAM *s)
{
	if( !s || !s->buffer ) {
		goto ErrorExit;
	}
DBGP serprintf("stream_parser_guess_msPerFrame\r\n");
	int last  = -1;
	int count = 0;
	int min   = INT_MAX;
	int gcd   = 0;
	
	while( 1 ) {
		STREAM_CHUNK sc = { 0 };

		if( stream_abort(s) ) {
DBGP serprintf("abort\r\n");
			break;
		}
		if( !stream_parser_can_add_chunks( s ) ) {
DBGS serprintf("cant add\r\n");
			break;
		}
		if ( !stream_parser_can_parse( s->buffer, NULL) ) {
DBGP serprintf("cant parse\r\n");
			break;
		}
		if ( s->parser->parse_chunk( s, &sc ) ) {
DBGP serprintf("cant parse chunk\r\n");
			break;
		}
		
		if( s->video_parse_end || s->audio_parse_end ) {
DBGP serprintf("parse end\r\n");
			break;
		}

		// add this chunk
		stream_parser_add_chunk( s, &sc );

		if ( sc.type == TYPE_VID ) {
			int diff = abs(sc.time - last);
		    	if( last != -1 && count > 2 && diff != 0 ) {
				gcd = av__gcd(gcd, diff);
				if( diff < min )
					min = diff;
			}
DBGP serprintf("%3d  time %5d  diff %5d  gcd %3d  min %3d \r\n", count, sc.time, diff, gcd, min);
			last = sc.time;
			if( count++ == 20 )
				break;
		}
	}

	int ms;
	if( gcd > 1 ) {
		ms = gcd;
	} else if( min != INT_MAX ) {
		ms = min;
	} else {
ErrorExit:
		// assume 25 fps
		ms = 40;
	}
DBGP serprintf("stream_parser_guess_msPerFrame: %d\r\n", ms);
	return ms;
}

// ***********************************************************
//
//	stream_parser_calc_rate
//
// ***********************************************************
int stream_parser_calc_rate( STREAM *s )
{
	int head_num = stream_parser_video_chunk_num( s );
DBGS2 serprintf("vid %3d  ", head_num );
	if ( !s->video->valid || !head_num ) {
		s->vtime_parsed  = 0;
		s->vcurrent_rate = 0;
	} else {
		static int vid_last;

		int   tail_num = 1;
		int   tail_time = s->video_time;
		INT64 tail_pos  = s->video_pos;
		if( tail_time == -1 ) {
			while( tail_num < head_num ) {
				STREAM_CHUNK *tail = stream_parser_peek_n_video_chunk( s, NULL, tail_num );
				if ( tail->valid && tail->time != -1 ) {
					tail_time = tail->time;
					tail_pos  = tail->pos;
DBGS2 serprintf("tail %3d/%5d  ", tail_num, tail_time );
					break;
				}
				tail_num ++;
			}
			
		} else {
DBGS2 serprintf("tail ---/%5d  ", tail_time );
		}
		while( head_num > tail_num ) {
			STREAM_CHUNK *head = stream_parser_peek_n_video_chunk( s, NULL, head_num );

			if ( head->valid && head->time != -1 && head->time != vid_last  ) {
				INT64 diff = head->pos - tail_pos;
//serprintf("(V: %d/%lld  %d/%lld)", head->time, head->pos, tail_time, tail_pos );
				if ( head->time > tail_time)
					s->vtime_parsed = head->time - tail_time;
				else
					s->vtime_parsed = 0;

				if( s->vtime_parsed && diff > 0 ) {
					s->vcurrent_rate = (UINT64)diff * (UINT64)1000 / (UINT64)s->vtime_parsed;
				} else {
					s->vcurrent_rate = 0;
				}
				vid_last = head->time;
DBGS2 serprintf("head %3d/%5d  ", head_num, head->time );
				break;
			} 
			head_num --;
		}
	} 
	
	head_num = stream_parser_audio_chunk_num( s );
DBGS2 serprintf("aud %3d  ", head_num );
	if ( !s->audio->valid || !head_num ) {
		s->atime_parsed  = 0;
		s->acurrent_rate = 0;
	} else {
		static int aud_last;
		
		int   tail_num = 1;
		int   tail_time = s->audio_time;
		INT64 tail_pos  = s->audio_pos;
		if( tail_time == -1 ) {
			while( tail_num < head_num ) {
				STREAM_CHUNK *tail = stream_parser_peek_n_audio_chunk( s, tail_num, NULL );
				if ( tail->valid && tail->time != -1 ) {
					tail_time = tail->time;
					tail_pos  = tail->pos;
DBGS2 serprintf("tail %3d/%5d  ", tail_num, tail_time );
					break;
				}
				tail_num ++;
			}
			
		} else {
DBGS2 serprintf("tail ---/%5d  ", tail_time );
		}
		while( head_num > tail_num ) {
			STREAM_CHUNK *head = stream_parser_peek_n_audio_chunk( s, head_num, NULL );

			if ( head->valid && head->time != -1 && head->time != aud_last ) {
				INT64 diff = head->pos - tail_pos;
//serprintf("(A: %d/%lld  %d/%lld)", head->time, head->pos, tail_time, tail_pos );
				if( head->time > tail_time)
					s->atime_parsed = head->time - tail_time;
				else
					s->atime_parsed = 0;

				if( s->atime_parsed && diff > 0 ) {
					s->acurrent_rate = (UINT64)diff * (UINT64)1000 / (UINT64)s->atime_parsed;
				} else {
					s->acurrent_rate = 0;
				}
DBGS2 serprintf("head %3d/%5d  ", head_num, head->time );
				aud_last = head->time;
				break;
			} 	
			head_num --;
		}
	} 

	if ( s->audio->valid && s->video->valid ) {
		s->time_parsed  = MIN( s->vtime_parsed,  s->atime_parsed  ); 
		s->current_rate = MAX( s->vcurrent_rate, s->acurrent_rate ); 
	} else if ( s->audio->valid ) {
		s->time_parsed  = s->atime_parsed;
		s->current_rate = s->acurrent_rate;
	} else { 
		s->time_parsed  = s->vtime_parsed;
		s->current_rate = s->vcurrent_rate;
	}
DBGS2 serprintf("time %5d  rate %8d \r\n", s->time_parsed, s->current_rate );
	return 0;
}

// ***********************************************************
//
//	stream_parser_add_chunk
//
// ***********************************************************
int stream_parser_add_chunk( STREAM *s, STREAM_CHUNK *sc )
{
	// this a valid chunk ?
	if ( sc->type != -1 && !ignore_chunks ) {
		// we have found a chunk
		if ( sc->type == TYPE_VID && s->video->valid ) {
			stream_parser_put_video_chunk( s, sc );
		} else if ( sc->type == TYPE_AUD && s->audio->valid ) {
			stream_parser_put_audio_chunk( s, sc );
		} else if ( sc->type == TYPE_SUBT && s->subtitle->valid ) {
			stream_parser_put_subtitle_chunk( s, sc );
		}
		return 0;
	}	
	return 1;
}

// ***********************************************************
//
//	stream_parser_can_parse
//
// ***********************************************************
int stream_parser_can_parse( STREAM_BUFFER *buffer, int *end )
{
	if( end )
		*end = 0;
	if( stream_buffer_get_head( buffer ) < buffer->s->parser_mindata_size ) {
		if( !stream_buffer_end( buffer ) ) {	
DBGP3 serprintf("(wfd  rd %8d  sc %8d  wr %8d  v %8lld/%8d  v %8lld/%8d  s %8lld/%8d)\r\n", 
			buffer->buf_read, buffer->buf_scan, buffer->buf_write, 
			buffer->vid_last_pos, buffer->vid_last_buf, 
			buffer->aud_last_pos, buffer->aud_last_buf, 
			buffer->sub_last_pos, buffer->sub_last_buf );
			
			msec_sleep( 10 );
			return 0;
		} else if (buffer->flags & STREAM_BUFFER_NO_WRAP) {
serprintf("MUST END! %d\r\n", buffer->buf_end );
			if( end )
				*end = 1;
		}
	}	

	return 1;
}

int stream_parser_max = 5;

// ***********************************************************
//
//	stream_parser_parse
//
// ***********************************************************
int stream_parser_parse( STREAM *s )
{
	int i;
	int err = 0;
	
	// load chunk aggressively, try more often ...
	for( i = 0; i < stream_parser_max; i ++ ) {
		int end = 0;

		// can we store more chunks ???
		if( !stream_parser_can_add_chunks( s ) || s->video_parse_end ) {
			break;
		}
		// if we have minimum amount of data in the buffer or we are the end of the file
		if ( stream_parser_can_parse( s->buffer, &end ) ) {
			if( end ) {
				err = 1;
			} else {
				STREAM_CHUNK sc = { 0 };
				err = s->parser->parse_chunk( s, &sc );
				stream_parser_add_chunk( s, &sc );
			}
			if( err ) {
				break;
			} 
		} else {
			if( end ) {
				err = 1;
			}
			break;
		}
	}
	if( err ) {
		// error or end of movie...
		s->video_parse_end = 1;
		s->audio_parse_end = 1;
DBGP serprintf("parse_end\r\n");
	}
		
	return 0;
}

// ***********************************************************
//
//	stream_parser_parse_not_interleaved
//
// ***********************************************************
int stream_parser_parse_not_interleaved( STREAM *s, PARSER_PARSE_CHUNK parse_video, PARSER_PARSE_CHUNK parse_audio  )
{
	// no interleaved, parse audio and video separately
	int i;
	int err = 0;
	
	// load chunk aggressively, try more often ...
	for( i = 0; i < 5; i ++ ) {
		// can we store more chunks ???
		if( !_stream_parser_can_add_video( s ) || s->video_parse_end )
			break;
	
		int end = 0;
		// if we have minimum data in buffer or we are the end of the file
		if ( stream_parser_can_parse( s->buffer, &end ) ) {
			STREAM_CHUNK sc = { 0 };
			err = parse_video( s, &sc );
			stream_parser_add_chunk( s, &sc );

			if( err ) {
				break;
			} 
		} else {
			if ( end ) {
				err = 1;
			}
			break;
		}
	}
	if( err ) {
		// error or end of movie...
		s->video_parse_end = 1;
serprintf("video_parse_end\r\n");
	}
	
	// load chunk aggressively, try more often ...
	if( s->audio->valid ) {
		for( i = 0; i < 5; i ++ ) {
			// can we store more chunks ???
			if( !_stream_parser_can_add_audio( s ) || s->audio_parse_end )
				break;

			// if we have minimum 128k in buffer or we are the end of the file
			if ( stream_buffer_get_head( s->buffer2 ) >= (128 * 1024) || stream_buffer_end( s->buffer2 ) ) {
				STREAM_CHUNK sc = { 0 };
				int err = parse_audio( s, &sc );
				stream_parser_add_chunk( s, &sc );

				if( err ) {
					// error or end of movie...
					s->audio_parse_end = 1;
serprintf("audio_parse_end\r\n");
					break;
				} 
			} else {
//serprintf("$");			
				break;
			}
		}
	}
	return 0;
}

// ***********************************************************
//
//	stream_parser_can_output
//
// ***********************************************************
int stream_parser_can_output( STREAM *s )
{
	if( !(s->parser_flags & STREAM_PARSER_LIVE) )
		return 1;
		
	//
	// live
	//
#if 1
	if( !s->video->valid || !s->audio->valid ) {
		return 1;
	}

	if( s->time_parsed < 200 ) {
		if( !s->parser_halt ) {
serprintf("parser HALT\r\n");	
			s->parser_halt = 1;
		}
	} else if( !stream_parser_can_add_chunks( s ) || s->time_parsed > 500 ) {
		if( s->parser_halt ) {
serprintf("parser GO\r\n");	
			s->parser_halt = 0;
			s->parser_video_discon = 1;
			s->parser_audio_discon = 1;
		}
	} 
	if( s->parser_halt ) {
//serprintf("aud %3d  vid %3d\r\n", aud_num, vid_num );
		return 0;
	}
	return 1;
#else
	//we want to have at least 5 video and audio chunks
	if( s->video->valid && s->audio->valid ) {
		int vid_num = stream_parser_video_chunk_num( s );
		int aud_num = stream_parser_audio_chunk_num( s );
		if( vid_num < 2 || aud_num < 1 ) {
			if( !s->parser_halt ) {
serprintf("parser HALT\r\n");	
				s->parser_halt = 1;
			}
		} else if( !stream_parser_can_add_chunks( s ) || (vid_num > 25 && aud_num > 5) ) {
			if( s->parser_halt ) {
serprintf("parser GO\r\n");	
				s->parser_halt = 0;
				s->parser_video_discon = 1;
				s->parser_audio_discon = 1;
			}
		}
		if( s->parser_halt ) {
//serprintf("aud %3d  vid %3d\r\n", aud_num, vid_num );
			return 0;
		}
	}
	return 1;
#endif
}

// ***************************************************************************
//
// 	stream_parser_get_audio_cdata
//
// ***************************************************************************
int stream_parser_get_audio_cdata( STREAM *s, CLEVER_BUFFER *audio_buffer, STREAM_CDATA *cdata )
{
	if( cdata->valid == 0 ) {
		STREAM_CHUNK c = { 0 };
		if ( !stream_parser_get_audio_chunk( s, &c ) ) {
			if( audio_buffer->size < c.size + 128 ) {
				if ( realloc_clever_buffer( audio_buffer, c.size + 128 ) ) {
					return 1;
				}
			}
			
			stream_CDATA_from_SC( cdata, &c );

			// select the buffer depending on buf2 flag
			STREAM_BUFFER *buffer = c.buf2 ? s->buffer2 : s->buffer;

			UCHAR *dst = audio_buffer->data;
			UCHAR *src = buffer->data + c.buf;

			if( s->audio->format == WAVE_FORMAT_MSAUDIO2 ) {
				int size = c.size;
				while( size > 0 ) {
					int copy = MIN( size, s->audio->blockAlign );
					memcpy( dst, src, copy);
					size -= copy;
					dst  += copy;
					src  += copy;
				}
			} else {
				memcpy( dst, src, cdata->size);
			}
			
			// free this chunks data
			stream_buffer_free_data( buffer, &c );

			cdata->valid = 1;
		} else {
			return 1;
		}
	}
	return 0;
}

// ***************************************************************************
//
// 	stream_parser_get_subtitle_cdata
//
// ***************************************************************************
int stream_parser_get_subtitle_cdata( STREAM *s, CLEVER_BUFFER *sub_buffer, STREAM_CDATA *cdata )
{
	if( cdata->valid == 0 ) {
		STREAM_CHUNK c = { 0 };
		if ( !stream_parser_get_subtitle_chunk( s, &c ) ) {
			// select the buffer depending on buf2 flag
			STREAM_BUFFER *buffer = c.buf2 ? s->buffer2 : s->buffer;

			if( c.stream != s->subtitle->stream ) {
				// drop the chunks which are not current stream
//serprintf("sub drop: %02X\r\n", c.stream );			
				// free this chunks data
				stream_buffer_free_data( buffer, &c );
				return 0;
			}

			if( sub_buffer->size < c.size ) {
				if ( realloc_clever_buffer( sub_buffer, c.size ) ) {
					return 1;
				}
			}
			
			stream_CDATA_from_SC( cdata, &c );

			memcpy( sub_buffer->data, buffer->data + c.buf, cdata->size);
			
			// free this chunks data
			stream_buffer_free_data( buffer, &c );

			cdata->valid = 1;
		} else {
			return 1;
		}
	}
	return 0;
}

// ***************************************************************************
//
// 	stream_parser_send_video_extra
//
// ***************************************************************************
void stream_parser_send_video_extra( VIDEO_PROPERTIES *video, CBE *cbe, int *size )
{
#ifdef CONFIG_H264
	if( video->format == VIDEO_FORMAT_H264 && video->needs_header ) {
		if(!video->header_sent) {
			if( !H264_parse_avcc( video, cbe, size, &video->nal_unit_size ) ) {
DBGS serprintf("extra: AVCC!\r\n" );
			}
			video->header_sent = 1;
		}
		return;
	}
#endif
#ifdef CONFIG_HEVC
	if( video->format == VIDEO_FORMAT_HEVC || video->format == VIDEO_FORMAT_WMV3 ) {
		// do not send HEVC extradata inline
		return;
	}
#endif

	if( video->extraDataSize && !video->extra_sent ) {
DBGCV serprintf("add extra: %d\r\n", video->extraDataSize );
Dump( video->extraData, video->extraDataSize );
		if (video->format == VIDEO_FORMAT_VC1) {
			int extraDataSize = 0;
			unsigned char * extraData = NULL;
			extraData = WMV_get_rcv_header(video, &extraDataSize);
			cbe_write( cbe, extraData, extraDataSize);
                        *size += extraDataSize;
			if (extraData)
				free(extraData);
		} else {
			cbe_write( cbe, video->extraData, video->extraDataSize);
			*size += video->extraDataSize;
		}
		video->extra_sent = 1;
	}
}

// ***********************************************************
//
//	_scs_chunk_num
//
// ***********************************************************
static int _scs_chunk_num( STREAM_CHUNK_STORE *scs )
{
	int num = scs->write - scs->read;
	if( num < 0 )
		num += scs->max;
	return num;
}

// ***********************************************************
//
//	_scs_chunk_max
//
// ***********************************************************
static int _scs_chunk_max( STREAM_CHUNK_STORE *scs )
{
	return scs->max - 5;
}

// ***********************************************************
//
//	_scs_can_add
//
// ***********************************************************
static int _scs_can_add( STREAM_CHUNK_STORE *scs )
{
	return _scs_chunk_num( scs ) < _scs_chunk_max( scs );
}

// ***********************************************************
//
//	_scs_clear_chunks
//
// ***********************************************************
static void _scs_clear_chunks( STREAM_CHUNK_STORE *scs )
{
	int i;
	scs->read  = 0;
	scs->write = 0;
	
	// clear the chunk lists
	for(i = 0; i < scs->max; i++ ) {
		scs->c[i].valid = CHUNK_FREE;
	}
}

// ***********************************************************
//
//	_scs_put_chunk
//
// ***********************************************************
static int _scs_put_chunk( STREAM_CHUNK_STORE *scs, STREAM_CHUNK *c, char *tag, int dbg )
{
	if( !scs || !scs->c  )
		return 1;
		
	STREAM_CHUNK *new = scs->c + scs->write;
	 
	if( c ) 
		memcpy( new, c, sizeof( STREAM_CHUNK ) );
	else
		memset( new, 0, sizeof( STREAM_CHUNK ) );
	
DBGC(dbg) serprintf("\t\tp%s[%4d|%4d]%02X  pos %8lld  buf %8d  siz %6d  time %8d  %s\r\n",
		tag, scs->write, _scs_chunk_num(scs), new->stream, (UINT64)new->pos, new->buf, new->size, c->time, c->buf2 ? "B2" : "B1" );

	new->valid = CHUNK_VALID;
	
	scs->write ++;
	if (scs->write == scs->max)
		scs->write = 0;
	return 0;
}

// ***********************************************************
//
//	_scs_get_chunk
//
// ***********************************************************
static int _scs_get_chunk( STREAM_CHUNK_STORE *scs, STREAM_CHUNK *c, char *tag, int dbg )
{
	if( !scs || !scs->c  ) {
		if( c )
			memset( c, 0, sizeof( STREAM_CHUNK ) );
		return 1;
	}
		
	if ( scs->c[scs->read].valid == CHUNK_FREE ) {
		return  1;
	}
	
	if( c ) 
		memcpy( c, scs->c + scs->read, sizeof( STREAM_CHUNK ) );
	scs->c[scs->read].valid = CHUNK_FREE;
	
DBGC(dbg) serprintf("g%s[%4d|%4d]%02X  pos %8lld  buf %8d  siz %6d  time %8d  %s%s\r\n",
		tag, scs->read, _scs_chunk_num(scs), c->stream, (UINT64)c->pos, c->buf, c->size, c->time, c->buf2 ? "B2" : "B1", c->key ? "I" : "" );

	scs->read ++;
	if (scs->read == scs->max)
		scs->read = 0;
	
	return 0;
}

// ***********************************************************
//
//	_scs_head_chunk
//
// ***********************************************************
static STREAM_CHUNK *_scs_head_chunk( STREAM_CHUNK_STORE *scs, STREAM_CHUNK *c )
{
	if( !scs || !scs->c  ) {
		if( c )
			memset( c, 0, sizeof( STREAM_CHUNK ) );
		return NULL;
	}
	int head = scs->write - 1;
	if (head < 0 )
		head += scs->max;
	
	if( c ) 
		memcpy( c, scs->c + head, sizeof( STREAM_CHUNK ) );
	return scs->c + head;
}

// ***********************************************************
//
//	_scs_peek_chunk
//
// ***********************************************************
static STREAM_CHUNK *_scs_peek_chunk( STREAM_CHUNK_STORE *scs, STREAM_CHUNK *c )
{
	if( !scs || !scs->c ) {
		if( c )
			memset( c, 0, sizeof( STREAM_CHUNK ) );
		return NULL;
	}
	if( c ) 
		memcpy( c, scs->c + scs->read, sizeof( STREAM_CHUNK ) );
	return scs->c + scs->read;
}

int stream_parser_audio_chunk_num( STREAM *s ) {	return _scs_chunk_num( &s->aud ); }
int stream_parser_video_chunk_num( STREAM *s ) {	return _scs_chunk_num( &s->vid ); }
int stream_parser_subtitle_chunk_num( STREAM *s ) {	return _scs_chunk_num( &s->sub ); }

static int _stream_parser_can_add_audio( STREAM *s ) { 	return _scs_can_add( &s->aud );   }
static int _stream_parser_can_add_video( STREAM *s ) { 	return _scs_can_add( &s->vid );   }
static int _stream_parser_can_add_subtitle( STREAM *s ){return _scs_can_add( &s->sub );   }

void stream_parser_clear_audio_chunks( STREAM *s ) {	_scs_clear_chunks( &s->aud ); }
void stream_parser_clear_video_chunks( STREAM *s ) 
{	
	if( s->ro_ctx ) {
		pts_ro_init( s->ro_ctx );
	}
	_scs_clear_chunks( &s->vid ); 
}
void stream_parser_clear_subtitle_chunks( STREAM *s ) {	_scs_clear_chunks( &s->sub ); }

int stream_parser_put_audio_chunk( STREAM *s, STREAM_CHUNK *c ) 
{
	return _scs_put_chunk( &s->aud, c, " A ", 1 );
}
int stream_parser_put_video_chunk( STREAM *s, STREAM_CHUNK *c ) 
{
	if( s->ro_ctx ) {
		pts_ro_put( s->ro_ctx, c->time );
	}
	return _scs_put_chunk( &s->vid, c, "V  ", 4 );
}
int stream_parser_put_subtitle_chunk( STREAM *s, STREAM_CHUNK *c ) 
{
	return _scs_put_chunk( &s->sub, c, "  S", 16 );
}

int stream_parser_get_audio_chunk( STREAM *s, STREAM_CHUNK *c )
{
	return _scs_get_chunk( &s->aud, c, " A ", 2 );
}
int stream_parser_get_video_chunk( STREAM *s, STREAM_CHUNK *c )
{
	int ret = _scs_get_chunk( &s->vid, c, "V  ", 8 );
	if( s->ro_ctx && !ret ) {
		int time = pts_ro_get( s->ro_ctx );
//serprintf("time: %8d\n", time);	
		if( time != -1 ) {
			c->time = time;
		}
	}
	return ret;
}
int stream_parser_get_subtitle_chunk( STREAM *s, STREAM_CHUNK *c )
{
	return _scs_get_chunk( &s->sub, c, "  S", 32 );
}

STREAM_CHUNK *stream_parser_head_audio_chunk( STREAM *s, STREAM_CHUNK *c )
{
	return _scs_head_chunk( &s->aud, c );
}
STREAM_CHUNK *stream_parser_head_video_chunk( STREAM *s, STREAM_CHUNK *c )
{
	return _scs_head_chunk( &s->vid, c );
}
STREAM_CHUNK *stream_parser_head_subtitle_chunk( STREAM *s, STREAM_CHUNK *c )
{
	return _scs_head_chunk( &s->sub, c );
}

STREAM_CHUNK *stream_parser_peek_audio_chunk( STREAM *s, STREAM_CHUNK *c )
{
	return _scs_peek_chunk( &s->aud, c );
}
STREAM_CHUNK *stream_parser_peek_video_chunk( STREAM *s, STREAM_CHUNK *c )
{
	return _scs_peek_chunk( &s->vid, c );
}
STREAM_CHUNK *stream_parser_peek_subtitle_chunk( STREAM *s, STREAM_CHUNK *c )
{
	return _scs_peek_chunk( &s->sub, c );
}

// ***********************************************************
//
//	stream_parser_clear_chunks
//
// ***********************************************************
void stream_parser_clear_chunks( STREAM *s )
{
	stream_parser_clear_audio_chunks( s );
	stream_parser_clear_video_chunks( s ); 
	stream_parser_clear_subtitle_chunks( s ); 
	
	stream_parser_clear_rate( s );
}

// ***********************************************************
//
//	stream_parser_can_add_chunks
//
// ***********************************************************
int stream_parser_can_add_chunks( STREAM *s )
{
	return ( _stream_parser_can_add_audio( s ) && _stream_parser_can_add_video( s ) && _stream_parser_can_add_subtitle( s ) );
}

// ***********************************************************
//
//	stream_parser_set_audio_stream
//
//	Generic code to change the audio stream for parsers that are seekable
//	we will reseek to the current time so that audio is re-read from the current position
//
// ***********************************************************
int stream_parser_set_audio_stream( STREAM *s, int audio_stream )
{
	if( thread_state_get( &s->parser_tstate ) != THREAD_EXIT && s->parser->seekable && s->parser->seekable( s ) ) {
		int current_time = stream_get_current_time( s, NULL );
		// reseek to current time
		stream_seek_time( s, current_time - 1, STREAM_SEEK_BACKWARD, 0 );
	} else {
		// we cannot seek to clear "old" audio, so we just force-clear it here :-(
		stream_parser_clear_audio_chunks( s );
	}
	return 0;
}

// ***********************************************************
//
//	stream_parser_pauseable
//
// ***********************************************************
int stream_parser_pauseable( STREAM *s )
{
	// assume yes unless told otherwise
	return 1;
}

// ***********************************************************
//
//	stream_parser_get_stats
//
// ***********************************************************
STREAM_PARSER_STATS *stream_parser_get_stats( STREAM *s, STREAM_PARSER_STATS *stats )
{
	stats->buffer = s->buffer;
	if( s->buffer ) {
		stats->buffer_used = stream_buffer_get_used( s->buffer );
		stats->buffer_size = s->buffer->buffer_size;
	} else {
		stats->buffer_used = 0;
		stats->buffer_size = 0;
	}
	
	stats->buffer2 = s->buffer2;
	if( s->buffer2 ) {
		stats->buffer2_used = stream_buffer_get_used( s->buffer2 );
		stats->buffer2_size = s->buffer2->buffer_size;
	} else {
		stats->buffer2_used = 0;
		stats->buffer2_size = 0;
	}
	
	stats->audio_chunks = stream_parser_audio_chunk_num( s );
	stats->video_chunks = stream_parser_video_chunk_num( s );
	stats->sub_chunks   = stream_parser_subtitle_chunk_num( s );

	stats->atime_parsed = s->atime_parsed;
	stats->vtime_parsed = s->vtime_parsed;
	
	stats->acurrent_rate = s->acurrent_rate;
	stats->vcurrent_rate = s->vcurrent_rate;
	
	return stats;
	
}

int stream_parser_find_key_frame( STREAM *s, int max_time, int *time )
{
	if( !s ) 
		return 0;

	int num = stream_parser_video_chunk_num( s );
//serprintf("max %d,  vid %3d  ", max_time, num );

	int best_time = 0;
	int best_i    = 0;
	int i;
	for( i = 1; i < num; i++ ) {
		STREAM_CHUNK *c = stream_parser_peek_n_video_chunk( s, NULL, i );
//serprintf("[%3d] %8d %s\r\n", i, c->time, c->key ? "key" : "" );
		if( c->time > max_time ) {
			break;
		}
		if( c->key ) {
			best_time = c->time;
			best_i    = i;
		}
	}
	if( best_time ) {
		if( time ) {
			*time = best_time;
		}
		return best_i;
	}
	return 0;
}

int stream_parser_drop_video( STREAM *s, int time )
{
	int dropped = 0;
	
	if( !s ) 
		return dropped;

	while( 1 ) {
		STREAM_CHUNK c = { 0 };
	
		stream_parser_peek_video_chunk( s, &c );
	
		if( c.valid != CHUNK_VALID ) {
			return dropped;
		}

		if( c.time < time ) {
			stream_parser_get_video_chunk( s, &c );
			stream_buffer_free_data( s->buffer, &c );
			dropped ++;
		} else {
			// we are done
			break;
		}
	}
	
	return dropped;
}	

#ifdef DEBUG_MSG
void *AV_get_ctx( void );
static void find_iframe( void )
{
	STREAM *s = AV_get_ctx();
	int time;
	int num;
	if( (num = stream_parser_find_key_frame( s, s->video_time + 5000, &time )) ) {
serprintf("next key_frame is at: [%d] %d < %d\r\n", num, time, s->video_time + 5000 );
	}
}

DECLARE_DEBUG_TOGGLE(      "spic", 	ignore_chunks );
DECLARE_DEBUG_COMMAND_VOID("spfi", 	find_iframe );
#endif

#endif
