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

// ************************************************************
// AUDIO player backend
// ************************************************************

#ifndef _AUDIO_H
#define _AUDIO_H

#include "av.h"
#include "id3tag.h"
#include "athread.h"
#include "cbe.h"
#include "stream_io.h"
#include "audio_interface.h"

// fwd declaration
struct AUDIO;

enum {
	MENU_SOURCE_MIC			= 0,
	MENU_SOURCE_JMIC 		= 1,
	MENU_SOURCE_LINE		= 2,
}; 

typedef enum {
	SOURCE_NONE			= 0,
	SOURCE_MIC			= (1 << 0),
	SOURCE_JMIC 			= (1 << 1),
	SOURCE_LINE			= (1 << 2),
	SOURCE_BLUETOOTH		= (1 << 3),
	SOURCE_FM			= (1 << 4),
	SOURCE_ANALOG_VOICE_MIC		= (1 << 5),
	SOURCE_ANALOG_VOICE_JMIC	= (1 << 6),
	SOURCE_DIGITAL_VOICE		= (1 << 7),
	NB_AUDIO_SOURCE
} AUDIO_SOURCE;

typedef enum {
	HARDWARE_OUTPUT_NONE		= 0,
	HARDWARE_OUTPUT_SPEAKER		= (1 << 0),
	HARDWARE_OUTPUT_EARPIECE 	= (1 << 1),
	HARDWARE_OUTPUT_HEADPHONES	= (1 << 2),
	HARDWARE_OUTPUT_LINE		= (1 << 3),
	HARDWARE_OUTPUT_BLUETOOTH_SCO	= (1 << 4),
	HARDWARE_OUTPUT_BLUETOOTH_A2DP	= (1 << 5),
	HARDWARE_OUTPUT_FM		= (1 << 6),
	HARDWARE_OUTPUT_SPDIF_PCM	= (1 << 7),
	HARDWARE_OUTPUT_SPDIF_AC3THRU	= (1 << 8),
	HARDWARE_OUTPUT_HDMI_PCM	= (1 << 9),
	HARDWARE_OUTPUT_HDMI_AC3THRU	= (1 << 10),
	HARDWARE_OUTPUT_ANALOG_VOICE	= (1 << 11),
	HARDWARE_OUTPUT_DIGITAL_VOICE	= (1 << 12),
	NB_HARDWARE_OUTPUT
} AUDIO_HARDWARE_OUTPUT;

#define AUDIO_OVERLAP_SIZE      (256 * 1024)
#define AUDIO_APIC_LOAD_SIZE	(512 * 512 * 2)

#define AUDIOREC_MINFREE_MB	64
#define AUDIOREC_MAXSIZE	( 2000 * 1024 * 1024 )

enum {
	PHASE_TRANS = 0,
	PHASE_START,
	PHASE_UGLY_HACK,
	PHASE_TAG,
	PHASE_SYNC,
	PHASE_PLAY,
	PHASE_ERROR,
	PHASE_END,
};

//
// AUDIO_PLAYER
//
typedef int  (*AUDIOP_OPEN)     ( struct AUDIO *a );
typedef void (*AUDIOP_START)    ( struct AUDIO *a );
typedef void (*AUDIOP_DECODE)   ( struct AUDIO *a );
typedef int  (*AUDIOP_GET_DATA) ( struct AUDIO *a, unsigned char **pdata, int *data_size);
typedef int  (*AUDIOP_EAT_DATA) ( struct AUDIO *a, int count, int frame_size );
typedef int  (*AUDIOP_NEXT)     ( struct AUDIO *a );
typedef int  (*AUDIOP_STOP)     ( struct AUDIO *a );
typedef int  (*AUDIOP_SEEKABLE) ( struct AUDIO *a );
typedef int  (*AUDIOP_SEEK)     ( struct AUDIO *a, int sec );
typedef int  (*AUDIOP_NEW_POS)  ( struct AUDIO *a, int sec );
typedef int  (*AUDIOP_GET_TIME) ( struct AUDIO *a, int *total );

