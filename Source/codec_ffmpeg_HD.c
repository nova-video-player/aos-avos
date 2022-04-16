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
#include "debug.h"
#include "util.h"
#include "stream.h"
#include "astdlib.h"
#include "mpg4.h"
#include "h264.h"
#include "codec_utils.h"

#include "stream_queue.h"

#ifdef SIM
#ifdef CONFIG_FFMPEG_HD
#ifdef CONFIG_STREAM

#define DBGS 	if(Debug[DBG_STREAM])

#define DBGCV 	if(Debug[DBG_CV])
#define DBGCV2 	if(Debug[DBG_CV] > 1 )
#define DBGCV3 	if(Debug[DBG_CV] > 2 )
#define DBGCV4 	if(Debug[DBG_CV] > 3 )
#define DBGCV5 	if(Debug[DBG_CV] > 4 )

static int debug_mc = 0;
static int sleep_arm = 10;
static int sleep_dsp = 10;

#define DBGMC	if(debug_mc)

#define IM_BUF_WIDTH		2160
#define IM_BUF_HEIGHT		720
#define MAX_IM_CTX		10
static int 			IM_ctx_num = 2;

static VIDEO_FRAME 		*context[MAX_IM_CTX];
static STREAM_Q 		*empty;	
static STREAM_Q 		*full;	
static STREAM_Q 		*done;	

typedef struct JOB {
	VIDEO_FRAME 	*ctx;
	VIDEO_FRAME 	*frame;
	int		time;
} JOB;

static pthread_t    		dsp_thread_handle;
static THREAD_STATE 		dsp_tstate;
static void *_dsp_thread( void *data );

static int threaded 		= 1;

static AVCodecContext 	*vctx = NULL;
static AVCodec 		*vcodec;
static AVFrame		*vframe;


// ******************************************
//
//	_queue_IM_buffers
//
// ******************************************
static void _queue_IM_buffers( void ) 
{
	// flush all queues
	stream_q_flush( empty );
	stream_q_flush( full  );
	stream_q_flush( done  );

	// and queue IMs for decode:
	int i;
	for( i = 0; i < IM_ctx_num; i++ ) {
		JOB job = { 
			.ctx   = context[i],
			.frame = NULL,
			.time  = 0,
		};
		stream_q_put( empty, &job );
	}
}

