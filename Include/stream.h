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

#ifndef _STREAM_H
#define _STREAM_H

#include "av.h"
#include "frame_q.h"
#include "athread.h"
#include "id3tag.h"
#include "cbe.h"

#include "stream_common.h"
#include "stream_parser.h"
#include "stream_filter_audio.h"
#include "stream_rc.h"
#include "audio_interface.h"

// forward declare STREAM
struct STREAM;


#include "stream_io.h"
#include "stream_buffer.h"

#include <pthread.h>

#define STREAM_DEFAULT_BUFFER_SIZE 64
#define STREAM_LARGE_BUFFER_SIZE   128
#define STREAM_MAX_FRAMES 64

// in sync with android/vendor/archos/frameworks/ArchosFrameworks/java/com/archos/frameworks/media/AvosPlayer.java
typedef enum
{
	STREAM_SPEED_NORMAL = 0,
	STREAM_SPEED_SLOW_2, 
	STREAM_SPEED_SLOW_4, 
	STREAM_SPEED_SLOW_8, 
	STREAM_SPEED_FAST_2, 
	STREAM_SPEED_FAST_4, 
	STREAM_SPEED_FAST_8, 
} STREAM_SPEED;

typedef enum
{
	STREAM_NO_ERROR    = 0,
	STREAM_ERROR       = 1, 
	// ...
	STREAM_ERROR_FATAL = 99, 
} STREAM_ERROR_TYPE;

typedef enum
{
	STREAM_MEM_NRM = 0,
	STREAM_MEM_DMA,
	STREAM_MEM_DMA_CACHED,
	STREAM_MEM_TILER,
	STREAM_MEM_ION,
	STREAM_MEM_ANDROID,
	STREAM_MEM_CMA,
	STREAM_MEM_BYO,
} STREAM_MEM_TYPE;

enum
{
	CHUNK_FREE = 0,
	CHUNK_VALID,
};

typedef struct STREAM_CHUNK {
	int  	      	type;		// audio or video chunk
	
	unsigned int  	valid;		// 0: free, 1: valid
	unsigned int  	key;		// key frame
		
	int 		frm_type;	// (video) frame type
	UINT64 		pos;		// absolute position in stream
	UINT 		buf;		// address relative to buffer start
	int  		size;		// size of chunk
	int		payload_size;	// size of payload
	UINT 		frame;		// frame number of this chunk
	int 		time;		// presentation time in msec
	UINT 		audio_skip;	// audio time skip when playing this chunk
	UINT 		video_skip;	// video time skip when playing this chunk
	int		buf2;		// from buffer2?
	int		stream;		// number of stream this chunk is from
		
	UINT64		sample_ID;	// for CARDEA
} STREAM_CHUNK;

typedef struct STREAM_CHUNK_STORE {
	STREAM_CHUNK 	*c;
	int		read;
	int 		write;
	int		max;
} STREAM_CHUNK_STORE;

typedef struct CLEVER_BUFFER
{
	unsigned char 	*data;
	unsigned int  	size;
} CLEVER_BUFFER;

int  malloc_clever_buffer ( CLEVER_BUFFER *buffer, int size );
int  realloc_clever_buffer( CLEVER_BUFFER *buffer, int min_size );
void free_clever_buffer   ( CLEVER_BUFFER *buffer );

typedef struct STREAM_CDATA
{
	int  		valid;
	int  		type;
	int		frame;
	int		time;
	int		size;
	int		key;
	int		video_skip;
	int		audio_skip;
	int		audio_skip_time;
	
	int 		frm_type;	// (video) frame type
	int  		offset;
	int		count;
	int		fields;
	int		ref_time;
	int		top;
	int		user_ID;
	int		crypt;
	
	// bits from coding extension
	int		tff;
	int		rff;
	int		pro;
	
	UINT64 		pos;
	
	AV_PROPERTIES	*changed;
} STREAM_CDATA;

// forward declare CBE
struct CBE;

//
// DEC_AUDIO
//

typedef int (*DEC_AUDIO_NEW )   ( AUDIO_PROPERTIES *audio );
typedef int (*DEC_AUDIO_OPEN )  ( AUDIO_PROPERTIES *audio );
typedef int (*DEC_AUDIO_RE_OPEN)( AUDIO_PROPERTIES *audio );
typedef int (*DEC_AUDIO_CLOSE)  ( AUDIO_PROPERTIES *audio );
typedef int (*DEC_AUDIO_DECODE) ( AUDIO_PROPERTIES *audio, UCHAR *data, int size, AUDIO_FRAME *frame, int *decoded, int *time );
typedef int (*DEC_AUDIO_FLUSH)  ( AUDIO_PROPERTIES *audio );
typedef int (*DEC_AUDIO_DELAY)  ( AUDIO_PROPERTIES *audio );
typedef int (*DEC_AUDIO_GET_RC) ( AUDIO_PROPERTIES *audio, STREAM_RC *rc );
typedef int (*DEC_AUDIO_DELETE) ( AUDIO_PROPERTIES *audio );
typedef int (*DEC_AUDIO_IS_SUPPORTED) ( AUDIO_PROPERTIES *audio );

