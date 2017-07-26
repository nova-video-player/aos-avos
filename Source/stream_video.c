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
#include "avi.h"
#include "debug.h"
#include "device_config.h"
#include "util.h"
#include "stream.h"
#include "stream_alloc.h"
#include "stream_rc.h"
#include "stream_parser.h"
#include "stream_subtitle.h"
#include "stream_avg.h"
#include "atime.h"
#include "color.h"
#include "astdlib.h"
#include "stream_sync.h"
#include "cbe.h"
#include "browse.h"
#include "frame_q.h"
#include "wmv.h"
#include "h264.h"
#include "hevc.h"
#include "mpg4.h"
#include "dts.h"
#include "fb.h"

#include <ctype.h>
#include <stdio.h>
#include <signal.h>

#ifdef CONFIG_STREAM
 
#define DBGV   	if(Debug[DBG_VID])
#define DBGV1  	if(Debug[DBG_VID] == 1)
#define DBGV2  	if(Debug[DBG_VID] > 1)
#define DBGV3 	if(Debug[DBG_VID] > 2)
#define DBGV4 	if(Debug[DBG_VID] > 3)
#define DBGY	if(Debug[DBG_SYNC])
#define DBGQ   	if(Debug[DBG_Q] == 1)
#define DBGQ2	if(Debug[DBG_Q] == 2)
#define DBGHD 	if(Debug[DBG_HD])
#define DBGS	if(Debug[DBG_STREAM])
#define DBGMNG 	if(Debug[DBG_MANGLER])
#define DBGCV1 	if(Debug[DBG_CV] > 1)
#define DBGP 	if(Debug[DBG_PARSER])

int 		stream_zero_fill   = 1;

int 		stream_max_delay   = 1;
int 		stream_no_sync     = 0;
int	 	stream_no_audio    = 0;
int	 	stream_force_drop_audio  = -1;
int	 	stream_no_subtitles = 0;
static int	stream_no_output   = 0;
static int 	stream_no_parser   = 0;
static int 	stream_no_seek     = 0;
static int 	stream_parser_sleep = 0;
static int 	stream_no_video    = 0;
int	 	stream_fake_video  = 0;
int	 	stream_fake_chapters = 0;
int		stream_fake_audio  = 0;
int 		stream_no_reorder  = 0;
static int 	stream_dump_video  = 0;
static int 	stream_dump_audio  = 0;
static int 	stream_dump_pcm    = 0;
int	 	stream_no_deblock  = 0;
static int 	stream_no_late_idx = 0;
int 		stream_io_fail     = 0;
int 		stream_io_hang     = 0;
static int	stream_force_nonlocal   = 0;
static int	stream_force_pauseable  = 0;
static int      stream_force_max_width  = 0;
static int 	stream_force_max_height = 0;
static int	stream_force_aspect_n   = 0;
static int	stream_force_aspect_d   = 0;
static int 	stream_vcodec_delay = 0;
//static int 	stream_acodec_delay = 0;
       int 	stream_bdrop_threshold = 0;
       int 	stream_pdrop_threshold = 0;
static int 	stream_bdrop_force = 0;
int 		stream_seek_no_index   = 0;
int	 	stream_video_force_align = 0;
       int 	stream_audio_paused = 0;
       int 	stream_video_paused = 0;
static int	stream_force_error = 0;
static int	stream_force_abort = 0;
static int	stream_fps_mode = 0;
int		stream_vtime_post_sink = -1;
static int	stream_screen_shot = 0;
static int 	stream_force_num_frames = -1;
static int 	stream_tweak_fps = 0;
static int 	stream_fake_ts_post  = 0;
static int 	stream_fake_ts_num   = 0;
static int 	stream_fake_ts_den   = 0;
static int 	stream_fake_ts_count = 0;
static int	stream_buffer_sleep_datarate = 14000; // in Mbit/s
static int	frames_dropped;
static int	frames_B_dropped;
static int 	frames_doubled;
static int	stream_force_prio = 0;
static int	stream_handle_codec_error = 1;
static int	stream_force_audio_filter_level = -1;
static int	stream_force_audio_filter_night_on = 0;
static int	stream_video_force_reorder = 0;

static int	stream_force_sw_deinterlacing = 1;
static int 	stream_deinterlacing_max_height = 600;
static int 	stream_audio_max_channels = 2;
static int	stream_audio_downmix = 1;

// ms value 
static int 	stream_sink_preroll   =  50;
static int 	stream_sink_max_delay = 100;
static int	stream_key_frame_only = 0;
static int 	codec_max = 100;

static int 	ignore_first_unpause = 0;

static int	do_core;
static const char *stream_force_codec = NULL;

static void _output_frame_no_resize  ( STREAM *s, VIDEO_FRAME *frame, VIDEO_FRAME **qframe );

#define MAX_VIDEO_FRAME_WIDTH		848
#define MAX_VIDEO_FRAME_HEIGHT		576

#define THREAD_PRIO_AUDIO	1
#define THREAD_PRIO_ENGINE	10
#define THREAD_PRIO_PARSER	1
#define THREAD_PRIO_VIDEO	2
#define THREAD_PRIO_SUBTITLE	0

int 		stream_prio_audio  = THREAD_PRIO_AUDIO;
int 		stream_prio_engine = THREAD_PRIO_ENGINE;
int 		stream_prio_parser = THREAD_PRIO_PARSER;
int 		stream_prio_video  = THREAD_PRIO_VIDEO;
int 		stream_prio_sub  = THREAD_PRIO_SUBTITLE;

AV_PROPERTIES *stream_force_video_props = NULL;

static int stream_sync_mode         = -1;
static int stream_buffer_size       = STREAM_DEFAULT_BUFFER_SIZE;
static int stream_buffer_large      = STREAM_LARGE_BUFFER_SIZE;
static int stream_force_buffer      = 0;   

static int stream_sink_video_frames = 4;
static int stream_sink_video_max    = 5;

static void	_seek_init( STREAM *s );
static void 	_free_all_frames( STREAM *s );
static int  	_stream_seek_loop( STREAM *s );
static int  	_stream_get_real_time( STREAM *s, int time );
static int  	_stream_seek_real( STREAM *s, int time, int pos, int dir, int flags, int force_reload );
static void 	_stream_player_sync( STREAM *s );
static void 	_stream_player_async( STREAM *s );
static int  	_stream_wait_for_idle( STREAM *s, int timeout );
static void 	_stream_play_n_frames( STREAM *s, int n, int time, int old_time );
static void 	_do_stuff( STREAM *s );
static void 	*_parser_thread( void *data );
static void 	*_player_thread( void *data );
static void 	*_decode_thread( void *data );
void 		*stream_sub_dec_thread( void *data );
static int 	_stream_redraw( STREAM *s );
static void	_query_sink_frames( STREAM *s );

STREAM_AVG a_avg;
STREAM_AVG v_avg;
STREAM_AVG aud_avg;

static int t_adecode;
static int t_adecode_bytes;
static int t_show;
static int t_showtime;

static inline STREAM_CPU get_cpu_priority( STREAM *s )
{
	return (s)->flags & STREAM_THUMB ? STREAM_CPU_ARM : s->cpu_prio;
}

STREAM_FILTER_AUDIO *stream_filter_audio_agc_new( void );
STREAM_FILTER_AUDIO *stream_filter_audio_compress_new( void );

// *****************************************************************************
//
//	_video_init
//
// *****************************************************************************
static void _video_init( STREAM *s, int time )
{
	if( s->video_dec && !s->video_dec->async ) {
		_free_all_frames( s );
	}
	
	stream_sync_restart( s );
	
	s->seek	       = 0;
	s->time_parsed = 0;

	stream_fake_ts_count = 0;
	
	//clear also error state, so we can skip over "broken" files
	s->error = 0;
	
	// is there a mangler, init it!
	if( s->video_mangler ) {
		s->video_mangler->init( s, time );
	}	
	if( s->video_dec && s->video_dec->seek ) {
		s->video_dec->seek( s->video_dec, time );
	}
	if( s->video_sink && s->video_sink->put_time ) {
		s->video_sink->put_time( s->video_sink, time );
	}

	clear_avg( &v_avg );
	clear_avg( &a_avg );
	clear_avg( &aud_avg );
}

// *****************************************************************************
//
//	_allocate_buffers
//
// *****************************************************************************
static int _allocate_buffers( STREAM *s )
{
DBGS serprintf("_allocate_buffers\r\n");
	
	// alloc one buffer for current audio bitstream data
	if( s->audio->valid ) {
		if( malloc_clever_buffer( &s->audio_now,  65536 ) ) {
serprintf("cannot alloc audio_frame\r\n");			
			return 1;
		}
	}
	return 0;
}

// *****************************************************************************
//
//	_allocate_video_buffers
//
// *****************************************************************************
static int _allocate_video_buffers( STREAM *s )
{
	int mem_type = s->video_rc.mem_type;
	int mem_size = s->video_rc.mem_size;
	int cached   = s->video_rc.output_cached;
	int width    = s->video->padded_width  ? s->video->padded_width  : s->video->width;
	int height   = s->video->padded_height ? s->video->padded_height : s->video->height;

	// hack, assume TILER/ION/CMA for NV12..
	int vid_mem_type = stream_get_mem_type();
	
	if( s->video->valid ) {
DBGS serprintf("_allocate_video_buffers, type %d  size %d  cs %d\r\n", mem_type, mem_size, s->video->colorspace);
		// alloc a large CBE buffer for bitstream data
		int size = mem_size ? mem_size : VIDEO_MINDATA_SIZE;
		int type = (mem_type == STREAM_MEM_DMA) ? 1 : 0; 
		if( !(s->cbe = cbe_new( size, size, type ) ) ) {
serprintf("cannot alloc CBE\r\n");			
			return 1;
		}

		s->num_frames = s->video_rc.num_frames;
DBGS serprintf("delay %d  num_frames %d\r\n",s->video->delay_frames, s->num_frames );
	
		if( !s->use_sink_frames ) {
			int i;
			for( i = 0; i < s->num_frames; i++ ) {
				if( cached ) {
					s->frames[i] = frame_alloc_cached( width, height );
				} else {
					s->frames[i] = frame_alloc_with_cs_and_mem( width, height, s->video->colorspace, vid_mem_type );
				}
				if( !s->frames[i] ) {
serprintf("cannot alloc frame %d\r\n", i);			
					return 1;
				}
				s->frames[i]->index   = i;
				s->frames[i]->user_ID = i;
				s->frames[i]->locked  = 0;
			}
		}
	}
	
	return 0;
}

// *****************************************************************************
//
//	_free_buffers
//
// *****************************************************************************
static void _free_buffers( STREAM *s )
{
DBGS serprintf("_free_buffers\r\n");
	free_clever_buffer( &s->audio_now ); 

	if( s->subtitle_frame ) {
		frame_free( s->subtitle_frame );
		s->subtitle_frame = NULL;
	}	

	free_clever_buffer( &s->sub_buffer );
}

// *****************************************************************************
//
//	_free_video_buffers
//
// *****************************************************************************
static void _free_video_buffers( STREAM *s )
{
DBGS serprintf("_free_video_buffers\r\n");
	cbe_delete( &s->cbe );

	int i;
	if( !s->use_sink_frames ) {
		for( i = 0; i < s->num_frames; i++ ) {
			if( s->frames[i] ) {
				frame_free( s->frames[i] );
				s->frames[i] = NULL;
			}
		}
	}
}

// *****************************************************************************
//
//	stream_open_audio_dec
//
// *****************************************************************************
static int stream_open_audio_dec( STREAM *s )
{
	if( s->audio_dec ) {
DBGS serprintf("stream_open_audio_dec\r\n");
		// open the decoder
		if( s->audio_dec->new && s->audio_dec->new( s->audio ) ) {
serprintf("error creating audio_dec!\r\n");
			s->audio_dec = NULL;		
			return 1;
		}
		if( stream_audio_downmix ) {
			s->audio->request_channels = s->audio_max_channels;
		}
		if( s->audio_dec->open( s->audio ) ) {
serprintf("error opening audio_dec!\r\n");
			s->audio_dec = NULL;		
			return 1;
		}
		s->audio->bytesPerFrame = s->audio->channels * s->audio->bitsPerSample / 8;

		memset( &s->audio_rc, 0, sizeof( s->audio_rc ) );
		if( s->audio_dec->get_rc ) {			
			if( !s->audio_dec->get_rc( s->audio, &s->audio_rc ) ) {
DBGS stream_show_rc( &s->audio_rc );
			}
		}

		if( stream_dump_audio ) {
			stream_dump_audio = 0;
			s->dump_audio_fd = file_open( HDD_ROOT"audio.dat", O_WRONLY | O_CREAT | O_TRUNC, 0600 );
		}
		if( stream_dump_pcm ) {
			stream_dump_pcm = 0;
			s->dump_pcm_fd = file_open( HDD_ROOT"audio.pcm", O_WRONLY | O_CREAT | O_TRUNC, 0600 );
		}
		
		return 0;
	}
serprintf("no audio dec found!\r\n");

	return 1;
}

// *****************************************************************************
//
//	stream_open_audio_filter
//
// *****************************************************************************
static int stream_open_audio_filter( STREAM *s )
{
	if( s->audio_filter_enabled ) {
#ifdef CONFIG_AUDIO_COMPRESS
		s->audio_filter = stream_filter_audio_compress_new();
#endif
#ifdef CONFIG_AUDIO_AGC
		s->audio_filter = stream_filter_audio_agc_new();
#endif
		if( s->audio_filter ) {
DBGS serprintf("stream_open_audio_filter: [%s]\r\n", s->audio_filter->name);
			if( s->audio_filter->open( s->audio_filter, s->audio ) ) {
				if( s->audio_filter->delete  ) {
					s->audio_filter->delete( s->audio_filter );
				}	
				s->audio_filter_enabled = 0;
				s->audio_filter = NULL;
				return 1;
			}
			stream_set_audio_filter_level( s, s->audio_filter_level, s->audio_filter_night_on );
		}
	}
	return 0;
}

// *****************************************************************************
//
//	stream_find_best_audio_stream
//
// *****************************************************************************
static void stream_find_best_audio_stream( STREAM *s )
{
	int best      = 0;
	int best_prio = 100;
	
	int i;
	for( i = 0; i < s->av.as_max; i++ ) {
		AUDIO_PROPERTIES *audio = s->av.audio + i;
		if( audio->priority < best_prio ) {
			best      = i;
			best_prio = audio->priority;
		}
	}
	s->av.as = best;
}

// *****************************************************************************
//
//	stream_try_open_audio_dec
//
//	go through all audio codecs and try to find at least one that we can open
//	(allowed and we have a decoder for it and we can open the decoder)
//
// *****************************************************************************
static int stream_try_open_audio_dec( STREAM *s, int new, int *unsupported )
{
	if( s->flags & STREAM_NO_AUDIO_CODEC ) {
		// just plain fail
		if( unsupported )
			*unsupported = 1;
		return 1;
	}

	int i;
	// try all audio codecs and stop if we find one we can open
	for( i = 0; i < s->av.as_max; i++, new++ ) {
		if( new >= s->av.as_max ) {
			new = 0;
		}
DBGS serprintf("try audio[%d]\r\n", new );
		s->audio = s->av.audio + new;
		
		if( !s->audio->valid ) {
DBGS serprintf("audio[%d] not valid\r\n", new );
			continue;
		}

		if( s->audio->not_allowed ) {
DBGS serprintf("audio[%d] not allowed\r\n", new );
			continue;
		}
		
		// try to get an audio decoder
		s->audio_dec = stream_get_audio_dec( s->audio );
		if( !s->audio_dec ) {
DBGS serprintf("audio[%d] no decoder\r\n", new );
			if( unsupported )
				*unsupported = 1;
			continue;
		}
		
		if( stream_open_audio_dec( s ) ) {
DBGS serprintf("audio[%d] cannot open\r\n", new);
			if( unsupported )
				*unsupported = 1;
			continue;
		}

		if( new != s->av.as ) {
DBGS serprintf("audio[%d] set parser!\r\n", new);
			// tell the parser that the audio stream has changed!
			s->av.as = new;
			s->parser->set_audio_stream( s, new );
		}
		
		s->av.as = new;
		
		return 0;
	}
	
	// fallback to original one
	s->audio = s->av.audio + s->av.as;
	
	return 1;
}

// *****************************************************************************
//
//	stream_open_video_dec
//
// *****************************************************************************
static int stream_open_video_dec( STREAM *s, int *unsupported )
{
DBGS serprintf("stream_open_video_dec\r\n");
	if( s->flags & STREAM_NO_VIDEO_CODEC ) {
		goto ErrorExit;
	}

	pthread_mutex_init( &s->video_sink_mutex, NULL );
	int prio = stream_force_prio ? stream_force_prio : get_cpu_priority(s);
	int forced;
	if (prio == STREAM_CPU_ANY) {
		prio = STREAM_CPU_DSP;
		forced = 0;
	} else {
		forced = 1;
	}
	while( prio ) {
		// reset previous error states
		s->video_error           = VE_NO_ERROR;
		s->video_error_qualifier = VEQ_NONE;
		s->video_error_desc[0]   = '\0';
		// try to get a video decoder
		s->video_dec = stream_get_new_dec_video( s->video, &s->video_mangler, prio, forced, stream_force_codec );
		prio = stream_force_prio ? 0 : prio - 1;
		
		if( !s->video_dec) {
			goto next;
		}
		// try to open the decoder
		if( s->video_dec->open( s->video_dec, s->video, s, &s->video->flush_frames, &s->video->delay_frames ) ) {
			// no dec
serprintf("error opening video_dec[%s]!\n", s->video_dec->name);
			goto next;
		}

		memset( &s->video_rc, 0, sizeof( s->video_rc ) );
		if( s->video_dec->get_rc ) {			
			if( !s->video_dec->get_rc( s->video_dec, &s->video_rc ) ) {
DBGS stream_show_rc( &s->video_rc );
			}
		}
		
		// for thumbs we dont need so many frames!
		if( s->flags & STREAM_THUMB ) {
			s->video_rc.num_frames = 2;
		}
		if(stream_force_num_frames != -1) { 
			s->video_rc.num_frames = stream_force_num_frames;
		}
		
		// Initialize the video sink
		// no caller provided sink, use standard
		if( !s->video_sink ) {
			if (s->video_dec->get_sink) {
				s->video_sink = s->video_dec->get_sink(s->video_dec);
			} else {
				s->video_sink = stream_get_default_video_sink(s);
			}
		}
		if( !s->video_sink ) {
serprintf("stream: no video sink!\r\n");
		}
serprintf("VID_SNK: [%s]\n", s->video_sink->name);	
		s->output_frame_fn = _output_frame_no_resize;
		if( stream_sink_video_allocates_frames(s->video_sink) ) {
			s->use_sink_frames = 1;
			s->vtime_post_sink = 1;
DBGS serprintf("stream: use sink frames!\r\n");
DBGS serprintf("stream: use post sink!\r\n");
		} else {
DBGS serprintf("stream: resize to sink!\r\n");
		}

		if( _allocate_video_buffers( s ) ) {
serprintf("could not allocate video_buffers!r\n");
			goto next;
		}

		if( s->video->msPerFrame >= 1000 ) {
			// do not try to sync to videos (slideshows?)
			// that have less than 1 fps
DBGS serprintf("slideshow!\r\n" );
			s->slideshow = 1;
		}

		if( s->video_sink ) {
			int num_frames = s->video_rc.num_frames;
			if( s->video_sink->open( s->video_sink, s->video, s, num_frames, &s->video_rc ) ) {
serprintf("error, could not open video sink!\r\n");
				goto next;
			}
		}
		if ( s->video->valid) {
			if( s->use_sink_frames ) {
				pthread_mutex_lock( &s->video_sink_mutex );
				_query_sink_frames( s );
				pthread_mutex_unlock( &s->video_sink_mutex );
			}
			if( s->video_dec->prepare ) {
				if( s->video_dec->prepare( s->video_dec, s->frames, s->num_frames ) ) {
serprintf("error, could not prepare video dec!\r\n");
					goto next;
				}
			}
		}

		if( s->video_dec->no_extra ) {
			s->video->no_extra = 1;
		}
DBGS serprintf("stream_open_video_dec: %s/%d/%d done!\r\n", s->video_dec->name, s->video_dec->cpu, s->video->no_extra);

		return 0;
next:
		if (s->video_dec) {
			if (s->video_dec->is_open) {
				// call cleanup if needed
				if( s->video_dec->cleanup && s->video_dec->cleanup( s->video_dec, s->frames, s->num_frames ) ) {
serprintf("error, could not cleanup video dec!\n");
				}
				s->video_dec->close( s->video_dec );
			}
			s->video_dec->destroy( s->video_dec );
			s->video_dec = NULL;
		}
		if (s->video_sink) {
			if (s->video_sink->is_open) {
				s->video_sink->close(s->video_sink);
			}
		}
	} 
ErrorExit:
serprintf("no video_dec found!\r\n");
	if( unsupported )
		*unsupported = 1;
	if (s->video_error == VE_NO_ERROR) {
		// video_dec didn't set any error
		s->video_error = VE_ERROR;
	}
	
	return 1;
}

