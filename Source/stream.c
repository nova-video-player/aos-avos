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

#include "audio_interface.h"
#include "types.h"
#include "global.h"
#include "stream.h"
#include "debug.h"
#include "util.h"
#include "astdlib.h"
#include "browse.h"
#include "power_hdd.h"
#include "stream_sync.h"

#include "athread.h"
#include "atime.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <signal.h>
#include <math.h>

extern int libavos_get_ac3_recoding_enabled(void);

#ifdef CONFIG_STREAM
#define DBGV DBG_IF(Debug[DBG_VID])
#define DBGS DBG_IF(Debug[DBG_STREAM])
#define DBGP DBG_IF(Debug[DBG_PARSER])

#define DBG DBG_IF(Debug[DBG_STREAM])
#define DBG2 DBG_IF(Debug[DBG_STREAM] > 1)

static void _free_chapters( STREAM *s );
static void _free_subtitle_urls( STREAM *s );

#ifdef CONFIG_ANDROID
#include "android_buffer.h"
STREAM_SINK_VIDEO *stream_sink_video_android_new(void *surface_handle);
STREAM_SINK_VIDEO *stream_sink_video_android2_new(void *surface_handle);
STREAM_SINK_VIDEO *stream_sink_video_android3_new(void *surface_handle);
#endif
extern STREAM_SINK_VIDEO *stream_sink_video_SIM_new( void );
extern STREAM_SINK_VIDEO *stream_sink_video_SIM2_new( void );
extern STREAM_SINK_VIDEO *stream_sink_video_FAKE_new( void );

extern STREAM_SINK_AUDIO stream_sink_audio;
extern STREAM_SINK_AUDIO stream_sink_audio_FAKE;

static int stream_use_fake_audio_sink    = 0;
static int stream_use_fake_video_sink    = 0;
static int stream_use_new_video_sink     = 1;

DECLARE_DEBUG_TOGGLE ("sfvs", 	stream_use_fake_video_sink );
DECLARE_DEBUG_TOGGLE ("sfas", 	stream_use_fake_audio_sink );
DECLARE_DEBUG_TOGGLE ("snvs", 	stream_use_new_video_sink );

// buffer used as cache before parser to tackle buffering issues in MB
// history 2015 12->24MB (20 NOK) for high bitrate 4k streaming
static int default_stream_buffer_size = 24;
// to cope with video frames size (can be HUGE, needs to be increased with resolution increase)
// history 2019 *2 again for H264 4K peak rates -> 1024 * 1536 * 4, 2015 *2 for H265 4K -> 1024 * 1536 * 2, 2011 1024 * 1024 -> 1024 * 1536 for HD frames
// 1024 * 1536 * 4 = 6 * 1024 * 1024
static int default_video_mindata_size = VIDEO_MINDATA_SIZE;


// ************************************************************
//
//	stream_get_default_video_sink
//
// ***********************************************************
STREAM_SINK_VIDEO *stream_get_default_video_sink( STREAM *s )
{
#ifdef CONFIG_ANDROID
	void *surface = stream_get_surface_handle(s);
	serprintf("stream_get_default_video_sink: %p\n", surface);
	if (surface) {
		if( stream_use_new_video_sink ) {
			if( android_surface_check_gralloc(surface) ) {
				return stream_sink_video_android2_new(surface);
			} else {
				return stream_sink_video_android3_new(surface);
			}
		} else {
			return stream_sink_video_android_new(surface);
		}
	} else {
		return stream_sink_video_FAKE_new();
	}
#endif

	if( stream_use_fake_video_sink ) {
		return stream_sink_video_FAKE_new();
	}
#ifdef CONFIG_FB_QVFB
	if( stream_use_new_video_sink ) {
		return stream_sink_video_SIM2_new();
	} else {
		return stream_sink_video_SIM_new();
	}
#endif
	return NULL;
}

// ************************************************************
//
//	stream_get_default_audio_sink
//
// ***********************************************************
STREAM_SINK_AUDIO *stream_get_default_audio_sink( void )
{
	if( stream_use_fake_audio_sink ) {
		return &stream_sink_audio_FAKE;
	}
	return &stream_sink_audio;
}

// ************************************************************
//
//	stream_put_chunk_cache
//
// ***********************************************************
int stream_put_chunk_cache( STREAM_CHUNK_CACHE *cc, CBE *cbe, STREAM_CDATA *cdata )
{
	if( cc->write == CHUNK_CACHE_MAX || cc->use_cache ) {
		return 1;
	}

//DBGS serprintf("put_chunk_cache[%2d] size %d\n", cc->write, cdata->size );
	cc->data[cc->write] = amalloc( cdata->size );
	memcpy( cc->data[cc->write], cbe_get_tail_p( cbe, cdata->size ), cdata->size );
	cc->cdata[cc->write] = *cdata;
	cc->write++;
	cc->read = cc->write;

	return 0;
}

// ************************************************************
//
//	stream_get_chunk_cache
//
// ***********************************************************
int stream_get_chunk_cache( STREAM_CHUNK_CACHE *cc, CBE *cbe, STREAM_CDATA *cdata )
{
	if( cc->read == cc->write ) {
		// stop at end of cache
		cc->use_cache = 0;
	}
	if( !cc->use_cache ) {
		return 1;
	}
	
	*cdata = cc->cdata[cc->read];
//DBGS serprintf("get_chunk_cache[%2d] size %d\n", cc->read, cdata->size );
	cbe_write( cbe, cc->data[cc->read], cdata->size );
	cc->read++;
	
	return 0;
}

// ************************************************************
//
//	stream_free_chunk_cache
//
// ***********************************************************
static void stream_free_chunk_cache( STREAM_CHUNK_CACHE *cc )
{
	int i;
	for( i = 0; i < cc->write; i++ ) { 
DBGS serprintf("stream_free_chunk_cache[%2d]\n", i);
		afree( cc->data[i] );	
	}
	cc->read      = 0;
	cc->write     = 0;
	cc->use_cache = 0;
}

// *****************************************************************************
//
//	_stream_reset
//
// *****************************************************************************
static void _stream_reset( STREAM *s )
{
	if( !s )
		return;
		
	// clear it, so it can be reopened!
	memset( s, 0, sizeof( STREAM ) );
	s->vol_l = s->vol_r = AUDIO_VOLUME_MAX;
	s->cpu_prio = STREAM_CPU_ANY;
	s->video_speed_num = 100;
	s->video_speed_den = 100;
	
	// set pointer for "audio"/"video"
	av_init_props( s );
	memset( &s->audio_sink_props, 0, sizeof( AUDIO_PROPERTIES ) );
	s->audio_time = -1;
	s->video_time = -1;
	s->audio_ref_time = -1;
}

static int stream_buffer_sec  = 64;

// ************************************************************
//
//	stream_new
//
// ***********************************************************
STREAM *stream_new( void )
{
	STREAM *s = (STREAM*) amalloc( sizeof( STREAM ) );
DBGS serprintf("stream_new: %08X\r\n", s );
	if( !s )
		return NULL;
		
	_stream_reset( s );
	
	// set this default value here!
	stream_set_buffer_chunk( s, stream_buffer_sec * 512 );

	return s;	 
}

// ************************************************************
//
//	stream_delete
//
// ***********************************************************
int stream_delete( STREAM **s )
{
DBGS serprintf("stream_delete: %08X\r\n", s ? (long)*s : -1 );
	if( !s || !*s )
		return 1;
	stream_url_clear( &(*s)->src );
	// See the comment in stream_close(): deferred this far so any last
	// stream_sub_ext_close() call for this stream is guaranteed to already
	// be done, same as mode2_heard_mutex's own deferred destroy.
	pthread_mutex_destroy( &(*s)->subtitle_owner_lock );
	pthread_cond_destroy(  &(*s)->subtitle_owner_cond );
	// subtitle_table_lock (stream.h): same deferred-destroy reasoning as
	// subtitle_owner_lock above -- stream_stop()'s stream_sub_ext_close()
	// waits for the discovery worker before returning, and that happens
	// after stream_close() but before here, so it's safe to destroy now.
	pthread_mutex_destroy( &(*s)->subtitle_table_lock );
	afree( *s );
	*s = NULL;
	return 0;
}

static int ref_count = 0;