typedef struct STREAM_DEC_AUDIO {
	const char	  *name;
	DEC_AUDIO_NEW     new;
	DEC_AUDIO_OPEN    open;
	DEC_AUDIO_OPEN    re_open;
	DEC_AUDIO_CLOSE   close;
	DEC_AUDIO_DECODE  decode;
	DEC_AUDIO_FLUSH   flush;
	DEC_AUDIO_DELAY   delay;
	DEC_AUDIO_GET_RC  get_rc;
	DEC_AUDIO_DELETE  delete;
	DEC_AUDIO_IS_SUPPORTED  is_supported;
} STREAM_DEC_AUDIO;

//
// SINK_AUDIO
//

typedef int (*SINK_AUDIO_OPEN )  ( struct STREAM *s );
typedef int (*SINK_AUDIO_CLOSE)  ( struct STREAM *s );
typedef int (*SINK_AUDIO_START )  ( struct STREAM *s );
typedef int (*SINK_AUDIO_STOP)  ( struct STREAM *s );
typedef int (*SINK_AUDIO_WRITE)  ( struct STREAM *s, AUDIO_FRAME *frame );
typedef int (*SINK_AUDIO_PRELOAD)( struct STREAM *s );
typedef int (*SINK_AUDIO_FLUSH)  ( struct STREAM *s );
typedef int (*SINK_AUDIO_END)    ( struct STREAM *s );
typedef int (*SINK_AUDIO_SYNC)   ( struct STREAM *s );
typedef int (*SINK_AUDIO_DELAY)  ( struct STREAM *s );
typedef int (*SINK_AUDIO_CAN_WR) ( struct STREAM *s, int len );
typedef int (*SINK_AUDIO_SET_PASS)( struct STREAM *s, int pass );
typedef int (*SINK_AUDIO_GET_PASS)( struct STREAM *s );
typedef int (*SINK_AUDIO_MUTE)   ( struct STREAM *s, int mute );
typedef int (*SINK_AUDIO_SET_VOL)( struct STREAM *s );
typedef int (*SINK_AUDIO_GET_SESSION_ID)( struct STREAM *s );

typedef struct STREAM_SINK_AUDIO {
	const char	    *name;
	SINK_AUDIO_OPEN     open;
	SINK_AUDIO_CLOSE    close;
	SINK_AUDIO_START    start;
	SINK_AUDIO_STOP     stop;
	SINK_AUDIO_WRITE    write;
	SINK_AUDIO_PRELOAD  preload;
	SINK_AUDIO_FLUSH    flush;
	SINK_AUDIO_END      end;
	SINK_AUDIO_SYNC     syncable;
	SINK_AUDIO_SYNC	    delay;
	SINK_AUDIO_CAN_WR   can_write;
	SINK_AUDIO_SET_PASS set_passthrough;
	SINK_AUDIO_GET_PASS get_passthrough;
	SINK_AUDIO_MUTE     mute;
	SINK_AUDIO_SET_VOL  set_vol;
	SINK_AUDIO_GET_SESSION_ID get_session_id;
} STREAM_SINK_AUDIO;

//
// DEC_VIDEO
//
#include "stream_dec_video.h"

//
// DEC_SUB
//
#include "stream_dec_sub.h"

//
// VIDEO_MANGLER
//
typedef int (*STREAM_VIDEO_INIT_MANGLER) ( struct STREAM *s, int time );
typedef int (*STREAM_VIDEO_PRE_MANGLER)  ( struct STREAM *s, struct CBE *cbe, STREAM_CDATA *cdata );
typedef int (*STREAM_VIDEO_POST_MANGLER) ( struct STREAM *s, VIDEO_FRAME  **p_frame );
typedef int (*STREAM_VIDEO_FLUSH_MANGLER)( struct STREAM *s );

typedef struct STREAM_VIDEO_MANGLER {
	const char	  		*name;
	STREAM_VIDEO_INIT_MANGLER	init;
	STREAM_VIDEO_PRE_MANGLER	pre;
	STREAM_VIDEO_POST_MANGLER	post;
	STREAM_VIDEO_FLUSH_MANGLER	flush;
} STREAM_VIDEO_MANGLER;

//
// SINK_VIDEO
//
#include "stream_sink_video.h"

//
// PARSER
//
typedef int (*PARSER_OPEN)           ( struct STREAM *s, int buffer_size, int flags );
typedef int (*PARSER_CLOSE)          ( struct STREAM *s );
typedef int (*PARSER_PAUSE)          ( struct STREAM *s, int pause );
typedef int (*PARSER_PARSE)          ( struct STREAM *s );
typedef int (*PARSER_PARSE_CHUNK)    ( struct STREAM *s, STREAM_CHUNK *sc );
typedef int (*PARSER_CALC_RATE)      ( struct STREAM *s );
typedef int (*PARSER_SET_AUDIO_STREAM)( struct STREAM *s, int audio_stream );
typedef int (*PARSER_GET_AUDIO_CDATA)( struct STREAM *s, CLEVER_BUFFER *buffer, STREAM_CDATA *cdata );
typedef int (*PARSER_GET_VIDEO_CDATA)( struct STREAM *s, struct CBE *cbe,       STREAM_CDATA *cdata );
typedef int (*PARSER_GET_SUBTITLE_CDATA)( struct STREAM *s, CLEVER_BUFFER *buffer, STREAM_CDATA *cdata );
typedef struct STREAM_CHUNK * 
         (*PARSER_PEEK_N_AUDIO_CHUNK)( struct STREAM *s, int n, UINT8 **data );
