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
#include "device_config.h"
#include "pts_reorder.h"

#ifdef CONFIG_SINK_VIDEO_ANDROID
#include "android_config.h"
#endif

#if defined( CONFIG_FFMPEG_VIDEO) || defined ( CONFIG_FFMPEG_AUDIO )

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

static int _ff_force_cached = 1;
static int _ff_do_render    = 1;
static int _ff_colorspace   = 0;
static int _ff_thread_count = 0;
static int _ff_frame_count  = 10;
static int _ff_render_count = 0;
static int _ff_fake         = 0;
static int _ff_deinterlace  = 1;
static int _ff_deinterlacing_max_height = 600;

#ifdef DEBUG_MSG
DECLARE_DEBUG_TOGGLE("ffca", _ff_force_cached );
DECLARE_DEBUG_TOGGLE("ffdr", _ff_do_render );
DECLARE_DEBUG_TOGGLE("ffcs", _ff_colorspace );
DECLARE_DEBUG_PARAM ("fftc", _ff_thread_count );
DECLARE_DEBUG_PARAM ("fffc", _ff_frame_count );
DECLARE_DEBUG_PARAM ("ffrc", _ff_render_count );
DECLARE_DEBUG_TOGGLE("fff",  _ff_fake );
DECLARE_DEBUG_TOGGLE("ffde", _ff_deinterlace );
DECLARE_DEBUG_PARAM("ffdel", _ff_deinterlacing_max_height );
#endif

#ifndef CONFIG_RELEASE
static void show_formats( void )
{
	avcodec_register_all(  );
	av_register_all(  );

	AVInputFormat *ifmt  = NULL;
	AVOutputFormat *ofmt = NULL;
	AVCodec *p           = NULL, *p2;
	AVCodecParser *cp    = NULL;
	
	const char *last_name;

	serprintf( "File formats:\n" );
	last_name = "000";
	for ( ;; ) {
		int decode = 0;
		int encode = 0;
		const char *name = NULL;
		const char *long_name = NULL;

		while ( ( ofmt = av_oformat_next( ofmt ) ) ) {
			if ( ( name == NULL || strcmp( ofmt->name, name ) < 0 ) && strcmp( ofmt->name, last_name ) > 0 ) {
				name = ofmt->name;
				long_name = ofmt->long_name;
				encode = 1;
			}
		}
		while ( ( ifmt = av_iformat_next( ifmt ) ) ) {
			if ( ( name == NULL || strcmp( ifmt->name, name ) < 0 ) && strcmp( ifmt->name, last_name ) > 0 ) {
				name = ifmt->name;
				long_name = ifmt->long_name;
				encode = 0;
			}
			if ( name && strcmp( ifmt->name, name ) == 0 )
				decode = 1;
		}
		if ( name == NULL )
			break;
		last_name = name;

		serprintf( " %s%s %-15s %s\n", decode ? "D" : " ", encode ? "E" : " ", name, long_name ? long_name : " " );
	}
	serprintf( "\n" );

	serprintf( "Codecs:\n" );

	const AVCodecDescriptor *desc = NULL;

	while ((desc = avcodec_descriptor_next(desc))) {
		serprintf( "  %5X  %-20s  %s\n", desc->id, desc->name, desc->long_name ? desc->long_name : "" );
	}
	serprintf( "\n" );

	serprintf( "Supported parsers:\n" );
	while ( ( cp = av_parser_next( cp ) ) ) {
		const AVCodecDescriptor *desc = avcodec_descriptor_get(cp->codec_ids[0]);
		serprintf( "  %5X  %s\n", cp->codec_ids[0], desc ? desc->name : "?" );
	}
	serprintf( "\n" );
	
	void *opaque = NULL;
	const char *name;
	serprintf( "Supported file protocols:\n" );
	while ((name = avio_enum_protocols(&opaque, 1)))
		serprintf( " %s:\n", name );
	serprintf( "\n\n" );
}

DECLARE_DEBUG_COMMAND_VOID( "ffsf", show_formats );
#endif
#endif

#ifdef CONFIG_FFMPEG_VIDEO
#ifdef CONFIG_STREAM

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
	void		*mt_ctx;
	int		reorder_pts;
} PRIV;