// ************************************************************
//
//	stream_init
//
// ***********************************************************
int stream_init( STREAM *s )
{
	if ( !s )
		return 1;

DBGS serprintf("stream_init\r\n" );
	
	// defaults:
	s->num_parts     = 1;
	s->get_part_name = stream_get_part_name;
	
	pthread_mutex_init( &s->drm_lock, NULL );

	pthread_mutex_init( &s->codec_mutex,       NULL );
	pthread_mutex_init( &s->video_done_mutex,  NULL );
	pthread_mutex_init( &s->audio_sink_mutex,  NULL );
	// Serializes video-sink calls from the audio thread with sink teardown.
	pthread_mutex_init( &s->video_sink_mutex,  NULL );
	pthread_mutex_init( &s->anchor_mutex,      NULL );
	pthread_mutex_init( &s->mode2_heard_mutex, NULL );
	pthread_mutex_init( &s->subtitle_owner_lock, NULL );
	pthread_cond_init(  &s->subtitle_owner_cond, NULL );
	pthread_mutex_init( &s->subtitle_table_lock, NULL );
	
	ref_count ++;
	return 0;
}

// ************************************************************
//
//	stream_close
//
// ***********************************************************
int stream_close( STREAM *s )
{
	if ( !s )
		return 1;
	
DBGS serprintf("stream_close\r\n");
	if( !s->open ) {
serprintf("s not open!\r\n");
		return 1;
	}
	
DBGS serprintf("waiting for threads to join\r\n");
	if( thread_state_get( &s->engine_tstate ) != THREAD_EXIT ) {
		thread_state_set( &s->engine_tstate, THREAD_EXIT );
		apthread_join( s->engine_thread_handle, NULL );
DBGS serprintf("player_thread joined\r\n");
	}
	
	if( thread_state_get( &s->parser_tstate ) != THREAD_EXIT ) {
		thread_state_set( &s->parser_tstate, THREAD_EXIT );
		apthread_join( s->parser_thread_handle, NULL );
DBGS serprintf("parser_thread joined\r\n");
	}
	
	if( thread_state_get( &s->audio_tstate ) != THREAD_EXIT ) {
		thread_state_set( &s->audio_tstate, THREAD_EXIT );
		apthread_join( s->audio_thread_handle, NULL );
DBGS serprintf("audio_thread joined\r\n");
	}

	if( thread_state_get( &s->sub_tstate ) != THREAD_EXIT ) {
		thread_state_set( &s->sub_tstate, THREAD_EXIT );
		apthread_join( s->sub_thread_handle, NULL );
DBGS serprintf("sub_thread joined\r\n");
	}

	if ( s->codec_run ) {
		s->codec_run = 0;
	
		pthread_mutex_lock( &s->codec_mutex );
		pthread_cond_broadcast( &s->codec_code );
		pthread_mutex_unlock( &s->codec_mutex );
	
		apthread_join( s->codec_thread_handle, NULL );
DBGS serprintf("codec_thread joined\r\n");
	}
	
	pthread_mutex_destroy( &s->codec_mutex  );
	pthread_mutex_destroy( &s->video_done_mutex );
	pthread_mutex_destroy( &s->audio_sink_mutex );
	// mode2_heard_mutex is also read by the asynchronous video renderer. Its
	// lifetime therefore extends until stream_stop() has joined decoder/sink
	// threads, not just the core stream threads joined above; destroy it there.
	// subtitle_owner_lock/subtitle_owner_cond guard subtitle_priv's validity
	// for stream_sub_ext.c's parse-worker pool (see their doc comment in
	// stream.h) and must, by the same reasoning, stay alive until
	// stream_sub_ext_close() has made its very last call for this stream --
	// which this function has no visibility into the timing of. Destroyed
	// in stream_delete() instead, once the STREAM itself is being freed and
	// nothing can still be resolving against it.
	
	s->open = 0;
	
	int ret = 0;
	// close parser if we have one
	if( s->parser ) {
		ret = s->parser->close( s );
	}
	
	_free_chapters( s );
	_free_subtitle_urls( s );
	
	stream_free_chunk_cache( &s->cc );
	
	ref_count --;
	
	return ret;
}

// ************************************************************
//
//	stream_get_part_name
//
// ***********************************************************
int stream_get_part_name( char *part_name, const char *full_path, int part_num )
{
	int ret;
	if( !part_name ) {
		return 1;
	}
	if( part_num > 0 ) {
		ret = snprintf( part_name, STREAM_MAX_PATH_LEN + 1, "%s.%d", full_path ? full_path : "", part_num + 1 );
	} else {
		ret = snprintf( part_name, STREAM_MAX_PATH_LEN + 1, "%s", full_path ? full_path : "" );
	}
	if (ret < 0 || ret > STREAM_MAX_PATH_LEN) {
		part_name[0] = '\0';
		return 1;
	}
DBGS serprintf("stream_get_part_name( %d ) = %s\r\n", part_num, part_name );
	return 0;
}

// *****************************************************************************
//
//	 stream_check_parts
//
// *****************************************************************************
int stream_check_parts( const char *full_path )
{
	int num = 0;
	
	for( num = 1; num < STREAM_MAX_PARTS; num ++ ) {
		char file[STREAM_MAX_PATH_LEN + 1];
		if( stream_get_part_name( file, full_path, num ) ) {
DBGP serprintf("part name overflow for %d\r\n", num );
			break;
		}

		STAT st;
		if( !file_stat( file, &st ) ) {
DBGP serprintf("found %s\r\n", file );		
		} else {
			break;
		}
	}
DBGP serprintf("found %d parts\r\n", num );
	return num;	
}

// *****************************************************************************
//
//	 stream_parse_parts
//
// *****************************************************************************
int stream_parse_parts( STREAM *s )
{
	int i;
	for( i = 0; i < s->num_parts; i ++ ) {
		char file[STREAM_MAX_PATH_LEN + 1];
		if( stream_get_part_name( file, s->src.url, i ) ) {
			stream_set_error( s, VE_FILE_ERROR );
			return 1;
		}

		STAT st;
		file_stat( file, &st );
		s->parts[i].real_size = st.st_size;
		s->parts[i].pad_size  = pad_to_buffer_chunk( s, st.st_size);
serprintf("real %8d  pad %8d\r\n", s->parts[i].real_size, s->parts[i].pad_size );
	}
	s->size = 0;

	for( i = 0; i < s->num_parts; i++ ) {
		s->size += s->parts[i].pad_size;
	}
serprintf("total size %lld\r\n", s->size );

	return 0;
}

// ************************************************************
//
//	stream_get_index
//
// ***********************************************************
int stream_get_index( STREAM *s, int *time, void **data, int *size )
{
	if( data )
		*data = NULL;
	if( size )
		*size = 0;

	if ( !s || !s->parser->get_index )
		return 1;
	
	return s->parser->get_index( s, time, data, size );
}

// ************************************************************
//
//	stream_set_crypt
//
// ************************************************************
int stream_set_crypt( STREAM *s, int crypt, void *key )
{
DBGS serprintf("stream_set_crypt: %d\r\n", crypt );
	if( !s || !key )
		return 1;
		
	s->crypt     = crypt;
	s->crypt_key = key;
	return 0;
}

// ************************************************************
//
//	stream_set_stop_handler
//
// ************************************************************
int stream_set_stop_handler( STREAM *s, STOP_HANDLER stop )
{
	if( !s )
		return 1;
		
	s->stop = stop;
	
	return 0;
}

// ************************************************************
//
//	stream_set_progress_handler
//
// ************************************************************
int stream_set_progress_handler( STREAM *s, PROGRESS_HANDLER progress )
{
	if( !s )
		return 1;
		
	s->progress = progress;
	
	return 0;
}

// ************************************************************
//
//	stream_set_abort_handler
//
// ************************************************************
int stream_set_abort_handler( STREAM *s, ABORT_HANDLER abort )
{
	if( !s )
		return 1;
		
	s->user_abort = abort;
	
	return 0;
}

// ************************************************************
//
//	stream_set_per_frame_handler
//
// ************************************************************
int stream_set_per_frame_handler( STREAM *s, PER_FRAME_HANDLER per_frame )
{
	if( !s )
		return 1;
		
	s->per_frame = per_frame;
	
	return 0;
}