typedef int (*PARSER_SEEK_TIME)      ( struct STREAM *s, int time, int dir, int flags, int force_reload, STREAM_CHUNK *sc );
typedef int (*PARSER_SEEK_POS)       ( struct STREAM *s, int pos,  int dir, int flags, int force_reload, STREAM_CHUNK *sc );
typedef int (*PARSER_SEEK_BY_INDEX)  ( struct STREAM *s, int idx_size, void *idx_data, int force_reload, STREAM_CHUNK *sc );
typedef int (*PARSER_SEEKABLE)       ( struct STREAM *s );
typedef int (*PARSER_PAUSEABLE)      ( struct STREAM *s );
typedef int (*PARSER_GET_INDEX)      ( struct STREAM *s, int *time, void **data, int *size );
typedef int (*PARSER_START_NEXT)     ( struct STREAM *s );
typedef struct STREAM_PARSER_STATS *
            (*PARSER_GET_STATS)      ( struct STREAM *s, struct STREAM_PARSER_STATS *stats );
typedef int (*PARSER_GET_TIME)       ( struct STREAM *s, int *total );
typedef int (*PARSER_GET_TIME_FOR_POS)( struct STREAM *s, UINT64 for_pos );

typedef struct stream_parser_str {
	const char		*name;
	PARSER_OPEN  		open;
	PARSER_CLOSE  		close;
	PARSER_PAUSE  		pause;
	PARSER_PARSE  		parse;
	PARSER_PARSE_CHUNK  	parse_chunk;
	PARSER_SET_AUDIO_STREAM set_audio_stream;
	PARSER_CALC_RATE 	calc_rate;
	PARSER_GET_AUDIO_CDATA 	get_audio_cdata;
	PARSER_GET_VIDEO_CDATA 	get_video_cdata;
	PARSER_GET_SUBTITLE_CDATA get_subtitle_cdata;
	PARSER_PEEK_N_AUDIO_CHUNK peek_n_audio_chunk;
	PARSER_SEEK_TIME	seek_time;
	PARSER_SEEK_POS		seek_pos;
	PARSER_SEEK_BY_INDEX	seek_by_index;
	PARSER_SEEKABLE		seekable;
	PARSER_SEEKABLE		pauseable;
	PARSER_GET_INDEX	get_index;
	PARSER_START_NEXT	start_next;
	PARSER_GET_STATS	get_stats;
	PARSER_GET_TIME		get_time;
	PARSER_GET_TIME_FOR_POS	get_time_for_pos;
} STREAM_PARSER;

//
// DUMPER
//
typedef int (*DUMPER_OPEN)           ( struct STREAM *s, int buffer_size, int flags );
typedef int (*DUMPER_CLOSE)          ( struct STREAM *s );
typedef int (*DUMPER_WRITE)          ( struct STREAM *s, UCHAR *data, STREAM_CDATA *cdata );

typedef struct stream_dumper_str {
	const char		*name;
	DUMPER_OPEN  		open;
	DUMPER_CLOSE  		close;
	DUMPER_WRITE  		write;
} STREAM_DUMPER;

//
// CHAPTER
//
#define STREAM_MAX_CHAPTERS 256
typedef struct STREAM_CHAPTER 
{
	char		title[MAX_TAG_LENGTH + 1];
	
	UINT64 		start;
	UINT64		end;
} STREAM_CHAPTER;

#define STREAM_MAX_PARTS 99

#define CHUNK_CACHE_MAX 16

typedef struct STREAM_CHUNK_CACHE {
	int          use_cache;
	int          write;
	int          read;
	UCHAR       *data[CHUNK_CACHE_MAX];
	STREAM_CDATA cdata[CHUNK_CACHE_MAX];
} STREAM_CHUNK_CACHE;

typedef struct VCODEC_CTRL {
	unsigned char	*data;
	unsigned int	data_size;
	VIDEO_FRAME 	*decode_frame;
	volatile int	done;
	volatile int	decoded;
	
	int		time;	// for debugging
	int		time2;	// for debugging
} VCODEC_CTRL;

typedef void (*STOP_HANDLER)      ( struct STREAM *s );
typedef void (*PROGRESS_HANDLER)  ( struct STREAM *s, int total, int progress );
typedef void (*PER_FRAME_HANDLER) ( struct STREAM *s, VIDEO_FRAME *frame );

enum
{
	STREAM_SYNC_CDATA = 0,
	STREAM_SYNC_SAMPLES,
};

typedef enum {
	STREAM_AUDIO_PROPS_CHANGED = 1,
	STREAM_VIDEO_PROPS_CHANGED,
	STREAM_SUB_PROPS_CHANGED,
	STREAM_SUBTITLE_CHANGED,
	STREAM_DECODER_CHANGED,
} STREAM_MESSAGE;

typedef enum {
	STREAM_CPU_ANY  = 0,
	STREAM_CPU_ARM  = 1,
	STREAM_CPU_ARM2 = 2,
	STREAM_CPU_DSP2 = 3,
	STREAM_CPU_DSP3 = 4,
	STREAM_CPU_DSP4 = 5,
	STREAM_CPU_DSP  = STREAM_CPU_DSP4,
} STREAM_CPU;

typedef void (*STREAM_MESSAGE_CB) ( struct STREAM *s, STREAM_MESSAGE message );
typedef int  (*STREAM_ASK_AUDIO)  ( struct STREAM *s, AUDIO_PROPERTIES *audio, int *answer );
typedef int  (*STREAM_ASK_VIDEO)  ( struct STREAM *s, VIDEO_PROPERTIES *video, int *answer );