// *****************************************************************************
//
//	stream_close_audio_dec
//
// *****************************************************************************
static void stream_close_audio_dec( STREAM *s )
{
	if( s->audio_dec ) {
DBGS serprintf("stream_close_audio_dec\r\n");
		// close the decoder
		s->audio_dec->close( s->audio );
		if( s->audio_dec->delete )
			s->audio_dec->delete( s->audio );
		s->audio_dec = NULL;
		if( s->dump_audio_fd > 0 ) {
			file_close( s->dump_audio_fd );
		}
		if( s->dump_pcm_fd > 0 ) {
			file_close( s->dump_pcm_fd );
		}
	}
}

// *****************************************************************************
//
//	stream_close_audio_filter
//
// *****************************************************************************
static void stream_close_audio_filter( STREAM *s )
{
	if( s->audio_filter ) {
DBGS serprintf("stream_close_audio_filter\r\n");
		s->audio_filter->close( s->audio_filter );
		if( s->audio_filter->delete  ) {
			s->audio_filter->delete( s->audio_filter );
		}
		s->audio_filter = NULL;
	}	
}

// *****************************************************************************
//
//	stream_close_video_dec
//
// *****************************************************************************
static void stream_close_video_dec( STREAM *s )
{
	if( s->video_dec) {
DBGS serprintf("stream_close_video_dec\r\n");
		// call cleanup if needed
		if( s->video_dec->cleanup && s->video_dec->cleanup( s->video_dec, s->frames, s->num_frames ) ) {
serprintf("error, could not cleanup video dec!\n");
		}
		s->video_dec->close( s->video_dec );
		s->video_dec->destroy( s->video_dec );
		s->video_dec = NULL;
	}
	if( s->video_sink && s->video_sink->is_open ) {
		s->video_sink->close( s->video_sink );
	}

	_free_video_buffers( s );
}

void stream_close_sub_dec( STREAM *s );

// ***************************************************************************
//
// 	_prebuffer_n_seconds
//
// ***************************************************************************
static void _prebuffer_n_seconds( STREAM *s, int seconds )
{
	int rate = MIN( 2000000, stream_get_total_rate( s ) );
	
	if( rate ) {
	 	int bytes = rate * seconds;
serprintf("prebuffer: %d\r\n", bytes);
		stream_parser_prebuffer( s, s->buffer, bytes );
	}
}

// *****************************************************************************
//
//	stream_check_video
//
// *****************************************************************************
static int stream_check_video( STREAM *s )
{
	s->video_error_qualifier = 0;
	
	s->video->format = video_get_format_from_fourcc( s->video->fourcc, &s->video->subfmt );
DBGS serprintf("video format %d  subfmt %d\r\n", s->video->format, s->video->subfmt );
	
	if( s->video->format == VIDEO_FORMAT_UNKNOWN && s->video->codec_id == 0 ) {
serprintf("video format %.4s not supported\r\n", &s->video->fourcc );
		// unknown format, give message
		stream_set_error( s, VE_VIDEO_NOT_SUPPORTED );
		s->video_error_qualifier = 0;
		return 1;
	}
		
	// check size of video frame
	if ( !s->video->width || !s->video->height ) {
serprintf("bad width/height  %d/%d\r\n", s->video->width, s->video->height );
		stream_set_error( s, VE_FILE_ERROR );
		return 1;
	}

	// check general max dimensions
	if ( s->video->width > s->video_max_width || s->video->height > s->video_max_height ) {
serprintf("video dimensions too big %d x %d, stream only supports %d x %d\r\n", s->video->width, s->video->height, s->video_max_width, s->video_max_height );
		stream_set_error( s, VE_TOO_BIG_FOR_STREAM );
		return 1;
	}

	// check codec specific max dimensions
	int dec_width, dec_height;
	if( !stream_get_dec_video_res( s->video, &dec_width, &dec_height, get_cpu_priority(s), stream_force_codec ) ) {
		if ( s->video->width > dec_width || s->video->height > dec_height ) {
serprintf("video dimension too big %d x %d, codec only supports %d x %d\r\n", s->video->width, s->video->height, dec_width, dec_height );
			stream_set_error( s, VE_TOO_BIG_FOR_CODEC );
			return 1;
		}
	}

	int answer;
	if( s->ask_video && !s->ask_video( s, s->video, &answer ) ) {
serprintf("video format %.4s not allowed\r\n", &s->video->fourcc);
		stream_set_error( s, VE_VIDEO_NOT_ALLOWED );
		s->video_error_qualifier = answer;
		return 1;
	}

	// no scale/rate yet, try to guess...
	if( !s->video->rate || !s->video->scale ) {
		// prebuffer at least 3s worth of video (unless thumb, then we dont care..
		if( !(s->flags & STREAM_THUMB) ) {
			_prebuffer_n_seconds( s, 3 );
		}
		// we need to roughly know the frame rate
		s->video->msPerFrame   = stream_parser_guess_msPerFrame(s);
		s->video->framesPerSec = 1000 / s->video->msPerFrame;
		s->video->scale        = 1;
		s->video->rate         = s->video->framesPerSec;
	} else {
		// if we have scale and rate (mostly for AVI)
		s->video->msPerFrame   = 1000 * (UINT64)s->video->scale / (UINT64)s->video->rate;		
		s->video->framesPerSec = (UINT64)s->video->rate / (UINT64)s->video->scale;
	} 

	if( s->video->format == VIDEO_FORMAT_MPG4 && !s->video->vol ) {
serprintf("MPG4: need VOL!\n" );
		VIDEO_PROPERTIES video = { 0 };
		if( s->video->extraDataSize ) {
			// try extra data first
			MPG4_get_video_props( &video, s->video->extraData, s->video->extraDataSize );
		}
		
		if( !video.vol ) {
			// try to get some chunks if we dont have any
			if( !stream_parser_video_chunk_num( s ) ) {
				stream_parser_guess_msPerFrame(s);
			}

			// try to read VOL from 1st video chunk
			STREAM_CHUNK *c = stream_parser_peek_video_chunk( s, NULL );
			if( c && c->valid == CHUNK_VALID ) {
DBGP serprintf("type %d  buf %d  size %d\n", c->type, c->buf, c->size );
DBGP Dump( s->buffer->data + c->buf, 256 );
				MPG4_get_video_props( &video, s->buffer->data + c->buf, c->size );
			}
		}
		
		if( video.vol ) {
serprintf("MPG4: got VOL!\n" );
			// copy only what we need, dont let MPG4 rate/scale overwrite
			// the container values
			s->video->vol          = video.vol;
			s->video->fourcc       = video.fourcc;
			s->video->format       = video.format;
			s->video->width        = video.width;
			s->video->height       = video.height;
			s->video->sprite_usage = video.sprite_usage;
		}
	}
	return 0;
}

static struct wav_fmt_str {
	int old;
	int new;
} wav_fmt[] = 
{
	{ WAVE_FORMAT_FAAD_AAC,	WAVE_FORMAT_AAC },	// 0x706D -> 0x00FF!
};

static void fixup_audio_format( AUDIO_PROPERTIES *audio )
{
	// check if we need to remap a wave format:
	int i;
	for( i = 0; i < sizeof( wav_fmt ) / sizeof( struct wav_fmt_str); i++ ) {
		if( wav_fmt[i].old == audio->format ) {
			audio->format = wav_fmt[i].new;
			break;
		}
	} 
}

#ifdef CONFIG_FFMPEG_AUDIO
int ffmpeg_audio_get_profile( AUDIO_PROPERTIES *audio, UCHAR *data, int size, int *profile, int *channels );
#endif

static void check_DTS_profile( STREAM *s, AUDIO_PROPERTIES *audio )
{
serprintf("check_DTS_profile!\n" );
	if(!s->parser->get_stats) {
		return;
	}
	if( !(s->flags & STREAM_THUMB) ) {
		_prebuffer_n_seconds( s, 3 );
	}
	STREAM_PARSER_STATS _st;
	STREAM_PARSER_STATS *st = s->parser->get_stats( s, &_st );
serprintf("stat: %d\n", st->audio_chunks );
	if( !st->audio_chunks ) {
		stream_parser_preparse(s);
	}
	st = s->parser->get_stats( s, &_st );
serprintf("stat: %d\n", st ? st->audio_chunks : 0 );
	int i;
	for( i = 0; i < st->audio_chunks; i++ ) {
		UCHAR *data;
		STREAM_CHUNK *sc = stream_parser_peek_n_audio_chunk( s, i, &data );
serprintf("[%d]  stream %d  size %5d\n", i, sc->stream, sc->size );
		if( sc && sc->stream == audio->stream ) {
#ifdef CONFIG_FFMPEG_AUDIO
			int profile  = 0;
			int channels = 0;
			ffmpeg_audio_get_profile( audio, data, sc->size, &profile, &channels );
			if( profile ) {
				audio->format = DTS_get_format_from_profile( profile );
serprintf("DTS_profile %d -> format %04X\n", profile, audio->format );
			}
			if( channels ) {
				audio->channels = channels;
serprintf("DTS_channels %d\n", audio->channels );
			}
#endif
			break;
		}
	}
}

// *****************************************************************************
//
//	stream_check_audio
//
//	go through all audio codecs and try to find at least one playable
//	(allowed and we have a decoder for it)
//
// *****************************************************************************
static int stream_check_audio( STREAM *s )
{
	s->audio_error_qualifier = 0;

	int playable    = 0;
	int not_allowed = 0;
	int reason      = - 1;
	
	int i; 
	// try all audio codecs and check for each if we have a decoder
	for( i = 0; i < s->av.as_max; i++ ) {
		AUDIO_PROPERTIES *audio = &s->av.audio[i];
DBGS serprintf("check audio[%d] format %4X\r\n", i, audio->format );
	
		fixup_audio_format( audio );
		
		if( !audio->valid ) {
DBGS serprintf("audio[%d] not valid\r\n", i);
			continue;
		}
		
		// check DTS profile
		if( audio->format == WAVE_FORMAT_DTS ) {
			check_DTS_profile( s, audio );
		}
		
		// try to get an audio decoder
		STREAM_DEC_AUDIO *dec = stream_get_audio_dec( audio );
		if( !dec ) {
DBGS serprintf("audio[%d] no decoder\r\n", i);
			continue;
		}
		
		// check if this codec is allowed
		int answer;
		if( s->ask_audio && !s->ask_audio( s, audio, &answer ) ) {  
serprintf("audio[%d] not allowed %d\r\n", i, audio->format );
			// remember
			audio->not_allowed = 1;
			audio->not_allowed_reason = answer;
			
			// remember 1st one:
			if( !not_allowed ) {
				not_allowed = 1;
				reason = answer;
			}
			continue;
		}
		
serprintf("audio[%d] playable!\r\n", i );
		playable = 1;
	}

	if( !playable && not_allowed ) {
		// there is no playable audio and at least one codec is not allowed: error!
		stream_set_error( s, VE_AUDIO_NOT_ALLOWED );
		s->audio_error_qualifier = reason;
		return 1;
	}

	return 0;
}

// *****************************************************************************
//
//	stream_drop_audio
//
// *****************************************************************************
static void stream_drop_audio( STREAM *s )
{
	s->audio->valid  = 0;
	if( s->buffer )
		s->buffer->audio = 0;
	stream_parser_clear_audio_chunks( s );
}

// *****************************************************************************
//
//	stream_drop_video
//
// *****************************************************************************
static void stream_drop_video( STREAM *s )
{
	s->video->valid  = 0;
	if( s->buffer )
		s->buffer->video = 0;
	stream_parser_clear_video_chunks( s );
}

void stream_drop_subtitles( STREAM *s );

// *****************************************************************************
//
//	_video_size_changed
//
// *****************************************************************************
static void _video_size_changed( STREAM *s )
{
serprintf("video size changed!\r\n");
	// change the linestep in our decoder frames!
	if( !s->use_sink_frames ) {
		int i;
		for( i = 0; i < s->num_frames; i++ ) {
			if( s->frames[i] ) {
				s->frames[i]->linestep[0] = linestep_from_width( s->video->width );
			}
		}
	}

	stream_resize( s );
}

// *****************************************************************************
//
//	_video_props_changed
//
// *****************************************************************************
static int _video_props_changed( STREAM *s, STREAM_CDATA *cdata )
{
	AV_PROPERTIES *changed = cdata->changed;
	UCHAR *saved_cbe = NULL;
	int saved_cbe_size = 0;
	int skip_init = 0;
	int err = 0;
	
serprintf("video props changed: ");
	if ( 	changed->video->width   == s->video->width   &&
		changed->video->height  == s->video->height  &&
		changed->video->scale   == s->video->scale   && 
		changed->video->rate    == s->video->rate    &&
		changed->video->fourcc  == s->video->fourcc  &&
		changed->video->profile == s->video->profile &&
		changed->video->level   == s->video->level     
	) {
serprintf("aspect only\n");
		// only aspect ratio changed
		skip_init = 1;
		s->video->aspect_n = changed->video->aspect_n;
		s->video->aspect_d = changed->video->aspect_d;
	} else {
serprintf("full\n");
		//copy the video props
		memcpy( &s->av.video, &changed->video, sizeof( changed->video ) );
		s->av.vs_max = changed->vs_max;
		s->av.vs = 0;
		s->video = &s->av.video[s->av.vs];
	}

	cdata->changed = NULL;
	
	
DBGV { 
int i;
for( i = 0; i < s->av.vs_max; i++ ) {
	show_video_props(&s->av.video[i] );
}
}
	if( skip_init ) {
		_video_size_changed( s );
		return 0;
	}

	// we have to preserve the bitstream data, because stream_close_video_dec will drop the CBE!
	if( cdata->size && s->cbe ) {
		saved_cbe_size = cbe_get_used( s->cbe );
		saved_cbe = amalloc( saved_cbe_size );
		if( saved_cbe ) { 
			memcpy( saved_cbe, cbe_get_p( s->cbe ), saved_cbe_size );
	  	}
	}
	// close the old video decoder
	stream_close_video_dec( s );
	
	if( stream_check_video( s ) ) {
		stream_drop_video( s );
		err = 1;
		goto ErrorExit;
	}
	
	// open the new one		
	if( stream_open_video_dec( s, NULL ) ) {
		goto ErrorExit;
	}

	s->player = s->video_dec->async ? _stream_player_async : _stream_player_sync;

	if( _allocate_video_buffers( s ) ) {
serprintf("could not allocate video_buffers!r\n");
		stream_set_error( s, VE_ERROR );
		err = 1;
		goto ErrorExit;
	}

	_video_size_changed( s );
	
	// now free all frames, otherwise there could be frames in the
	// output pipe (s->decode/output_frame) that are now invalid
	if( s->video_dec) {
		_free_all_frames( s );
	}
	
	// and put the saved video data back:
	if( s->cbe && saved_cbe && saved_cbe_size ) {
		int to_copy = MIN( saved_cbe_size, cbe_get_free( s->cbe ) );
serprintf("cbe copy: %d\r\n", to_copy );	
		cbe_write( s->cbe, saved_cbe, to_copy );
	}
	
	// tell the user
	if( s->message_cb ) {
		s->message_cb( s, STREAM_VIDEO_PROPS_CHANGED );
	}
ErrorExit:
	if( saved_cbe )
		afree( saved_cbe );
		
	// force resync!
	cdata->video_skip = 1;
	
	return err;
}

// *****************************************************************************
//
//	stream_audio_samplerate_changed
//
// *****************************************************************************
void stream_audio_samplerate_changed( STREAM *s )
{
serprintf("stream_audio_samplerate_changed!\r\n");
	// stop audio sink
	if( s->audio_sink) {
		s->audio_sink->flush( s );
		s->audio_sink->stop( s );
	}
	if( s->audio_sink->start( s ) ) {
		// no audio, close the codec
		stream_close_audio_dec( s );
		// drop audio
		stream_drop_audio( s );
	}
}

// *****************************************************************************
//
//      stream_set_audio_downmix
//
// *****************************************************************************
void stream_set_audio_downmix( int downmix )
{
	stream_audio_downmix = downmix;
}

// *****************************************************************************
//
//	stream_audio_props_changed
//
// *****************************************************************************
void stream_audio_props_changed( STREAM *s, STREAM_CDATA *cdata )
{
	AV_PROPERTIES *changed = cdata->changed;

serprintf("audio props changed!\r\n");
	//copy the video props
	memcpy( &s->av.audio, &changed->audio, sizeof( changed->audio ) );
	s->av.as_max = changed->as_max;
	if(changed->as < changed->as_max) {
		s->av.as = changed->as;
	} else {
		s->av.as = 0;
	}
	s->audio = &s->av.audio[s->av.as];

	// tell the user
	if( s->message_cb ) {
		s->message_cb( s, STREAM_AUDIO_PROPS_CHANGED );
	}
	
DBGV { 
int i;
for( i = 0; i < s->av.as_max; i++ ) {
	show_audio_props(&s->av.audio[i] );
}
}

	// close old audio decoder
	stream_close_audio_dec( s );

	// stop audio sink
	if( s->audio_sink) {
		s->audio_sink->flush( s );
		s->audio_sink->stop( s );
	}

	// check audio format
	if( stream_check_audio( s ) ) {
		stream_drop_audio( s );
		goto ErrorExit;
	}	
	
	// open the new one		
	if( stream_try_open_audio_dec( s, s->av.as, NULL ) ) {
		// no audio, disable it
		stream_drop_audio( s );
	} else {
		if( s->audio_sink->start( s ) ) {
			// no audio, close the codec
			stream_close_audio_dec( s );
			// drop audio
			stream_drop_audio( s );
		}
			
		if( s->sync_mode == STREAM_SYNC_SAMPLES ) {
			s->audio_time     = -1;
			s->audio_ref_time = -1;
		}
	}

ErrorExit:
	cdata->changed = NULL;
	// force resync!
	cdata->audio_skip = 1;
}

// *****************************************************************************
//
//	stream_yield
//
// *****************************************************************************
void stream_yield( void )
{
	msec_sleep( 1 );
}

// *****************************************************************************
//
//	stream_yield_RT
//
// *****************************************************************************
void stream_yield_RT( void )
{
	msec_sleep( 1 );
}

static VIDEO_FRAME *find_frame( STREAM *s, void *tag )
{
	int i;
	for( i = 0; i < s->num_frames; i++ ) {
		if( s->frames[i] && s->frames[i] == (VIDEO_FRAME*)tag ) {
			return s->frames[i];
		}
	}
	return NULL;
}

// *****************************************************************************
//
//	stream_lock_frame
//
// *****************************************************************************
int stream_lock_frame( STREAM *s, void *tag )
{
	VIDEO_FRAME *f = find_frame( s, tag );
	if( f ) {
//DBGQ serprintf(" LOCK( %08X %2d ) ", tag, f->index );
DBGQ serprintf("LCK[%2d] ", f->index );
		f->locked = 1;
		return f->index;
	}
	return -1;
}

// *****************************************************************************
//
//	stream_get_frame
//
// *****************************************************************************
VIDEO_FRAME *stream_get_frame( STREAM *s, void *tag )
{
	return find_frame( s, tag );
}

// *****************************************************************************
//
//	stream_unlock_frame
//
// *****************************************************************************
VIDEO_FRAME *stream_unlock_frame( STREAM *s, void *tag )
{
	VIDEO_FRAME *f = find_frame( s, tag );
	if( f ) {
//DBGQ serprintf(" UNLOCK( %08X %2d ) ", tag, f->index );
DBGQ serprintf("UNL[%2d] ", f->index );
		f->locked = 0;
	}
	return f;
}

// *****************************************************************************
//
//	_query_sink_frames
//
// *****************************************************************************
static void _query_sink_frames( STREAM *s )
{
	s->num_frames = 0;
	while( s->num_frames < STREAM_MAX_FRAMES ) {
		s->frames[s->num_frames] = s->video_sink->get_frame( s->video_sink, s->num_frames );
		if( !s->frames[s->num_frames] )
			break;
		s->num_frames++;	
	}
DBGS serprintf("got %d frames from sink!\n", s->num_frames );
}