// ************************************************************
//
//	stream_set_av_delay
//
// ************************************************************
int stream_set_av_delay( STREAM *s, int av_delay )
{
	if( !s )
		return 1;

	int passthrough_mode = (s->audio_sink && s->audio_sink->get_passthrough)
		? s->audio_sink->get_passthrough( s ) : 0;
	if( av_delay < 0 && passthrough_mode ) {
		serprintf("stream_set_av_delay: negative av_delay=%d is not supported with passthrough mode %d\n",
			av_delay, passthrough_mode);
	}
		
	s->av_delay = av_delay;
	s->manual_audio_delay_target_ms = (av_delay < 0) ? -av_delay : 0;
	if( av_delay >= 0 ) {
		s->manual_audio_delay_applied_ms = 0;
		s->manual_audio_hold_pending_ms = 0;
	}
	// Snapshot the current realized A/V phase and open a short diagnostic window
	// so the applied shift is unmistakable in the log: manual_delay_applied lines
	// report frame_minus_heard against this baseline as the hold takes effect.
	s->manual_delay_fmh_baseline = s->manual_delay_fmh_last;
	s->manual_delay_log_until_ms = atime() + 5000;
DBG serprintf("stream_set_av_delay: av_delay=%d manual_target=%d applied=%d baseline_fmh=%d\r\n",
		av_delay, s->manual_audio_delay_target_ms, s->manual_audio_delay_applied_ms,
		s->manual_delay_fmh_baseline);

	return 0;
}

// ************************************************************
//
//	stream_set_av_speed
//
// ************************************************************
extern void _stream_resync( STREAM *s );

static void _stream_anchor_video_sink_to_audio_clock( STREAM *s, int audio_time_ts )
{
	if( !s || audio_time_ts < 0 )
		return;

	pthread_mutex_lock( &s->video_sink_mutex );
	if( s->video_sink && s->video_sink->is_open && s->video_sink->put_time )
		s->video_sink->put_time( s->video_sink, audio_time_ts );
	pthread_mutex_unlock( &s->video_sink_mutex );
	DBG serprintf( "stream:stream_set_av_speed anchored video sink to audio_ts=%d put_time=%d av_delay=%d\n",
		audio_time_ts, audio_time_ts, stream_sync_av_delay( s ) );
}

static int _stream_get_speed_anchor_ts( STREAM *s, int current_time_ts, int heard_ts,
	int speed_changed, int using_atempo, int *used_current_ts, int *used_last_good )
{
	int use_current = 0;
	int use_last_good = 0;
	int anchor_ts = heard_ts;

	if( s && s->audio_ctx ) {
		int delay_valid = audio_interface_is_delay_valid( s->audio_ctx );
		if( !delay_valid && speed_changed && s->last_good_delay_valid && s->audio_time >= 0 ) {
			int effective_delay = s->last_good_delay_ms;
			if( using_atempo && s->audio_filter_atempo && s->audio_filter_atempo->delay ) {
				effective_delay += s->audio_filter_atempo->delay( s->audio_filter_atempo );
				if( effective_delay < 0 ) {
					effective_delay = 0;
				}
			}
			anchor_ts = s->audio_time - effective_delay;
			if( anchor_ts < 0 ) {
				anchor_ts = 0;
			}
			use_last_good = 1;
		} else if( !delay_valid && s->video_sink && s->video_sink->name && strcmp(s->video_sink->name, "sfdec2") == 0 ) {
			// sfdec2 timed pacing: if delay is invalid, heard_ts can lag far behind stream time.
			// Using it for timeline_map_apply bakes in large skew during speed changes.
			// Fall back to the current stream time until delay is valid.
			use_current = 1;
		}
	}

	if( used_current_ts ) {
		*used_current_ts = use_current;
	}
	if( used_last_good ) {
		*used_last_good = use_last_good;
	}
	return use_current ? current_time_ts : anchor_ts;
}