typedef void (*STREAM_PLAYER)     ( struct STREAM *s );

typedef struct STREAM {
	// ****************************************
	// these members must be the same in VIDEO!
	// ****************************************
	int 		_type;	
	int 		etype;
	int		audio_end;
	AUDIO_PROPERTIES *audio;
	// ****************************************
	// end of common area
	// ****************************************

	int 		open;
	int		flags;
	int		start_time;	
	int		stop_time;
	int		start_pos;	// some streams have no known duration
	int		idx_size;	// size of index data entry
	unsigned char 	idx_data[64];	// for videos, index entry data to start stream from
	
	int		max_size;
	int		max_time;
	
	STOP_HANDLER 	stop;
	PROGRESS_HANDLER progress;
	int		do_progress;
	
	ABORT_HANDLER	user_abort;	// abort handler provided by the user
	ABORT_HANDLER	abort;		// internal abort handler
	int		aborted;	// remember that we are aborting!
	
	PER_FRAME_HANDLER per_frame;

	STREAM_MESSAGE_CB message_cb;
	STREAM_ASK_AUDIO ask_audio;
	STREAM_ASK_VIDEO ask_video;

	int		video_max_width;
	int		video_max_height;

	int 		audio_max_channels;
		
	STREAM_URL	src;
	char 		src_query[STREAM_MAX_PATH_LEN + 1];
	
	int		dump_audio_fd;
	int		dump_video_fd;
	int		dump_pcm_fd;
	
	// for multi segment recordings
	STREAM_PART	parts[STREAM_MAX_PARTS];
	int		num_parts;
	int		total_time;
	
	VIDEO_PROPERTIES *video;
	SUB_PROPERTIES   *subtitle;
	
	AV_PROPERTIES	av;
	
	STREAM_CHAPTER	*chapters[STREAM_MAX_CHAPTERS];
	int		num_chapters;
	
	int 		aspect_n;		// aspect ratio set by user
	int		aspect_d;		// aspect ratio set by user
	
	UINT64		size;
	
	int 		duration;
	int 		no_duration;		// this stream has no duration!
	
	int		frame_count;

	// for DVDs
	int 		vob;

	int 		sink_delay;		// delay between video and it's sink
	int 		sink_delay_count;	// delay between video and it's sink
	int		sync_mode;
	int		av_delay;		// user provided AV delay
	
	int 		audio_time;
	int 		audio_ref_time;
	int 		audio_samples;
	UINT64		audio_pos;
	
	int 		video_time;
	UINT64		video_pos;
	int 		video_drop;
	
	int 		delay;
	int 		delay_valid;
	int 		delay_fb;

	int		sink_ref_time;
	int		vid_ref_time;
	int 		drop;
	int 		drop_count;
	int 		drop_B;
	int 		drop_P;

	int 		has_index;
	UCHAR		*index_buffer;
	
	int 		archos_dvr;

	int		data_rate;
	
	int 		vtime_parsed;
	int 		vcurrent_rate;
	int 		atime_parsed;
	int 		acurrent_rate;
	
	int 		time_parsed;
	int 		current_rate;

	int		crypt;
	void 		*crypt_key;

	DRM_CTX		drm_ctx;	

	DRM_PROPERTIES 	drm;		// for JANUS
	pthread_mutex_t drm_lock;

	STREAM_IO_SETUP io_setup;
	void		*io_setup_ctx;
	
	STREAM_IO_META  io_meta;
	void		*io_meta_ctx;
	
	pthread_t 	control_thread_handle;
	THREAD_STATE	control_tstate;
	
	pthread_t 	parser_thread_handle;
	THREAD_STATE	parser_tstate;
	
	pthread_t 	sub_thread_handle;
	THREAD_STATE	sub_tstate;

	pthread_t 	engine_thread_handle;
	THREAD_STATE	engine_tstate;
	int		engine_yield;
	
	pthread_t 	audio_thread_handle;
	THREAD_STATE	audio_tstate;
	int		audio_yield;
	
	VCODEC_CTRL	vcodec;

	pthread_t 	codec_thread_handle;
	pthread_mutex_t codec_mutex;
	int		codec_run;
	pthread_cond_t	codec_code;
	
	pthread_mutex_t video_done_mutex;
	pthread_cond_t	video_done;
	
	volatile int	video_state; 
	
	int 	 	video_flush;
	int		video_output;
	
	int		buffer_size;
	int		buffer_flags;
	void		*buffer_opaque;
	
	STREAM_BUFFER	*buffer;
	STREAM_BUFFER	*buffer2;
	int		buffer_chunk;
		
	int 		error;
	int		audio_parse_end;	// parser has parsed complete file
	int		video_parse_end;	// parser has parsed complete file
	int		video_end;		// video is at the end of file
	int		stream_end;		// stream is at it's end
	
	STREAM_CHUNK_STORE aud;
	STREAM_CHUNK_STORE vid;
	STREAM_CHUNK_STORE sub;

	void 		*ro_ctx;		// reorder context for stupid codecs

	STREAM_IO	*io;
	STREAM_IO	*io2;
	
	STREAM_PARSER	*parser;
	int		parser_open;
	int		parser_extern;		// parser is not opened/closed by us!
	int		parser_paused;
	int 		parser_error;
	int		parser_mindata_size;
	int		parser_flags;
	int		parser_halt;
	int		parser_audio_discon;
	int		parser_video_discon;
	pthread_mutex_t	parser_buffer_mutex;
	void		*parser_priv;		// private data for parser
	STREAM_PARSER	*real_parser;
	STREAM_GET_PART_NAME get_part_name;
	int		parser_parse_once;
	
	STREAM_PLAYER	player;
	
	void		*mangler_priv;		// private data for mangler

	STREAM_DUMPER	*video_dumper;
	STREAM_DUMPER	*audio_dumper;

	STREAM_DEC_AUDIO *audio_dec;
	STREAM_RC 	audio_rc;

	STREAM_FILTER_AUDIO *audio_filter;
	int             audio_filter_enabled;
	int             audio_filter_level;
	int             audio_filter_night_on;

	STREAM_SINK_AUDIO *audio_sink;
	int		audio_sink_open;
	pthread_mutex_t audio_sink_mutex;
	int		audio_session_id;
	
	STREAM_DEC_VIDEO *video_dec;
	int		video_dec_open;
	STREAM_RC 	video_rc;
	
	STREAM_VIDEO_MANGLER   *video_mangler;
	
	STREAM_DEC_SUB *sub_dec;
	int		sub_dec_open;

	STREAM_SINK_VIDEO *video_sink;
	pthread_mutex_t video_sink_mutex;
	int		video_sink_count;

	VIDEO_FRAME	*frames[STREAM_MAX_FRAMES];
	int		num_frames;
	
	FRAME_Q		decode_q;
	FRAME_Q 	disp_q; 
	FRAME_Q		locked_q;
	FRAME_Q		codec_q;
	
	VIDEO_FRAME 	*decode_frame; 

	VIDEO_FRAME 	*current_frame;
	VIDEO_FRAME 	*current_out_frame;
	 
	VIDEO_FRAME	*out_frame;
	
	int		use_sink_frames;
	int		vtime_post_sink;
	
	void (*output_frame_fn)( struct STREAM *s, VIDEO_FRAME *frame, VIDEO_FRAME **qframe );

	int 		seek;
	
	STREAM_CDATA	cdata_now;
	STREAM_CDATA 	cdata_next;
	struct CBE	*cbe;

	STREAM_CHUNK_CACHE cc;
	
	CLEVER_BUFFER	audio_now;
	unsigned char 	*video_now;
	int		video_now_size;
	
	// current audio chunk size and read offset
	int 		audio_buffer_size;
	unsigned char 	*audio_buffer;
	
	char 		*sub_url[SUB_STREAM_MAX + 1 + 1];  // current path in slot "0"
	VIDEO_FRAME 	*subtitle_frame; 
	int 		subtitle_offset;
	int		subtitle_ratio_n;		// allow to scale sub timestamps with a given ratio
	int		subtitle_ratio_d;
	void		*subtitle_priv;		// private data for subtitles
	
	// current subtitle chunk
	STREAM_CDATA	cdata_sub;
	CLEVER_BUFFER	sub_buffer;
	int		subtitle_changed;
	
	int 		error_count;
	int 		video_error;
	int 		video_error_qualifier;
	char	 	video_error_desc[STREAM_MAX_PATH_LEN + 1];
	int 		audio_error_qualifier;

	int		paused;
	int		paused_internal;
	int		speed;
	int		seek_paused;
	int 		seek_epoch;
	int		muted;

	int		sync_audio;
	int		sync_video;
	int 		sync_v_time;
	int 		sync_a_time;
	int		audio_preload;
	int		audio_stuff_zero;
		
	int		play_n_video_frames;
	int		play_n_video_one;
	int		play_n_audio_frames;
	int		play_n_video_time;
	int		play_n_old_time;
	int		seek_frame;
	int		slideshow;	// this stream is a slideshow (fps < 1)

	ID3_TAG		tag;
	int		tag_new;
	
	APIC		apic;
	int		apic_new;
	
	audio_ctx_t	*audio_ctx;
	void 		*user_ctx;
	int		vol_l;
	int		vol_r;

	int		cpu_prio;
	
	int 		fps_start;
	int		fps_count;
	void		*surface_handle;
} STREAM;