// ******************************************
//
//	_open
//
// ******************************************
static int _open( STREAM_DEC_VIDEO *dec, VIDEO_PROPERTIES *video, void *ctx, int *_need_flush, int *_need_reorder )
{
DBGS serprintf( "FF_HD: open: ");
	avcodec_register_all();
	
	vctx = avcodec_alloc_context();

	video->colorspace = AV_IMAGE_YUV_422;
	dec->ctx   = ctx;
	dec->video = &dec->_video;
	memcpy( dec->video, video, sizeof( VIDEO_PROPERTIES ) );
	// provide all the data that the decoder might need
	vctx->coded_width    = dec->video->width;
	vctx->coded_height   = dec->video->height;

	vctx->extradata      = dec->video->extraData;
	vctx->extradata_size = dec->video->extraDataSize;
	
	int need_flush    = 1 + IM_ctx_num;
	int need_reorder  = IM_ctx_num;
	
	switch( dec->video->format ) {
	case VIDEO_FORMAT_MPG4:
		vctx->codec_type = CODEC_TYPE_VIDEO;
		vctx->codec_id   = CODEC_ID_MPEG4;
		break;
	case VIDEO_FORMAT_WMV3:
		vctx->codec_type = CODEC_TYPE_VIDEO;
		vctx->codec_id   = CODEC_ID_WMV3;
		break;
	case VIDEO_FORMAT_VC1:
		vctx->codec_type = CODEC_TYPE_VIDEO;
		vctx->codec_id   = CODEC_ID_VC1;
		break;
	case VIDEO_FORMAT_H264:
		vctx->codec_type = CODEC_TYPE_VIDEO;
		vctx->codec_id   = CODEC_ID_H264;
		// no extradata, we will convert to ES!
		vctx->extradata_size = 0;
		// decode until valid frame!
		need_flush = -1;
		break;
	default:
		return 1;
	}
	
	// Find the decoder for the video stream
	vcodec = avcodec_find_decoder( vctx->codec_id );
	if( !vcodec ) {
serprintf("cannot find codec\r\n");
		return 1;	
	}
	// Open codec
	if (avcodec_open(vctx, vcodec) < 0) {
serprintf("cannot open codec\r\n");
		return 1;
	}

DBGS serprintf("name %s  type %d  id %d \r\n", vcodec->name, vcodec->type, vcodec->id);
	vframe = avcodec_alloc_frame();
	
	if( !(empty = stream_q_new( IM_ctx_num, sizeof( JOB ) ) ) ) {
serprintf("cannot allow empty Q\r\n");
		return 1;	
	}
	if( !(full = stream_q_new( IM_ctx_num, sizeof( JOB ) ) ) ) {
serprintf("cannot allow full Q\r\n");
		return 1;	
	}
	if( !(done = stream_q_new( IM_ctx_num, sizeof( JOB ) ) ) ) {
serprintf("cannot allow done Q\r\n");
		return 1;	
	}

	// allocate IM buffers
	int i;
	for( i = 0; i < IM_ctx_num; i++ ) {
		context[i] = frame_alloc( IM_BUF_WIDTH, IM_BUF_HEIGHT );
	}
	
	_queue_IM_buffers();
	
	if( threaded ) {
		thread_state_init( &dsp_tstate, THREAD_IDLE, "dsp" );
		thread_create( &dsp_thread_handle, _dsp_thread, (void*)dec, 10, "ff_hd_dsp_thread");
		thread_state_set( &dsp_tstate, THREAD_RUNNING );
	}

	dec->is_open = 1;

	if( _need_flush )
		*_need_flush = need_flush;
	if( _need_reorder )
		*_need_reorder = need_reorder;
	
	return 0;
}

// ******************************************
//
//	_close
//
// ******************************************
static int _close( STREAM_DEC_VIDEO *dec )
{
DBGS serprintf( "FF_HD: close\r\n");
	if( !dec->is_open ) {
serprintf("ffvd not open!\r\n");
		return 1;
	}

	if( threaded ) {
		thread_state_set( &dsp_tstate, THREAD_EXIT );
		apthread_join( dsp_thread_handle, NULL );
	}
serprintf("dsp_thread joined\r\n");

 	// Free the YUV frame
	av_free( vframe );

	// Close the codec
	if( vctx ) {
		avcodec_close( vctx );
		av_free( vctx );
		vctx = NULL;
	}
	
	// free IM buffers
	int i;
	for( i = 0; i < IM_ctx_num; i++ ) {
		frame_free( context[i] );
	}
	
	if( empty )
		stream_q_delete( &empty );
	if( full )
		stream_q_delete( &full );
	if( done )
		stream_q_delete( &done );

	dec->is_open = 0;
	return 0;
}

static void dsp_worker( STREAM_DEC_VIDEO *dec );

static void *_dsp_thread( void *data )
{
	STREAM_DEC_VIDEO *dec = (STREAM_DEC_VIDEO *)data;
DBGS serprintf("PID[%5d] dsp_thread::starting\r\n", getpid() );	
	
	while( thread_state_get( &dsp_tstate ) != THREAD_EXIT ) {
		thread_state_ack( &dsp_tstate );
		if( thread_state_get( &dsp_tstate ) == THREAD_RUNNING ) {
			dsp_worker( dec );
		} else {
			msec_sleep( 1 );
		}
	}
DBGS serprintf("PID[%5d] dsp_thread::exiting\r\n", getpid() );	
	return NULL;
	
}