// *****************************************************************************
//
//	_queue_sink_frames
//
// *****************************************************************************
static void _queue_sink_frames( STREAM *s )
{
	// try to get all the frame from the sink and queue them
	VIDEO_FRAME *frame;
	while ( !s->video_sink->get( s->video_sink, &frame ) ) {
		s->video_sink_count--;
		
		// got a frame, queue it
		if( s->vtime_post_sink && frame->time != -1 ) {
			if( frame->epoch == s->seek_epoch ) {
				s->video_time = frame->time;
			}
		}

		s->video_drop   = (frame->blit_time == -1);
		if( s->video_sink->put_time ) {
			frames_dropped += s->video_drop;
		}
		frame->time = -1;
		if( frame->locked ) {
DBGQ serprintf("LCQ[%2d<", frame->index );
			frame_q_put( &s->locked_q, frame );
DBGQ serprintf(">%2d] ", frame_q_count( &s->locked_q ) );
		} else {
DBGQ serprintf("PUT[%2d<", frame->index );
			frame_q_put( &s->decode_q, frame );
DBGQ serprintf(">%2d] ", frame_q_count( &s->decode_q ) );
		}
	}
	// are there frames that became unlocked?
	while( (frame = frame_q_get_unlocked( &s->locked_q )) ) {
DBGQ serprintf("L2D[%2d<", frame->index );
		frame_q_put( &s->decode_q, frame );
DBGQ serprintf(">%2d] ", frame_q_count( &s->decode_q ) );
	}
}

// *****************************************************************************
//
//	_free_all_frames
//
// *****************************************************************************
static void _free_all_frames( STREAM *s )
{
DBGV serprintf("_free_all_frames\r\n");
	// we do not hold any more frames
	s->decode_frame      = NULL;
	s->current_frame     = NULL;
	
	// flush the display queue
	frame_q_flush( &s->disp_q );
	
	// flush the decode queue and put all frames back in
	frame_q_flush( &s->decode_q );
	
	// flush the locked queue
	frame_q_flush( &s->locked_q );
	
	// flush the codec queue
	frame_q_flush( &s->codec_q );

	int i;
	for( i = 0; i < s->num_frames; i++ ) {
		if( s->frames[i] )
			s->frames[i]->locked = 0;
	}
	
	if( s->video->valid && s->use_sink_frames ) {
		pthread_mutex_lock( &s->video_sink_mutex );
		_query_sink_frames( s );
		s->video_sink->flush( s->video_sink );
		_queue_sink_frames( s );
		s->video_sink_count = 0;
		pthread_mutex_unlock( &s->video_sink_mutex );
	} else {
		int i;
		for( i = 0; i < s->num_frames; i++ ) {
			// but why should this frame not exist?
			if( s->frames[i] ) {
				// invalid and unlocked!
				s->frames[i]->time   = -1;
				s->frames[i]->locked = 0;

DBGQ serprintf("PUT[%2d<", s->frames[i]->index );
				frame_q_put( &s->decode_q, s->frames[i] );
DBGQ serprintf(">%2d] ", frame_q_count( &s->decode_q ) );
			}
		}
	}
}

enum {
	VID_CALL_DECODER = 0,
	VID_WAITING_FOR_DEC_RSP,
};

UNUSED static void _get_sink_props( STREAM *s, int *width, int *height, int *num_frames ) 
{
	int sink_frames = stream_sink_video_frames;
	if( s->use_sink_frames ) {
		*num_frames = 0;
		*width      = s->video->width;
		*height     = s->video->height;
	} else {
		*num_frames = sink_frames;
		*width      = 1280;
		*height     = 720;
	}
}

static void _fake_chapters( STREAM *s )
{
	if( !s->duration )
		return;
	int i;
	#define NUM_CH 10
	for( i = 0; i < NUM_CH; i++ ) {
		char title[32];
		sprintf( title, "Chapter %d\r\n", i + 1 );
		stream_add_chapter( s, s->duration * i / NUM_CH, s->duration * (i + 1) / NUM_CH, title );
	}
}

// ************************************************************
//
//	_get_file_parser
//
// ************************************************************
static STREAM_PARSER *_get_file_parser( STREAM *s, int etype, const char *path ) 
{
	STREAM_PARSER *parser;

	// check first for etype from io
	int io_etype = stream_get_io_etype( path );
	if( io_etype && ( parser = stream_get_parser( io_etype ) ) ) {
		s->etype = io_etype;
		return parser;
	}
	
	if( ( parser = stream_get_parser( etype ) ) ) {
		s->etype = etype;
		return parser;
	} 
serprintf("no parser found for etype %d\r\n", etype);
	
serprintf("no parser found for io_etype %d\r\n", io_etype);
	
	stream_set_error( s, VE_ERROR );
	s->video_error_qualifier = 0;
	return NULL;
}

static void check_resize( STREAM *s )
{
	if (!(s->flags & STREAM_RESIZE_BUFFER) || !stream_buffer_large ) {
		return;
	}
	if( s->buffer2 ) {
		// we have a 2nd buffer, can't resize for now
		return;
	}
	if( stream_parser_audio_chunk_num( s ) || stream_parser_video_chunk_num( s ) || stream_parser_subtitle_chunk_num( s ) ) {
		// we already have chunks, don't resize;
		return;
	}
	if( s->size < 256 * 1024 * 1024 ) {
		// smaller than 256 MB, don't bother to resize
		return;
	}
	
serprintf("\n");
serprintf("RESIZE BUFFER!\n");
serprintf("\n");

	// resize, we can ignore if that fails, since in that case we just use the old buffer..
	int new_size = stream_buffer_large * 1024 * 1024;

	stream_buffer_resize_and_rebuffer( s->buffer, new_size );
}

// *****************************************************************************
//
//	stream_open
//
// *****************************************************************************
int stream_open(STREAM     *s, 
		STREAM_URL *src,
		int        etype,
		int        _flags ) 
{
	if( !s )
		return 1;
serprintf("\r\nstream_open %s  etype %d->%s  flags %02X\r\n",  src->url, etype, av_get_etype_name(etype), _flags );	

	if( stream_force_error ) {
		stream_force_error = 0;
serprintf("stream force error!\r\n");
		stream_set_error( s, VE_ERROR );
		goto ErrorExit;
	}
	
	if( stream_force_abort ) {
		stream_force_abort = 0;
serprintf("stream force abort!\r\n");
		stream_set_error( s, VE_USER_ABORT );
		goto ErrorExit;
	}

	STREAM_PARSER *_parser = _get_file_parser( s, etype, src->url );
	if( !_parser ) {
		// no parser, no movie :-(
serprintf("no parser found\r\n");
		stream_set_error( s, VE_ERROR );
		goto ErrorExit;
	}

	// init stream
	if( stream_init( s ) ) {
serprintf("error in stream_init\r\n");
		stream_set_error( s, VE_FILE_ERROR );
		goto ErrorExit;
	}

	s->open = 1;

	if( src )
		stream_url_cpy( &s->src, src );
	else
		stream_url_cpy_url( &s->src, "" );

	int    idx_size = 0; 
	UCHAR *idx_data = NULL;
	if( !idx_size || !idx_data ) {
		s->idx_size = 0;
	} else {
		// if we have index data do not read the index!
		_flags |= STREAM_NO_INDEX;	
		// copy index data
		s->idx_size = MIN( sizeof(s->idx_data), idx_size);
		memcpy( s->idx_data, idx_data, s->idx_size ); 
	}	 	
	
	if( stream_no_audio || stream_fps_mode ) {
		_flags |= STREAM_NO_AUDIO;
	}
	if( stream_no_video ) {
		_flags |= STREAM_NO_VIDEO;
	}
	if( stream_no_subtitles ) {
		_flags |= STREAM_NO_SUBTITLES;
	}
	if( stream_force_nonlocal ) {
		_flags |= STREAM_FILE_NONLOCAL;
	}
	
	if( _flags & ( STREAM_THUMB | STREAM_THUMB_PLAY ) ) {
		// we want to get or play a thumbnail!
		_flags |= STREAM_PAUSED;
		_flags |= STREAM_NO_AUDIO;
		_flags |= STREAM_NO_SUBTITLES;
	} else {
		if( !stream_no_late_idx )
			_flags |= STREAM_LATE_INDEX;
	}	
	
	stream_show_flags( _flags );
	s->flags = _flags;
	
	if( s->start_time >= s->stop_time ) {
		s->stop_time = 0;
	}
	if( s->progress )
		s->do_progress = 1;

	s->parser        = _parser;
	s->sync_mode     = STREAM_SYNC_SAMPLES;

	// did we get no custom max dimensions?
	if( !s->video_max_width ) {
		s->video_max_width = MAX_VIDEO_FRAME_WIDTH;
	}
	if( !s->video_max_height ) {
		s->video_max_height = MAX_VIDEO_FRAME_HEIGHT;
	}
	
	if( !s->audio_max_channels ) {
		s->audio_max_channels = stream_audio_max_channels; // 2 unless changed
	}

	if( stream_force_max_width ) {
		s->video_max_width = stream_force_max_width;
	}
	if( stream_force_max_height ) {
		s->video_max_height = stream_force_max_height;
	}
	
	if( stream_force_audio_filter_level != -1 ) {
		stream_set_audio_filter_level( s, stream_force_audio_filter_level , stream_force_audio_filter_night_on);
	}
	
	// check if the url query is for us directly
	char *query;
	if( (query = strstr( s->src.url, "?mpegts&") ) ) {
serprintf("src_query: %s\n", query );
		strnZcpy(s->src_query, query, STREAM_MAX_PATH_LEN);
		*query = '\0';
	}
	
	// check if this io is nonlocal
	if( stream_get_io_nonlocal( s->src.url ) ) {
serprintf("stream_io_nonlocal!\r\n"); 
		s->flags |= STREAM_FILE_NONLOCAL;
	}
	
	// determine buffer size
	if( !s->buffer_size ) {
		// use more for normal video
		s->buffer_size = stream_buffer_size;
	}

	if( stream_force_buffer ) {
		s->buffer_size = stream_force_buffer;
	}

DBGS serprintf("opening parser: [%s] with %d MB\r\n", s->parser->name, s->buffer_size );
	// open the parser
	if ( s->parser_open ) {
		// parser is already open, so it does not
		// belong to us, we will also not close it!
		s->parser_extern = 1;
	} else {
		int flags = 0;
		if( s->flags & STREAM_FILE_NONLOCAL ) {
			flags |= STREAM_PARSER_FILE_NONLOCAL;
		}
		if( s->flags & STREAM_TIMESHIFT ){
			flags |= STREAM_PARSER_TIMESHIFT;
		}
		if( s->flags & STREAM_MPEG_SKIP_PSI_PREPARSE ){
			flags |= STREAM_PARSER_MPEG_SKIP_PSI_PREPARSE;
		}
		
		s->num_parts = stream_check_parts( s->src.url );
		if( s->num_parts > 1 ) {
			stream_parse_parts( s );
		}
		s->abort = s->user_abort;
		if( s->parser->open( s, s->buffer_size * 1024 * 1024, flags ) ) {
serprintf("error opening parser: %d\r\n", s->video_error );
			if ( !s->video_error ) {
				stream_set_error( s, VE_FILE_ERROR );
			}
			goto ErrorExit;
		}
	}

	// allow to override the sync mode
	if( stream_sync_mode != -1 ) {
		s->sync_mode = stream_sync_mode;
	}
	
	// make sure we have either video or audio
	if( !s->video->valid && !s->audio->valid ) {
serprintf("no video or audio !r\n");
		if ( !s->video_error ) {
			stream_set_error( s, VE_FILE_ERROR );
		}
		goto ErrorExit;
	}

	if( !s->duration && s->video->valid ) {
		s->duration = s->video->duration;
	}
	if( !s->duration && s->audio->valid ) {
		s->duration = s->audio->duration;
	}
	
	if( !s->num_chapters && stream_fake_chapters ) { 
		_fake_chapters( s );
	}
	
	if( s->flags & ( STREAM_THUMB | STREAM_THUMB_PLAY ) ) {
		// for thumbs, if there is no or crypted video fail!
		if( !s->video->valid ) {
serprintf("thumb no video!r\n");
			stream_set_error( s, VE_ERROR );
			goto ErrorExit;
		}
		if( s->video->crypted && !s->drm.bound ) {
			// DRM -> fail
serprintf("thumb crypted!r\n");
			stream_set_error( s, VE_CRYPTED );
			goto ErrorExit;
		}
	}
	
	if( s->flags & STREAM_NO_AUDIO || (stream_fake_ts_num && stream_fake_ts_den) ) {
		// no audio 
		stream_drop_audio( s );
	}
	
	if( s->flags & STREAM_NO_VIDEO ) {
		// no video 
		stream_drop_video( s );
	}

	if( s->flags & STREAM_NO_SUBTITLES ) {
		// no subs 
		stream_drop_subtitles( s );
	}

	if( s->video->valid ) {
		if( stream_video_force_reorder ) {
			s->video->reorder_pts = 1;
		}
		// do that early, we should move away from that FOURCC in the end...
		s->video->format = video_get_format_from_fourcc( s->video->fourcc, &s->video->subfmt );

		// this is ugly, I know...
		WMV_get_video_props( s->video );
		// more ugly, I know...
#ifdef CONFIG_H264
		if( s->video->fourcc == VIDEO_FOURCC_H264 && s->video->extraDataSize && s->video->extraData[0] == 1 ) {
			if( !H264_parse_avcc( s->video, NULL, NULL, &s->video->nal_unit_size ) ) {
DBGP serprintf("AVCC!\r\n" );
				s->video->avcc = 1;
				s->video->needs_header = 1;
				if( s->video->sps.width > s->video->width ) {
serprintf("AVCC: width from SPS %d > %d\n", s->video->sps.width, s->video->width );
					s->video->width = s->video->sps.width;
				}
				if( s->video->sps.height > s->video->height ) {
serprintf("AVCC: height from SPS %d > %d\n", s->video->sps.height, s->video->height );
					s->video->height = s->video->sps.height;
				}
			}
		}
		// and now with even moar ugly:
		if( s->video->fourcc == VIDEO_FOURCC_H264 && !s->video->extraDataSize && !s->video->sps.valid ) {
DBGS serprintf("H264 with NO SPS in extradata!\n");
			STREAM_CHUNK c = { 0 };
			int count = 0;
			// try up to 1s to get a video frame parsed
			while( !stream_no_parser && count++ < 100 ) {
				stream_parser_peek_video_chunk( s, &c );
				if( c.valid == CHUNK_VALID && s->buffer && s->buffer->data ) {
					H264_get_video_props( s->video, s->buffer->data + c.buf, c.size, &s->video->sps );
					break;
				} 
				s->parser->parse( s );
				msec_sleep( 10 );
			}
		}
#endif				 
#ifdef CONFIG_HEVC
		if( s->video->fourcc == VIDEO_FOURCC_HEVC && s->video->extraDataSize ) {
			// convert extradata from HVCC to annexB if needed
			if( !HEVC_convert_extradata( s->video ) ) {
DBGS serprintf("HVCC!\r\n" );
				s->video->hvcc = 1;
				//s->video->needs_header = 1;
			}
		}
#endif
#ifdef CONFIG_MPG4
		// let's pile on the ugly:
		if( s->video->format == VIDEO_FORMAT_MPG4 && !s->video->extraDataSize ) {
DBGS serprintf("MPEG4: create extradata!\n");
			int count = 0;
			// try up to 1s to get a video frame parsed
			while( !stream_no_parser && count++ < 100 ) {
				STREAM_CHUNK *c = stream_parser_peek_video_chunk( s, NULL );
				if( c && c->valid == CHUNK_VALID && s->buffer && s->buffer->data ) {
					if( !MPG4_get_extradata( s->video, s->buffer->data + c->buf, c->size )) {
DBGS serprintf("MPEG4: found extradata: %d!\n", s->video->extraDataSize);
						break;
					}
				}
				s->parser->parse( s );
				msec_sleep( 10 );
			}
			if( !s->video->extraDataSize ) {
serprintf("MPEG4: no extradata found!\n");
			}
		}
#endif
	}
DBGS {
	stream_show_props( s );
} else {
	stream_show_short_props( s );
}

	// stream has a known duration
	if( !s->no_duration ) {
		if( !s->duration ) {
serprintf("no duration!\r\n" );
			stream_set_error( s, VE_ERROR );
			goto ErrorExit;
		}
	
		// stream average data rate
		s->data_rate = (UINT64)s->size * (UINT64)1000 / (UINT64)s->duration;
	
		// if no video bytes per sec, set it here
		if( !s->video->bytesPerSec ) {
			s->video->bytesPerSec = 1000 * (UINT64)s->size / (UINT64)s->duration;

			// substract all audio rates (even with audio suppressed!)
			int a;
			for( a = 0; a < AUDIO_STREAM_MAX; a++) {
				AUDIO_PROPERTIES *audio = s->av.audio + a;
				if( audio->bytesPerSec ) {
					s->video->bytesPerSec -= audio->bytesPerSec;
				}
			}
			if( s->video->bytesPerSec < 0 ) 
				s->video->bytesPerSec = 0;
		}
	}
	if ((s->data_rate / 125) > stream_buffer_sleep_datarate && s->buffer) {
		// don't pause buffer thread for high bitrate files: avoid freeze sometimes on hdd spin up
		s->buffer->flags |= STREAM_BUFFER_NO_SLEEP;
		if( s->buffer2 ) {
			s->buffer2->flags |= STREAM_BUFFER_NO_SLEEP;
		}
	}
	
	// check video format
	if( s->video->valid ) {
		if( stream_check_video( s ) ) {
			goto ErrorExit;
		}
	}
	
	// check audio format
	if( s->audio->valid ) {
		if( stream_check_audio( s ) ) {
			goto ErrorExit;
		}
	}	

	int video_unsupported = 0;
	int audio_unsupported = 0;
	
	if( !s->video->valid ) {
		// no video -> no subtitles!
		stream_drop_subtitles( s );
	} else {
		// Initialize the video decoder    
		frames_dropped   = 0;
		frames_B_dropped = 0;
		frames_doubled   = 0;

		// go through all aspect ratios
		int i;
		for( i = 0; i < s->av.vs_max; i ++ ) { 
			// did the user force an aspect ratio?
			if( s->aspect_n && s->aspect_d ) {
				// apply forced aspect ratio
				s->av.video[i].aspect_n = s->aspect_n;
				s->av.video[i].aspect_d = s->aspect_d;
			} 
			// did we debug force an aspect ratio ?
			if( stream_force_aspect_n && stream_force_aspect_d ) {
				s->av.video[i].aspect_n = stream_force_aspect_n;
				s->av.video[i].aspect_d = stream_force_aspect_d;
			}
			// no aspect at all, force 1/1
			if( !s->av.video[i].aspect_n || !s->av.video[i].aspect_d ) {
				s->av.video[i].aspect_n = 1;
				s->av.video[i].aspect_d = 1;
			}
			// reduce the aspect ratio as much as possible
			av__reduce( &s->av.video[i].aspect_n, &s->av.video[i].aspect_d );  
		}
		
		if( stream_open_video_dec( s, &video_unsupported ) ) {
			goto ErrorExit;
		} 
serprintf("VID_DEC: [%s]\r\n", s->video_dec ? s->video_dec->name : "(none)" ); 

		// do we dump the stream?
		if( stream_dump_video ) {
			stream_dump_video = 0;

			if( !(s->video_dumper = stream_get_dumper( TYPE_VID, s->video->format ) ) ) {
				// try generic dumper
				s->video_dumper = stream_get_dumper( TYPE_VID, VIDEO_FORMAT_UNKNOWN );
			}
			if( !s->video_dumper ) {
serprintf("cannot get dumper for format %d\r\n", s->video->format );
			} else {
				// try to open the dumper
				if( s->video_dumper->open( s, 0, 0 ) ) {
serprintf("cannot open dumper!\r\n" );
					s->video_dumper = NULL;
				} 
			}
		}

		// check for external subs if we have video:
		stream_sub_ext_check( s );
	} 
	
	// Initialize the audio decoder    
	if( s->audio->valid ) {
		// pick the best audio stream
		stream_find_best_audio_stream( s );
		if(  stream_try_open_audio_dec( s, s->av.as, &audio_unsupported ) ) {
			// no audio, disable it
			stream_drop_audio( s );
		} 
serprintf("AUD_DEC: [%s]\r\n", s->audio_dec ? s->audio_dec->name : "(none)" ); 

		s->audio_stuff_zero = 1;

		stream_open_audio_filter( s );
	}

	// if we have neither audio or video we fail here
	if( s->video->valid ) {
		s->player = s->video_dec->async ? _stream_player_async : _stream_player_sync;
	} else if ( s->audio->valid ) {
		s->player = _stream_player_sync;
	} else {
serprintf("no video or audio !\r\n");
		if( video_unsupported ) {
			stream_set_error( s, VE_VIDEO_NOT_SUPPORTED );
		} else if( audio_unsupported ) {
			stream_set_error( s, VE_AUDIO_NOT_SUPPORTED );
		} else {
			stream_set_error( s, VE_ERROR );
		}
		goto ErrorExit;
	}

	// ok we made it this far, now check if we need a bigger buffer?
	check_resize( s );
	
	return 0;

ErrorExit:
	s->abort    = NULL;
	s->progress = NULL;
	stream_stop( s );

	return 1;
}