#define STREAM_POS_MAX 1000

//
//	PUBLIC STREAM PLAYER API
//
#define	STREAM_PAUSED			0x0001	// open the stream, but pause it immediately
#define	STREAM_THUMB			0x0002	// open the stream for thumbnail extraction
#define	STREAM_NO_AUDIO			0x0004	// play without sound even if there is audio
#define	STREAM_NO_INDEX			0x0008	// do not try to read the AVI/ASF index
#define	STREAM_LOOP			0x0010	// loop back to start at end of file
#define	STREAM_THUMB_PLAY		0x0020	// open the stream for thumbnail animation
#define	STREAM_NO_AUDIO_CODEC		0x0040	// do not encode/decode audio, just pass through the compressed data
#define	STREAM_NO_VIDEO_CODEC		0x0080	// do not encode/decode video, just pass through the compressed data
#define	STREAM_UNCUT			0x0100	// do not take editing into account when playin the stream
#define STREAM_LATE_INDEX		0x0200	// read index asynchrously to speed up video start time
#define	STREAM_MULTI			0x0400	// rec:  split the recorded stream into multiple segments if max file_size reached
					// play: play all segments of a multirecorded file
#define	STREAM_NO_VIDEO			0x0800	// play without video even if there is video
#define	STREAM_NO_DEINT			0x1000	// do not use deinterlacer
#define	STREAM_FILE_NONLOCAL		0x2000	// the file is not local, so there is no drive to sleep
#define	STREAM_THUMB_DRM		0x4000	// open the stream for thumbnail animation with drm
#define	STREAM_NOT_INTERLEAVED		0x8000	// force the parser to treat the file as not interleaved
#define	STREAM_NO_SUBTITLES		0x10000	// play without subtitles
#define	STREAM_TIMESHIFT		0x20000	// for live streams, write/read data from the HDD if we do not play fast enough
#define	STREAM_RESIZE_BUFFER 		0x40000	// allow to resize buffer
#define	STREAM_MPEG_SKIP_PSI_PREPARSE	0x80000	// skipping preparse MPEG PSI that come from RTSP
#define	STREAM_SUBTITLES_CLEAN_TAGS	0x100000 // clean html tags in subtitles