static int _decode_dsp( STREAM_DEC_VIDEO *dec,                        VIDEO_FRAME *ctx, VIDEO_FRAME *frame, int *_time );
static int _decode_arm( STREAM_DEC_VIDEO *dec, UCHAR *data, int size, VIDEO_FRAME *ctx, int *_decoded,      int *_time );

// ******************************************
//
//	dsp_worker
//
// ******************************************
static void dsp_worker( STREAM_DEC_VIDEO *dec )
{
	// get a job from the full queue 
	JOB from_full;	
	while( stream_q_get_wait( full, &from_full, 50 ) ) {
//serprintf("d");
//		msec_sleep( 1 );
		if( threaded ) {
			return;
		}
	}

	// decode the DSP part
	_decode_dsp( dec, from_full.ctx, from_full.frame, &from_full.time );
	
	// put the IM back to the done queue
	JOB to_done = {
		.ctx   = from_full.ctx,
		.frame = from_full.frame,
		.time  = from_full.time,
	};
	stream_q_put( done, &to_done );
}

// ******************************************
//
//	_decode
//
// ******************************************
static int _decode( STREAM_DEC_VIDEO *dec, UCHAR *data, int size, VIDEO_FRAME **pframe, int *_decoded, int *_time )
{
	VIDEO_FRAME *frame = *pframe;
	
DBGMC serprintf("\r\nARM{in %d  siz %6d/%c  ", frame ? frame->index : -1, size, frame_type(frame->type) );

	JOB from_done = { 0 };	
	JOB from_empty;	
	while( 1 ) { 
		// try to get a job from the done queue 
		if( !stream_q_get_wait( done, &from_done, 50 ) ) {
			// place it into the empty queue
			JOB to_empty = {
				.ctx   = from_done.ctx,
				.frame = NULL,
				.time  = 0,
			};
			stream_q_put( empty, &to_empty );
		}

		// until we get a job from the empty queue 
		if( !stream_q_get( empty, &from_empty ) ) {
			break;
		}
DBGMC serprintf("_");
//		msec_sleep( 1 );
	}
	
	// decode the ARM part
	from_empty.ctx->linestep[0] = frame->linestep[0];
	from_empty.ctx->time        = frame->time;
	from_empty.ctx->type        = frame->type;
	_decode_arm( dec, data, size, from_empty.ctx, _decoded, _time );
	
	JOB to_full = {
		.ctx   = from_empty.ctx,
		.frame = frame,
		.time  = 0,
	};
	
	// put the IM to the full q
	stream_q_put( full, &to_full );

	// and also lock this frame
	STREAM *s = dec->ctx;
	if( s ) {
		stream_lock_frame( s, frame->data[0] );
	}
	
	//
	// DSP part
	//
	if( !threaded ) {
		dsp_worker( dec );
	}
		
	// did the dsp return a frame on the last job?
	if( from_done.frame  ) {
		if( s ) {
			*pframe = stream_unlock_frame( s, from_done.frame->data[0] );
		} else {
			*pframe = from_done.frame;
		}
	} else {
		*pframe = NULL;
	}
DBGMC serprintf("time %d  out %d|%d}", *_time, from_done.frame ? from_done.frame->index : -1, *pframe ? (*pframe)->index : -1 ); 	
	return 0;
}