// *****************************************************************************
//
//	stream_start
//
// *****************************************************************************
int stream_start(STREAM *s) 
{
serprintf("\r\nstream_start %s\r\n", s->src.url );	
	
	// Initialize the audio sink    
	if( s->audio->valid ) {
		// no caller provided sink, use standard
		if( !s->audio_sink )
			s->audio_sink = stream_get_default_audio_sink();
			
		if( s->audio_sink ) {
serprintf("AUD_SNK: [%s]\n", s->audio_sink->name);	
			if( s->audio_sink->open( s ) ) {
serprintf("cannot open audio!\n");	
				// could not open, give up
				stream_close_audio_dec( s );
				// drop audio
				stream_drop_audio( s );
			} else {
				if( s->audio_sink->start( s ) ) {
serprintf("cannot start audio!\n");	
					// cannot start, close the codec
					stream_close_audio_dec( s );
					// drop audio
					stream_drop_audio( s );
				}
			}
		}
	}

	if ( s->audio->valid && s->video->valid ) {
		// here is the part were we define the initial audio 2 video delay
		int adec_delay    = s->audio_dec->delay( s->audio );
		int asink_delay   = s->audio_sink->delay( s );
		int vsink_delay;
		if( s->vtime_post_sink ) {
			// we sample after the sink, so the time stamps are the one we get out of the sink
			vsink_delay = 0;
		} else {
			// we sample before the sink, so the video frames have to pass through the sink
		  	vsink_delay = ( s->video_sink && s->video_sink->delay ) ? s->video_sink->delay( s->video_sink ) : 0;
 		}
 
 		s->delay_fb = 900;
DBGS serprintf("\r\nAUDIO DELAY(ms): adec %d  asink %d  vsink %d  tot %d  delay_fb %d  smode %s  vtime %s\r\n", 
				adec_delay, asink_delay, vsink_delay, stream_sync_av_delay( s ), s->delay_fb,
				s->sync_mode == STREAM_SYNC_SAMPLES ? "SAMPLES" : "CDATA",
				s->vtime_post_sink ? "POST" : "PRE" ); 		
	}

	if( s->sync_mode == STREAM_SYNC_SAMPLES ) {
		// resync audio time!
		s->audio_time     = -1;
		s->audio_ref_time = -1;
	}
	
	if( _allocate_buffers( s ) ) {
serprintf("could not allocate buffers!r\n");
		stream_set_error( s, VE_ERROR );
		goto ErrorExit;
	}

	// open the drm context
	if( s->drm_ctx.open ) {
		if( s->drm_ctx.open( s->drm_ctx.handle ) ) {
serprintf("could not open drm_ctx!r\n");
			stream_set_error( s, VE_ERROR );
			goto ErrorExit;
		}
	}

	frame_q_init( &s->disp_q,   "disp" );
	frame_q_init( &s->decode_q, "dec" );
	frame_q_init( &s->locked_q, "lock" );
	frame_q_init( &s->codec_q,  "codec" );
	
	_video_init( s, 0 );
	
	s->video_state = VID_CALL_DECODER;
	
	// pause the video
	stream_pause( s );

	// lock and start the engine thread
	thread_state_init( &s->engine_tstate, THREAD_IDLE, "eng" );
	thread_create( &s->engine_thread_handle, _player_thread, (void*)s, stream_prio_engine, "video player engine");
	
	// lock and start the parser thread
	thread_state_init( &s->parser_tstate, THREAD_IDLE, "par" );
	thread_create( &s->parser_thread_handle, _parser_thread, (void*)s, stream_prio_parser, "video player parser" );
	
	// lock and start the subtitle_thread
	thread_state_init( &s->sub_tstate, THREAD_IDLE, "sub" );
	thread_create( &s->sub_thread_handle, stream_sub_dec_thread, (void*)s, stream_prio_sub, "video player subtitle" );
	
	if( s->audio->valid ) {
		// lock and start the audio_thread
		thread_state_init( &s->audio_tstate, THREAD_IDLE, "aud" );
		thread_create( &s->audio_thread_handle, stream_audio_dec_thread, (void*)s, stream_prio_audio, "video player audio" );
	}
	
	// lock and start the video_thread
	pthread_cond_init(&s->codec_code, 0);
	pthread_cond_init(&s->video_done, 0);
	pthread_mutex_lock( &s->codec_mutex );
	s->codec_run = 1;
	thread_create( &s->codec_thread_handle, _decode_thread, (void*)s, stream_prio_video, "video player decoder");
	
	// allow the video playing
DBGV serprintf("GO_VID\r\n");				
	
	s->fps_start = atime();
	s->fps_count = 0;
	
	pthread_mutex_unlock( &s->codec_mutex );

	thread_state_set( &s->parser_tstate, THREAD_RUNNING );
	thread_state_set( &s->engine_tstate, THREAD_RUNNING );
	thread_state_set( &s->sub_tstate,    THREAD_RUNNING );
	
	// the caller supplied his own start_time, but we have to convert that to a real time
	if( s->start_time ) {
		s->start_time = _stream_get_real_time( s, s->start_time );
serprintf("start real %d \r\n", s->start_time );
	}
	if( s->stop_time ) {
		s->stop_time = _stream_get_real_time( s, s->stop_time  );
serprintf("stop  real %d \r\n", s->stop_time );
	}
	
	if( !stream_seekable( s ) ) {
serprintf("not seekable\n");
		s->start_pos = s->start_time = 0;
	}
	
	if( s->no_duration ) {
serprintf("start pos = time!\r\n");
		// asume that start time is actually a pos!
		s->start_pos = s->start_time;
		s->start_time = 0;
	}
	
	if ( s->video->valid ) {
		// allow video frame output
		s->video_output = 1;
		
		// if we are creating a thumb and the stream is not seekable, 
		// we need to play some frames here
		if( (s->flags & STREAM_THUMB) && !stream_seekable( s ) ) {
serprintf("thumb: play some!\r\n" );
			s->video_flush  = 1;
			_stream_play_n_frames( s, 10, -1, 0 );
		}
	}

	// have to resume to a specific position
	if( s->idx_size ) {
serprintf("start video from index is DEPRECATED, YOU LOSE!\r\n" );
		stream_set_error( s, VE_ERROR );
		goto ErrorExit;
	} else if ( s->start_time ) {
serprintf("start video at time %d \r\n", s->start_time );
		_stream_seek_real( s, s->start_time, -1, STREAM_SEEK_BACKWARD, STREAM_SEEK_STRICT, 0 );
	} else if ( s->start_pos ) {
serprintf("start video at pos %d \r\n", s->start_pos );
		_stream_seek_real( s, -1, s->start_pos, STREAM_SEEK_FORWARD, STREAM_SEEK_STRICT, 0 );
	} else {
serprintf("start video at start\r\n");
		// init the AV sync machinery to have a clean start of the video
		stream_sync_init( s, -1 );
	}

	if( s->flags & STREAM_THUMB_PLAY ) {
		if( s->buffer && s->size > ((UINT64)s->buffer_size * (UINT64)1024 * (UINT64)1024) ) { 
serprintf("no wrap!\r\n");
			s->buffer->flags |= STREAM_BUFFER_NO_WRAP;
		}
	}	

	s->do_progress = 0;

	// unpause if we have audio
	if ( s->audio->valid == 1 ) {
		thread_state_set( &s->audio_tstate, THREAD_RUNNING );
	} 

	if( !(s->flags & STREAM_PAUSED) ) {
		stream_un_pause( s, 0 );
	}
	s->abort    = NULL;
	return 0;

ErrorExit:
	s->abort    = NULL;
	s->progress = NULL;
	stream_stop( s );
	return 1;
}

// ************************************************************
//
//	stream_resize
//
// ************************************************************
int stream_resize( STREAM *s )
{
	if( !s ) {
		return 1;
	}
DBGS serprintf("stream_resize\r\n");


	if( !s->use_sink_frames ) {
		if ( s->video->valid ) {
			s->paused_internal = !stream_pause( s );

			// wait some time for the stream engine to output all
			// frames in the pipe.
			msec_sleep( 300 );
		}
		if( s->video_sink ) {
			s->video_sink->close( s->video_sink );
			s->video_sink->open( s->video_sink, s->video, s, 0, &s->video_rc );
		}
		if ( s->video->valid ) {
			// FIXME: hangs without this
			s->current_frame = NULL;

			_stream_redraw( s );
			stream_un_pause( s, !s->paused_internal );
			s->paused_internal = 0;
		}
	} else if( s->video_sink->resize ) {
		if (s->video_sink->resize( s->video_sink, s->video ) == 1 && stream_is_paused( s ))
			_stream_redraw( s );
	}	
	

	return 0;
}

// *****************************************************************************
//
//	stream_stop
//
// *****************************************************************************
int stream_stop( STREAM *s )
{
serprintf("stream_stop\r\n" );
	if( !s || !s->open ) {
serprintf("STP: not open!\r\n");
		return 1;
	}

	stream_pause( s );

	int took = atime() - s->fps_start;

	// abort everything
	s->aborted = 1;
	
	// close the video dumper here so that we have a chance to capture a stream
	// that hangs the codec (DSP)
	if( s->video_dumper ) {
		s->video_dumper->close( s );
		s->video_dumper = NULL;
	}

	// stop all threads
stream_close( s );

	// stop audio sink
	if( s->audio_sink) {
		s->audio_sink->stop( s );
		s->audio_sink->close( s );
	}
	// stop video sink
	if( s->video_sink) {
		s->video_sink->close( s->video_sink );
		if( s->video_sink->delete ) {
			s->video_sink->delete( s->video_sink );
		}
		s->video_sink = NULL;
	}

	// stop audio decoder
	stream_close_audio_dec( s );

	stream_close_audio_filter( s );

	// stop video decoder
	stream_close_video_dec( s );

	// close the subtitle decoder
	stream_close_sub_dec( s );

	// close the drm context
	if( s->drm_ctx.close ) {
		s->drm_ctx.close( s->drm_ctx.handle );
	}

	_free_buffers( s );

	afree(s->mangler_priv);
	s->mangler_priv = NULL;
	
	// close all ext subs
	stream_sub_ext_close( s );
	
	if( s->video->valid ) {
serprintf("DROPPED: %d  B_DROPPED %d  DOUBLED %d \r\n", frames_dropped, frames_B_dropped, frames_doubled );
		if( stream_fps_mode ) {
serprintf("took %d  frames %d  FPS %f\n", took, s->fps_count, (float)s->fps_count * 1000 / took );
		}
	}
	
	return 0;
}

// ************************************************************
//
//	_stream_resync
//
// ************************************************************
void _stream_resync( STREAM *s ) 
{
	s->sink_ref_time = -1;
	stream_sync_restart( s );
}

// ************************************************************
//
//	stream_audio_mute
//
// ************************************************************
void stream_audio_mute( STREAM *s )
{
	if( s->audio->valid && s->audio_sink ) {
		s->audio_sink->mute( s, 1 );
		s->muted = 1;
	}
}

// ************************************************************
//
//	stream_audio_unmute
//
// ************************************************************
void stream_audio_unmute( STREAM *s )
{
	if( s->audio->valid && s->audio_sink ) {
		s->audio_sink->mute( s, 0 );
		s->muted = 0;
	}
}

// ************************************************************
//
//	stream_audio_is_muted
//
//	returns audio mute status:
//	returns: 1 if stream audio is muted
//	         0 if stream audio is unmuted
//
// ************************************************************
int stream_audio_is_muted( STREAM *s )
{
	if( !s || !s->open ) {
serprintf("MUT: not_open\r\n");
		return 0;
	}

	return s->muted;
}

// ************************************************************
//
//	stream_pause
//
// ************************************************************
int stream_pause( STREAM *s )
{
	int was_paused;

	if( !s || !s->open ) {
serprintf("PAU: not_open\r\n");
		return 0;
	}

	was_paused = s->paused;

	if ( !was_paused ) {
DBGS serprintf("stream_pause\r\n");
		if ( s->parser && s->parser->pause ) {
			s->parser->pause( s, 1 );
		}

		stream_audio_mute( s );

		s->paused = 1;
	}

	_stream_wait_for_idle( s, 1000 );

	return was_paused;
}

// ************************************************************
//
//	stream_un_pause
//
// ************************************************************
void stream_un_pause( STREAM *s, int was_paused )
{
	if( !s || !s->open ) {
serprintf("UNP: not_open\r\n");
		return;
	}
	if ( !was_paused ) {
DBGS serprintf("stream_un_pause\r\n");

		_stream_resync( s );

		// when we unpause, we re-fill the audio sink with 0 samples
		// so that we are back to the same a2v sync as before
		if ( stream_zero_fill && s->audio->valid && s->speed == STREAM_SPEED_NORMAL ) {
			s->audio_preload = 1;
		} else {
			s->audio_preload = 0;
			s->audio_stuff_zero = 0;
		}

		s->paused = 0;

		if ( s->speed == STREAM_SPEED_NORMAL ) {
			stream_audio_unmute( s );
		}

		if ( s->parser && s->parser->pause ) {
			s->parser->pause( s, 0 );
		}
	}
}

void stream_un_pause_from_jni( STREAM *s, int was_paused )
{
	if( ignore_first_unpause ) {
DBGS serprintf("ignore_first_unpause!!\n");
		ignore_first_unpause = 0;
		return;
	}
	stream_un_pause( s, was_paused );
}

// ************************************************************
//
//	stream_is_paused
//
//	returns pause status:
//	returns: 1 if stream is paused
//	         0 if stream is playing
//
// ************************************************************
int stream_is_paused( STREAM *s )
{
	if( !s || !s->open ) {
serprintf("PAU: not_open\r\n");
		return 0;
	}

	return s->paused && !s->paused_internal;
}

// ************************************************************
//
//	_real_time
//
// ************************************************************
static int _real_time( STREAM *s, int frame_time )
{
	int mul = 1;
	int div = 1;
	
	switch( s->speed ) {
		case STREAM_SPEED_NORMAL:
			return s->vid_ref_time + (frame_time - s->vid_ref_time);
			
		case STREAM_SPEED_SLOW_2: mul = 2; break;
		case STREAM_SPEED_SLOW_4: mul = 4; break;
		case STREAM_SPEED_SLOW_8: mul = 8; break;
		
		case STREAM_SPEED_FAST_2: div = 2; break;
		case STREAM_SPEED_FAST_4: div = 4; break;
		case STREAM_SPEED_FAST_8: div = 8; break;
	}

	return s->vid_ref_time + (frame_time - s->vid_ref_time) * mul / div;
}

#if 1
static int __last_wait;
static int __wait;
#define WAIT_INIT(a ) { __wait = time_update_time(); serprintf("[%s %2d]", a, (__wait - __last_wait)/10 );__last_wait = __wait; }
#define WAIT( a )   { int _t = time_update_time(); serprintf("[%s %2d]", a, (_t - __wait)/10 ); }
#else
#define WAIT_INIT();
#define WAIT( a );
#endif

// ************************************************************
//
//	_engine_abort
//
// ************************************************************
static int _engine_abort( STREAM *s )
{
	if( thread_state_asked( &s->engine_tstate ) == THREAD_RUNNING )
		return 0;
serprintf("_engine_abort!\r\n");		
	return 1;
}

// ************************************************************
//
//	_check_sink_ref_time
//
// ************************************************************
static void _check_sink_ref_time( STREAM *s, VIDEO_FRAME *frame )
{
	if( s->sink_ref_time == -1 ) {			
		if( s->video_sink->put_time ) {
			s->sink_ref_time = frame->time; 

			if( s->audio->valid ) {
				// set the time to 0 to prevent video from playing and let the audio thread update it
				s->video_sink->put_time(s->video_sink, 1 ); 
			}else {
				s->video_sink->put_time(s->video_sink, frame->time ); 
			}
serprintf("  <NSR %d>", frame->time );
		} else {
			s->vid_ref_time = frame->time;
			int reftime = s->video_sink->get_time( s->video_sink );
			s->sink_ref_time = reftime - s->vid_ref_time;
serprintf("  <NSR %d/%d>", frame->time, reftime );
		}
	}
}

// ************************************************************
//
//	_put_frame_in_sink
//
// ************************************************************
static void _put_frame_in_sink( STREAM *s, VIDEO_FRAME *frame, int time )
{
	if( s->video_sink->put_time ) {
		frame->blit_time = _real_time( s, time );
	} else {
		// we add "stream_sink_preroll" here because the sink might switch to it's next frame
		// while we do the call!
		frame->blit_time = _real_time( s, time ) + s->sink_ref_time + stream_sink_preroll;
	}
//serprintf("real %8d  ref %8d  blit %8d\n", _real_time( s, time ), s->sink_ref_time, frame->blit_time );
	frame->time = time;
	// a sink might want that info
	frame->aspect_n = s->video->aspect_n,
	frame->aspect_d = s->video->aspect_d;
	frame->duration = s->video->msPerFrame;
				
	pthread_mutex_lock( &s->video_sink_mutex );
	s->sink_delay = frame->blit_time - s->video_sink->put( s->video_sink, frame ); 	
	s->video_sink_count ++;
	pthread_mutex_unlock( &s->video_sink_mutex );
DBGQ serprintf("OUT[%2d|%2d] ", frame->index, frame_q_count( &s->decode_q ) );
	if( s->play_n_video_one ) {
		s->play_n_video_frames = 0;
		s->play_n_video_one    = 0;
	}
}

// ************************************************************
//
//	_check_sink_delay
//
// ************************************************************
static void _check_sink_delay( STREAM *s )
{
	if( s->video_sink->put_time ) {
		return;
	}
	static int vt = 0;
	int at = atime();
DBGV2 serprintf("  d %3d|%3d(%2d)", s->sink_delay, at - vt, s->video_sink_count );
	vt = at;
				
	if( s->sink_delay < 0 ) {
		s->sink_delay_count ++;
		if( s->sink_delay_count > 2 || s->sink_delay < (-1 * stream_sink_max_delay) ) {
			s->sink_ref_time = -1;
			s->sink_delay_count = 0;
		}
	} else {
		s->sink_delay_count = 0;
	}
}

// ************************************************************
//
//	_output_frame_no_resize - kilroy was here
//
// ************************************************************
static void _output_frame_no_resize( STREAM *s, VIDEO_FRAME *frame, VIDEO_FRAME **qframe )
{
	if( !frame || !frame->valid || !s->video_output || frame->time == -1 ) {
		goto Discard;
	}
	
	while( qframe && !_engine_abort( s ) && stream_sync_video( s, frame->time ) ) {
serprintf("#");
		msec_sleep( 10 );
	}
		
DBGV2 serprintf("  out %8d/%2d/%c", frame->time, frame->index, frame_type( frame->type ) );
	if( s->video_sink ) {
DBGV1 WAIT("OF")
		if( stream_fps_mode ) {
			s->fps_count ++;
			if( stream_fps_mode == 2 ) {
				goto Discard;
			} 
			frame->blit_time = s->video_sink->get_time( s->video_sink );
			pthread_mutex_lock( &s->video_sink_mutex );
			s->video_sink->put( s->video_sink, frame );
			s->video_sink_count ++;
			pthread_mutex_unlock( &s->video_sink_mutex );
			
			s->current_frame     = frame;
			s->current_out_frame = frame;
DBGQ serprintf("OUT[%2d|%2d] ", frame->index, frame_q_count( &s->decode_q ) );
			if( qframe ) {
				// we gave this frame to the sink!
				*qframe = NULL;
			}
		} else {
			_check_sink_ref_time( s, frame ); 

			if( s->drop > 0 ) {
				// drop one frame
				s->drop --;
				s->sink_ref_time -= s->video->msPerFrame;
				frames_dropped ++;
DBGY serprintf("[-%8d] ", frame->time );
				s->drop_count ++;
				if( s->vtime_post_sink ) {
					s->video_time += s->video->msPerFrame;
				}
			} else if( s->drop < 0 ) {
				// double one frame
				s->drop ++;
				s->sink_ref_time += s->video->msPerFrame;
				frames_doubled ++;
DBGY serprintf("[+%8d] ", frame->time );
				if( s->vtime_post_sink ) {
					s->video_time -= s->video->msPerFrame;
				}
			} else {	
DBGY serprintf("[ %8d] ", frame->time );
				s->drop_count = 0;
			}

			_put_frame_in_sink( s, frame, frame->time );
			
			if( qframe ) {
				// we gave this frame to the sink!
				*qframe = NULL;
			}
			
			_check_sink_delay( s );

			s->current_frame     = frame;
			s->current_out_frame = frame;
DBGV1 WAIT("P")
		}
	}
	
	if( (s->flags & STREAM_THUMB) && s->play_n_video_frames ) {
		// hack: in THUMB mode, we stop after we put the 1st frame to the sink!
		s->play_n_video_frames = 0;
	}

Discard:	
	if( frame && qframe && *qframe ) {
		// put this frame back into the queue
DBGQ serprintf("DIS[%2d<", frame->index );
		frame_q_put( &s->decode_q, frame );
DBGQ serprintf(">%2d]", frame_q_count( &s->decode_q ) );
		*qframe = NULL;
	}
}