//
// VIDEO
//
static int ffmpeg_video_codec_open( STREAM_DEC_VIDEO *dec, VIDEO_PROPERTIES *video, void *ctx, int *_need_flush, int *_need_reorder )
{
DBGS serprintf( "stream_dec_video_open_FFMPEG:\n");
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
	int codec_tag     = 0;
	int supported     = 1;
	
	if (!device_config_is_video_format_supported(dec->video->format)) {
		supported = 0;
		goto ErrorExit2;
	}

	p->reorder_pts = video->reorder_pts;

	if(p->reorder_pts && pts_force_reorder) {
		STREAM *s = dec->ctx;
		if( s && video->reorder_pts && s->ro_ctx) {
			pts_ro_run(s->ro_ctx);
			p->reorder_pts = 0;
		}
	}
	
	switch( dec->video->format ) {
	case VIDEO_FORMAT_MPG4:
	case VIDEO_FORMAT_MPG4GMC:
		dec->no_extra    = 1;
		codec_id         = AV_CODEC_ID_MPEG4;
		break;
	case VIDEO_FORMAT_MPEG:
		codec_id         = AV_CODEC_ID_MPEG2VIDEO;
		need_reorder     = 1;
		break;
	case VIDEO_FORMAT_H264:
		codec_id         = AV_CODEC_ID_H264;
		// no extradata, we will convert to ES!
		no_extra         = 1;
		need_reorder     = 1;
		break;
#ifdef CONFIG_FF_HEVC
	case VIDEO_FORMAT_HEVC:
		codec_id         = AV_CODEC_ID_HEVC;
		need_flush       = 0;
		break;
#endif
	case VIDEO_FORMAT_WMV1:
		codec_id         = AV_CODEC_ID_WMV1;
		need_reorder     = 1;
		break;
	case VIDEO_FORMAT_WMV2:
		codec_id         = AV_CODEC_ID_WMV2;
		need_reorder     = 1;
		break;
	case VIDEO_FORMAT_WMV3:
	case VIDEO_FORMAT_WMV3B:
		codec_id         = AV_CODEC_ID_WMV3;
		need_reorder     = 1;
		break;
	case VIDEO_FORMAT_VC1:
		codec_id         = AV_CODEC_ID_VC1;
		need_reorder     = 1;
		break;
	case VIDEO_FORMAT_VC1IMAGE:
		codec_id         = AV_CODEC_ID_VC1IMAGE;
		codec_tag        = VIDEO_FOURCC_WVP2;
		break;
	case VIDEO_FORMAT_MSMP43:
		codec_id         = AV_CODEC_ID_MSMPEG4V3;
		break;
	case VIDEO_FORMAT_MSMP42:
		codec_id         = AV_CODEC_ID_MSMPEG4V2;
		break;
	case VIDEO_FORMAT_MSMP41:
		codec_id         = AV_CODEC_ID_MSMPEG4V1;
		break;
	case VIDEO_FORMAT_MJPG:
		codec_id         = AV_CODEC_ID_MJPEG;
		break;
	case VIDEO_FORMAT_SPARK:
		codec_id         = AV_CODEC_ID_FLV1;
		break;
	case VIDEO_FORMAT_VP6:
		codec_id         = AV_CODEC_ID_VP6F;
		break;
	case VIDEO_FORMAT_VP8:
		codec_id         = AV_CODEC_ID_VP8;
		break;
	case VIDEO_FORMAT_VP9:
		codec_id	= AV_CODEC_ID_VP9;
		break;
	case VIDEO_FORMAT_RV10:
		codec_id         = AV_CODEC_ID_RV10;
		break;
	case VIDEO_FORMAT_RV20:
		codec_id         = AV_CODEC_ID_RV20;
		break;
	case VIDEO_FORMAT_RV30:
		codec_id         = AV_CODEC_ID_RV30;
		break;
	case VIDEO_FORMAT_RV40:
		codec_id         = AV_CODEC_ID_RV40;
		break;
	case VIDEO_FORMAT_H263:
		codec_id         = AV_CODEC_ID_H263;
		break;
	case VIDEO_FORMAT_THEORA:
		codec_id         = AV_CODEC_ID_THEORA;
		break;
	case VIDEO_FORMAT_LAVC:
		if( dec->video->codec_id ) {
			codec_id = dec->video->codec_id;
			break;
		}
		// fallthrough
	default:
serprintf( "stream_dec_video_open_FFMPEG: unknown format: %d\n", dec->video->format);
		supported = 0;
		goto ErrorExit2;
	}
	
	// Find the decoder for the video stream
	p->vcodec = avcodec_find_decoder( codec_id );
	AVCodec *vcodec = p->vcodec;

	if( !vcodec ) {
serprintf("cannot find codec\r\n");
		supported = 0;
		goto ErrorExit2;
	}

	p->vctx = avcodec_alloc_context3(p->vcodec);
	AVCodecContext *vctx = p->vctx;
#ifdef LOG
	vctx->debug |= FF_DEBUG_PICT_INFO;
	av_log_set_callback( av_log_cb );
#endif
	// provide all the data that the decoder might need
	vctx->coded_width    = dec->video->width;
	vctx->coded_height   = dec->video->height;
	if( codec_tag ) {
		vctx->codec_tag = codec_tag;
	}
	
	if(!no_extra) {
		if( dec->video->extraDataSize ) {
			vctx->extradata      = dec->video->extraData;
			vctx->extradata_size = dec->video->extraDataSize;
		} else {
			vctx->extradata      = dec->video->extraData2;
			vctx->extradata_size = dec->video->extraDataSize2;
		}
	}
	vctx->thread_count = _ff_thread_count ? _ff_thread_count : device_get_cpu_count();
	
	if (avcodec_open2(vctx, vcodec, NULL) < 0) {
serprintf("cannot open codec\r\n");
		goto ErrorExit;
	}

DBGS serprintf("name %s  type %d  id %d  extra %d  threads %d\r\n", vcodec->name, vcodec->type, vcodec->id, vctx->extradata_size, vctx->thread_count);
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
DBGS serprintf("FFMPEG: drop extra\r\n");
		video->extraDataSize = 0;
		break;
	}
	
	if( _ff_render_count ) {
		p->mt_ctx = codec_convert_mt_init( _ff_render_count );
	}
	
	return 0;
	