int stream_set_av_speed( STREAM *s, float av_speed )
{
	if( !s ) return 1;

	if( !audio_interface_is_audio_speed_enabled() ) {
		DBG serprintf( "stream:stream_set_av_speed audio speed disabled %f\n", av_speed );
		return 0;
	}
	int ac3_recoding = 0;
	int passthrough = 0;
#ifdef CONFIG_AUDIO_AC3
	ac3_recoding = libavos_get_ac3_recoding_enabled();
#endif
	if( s->audio_sink ) {
		passthrough = s->audio_sink->get_passthrough( s );
	}

	// MediaCodec is an asynchronous vendor decoder and is not guaranteed to
	// deliver PCM faster than real time. A faster consumer can then starve
	// AudioTrack, regardless of whether atempo or PlaybackParams changes the
	// speed. Keep this decoder strictly at 1x; callers may choose ffmpeg if
	// variable-speed playback is required.
	if( !passthrough && !ac3_recoding &&
		fabsf( av_speed - 1.0f ) > 1e-6f && s->audio_dec &&
		s->audio_dec->name && !strcmp( s->audio_dec->name, "MediaCodec" ) ) {
		float current_speed = audio_interface_get_audio_speed();
		if( fabsf( current_speed - 1.0f ) > 1e-6f &&
			!audio_interface_is_using_atempo() && s->audio_ctx ) {
			audio_interface_change_audio_speed( s->audio_ctx, 1.0f );
		}
		audio_interface_set_audio_speed( 1.0f );
		serprintf("stream:stream_set_av_speed rejected %.3fx: variable speed is disabled with MediaCodec audio\n",
			av_speed);
		return 1;
	}

	int using_atempo = (s->audio_filter_atempo != NULL);
	if (!audio_interface_is_audio_speed_enabled() || !audio_interface_is_using_atempo()) {
		using_atempo = 0;
	}
	if( passthrough || ac3_recoding ) {
		using_atempo = 0;
	}
	audio_interface_set_using_atempo( using_atempo );
	DBG2 serprintf("stream:stream_set_av_speed gate req=%.3f speed_enabled=%d filter=%p using_atempo_pref=%d effective_using_atempo=%d current_speed=%.3f\n",
		av_speed, audio_interface_is_audio_speed_enabled(), s->audio_filter_atempo,
		audio_interface_is_using_atempo(), using_atempo, audio_interface_get_audio_speed());

	int audio_latency_ms = -1;
	if( s ) {
		audio_latency_ms = stream_get_anchor_delay_ms( s, 1 );
	}
	float previous_speed = audio_interface_get_audio_speed();
	int speed_changed = is_audio_speed_changed( av_speed );

	int current_time_ts = s->video->valid ? s->video_time : s->audio_time;
	if( current_time_ts < 0 ) {
		current_time_ts = 0;
	}
	int anchor_ts = stream_get_heard_audio_ts( s, current_time_ts );
	int use_current_ts_for_speed = 0;
	int use_last_good_for_speed = 0;
	int speed_anchor_ts = _stream_get_speed_anchor_ts( s, current_time_ts, anchor_ts,
		speed_changed, using_atempo, &use_current_ts_for_speed, &use_last_good_for_speed );
	int stream_current_time_rst = TS_TO_RST_TIME( speed_anchor_ts, int );
	if( stream_current_time_rst < 0 ) {
		stream_current_time_rst = 0;
	}

	// Check if video is actively playing.
	int video_active = (s->video_dec && s->video_dec->set_playback_speed && s->video && s->video->valid);

	if( speed_changed ) {
		s->audio_speed_diag_epoch++;
		s->audio_speed_diag_writes_left = 20;
		int delay_valid = s->audio_ctx ? audio_interface_is_delay_valid( s->audio_ctx ) : 1;
		int delay_streak = s->audio_ctx ? audio_interface_get_delay_valid_streak( s->audio_ctx ) : 0;
		int atempo_delay = 0;
		if( using_atempo && s->audio_filter_atempo && s->audio_filter_atempo->delay ) {
			atempo_delay = s->audio_filter_atempo->delay( s->audio_filter_atempo );
		}
		DBG serprintf( "stream:stream_set_av_speed delay_valid=%d streak=%d v=%d a=%d heard_ts=%d av_delay=%d\n",
			delay_valid, delay_streak, s->video_time, s->audio_time, anchor_ts, stream_sync_av_delay( s ) );
		DBG serprintf( "stream:stream_set_av_speed speed_change prev=%.3f target=%.3f using_atempo=%d atempo_delay=%d use_current_ts=%d cur_ts=%d anchor_ts=%d speed_anchor_ts=%d\n",
			previous_speed, av_speed, using_atempo, atempo_delay, use_current_ts_for_speed,
			current_time_ts, anchor_ts, speed_anchor_ts );
		DBG serprintf( "stream:stream_set_av_speed epoch=%d writes_budget=%d\n",
			s->audio_speed_diag_epoch, s->audio_speed_diag_writes_left );
		DBG serprintf( "stream:stream_set_av_speed snapshot last_good=%d last_good_valid=%d last_good_atempo=%d hist=%d sink_driven=%d\n",
			s->last_good_delay_ms, s->last_good_delay_valid,
			s->last_good_atempo_delay_ms, s->av_delay_history_count,
			(s->video_sink && s->video_sink->put_time) ? 1 : 0 );
		if( use_last_good_for_speed ) {
			int current_atempo_delay = 0;
			if( using_atempo && s->audio_filter_atempo && s->audio_filter_atempo->delay ) {
				current_atempo_delay = s->audio_filter_atempo->delay( s->audio_filter_atempo );
			}
			DBG serprintf( "stream:stream_set_av_speed speed_change using last_good_delay=%d last_good_atempo=%d cur_atempo=%d (audio_time=%d anchor_ts=%d)\n",
				s->last_good_delay_ms, s->last_good_atempo_delay_ms, current_atempo_delay, s->audio_time, anchor_ts );
		}
	}

	DBG serprintf( "stream:stream_set_av_speed anchor_ts=%d speed_anchor_ts=%d, anchor_rst=%d (audio_latency_ms=%d video_active=%d)\n",
		anchor_ts, speed_anchor_ts, stream_current_time_rst, audio_latency_ms, video_active );

	float applied_speed = av_speed;
	int defer_commit = 0;
	if( using_atempo ) {
		float clamped_speed = av_speed;
		if( clamped_speed < 0.5f ) {
			clamped_speed = 0.5f;
		} else if( clamped_speed > 2.0f ) {
			clamped_speed = 2.0f;
		}
		// Set the global speed — the atempo filter reads it on its next filter() call.
		audio_interface_set_audio_speed( clamped_speed );
		if( speed_changed ) {
			s->atempo_ledger_dense_until_ms = atime() + 2000;
		}
		DBG serprintf( "stream:stream_set_av_speed apply atempo speed=%.3f (audio_time=%d video_time=%d)\n",
			clamped_speed, s->audio_time, s->video_time );
		// The sink still holds queued old-speed output.  Switching the video
		// timeline now would make video advance at the new media rate while the
		// speaker still plays old-speed content for the drain duration, leaving a
		// permanent A/V offset of queue_ms * delta_speed per step.  Defer the
		// video-side commit until the playhead crosses the boundary where new-speed
		// content begins (stream_atempo_commit_poll).
		if( speed_changed && video_active && s->atempo_ledger_active ) {
			UINT64 flt_out = 0;
			int flt_fifo = 0, flt_rate = 0;
			stream_filter_audio_atempo_get_ledger_stats( s->audio_filter_atempo,
				&flt_out, &flt_fifo, &flt_rate );
			// Diag "prev" is the speed in effect just before this step: the
			// last queued checkpoint's target if a ramp is in flight, else the
			// currently committed mapping speed.
			float prev_for_diag = previous_speed;
			if( s->atempo_commit_count > 0 ) {
				int tail = ( s->atempo_commit_head + s->atempo_commit_count - 1 ) % STREAM_ATEMPO_COMMIT_MAX;
				prev_for_diag = s->atempo_commit_q[tail].speed;
			}
			if( s->atempo_commit_count >= STREAM_ATEMPO_COMMIT_MAX ) {
				// Full (pathological ramp); drop the oldest to make room.
				s->atempo_commit_head = ( s->atempo_commit_head + 1 ) % STREAM_ATEMPO_COMMIT_MAX;
				s->atempo_commit_count--;
				serprintf( "atempo_commit_overflow: queue full, dropped oldest\n" );
			}
			int slot = ( s->atempo_commit_head + s->atempo_commit_count ) % STREAM_ATEMPO_COMMIT_MAX;
			s->atempo_commit_q[slot].speed      = clamped_speed;
			s->atempo_commit_q[slot].prev_speed = prev_for_diag;
			s->atempo_commit_q[slot].boundary   = s->atempo_ledger_output_frames +
				(UINT64)(flt_fifo > 0 ? flt_fifo : 0);
			s->atempo_commit_q[slot].wall_ms    = atime();
			s->atempo_commit_count++;
			defer_commit = 1;
			DBG serprintf( "atempo_commit_arm: prev=%.3f target=%.3f boundary=%llu out_cursor=%llu flt_fifo=%d audio=%d anchor_ts=%d qlen=%d\n",
				prev_for_diag, clamped_speed,
				(unsigned long long)s->atempo_commit_q[slot].boundary,
				(unsigned long long)s->atempo_ledger_output_frames,
				flt_fifo, s->audio_time, speed_anchor_ts, s->atempo_commit_count );
		} else if( s->atempo_commit_count == 0 || speed_changed ) {
			// Same-speed re-anchor calls must not touch the mapping while
			// commits are pending (global speed already holds the pending
			// target).  A genuine speed change that cannot be deferred (no
			// ledger/video) supersedes the queue: drop it, apply immediately.
			s->atempo_commit_count = 0;
			s->atempo_commit_head  = 0;
			timeline_map_apply( (double)stream_current_time_rst, (double)speed_anchor_ts, clamped_speed );
			DBG serprintf( "stream:stream_set_av_speed using atempo filter WITH timeline mapping, anchor_rst=%d anchor_ts=%d, speed=%.3f\n",
					   stream_current_time_rst, speed_anchor_ts, clamped_speed );
		}
		applied_speed = clamped_speed;
	} else {
		s->atempo_commit_count = 0;
		s->atempo_commit_head  = 0;
		if( speed_changed ) {
			// Arm the epoch before changing speed so any heard_ts calls during
			// audio_interface_change_audio_speed() see the new anchor immediately.
			// Speed field is updated to applied_speed once the call returns.
			if( !passthrough && !ac3_recoding && s->audio_ctx && s->audio_time >= 0 ) {
				UINT64 ep_frames = 0;
				int ep_rate = 0, ep_src = 0, ep_age = 0;
				int ep_frames_valid = audio_interface_get_presented_frames( s->audio_ctx, &ep_frames, &ep_rate, &ep_src, &ep_age, 1 );
				if( !ep_frames_valid ) {
					s->at_speed_epoch_active = 0;
					DBG serprintf( "at_speed_epoch_arm: skipped no_playhead audio=%d anchor_ts=%d speed=%.3f\n",
						s->audio_time, anchor_ts, av_speed );
				} else {
					int epoch_wall_ms = atime();
					s->at_speed_epoch_active            = 1;
					s->at_speed_epoch_audio_time_ts     = s->audio_time;
					s->at_speed_epoch_heard_ts          = anchor_ts;
					s->at_speed_epoch_speed             = av_speed;
					s->at_speed_epoch_presented_frames  = ep_frames;
					s->at_speed_epoch_rate              = ep_rate;
					s->at_speed_epoch_wall_ms           = epoch_wall_ms;
					s->at_speed_epoch_frames_cached     = ep_frames;
					s->at_speed_epoch_cache_wall_ms     = epoch_wall_ms;
					DBG {
						int live_delay_ms = stream_sync_av_delay( s );
						serprintf( "at_speed_epoch_arm: audio=%d anchor_ts=%d live_delay=%d speed=%.3f frames=%llu rate=%d src=%d age=%d\n",
							s->audio_time, anchor_ts, live_delay_ms, av_speed,
							(unsigned long long)ep_frames, ep_rate, ep_src, ep_age );
					}
				}
			}
			int rc = audio_interface_change_audio_speed( s->audio_ctx, av_speed );
			applied_speed = audio_interface_get_audio_speed();
			if( s->at_speed_epoch_active ) {
				s->at_speed_epoch_speed = applied_speed;
			}
			DBG serprintf( "stream:stream_set_av_speed applied seamless speed change, anchor_rst=%d anchor_ts=%d, applied_speed=%f rc=%d\n",
					   stream_current_time_rst, speed_anchor_ts, applied_speed, rc );
			if( fabsf( applied_speed - av_speed ) > 1e-6f ) {
				serprintf( "stream:stream_set_av_speed requested=%.3f applied=%.3f (rc=%d)\n",
					   av_speed, applied_speed, rc );
			}
		} else {
			applied_speed = previous_speed;
			DBG serprintf( "stream:stream_set_av_speed no audio hw change required (speed=%f)\n", applied_speed );
		}
		timeline_map_apply( (double)stream_current_time_rst, (double)speed_anchor_ts, applied_speed );
	}

	int applied_num = (int)( applied_speed * 100 + 0.5f );
	int applied_den = 100;
	applied_num = MAX( 1, applied_num );
	if( !defer_commit ) {
		s->video_speed_num = applied_num;
		s->video_speed_den = applied_den;
	}

	if( video_active && !defer_commit ) {
		DBG serprintf( "stream:stream_set_av_speed set_playback_speed den=%d num=%d requested=%.3f applied=%.3f (v=%d a=%d anchor_delay=%d)\n",
			applied_den, applied_num, av_speed, applied_speed,
			s->video_time, s->audio_time, stream_get_anchor_delay_ms( s, 1 ) );
		s->video_dec->set_playback_speed( s->video_dec, applied_den, applied_num );
	}

	int seek_time_ts = anchor_ts;
	int lead_ms = 0;
	if( s->video && s->video->msPerFrame > 0 ) {
		lead_ms = s->video->msPerFrame * 4;
	}
	if( lead_ms > 0 ) {
		seek_time_ts += lead_ms;
	}
	if( seek_time_ts <= 0 ) {
		seek_time_ts = current_time_ts;
	}
	int seek_time_rst = seek_time_ts > 0 ? TS_TO_RST_TIME( seek_time_ts, int ) : -1;
	if( seek_time_rst <= 0 ) {
		seek_time_rst = stream_get_current_time( s, NULL );
		if( seek_time_rst <= 0 && seek_time_ts > 0 ) {
			seek_time_rst = TS_TO_RST_TIME( seek_time_ts, int );
		}
	}
	if( s->video->valid && !defer_commit ) {
		int is_sfdec2 = (s->video_sink && s->video_sink->name && strcmp(s->video_sink->name, "sfdec2") == 0);
		if( is_sfdec2 ) {
			// For sfdec2 timed rendering, always seed the anchor when audio_time exists.
			if( s->audio_time != -1 ) {
				int delay_valid = s->audio_ctx ? audio_interface_is_delay_valid( s->audio_ctx ) : 1;
				if( delay_valid ) {
					_stream_anchor_video_sink_to_audio_clock( s, anchor_ts );
				} else {
					// Defer re-anchoring on speed change until delay is valid to avoid catch-up bursts
					int delay_streak = s->audio_ctx ? audio_interface_get_delay_valid_streak( s->audio_ctx ) : 0;
					DBG serprintf("stream:stream_set_av_speed defer anchor (delay invalid, streak=%d v=%d a=%d heard_ts=%d)\n",
						delay_streak, s->video_time, s->audio_time, anchor_ts);
				}
			} else {
				DBG serprintf( "stream:stream_set_av_speed defer anchor (audio_time=%d)\n", s->audio_time );
			}
		} else {
			_stream_anchor_video_sink_to_audio_clock( s, anchor_ts );
		}
	}

	if( speed_changed ) {
		s->av_delay_history_count = 0;
		if( s->audio_ctx ) {
			audio_interface_invalidate_delay_cache( s->audio_ctx );
		}
	}

	return 0;
}