STREAM *stream_new( void );
int 	stream_delete( STREAM **s );
int 	stream_open    (STREAM 	        *s, 		// STREAM structure
			STREAM_URL	*src,
			int		etype,		// extended type
			int 		flags );	// see above flags definition
int 	stream_start( STREAM *s );
int 	stream_stop( STREAM *s );

enum {
	STREAM_SEEK_FORWARD = 0,
	STREAM_SEEK_BACKWARD,
};

enum {
	STREAM_SEEK_STRICT = 0,
	STREAM_SEEK_RELAXED,
};

int	stream_seekable  ( STREAM *s );
int	stream_pauseable ( STREAM *s );
int	stream_seek_time ( STREAM *s, int time,  int dir, int flags );
int	stream_seek_pos  ( STREAM *s, int pos,   int dir, int flags );
int	stream_seek_frame( STREAM *s, int frame, int dir, int force_reload );
int	stream_set_speed( STREAM *s, STREAM_SPEED speed );
int	stream_set_audio_stream( STREAM *s, int audio_stream );
int	stream_set_audio_filter_level( STREAM *s, int level, int night_on ); 
void	stream_set_audio_downmix( int downmix );
int	stream_check_subtitles( STREAM *s );
int	stream_set_subtitle_stream( STREAM *s, int sub_stream );
void	stream_audio_mute    ( STREAM *s );
void	stream_audio_unmute  ( STREAM *s );
int	stream_audio_is_muted( STREAM *s );
int	stream_pause    ( STREAM *s );
void	stream_un_pause ( STREAM *s, int was_paused );
int	stream_is_paused( STREAM *s );
int     stream_get_current_speed( STREAM *s );
int     stream_get_current_time ( STREAM *s, int *total_time );
int     stream_get_buffered_time( STREAM *s, int *total_time );
int     stream_get_current_pos  ( STREAM *s, int *total_pos  );
int     stream_get_buffered_pos ( STREAM *s, int *total_pos  );
int     stream_get_current_audio_stream( STREAM *s  );
int	stream_get_audio_session_id( STREAM *s );	
VIDEO_FRAME *stream_get_current_frame( STREAM *s );
int	stream_resize( STREAM *s );
int	stream_redraw( STREAM *s );

int 	stream_set_stop_handler     ( STREAM *s, STOP_HANDLER stop           );
int 	stream_set_abort_handler    ( STREAM *s, ABORT_HANDLER abort         );
int	stream_set_progress_handler ( STREAM *s, PROGRESS_HANDLER progress   );
int	stream_set_per_frame_handler( STREAM *s, PER_FRAME_HANDLER per_frame );
int	stream_set_av_delay         ( STREAM *s, int av_delay );

int 	stream_set_crypt( STREAM *s, int crypt, void *key );
void 	stream_set_size( STREAM *s, UINT64 size );
void 	stream_set_buffer_size( STREAM *s, int buffer_size );
void 	stream_set_buffer_flags( STREAM *s, int flags, void *opaque );
void 	stream_set_aspect_ratio( STREAM *s, int aspect_n, int aspect_d );
void 	stream_set_max_video_dimensions( STREAM *s, int max_width, int max_height );
void	stream_set_audio_max_channels( STREAM *s, int max_channels );
void 	stream_set_message_cb( STREAM *s, STREAM_MESSAGE_CB message_cb );
void 	stream_set_ask_audio( STREAM *s, STREAM_ASK_AUDIO ask );
void 	stream_set_ask_video( STREAM *s, STREAM_ASK_VIDEO ask );
void 	stream_set_drm_ctx( STREAM *s, DRM_CTX *drm_ctx );
void    stream_set_user_ctx( STREAM *s, void *ctx );
void   *stream_get_user_ctx( STREAM *s);
int 	stream_get_tag_state( STREAM *s, int *tag_new, int *apic_new );
int 	stream_get_tag      ( STREAM *s, ID3_TAG *tag, APIC *apic    );
int	stream_get_chapter  ( STREAM *s, int num, STREAM_CHAPTER *chapter );
void 	stream_set_subtitle_offset( STREAM *s, int offset );
void	stream_set_subtitle_ratio( STREAM *s, int n, int d );
void	stream_set_subtitle_url( STREAM *s, const char **url_list );
int 	stream_set_abort( STREAM *s );
int	stream_set_volume( STREAM *s, int vol_l, int vol_r );
void	stream_set_surface_handle( STREAM *s, void *surface_handle );
void	*stream_get_surface_handle( STREAM *s );
void    stream_set_start_time( STREAM *s, int time );
void    stream_set_stop_time ( STREAM *s, int time );
void    stream_set_video_sink( STREAM *s, STREAM_SINK_VIDEO *sink );
void    stream_set_audio_sink( STREAM *s, STREAM_SINK_AUDIO *sink );
STREAM_SINK_VIDEO *stream_get_video_sink( STREAM *s );
STREAM_SINK_AUDIO *stream_get_audio_sink( STREAM *s );