void stream_audio_debug( STREAM *s, int samples, int decoded, int time )
{
	do_avg_audio( &aud_avg, time, samples, s->audio );
	t_adecode       += time;
	t_adecode_bytes += decoded;
}

// ************************************************************
//
//	_video_decode
//
// ************************************************************
static void _video_decode( STREAM *s )
{
	int decoded = 0;
	int time    = 0;

	if( s->video_dec ) {
		if( s->video_flush ) {
			s->video_flush = 0;

			// tell the decoder we want to flush it
			s->video_dec->flush( s->video_dec );

			if ( s->video->flush_frames ) {
DBGS serprintf("video_flush: %d\r\n", s->video->flush_frames );
				// do not let the decoder mess with this frames time while we flush!
				int frame_time = s->vcodec.decode_frame ? s->vcodec.decode_frame->time: -1;
				// flush the decoder by decoding the frame multiple times to flush out all old frames in the queue
				int max = (s->video->flush_frames == -1) ? 20 : s->video->flush_frames;
				int i;
				for( i = 0; i < max; i++ ) {
					if( s->video_dec && s->vcodec.decode_frame ) {
						int _decoded;
						int dummy;
						VIDEO_FRAME *in_frame = s->vcodec.decode_frame;
						if( s->video_dec->decode2 ) {
							s->video_dec->decode2( s->video_dec, s->vcodec.data, s->vcodec.data_size, &in_frame, &s->vcodec.decode_frame, &_decoded, &dummy );
							if( in_frame ) {
serprintf("use(%d)", in_frame->index);
								frame_q_put_head( &s->decode_q, in_frame );
								decoded = _decoded;
								thread_state_set( &s->parser_tstate,  THREAD_RUNNING );
								s->seek_frame = 1;
								goto GOT_NO_FRAME;
							}
						} else {
							s->video_dec->decode( s->video_dec, s->vcodec.data, s->vcodec.data_size, &s->vcodec.decode_frame, &_decoded, &dummy );
						}
					}
					if( !s->vcodec.decode_frame ) {
						// if the decoder locked this frame, we need a new one to be able to go on!
						s->vcodec.decode_frame = frame_q_get( &s->decode_q );
DBGQ serprintf("FLU[%2d|%2d] ", s->vcodec.decode_frame ? s->vcodec.decode_frame->index : -1, frame_q_count( &s->decode_q ) );
					}
					if( s->vcodec.decode_frame ) {
						if( s->video->flush_frames == -1 && s->vcodec.decode_frame->time == frame_time ) {
							// we got the right frame, stop!
							goto GOT_FRAME;
						}
						s->vcodec.decode_frame->time = frame_time;
					} else {
serprintf("flush: no decode frame\r\n");					
					}
				}
			}
		}
		s->vcodec.decode_frame->error       = 0;
		s->vcodec.decode_frame->interlaced  = 0; 
		s->vcodec.decode_frame->decode_time = 0;
	
		if( stream_fake_video == 1 ) { 
			s->vcodec.decode_frame->valid = 1;
			decoded = s->vcodec.data_size;
		} else {		
			VIDEO_FRAME *in_frame = s->vcodec.decode_frame;
			int ret;
			if( s->video_dec->decode2 ) {
				ret = s->video_dec->decode2( s->video_dec, s->vcodec.data, s->vcodec.data_size, &in_frame, &s->vcodec.decode_frame, &decoded, &time );
				if( in_frame ) {
//serprintf("use(%d)", in_frame->index);
					frame_q_put_head( &s->decode_q, in_frame );
				}
			} else { 
				ret = s->video_dec->decode( s->video_dec, s->vcodec.data, s->vcodec.data_size, &s->vcodec.decode_frame, &decoded, &time );
			}
			if( ret ) {
				// error!
serprintf("video_decode_error(%d)!\r\n", decoded);
				// signal error in case the codec did not
				if( s->vcodec.decode_frame )
					s->vcodec.decode_frame->error = 1;
			}
		}
		
		s->vcodec.time = time;
		if( stream_vcodec_delay ) {
			int delay = atime();
			msec_sleep( stream_vcodec_delay );
			s->vcodec.time += 10 * (atime() - delay);
		}
GOT_NO_FRAME:
		if( s->vcodec.decode_frame ) {
GOT_FRAME:
			s->vcodec.decode_frame->decode_time = time;

			// check if the size changed!
			if( s->vcodec.decode_frame->valid && (s->vcodec.decode_frame->width      != s->video->width 
			                                   || s->vcodec.decode_frame->height     != s->video->height
			                                   || s->vcodec.decode_frame->interlaced != s->video->interlaced ) ) {
DBGS serprintf("VIDEO SIZE CHANGED! %dx%d|%d -> %dx%d|%d\r\n", s->video->width, s->video->height, s->video->interlaced,
							s->vcodec.decode_frame->width,s->vcodec.decode_frame->height, s->vcodec.decode_frame->interlaced  );
				s->video->width      = s->vcodec.decode_frame->width;
				s->video->height     = s->vcodec.decode_frame->height;
				s->video->interlaced = s->vcodec.decode_frame->interlaced;
				if( s->message_cb ) {
					s->message_cb( s, STREAM_VIDEO_PROPS_CHANGED );
				}
				if( s->video_sink->resize ) {
					s->video_sink->resize( s->video_sink, s->video );
				}
			}

			if( stream_screen_shot ) {
				av_dump_video_frame( s->vcodec.decode_frame );
				stream_screen_shot = 0;
			}
		}
	} else {
		VIDEO_FRAME *frame = s->vcodec.decode_frame;
		// no decoder, pass through the data:
		memcpy( frame->data[0], s->vcodec.data, s->vcodec.data_size );
		frame->size = s->vcodec.data_size;
		 
		frame->width	       = 0;
		frame->height	       = 0;
		frame->linestep[0]        = 0;
		frame->error	       = 0;
		frame->interlaced      = 0;
		frame->top_field_first = 0;
		frame->pts	       = 0;
		frame->cpn	       = 0;
		frame->dpn	       = 0;
	}
	s->vcodec.decoded = decoded;

}

// ************************************************************
//
//	_decode_thread
//
// ************************************************************
static void *_decode_thread( void *data )
{
	STREAM *s = (STREAM *)data;
DBGS serprintf("PID[%5d] decode_thread::Starting\r\n", getpid() );	
	
	while( s->codec_run ) {
		pthread_mutex_lock( &s->codec_mutex );
		
		while( s->vcodec.done == 1 ) {
DBGV1 WAIT("DS");
			_video_decode( s );
			s->vcodec.data = NULL;
DBGV1 WAIT("DE");
			pthread_mutex_lock(&s->video_done_mutex);
			s->vcodec.done = 2;
			pthread_cond_signal(&s->video_done);
//DBGV1 WAIT("DE1");
			pthread_mutex_unlock(&s->video_done_mutex);
//DBGV1 WAIT("DE");
			sched_yield();
		}
				
		pthread_cond_wait(&s->codec_code, &s->codec_mutex);
		
		pthread_mutex_unlock( &s->codec_mutex );
	}
DBGS serprintf("PID[%5d] decode_thread::Exiting\r\n", getpid() );	
	return NULL;
}

// ************************************************************
//
//	_parser_thread
//
// ************************************************************
static void *_parser_thread( void *data )
{
	STREAM *s = (STREAM *)data;
	int yield = 1;
	int last_time = 0;
DBGS serprintf("PID[%5d] stream_parser_thread::Starting\r\n", getpid() );	
	
	while( thread_state_get( &s->parser_tstate ) != THREAD_EXIT ) {
		thread_state_ack( &s->parser_tstate );
		yield = 1;
		if( thread_state_get( &s->parser_tstate ) == THREAD_RUNNING ) {
			// call parser if needed
			if ( !s->paused || s->play_n_video_frames || s->play_n_audio_frames || s->parser_parse_once ) {
				if( s->parser_parse_once )
					s->parser_parse_once--;
					
				if( !stream_no_parser ) {
					s->parser->parse( s );

					if( atime() > last_time + 100 ) {
						last_time = atime();
						if( s->parser->calc_rate ) {
							s->parser->calc_rate( s );
						}
					}
					if( stream_parser_sleep ) {
						msec_sleep( stream_parser_sleep );
					}
				}
			}
		}
		
		if( yield ) {
			stream_yield_RT();
		} 
	}
DBGS serprintf("PID[%5d] stream_parser_thread::Exiting\r\n", getpid() );	
	return NULL;
}

// ************************************************************
//
//	_player_thread
//
// ************************************************************
static void *_player_thread( void *data )
{
	STREAM *s = (STREAM *)data;
DBGS serprintf("PID[%5d] stream_player_thread::Starting\r\n", getpid() );	
	
	while( thread_state_get( &s->engine_tstate ) != THREAD_EXIT ) {
		thread_state_ack( &s->engine_tstate );
		s->engine_yield = 1;
		if( thread_state_get( &s->engine_tstate ) == THREAD_RUNNING ) {
			if( s->player && s->video_error == VE_NO_ERROR ) {
				s->player( s );
			}
		}
		
		if( s->engine_yield ) {
			stream_yield_RT();
		} 
	}
DBGS serprintf("PID[%5d] stream_player_thread::Exiting\r\n", getpid() );	
	return NULL;
}

int _get_video_header_chunk( STREAM *s, CBE *cbe, STREAM_CDATA *cdata );

static int cdata_frame;
static int cdata_time;
static int cdata_time;
static int cdata_v_time;

#define POS( pos, size, max ) ((unsigned int)((UINT64)pos * (UINT64)max / (UINT64)size))
#ifdef CONFIG_TDES
extern int ext_buf_decrypt;
#endif

// ************************************************************
//
//	_print_debug
//
// ************************************************************
static void _print_debug( STREAM *s )
{
	STREAM_PARSER_STATS _st;
	STREAM_PARSER_STATS *st = s->parser->get_stats ? s->parser->get_stats( s, &_st ) : NULL;
	static int last_audio = 0;
	static int last_video = 0;

	DBGV1 {
		if( !st )
			return;	
//		serprintf(" [d %3d %3d|%3d] ", s->delay, st->audio_chunks, st->video_chunks );
	} else 
	DBGY {
		serprintf("\r\nv %7d/%4d  a %7d/%4d  ", s->video_time, s->video_time - last_video, s->audio_time - stream_sync_av_delay(s), s->audio_time - last_audio );
	} else 
	DBGV2 {
		serprintf("\r\n");
		if( !st )
			return;	
		if( st->buffer_size ) {
			serprintf("[%2lld%%] ", (UINT64)st->buffer_used * 100 / (UINT64)st->buffer_size);
			if( st->buffer2_size )
				serprintf( "[%2lld%%] ", (UINT64)st->buffer2_used * 100 / (UINT64)st->buffer2_size );
		}
		serprintf("[%6d  %8d  ", cdata_frame, cdata_time );
//		serprintf("{%02d}", cdata_v_time );
		serprintf("[%03d|%03d|%6d]  ", v_avg.time, v_avg.time_avg, v_avg.data_avg);
		if( s->audio->valid ) {
		serprintf("[%03d|%03d]  ", a_avg.time, aud_avg.time_avg );
		}
		serprintf("s %3d  ", t_show );
		serprintf("v %7d%c  a %7d/%4d  ", s->video_time, s->video_drop ? '-' : ' ', s->audio_time, s->audio_time - last_audio );
		serprintf("d %3d  avs %3d|%3d|%3d]  ", s->delay, st->audio_chunks, st->video_chunks, st->sub_chunks );
#if 0
		if( s->audio->valid ) {
			int free;
			audiodevice_get_obuffer( NULL, &free );
			serprintf("[%5d|%d]  ", free, audiodevice_can_write());
		}
#endif
DBGV3 		serprintf("tp %5d/%5d  rate %7d/%7d  ", st->atime_parsed, st->vtime_parsed, st->acurrent_rate, st->vcurrent_rate  );			
	} else 
	DBGQ {
		serprintf("\r\n");
	}
#ifdef CONFIG_GUI
	DBGV4 {
#define WID	200
#define BUF	 60
#define BUF2	(BUF + WID + 10)

		static int y;
		LCD_DrawHLine( 0,   y,     BUF2 + WID, TRANSPARENT_COLOR );
		LCD_DrawHLine( 0,   y + 1, BUF2 + WID, TRANSPARENT_COLOR );
		LCD_DrawHLine( 0,   y + 2, BUF2 + WID, TRANSPARENT_COLOR );
		LCD_DrawPixel( 10,  y,				BLUE );
		LCD_DrawPixel( 10 + s->video->msPerFrame, y,    BLUE );
		LCD_DrawPixel( 10 - s->delay / 10, y,		GREEN );	// audio 2 video delay [frames]
//		LCD_DrawPixel( 10 + t_show,        y,		YELLOW );	// time between two frames shown [ms]
		LCD_DrawPixel( 10 + v_avg.time/ 10,y,		RED );		// time to decode last frame [ms]
		LCD_DrawPixel( 10 + v_avg.time_avg / 10,  y,	YELLOW );	// time to decode last frame [ms]
	DBGV4 {
	if( st->buffer ) {
		LCD_DrawPixel( BUF,                                                               y, BLUE );
		LCD_DrawPixel( BUF + WID,                                                         y, BLUE );
		LCD_DrawPixel( BUF + POS(st->buffer->buf_write, st->buffer->buffer_size, WID),    y, CYAN);
#ifdef CONFIG_TDES
		LCD_DrawPixel( BUF + POS(ext_buf_decrypt, st->buffer->buffer_size, WID),          y, BLACK);
#endif
		LCD_DrawPixel( BUF + POS(st->buffer->buf_scan,  st->buffer->buffer_size, WID),    y, LT_GREEN );
	if( st->buffer->video )	
		LCD_DrawPixel( BUF + POS(st->buffer->vid_last_buf, st->buffer->buffer_size, WID), y, PURPLE);	
	if( st->buffer->audio )	
		LCD_DrawPixel( BUF + POS(st->buffer->aud_last_buf, st->buffer->buffer_size, WID), y, RED);
	if( st->buffer->subtitle )	
		LCD_DrawPixel( BUF + POS(st->buffer->sub_last_buf, st->buffer->buffer_size, WID), y, YELLOW);
	if( s->audio->valid && st->audio_chunks == 0 ) 
		LCD_DrawHLine( BUF + WID - 3, y, BUF + WID, RED );
	if( st->buffer2 ) {	
		LCD_DrawPixel( BUF2,                                                                 y, BLUE );
		LCD_DrawPixel( BUF2 + WID,                                                           y, BLUE );
		LCD_DrawPixel( BUF2 + POS(st->buffer2->buf_write,    st->buffer2->buffer_size, WID), y, CYAN);
		LCD_DrawPixel( BUF2 + POS(st->buffer2->buf_scan,     st->buffer2->buffer_size, WID), y, LT_GREEN );
		LCD_DrawPixel( BUF2 + POS(st->buffer2->aud_last_buf, st->buffer2->buffer_size, WID), y, RED);
	}
	}
	}
		if ( ++y == SCREEN_height - 48 ) y = 0;
	}
#endif	
	last_audio = s->audio_time;
	last_video = s->video_time;
}

// ************************************************************
//
//	_print_audio_debug
//
// ************************************************************
static void _print_audio_debug( STREAM *s )
{
	STREAM_PARSER_STATS _st;
	STREAM_PARSER_STATS *st = s->parser->get_stats ? s->parser->get_stats( s, &_st ) : NULL;
	static int last_audio;
	
	if( s->audio_time == last_audio )
		return;
		
	DBGV2 {
		serprintf("\r\n");
		if( !st )
			return;	
		if( st->buffer_size ) {
			serprintf("[%2lld%%] ", (UINT64)st->buffer_used * 100 / (UINT64)st->buffer_size );
			if( st->buffer2_size )
				serprintf( "[%2lld%%] ", (UINT64)st->buffer2_used * 100 / (UINT64)st->buffer2_size );
		}
		if( s->audio->valid ) {
		serprintf("[%03d|%03d]  ", a_avg.time, aud_avg.time_avg );
		}
		serprintf("a %7d/%4d  ", s->audio_time, s->audio_time - last_audio );
		serprintf("chu %3d  ", st->audio_chunks );
DBGV3 		serprintf("tp %5d  rate %7d  ", st->atime_parsed, st->acurrent_rate );			
	}
	last_audio = s->audio_time;
}

static int _handle_video_codec_error( STREAM *s );

// *****************************************************************************
//
//	_check_end
//
// *****************************************************************************
static int _check_end( STREAM *s )
{
	if( s->stop_time && 	((s->video->valid && s->video_time > s->stop_time) || 
				(!s->video->valid && s->audio_time > s->stop_time) ) 
	) {
		if( !s->stream_end ) {
DBGS serprintf("stop_time reached %d  v %d  a%d\r\n", s->stop_time, s->video_time, s->audio_time );
DBGS if( s->buffer ) {
serprintf("____%d %lld\r\n", s->buffer->buf_scan, s->buffer->buf_scan_pos );
}
			s->video_end = 1;
			s->audio_end = 1;
			goto FORCE_STOP;
		}
	}
	
	if( s->video_error ) {
		if( stream_handle_codec_error && !stream_force_prio && s->video_error == VE_VIDEO_CODEC_ERROR ) {
			if( !_handle_video_codec_error( s ) ) {
				_free_all_frames( s );
		
				// try to re-use the cached chunks
				_seek_init( s );
				s->seek = 0;
				// read from the cache to restart video
				s->cc.use_cache = 1;
				s->cc.read      = 0;
				return 1;
			}
serprintf("failed to handle video codec error\r\n");
		}
serprintf("error, stop!\r\n");
		s->video_end = 1;
		s->audio_end = 1;
	}
	
	if ( s->stream_end == 0 ) {
		// check if both video and audio are at the end (if present)
		int video_end = s->video_error || (s->video->valid ? (s->video_end && !s->cdata_now.valid && !s->cdata_next.valid) : 1);
		int audio_end = s->video_error || (s->audio->valid ?  s->audio_end : 1);
		
		if( video_end && audio_end ) {
			// maybe the parser wants to go on?
			if( !s->video_error && s->parser->start_next && !s->parser->start_next( s ) ) {
DBGS serprintf("stream_end: parser starts next!\r\n");
				_seek_init( s );
				_video_init( s, -1 );
				return 0;
			}
FORCE_STOP:
DBGS serprintf("stream_end\r\n");
			
			s->stream_end = 1;
			if( s->parser_error ) {
				// if the parser set an error flag, we report a broken file
				stream_set_error( s, VE_FILE_ERROR );
			}
			if( s->flags & STREAM_LOOP && !s->video_error ) {
DBGS serprintf("stream_loop\r\n");
				if( _stream_seek_loop( s ) ) {
DBGS serprintf("cannot_loop\r\n");
					stream_set_error( s, VE_ERROR );	
				}
			} else {
				if( s->stop ) {
					s->stop( s );
				}
				// signal end to video sink
				if( s->video_sink) {
					s->video_sink->end( s->video_sink );
				}

			}
		}
	} else {
		if( (s->video->valid && !s->video_end) || (!s->video->valid && !s->audio_end) ) {
DBGS serprintf("stream_continue\r\n");
			s->stream_end = 0;
		} else {
			return 1;
		}
	}
	return 0;
}