// Apply the deferred atempo video-side speed commit once the audio playhead
// crosses the output-frame boundary where new-speed content begins.
// Called from stream_sync_audio (audio sync path) on every audio write.
void stream_atempo_commit_poll( STREAM *s )
{
	if( !s || s->atempo_commit_count <= 0 )
		return;
	UINT64 playhead = 0;
	int rate = 0, src = 0, age = 0;
	int have_ph = ( s->audio_ctx && audio_interface_get_presented_frames( s->audio_ctx,
			&playhead, &rate, &src, &age, 1 ) );

	// Anchor for every checkpoint promoted in this poll is the heard clock now:
	// once a boundary is crossed, that step's old-speed content has been played,
	// so the video timeline catches up to the current audible position.
	int anchor_ts = stream_get_heard_audio_ts( s, s->audio_time );
	if( anchor_ts < 0 )
		anchor_ts = s->audio_time >= 0 ? s->audio_time : 0;
	int anchor_rst = TS_TO_RST_TIME( anchor_ts, int );
	if( anchor_rst < 0 )
		anchor_rst = 0;

	float applied_speed = 0.0f;
	int applied_any = 0;

	// Promote front checkpoints strictly in order while their boundary is
	// crossed (or they have timed out).  Several may have crossed since the
	// last poll; drain them in sequence — the final one sets the live state.
	while( s->atempo_commit_count > 0 ) {
		int idx = s->atempo_commit_head;
		STREAM_ATEMPO_COMMIT *cp = &s->atempo_commit_q[idx];
		int waited_ms = atime() - cp->wall_ms;
		int crossed;
		if( cp->boundary == STREAM_ATEMPO_COMMIT_BOUNDARY_DEFER ) {
			// Post-reset collapsed commit: the old ledger frame domain is gone, so a
			// frame boundary is meaningless.  Apply only once the NEW ledger is active
			// and the audible playhead resolves strictly inside a block (state==0), so
			// the flip below anchors to the fresh ledger RST and not the stale map.
			int ledger_state = 0;
			int ledger_rst = have_ph
				? stream_atempo_ledger_lookup_rst( s, playhead, rate, &ledger_state )
				: STREAM_NO_PTS_VALUE;
			crossed = ( ledger_rst != STREAM_NO_PTS_VALUE && ledger_rst >= 0 && ledger_state == 0 );
		} else {
			crossed = have_ph && playhead >= cp->boundary;
		}
		// Timeout safety: if the playhead stalls or becomes unavailable (pause,
		// sink recreation), fall back to immediate apply.  Strict ordering: if
		// the front is not ready, stop — never skip ahead to a later step.
		if( !crossed && waited_ms < 3000 )
			break;
		s->atempo_commit_head = ( idx + 1 ) % STREAM_ATEMPO_COMMIT_MAX;
		s->atempo_commit_count--;
		applied_speed = cp->speed;
		applied_any = 1;
		DBG serprintf( "atempo_commit_apply: prev=%.3f speed=%.3f boundary=%llu playhead=%llu crossed=%d waited=%d anchor_ts=%d anchor_rst=%d audio=%d video=%d qlen=%d\n",
			cp->prev_speed, cp->speed,
			(unsigned long long)cp->boundary,
			(unsigned long long)playhead, crossed, waited_ms,
			anchor_ts, anchor_rst, s->audio_time, s->video_time,
			s->atempo_commit_count );
	}
	if( !applied_any )
		return;

	// Anchor the timeline to the ledger's filter-paced media/RST clock at the
	// audible playhead, instead of the TS_TO_RST_TIME() projection of anchor_ts.
	// The projection re-derives RST through the timeline's committed (pre-step)
	// speed, so it leads the audible playhead by the sink+wrapper+ring backlog and
	// the lead accumulates across ramp-up steps.  Only flip when the playhead
	// resolves strictly INSIDE a ledger block (state==0); stale (-1) or
	// extrapolated (+1) lookups have a weaker media slope, so fall back to the
	// projection there.  anchor_ts and the video-sink anchor stay unchanged.
	int anchor_rst_use = anchor_rst;
	{
		int ledger_state = 0;
		int anchor_rst_ledger = STREAM_NO_PTS_VALUE;
		if( have_ph ) {
			anchor_rst_ledger = stream_atempo_ledger_lookup_rst( s, playhead, rate, &ledger_state );
		}
		int flipped = ( anchor_rst_ledger != STREAM_NO_PTS_VALUE && anchor_rst_ledger >= 0 && ledger_state == 0 );
		if( flipped ) {
			anchor_rst_use = anchor_rst_ledger;
		}
		DBG serprintf( "atempo_rst_anchor: speed=%.3f anchor_ts=%d anchor_rst_proj=%d anchor_rst_ledger=%d state=%d flipped=%d playhead=%llu have_ph=%d\n",
			applied_speed, anchor_ts, anchor_rst, anchor_rst_ledger, ledger_state,
			flipped, (unsigned long long)playhead, have_ph );
	}

	timeline_map_apply( (double)anchor_rst_use, (double)anchor_ts, applied_speed );

	int num = (int)( applied_speed * 100 + 0.5f );
	num = MAX( 1, num );
	s->video_speed_num = num;
	s->video_speed_den = 100;
	if( s->video_dec && s->video_dec->set_playback_speed && s->video && s->video->valid ) {
		s->video_dec->set_playback_speed( s->video_dec, 100, num );
	}
	if( s->video && s->video->valid ) {
		_stream_anchor_video_sink_to_audio_clock( s, anchor_ts );
	}
}


