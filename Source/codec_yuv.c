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
#include "atime.h"

#define DBGS 	if(Debug[DBG_STREAM])
#define DBGCV 	if(Debug[DBG_CV])

#ifdef CONFIG_STREAM

#ifdef CONFIG_YUV

//
// DECODER
//

static int yuv_dec_open(STREAM_DEC_VIDEO *dec, VIDEO_PROPERTIES *video, void *ctx, int *_need_flush, int *_need_reorder)
{
DBGS serprintf("video_dec_open_YUV\r\n");

	video->colorspace = AV_IMAGE_YUV_422;
	dec->ctx = ctx;
	dec->is_open = 1;
	dec->video = &dec->_video;
	memcpy( dec->video, video, sizeof( VIDEO_PROPERTIES ) );

	if( _need_flush )
		*_need_flush = 0;
	if( _need_reorder )
		*_need_reorder = 0;

	return 0;
}

static int yuv_dec_close( STREAM_DEC_VIDEO *dec )
{
DBGS serprintf( "video_dec_close_YUV\r\n");
	if( !dec->is_open ) {
serprintf("not open!\r\n");
		return 1;
	}
	
	dec->is_open = 0;
 	return 0;
}

static int yuv_dec_decode( STREAM_DEC_VIDEO *dec, UCHAR *data, int size, VIDEO_FRAME **pframe, int *_decoded, int *_time )
{
	VIDEO_FRAME *frame = *pframe;
	int i;
	int error;
	int decoded;

DBGCV serprintf("decode %d bytes\r\n", size );

	if( 2 * dec->video->width * dec->video->height > size ){
		// not enough data
serprintf("not enough data %d > %d\r\n", 2 * dec->video->width * dec->video->height, size );
 		error = 1;
		decoded = 0;
	} else {
		UCHAR *p;
		UCHAR *q = data;

		// memcpy
		for( i = 0 ; i < dec->video->height ; i++ ){
			p = frame->data[0] + i * frame->linestep[0];

			memcpy( p, q, 2 * dec->video->width );
swap16_buf( p, 2 * dec->video->width );
			q += 2 * dec->video->width;
		}

		error   = 0;
		decoded = 2 * dec->video->width * dec->video->height;
	}

	frame->error           = error;
	frame->type            = I_VOP;
	frame->interlaced      = 0;
	frame->top_field_first = 0;
	frame->pts             = 0;
	frame->cpn             = 0;
	frame->dpn             = 0;
	frame->valid	       = 1;

	*pframe = frame;
	
	if( _decoded )
		*_decoded = decoded;

	if( _time )
		*_time = 0;
	
	return 0;
}

static int yuv_dec_flush( STREAM_DEC_VIDEO *dec )
{	
	return 0;
} 

static int yuv_get_rc( STREAM_DEC_VIDEO *dec, STREAM_RC *rc)
{
	if( !rc )
		return 1;
	memset( rc, 0, sizeof( STREAM_RC ) );
	return 0;
}
static int yuv_dec_destroy( STREAM_DEC_VIDEO *dec )
{
	if( dec	)
		afree( dec );
	return 0;
} 

static STREAM_DEC_VIDEO *_new_dec_video_yuv( void ) 
{ 
	STREAM_DEC_VIDEO *dec = (STREAM_DEC_VIDEO *)amalloc( sizeof( STREAM_DEC_VIDEO ) );
	
	if( !dec )
		return NULL;
	memset( dec, 0, sizeof( STREAM_DEC_VIDEO ) );

	static char name[] = "yuv";
	dec->name    = name;
	dec->destroy = yuv_dec_destroy;
	dec->open    = yuv_dec_open;
	dec->close   = yuv_dec_close;
	dec->decode  = yuv_dec_decode;
	dec->flush   = yuv_dec_flush;
	dec->get_rc  = yuv_get_rc;
	
	return dec;
}

STREAM_REGISTER_DEC_VIDEO( VIDEO_FORMAT_YUV, 0, 720, 576, GPP, _new_dec_video_yuv, "yuv", NULL, NULL );

#endif // CONFIG_YUV

#endif // CONFIG_STREAM