ErrorExit:
	// Close the codec
	if ( vctx ) {
		avcodec_close( vctx );
		av_free( vctx );
	}
	vctx = NULL;

ErrorExit2:
	if (!supported) {
		STREAM *s = dec->ctx;
		if( s ) {
			s->video_error = VE_VIDEO_NOT_SUPPORTED;
		}
	}
	return 1;
}

static int ffmpeg_video_codec_close( STREAM_DEC_VIDEO *dec )
{
DBGS serprintf( "stream_dec_video_close_FFMPEG\r\n");
	if( !dec->is_open ) {
serprintf("ffvd not open!\r\n");
		return 1;
	}
 
	PRIV *p = (PRIV*)dec->priv;

 	// free the YUV frame
	av_free( p->vframe );

	if( p->mt_ctx ) {
		codec_convert_mt_exit( p->mt_ctx );
	}

	STREAM *s = dec->ctx;
	if( s && s->ro_ctx) {
		pts_ro_stop(s->ro_ctx);
	}

	// Close the codec
	if( p->vctx ) {
		avcodec_close( p->vctx );
		av_free( p->vctx );
		p->vctx = NULL;
	}
	
	dec->is_open = 0;

	return 0;
}

static int ffmpeg_video_codec_prepare(STREAM_DEC_VIDEO *dec, VIDEO_FRAME **frames, int num_frames)
{
serprintf("ffmpeg_video_codec_prepare\n");
	int i;
	for( i = 0; i < num_frames; i++ ) {
		VIDEO_FRAME *f = frames[i];
		f->priv = NULL;
	}

	return 0;
}

static int ffmpeg_video_codec_cleanup(STREAM_DEC_VIDEO *dec, VIDEO_FRAME **frames, int num_frames)
{
serprintf("ffmpeg_video_codec_cleanup\n");
	int i;
	for( i = 0; i < num_frames; i++ ) {
		VIDEO_FRAME *f = frames[i];
		av_frame_free((AVFrame**)&f->priv);
	}

	return 0;
}

static int _fake_dsp_mpeg2( UCHAR *data, int size )
{
	int 	count = 0;
	UINT	code = 0;
	int	first = 1;
	
	while( count < size - 4 ) {
		code = ( code << 8 ) | *data++;
		if( code == 0x00000100 ) {
//serprintf("start at: %d\r\n", count );		
			if( first )
				first = 0;
			else
				return count - 3;
		}
		count ++;
	}
	return 0;
}