// ************************************************************
//
//	stream_drive_sleep
//
// ************************************************************
int stream_drive_sleep( STREAM *s )
{
	if ( !s )
		return 1;

	if( !(s->flags & STREAM_FILE_NONLOCAL) ) {
#ifdef CONFIG_HARDWARE
		power_set_hdd_SYS_poweroff();
#endif
		return 0;
	}
	return 1;
}

// ************************************************************
//
//	stream_CDATA_from_SC
//
// ************************************************************
void stream_CDATA_from_SC( STREAM_CDATA *cdata, STREAM_CHUNK *c )
{
	if ( !cdata || !c )
		return;
		
	memset( cdata, 0, sizeof( STREAM_CDATA ) );
	cdata->type       = c->type;
	cdata->size       = c->size;
	cdata->time       = c->time;
	cdata->key        = c->key;
	cdata->frm_type   = c->frm_type;
	cdata->frame      = c->frame;
	cdata->pos        = c->pos;
}

// ************************************************************
//
//	stream_set_message_cb
//
// ************************************************************
void stream_set_message_cb( STREAM *s, STREAM_MESSAGE_CB message_cb )
{
	if ( !s )
		return;
DBGS serprintf("stream_set_message_cb\r\n");
	s->message_cb = message_cb;
}

// ************************************************************
//
//	stream_set_ask_audio
//
// ************************************************************
void stream_set_ask_audio( STREAM *s, STREAM_ASK_AUDIO ask )
{
	if ( !s )
		return;
DBGS serprintf("stream_set_ask_audio\r\n");
	s->ask_audio = ask;
}

// ************************************************************
//
//	stream_set_ask_video
//
// ************************************************************
void stream_set_ask_video( STREAM *s, STREAM_ASK_VIDEO ask )
{
	if ( !s )
		return;
DBGS serprintf("stream_set_ask_video\r\n");
	s->ask_video = ask;
}

// ************************************************************
//
//	stream_set_size
//
// ************************************************************
void stream_set_size( STREAM *s, UINT64 size )
{
	if( !s )
		return;
DBGS serprintf("stream_set_size: %ld\r\n", size );
	s->size	= size;
}

// ************************************************************
//
//	stream_set_buffer_size
//
// ************************************************************
void stream_set_buffer_size( STREAM *s, int buffer_size )
{
	if( !s )
		return;
DBGS serprintf("stream_set_buffer_size: %ld\r\n", buffer_size );
	s->buffer_size = buffer_size;
}

// ************************************************************
//
//	stream_set_buffer_flags
//
// ************************************************************
void stream_set_buffer_flags( STREAM *s, int flags, void *opaque )
{
	if( !s )
		return;
DBGS serprintf("stream_set_buffer_flags: %d/%08X\r\n", flags, opaque );
	s->buffer_flags  = flags;
	s->buffer_opaque = opaque;
}

// ************************************************************
//
//	stream_set_aspect_ratio
//
// ************************************************************
void stream_set_aspect_ratio( STREAM *s, int aspect_n, int aspect_d )
{
	if( !s )
		return;
DBGS serprintf("stream_set_aspect_ratio: %d x %d\r\n", aspect_n, aspect_d );
	s->aspect_n = aspect_n;
	s->aspect_d = aspect_d;
}

// ************************************************************
//
//	stream_set_audio_filter_level
//
// ************************************************************
int stream_set_audio_filter_level( STREAM *s, int level, int night_on )
{
	if( !s ) {
		return 1;
	}
	
	s->audio_filter_enabled = 1;
	s->audio_filter_level   = level;
	s->audio_filter_night_on = night_on;
	// Apply to all active filters
	if( s->audio_filter && s->audio_filter->set_param ) {
		s->audio_filter->set_param( s->audio_filter, &level, &night_on );
	}
	if( s->audio_filter_compress && s->audio_filter_compress->set_param ) {
		s->audio_filter_compress->set_param( s->audio_filter_compress, &level, &night_on );
	}
	if( s->audio_filter_ac3 && s->audio_filter_ac3->set_param ) {
		s->audio_filter_ac3->set_param( s->audio_filter_ac3, &level, &night_on );
	}
	return 0;
}

// ************************************************************
//
//	stream_set_max_video_dimensions
//
// ************************************************************
void stream_set_max_video_dimensions( STREAM *s, int max_width, int max_height )
{
	if( !s )
		return;
DBGS serprintf("stream_set_max_video_dimensions: %d x %d\r\n", max_width, max_height );
	s->video_max_width  = max_width;
	s->video_max_height = max_height;
}

// ************************************************************
//
//	stream_set_audio_max_channels
//
// ************************************************************
void stream_set_audio_max_channels( STREAM *s, int max_channels )
{
	if( !s )
		return;
DBGS serprintf("stream_set_audio_max_channels: %d\r\n", max_channels );
	s->audio_max_channels = max_channels;
}

// ************************************************************
//
//	stream_set_drm_ctx
//
// ************************************************************
void stream_set_drm_ctx( STREAM *s, DRM_CTX *drm_ctx )
{
	if( !s || !drm_ctx )
		return;
DBGS serprintf("stream_set_drm_ctx %08X\r\n", drm_ctx );
	memcpy( &s->drm_ctx, drm_ctx, sizeof( DRM_CTX ) );
}

// ************************************************************
//
//	stream_set_start_time
//
// ************************************************************
void stream_set_start_time( STREAM *s, int time )
{
	if( !s )
		return;
DBGS serprintf("stream_set_start_time: %d\r\n", time );
	s->start_time = time;
}

// ************************************************************
//
//	stream_set_stop_time
//
// ************************************************************
void stream_set_stop_time( STREAM *s, int time )
{
	if( !s )
		return;
DBGS serprintf("stream_set_stop_time: %d\r\n", time );
	s->stop_time  = time;
}

// ************************************************************
//
//	stream_set_video_sink
//
// ************************************************************
void stream_set_video_sink( STREAM *s, STREAM_SINK_VIDEO *sink )
{
	if( !s )
		return;
DBGS serprintf("stream_set_video_sink: %s\r\n", sink ? sink->name : "(NULL)s" );
	s->video_sink = sink;
}

// ************************************************************
//
//	stream_get_video_sink
//
// ************************************************************
STREAM_SINK_VIDEO *stream_get_video_sink( STREAM *s )
{
	return s ? s->video_sink : NULL;
}

// ************************************************************
//
//	stream_set_audio_sink
//
// ************************************************************
void stream_set_audio_sink( STREAM *s, STREAM_SINK_AUDIO *sink )
{
	if( !s )
		return;
DBGS serprintf("stream_set_audio_sink: %s\r\n", sink ? sink->name : "(NULL)s" );
	s->audio_sink = sink;
}

// ************************************************************
//
//	stream_get_audio_sink
//
// ************************************************************
STREAM_SINK_AUDIO *stream_get_audio_sink( STREAM *s )
{
	return s ? s->audio_sink : NULL;
}

// ************************************************************
//
//	stream_set_user_ctx
//
// ************************************************************
void stream_set_user_ctx( STREAM *s, void *ctx )
{
	if( !s )
		return;
DBGS serprintf("stream_set_user_ctx: %08x\r\n", ctx );

	s->user_ctx = ctx;
}

