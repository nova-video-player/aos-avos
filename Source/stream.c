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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <signal.h>

#ifdef CONFIG_STREAM
#define DBGV if(Debug[DBG_VID])
#define DBGS if(Debug[DBG_STREAM])
#define DBGP if(Debug[DBG_PARSER])

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
	
	// set pointer for "audio"/"video"
	av_init_props( s );
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
void stream_get_part_name( char *part_name, const char *full_path, int part_num )
{
	if( part_name ) {
		if( part_num > 0 ) {
			sprintf( part_name, "%s.%d", full_path, part_num + 1 );
		} else {
			sprintf( part_name, "%s", full_path );
		}
DBGS serprintf("stream_get_part_name( %d ) = %s\r\n", part_num, part_name );
	}
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
		stream_get_part_name( file, full_path, num );

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
		stream_get_part_name( file, s->src.url, i );

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
		
	s->av_delay = av_delay;
	
	return 0;
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
	if( s->audio_filter && s->audio_filter->set_param ) {
		s->audio_filter->set_param( s->audio_filter, &level, &night_on );
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
		for ( i = 0; i < SUB_STREAM_MAX && s->sub_url[i]; i++ ) {
			afree( s->sub_url[i] );
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
	while( i < SUB_STREAM_MAX && s->sub_url[i] ) {
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
	
	stream_get_part_name( res, path, num );
serprintf("stream_get_part_name( %s, %d ): %s\r\n", path, num, res ); 
}

static void _perform_stream_abort( void )
{
	STREAM *s = AV_get_type() == TYPE_VID ? AV_get_ctx() : NULL;
	if (s) {
		stream_set_abort(s);
	}
}

DECLARE_DEBUG_COMMAND("sbs", 	_stream_buffer_sec      );
DECLARE_DEBUG_COMMAND("sgpn", 	_stream_get_part_name   );
DECLARE_DEBUG_COMMAND_VOID("psa", _perform_stream_abort  );
#endif
#endif