int 	stream_get_index( STREAM *s, int *time, void **data, int *size );

void	stream_get_part_name( char *part_name, const char *full_path, int part_num );
int	stream_is_part_name ( const char *full_path, const char *ext );
void	stream_set_cpu_priority( STREAM *s, int cpu_prio );

//
//	INTERNAL API
//

#define VIDEO_MINDATA_SIZE		(1024 * 1536 * 2)
#define VIDEO_OVERLAP_SIZE		VIDEO_MINDATA_SIZE

int 	stream_init ( STREAM *s );
int 	stream_close( STREAM *s );
int	stream_drive_sleep( STREAM *s );
int 	stream_check_parts( const char *full_path );
int 	stream_parse_parts( STREAM *s );
int 	stream_set_error( STREAM *s, int error );
int 	stream_get_error( STREAM *s );
void 	stream_show_flags( int flags );
void 	stream_show_props( STREAM *s );
void 	stream_show_short_props( STREAM *s );
void 	stream_show_rc( STREAM_RC *rc );

void 	*stream_audio_dec_thread( void *data );

void 	stream_audio_flush( STREAM *s );

int	stream_sync_video( STREAM *s, int video_time );
int	stream_sync_audio( STREAM *s, int audio_time );

int 	stream_lock_frame       ( STREAM *s, void *tag );
VIDEO_FRAME *stream_unlock_frame( STREAM *s, void *tag );
VIDEO_FRAME *stream_get_frame   ( STREAM *s, void *tag );

static inline UINT64 stream_get_size( STREAM *s ) 
{ 
	return s->size; 
}
int     stream_get_time_default( STREAM *s, int *total_time );

int 	stream_get_total_rate( STREAM *s );
int 	stream_abort( STREAM *s );
void 	stream_CDATA_from_SC( STREAM_CDATA *cdata, STREAM_CHUNK *sc );
void 	stream_yield( void );
void 	stream_yield_RT( void );
int  	stream_add_chapter( STREAM *s, UINT64 start, UINT64 end, const char *title );
void    stream_set_audio_name( AUDIO_PROPERTIES *audio, int track_num );
void    stream_set_subtitle_name( SUB_PROPERTIES *subtitle, int track_num );
int     stream_put_chunk_cache( STREAM_CHUNK_CACHE *cc, CBE *cbe, STREAM_CDATA *cdata );
int     stream_get_chunk_cache( STREAM_CHUNK_CACHE *cc, CBE *cbe, STREAM_CDATA *cdata );

STREAM_SINK_VIDEO *stream_get_default_video_sink( STREAM *s );
STREAM_SINK_AUDIO *stream_get_default_audio_sink( void );

//
// STREAM BUFFER
//
static inline int stream_buffer_chunk( STREAM *s )
{
	return s->buffer_chunk;
}

static inline void stream_set_buffer_chunk( STREAM *s, int size )
{
	s->buffer_chunk = size / 512 * 512;
}

static inline UINT64 pad_to_buffer_chunk( STREAM *s, UINT64 size )
{
	UINT64 STREAM_BUFFER_CHUNK = stream_buffer_chunk( s );
	return ((size + STREAM_BUFFER_CHUNK - 1 ) / STREAM_BUFFER_CHUNK) * STREAM_BUFFER_CHUNK;
}

enum {
	BUFFER_RDWR = 0,
	BUFFER_SLEEP,
	BUFFER_NO_SLEEP,
};

int  	stream_buffer_open	( STREAM_BUFFER *buffer, STREAM *s, STREAM_IO *io, 
				  int buffer_size, int overlap_size, UINT64 start, UINT64 end, UINT32 flags, const char *tag );
int 	stream_buffer_split	( STREAM_BUFFER *buffer, STREAM *s, STREAM_IO *io, 
				  int buffer_size, int overlap_size, UINT64 start, UINT64 end, UINT32 flags, const char *tag, STREAM_BUFFER *src );
int 	stream_buffer_resize_and_rebuffer( STREAM_BUFFER *buffer, int new_size );
int  	stream_buffer_flush	( STREAM_BUFFER *buffer ); 

static inline UINT stream_sink_buffer_get_pos( STREAM_BUFFER *buffer )
{
	return buffer->buf_write_pos;
}

static inline int stream_buffer_end( STREAM_BUFFER *buffer )
{
	return buffer->buf_end;
}

int 	stream_buffer_set_pos( STREAM_BUFFER *buffer, UINT64 pos, int force_reload );
int 	stream_buffer_set_pos_seek( STREAM_BUFFER *buffer, UINT64 pos, int force_reload );

UINT64 	stream_buffer_get_pos( STREAM_BUFFER *buffer );
UINT    stream_buffer_get_buf_pos( STREAM_BUFFER *buffer );
UCHAR 	*stream_buffer_get_pointer( STREAM_BUFFER *buffer );

UCHAR 	stream_buffer_peek_byte( STREAM_BUFFER *buffer, int offset );
UCHAR 	stream_buffer_read_byte( STREAM_BUFFER *buffer );
void 	stream_buffer_read     ( STREAM_BUFFER *buffer, UCHAR *data, int size );
UINT64  stream_buffer_skip     ( STREAM_BUFFER *buffer, int offset );