// ************************************************************
//
//	stream_get_user_ctx
//
// ************************************************************
void *stream_get_user_ctx( STREAM *s)
{
	if( !s )
		return NULL;
		
	return s->user_ctx;
}

// ************************************************************
//
//	stream_get_tag_state
//
// ************************************************************
int stream_get_tag_state( STREAM *s, int *tag_new, int *apic_new )
{
	if( !s )
		return 1;
		
	if( tag_new ) {
		*tag_new = s->tag_new;
	}	
	
	if( apic_new ){
		*apic_new = s->apic_new;
	}
	
	return 0;
}

// ************************************************************
//
//	stream_get_tag
//
// ************************************************************
int stream_get_tag( STREAM *s, ID3_TAG *tag, APIC *apic )
{
	if( !s )
		return 1;

	if( tag ) {
		s->tag_new = 0;
		memcpy( tag, &s->tag, sizeof( ID3_TAG ) );
	}
	
	if( apic ){
	 	s->apic_new = 0;
		memset( apic, 0, sizeof( APIC ) );
		if( s->apic.valid && s->apic.buffer && s->apic.buffer_size && s->apic.size ) {
			apic->buffer = amalloc( s->apic.buffer_size );
			if( apic->buffer ) {
				memcpy( apic->buffer, s->apic.buffer, s->apic.buffer_size );
				apic->buffer_size = s->apic.buffer_size;
				apic->size        = s->apic.size;
				apic->etype       = s->apic.etype;
				apic->valid       = s->apic.valid; 
			}
		}
	}

	return 0;
}

// ************************************************************
//
//	stream_add_chapter
//
// ************************************************************
int stream_add_chapter( STREAM *s, UINT64 start, UINT64 end, const char *title )
{
	if( !s || s->num_chapters >= STREAM_MAX_CHAPTERS )
		return 1;

	STREAM_CHAPTER *c = s->chapters[s->num_chapters++] = amalloc( sizeof( STREAM_CHAPTER ) );
	if( !c )
		return 1;

	c->start = start;
	c->end   = end;
	strnZcpy( c->title, title, MAX_TAG_LENGTH );

	return 0;
}

// ************************************************************
//
//	stream_set_audio_name
//
// ************************************************************
void stream_set_audio_name( AUDIO_PROPERTIES *audio, int track_num )
{
	sprintf(audio->name, "Track %d", track_num );
}

// ************************************************************
//
//	stream_set_subtitle_name
//
// ************************************************************
void stream_set_subtitle_name( SUB_PROPERTIES *subtitle, int sub_num )
{
	sprintf(subtitle->name, "Subtitle %d", sub_num );
}

// ************************************************************
//
//	_free_chapters
//
// ************************************************************
static void _free_chapters( STREAM *s )
{
	int i;
	for( i = 0; i < s->num_chapters; i++ ) {
		if( s->chapters[i] ) {
			afree( s->chapters[i] );
			s->chapters[i] = NULL;
		}
	}
	s->num_chapters = 0;
}

// ************************************************************
//
//	_free_subtitle_urls
//
// ************************************************************
static void _free_subtitle_urls( STREAM *s )
{
	if( s ) {
		int i;
		for ( i = 0; i < SUB_TRACK_MAX + 2; i++ ) {
			if( s->sub_url[i] ) {
				afree( s->sub_url[i] );
				s->sub_url[i] = NULL;
			}
		}
	}
}

// ************************************************************
//
//	stream_get_chapter
//
// ************************************************************
int stream_get_chapter( STREAM *s, int num, STREAM_CHAPTER *chapter )
{
	if( !s || num >= s->num_chapters )
		return 0;

	if( chapter ) {
		*chapter = *s->chapters[num];
		if( chapter->end == -1 ) {
			// no end given use start of next one:
			if( num < s->num_chapters - 1 ) {
				chapter->end = s->chapters[num + 1]->start;
			} else {
				chapter->end = s->duration;
			}
		}
DBGS serprintf("stream_get_chapter(%2d)  %8lld -> %8lld  %s\r\n", num, chapter->start, chapter->end, chapter->title );
	}
	return s->num_chapters;
}

// ************************************************************
//
//	stream_set_error
//
// ************************************************************
int stream_set_error( STREAM *s, int error )
{
	if( !s ) {
		return 1;
	}

	if( s->aborted )
		return 0;

DBGS serprintf("stream_set_error: %d\r\n", error );
	if ( error == VE_USER_ABORT ) {
		s->video_error = error;
	}
	
	if ( s->video_error != VE_USER_ABORT ) {
		s->video_error = error;
	}

	return 0;
}

// ************************************************************
//
//	stream_get_error
//
// ************************************************************
int stream_get_error( STREAM *s )
{
	if( !s ) {
		return 0;
	}

	return s->video_error;
}

// ************************************************************
//
//	stream_abort
//
// ************************************************************
int stream_abort( STREAM *s )
{
	if( s->abort && s->abort( s ) ) {
		if( !s->aborted ) {
serprintf("stream: USER abort!\r\n");
			stream_set_error( s, VE_USER_ABORT );
			s->aborted = 1;
		}
	}

	return s->aborted;
}

// ************************************************************
//
//	stream_set_abort
//
// ************************************************************
int stream_set_abort( STREAM *s )
{
	if( !s )
		return 1;

	s->video_error = VE_USER_ABORT;
	s->video_error_qualifier = VEQ_NONE;

	s->aborted = 1;
	return 0;
}

// ************************************************************
//
//	stream_set_volume
//
// ************************************************************
int stream_set_volume( STREAM *s, int vol_l, int vol_r )
{
	s->vol_l = vol_l;
	s->vol_r = vol_r;
	if( s->audio_sink && s->audio_sink->set_vol )
		s->audio_sink->set_vol(s);
	return 0;
}

// *****************************************************************************
//
//	stream_set_surface_handle
//
// *****************************************************************************
void stream_set_surface_handle( STREAM *s, void *surface_handle )
{
	s->surface_handle = surface_handle;
}

// *****************************************************************************
//
//	stream_get_surface_handle
//
// *****************************************************************************
void *stream_get_surface_handle( STREAM *s )
{
	return s->surface_handle;
}

// ************************************************************
//
//	stream_get_total_rate
//
// ************************************************************
int stream_get_total_rate( STREAM *s )
{
	int total_rate = 0;
	int duration   = 0;

	if( s->audio->valid && s->video->valid ) {
		// audio and video
		if( s->audio->bytesPerSec && s->video->bytesPerSec ) {
			total_rate = s->audio->bytesPerSec + s->video->bytesPerSec;	
		}
		duration = s->video->duration ? s->video->duration : s->audio->duration;
	} else if( s->audio->valid ) {
		// audio only
		total_rate = s->audio->bytesPerSec;	
		duration   = s->audio->duration;
	} else if( s->video->valid ) {
		// video only
		total_rate = s->video->bytesPerSec;	
		duration   = s->video->duration;
	}
DBGS serprintf("stream: total rate: %6d  (from a/v bytesPerSec)\r\n", total_rate );

	if( !total_rate ) {
		// guess a rate from size and duration
		if( s->size && duration ) {
			total_rate = 1000 *(UINT64)s->size / (UINT64)duration;
DBGS serprintf("stream: total rate: %6d  (from size and duration)\r\n", total_rate );
		}
	}
	return total_rate;
}