// *****************************************************************************
//
//	_get_next_chunk
//
// *****************************************************************************
static void _get_next_chunk( STREAM *s )
{
	if( !s->seek && !s->cdata_next.valid && !stream_video_paused ) {
		// get one video data chunk from the parser
		int start = atime();
		if( stream_get_chunk_cache( &s->cc, s->cbe, &s->cdata_next )) {
			if( s->parser->get_video_cdata( s, s->cbe, &s->cdata_next ) ) {
				if( s->video_parse_end ) {
					if( s->video_end == 0 ) {
DBGS serprintf("video end\r\n");
						s->video_end = 1;
					}
				}
			} else {
				stream_put_chunk_cache( &s->cc, s->cbe, &s->cdata_next );
			}
		}
		cdata_v_time = atime() - start;
	}

	// use next chunk if current one has been all eaten
	if( !s->cdata_now.valid && s->cdata_next.valid ) {
		s->cdata_now = s->cdata_next;
		s->cdata_next.valid = 0;

		if( !stream_fake_ts_post && stream_fake_ts_num && stream_fake_ts_den ) {
			s->cdata_now.time = (UINT64)stream_fake_ts_num * stream_fake_ts_count++ / stream_fake_ts_den;
		}
		if( stream_tweak_fps ) {
			s->cdata_now.time = (UINT32)((UINT64)s->cdata_now.time * (UINT64)stream_tweak_fps / 100);
		}
		if( s->cdata_now.pos != -1 ) {
			s->video_pos = s->cdata_now.pos;
		}
		if( s->video_dumper) {
			s->video_dumper->write( s, cbe_get_p( s->cbe ), &s->cdata_now );
		}
		// is there a pre_mangler, then call it!
		if( s->video_mangler ) {
			s->video_mangler->pre( s, s->cbe, &s->cdata_now );
		}	
	}
}

// *****************************************************************************
//
//	_do_stuff
//
// *****************************************************************************
static void _do_stuff( STREAM *s )
{
	if ( s->video->valid ) {
		_get_next_chunk( s );
	}
}

// *****************************************************************************
//
//	_droppable
//
// *****************************************************************************
static int _droppable( STREAM *s, int type )
{
	// MJPEG frames are always droppable if needed
	if( s->drop > 0 && s->video->format == VIDEO_FORMAT_MJPG ) {
		s->drop --;
		frames_dropped ++;
		return 1;
	}
	
	// B frames: 
	if( type == B_VOP && (s->drop_B || stream_bdrop_force) ) {
serprintf("b!");
		if( s->drop ) {
			s->drop --;
			frames_B_dropped ++;
		}

		return 1;
	}
	
	return 0;
}

// *****************************************************************************
//
//	output_frames
//
// *****************************************************************************
static int output_frames( STREAM *s )
{
	int ret = 0;
	VIDEO_FRAME *output_frame = frame_q_get( &s->disp_q );
	
	while( output_frame ) {
		if( stream_fake_ts_post && stream_fake_ts_num && stream_fake_ts_den ) {
			output_frame->time = (UINT64)stream_fake_ts_num * stream_fake_ts_count++ / stream_fake_ts_den;
		}
	
DBGQ serprintf("UNQ[%2d|%2d] ", output_frame->index, frame_q_count( &s->disp_q ) );
DBGQ2 serprintf("\r\nDEC[%2d]  DISP[%2d]  ", frame_q_count( &s->decode_q ), frame_q_count( &s->disp_q ));
		output_frame->epoch = s->seek_epoch;
		s->output_frame_fn( s, output_frame, &output_frame );
		ret = 1;
		
		if( s->video_sink->put_time ) {
			output_frame = frame_q_get( &s->disp_q );
			if( output_frame ) {
serprintf("+");		
			}
		} else {
			break;
		}
	}
	
	return ret;
}

// *****************************************************************************
//
//	_stream_player_sync (kilroy was here)
//
// *****************************************************************************
static void _stream_player_sync( STREAM *s )
{
DECODE_AGAIN:
	if( s->video->valid && s->use_sink_frames ) {
		pthread_mutex_lock( &s->video_sink_mutex );
		_queue_sink_frames( s );
		pthread_mutex_unlock( &s->video_sink_mutex );
	}
	
	_do_stuff( s );

	if( !s->play_n_video_frames ) {
		// return if paused
		if ( s->paused ) {
			return;
		}
		// do we stop?
		if( _check_end( s ) ) {
			return;
		}
	}

	if ( !s->video->valid ) {
		// leave here if no video
		if( t_adecode || t_adecode_bytes ) {
			do_avg( &a_avg, t_adecode, t_adecode_bytes );
			t_adecode       = 0;
			t_adecode_bytes = 0;
			_print_audio_debug( s );
		}
		return;
	}

	if( !s->cdata_now.valid ) {
DBGV serprintf("!" );				
		msec_sleep( 10 );
		return;
	}

	// get a decode frame          
	s->decode_frame = frame_q_get( &s->decode_q );
	if( s->decode_frame ) {
DBGQ  serprintf("GET[%2d|%2d] ", s->decode_frame->index, frame_q_count( &s->decode_q ) );
	} else  {
		// NO decode frame!
		if( s->use_sink_frames ) {
			// we will wait for the sink to free more frames

			// maybe we can output some more frames in the meantime? 
			if( !s->paused && !stream_no_output && s->video_sink_count < stream_sink_video_max ) {
				output_frames( s );
			}

			return;
		} else {
			// OK, we should really never be here, if we ran out of
			// frames something is really wrong!
serprintf("cannot get DECODE_FRAME!\r\n");
			// but, life goes on and so do we, so free all frames and start all over
			_free_all_frames( s );
			s->decode_frame = frame_q_get( &s->decode_q );
			if( !s->decode_frame ) {
serprintf("really cannot get DECODE_FRAME!\r\n");
				return;	
			}
		}
	}
DBGV1 WAIT("F")

DBGV1 WAIT_INIT("A")

	if( stream_force_video_props && s->cdata_now.key ) {
		s->cdata_now.changed = stream_force_video_props;
		stream_force_video_props = NULL;
	}

	// check if some video props changed
	if( s->cdata_now.changed ) {
		if( _video_props_changed( s, &s->cdata_now ) ) {
serprintf("_video_props_changed: FAILED!\r\n");
			return;	
		}

		// prevents s->decode_frame to be NULL below - at least on the SIM
		if( !s->decode_frame ) {
			s->decode_frame = frame_q_get( &s->decode_q );
		}
		if( !s->decode_frame ) {
serprintf("really cannot get DECODE_FRAME!\r\n");
			return;	
		}
	}
	
	if( !s->paused && s->audio_sink && s->audio_sink->syncable(s ) && s->video_sink && s->video_sink->syncable( s->video_sink ) ) {
		stream_sync( s );
	
		if( s->drop_P ) {
			// do all the magic that is needed to forget anything about the current decode pipeline
			// and start with the next chunk which is a key frame...
			s->drop          = 0;
			s->drop_P        = 0;
			s->delay         = 0;
			s->delay_valid   = 0;
			s->sink_ref_time = -1;
			s->video_flush   = 1;
			if( s->cdata_now.valid ) {
				cbe_skip( s->cbe, s->cdata_now.size );
				s->cdata_now.valid = 0;
			}
			if( s->cdata_next.valid ) {
				cbe_skip( s->cbe, s->cdata_next.size );
				s->cdata_next.valid = 0;
			}
			_free_all_frames( s );
			goto DECODE_AGAIN;
		}
	}
DBGV1 serprintf("[d %3d  %03d|%03d|%6d ", s->delay, v_avg.time, v_avg.time_avg, v_avg.data_avg);
t_show = m_time - t_showtime;
t_showtime = m_time;

	
cdata_frame = s->cdata_now.frame;
cdata_time  = s->cdata_now.time;
	// do this here, there is no way to leave and
	// we could switch context before we reach the end
	// of this case!
	s->video_state = VID_WAITING_FOR_DEC_RSP;

	// guess the frame type (I vs BP) by looking at the key flag (manglers and decoders will update that)
	s->cdata_now.frm_type = s->cdata_now.key ? I_VOP : P_VOP;

	// is this a frame we can drop?
	if( _droppable( s, s->cdata_now.frm_type )
	    || (stream_key_frame_only && !s->cdata_now.key ) ) {
		
		cbe_skip( s->cbe, s->cdata_now.size );
		s->cdata_now.valid = 0;

		frame_q_put( &s->decode_q, s->decode_frame );
		s->decode_frame = NULL;

		goto DECODE_AGAIN;
	}

	//  Call the Decoder (non-blocking)    
	//-----------------------------------   
	s->decode_frame->time       = s->cdata_now.time;
	s->decode_frame->user_ID    = s->cdata_now.user_ID;
	s->decode_frame->type       = s->cdata_now.frm_type;
	s->decode_frame->audio_skip = s->cdata_now.audio_skip;
	s->decode_frame->video_skip = s->cdata_now.video_skip;
	// clear the values!
	s->cdata_now.audio_skip = 0;
	s->cdata_now.video_skip = 0;

	s->vcodec.data         = cbe_get_p( s->cbe );
	s->vcodec.data_size    = s->cdata_now.size;
	s->vcodec.decode_frame = s->decode_frame;

	if( s->vcodec.data_size != -1 ) {
		// call decoder, set flag last!!!		
		pthread_mutex_lock( &s->codec_mutex );
		s->vcodec.done = 1;
		pthread_cond_broadcast(&s->codec_code);
		pthread_mutex_unlock( &s->codec_mutex );
	}
	
	if( !s->paused && !stream_no_output && s->video_sink_count < stream_sink_video_max ) {
		output_frames( s );
	}

	_print_debug( s );
DBGV1 WAIT("W")

	if( do_core ) {
serprintf("stream: core now\r\n");
		msec_sleep( 10 );
		raise( SIGABRT );
	}

	if( s->vcodec.data_size == -1 ) {
		s->vcodec.decode_frame->error = 0;
		s->vcodec.decode_frame->valid = 1;
		s->vcodec.decoded   = 0;
		s->vcodec.data_size = 0;
		s->cdata_now.size   = 0;
// FIXME
//		memset16( s->decode_frame->data[0],  0x0080, s->decode_frame->size / 2 );
		goto SKIP_DECODE;
	}
#if 0
	pthread_mutex_lock(&s->video_done_mutex);
	while( s->vcodec.done != 2 ) {
DBGV1 serprintf(" w ");
		pthread_cond_wait(&s->video_done, &s->video_done_mutex);
	}
	pthread_mutex_unlock(&s->video_done_mutex);
#else
	while( s->vcodec.done != 2 ) {
		if( _engine_abort( s ) ) {
			return;
		}
		if( !s->paused )
			_do_stuff( s );
		stream_yield_RT();
	}
#endif
SKIP_DECODE:
	// get back the decoded frame
	s->decode_frame = s->vcodec.decode_frame;

DBGV1 WAIT("E")
DBGV1 serprintf("\r\n");

	// stay in this thread as we have more stuff to do
	s->engine_yield = 0;

	do_avg( &v_avg, s->vcodec.time, s->vcodec.decoded );
	do_avg( &a_avg, t_adecode,      t_adecode_bytes   );
	t_adecode       = 0;
	t_adecode_bytes = 0;
	
	// handle errors			
	if( s->decode_frame ) {
		if ( !s->decode_frame->error ) {
			// SUCCESS
			s->error_count = 0;
		} else {
			// ERROR !!
			s->error_count ++;
serprintf("ve!");
DBGV serprintf(" at %d \r\n", s->frame_count );
			if( !s->vcodec.decoded ) {
				// naughty did not eat any pudding, we cannot have that
				// so it must eat all the pudding now!
				s->vcodec.decoded = s->cdata_now.size;
			}
		} 
DBGCV1 serprintf("[%6d  d %6d/%7d %d|%8d|%c]", cdata_time, s->vcodec.decoded, s->cdata_now.size, s->decode_frame->valid, s->decode_frame->time, frame_type( s->decode_frame->type ) );
	
		// is there a post mangler, then call it!
		if( s->video_mangler ) {
			s->video_mangler->post( s, &s->decode_frame );
		}	
	}

	cbe_skip( s->cbe, MIN( s->cdata_now.size, s->vcodec.decoded ) );
	s->cdata_now.size -= s->vcodec.decoded;

	if( s->cdata_now.size < 8 ) {
		// we ate up all the data in this chunk, free it
		cbe_skip( s->cbe, MAX( 0, s->cdata_now.size) );
		s->cdata_now.valid = 0;
	}

	int output = 0;
	// queue the decode frame for output
	if( s->decode_frame ) {
		output = 1;
DBGQ serprintf("QUE[%2d<", s->decode_frame->index );
		frame_q_put( &s->disp_q, s->decode_frame );
DBGQ serprintf(">%2d] ", frame_q_count( &s->disp_q ) );

		s->decode_frame = NULL;
	}
	
	if( s->paused ) {
		// output now if we are paused or single stepping,
		// (output while decoding next frame if not)
		output_frames( s );
	}

	if( s->play_n_video_frames > 0 ) {
		if( !s->seek_frame || output ) {
serprintf("%d %d\n", s->seek_frame, output );
			s->play_n_video_frames --;
		}
	}

	s->video_state = VID_CALL_DECODER;

	return;
}

static int check_realloc( STREAM *s )
{
	if( !s->video_dec->need_realloc || !s->use_sink_frames ) {
		return 0;
	}	

	int num_frames;
	int realloc = s->video_dec->need_realloc( s->video_dec, s->video, &num_frames );

	if (realloc) {
		pthread_mutex_lock( &s->video_sink_mutex );

		s->decode_frame      = NULL;
		s->current_frame     = NULL;

		frame_q_flush( &s->disp_q   );
		frame_q_flush( &s->decode_q );
		frame_q_flush( &s->locked_q );
		frame_q_flush( &s->codec_q  );

		int i;
		for( i = 0; i < s->num_frames; i++ ) {
			if( s->frames[i] )
				s->frames[i]->locked = 0;
		}

		s->video_sink_count = 0;

		s->video_sink->close( s->video_sink );
		if( s->video_sink->open( s->video_sink, s->video, s, num_frames, &s->video_rc ) ) {
			stream_set_error( s, VE_VIDEO_CODEC_ERROR );
			s->num_frames = 0;
			pthread_mutex_unlock( &s->video_sink_mutex );
			return 1;
		}
		_query_sink_frames( s );

		pthread_mutex_unlock( &s->video_sink_mutex );

		// call prepare if needed
		if( s->video_dec->prepare && s->video_dec->prepare( s->video_dec, s->frames, s->num_frames ) ) {
serprintf("error preparing decoder in realloc!\n");
			stream_set_error( s, VE_VIDEO_CODEC_ERROR );
			return 1;
		}
	}

	return 0;
}

static int _handle_video_codec_error( STREAM *s )
{
	if (s->video_sink) {
		if (s->video_sink->is_open) {
			s->video_sink->close(s->video_sink);
			if( s->video_sink->delete ) {
				s->video_sink->delete( s->video_sink );
			}
			s->video_sink = NULL;
		}
	}

	int cpu = s->video_dec->cpu;	

	if( s->video_dec->is_open) {
		stream_close_video_dec( s );
	}

	// lower the priority
	cpu--;
	if( !cpu ) {
serprintf("no lower prio possible!\n" ); 
		return 1;
	}	
	stream_set_cpu_priority( s, cpu );

	if( stream_open_video_dec( s, NULL ) ) {
		return 1;
	} 

	s->player = s->video_dec->async ? _stream_player_async : _stream_player_sync;

	if( s->message_cb ) {
		s->message_cb( s, STREAM_DECODER_CHANGED );
	}
serprintf("VID_DEC: [%s]\r\n", s->video_dec ? s->video_dec->name : "(none)" ); 

	return 0;
}

// *****************************************************************************
//
//	_stream_player_async (kilroy was here)
//
// *****************************************************************************
static void _stream_player_async( STREAM *s )
{
	int ret = 0;
	if ( !s->video->valid ) {
		// leave here if no video
		return;
	}
	
	if( s->video_flush ) {
		s->video_flush = 0;

		// tell the decoder we want to flush it
		s->video_dec->flush( s->video_dec );
	}

	if( s->use_sink_frames ) {
		pthread_mutex_lock( &s->video_sink_mutex );
		_queue_sink_frames( s );
		pthread_mutex_unlock( &s->video_sink_mutex );
	}
	
	_do_stuff( s );

	if( !s->play_n_video_frames ) {
		// return if paused
		if ( s->paused ) {
			return;
		}
	}
	
	// do we stop?
	if( _check_end( s ) ) {
		return;
	}

	// get a decode frame          
	if( !s->decode_frame ) {
		s->decode_frame = frame_q_get( &s->decode_q );
		if( s->decode_frame ) {
DBGQ  serprintf("GET[%2d|%2d] ", s->decode_frame->index, frame_q_count( &s->decode_q ) );
		}
	}
	if( s->decode_frame && frame_q_count( &s->codec_q ) < codec_max ) {
t_show = m_time - t_showtime;
t_showtime = m_time;
		// feed it to the decoder
		VIDEO_FRAME *in_frame  = s->decode_frame;
		ret = s->video_dec->put_out( s->video_dec, &s->decode_frame );
DBGQ  serprintf("put_out: %08X -> %08X \n", in_frame, s->decode_frame );
	} else {
		ret = s->video_dec->put_out( s->video_dec, NULL );
	}
	if (ret) {
		stream_set_error( s, VE_VIDEO_CODEC_ERROR );
		if (_check_end( s ))
			return;
	}
	
	{
		VIDEO_FRAME *out_frame = NULL;
		s->video_dec->get_out( s->video_dec, &out_frame );
		if( out_frame ) {

			// is there a post mangler, then call it!
			if( s->video_mangler ) {
				s->video_mangler->post( s, &out_frame );
			}	

			if( s->play_n_video_frames && s->play_n_video_time != -1 ) {
				if(( s->play_n_old_time && out_frame->time >= s->play_n_old_time   ) || // seek back
				   (!s->play_n_old_time && out_frame->time <  s->play_n_video_time )) { // seek forward
					// discard the frame to decode queue
DBGQ serprintf("DIS %08d|%08d|%08d [%2d<", out_frame->time, s->play_n_video_time, s->play_n_old_time, out_frame->index );
					frame_q_put( &s->decode_q, out_frame );
DBGQ serprintf(">%2d] ", frame_q_count( &s->decode_q ) );
					out_frame = NULL;
				}
			} 
			if( out_frame ) {
				out_frame->deinterlace = 0;
				if ((out_frame->interlaced != VIDEO_PROGRESSIVE) && stream_force_sw_deinterlacing) {
					int deinterlacing_limit = (out_frame->interlaced == VIDEO_INTERLACED_ONE_FIELD) ? stream_deinterlacing_max_height / 2 : stream_deinterlacing_max_height;
					if ((out_frame->height <= deinterlacing_limit) && (s->video_dec->cpu != STREAM_CPU_ARM)) {
serprintf("deinterlacing_limit: %d < %d\n", out_frame->height, deinterlacing_limit);
						stream_set_error( s, VE_VIDEO_CODEC_ERROR );
						if (_check_end( s ))
							return;
					} else if (out_frame->height <= deinterlacing_limit) {
						out_frame->deinterlace = 1;
					}
				}
				// queue the frame for output
DBGQ serprintf("QUE[%2d<", out_frame->index );
				frame_q_put( &s->disp_q, out_frame );
DBGQ serprintf(">%2d] ", frame_q_count( &s->disp_q ) );

				// check if the size changed!
				if( out_frame->valid && (out_frame->width != s->video->width
				    || out_frame->height     != s->video->height
				    || out_frame->interlaced != s->video->interlaced ) ) {
serprintf("VIDEO SIZE CHANGED! %dx%d|%d -> %dx%d|%d\r\n", s->video->width, s->video->height, s->video->interlaced, out_frame->width, out_frame->height, out_frame->interlaced  );
					s->video->width      = out_frame->width;
					s->video->height     = out_frame->height;
					s->video->interlaced = out_frame->interlaced;

					if( s->message_cb ) {
						s->message_cb( s, STREAM_VIDEO_PROPS_CHANGED );
					}
				}
			}

			if( !s->paused && s->audio_sink && s->audio_sink->syncable(s ) && s->video_sink && s->video_sink->syncable( s->video_sink ) ) {
				stream_sync( s );
			}
		}
	}

	if( check_realloc( s ) ) {
		_check_end( s );
		return;
	}
	
	// maybe we can output some more frames in the meantime? 
	if( (!s->paused || s->play_n_video_frames) && !stream_no_output && s->video_sink_count < stream_sink_video_max ) {
		if( output_frames( s ) ) {
			if( s->play_n_video_frames > 0 ) {
				s->play_n_video_frames --;
			}
			_print_debug( s );
		}
	}

	if( s->cdata_now.valid ) {
		int decoded = 0;
		int time; 
		int size = s->cdata_now.size;

		if( stream_fps_mode == 3 ) {
serprintf("[%5d] siz %6d\n", s->fps_count, size );		
			s->fps_count ++;
			cbe_skip( s->cbe, size );
			s->cdata_now.valid = 0;
			return;
		}
		if( stream_key_frame_only && !s->cdata_now.key ) {
			cbe_skip( s->cbe, size );
			s->cdata_now.valid = 0;
			return;
		}
		VIDEO_FRAME _d = { 0 };
		VIDEO_FRAME *d = &_d;
		d->data[0] = cbe_get_p( s->cbe );
		d->size    = size;
		d->time    = s->cdata_now.time;
		d->user_ID = s->cdata_now.user_ID;
		
		// guess the frame type (I vs BP) by looking at the key flag (manglers and decoders will update that)
		d->type    = s->cdata_now.key ? I_VOP : P_VOP;
	 
		ret = s->video_dec->dec_in( s->video_dec, &d, &decoded, &time );
		if (ret) {
			stream_set_error( s, VE_VIDEO_CODEC_ERROR );
			if (_check_end( s ))
				return;
		}
		
		if (decoded) {
			do_avg( &v_avg, -1, decoded );
			
			cbe_skip( s->cbe, decoded );
			s->cdata_now.size -= decoded;
			if (decoded == size) {
//serprintf("dec_in:  %6d -> %6d tim %3d\n", size, decoded, time );
				// skip the input data
				s->cdata_now.valid = 0;
			}
		}
	}
}