typedef struct AUDIO_PLAYER {
	char 		*name;
	AUDIOP_OPEN  	open;
	AUDIOP_START  	start;
	AUDIOP_DECODE  	decode;
	AUDIOP_GET_DATA	get_data;
	AUDIOP_EAT_DATA	eat_data;
	AUDIOP_NEXT   	next;
	AUDIOP_STOP   	stop;
	AUDIOP_SEEKABLE	seekable;
	AUDIOP_SEEK   	seek;
	AUDIOP_SEEK   	seek_internal;
	AUDIOP_SEEK   	get_new_pos;
	AUDIOP_GET_TIME	get_time;
} AUDIO_PLAYER;

struct AUDIO_REG_PLAYER;

typedef struct AUDIO_REG_PLAYER {
	int			etype;
	const AUDIO_PLAYER 	*player;
	struct AUDIO_REG_PLAYER *next;
} AUDIO_REG_PLAYER;

int audio_register_player( AUDIO_REG_PLAYER *reg );
AUDIO_PLAYER *audio_get_player( int etype );

#define AUDIO_REGISTER_PLAYER( etype, player ) \
		static AUDIO_REG_PLAYER _reg_##etype##player = { \
			etype, \
			&player,\
			NULL\
		}; \
		static void _fn_reg_##etype##player( void ) __attribute__((constructor));\
		static void _fn_reg_##etype##player( void )\
		{ \
			audio_register_player( &_reg_##etype##player ); \
		}


typedef enum {
	AM_NO_MSG = 0,
	AM_TRACK_ERROR,
	AM_TRACK_CHANGED,
	AM_NO_VALID_TRACK,
	AM_DISCARD_TRACK,
	AM_RECORDING_ERROR,
	AM_RECORDING_DISK_FULL,
	AM_RECORDING_MAXSIZE,
} AUDIO_MESSAGE;

typedef void (*AUDIO_MESSAGE_CB) ( struct AUDIO *a, AUDIO_MESSAGE message );
typedef void (*AUDIO_PROGRESS_CB)( struct AUDIO *a, const char *type, const char *value );

typedef struct AUDIO_TRACK {
	int 		etype;
	unsigned long 	size;
	STREAM_URL	src;
	ID3_TAG 	tag;
	DRM_CTX		drm_ctx;
	int		buf_end;
	
} AUDIO_TRACK;

typedef struct AUDIO {
	int		audio_end;
	int		decode_end;
	AUDIO_PROPERTIES *audio;

	AUDIO_MESSAGE_CB  message_cb;
	AUDIO_PROGRESS_CB progress_cb;
	
	AUDIO_PROPERTIES _audio;
	AUDIO_PROPERTIES _old_audio;
	AUDIO_PROPERTIES *old_audio;
	
	AUDIO_TRACK	_next;
	AUDIO_TRACK	*next;
	AUDIO_TRACK	_now;
	AUDIO_TRACK	*now;

	STREAM_IO	*io;
	
	unsigned char	v1_tag[128];			// tag read by the buffer thread
	
	int		rec_fd;				// for the recorder
	int		recording;			// the recorder engine is CURRENTLY recording (0 or 1)
	int 		clippable;

	int 		duration;

	int		size;
	int 		data_start;
	int 		data_size;
	int 		data_end;

	int 		header_size;
	
	unsigned char  *buffer;
	int 		max_buffer_size;
	int 		buffer_size;
	int 		max_overlap_size;
	int 		overlap_size;
	
	int		min_used;
	int		min_used_mp3;
	
	APIC  		apic;

	int		fs;
	
	pthread_t 	engine_thread_handle;	// for the backend player or recorder engine
	THREAD_STATE	engine_state;
	int		engine_yield;
	pthread_mutex_t	engine_mutex;
	
	pthread_t 	buffer_thread_handle;	// for the player or recorder buffer engine
	THREAD_STATE	buffer_state;
	pthread_mutex_t	buffer_mutex;
	pthread_mutex_t	buffer_rsz_mutex;
	
	int 		next_buffer;
	volatile int 	next_in_buffer;
	volatile int 	track_changing;
	
	volatile int 	sec_to_read;
	
	volatile int 	buf_write;		// write index into main buffer
	volatile int 	buf_write_pos;		// write position in file
	volatile int 	buf_write_sec;		// sector write position in file
	
	volatile int	buf_read;		// read index into main buffer
	volatile int	buf_read_pos;		// read position in file

	volatile int 	buf_hdd;		// read index for encoded data written to HDD
	volatile int 	buf_tail;		// pointer to the last valid data in buffer
	volatile int 	buf_wrap;		// did the buffer wrap yet?
	volatile int 	buf_next;		// read index for next song, during transition
	int 		buf_error;
	
	volatile int	buf_end;
	
	volatile int 	buf_state;
	
	int 		error;
	int 		error_message;
	ABORT_HANDLER	abort;
	int		aborted;
	int		paused;			// the player or recorder engine is currently paused
	int		running;		// the player or recorder engine is currently running
	int		phase;
	int		resume_sec;
	
	AUDIO_PLAYER	*player;
	void 		*player_priv;
	
	void 		*dec;
	
	int		dropped_samples;
	int		stuff_samples;		// at start or unpause we stuff the audio pipe with silence
	
	AUDIO_FRAME 	frame;
	
	struct CBE	*pcm;
	pthread_t 	pcm_thread_handle;
	THREAD_STATE	pcm_state;
	int		dump_fd;
	audio_ctx_t	*audio_ctx;
	void		*user_ctx;
	int		time_max;
	int		time_avg;
	int		time_sum;
	int		samples;
	int		next_pending;
	int		vol_l;
	int		vol_r;
} AUDIO;

void 	*Abuffer_thread	( void *data );
int	Abuffer_need_wake( AUDIO *a );
int 	audio_buffer_set_pos( AUDIO *a, int pos );

static inline int Aget_free( AUDIO *a ) 
{
	int free = a->buf_read - a->buf_write;
	if (free <= 0) 
		free += a->buffer_size;
	return free;
}	

static inline int Aget_used( AUDIO *a ) 
{
	int used = a->buf_write - a->buf_read;
	if (used < 0) 
		used += a->buffer_size;
	return used;
}	

static inline int Aget_tail( AUDIO *a ) 
{
	int used = a->buf_read - a->buf_tail;
	if (used < 0) 
		used += a->buffer_size;
	return used;
}	

// ************************************************************
//
// 	Apeek
//
//	Get a byte from the main buffer at "offset", don`t move read pointer
//
// ***********************************************************
static inline unsigned char Apeek(AUDIO *a, int offset)
{
	int tmp;

	tmp = a->buf_read + offset;
	if (tmp >= a->buffer_size)
		tmp -= a->buffer_size;
		
	return a->buffer[tmp];
}

static inline void audio_skip_bytes( AUDIO *a, int bytes ) 
{
	a->buf_read += bytes;
	
	if ( a->buf_read > a->buffer_size ) {
		a->buf_read -= a->buffer_size;
	} else if ( a->buf_read < 0 ) {
		a->buf_read += a->buffer_size;
	}
	a->buf_read_pos += bytes;
}	

// Audio errors
enum AUDIO_ERROR {
	AUDIO_NO_ERROR = 0,
	AUDIO_ERROR,
	AUDIO_BAD_APIC,
	AUDIO_NO_SYNC,
	AUDIO_UNSUPPORTED,		// unsupported AUDIO codec
	WMA_HEADER_NOT_FOUND,
	WMA_FILE_TOO_SMALL,
	WMA_CORRUPTED_HEADER,
	WMA_BAD_BITRATE,	
	WMA_ACELP_NOT_SUPPORTED,
	WMA_BAD_DRM_INIT,		// DRM protected and no licence etc.
	WAV_BAD_HEADER,
	WAV_UNSUPPORTED,
	APIC_UNRECOVERABLE,
	PLAYER_DYSFUNCTION,
	DRM_NAPSTER_NOLICENSE,		// Napster no license
	DRM_NAPSTER_NOSYNC,		// Napster to go expired, need to sync
	ERR_FILENAME,			// can not create the filename (audiocorder)
	ERR_CREATEFILE,			// can not create the file (audiocorder)
	ERR_WRITEFILE,			// can not write the file (audiocorder)
};

//
// PUBLIC API
//
int  audio_open( AUDIO *a, int buffer_size, AUDIO_MESSAGE_CB message_cb, AUDIO_PROGRESS_CB progress_cb );
int  audio_close( AUDIO *a );
int  audio_play( AUDIO *a, int start, int paused );
int  audio_stop( AUDIO *a );
int  audio_skip_track( AUDIO *a );
int  audio_set_next_track( AUDIO *a, STREAM_URL *src, int size, int etype, DRM_CTX *drm_ctx );
int  audio_is_paused( AUDIO *a );
int  audio_pause( AUDIO *a );
void audio_un_pause( AUDIO *a, int was_paused );
void audio_end_of_track( AUDIO *a );
void audio_track_changed( AUDIO *a );
void audio_discard_track( AUDIO *a );
void audio_set_user_ctx( AUDIO *a, void *ctx );
void *audio_get_user_ctx( AUDIO *a );
void audio_set_volume( AUDIO *a, int vol_l, int vol_r );
int  audio_get_audio_session_id( AUDIO *a );

ID3_TAG	    *audio_get_current_tag    ( AUDIO *a );
int          audio_get_current_etype  ( AUDIO *a );
int	     audio_get_current_url   ( AUDIO *a, STREAM_URL *src );
int	     audio_get_current_drm_ctx( AUDIO *a, DRM_CTX *drm_ctx );
int          audio_get_next_etype     ( AUDIO *a );
int          audio_get_next_url( AUDIO *a, STREAM_URL *src );
int          audio_get_next_drm_ctx   ( AUDIO *a, DRM_CTX *drm_ctx );

int  audio_get_current_time( AUDIO *s, int *total );
int  audio_seekable( AUDIO *a );
int  audio_set_pos( AUDIO *a, int sec );
int  audio_abort( AUDIO *a );
int  audio_set_abort_handler( AUDIO *a, ABORT_HANDLER handler );

//
// NON-PUBLIC API (audio_player class methods)
//
int  audio_player_switch_buffer( AUDIO *a, int etype );
void audio_player_start_and_change_track( AUDIO *a );
void audio_player_discard_track( AUDIO *a );
int  audio_player_seek_from_header( AUDIO *a, int new_pos );
void audio_player_decode( AUDIO *a );
int  audio_player_get_data( AUDIO *a, unsigned char **pdata, int *data_size );
int  audio_player_eat_data( AUDIO *a, int count, int frame_size );
int  audio_player_next( AUDIO *a );
int  audio_player_sync( AUDIO *a );
int  audio_player_stop( AUDIO *a );
int  audio_player_is_seekable( AUDIO *a );
int  audio_player_seek( AUDIO *a, int sec );
int  audio_player_seek_internal( AUDIO *a, int sec );
int  audio_player_get_time_by_bitrate( AUDIO *a, int *total );

int  audio_player_time_in_buffer( AUDIO *a );

int  audio_drive_sleep( AUDIO *a );

void *audio_pcm_thread( void *a );
int  audio_pcm_flush( AUDIO *a );

#endif	// _AUDIO_H