// *****************************************************************************
//
//	stream_show_props
//
// *****************************************************************************
void stream_show_props( STREAM *s ) 
{
	serprintf("\n");
	serprintf("stream: [%s]\r\n", s->src.url);
	int i = 0;
	while( i < SUB_TRACK_MAX && s->sub_url[i] ) {
		serprintf(" sub_url:  [%s]\r\n", s->sub_url[i] );
		i++;
	}
	if( s->parser ) {
		serprintf("  parser:  [%s]\r\n", s->parser->name );
	}
	
	int hh,mm,ss = s->duration / 1000;
	sec_to_hms( &hh, &mm, &ss );

	serprintf("  %sduration %d  %02d:%02d:%02d\r\n", s->no_duration ? "NO ": "", s->duration, hh, mm, ss);
	serprintf("  size     %lld\r\n", s->size);
	serprintf("  index    %d\r\n", s->has_index);
	serprintf("  drm      %d\r\n", s->drm.drm);
	serprintf("  rate     %d\r\n", s->data_rate);
	serprintf("  seekable %d\r\n", stream_seekable( s ) );

	show_av_props( &s->av );
	
	if( s->audio->valid && s->video->valid ) {
		serprintf("a2v:\r\n");
		serprintf("  delay  %d\r\n", stream_sync_av_delay( s ) );
	}
	serprintf("  a_dvr  %d\r\n\r\n", s->archos_dvr );
	
	if( s->tag.valid ) {
		serprintf("  artist [%s]\r\n", s->tag.artist);
		serprintf("  album  [%s]\r\n", s->tag.album );
		serprintf("  title  [%s]\r\n", s->tag.title);
		serprintf("  genre  [%s]\r\n", s->tag.genre);
		serprintf("  year   [%s]\r\n", s->tag.year);
	}

	if( s->num_chapters ) {
		int i;
		serprintf("chapters:\r\n");	
		for( i =0; i < s->num_chapters; i++ ) {
			STREAM_CHAPTER *ch = s->chapters[i];
			serprintf("  [%2d] start/end %8lld/%8lld  (%s)\r\n", i+1, ch->start, ch->end, ch->title );
		}
		serprintf("\r\n");
	}
}

// *****************************************************************************
//
//	stream_show_rc
//
// *****************************************************************************
void stream_show_rc( STREAM_RC *rc ) 
{
	serprintf("rc:\r\n");
	serprintf("  min_clock  %d\r\n", rc->min_clock );
	serprintf("  mem_type   %s\r\n", rc->mem_type ? "DMA" : "NRM" );
	serprintf("  mem_size   %d\r\n", rc->mem_size );
	serprintf("  out_cached %s\r\n", rc->output_cached ? "CACHED" : "NON-CACHED" );
	serprintf("  num_frames %d\r\n", rc->num_frames );
	serprintf("  cpu_type   %d -> %s\r\n", rc->cpu_type, rc->cpu_type == STREAM_CPU_ARM ? "ARM" : "HW" );
}

// *****************************************************************************
//
//	stream_show_short_props
//
// *****************************************************************************
void stream_show_short_props( STREAM *s ) 
{
	if( s->parser )
		serprintf("PARSER: [%s]  buffersize %d\r\n", s->parser->name, s->buffer ? s->buffer->buffer_size : -1 );
	if( s->video->valid ) {
		serprintf("VIDEO:  [%.4s] [%s] %dx%d  %2dfps  %dkbit/s  dec [%s]\r\n", 	
				(UCHAR*)&s->video->fourcc, 
				video_get_fourcc_name(s->video),
				s->video->width, s->video->height,
				s->video->framesPerSec, 
				s->video->bytesPerSec / 125,
				s->video_dec ? s->video_dec->name : "(none)"  
		);
	}
	if( s->audio->valid ) {
		int i;
		for( i = 0; i < s->av.as_max; i++ ) {
			AUDIO_PROPERTIES *audio = s->av.audio + i;
			serprintf("AUDIO:  [%04X] [%s] %5dHz  %d-ch %dbit  %dkbit/s%s  dec [%s]\r\n", 
				audio->format, 
				audio_get_format_name( audio ),
				audio->samplesPerSec,
				audio->channels,
				audio->bitsPerSample,
				audio->bytesPerSec / 125,
				audio->vbr ? "  VBR" : "",
				s->audio_dec ? s->audio_dec->name : "(none)"  
			);
		}
	}
	// Locked -- debug output only, but see subtitle_table_lock (stream.h).
	pthread_mutex_lock( &s->subtitle_table_lock );
	if( s->subtitle->valid ) {
		int i;
		for( i = 0; i < s->av.subs_max; i++ ) {
			SUB_PROPERTIES *sub = s->av.sub + i;
			serprintf("SUB:    [%04X] [%s] %s gfx %d  ext %d  %d/%d\r\n", 
				sub->format, 
				sub_get_format_name( sub ),
				sub->name,
				sub->gfx,
				sub->ext,
				sub->scale,
				sub->rate
			);
		}
	}
	pthread_mutex_unlock( &s->subtitle_table_lock );
}

// *****************************************************************************
//
//	stream_show_flags
//
// *****************************************************************************
void stream_show_flags( int flags )
{
	struct FLAG_NAME {
		int flag;
		const char *name; 
	} f[] = {
		{STREAM_PAUSED, 	"PAUSED" },	      	
		{STREAM_THUMB, 		"THUMB" },	      	
		{STREAM_NO_AUDIO, 	"NO_AUDIO" },	      	
		{STREAM_NO_INDEX, 	"NO_INDEX" },	      	
		{STREAM_LOOP, 		"LOOP" },	      	
		{STREAM_THUMB_PLAY, 	"THUMB_PLAY" },   	
		{STREAM_NO_AUDIO_CODEC, "NO_AUDIO_CODEC" }, 	
		{STREAM_NO_VIDEO_CODEC, "NO_VIDEO_CODEC" }, 	
		{STREAM_UNCUT, 		"UNCUT" },	      	
		{STREAM_LATE_INDEX, 	"LATE_INDEX" },   	
		{STREAM_MULTI, 		"MULTI" },      	

		{STREAM_NO_VIDEO, 	"NO_VIDEO" },	      	
		{STREAM_NO_DEINT, 	"NO_DEINT" },	      	
		{STREAM_FILE_NONLOCAL ,	"FILE_NONLOCAL" },	
		{STREAM_THUMB_DRM, 	"THUMB_DRM" },  	
		{STREAM_NOT_INTERLEAVED,"NOT_INTERLEAVED" },
		{STREAM_NO_SUBTITLES, 	"NO_SUBTITLES" }, 	
		{STREAM_TIMESHIFT, 	"TIMESHIFT" },  	
		{STREAM_RESIZE_BUFFER, 	"RESIZE_BUFFER" },
		{0, 			"" },  	
	};

serprintf("flags:");	
	int i = 0 ;
	while ( f[i].flag ) {
		if( flags & f[i].flag ) {
serprintf("  %s", f[i].name );		
		}
		i++;  
	}
serprintf("\r\n");	
}

#ifdef DEBUG_MSG
#include "app_av.h"
void asignal( int sig );

static void _stream_buffer_sec( int argc, char *argv[] )
{ 
	if( argc > 1 ) {
		stream_buffer_sec = atoi( argv[1] );
	} else {
		stream_buffer_sec = 64;
	}
serprintf("stream_buffer_sec: %d\r\n", stream_buffer_sec ); 
}

static void _stream_get_part_name( int argc, char *argv[] )
{ 
	if( argc < 3 ) {
		return;
	}
	
	char *path = argv[1];
	int num    = atoi(argv[2]);
	char	res[STREAM_MAX_PATH_LEN + 1];
	
	if( stream_get_part_name( res, path, num ) ) {
serprintf("stream_get_part_name( %s, %d ): overflow\r\n", path, num ); 
	} else {
serprintf("stream_get_part_name( %s, %d ): %s\r\n", path, num, res ); 
	}
}

static void _perform_stream_abort( void )
{
	STREAM *s = AV_get_type() == TYPE_VID ? AV_get_ctx() : NULL;
	if (s) {
		stream_set_abort(s);
	}
}

void define_default_stream_buffer_size(int size)
{
	DBG serprintf("stream:define_default_stream_buffer_size %d\n", size);
	default_stream_buffer_size = size;
}

int get_default_stream_buffer_size()
{
	DBG serprintf("stream:get_default_stream_buffer_size %d\n", default_stream_buffer_size);
	return default_stream_buffer_size;
}

void define_default_stream_max_iframe_size(int size)
{
	DBG serprintf("stream:define_default_stream_max_iframe_size %d\n", size);
	default_video_mindata_size = size * 1024 * 1024;
}

int get_default_stream_max_iframe_size()
{
	DBG serprintf("stream:get_default_stream_max_iframe_size %d\n", default_video_mindata_size);
	return default_video_mindata_size;
}

DECLARE_DEBUG_COMMAND("sbs", 	_stream_buffer_sec      );
DECLARE_DEBUG_COMMAND("sgpn", 	_stream_get_part_name   );
DECLARE_DEBUG_COMMAND_VOID("psa", _perform_stream_abort  );
#endif
#endif
