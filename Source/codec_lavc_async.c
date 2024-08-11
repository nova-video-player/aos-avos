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
#include "stream_alloc.h"
#include "astdlib.h"
#include "mpg4.h"
#include "h264.h"
#include "mpeg2.h"
#include "rc_clocks.h"
#include "get.h"
#include "codec_utils.h"

#ifdef CONFIG_SINK_VIDEO_ANDROID
#include "android_config.h"
#endif

#ifdef CONFIG_FFMPEG_VIDEO
#ifdef CONFIG_STREAM

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

static int _ff_do_render    = 1;
static int _ff_colorspace   = 0;
static int _ff_frame_count  = 3;
static int _force_realloc   = 0;
static int _force_realloc_fail = 0;

#ifdef DEBUG_MSG
DECLARE_DEBUG_TOGGLE("affdr", _ff_do_render );
DECLARE_DEBUG_TOGGLE("affcs", _ff_colorspace );
DECLARE_DEBUG_PARAM ("afffc", _ff_frame_count );
DECLARE_DEBUG_TOGGLE("fore", _force_realloc);
DECLARE_DEBUG_TOGGLE("forf", _force_realloc_fail);
#endif

#define DBGS 	if(Debug[DBG_STREAM])

#define DBGCV 	if(Debug[DBG_CV])
#define DBGCV2 	if(Debug[DBG_CV] > 1 )
#define DBGCV3 	if(Debug[DBG_CV] > 2 )
#define DBGCV4 	if(Debug[DBG_CV] > 3 )
#define DBGCV5 	if(Debug[DBG_CV] > 4 )

#ifdef LOG
void av_log_cb(void*, int, const char*, va_list);
#endif

typedef struct PRIV {
	AVCodecContext 	*vctx;
	AVCodec 	*vcodec;
	AVFrame		*vframe;
	
	VIDEO_FRAME	*in_frame;
	VIDEO_FRAME	*out_frame;
	
	int		need_realloc;
	int		need_realloc_fail;
} PRIV;

//
// VIDEO
//
static int _open( STREAM_DEC_VIDEO *dec, VIDEO_PROPERTIES *video, void *ctx, int *_need_flush, int *_need_reorder )
{
DBGS serprintf( "FFMA: open");
	PRIV *p = (PRIV*)dec->priv;

   	avcodec_register_all();

#ifdef CONFIG_SINK_VIDEO_ANDROID
	video->colorspace = android_get_av_color(BUFFER_TYPE_SW);
#else
	video->colorspace = _ff_colorspace ? AV_IMAGE_NV12 : AV_IMAGE_YUV_422;
#endif
	dec->ctx   = ctx;
	dec->video = &dec->_video;
	memcpy( dec->video, video, sizeof( VIDEO_PROPERTIES ) );
	
	int need_flush    = -1;
	int need_reorder  = 0;
	int no_extra      = 0;
	int codec_id;
	
	switch( dec->video->format ) {
	case VIDEO_FORMAT_MPG4:
	case VIDEO_FORMAT_MPG4GMC:
		codec_id         = AV_CODEC_ID_MPEG4;
		break;
	case VIDEO_FORMAT_H264:
		if( video->sps.valid && video->profile > H264_PROFILE_HIGH ) {
serprintf("FFMA: profile %d not supported!\n", video->profile );
			STREAM *s = dec->ctx;
			if( s ) {
				s->video_error           = VE_ERROR;
				s->video_error_qualifier = VEQ_PROFILE_AND_LEVEL_UNSUPPORTED;
        		}
			return 1;		
		}
		codec_id         = AV_CODEC_ID_H264;
		// no extradata, we will convert to ES!
		no_extra         = 1;
		need_reorder     = 1;
		break;
	default:
serprintf( "FFMA: unknown format: %d\n", dec->video->format);
		goto ErrorExit2;
	}
	
	// Find the decoder for the video stream
	p->vcodec = avcodec_find_decoder( codec_id );
	AVCodec *vcodec = p->vcodec;

	if( !vcodec ) {
serprintf("cannot find codec\r\n");
		goto ErrorExit2;
	}

#ifdef LOG
	vctx->debug |= FF_DEBUG_PICT_INFO;
	av_log_set_callback( av_log_cb );
#endif

	p->vctx = avcodec_alloc_context3(p->vcodec);
	AVCodecContext *vctx = p->vctx;

	// provide all the data that the decoder might need
	vctx->coded_width    = dec->video->width;
	vctx->coded_height   = dec->video->height;
	
	if(!no_extra) {
		vctx->extradata      = dec->video->extraData;
		vctx->extradata_size = dec->video->extraDataSize;
	}
	
	if (avcodec_open2(vctx, vcodec, NULL) < 0) {
serprintf("cannot open codec\r\n");
		goto ErrorExit;
	}

DBGS serprintf("name %s  type %d  id %d \r\n", vcodec->name, vcodec->type, vcodec->id);
	p->vframe = av_frame_alloc();
	
	dec->is_open = 1;

	if( _need_flush )
		*_need_flush = need_flush;
	if( _need_reorder )
		*_need_reorder = need_reorder;
	
	switch( dec->video->format ) {
	case VIDEO_FORMAT_RV10:
	case VIDEO_FORMAT_RV20:
	case VIDEO_FORMAT_RV30:
	case VIDEO_FORMAT_RV40:
		// RV consumes extradata on startup, delete it:
DBGS serprintf("LAVC_A: drop extra\r\n");
		video->extraDataSize = 0;
		break;
	}
	return 0;
	
ErrorExit:
	// Close the codec
	if( vctx ) {
		avcodec_close( vctx );
		av_free( vctx );
	}
	vctx = NULL;

ErrorExit2:
	return 1;
}