// ******************************************
//
//	_decode_arm
//
// ******************************************
static int _decode_arm( STREAM_DEC_VIDEO *dec, UCHAR *data, int size, VIDEO_FRAME *ctx, int *_decoded, int *_time )
{
	STREAM *s = dec->ctx;
	int force_decoded = 0;
	int decoded = 0;
	VIDEO_FRAME *avos_frame = ctx;

DBGCV2 serprintf("\r\nFFARM: siz %6d  typ %c  tim %8d  ", size, frame_type(avos_frame->type), avos_frame->time);
	if( _time )
		*_time = 0;

	avos_frame->valid = 0;

	switch( dec->video->format ) {
	case VIDEO_FORMAT_MPG4: {
		MPG4_fix_vol_header( data, 20 );
		int vol = MPG4_get_VOL_len( data, size );
DBGCV2 if( vol ) serprintf("vol %3d  ", vol );
		break;
	}
	}

DBGCV3 {
unsigned short chk = 0; unsigned char *p = data; int i; for(i = 0; i < size; i++ ) { chk += *(p++); }
serprintf("CHK %04X ", chk );
}
DBGCV5 {
serprintf("\r\n");
Dump( data, size );
} else DBGCV4 {
serprintf("\r\n");
Dump( data, 64 );
}
	int start = time_update_time();
	
	// decode the frame
	int finished = 0;
	while( !finished && size > 0 ) {
		AVPacket avpkt = { .data = data, .size = size };
		av_init_packet(&avpkt);
		vctx->reordered_opaque = avos_frame->time;
		int bytes = avcodec_decode_video2( vctx, vframe, &finished, &avpkt);
		if( bytes < 0 ) {
			avos_frame->error = 1;
			return -1;
		}
DBGCV2 serprintf("siz %6d  byt %6d  %d  %c  out %8d  ", size, bytes, vframe->key_frame, frame_type( vframe->pict_type - 1), vframe->reordered_opaque);
		if( dec->video->format == VIDEO_FORMAT_MJPG ) {
			bytes = size;
		}
		data += bytes;
		size -= bytes;
		decoded += bytes;
	}	
	
	if( 0 ) {
		STREAM *s = dec->ctx;
		s->video_error           = VE_ERROR;
		s->video_error_qualifier = VEQ_MPG4_UNSUPPORTED;
		return 1;
	}

	if( finished ) {
		// render it into the buffer
		codec_convert_planar_to_interleaved( PIXFMT_YUV420P, vframe->data[0], vframe->linesize, vctx->width, vctx->height, avos_frame->data[0], avos_frame->linestep[0] / 2 );
		avos_frame->valid = 1;
	}

	msec_sleep( sleep_arm );
	
	int t = time_update_time() - start;
DBGCV2 serprintf("time %d  ", t );
	if( _time )
		*_time = t;

	avos_frame->error           = 0;
	avos_frame->type            = vframe->pict_type - 1;
	avos_frame->interlaced      = vframe->interlaced_frame;
	avos_frame->top_field_first = vframe->top_field_first;
	avos_frame->pts             = vframe->pts;

	avos_frame->cpn             = vframe->coded_picture_number;
	avos_frame->dpn             = vframe->display_picture_number;

	avos_frame->width           = vctx->width;
	avos_frame->height          = vctx->height;
	if( dec->video->reorder_pts ) {
		avos_frame->time    = vframe->reordered_opaque;
	}
	
	if( _decoded )
		*_decoded = decoded;
	if( force_decoded )
		*_decoded = force_decoded;
		
	return 0;
}

// ******************************************
//
//	_decode_dsp
//
// ******************************************
static int _decode_dsp( STREAM_DEC_VIDEO *dec, VIDEO_FRAME *ctx, VIDEO_FRAME *frame, int *_time )
{
DBGMC serprintf("DSP[");
	int start = time_update_time();

	// the dummy DECODER decoder does only a memcpy of the VIDEO data to the output frame
	memcpy( frame->data[0], ctx->data, frame->size );

	frame->error           = ctx->error;
	frame->type            = ctx->type;
	frame->interlaced      = ctx->interlaced;
	frame->top_field_first = ctx->top_field_first;
	frame->pts             = ctx->pts;
	frame->cpn             = ctx->cpn;
	frame->dpn             = ctx->dpn;
	frame->valid 	       = ctx->valid;
	frame->width           = ctx->width;
	frame->height          = ctx->height;
	frame->time 	       = ctx->time;

	// and wastes some time
	msec_sleep( sleep_dsp );
	
	int t = time_update_time() - start;

	if( _time )
		*_time = t;
DBGMC serprintf("time %d]", t);

	return 0;
}