/*
static void _mark( UCHAR *data, int width, int height, int linestep ) 
{
	int j;
	for( j = -1; j < 2; j++ ) {
		USHORT *w = (USHORT*)( data + (height / 2 + j) * linestep);
		int i;
		for( i = 0; i < width; i++ ) {
			*w++ = 0xFFFF;
		}
	}	
}
*/

static int map_pixfmt( int pix_fmt )
{
	switch( pix_fmt ) {
	case AV_PIX_FMT_YUYV422:
	case AV_PIX_FMT_YUVJ422P:
		return PIXFMT_YUV422P;
	case AV_PIX_FMT_YUV420P10LE:
		return PIXFMT_YUV420P10LE;
	case AV_PIX_FMT_YUV444P:
		return PIXFMT_YUV444P;
	default:
		return PIXFMT_YUV420P;
	}
}

static int ffmpeg_video_codec_decode2( STREAM_DEC_VIDEO *dec, UCHAR *data, int size, VIDEO_FRAME **pin_frame, VIDEO_FRAME **pout_frame, int *_decoded, int *_time )
{
	PRIV *p = (PRIV*)dec->priv;
	AVCodecContext *vctx = p->vctx;
	AVFrame	*vframe = p->vframe;

	int slice_offset[256];
	int force_decoded = 0;
	int decoded = 0;
	VIDEO_FRAME *avos_frame = *pin_frame;

	*pin_frame  = NULL;
	
DBGCV2 serprintf("\r\nFFM: %2d  siz %6d/%c  tim %8d  ", avos_frame->index, size, frame_type(avos_frame->type), avos_frame->time);
	if( _time )
		*_time = 0;
	
	avos_frame->valid = 0;
	
	switch( dec->video->format ) {
	case VIDEO_FORMAT_MPEG: {
		int fake_size = _fake_dsp_mpeg2( data, size );
		if( fake_size > 0 )
			size = fake_size;
DBGCV2 serprintf("fake %6d  ", fake_size);
		int seq = MPEG2_get_SEQ_len( data, size );
DBGCV2 if( seq ) serprintf("seq %3d  ", seq );
		break;
	}
	case VIDEO_FORMAT_MPG4: {
		MPG4_fix_vol_header( data, 20 );
		int vol = MPG4_get_VOL_len( data, size );
DBGCV2 if( vol ) serprintf("vol %3d  ", vol );
		break;
	}
	case VIDEO_FORMAT_H264: {
		break;
	}
	case VIDEO_FORMAT_WMV3: 
	case VIDEO_FORMAT_WMV3B: 
	case VIDEO_FORMAT_VC1: {
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
	if( p->reorder_pts ) {
		vctx->reordered_opaque = avos_frame->time;
        avpkt.pts = avos_frame->time;
	} else {
		vctx->reordered_opaque = avos_frame->user_ID;
        avpkt.pts = avos_frame->user_ID;
	}
DBGCV2 serprintf("<"); 
	int start = time_update_time();
	int ret = 0;
	if( !_ff_fake ) {
        ret = avcodec_send_packet(vctx, &avpkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
             //try again later -- ignore error silently
             ret = 0;
        }
        else if (ret < 0) {
            serprintf("FFM: avcodec_send_packet failed\r\n");
        }
        else
            decoded += size;

        ret = avcodec_receive_frame(vctx, vframe);
        if (ret == AVERROR(EAGAIN) ) {
             //try again later -- ignore error silently
             ret = 0;
        }
        else if (ret < 0) {
            serprintf("FFM: avcodec_receive_frame error\r\n");
        }
        else
            got_picture = 1;

	} else {
		got_picture = 1;
		vframe->reordered_opaque = vctx->reordered_opaque;
		vctx->width  = dec->video->width;
		vctx->height = dec->video->height;
		vframe->interlaced_frame = 0;
	}
	start = time_update_time() - start;
DBGCV2 serprintf("> tim %3d  ", start); 
	if( ret < 0 ) {
serprintf("FFM: ERR\r\n");
DBGCV2 Dump( data, MIN( size, 32 ) );
		avos_frame->error = 1;
		return -1;
	}
DBGCV2 serprintf("siz %6d  ret %6d  %d|%c  out %8d  ", 
	size, ret, !!got_picture, frame_type( vframe->pict_type - 1), vframe->reordered_opaque );
DBGCV3 serprintf("[line %4d %3d %3d  px %d -> %d]", 
	vframe->linesize[0], vframe->linesize[1], vframe->linesize[2], vctx->pix_fmt, avos_frame->linestep[0]);

	
	if( 0 ) {
		STREAM *s = dec->ctx;
		s->video_error           = VE_ERROR;
		s->video_error_qualifier = VEQ_MPG4_UNSUPPORTED;
		return 1;
	}
	
	if( got_picture ) {
		if( _ff_do_render && !_ff_fake ) {
			// render it into the buffer
DBGCV2 serprintf("[");
			int start = time_update_time();
			avos_frame->interlaced  = vframe->interlaced_frame;
			avos_frame->deinterlace = 0;
			if (avos_frame->interlaced != VIDEO_PROGRESSIVE && _ff_deinterlace) {
				int deinterlacing_limit = (avos_frame->interlaced == VIDEO_INTERLACED_ONE_FIELD) ? _ff_deinterlacing_max_height / 2 : _ff_deinterlacing_max_height;
				if (avos_frame->height <= deinterlacing_limit) {
					avos_frame->deinterlace = 1;
				} else {
					avos_frame->deinterlace = 0;
				}
			}
			if( !avos_frame->data[0] ) {
				avos_frame->dec = dec;
				// keep the frame:
				av_frame_free((AVFrame**)&avos_frame->priv);
				avos_frame->priv = (void*)av_frame_clone(vframe);
			} else {
				avos_frame->dec = NULL;
				if( p->mt_ctx ) {
					codec_convert_mt( p->mt_ctx, map_pixfmt( vctx->pix_fmt ), vframe->data, vframe->linesize, vctx->width, vctx->height, avos_frame );
				} else {	
					codec_convert_pixel_format( map_pixfmt( vctx->pix_fmt ), vframe->data, vframe->linesize, vctx->width, vctx->height, avos_frame);
				}
			}
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
		if( p->reorder_pts ) {
            if (vframe->reordered_opaque != 0)
			    avos_frame->time    = vframe->reordered_opaque;
            else 
                avos_frame->time = vframe->pts;
		} else {
            if (vframe->reordered_opaque != 0)
                avos_frame->user_ID = vframe->reordered_opaque;
            else
                avos_frame->user_ID = vframe->pts;
		}
	} else {
			avos_frame->time    = -1;
	}
	
	if( _decoded )
		*_decoded = decoded;
	if( force_decoded )
		*_decoded = force_decoded;

	*pout_frame = avos_frame;
		
//	_mark( avos_frame->data[0], vctx->width, vctx->height, avos_frame->linestep[0]);

	return 0;
}

static int ffmpeg_video_codec_render( STREAM_DEC_VIDEO *dec, VIDEO_FRAME *dst, VIDEO_FRAME *src )
{
	PRIV *p = (PRIV*)dec->priv;
	AVCodecContext *vctx = p->vctx;
	AVFrame	*avframe = (AVFrame*)src->priv;
DBGCV3 serprintf("ffrender %2d %08X %08X %08X\n", src->index, avframe->data, avframe->data[0], dst ? dst->data[0] : 0 );
	if( dst ) {
		if( p->mt_ctx ) {
			codec_convert_mt( p->mt_ctx, map_pixfmt( vctx->pix_fmt ), avframe->data, avframe->linesize, vctx->width, vctx->height, dst);
		} else {	
			codec_convert_pixel_format( map_pixfmt( vctx->pix_fmt ), avframe->data, avframe->linesize, vctx->width, vctx->height, dst);
		}
	}
	av_frame_free((AVFrame**)&src->priv);

	return 0;
}

static int ffmpeg_video_codec_flush( STREAM_DEC_VIDEO *dec  )
{
	PRIV *p = (PRIV*)dec->priv;
	if( p->vctx )
		avcodec_flush_buffers( p->vctx );
	return 0;
}

static int ffmpeg_video_codec_get_rc( STREAM_DEC_VIDEO *dec, STREAM_RC *rc)
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
	if( _ff_force_cached ) {
		rc->output_cached = 1;
	}
#endif
	return 0;
}

static int ffmpeg_video_codec_destroy( STREAM_DEC_VIDEO *dec )
{
	if( !dec ) 
		return 1;
		
DBGS serprintf( "FFM: delete\n");
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

	static char name[] = "ffmpeg";
	dec->name    = name;
	dec->destroy = ffmpeg_video_codec_destroy;
	dec->open    = ffmpeg_video_codec_open;
	dec->close   = ffmpeg_video_codec_close;
	dec->prepare = ffmpeg_video_codec_prepare;
	dec->cleanup = ffmpeg_video_codec_cleanup;
	dec->decode  = NULL;
	dec->decode2 = ffmpeg_video_codec_decode2;
	dec->flush   = ffmpeg_video_codec_flush;
	dec->get_rc  = ffmpeg_video_codec_get_rc;
	dec->render  = ffmpeg_video_codec_render;
	
	if( !(dec->priv = acalloc( 1, sizeof( PRIV ) ) ) ) {
serprintf("FFM: cannot alloc priv\n");
		free( dec );
		return NULL;		
	}
	
	return dec;
}

#define MAXW VIDEO_MAX_WIDTH
#define MAXH VIDEO_MAX_HEIGHT

STREAM_REGISTER_DEC_VIDEO( VIDEO_FORMAT_LAVC,   0,                MAXW, MAXH,                    GPP, _new ,"ffmpeg", NULL );

#ifdef CONFIG_DIVX311
STREAM_REGISTER_DEC_VIDEO( VIDEO_FORMAT_MSMP41, 0,                MAXW, MAXH,                    GPP, _new ,"ffmpeg", NULL );
STREAM_REGISTER_DEC_VIDEO( VIDEO_FORMAT_MSMP42, 0,                MAXW, MAXH,                    GPP, _new, "ffmpeg", NULL );
STREAM_REGISTER_DEC_VIDEO( VIDEO_FORMAT_MSMP43, 0,                MAXW, MAXH,                    GPP, _new ,"ffmpeg", NULL );
#endif

#ifdef CONFIG_MJPG
STREAM_REGISTER_DEC_VIDEO( VIDEO_FORMAT_MJPG, 0,                  MAXW, MAXH,                    GPP, _new, "ffmpeg", NULL );
#endif


#ifdef CONFIG_FF_MPEG2
STREAM_REGISTER_DEC_VIDEO( VIDEO_FORMAT_MPEG, VIDEO_SUBFMT_MPEG2, MAXW, MAXH,                    GPP, _new, "ffmpeg", &stream_video_mangler_MPEG2 );
STREAM_REGISTER_DEC_VIDEO( VIDEO_FORMAT_MPEG, VIDEO_SUBFMT_MPEG1, MAXW, MAXH,                    GPP, _new, "ffmpeg", &stream_video_mangler_MPEG2 );
#endif

#ifdef CONFIG_FF_MPEG4
STREAM_REGISTER_DEC_VIDEO( VIDEO_FORMAT_MPG4, 0,                  MAXW, MAXH,                    GPP, _new, "ffmpeg", NULL );
STREAM_REGISTER_DEC_VIDEO( VIDEO_FORMAT_MPG4GMC, 0,               MAXW, MAXH,                    GPP, _new, "ffmpeg", NULL );
#ifdef CONFIG_H263
STREAM_REGISTER_DEC_VIDEO( VIDEO_FORMAT_H263, 0,                  MAXW, MAXH,                    GPP, _new, "ffmpeg", NULL );
#endif
#ifdef CONFIG_FLV
STREAM_REGISTER_DEC_VIDEO( VIDEO_FORMAT_SPARK, 0,                 MAXW, MAXH,                    GPP, _new ,"ffmpeg", NULL );
#endif
#endif

#ifdef CONFIG_FF_H264
STREAM_REGISTER_DEC_VIDEO2( VIDEO_FORMAT_H264, 0,                 MAXW, MAXH, H264_PROFILE_HIGH, GPP, _new ,"ffmpeg", &stream_video_mangler_H264  );
#endif

#ifdef CONFIG_FF_HEVC
STREAM_REGISTER_DEC_VIDEO2( VIDEO_FORMAT_HEVC, 0,                 MAXW, MAXH, H264_PROFILE_HIGH, GPP, _new ,"ffmpeg", NULL );
#endif

#ifdef CONFIG_FF_WMV
STREAM_REGISTER_DEC_VIDEO( VIDEO_FORMAT_WMV1, 0,                   640,  480,                    GPP, _new, "ffmpeg", NULL );
STREAM_REGISTER_DEC_VIDEO( VIDEO_FORMAT_WMV2, 0,                   640,  480,                    GPP, _new, "ffmpeg", NULL );
STREAM_REGISTER_DEC_VIDEO( VIDEO_FORMAT_WMV3, 0,                  MAXW, MAXH,                    GPP, _new, "ffmpeg", NULL );
STREAM_REGISTER_DEC_VIDEO( VIDEO_FORMAT_WMV3B,0,                  MAXW, MAXH,                    GPP, _new, "ffmpeg", NULL );
#ifdef CONFIG_VC1
STREAM_REGISTER_DEC_VIDEO( VIDEO_FORMAT_VC1,  0,                  MAXW, MAXH,                    GPP, _new, "ffmpeg", NULL );
STREAM_REGISTER_DEC_VIDEO( VIDEO_FORMAT_VC1IMAGE, 0,              MAXW, MAXH,                    GPP, _new, "ffmpeg", NULL );
#endif
#endif

#ifdef CONFIG_FF_RV1020
STREAM_REGISTER_DEC_VIDEO( VIDEO_FORMAT_RV10, 0,                  MAXW, MAXH,                    GPP, _new ,"ffmpeg", NULL );
STREAM_REGISTER_DEC_VIDEO( VIDEO_FORMAT_RV20, 0,                  MAXW, MAXH,                    GPP, _new ,"ffmpeg", NULL );
#endif

#ifdef CONFIG_FF_RV3040
STREAM_REGISTER_DEC_VIDEO( VIDEO_FORMAT_RV40, 0,                  MAXW, MAXH,                    GPP, _new, "ffmpeg", &stream_video_mangler_REAL );
STREAM_REGISTER_DEC_VIDEO( VIDEO_FORMAT_RV30, 0,                  MAXW, MAXH,                    GPP, _new, "ffmpeg", &stream_video_mangler_REAL );
#endif

#ifdef CONFIG_FF_VP6
STREAM_REGISTER_DEC_VIDEO( VIDEO_FORMAT_VP6,  0,                   848,  576,                    GPP, _new ,"ffmpeg", NULL );
#endif

#ifdef CONFIG_FF_VP8
STREAM_REGISTER_DEC_VIDEO( VIDEO_FORMAT_VP8,  0,                  MAXW, MAXH,                    GPP, _new ,"ffmpeg", NULL );
#endif

#ifdef CONFIG_FF_VP9
STREAM_REGISTER_DEC_VIDEO( VIDEO_FORMAT_VP9,  0,                  MAXW, MAXH,                    GPP, _new ,"ffmpeg", NULL );
#endif

#ifdef CONFIG_THEORA
STREAM_REGISTER_DEC_VIDEO( VIDEO_FORMAT_THEORA, 0,                MAXW, MAXH,                    GPP, _new ,"ffmpeg", NULL );
#endif
#ifdef DEBUG_MSG
static STREAM_REG_DEC_VIDEO reg_mpeg4 = { VIDEO_FORMAT_MPG4,   0,                MAXW, MAXH, 0,                 GPP, _new, "ffmpeg", NULL                        };
static STREAM_REG_DEC_VIDEO reg_h263  = { VIDEO_FORMAT_H263,   0,                MAXW, MAXH, 0,                 GPP, _new, "ffmpeg", NULL			 };
static STREAM_REG_DEC_VIDEO reg_h264  = { VIDEO_FORMAT_H264,   0,                MAXW, MAXH, H264_PROFILE_HIGH, GPP, _new, "ffmpeg", &stream_video_mangler_H264  };
static STREAM_REG_DEC_VIDEO reg_hevc  = { VIDEO_FORMAT_HEVC,   0,                MAXW, MAXH, H264_PROFILE_HIGH, GPP, _new, "ffmpeg", NULL                        };
static STREAM_REG_DEC_VIDEO reg_mp4v1 = { VIDEO_FORMAT_MSMP41, 0,                MAXW, MAXH, 0,                 GPP, _new, "ffmpeg", NULL			 };
static STREAM_REG_DEC_VIDEO reg_mp4v2 = { VIDEO_FORMAT_MSMP42, 0,                MAXW, MAXH, 0,                 GPP, _new, "ffmpeg", NULL			 };
static STREAM_REG_DEC_VIDEO reg_mp4v3 = { VIDEO_FORMAT_MSMP43, 0,                MAXW, MAXH, 0,                 GPP, _new, "ffmpeg", NULL			 };
static STREAM_REG_DEC_VIDEO reg_mpeg1 = { VIDEO_FORMAT_MPEG, VIDEO_SUBFMT_MPEG1, MAXW, MAXH, 0,                 GPP, _new, "ffmpeg", &stream_video_mangler_MPEG2 };
static STREAM_REG_DEC_VIDEO reg_mpeg2 = { VIDEO_FORMAT_MPEG, VIDEO_SUBFMT_MPEG2, MAXW, MAXH, 0,                 GPP, _new, "ffmpeg", &stream_video_mangler_MPEG2 };
static STREAM_REG_DEC_VIDEO reg_rv4   = { VIDEO_FORMAT_RV40,   0,                MAXW, MAXH, 0,                 GPP, _new, "ffmpeg", NULL };
static STREAM_REG_DEC_VIDEO reg_rv3   = { VIDEO_FORMAT_RV30,   0,                MAXW, MAXH, 0,                 GPP, _new, "ffmpeg", NULL };
static STREAM_REG_DEC_VIDEO reg_wmv3  = { VIDEO_FORMAT_WMV3,   0,                MAXW, MAXH, 0,                 GPP, _new, "ffmpeg", NULL };
static STREAM_REG_DEC_VIDEO reg_vc1   = { VIDEO_FORMAT_VC1,    0,                MAXW, MAXH, 0,                 GPP, _new, "ffmpeg", NULL };

static void _reg_ff( void ) 
{
serprintf("register lavc for VIDEO_FORMAT_MPG4\r\n");
	stream_unregister_dec_video( VIDEO_FORMAT_MPG4 );
	stream_register_dec_video( &reg_mpeg4 );

serprintf("register lavc for VIDEO_FORMAT_H263\r\n");
	stream_unregister_dec_video( VIDEO_FORMAT_H263 );
	stream_register_dec_video( &reg_h263 );

serprintf("register lavc for VIDEO_FORMAT_H264\r\n");
	stream_unregister_dec_video( VIDEO_FORMAT_H264 );
	stream_register_dec_video( &reg_h264 );

serprintf("register lavc for VIDEO_FORMAT_HEVC\r\n");
	stream_unregister_dec_video( VIDEO_FORMAT_HEVC );
	stream_register_dec_video( &reg_hevc );

serprintf("register lavc for VIDEO_FORMAT_MSMP41\r\n");
	stream_unregister_dec_video( VIDEO_FORMAT_MSMP41 );
	stream_register_dec_video( &reg_mp4v1 );

serprintf("register lavc for VIDEO_FORMAT_MSMP42\r\n");
	stream_unregister_dec_video( VIDEO_FORMAT_MSMP42 );
	stream_register_dec_video( &reg_mp4v2 );

serprintf("register lavc for VIDEO_FORMAT_MSMP43\r\n");
	stream_unregister_dec_video( VIDEO_FORMAT_MSMP43 );
	stream_register_dec_video( &reg_mp4v3 );

serprintf("register lavc for VIDEO_FORMAT_MPEG1\r\n");
	stream_unregister_dec_video( VIDEO_FORMAT_MPEG );
	stream_register_dec_video( &reg_mpeg1 );
	stream_register_dec_video( &reg_mpeg2 );
serprintf("register lavc for VIDEO_FORMAT_RV40\r\n");
	stream_unregister_dec_video( VIDEO_FORMAT_RV40 );
	stream_register_dec_video( &reg_rv4 );
serprintf("register lavc for VIDEO_FORMAT_RV30\r\n");
	stream_unregister_dec_video( VIDEO_FORMAT_RV30 );
	stream_register_dec_video( &reg_rv3 );
serprintf("register lavc for VIDEO_FORMAT_WMV3\r\n");
	stream_unregister_dec_video( VIDEO_FORMAT_WMV3 );
	stream_register_dec_video( &reg_wmv3 );
serprintf("register lavc for VIDEO_FORMAT_VC1\r\n");
	stream_unregister_dec_video( VIDEO_FORMAT_VC1 );
	stream_register_dec_video( &reg_vc1 );
}

DECLARE_DEBUG_COMMAND_VOID( "regfc", 	_reg_ff );
#endif

#endif	// CONFIG_STREAM
#endif