static int _close( STREAM_DEC_VIDEO *dec )
{
DBGS serprintf( "FFMA: close\r\n");
	if( !dec->is_open ) {
serprintf("ffvd not open!\r\n");
		return 1;
	}
 
	PRIV *p = (PRIV*)dec->priv;

 	// Free the YUV frame
	av_free( p->vframe );

	// Close the codec
	if( p->vctx ) {
		avcodec_close( p->vctx );
		av_free( p->vctx );
		p->vctx = NULL;
	}
	
	dec->is_open = 0;

	return 0;
}

static int map_pixfmt( int pix_fmt )
{
	switch( pix_fmt ) {
	case AV_PIX_FMT_YUYV422:
	case AV_PIX_FMT_YUVJ422P:
		return PIXFMT_YUV422P;
	default:
		return PIXFMT_YUV420P;
	}
}

static int _decode( STREAM_DEC_VIDEO *dec, UCHAR *data, int size, VIDEO_FRAME **pin_frame, VIDEO_FRAME **pout_frame, int *_decoded, int *_time )
{
	PRIV *p = (PRIV*)dec->priv;
	AVCodecContext *vctx = p->vctx;
	AVFrame	*vframe = p->vframe;

	int force_decoded = 0;
	int decoded = 0;
	VIDEO_FRAME *avos_frame = *pin_frame;

	*pin_frame  = NULL;
	
DBGCV2 serprintf("\r\nFFMA: siz %6d/%c  tim %8d  ", size, frame_type(avos_frame->type), avos_frame->time);
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
	int total = time_update_time();
	// decode the frame
	int got_picture = 0;

	AVPacket avpkt = { .data = data, .size = size };
	av_init_packet(&avpkt);
	if( dec->video->reorder_pts ) {
		vctx->reordered_opaque = avos_frame->time;
	} else {
		vctx->reordered_opaque = avos_frame->user_ID;
	}
DBGCV2 serprintf("<"); 
	int start = time_update_time();
	int ret = avcodec_decode_video2( vctx, vframe, &got_picture, &avpkt);
	start = time_update_time() - start;
DBGCV2 serprintf("> tim %3d  ", start); 
	if( ret < 0 ) {
serprintf("FFMA: ERR\r\n");
DBGCV2 Dump( data, MIN( size, 32 ) );
		avos_frame->error = 1;
		return -1;
	}
DBGCV2 serprintf("siz %6d  ret %6d  %d|%c  out %8d  ", 
	size, ret, !!got_picture, frame_type( vframe->pict_type - 1), vframe->reordered_opaque );
DBGCV3 serprintf("[line %4d %3d %3d  px %d -> %d]", 
	vframe->linesize[0], vframe->linesize[1], vframe->linesize[2], vctx->pix_fmt, avos_frame->linestep[0]);

	decoded += size;
	
	if( 0 ) {
		STREAM *s = dec->ctx;
		s->video_error           = VE_ERROR;
		s->video_error_qualifier = VEQ_MPG4_UNSUPPORTED;
		return 1;
	}
	
	if( got_picture ) {
		if( _ff_do_render ) {
			// render it into the buffer
DBGCV2 serprintf("[");
			int start = time_update_time();
			codec_convert_pixel_format( map_pixfmt( vctx->pix_fmt ), vframe->data, vframe->linesize, vctx->width, vctx->height, avos_frame);
			start = time_update_time() - start;
DBGCV2 serprintf("yuv %3d]", start); 
		}
	} else {
DBGCV2 serprintf("[   -   ]"); 
	}
	if( _time )
		*_time = time_update_time() - total;
	
	if( got_picture ) {
		avos_frame->valid           = 1;
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
		} else {
			avos_frame->user_ID = vframe->reordered_opaque;
		}
	} else {
			avos_frame->time    = -1;
	}
	
	if( _decoded )
		*_decoded = decoded;
	if( force_decoded )
		*_decoded = force_decoded;

	*pout_frame = avos_frame;
		
	return 0;
}