int 	stream_buffer_write    ( STREAM_BUFFER *buffer, const UCHAR *data, int size );

int 	stream_buffer_EOF( STREAM_BUFFER *buffer );

int 	stream_buffer_get_free( STREAM_BUFFER *buffer );
int 	stream_buffer_get_used( STREAM_BUFFER *buffer );
int 	stream_buffer_get_head( STREAM_BUFFER *buffer );
int 	stream_buffer_get_tail( STREAM_BUFFER *buffer );

void    stream_buffer_skip_buf_pos( STREAM_BUFFER *buffer, int offset );
void    stream_buffer_skip_pos    ( STREAM_BUFFER *buffer, INT64 offset );

void 	stream_buffer_free_data    ( STREAM_BUFFER *buffer, STREAM_CHUNK *sc );
void 	stream_buffer_free_all_data( STREAM_BUFFER *buffer, UINT64 pos, UINT buf );

//
// STREAM PARSER
//
#define STREAM_INDEX_SIZE	(384 * 1024)

#define	STREAM_PARSER_LIVE			0x0001	// the stream is live, not from HDD
#define	STREAM_PARSER_NO_PREBUFFER		0x0002	// 
#define	STREAM_PARSER_FILE_NONLOCAL		0x0004	// 
#define	STREAM_PARSER_TIMESHIFT			0x0008
#define	STREAM_PARSER_MPEG_SKIP_PSI_PREPARSE	0x0010

int  	stream_parser_open ( STREAM *s, int buffer_size, int flags );
int  	stream_parser_close( STREAM *s );
int  	stream_parser_pause( STREAM *s, int paused );

int 	stream_parser_prebuffer( STREAM *s, STREAM_BUFFER *buffer, int min_data );
int 	stream_parser_can_parse( STREAM_BUFFER *buffer, int *end );
int 	stream_parser_can_output( STREAM *s );
int  	stream_parser_parse( STREAM *s );
int 	stream_parser_parse_not_interleaved( STREAM *s, PARSER_PARSE_CHUNK parse_video, PARSER_PARSE_CHUNK parse_audio  );
int 	stream_parser_guess_msPerFrame(STREAM *s);
int 	stream_parser_calc_rate( STREAM *s );
int  	stream_parser_add_chunk( STREAM *s, STREAM_CHUNK *sc );
int 	stream_parser_can_add_chunks( STREAM *s );
void 	stream_parser_clear_chunks( STREAM *s );
int 	stream_parser_set_audio_stream( STREAM *s, int audio_stream );
int 	stream_parser_preparse(STREAM *s);

int 	stream_parser_audio_chunk_num( STREAM *s );
int 	stream_parser_video_chunk_num( STREAM *s );
int 	stream_parser_subtitle_chunk_num( STREAM *s );
int 	stream_parser_audio_chunk_max( STREAM *s );
int 	stream_parser_video_chunk_max( STREAM *s );
int 	stream_parser_subtitle_chunk_max( STREAM *s );
int 	stream_parser_put_audio_chunk   ( STREAM *s, STREAM_CHUNK *c );
int 	stream_parser_put_video_chunk   ( STREAM *s, STREAM_CHUNK *c );
int 	stream_parser_put_subtitle_chunk( STREAM *s, STREAM_CHUNK *c );
int 	stream_parser_get_audio_chunk   ( STREAM *s, STREAM_CHUNK *c );
int 	stream_parser_get_video_chunk   ( STREAM *s, STREAM_CHUNK *c );
int 	stream_parser_get_subtitle_chunk( STREAM *s, STREAM_CHUNK *c );
void 	stream_parser_clear_audio_chunks   ( STREAM *s );
void 	stream_parser_clear_video_chunks   ( STREAM *s );
void 	stream_parser_clear_subtitle_chunks( STREAM *s );
void    stream_parser_send_video_extra( VIDEO_PROPERTIES *video, CBE *cbe, int *size );

STREAM_CHUNK *stream_parser_peek_n_audio_chunk(STREAM *s, int n, UCHAR **data );

STREAM_CHUNK *stream_parser_peek_video_chunk( STREAM *s, STREAM_CHUNK *c );
STREAM_CHUNK *stream_parser_peek_audio_chunk( STREAM *s, STREAM_CHUNK *c );
STREAM_CHUNK *stream_parser_peek_sub_chunk  ( STREAM *s, STREAM_CHUNK *c );

int 	stream_parser_get_audio_cdata       ( STREAM *s, CLEVER_BUFFER *audio_buffer, STREAM_CDATA *cdata );
int 	stream_parser_get_audio_cdata_header( STREAM *s, CLEVER_BUFFER *audio_buffer, STREAM_CDATA *cdata );
int 	stream_parser_get_subtitle_cdata    ( STREAM *s, CLEVER_BUFFER *sub_buffer,   STREAM_CDATA *cdata );
int 	stream_parser_pauseable( STREAM *s );

//
// STREAM VIDEO MANGLER
//
extern STREAM_VIDEO_MANGLER 	stream_video_mangler_MPEG2;
extern STREAM_VIDEO_MANGLER 	stream_video_mangler_H264;
extern STREAM_VIDEO_MANGLER	stream_video_mangler_REAL; 

#include "stream_config.h"

#endif