// ******************************************
//
//	_flush
//
// ******************************************
static int _flush( STREAM_DEC_VIDEO *dec  )
{
	// stop the DSP thread:
	if( threaded ) {
		thread_state_set( &dsp_tstate, THREAD_IDLE );
	}	

	_queue_IM_buffers();
	
	// let DSP run again:
	if( threaded ) {
		thread_state_set( &dsp_tstate, THREAD_RUNNING );
	}
	
	if( vctx )
		avcodec_flush_buffers( vctx );
	return 0;
}

// ******************************************
//
//	_get_rc
//
// ******************************************
static int _get_rc( STREAM_DEC_VIDEO *dec, STREAM_RC *rc)
{
	if( !rc )
		return 1;
	memset( rc, 0, sizeof( STREAM_RC ) );
	rc->mem_size = 1024 * 512;
	return 0;
}

// ******************************************
//
//	_destroy
//
// ******************************************
static int _destroy( STREAM_DEC_VIDEO *dec )
{
	if( dec	)
		afree( dec );
	return 0;
} 

static STREAM_DEC_VIDEO *_new( void ) 
{ 
	STREAM_DEC_VIDEO *dec = (STREAM_DEC_VIDEO *)amalloc( sizeof( STREAM_DEC_VIDEO ) );
	
	if( !dec )
		return NULL;
	memset( dec, 0, sizeof( STREAM_DEC_VIDEO ) );

	static char name[] = "ff_HD";
	dec->name    = name;
	dec->destroy = _destroy;
	dec->open    = _open;
	dec->close   = _close;
	dec->decode  = _decode;
	dec->flush   = _flush;
	dec->get_rc  = _get_rc;
	
	return dec;
}

STREAM_REGISTER_DEC_VIDEO( VIDEO_FORMAT_MPG4, 0, 1280, 720, GPP, _new, "ff_HD", NULL, NULL );
#ifdef CONFIG_WMV
STREAM_REGISTER_DEC_VIDEO( VIDEO_FORMAT_WMV3, 0, 1280, 720, GPP, _new, "ff_HD", NULL, NULL );
#endif
#ifdef CONFIG_VC1
STREAM_REGISTER_DEC_VIDEO( VIDEO_FORMAT_VC1,  0, 1280, 720, GPP, _new, "ff_HD", NULL, NULL );
#endif
#ifdef CONFIG_H264
STREAM_REGISTER_DEC_VIDEO2( VIDEO_FORMAT_H264,  0, 1280, 720, H264_PROFILE_HIGH, GPP, _new, "ff_HD", NULL, &stream_video_mangler_H264 );
#endif

static STREAM_REG_DEC_VIDEO reg_mpeg4 = {
	VIDEO_FORMAT_MPG4, 0, 1280, 720, 0, GPP, _new, "ff_2HD", NULL, NULL
};

static STREAM_REG_DEC_VIDEO reg_h264 = {
	VIDEO_FORMAT_H264, 0, 1280, 720, H264_PROFILE_HIGH, GPP, _new, "ff_2HD", NULL, &stream_video_mangler_H264
};

static void _reg_hd( void ) 
{
	stream_unregister_dec_video( VIDEO_FORMAT_MPG4 );
	stream_register_dec_video( &reg_mpeg4 );
	stream_unregister_dec_video( VIDEO_FORMAT_H264 );
	stream_register_dec_video( &reg_h264 );
}

DECLARE_DEBUG_COMMAND_VOID( "reghd", 	_reg_hd );
DECLARE_DEBUG_PARAM       ( "hdctx", 	IM_ctx_num );
DECLARE_DEBUG_TOGGLE      ( "shdt", 	threaded );
DECLARE_DEBUG_TOGGLE      ( "dbgmc", 	debug_mc );
DECLARE_DEBUG_PARAM       ( "ffsa", 	sleep_arm );
DECLARE_DEBUG_PARAM       ( "ffsd", 	sleep_dsp);


#endif // CONFIG_STREAM
#endif // CONFIG_FFMPEG_VIDEO 
#endif // SIM