static int _dec_in( STREAM_DEC_VIDEO *dec, VIDEO_FRAME **data_frame, int *decoded, int *time )
{
	PRIV *p = (PRIV*)dec->priv;
	VIDEO_FRAME *d = *data_frame;
	
	if( !p->in_frame || p->out_frame ) {
		// if we have no frame to decode into or old frame is not yet claimed, return
		*decoded = 0;
		return 0;
	}
	
	if( _force_realloc || _force_realloc_fail ) {
serprintf("FFMA: force realloc\n");
		_force_realloc = 0;
		p->need_realloc = 1;
		if( _force_realloc_fail	) {
serprintf("FFMA: force realloc FAIL\n");
			_force_realloc_fail = 0;
			p->need_realloc_fail = 1;
		}
	}

	// copy some frame related values to the in_frame
	p->in_frame->time    = d->time;
	p->in_frame->user_ID = d->user_ID;
	p->in_frame->type    = d->type;

	int ret = _decode( dec, d->data[0], d->size, &p->in_frame, &p->out_frame, decoded, time );

	// mark the data_frame as consumed
	*data_frame = NULL;
	
	return ret;
}

static int _put_out( STREAM_DEC_VIDEO *dec, VIDEO_FRAME **pin_frame )
{
	PRIV *p = (PRIV*)dec->priv;

	if( !p->in_frame && pin_frame ) {
		// queue the frame for next decode
		p->in_frame = *pin_frame;
		*pin_frame  = NULL;
	}
	
	return 0;
}

static int _get_out( STREAM_DEC_VIDEO *dec, VIDEO_FRAME **pout_frame )
{
	PRIV *p = (PRIV*)dec->priv;

	if( p->out_frame ) {
		// we have a decoded frame, return it
		*pout_frame = p->out_frame;
		p->out_frame = NULL;
	}
	return 0;
}

static int _flush( STREAM_DEC_VIDEO *dec  )
{
	PRIV *p = (PRIV*)dec->priv;
	if( p->vctx )
		avcodec_flush_buffers( p->vctx );
	return 0;
}

static int _get_rc( STREAM_DEC_VIDEO *dec, STREAM_RC *rc)
{
	if( !rc )
		return 1;
	memset( rc, 0, sizeof( STREAM_RC ) );

	rc->num_frames = _ff_frame_count;
#ifdef SIM
	rc->mem_type  = STREAM_MEM_DMA;
#else
	#define RES_LIMIT (600 * 400)
	int res = dec->video->width * dec->video->height;
	
	if( res > RES_LIMIT ) {
		rc->min_clock = RC_SYSCLK_VIDDEC_VERY_HIGH;
	} else {
		rc->min_clock = RC_SYSCLK_VIDDEC_HIGH;
	}
	rc->cpu_type  = STREAM_CPU_ARM;
	rc->mem_type  = STREAM_MEM_NRM;
#endif
	return 0;
}