// *****************************************************************************
//
//	stream_seekable
//
// *****************************************************************************
int stream_seekable( STREAM *s )
{
	if( !s || !s->open ) {
serprintf("SKB: not open!\r\n");
		return 0;
	}
	if( stream_no_seek )
		return 0;
		
	return s->parser->seekable ? s->parser->seekable( s ) : 0;
}

// *****************************************************************************
//
//	stream_pauseable
//
// *****************************************************************************
int stream_pauseable( STREAM *s )
{
	if( !s || !s->open ) {
serprintf("SPB: not open!\r\n");
		return 0;
	}
	if( stream_force_pauseable )
		return 1;
		
	return s->parser->pauseable ? s->parser->pauseable( s ) : 0;
}

// *****************************************************************************
//
//	_seek_init
//
// *****************************************************************************
static void _seek_init( STREAM *s )
{
	s->seek = 1;
	
	if( s->cbe )
		cbe_flush( s->cbe );
	
	s->cdata_now.valid  = 0;
	s->cdata_next.valid = 0;

	s->cdata_sub.valid  = 0;

	if ( s->video->needs_header ) {
		s->video->header_sent = 0;
	}
	s->video->extra_sent = 0;
}

// *****************************************************************************
//
//	_seek_pause
//
// *****************************************************************************
static int _seek_pause( STREAM *s )
{
	s->seek_paused     = 1;
	s->sync_audio      = 0;
	s->sync_video      = 0;
	
	thread_state_set( &s->parser_tstate,  THREAD_IDLE );
	if( s->audio->valid )
		thread_state_set( &s->audio_tstate,  THREAD_IDLE );

	return stream_pause( s );
}

// *****************************************************************************
//
//	_seek_un_pause
//
// *****************************************************************************
static void _seek_un_pause( STREAM *s, int was_paused )
{
	if( s->audio->valid )
		thread_state_set( &s->audio_tstate,  THREAD_RUNNING );
	thread_state_set( &s->parser_tstate,  THREAD_RUNNING );
	
	stream_un_pause( s, was_paused );
	s->seek_paused = 0;
}

// *****************************************************************************
//
//	_stream_wait_for_idle
//
// *****************************************************************************
static int _stream_wait_for_idle( STREAM *s, int timeout )
{
	timeout += atime(); 

	while( 1 ) { 
		if( !s->video->valid || s->video_state == VID_CALL_DECODER ) {
			return 0;
		}
		if( atime() >= timeout ) {
serprintf("can't idle!\r\n");
			return 1;
		}
//serprintf(":");					
		stream_yield();
	}

	return 0;
}

// *****************************************************************************
//
//	_stream_seek_loop
//
// *****************************************************************************
static int _stream_seek_loop( STREAM *s )
{
	int err = 1;
	STREAM_CHUNK sc = { 0 };
	int was_paused;
	
	if( !stream_seekable( s ) ) {
serprintf("not seekable!\r\n");
		return 1;	
	}
	
	// pause the stream
	was_paused = _seek_pause( s );
	
DBGS serprintf("stream_seek_loop time %d  pos %d  \r\n", s->start_time, s->start_pos );
	_seek_init( s );
	
	if ( s->start_time ) {
DBGS serprintf("loop from time %d \r\n", s->start_time );
		err = s->parser->seek_time ? s->parser->seek_time( s, s->start_time, STREAM_SEEK_BACKWARD, STREAM_SEEK_STRICT, 0, &sc ) : 1;
	} else if ( s->start_pos ) {
DBGS serprintf("loop from pos %d \r\n", s->start_pos );
		err = s->parser->seek_pos ? s->parser->seek_pos( s, s->start_pos, STREAM_SEEK_BACKWARD, STREAM_SEEK_STRICT, 0, &sc ) : 1;
	} else {
DBGS serprintf("loop from start (pos = 0) \r\n" );
		err = s->parser->seek_pos ? s->parser->seek_pos( s, 0, STREAM_SEEK_BACKWARD, STREAM_SEEK_STRICT, 0, &sc ) : 1;
	}

	if( err ) {
serprintf("video_seek err!\r\n");
		// un_pause the stream
		_seek_un_pause( s, was_paused );
		return err;	
	}

DBGS serprintf("stream_seek_loop from %d to frame %d  time %d\r\n", s->video_time, sc.frame, sc.time );
	
	_video_init( s, sc.time );
	
	stream_audio_flush( s );

	if( s->video_dec) {
		s->video_flush = 1;
		s->cdata_now.valid  = 0;
		s->cdata_next.valid = 0;

		_free_all_frames( s );
	}
	if( s->sub_dec) {
		s->sub_dec->flush( s->sub_dec );
		s->cdata_sub.valid = 0;
	}
	_stream_resync( s );
	
	// init the AV sync machinery to have a clean start of the video
	stream_sync_init( s, sc.time );

	// un_pause the stream
	_seek_un_pause( s, was_paused );
	
	return err;	
}

// *****************************************************************************
//
//	_stream_seek_real
//
// *****************************************************************************
static int _stream_seek_real( STREAM *s, int time, int pos, int dir, int flags, int force_reload )
{
	int err = 1;
	STREAM_CHUNK sc = { 0 };
	int was_paused;
	int start1   = atime();
	int old_time = s->video_time;
	
	if( !s->open ) {
serprintf("SEE: not open!\n");
		return 1;
	}
	
	if( !stream_seekable( s ) ) {
serprintf("not seekable!\n");
		return 1;	
	}
	
	// pause the stream
	was_paused = _seek_pause( s );
DBGS serprintf("\n----------> seek to time %d   pos  %d  dir  %d\n", time, pos, dir );
	
	if( _stream_wait_for_idle( s, 1000 ) ) {
		// un_pause the stream
		_seek_un_pause( s, was_paused );
		return 1;
	}

	_seek_init( s );

	if( time != -1 ) {
		// seek by time
		if( (err = s->parser->seek_time ? s->parser->seek_time( s, time, dir, flags, force_reload, &sc ) : 1) ) {
serprintf("stream_seek time err!\n");
		}
	} else {
		// seek by pos
		if( (err = s->parser->seek_pos ? s->parser->seek_pos( s, pos, dir, flags, force_reload, &sc ) : 1) ) {
serprintf("stream_seek pos err!\n");
		}
	}
DBGS serprintf("\nparser seeked to time %d\n", sc.time );

	_video_init( s, sc.time );

	stream_audio_flush( s );

	if( err ) {
		stream_sync_init( s, sc.time );
		// un_pause the stream
		_seek_un_pause( s, was_paused );
		return err;	
	}

	if( s->audio->valid ) {
		if( time == 0 || pos == 0 ) {
serprintf("STUFF_ZERO!\n");
			s->audio_stuff_zero = 1;
		}
	}
	if( s->video_dec) {
		s->video_flush = 1;
		if( s->video_dec->async ) {
			// in async case we let the machine roll from here until we get the frame we want
			s->seek = 0;
			thread_state_set( &s->parser_tstate,  THREAD_RUNNING );
		}
	}
	
	int start2 = atime();
	
	thread_state_set( &s->parser_tstate,  THREAD_RUNNING );

	if( s->video->valid ) {
DBGV serprintf("play one frame\n");
		s->play_n_video_one = 1;
		_stream_play_n_frames( s, 10, sc.time, old_time );
		s->seek_epoch++;
		s->seek_frame = 0;
	}

	// init the AV sync machinery to have a clean start of the video
	stream_sync_init( s, sc.time );
	
DBGS serprintf("\nseeked to frame %d  time %d|%d   pos %lld|%lld <------------ took %3d/%3d\n", sc.frame, s->video_time, s->audio_time, s->video_pos, s->audio_pos, atime() - start1, atime()- start2 );
	
	// un_pause the stream
	_seek_un_pause( s, was_paused );
	
	return err;	
}

// *****************************************************************************
//
//	_stream_seek_abortable
//
// *****************************************************************************
static int _stream_seek_abortable( STREAM *s, int time, int pos, int dir, int flags, int force_reload )
{
	s->abort = s->user_abort;
	int ret  = _stream_seek_real( s, time, pos, dir, flags, force_reload );
	s->abort = NULL;
	if( s->aborted ) {
serprintf("STREAM_seek: aborted\r\n");	
		if( s->stop ) {
			s->stop( s );
			s->stop = NULL;
		}
		return 1;
	}
	return ret;
}

// *****************************************************************************
//
//	stream_seek_time
//
// *****************************************************************************
int stream_seek_time( STREAM *s, int time, int dir, int flags )
{
	int real_time;
	
	if( time < 0 )
		time = 0;
		
	real_time = _stream_get_real_time( s, time );
	
	return _stream_seek_abortable( s, real_time, -1, dir, flags, 0 );
}

// *****************************************************************************
//
//	stream_seek_pos
//
// *****************************************************************************
int stream_seek_pos( STREAM *s, int pos, int dir, int flags )
{
	int ret;
	
	if( pos < 0 )
		pos = 0;
		
	if( !s->no_duration ) {
		// if we have duration, pos is the same as time!!!
		ret = stream_seek_time( s, pos, dir, flags );
	} else {
		ret = _stream_seek_abortable( s, -1, pos, dir, flags, 0 );
	}
	return ret; 
	
}

// *****************************************************************************
//
//	stream_seek_frame
//
// *****************************************************************************
int stream_seek_frame( STREAM *s, int frame, int dir, int force_reload )
{
	int time = (int)(1000 * (UINT64)frame * (UINT64)s->video->scale / (UINT64)s->video->rate);
	if( !s->open ) {
serprintf("SFR: not open!\r\n");
		return 1;
	}

	return _stream_seek_abortable( s, time, -1, dir, STREAM_SEEK_STRICT, force_reload );
}

// *****************************************************************************
//
//	_stream_play_n_frames
//
// *****************************************************************************
static void _stream_play_n_frames( STREAM *s, int n, int time, int old_time )
{
	int timeout = atime() + 1000; // 1 second before we stop waiting
serprintf("stream_play_n_frames( %d, %d, %d )\r\n", n, time, old_time );
	
	if( !s || !s->open ) {
serprintf("PNF: not open!\r\n");
		return;
	}

	_stream_resync( s );

	s->play_n_video_frames = n;
	s->play_n_video_time   = s->video_dec && s->video_dec->seek ? -1 : time;
	if( old_time > time ) {
		// seek back
		s->play_n_old_time = old_time; 
	} else {
		s->play_n_old_time = 0;
	}
	
	// wait for it to play
	while( s->play_n_video_frames && atime() < timeout ) {
//serprintf("-");	
		stream_yield();
	}

	_stream_wait_for_idle( s, 1000 );
}

// *****************************************************************************
//
//	stream_set_speed
//
// *****************************************************************************
int stream_set_speed( STREAM *s, STREAM_SPEED speed )
{
	int unmute = 0;
serprintf("stream_video_set_speed( %d )\r\n", speed );
	if( !s->open ) {
serprintf("SSP: not open!\r\n");
		return 1;
	}

	int was_paused = stream_pause( s );

	if( _stream_wait_for_idle( s, 1000 ) ) {
		// un_pause the stream
		_seek_un_pause( s, was_paused );
		return 1;
	}

	thread_state_set( &s->engine_tstate, THREAD_IDLE );
	thread_state_set( &s->sub_tstate,    THREAD_IDLE );

	if ( speed == STREAM_SPEED_NORMAL ) {
		if ( s->speed != STREAM_SPEED_NORMAL ) {
			s->speed = STREAM_SPEED_NORMAL;
	
			// if we are not paused ...
			if ( !s->paused ) {
				unmute = 1;
			}
			s->audio->header_sent = 0;

			if( s->sync_mode == STREAM_SYNC_SAMPLES ) {
				// resync audio time!
				s->audio_time     = -1;
				s->audio_ref_time = -1;
			}
		}
	} else {
		if ( s->speed == STREAM_SPEED_NORMAL ) {
			// mute codec first
			stream_audio_mute( s );
	
			if( s->audio->valid ) {
				// resync audio decoder
				thread_state_set( &s->audio_tstate, THREAD_IDLE );
				stream_audio_flush( s );
				thread_state_set( &s->audio_tstate, THREAD_RUNNING );
			}
		}
		s->speed = speed;
	}

	_stream_resync( s );

	thread_state_set( &s->engine_tstate, THREAD_RUNNING );
	thread_state_set( &s->sub_tstate,    THREAD_RUNNING );

	if( unmute ) {
		stream_audio_unmute( s );
	}

	stream_un_pause( s, was_paused );

	return 0;
}

// *****************************************************************************
//
//	stream_set_audio_stream
//
// *****************************************************************************
int stream_set_audio_stream( STREAM *s, int audio_stream )
{	
serprintf("stream_set_audio_stream( %d )\r\n", audio_stream );
 
	if( !s->open ) {
serprintf("SAS: not open!\r\n");
		return 1;
	}
	
	if( !s->audio->valid ) {
serprintf("SAS: not audio!\r\n");
		return 1;
	}

	if( audio_stream >= s->av.as_max ) {
serprintf("SAS: audio_stream > av.as_max\n");	
		return 1;
	}
	if( audio_stream == s->av.as ) {
serprintf("SAS: audio_stream already set\n");	
		return 0;
	}

	int was_paused = stream_pause( s );
	
	// idle threads to make sure they are at a known state
	thread_state_set( &s->audio_tstate,  THREAD_IDLE );
	thread_state_set( &s->engine_tstate, THREAD_IDLE );
	thread_state_set( &s->sub_tstate,    THREAD_IDLE );
	
	// close old audio decoder
	stream_close_audio_dec( s );

	// stop audio sink
	if( s->audio_sink ) {
		s->audio_sink->flush( s );
		s->audio_sink->stop( s );
	}

	// check audio format
	if( stream_check_audio( s ) ) {
		stream_drop_audio( s );
		goto ErrorExit;
	}	
	
	// open the new audio stream (if possible)	
	if( stream_try_open_audio_dec( s, audio_stream, NULL ) ) {
		// no audio, disable it
		stream_drop_audio( s );
	} else {
		if( s->audio_sink ) {
			if( s->audio_sink->start( s ) ) {
				// no audio, close the codec
				stream_close_audio_dec( s );
				// drop audio
				stream_drop_audio( s );
			}
		}
	}

ErrorExit:	
	// run threads again
	thread_state_set( &s->audio_tstate,  THREAD_RUNNING );
	thread_state_set( &s->engine_tstate, THREAD_RUNNING );
	thread_state_set( &s->sub_tstate,    THREAD_RUNNING );
	
	stream_un_pause( s, was_paused );
	
	return 0;
}

// *****************************************************************************
//
//	stream_get_current_frame
//
// *****************************************************************************
VIDEO_FRAME *stream_get_current_frame( STREAM *s )
{
	return s ? s->current_frame : NULL;
}

#define DBGT if(0)

// *****************************************************************************
//
//	stream_get_time_default
//
// *****************************************************************************
int stream_get_time_default( STREAM *s, int *total )
{
	if ( !s )
		return 0;
	
	if( total )
		*total = s->duration;
	int time = s->video->valid ? s->video_time : s->audio_time;
DBGT serprintf("sgct  pos: %8d  tot %d\r\n", time, total ? *total : -1 );	
	return time;
}

// *****************************************************************************
//
//	stream_get_current_time
//
// *****************************************************************************
int stream_get_current_time( STREAM *s, int *total )
{
	if ( !s )
		return 0;
	
	if( s->parser && s->parser->get_time ) {
		return s->parser->get_time( s, total );
	}
	
	return stream_get_time_default( s, total );
}

// *****************************************************************************
//
//	stream_get_buffered_time
//
// *****************************************************************************
int stream_get_buffered_time( STREAM *s, int *total )
{
	if ( !s )
		return 0;

	int time = stream_get_current_time( s, total ) + s->time_parsed;
DBGT serprintf("sgbt  buf: %8d  tot %d\r\n", time, total ? *total : -1 );	
	return time / 1000 * 1000;	// round down to full seconds
}

// *****************************************************************************
//
//	stream_get_current_pos
//
// *****************************************************************************
int stream_get_current_pos( STREAM *s, int *total )
{	
	if ( !s )
		return 0;
	
	if( !s->no_duration )
		return stream_get_current_time( s, total );
		
	if( total )
		*total = STREAM_POS_MAX;
	
	if( s->size ) {
		UINT64 pos = s->video ? (s->video->valid ? s->video_pos : s->audio_pos) : s->audio_pos;
		int    ret = pos * STREAM_POS_MAX / s->size;
DBGT serprintf("sgcp  pos: %4d  tot %d\r\n", ret, total ? *total : -1 );	
		return ret;
	}
	return 0;
}

// *****************************************************************************
//
//	stream_get_buffered_pos
//
// *****************************************************************************
int stream_get_buffered_pos( STREAM *s, int *total )
{
	if ( !s )
		return 0;

	if( !s->no_duration )
		return stream_get_buffered_time( s, total );

	if( total )
		*total = STREAM_POS_MAX;
	
	if( s->size && s->buffer ) {
		UINT64 pos = s->video->valid ? s->video_pos : s->audio_pos; 
	       	pos       += stream_buffer_get_used( s->buffer );
		pos        = MIN( pos, s->size ); 
		int ret    = pos * STREAM_POS_MAX / s->size;
DBGT serprintf("sgbp  buf: %4d  tot %d\r\n", ret, total ? *total : -1 );	
		return ret;
	}
	return 0;
}

// *****************************************************************************
//
//	stream_get_current_audio_stream
//
// *****************************************************************************
int stream_get_current_audio_stream( STREAM *s )
{
	if ( !s )
		return 0;
	
	return s->av.as;
}

// *****************************************************************************
//
//	stream_get_audio_session_id
//
// *****************************************************************************
int stream_get_audio_session_id( STREAM *s )
{
	if ( !s || !s->audio_sink )
		return 0;
	
	return s->audio_sink->get_session_id( s );
}

// *****************************************************************************
//
//	stream_get_current_speed
//
// *****************************************************************************
int stream_get_current_speed( STREAM *s )
{
	return s ? s->speed : STREAM_SPEED_NORMAL;
}

// *****************************************************************************
//
//	_stream_get_real_time
//
// *****************************************************************************
static int _stream_get_real_time( STREAM *s, int time )
{	
	if( time < 0 )
		time = 0;
	return time;
}