static int _need_realloc( STREAM_DEC_VIDEO *dec, VIDEO_PROPERTIES *video, int *num_frames)
{
	PRIV *p = (PRIV*)dec->priv;

	if (num_frames)
		*num_frames = _ff_frame_count;

	return p->need_realloc;
}

static int _prepare(STREAM_DEC_VIDEO *dec, VIDEO_FRAME **frames, int num_frames)
{
DBGS serprintf( "FFMA: prepare\n");
	PRIV *p = (PRIV*)dec->priv;
	
	// dummy, just clear realloc
	p->need_realloc = 0;
	
	return p->need_realloc_fail;
}

static int _destroy( STREAM_DEC_VIDEO *dec )
{
	if( !dec ) 
		return 1;
		
DBGS serprintf( "FFMA: delete\n");
	afree( dec->priv );
	afree( dec );

	return 0;
} 

static STREAM_DEC_VIDEO *_new( void ) 
{ 
	STREAM_DEC_VIDEO *dec = (STREAM_DEC_VIDEO *)amalloc( sizeof( STREAM_DEC_VIDEO ) );
	
	if( !dec )
		return NULL;
	memset( dec, 0, sizeof( STREAM_DEC_VIDEO ) );

	static char name[] = "lavc_async";
	dec->name    = name;
	dec->destroy = _destroy;
	dec->open    = _open;
	dec->prepare = _prepare;
	dec->close   = _close;
	dec->decode  = NULL;
	dec->decode2 = NULL;
	dec->dec_in  = _dec_in;
	dec->put_out = _put_out;
	dec->get_out = _get_out;
	dec->flush   = _flush;
	dec->get_rc  = _get_rc;	
	dec->need_realloc  = _need_realloc;
	
	dec->async = 1;
	
	if( !(dec->priv = acalloc( 1, sizeof( PRIV ) ) ) ) {
serprintf("FFM: cannot alloc priv\n");
		free( dec );
		return NULL;		
	}
	
	return dec;
}

#define MAXW VIDEO_MAX_WIDTH
#define MAXH VIDEO_MAX_HEIGHT

#ifdef CONFIG_FF_MPEG4
//STREAM_REGISTER_DEC_VIDEO( VIDEO_FORMAT_MPG4, 0,                  MAXW, MAXH,                    GPP, _new, "lavc_a", NULL, NULL );
//STREAM_REGISTER_DEC_VIDEO( VIDEO_FORMAT_MPG4GMC, 0,               MAXW, MAXH,                    GPP, _new, "lavc_a", NULL, NULL );
#endif

#ifdef CONFIG_FF_H264
//STREAM_REGISTER_DEC_VIDEO2( VIDEO_FORMAT_H264, 0,                 MAXW, MAXH, H264_PROFILE_HIGH, GPP, _new ,"lavc_a", NULL, &stream_video_mangler_H264  );
#endif

#ifdef DEBUG_MSG
static STREAM_REG_DEC_VIDEO reg_mpeg4 = { VIDEO_FORMAT_MPG4,   0, MAXW, MAXH, 0,                 GPP2, _new, "lavc_a", NULL };
static STREAM_REG_DEC_VIDEO reg_h264  = { VIDEO_FORMAT_H264,   0, MAXW, MAXH, H264_PROFILE_HIGH, GPP2, _new, "lavc_a", &stream_video_mangler_H264  };

static void _reg_la( void ) 
{
serprintf("register lavc for VIDEO_FORMAT_MPG4\r\n");
//	stream_unregister_dec_video( VIDEO_FORMAT_MPG4 );
	stream_register_dec_video( &reg_mpeg4 );

serprintf("register lavc for VIDEO_FORMAT_H264\r\n");
//	stream_unregister_dec_video( VIDEO_FORMAT_H264 );
	stream_register_dec_video( &reg_h264 );
}

DECLARE_DEBUG_COMMAND_VOID( "regla", 	_reg_la );
#endif

#endif	// CONFIG_STREAM
#endif