// *****************************************************************************
//
//	_stream_redraw
//
//	force stream engine to output (redraw) the current frame
//
// *****************************************************************************
static int _stream_redraw( STREAM *s )
{
	if( !s->paused )
		return 1;

serprintf("stream_redraw\r\n");
	s->sink_ref_time = -1;
	s->drop          = 0;

	if( s->use_sink_frames ) {
		_stream_play_n_frames( s, 1, -1, 0 );
	} else {
		if( s->video_sink ) {
			s->video_sink->flush( s->video_sink );
			if( s->video_sink->clear ) {
				s->video_sink->clear( s->video_sink );
			}
		}

		if( s->current_frame ) {
			s->current_frame->time = s->video_time;
			s->output_frame_fn( s, s->current_frame, NULL );
		} else {
serprintf("CANNOT redraw\r\n");
			// output one frame by decoding one
			_stream_play_n_frames( s, 1, -1, 0 );
		}
	}
	return 0;	
}

// *****************************************************************************
//
//	stream_redraw
//
// *****************************************************************************
int stream_redraw( STREAM *s )
{
	if( !s )
		return 1;

	_stream_redraw( s );

	return 0;
}

// *****************************************************************************
//
//	stream_clear
//
//	force stream engine to clear the video output
//
// *****************************************************************************
int stream_clear( STREAM *s )
{
serprintf("stream_clear\r\n");

	if(s->video_sink && s->video_sink->clear) {
		s->video_sink->clear( s->video_sink );
	}
	
	return 0;	
}

// *****************************************************************************
//
//	stream_set_cpu_priority
//
// *****************************************************************************
void stream_set_cpu_priority( STREAM *s, int cpu_prio )
{
	s->cpu_prio = cpu_prio;
}

#ifdef DEBUG_MSG
// ********************************************************************************
// ********************************************************************************
//
//	D E B U G stuff
//
// ********************************************************************************
// ********************************************************************************
void *AV_get_ctx( void );

void stream_show_props( STREAM *s );

static void _stream_props( int argc, char *argv[] )  	
{ 
	STREAM *s = AV_get_ctx();
	if( s )
		stream_show_props( s );
}

static void _stream_delay_fb( int argc, char *argv[] )  	
{ 
	STREAM *s = AV_get_ctx();
	
	if( s && argc > 1) {
		s->delay_fb = atoi( argv[1] );
serprintf("stream_delay_fb: %d\r\n", s->delay_fb ); 
	}
}

static void _stream_force_max_dim( int argc, char *argv[] )
{
	if( argc > 2 ) {
		stream_force_max_width  = atoi( argv[1]);
		stream_force_max_height = atoi( argv[2]);
	} 
serprintf("stream_force_max_dim: %d x %d\r\n", stream_force_max_width, stream_force_max_height );
}

static void _stream_force_aspect_ratio( int argc, char *argv[] )
{
	if( argc > 2 ) {
		stream_force_aspect_n = atoi( argv[1]);
		stream_force_aspect_d = atoi( argv[2]);
	} else {
		stream_force_aspect_n = 0;
		stream_force_aspect_d = 0;
	}
serprintf("stream_force_aspect_ratio: %d x %d\r\n", stream_force_aspect_n, stream_force_aspect_d );
}

static void _stream_sync_mode( int argc, char *argv[] )  	
{ 
	if( stream_sync_mode == -1 )
		stream_sync_mode ++;
		
	stream_sync_mode = 1 - stream_sync_mode;
serprintf("stream_sync_mode: %s\r\n", stream_sync_mode ? "SAMPLES" : "CDATA" ); 
}

static void _stream_no_output( int argc, char *argv[] )  	
{ 
	stream_no_output = 1 - stream_no_output;
	stream_audio_paused = stream_no_output;
serprintf("stream_output: %s\r\n", stream_no_output ? "OFF" : "ON" ); 
}

extern int stream_parser_max;
static void _stream_parser_pause( int argc, char *argv[] )  	
{ 
	STREAM *s = AV_get_ctx();

	if( s && argc > 1 ) {
		int pause = atoi( argv[1] );
serprintf("stream_parser_pause: %d\r\n", pause ); 
		s->parser->pause( s, pause );
	}
}

static void _stream_no_audio( int argc, char *argv[] )  	
{ 
	stream_no_audio = 1 - stream_no_audio;
serprintf("stream_audio: %s\r\n", stream_no_audio ? "OFF" : "ON" ); 
}

static void _stream_no_subtitles( int argc, char *argv[] )  	
{ 
	stream_no_subtitles = 1 - stream_no_subtitles;
serprintf("stream_subtitles: %s\r\n", stream_no_subtitles ? "OFF" : "ON" ); 
}

static void _stream_no_video( int argc, char *argv[] )  	
{ 
	stream_no_video = 1 - stream_no_video;
serprintf("stream_video: %s\r\n", stream_no_video ? "OFF" : "ON" ); 
}

static void _stream_drop( int argc, char *argv[] )  	
{ 
	STREAM *s = AV_get_ctx();
	if( !s ) 
		return;
	s->drop = (argc > 1) ? atoi( argv[1] ) : 1;
}

static void _stream_drop_P( int argc, char *argv[] )  	
{ 
	STREAM *s = AV_get_ctx();
	if( !s ) 
		return;
	s->drop_P = 1;
}

static void _stream_double( int argc, char *argv[] )  	
{ 
	STREAM *s = AV_get_ctx();
	if( !s ) 
		return;
	s->drop = -1 * ((argc > 1) ? atoi( argv[1] ) : 1);
}

static void _stream_force_codec( int argc, char *argv[] )
{
	static char stream_force_codec_buf[256];
	if (argc > 1) {
		strncpy(stream_force_codec_buf, argv[1], 255);
		stream_force_codec = stream_force_codec_buf;
	} else {
		stream_force_codec = NULL;
	}
}

static void _stream_vtime_post_sink( int argc, char *argv[] )
{ 
	stream_vtime_post_sink = (stream_vtime_post_sink + 2) % 3 - 1 ;
serprintf("stream_vtime_post_sink: %d\r\n", stream_vtime_post_sink); 
}

extern AV_PROPERTIES *stream_force_audio_props;
static void _stream_force_audio_props( int argc, char *argv[] )  	
{ 
	STREAM *s = AV_get_ctx();

	if( !s ) 
		return;
		
	static AV_PROPERTIES props;
serprintf("_stream_force_audio_props\r\n" ); 
	memcpy( &props, &s->av, sizeof( AV_PROPERTIES ) );
	stream_force_audio_props = &props;
}

static void _stream_force_video_props( int argc, char *argv[] )  	
{ 
	STREAM *s = AV_get_ctx();

	if( !s ) 
		return;

	static AV_PROPERTIES props;
serprintf("_stream_force_video_props\r\n" ); 
	memcpy( &props, &s->av, sizeof( AV_PROPERTIES ) );
	stream_force_video_props = &props;
}

static void _stream_seek_time( int argc, char *argv[] )
{
	STREAM *s = AV_get_ctx();
	
	if ( s && argc > 1) {
		int time = atoi( argv[1] );
		int now  = stream_get_current_time( s, NULL );
		stream_seek_time( s, time, time > now ? STREAM_SEEK_FORWARD : STREAM_SEEK_BACKWARD, 0 );
	}
}

static void _stream_seek_pos( int argc, char *argv[] )
{
	STREAM *s = AV_get_ctx();

	if ( s && argc > 1) {
		int pos = atoi( argv[1] );
		int now = stream_get_current_pos( s, NULL );
		stream_seek_pos( s, pos, pos > now ? STREAM_SEEK_FORWARD : STREAM_SEEK_BACKWARD, 0 );
	}
}

static void _stream_seek_percent( int argc, char *argv[] )
{
	STREAM *s = AV_get_ctx();

	if ( s && argc > 1) {
		int total;
		int now = stream_get_current_pos( s, &total );
		int new = (int)((INT64)total * (INT64)atoi( argv[1] ) / 100);
		stream_seek_pos( s, new, new > now ? STREAM_SEEK_FORWARD : STREAM_SEEK_BACKWARD, 0 );
	}
}

static void _stream_seek_chapter( int argc, char *argv[] )
{
	STREAM *s = AV_get_ctx();

	if ( s && argc > 1) {
		int chapter = atoi( argv[1] );

		STREAM_CHAPTER ch;
		if( stream_seekable( s ) && stream_get_chapter( s, chapter, &ch ) ) {
			int new  = ch.start;
			int now  = stream_get_current_time( s, NULL );
			stream_seek_time( s, new, new > now ? STREAM_SEEK_FORWARD : STREAM_SEEK_BACKWARD, 0 );
		}
	}
}

static void _stream_force_error( int argc, char *argv[] )
{
	STREAM *s = AV_get_ctx();
	
	if( s ) {
		int ve  = VE_FILE_ERROR;
		int veq = VEQ_NONE;
		
		if( argc > 1 )
			ve = atoi( argv[1] );
		if (argc > 2 )
			veq = atoi( argv[2] );
			
		s->video_error = ve;
		s->video_error_qualifier = veq;
serprintf("stream_force_error %d / %d\r\n", ve, veq );
	} else {
		stream_force_error = 1;
	}
}

static void _stream_force_audio_filter_level( int argc, char *argv[] )
{
	if( argc > 2 ) {
		stream_force_audio_filter_level = atoi( argv[1] );
		stream_force_audio_filter_night_on = atoi( argv[2] );
	}
serprintf("stream_force_audio_filter_level: %d %d\n", stream_force_audio_filter_level, stream_force_audio_filter_night_on);
	
	STREAM *s = AV_get_ctx();
	if( s ) {
		stream_set_audio_filter_level( s, stream_force_audio_filter_level, stream_force_audio_filter_night_on );
	}	
}

static void _stream_fake_ts( int argc, char *argv[] )
{
	if( argc > 1 )
		stream_fake_ts_num = atoi( argv[1] );
	if (argc > 2 )
		stream_fake_ts_den = atoi( argv[2] );
	if (argc > 3 )
		stream_fake_ts_post = atoi( argv[3] );
			
serprintf("stream_fake_ts %d / %d [%s]\r\n", 
	stream_fake_ts_num, stream_fake_ts_den, stream_fake_ts_post ? "post" : "pre" );
}

static void _stream_user_abort( int argc, char *argv[] )
{
	STREAM *s = AV_get_ctx();
	
	if( s ) {
		stream_set_abort( s );
	}
}

static void _video_stop( int argc, char *argv[] )
{
	STREAM *s = AV_get_ctx();
	
	if( s )
		stream_stop( s );
}

static void _video_singlestep( int argc, char *argv[] ) 
{
	STREAM *s = AV_get_ctx();

	if( !s )
		return;

	_stream_play_n_frames( s, (argc > 1) ? atoi(argv[1]) : 1, -1, 0 );
serprintf("\r\n");
}

static void _parse_once( int argc, char *argv[] ) 
{
	STREAM *s = AV_get_ctx();
	
	if( s ) {
		s->parser_parse_once = (argc > 1 ) ? atoi(argv[1]) : 1;
	}
}

static void _stream_dropped_doubled(int argc, char *argv[] ) 
{ 
serprintf("DROPPED: %d  B_DROPPED %d  DOUBLED %d \r\n", frames_dropped, frames_B_dropped, frames_doubled );
}

static void _stream_prio_dump  (int argc, char *argv[] ) 
{
serprintf("stream_prio_audio  %d\r\n", stream_prio_audio);
serprintf("stream_prio_video  %d\r\n", stream_prio_video);
serprintf("stream_prio_engine %d\r\n", stream_prio_engine);
serprintf("stream_prio_parser %d\r\n", stream_prio_parser);
serprintf("stream_prio_sub    %d\r\n", stream_prio_sub);
}

static void _stream_queue_dump(int argc, char *argv[] ) 
{
	STREAM *s = AV_get_ctx();
	
	if( s ) {
		frame_q_dump2( &s->decode_q );
		frame_q_dump2( &s->codec_q  );
		frame_q_dump2( &s->disp_q   ); 
		frame_q_dump2( &s->locked_q );
		if( s->video_sink && s->video_sink->dump ) {
			s->video_sink->dump( s->video_sink );
		}
		serprintf("\r\n");
	}
}

static void _stream_gah(int argc, char *argv[] ) 
{
	STREAM *s = AV_get_ctx();
	
	if( s ) {
		int was_paused = stream_pause( s );
 
		if( argc > 1 ) {
			int index = atoi( argv[1] );
			VIDEO_FRAME *frame = frame_q_get_index( &s->codec_q, index );
			if( frame ) {
serprintf("freeing frame %d from codec\n");
				frame_q_put( &s->decode_q, frame );
			}
		}
		stream_un_pause( s, was_paused );
	}
}

static void _stream_un_pause_now( int argc, char *argv[] )  	
{ 
	STREAM *s = AV_get_ctx();
	if( s )
		stream_un_pause( s, 0 );
}

DECLARE_DEBUG_TOGGLE("score", 	do_core );

DECLARE_DEBUG_COMMAND("s", 	_video_singlestep );
DECLARE_DEBUG_COMMAND("pp", 	_parse_once );
DECLARE_DEBUG_COMMAND("vps", 	_video_stop );
DECLARE_DEBUG_COMMAND("sfe", 	_stream_force_error  );
DECLARE_DEBUG_TOGGLE ("sab", 	stream_force_abort );
DECLARE_DEBUG_COMMAND("sua", 	_stream_user_abort );
DECLARE_DEBUG_PARAM  ("sfps", 	stream_fps_mode );
DECLARE_DEBUG_TOGGLE ("sss", 	stream_screen_shot );

DECLARE_DEBUG_PARAM  ("sspr", 	stream_sink_preroll );
DECLARE_DEBUG_PARAM  ("ssmd", 	stream_sink_max_delay );
DECLARE_DEBUG_PARAM  ("skfo", 	stream_key_frame_only );

DECLARE_DEBUG_COMMAND("sst", 	_stream_seek_time );
DECLARE_DEBUG_COMMAND("ssp", 	_stream_seek_pos  );
DECLARE_DEBUG_COMMAND("ssx", 	_stream_seek_percent );
DECLARE_DEBUG_COMMAND("ssc", 	_stream_seek_chapter );
DECLARE_DEBUG_COMMAND("spp", 	_stream_props  );
DECLARE_DEBUG_PARAM  ("smd", 	stream_max_delay  );
DECLARE_DEBUG_TOGGLE ("ssy", 	stream_no_sync  );
DECLARE_DEBUG_COMMAND("sso", 	_stream_no_output  );
DECLARE_DEBUG_TOGGLE ("spa", 	stream_no_parser  );
DECLARE_DEBUG_PARAM  ("sps", 	stream_parser_sleep  );
DECLARE_DEBUG_COMMAND("spu", 	_stream_parser_pause );
DECLARE_DEBUG_PARAM  ("spm", 	stream_parser_max  );
DECLARE_DEBUG_PARAM  ("smb", 	stream_buffer_size  );
DECLARE_DEBUG_PARAM  ("sfb", 	stream_force_buffer );
DECLARE_DEBUG_PARAM  ("slb", 	stream_buffer_large );
DECLARE_DEBUG_TOGGLE ("svfa", 	stream_video_force_align );
DECLARE_DEBUG_TOGGLE ("sli", 	stream_no_late_idx );
DECLARE_DEBUG_PARAM  ("sdra", 	stream_force_drop_audio  );
DECLARE_DEBUG_COMMAND("saud", 	_stream_no_audio  );
DECLARE_DEBUG_COMMAND("svid", 	_stream_no_video  );
DECLARE_DEBUG_COMMAND("ssub", 	_stream_no_subtitles  );
DECLARE_DEBUG_TOGGLE ("sns", 	stream_no_seek );
DECLARE_DEBUG_TOGGLE ("sfv", 	stream_fake_video );
DECLARE_DEBUG_TOGGLE ("sfa", 	stream_fake_audio );
DECLARE_DEBUG_TOGGLE ("sfc", 	stream_fake_chapters  );
DECLARE_DEBUG_TOGGLE ("sror", 	stream_no_reorder);
DECLARE_DEBUG_TOGGLE ("sdbl", 	stream_no_deblock);
DECLARE_DEBUG_TOGGLE ("sdv", 	stream_dump_video );
DECLARE_DEBUG_TOGGLE ("sda", 	stream_dump_audio );
DECLARE_DEBUG_TOGGLE ("sdp", 	stream_dump_pcm );
DECLARE_DEBUG_COMMAND("sdel", 	_stream_delay_fb );
DECLARE_DEBUG_PARAM  ("svd", 	stream_vcodec_delay );
//DECLARE_DEBUG_PARAM  ("sad", 	stream_acodec_delay );
DECLARE_DEBUG_TOGGLE ("sap", 	stream_audio_paused );
DECLARE_DEBUG_TOGGLE ("svp", 	stream_video_paused );
DECLARE_DEBUG_TOGGLE ("szf", 	stream_zero_fill );
DECLARE_DEBUG_TOGGLE ("sif", 	stream_io_fail );
DECLARE_DEBUG_TOGGLE ("sih", 	stream_io_hang );
DECLARE_DEBUG_TOGGLE ("sfnl", 	stream_force_nonlocal );
DECLARE_DEBUG_TOGGLE ("sfp", 	stream_force_pauseable );
DECLARE_DEBUG_COMMAND("sfmd", 	_stream_force_max_dim );
DECLARE_DEBUG_COMMAND("sfar", 	_stream_force_aspect_ratio );
DECLARE_DEBUG_COMMAND("ssm", 	_stream_sync_mode );
DECLARE_DEBUG_COMMAND("svt", 	_stream_vtime_post_sink );
DECLARE_DEBUG_PARAM  ("samc", 	stream_audio_max_channels );
DECLARE_DEBUG_PARAM  ("ssvf", 	stream_sink_video_frames );
DECLARE_DEBUG_PARAM  ("ssvm", 	stream_sink_video_max );
DECLARE_DEBUG_PARAM  ("sbdt", 	stream_bdrop_threshold );
DECLARE_DEBUG_PARAM  ("spdt", 	stream_pdrop_threshold );
DECLARE_DEBUG_TOGGLE ("sbdf", 	stream_bdrop_force );
DECLARE_DEBUG_TOGGLE ("ssni", 	stream_seek_no_index  );
DECLARE_DEBUG_COMMAND("sfap", 	_stream_force_audio_props  );
DECLARE_DEBUG_COMMAND("sfvp", 	_stream_force_video_props  );
DECLARE_DEBUG_PARAM  ("stfps", 	stream_tweak_fps  );
DECLARE_DEBUG_COMMAND("sfts", 	_stream_fake_ts  );
DECLARE_DEBUG_COMMAND("sfaf",   _stream_force_audio_filter_level  );
DECLARE_DEBUG_TOGGLE ("sfde",   stream_force_sw_deinterlacing );

DECLARE_DEBUG_PARAM  ("stpa", 	stream_prio_audio  );
DECLARE_DEBUG_PARAM  ("stpv", 	stream_prio_video  );
DECLARE_DEBUG_PARAM  ("stpp", 	stream_prio_parser );
DECLARE_DEBUG_PARAM  ("stpe", 	stream_prio_engine );
DECLARE_DEBUG_PARAM  ("stps", 	stream_prio_sub    );
DECLARE_DEBUG_COMMAND("stp", 	_stream_prio_dump   );
DECLARE_DEBUG_COMMAND("sdd", 	_stream_dropped_doubled );
DECLARE_DEBUG_COMMAND("sdr", 	_stream_drop    );
DECLARE_DEBUG_COMMAND("sdrp", 	_stream_drop_P  );
DECLARE_DEBUG_COMMAND("sdo", 	_stream_double  );
DECLARE_DEBUG_COMMAND("sdq", 	_stream_queue_dump );
DECLARE_DEBUG_COMMAND ("scod",	_stream_force_codec);
DECLARE_DEBUG_PARAM  ("sfnf", 	stream_force_num_frames );
DECLARE_DEBUG_PARAM  ("sbsd", 	stream_buffer_sleep_datarate );
DECLARE_DEBUG_PARAM  ("sprio",  stream_force_prio);
DECLARE_DEBUG_PARAM  ("cmax",   codec_max);
DECLARE_DEBUG_TOGGLE ("shce",	stream_handle_codec_error);
DECLARE_DEBUG_TOGGLE ("svfr",	stream_video_force_reorder);
DECLARE_DEBUG_COMMAND("sgah",	_stream_gah);
DECLARE_DEBUG_TOGGLE ("sadm",	stream_audio_downmix);
DECLARE_DEBUG_TOGGLE ("sifu",	ignore_first_unpause);
DECLARE_DEBUG_COMMAND("sunp",	_stream_un_pause_now);
#endif

#endif
