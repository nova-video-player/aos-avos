/*
 * Copyright 2023 Pierre-Hugues Husson
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

#include "types.h"
#include "global.h"
#include "debug.h"
#include "astdlib.h"
#include "stream.h"
#include "util.h"

#include <string.h>
#include <ctype.h>
#include <stdio.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#define DBGS	if(1 || Debug[DBG_STREAM])
#define DBG 	if(Debug[DBG_SUB])
#define DBG2 	if(Debug[DBG_SUB] > 1)

typedef struct {
	STREAM_DEC_SUB base;
	AVCodecContext* avcontext;
} my_dec_sub;

#include <libswscale/swscale.h>

void blend_bitmap_subtitle(VIDEO_FRAME *frame, AVSubtitleRect *rect) {
	// Ensure the frame is allocated and initialized
	if (!frame || !rect || !rect->data[0]) {
		serprintf("codec_ffsub: blend_bitmap_subtitle: Invalid input\n");
		return;
	}

	// Get the dimensions of the frame and the rect
	int frame_width = frame->width;
	int frame_height = frame->height;
	int rect_width = rect->w;
	int rect_height = rect->h;

	serprintf("codec_ffsub: blend_bitmap_subtitle: frame_width=%d, frame_height=%d, rect_width=%d, rect_height=%d, rect_x=%d, rect_y=%d\n",
			   frame_width, frame_height, rect_width, rect_height, rect->x, rect->y);

	// Ensure the rect fits within the frame
	if (rect->x + rect_width > frame_width || rect->y + rect_height > frame_height) {
		serprintf("codec_ffsub: blend_bitmap_subtitle: Rect does not fit within the frame\n");
		return;
	}

	// Initialize the SwsContext for conversion
	struct SwsContext *sws_ctx = sws_getContext(
		rect_width, rect_height, AV_PIX_FMT_PAL8,
		rect_width, rect_height, frame->colorspace,
		SWS_BILINEAR, NULL, NULL, NULL);

	if (!sws_ctx) {
		serprintf("codec_ffsub: Failed to create SwsContext\n");
		return;
	}

	// Allocate a buffer for the converted bitmap
	uint8_t *converted_data[4] = { NULL, NULL, NULL, NULL };
	int converted_linesize[4] = { 0, 0, 0, 0 };
	if (av_image_alloc(converted_data, converted_linesize, rect_width, rect_height, frame->colorspace, 1) < 0) {
		serprintf("codec_ffsub: Failed to allocate image buffer\n");
		sws_freeContext(sws_ctx);
		return;
	}

	// Convert the subtitle bitmap to the target colorspace
	sws_scale(sws_ctx, (const uint8_t *const *)rect->data, rect->linesize, 0, rect_height, converted_data, converted_linesize);

	// Define the destination pointers and line sizes
	uint8_t *dst_data[4] = { frame->data[0] + rect->y * frame->linestep[0] + rect->x, NULL, NULL, NULL };
	int dst_linesize[4] = { frame->linestep[0], 0, 0, 0 };

	// Copy the converted bitmap into the frame
	av_image_copy(dst_data, dst_linesize, (const uint8_t **)converted_data, converted_linesize, frame->colorspace, rect_width, rect_height);

	// Free the allocated buffer
	av_freep(&converted_data[0]);
	sws_freeContext(sws_ctx);
}

static int _open( STREAM_DEC_SUB *dec, SUB_PROPERTIES *sub, void *ctx )
{
	my_dec_sub *self = (my_dec_sub*)dec;
	DBGS serprintf("codec_ffsub: ffsub_open format %d\n", sub->format);

	dec->subtitle = &dec->_subtitle;
	*dec->subtitle = *sub;

	dec->ctx = ctx;
	dec->is_open = 1;

	const AVCodec* myCodec;
	if (sub->format == SUB_FORMAT_TEXT) {
		DBGS serprintf("codec_ffsub: ffsub: Open text\n");
		myCodec = avcodec_find_decoder(AV_CODEC_ID_TEXT);
	} else if (sub->format == SUB_FORMAT_SSA) {
		DBGS serprintf("codec_ffsub: ffsub: Open ssa\n");
		myCodec = avcodec_find_decoder(AV_CODEC_ID_SSA);
	} else if (sub->format == SUB_FORMAT_ASS) {
		DBGS serprintf("codec_ffsub: ffsub: Open ass\n");
		myCodec = avcodec_find_decoder(AV_CODEC_ID_ASS);
	} else if (sub->format == SUB_FORMAT_MOV_TEXT) {
		DBGS serprintf("codec_ffsub: ffsub: Open mov_text\n");
		myCodec = avcodec_find_decoder(AV_CODEC_ID_MOV_TEXT);
	} else if (sub->format == SUB_FORMAT_DVD_GFX) {
		DBGS serprintf("codec_ffsub: ffsub: Open vobsub\n");
		myCodec = avcodec_find_decoder(AV_CODEC_ID_DVD_SUBTITLE);
	} else if (sub->format == SUB_FORMAT_PGS) {
		DBGS serprintf("codec_ffsub: ffsub: Open pgs\n");
		myCodec = avcodec_find_decoder(AV_CODEC_ID_HDMV_PGS_SUBTITLE);
	} else {
		DBGS serprintf("codec_ffsub: ffsub: Unknown subtitle format %d\n", sub->format);
		return 1;
	}

	self->avcontext = avcodec_alloc_context3(myCodec);
	avcodec_open2(self->avcontext, myCodec, NULL);
	DBGS serprintf("codec_ffsub: ffsub: Allocated avcontext %p\n", self->avcontext);

	return 0;
}

static int _close( STREAM_DEC_SUB *dec )
{
	my_dec_sub *self = (my_dec_sub*)dec;
	if( !dec->is_open ) {
		return 1;
	}
	if(self->avcontext) {
		DBGS serprintf("ffsub: Freeing context %p\n", self->avcontext);
		avcodec_free_context(&self->avcontext);
	}
	
	dec->is_open = 0;
 	return 0;
}

static int _decode( STREAM_DEC_SUB *dec, UCHAR *data, int size, int time, VIDEO_FRAME **pframe )
{
	serprintf("codec_ffsub: _decode\n");
	my_dec_sub *self = (my_dec_sub*)dec;
	DBG {
		serprintf("codec_ffsub: ffsub decode\n", data, size);
		Dump(data, size);
	}

	VIDEO_FRAME *frame = *pframe;
	int   max = frame->size - 1;
	char *dst = frame->data[0];
	*dst = 0;
	frame->time = time;
	frame->duration = -1;

	serprintf("codec_ffsub: frame width=%d, height=%d, size=%d\n", frame->width, frame->height, frame->size);

	AVPacket *avpkt = av_packet_alloc();
	char *avdata = av_malloc(size);
	memcpy(avdata, data, size);
	av_packet_from_data(avpkt, avdata, size);
	avpkt->pts = time;
	avpkt->dts = time;

	int got_frame;
	AVSubtitle sub;
	int ret = avcodec_decode_subtitle2(self->avcontext, &sub, &got_frame, avpkt);
	if(!got_frame) {
		serprintf("codec_ffsub: No subtitle frame\n");
		av_packet_free(&avpkt);
		return 0;
	}

	DBGS serprintf("codec_ffsub: decoded subtitle, format %d, start %d, end %d, rects %d, pts %d\n",
					sub.format,
					sub.start_display_time,
					sub.end_display_time,
					sub.num_rects,
					sub.pts);

	for(int i=0; i<sub.num_rects;i++) {
		AVSubtitleRect *rect = sub.rects[i];
		DBGS serprintf("codec_ffsub: ... text is %s ass is %s type is %d\n", rect->text, rect->ass, rect->type);
		if(rect->text != NULL) {
			strnZcpy(dst, rect->text, max-1);
		} else if(rect->ass != NULL) {
			char *pos = rect->ass;

			// Skip 9 comas to transform ass to text
			for(int i=0; i < 9 && pos != NULL; i++) {
				pos = strchr(pos, ',');
				if(pos) pos++;
			}
			if(pos != NULL) {
				strnZcpy(dst, pos, max-1);
				while( (pos = strstr(dst, "\\N")) != NULL) {
					pos[0] = ' ';
					pos[1] = '\n';
				}
			}
		} else if (rect->type == SUBTITLE_BITMAP) {
			// Check if the bitmap rect is not empty and contains non-black pixels
			if( rect->w > 0 && rect->h > 0 && rect->data[0] != NULL && rect->linesize[0] > 0 ) {
				DBGS serprintf( "codec_ffsub: blend bitmap\n" );
				blend_bitmap_subtitle( frame, rect );
			} else {
				DBGS serprintf( "codec_ffsub: bitmap rect is empty\n" );
			}
		}
	}
	av_packet_free(&avpkt);

	return 0;
}

static int _flush( STREAM_DEC_SUB *dec )
{	
	return 0;
} 

static int _destroy( STREAM_DEC_SUB *dec )
{
	if( dec	) {
		afree( dec );
	}
	return 0;
} 

static STREAM_DEC_SUB *_new_dec( void )
{ 
	my_dec_sub *self = (my_dec_sub*)amalloc(sizeof(my_dec_sub));
	if( !self )
		return NULL;
	memset( self, 0, sizeof( my_dec_sub ) );

	STREAM_DEC_SUB *dec = &self->base;

	//static char name[] = "MOV_TEXT";
	static char name[] = "FFSUB";
	dec->name    = name;
	dec->destroy = _destroy;
	dec->open    = _open;
	dec->close   = _close;
	dec->decode  = _decode;
	dec->flush   = _flush;
	
	return dec;
}

// TODO MARC XSUB not covered

STREAM_REGISTER_DEC_SUB( SUB_FORMAT_MOV_TEXT, _new_dec, "MOV_TEXT" );
STREAM_REGISTER_DEC_SUB( SUB_FORMAT_TEXT, _new_dec, "TEXT" );
STREAM_REGISTER_DEC_SUB( SUB_FORMAT_SSA, _new_dec, "SSA" );
STREAM_REGISTER_DEC_SUB( SUB_FORMAT_PGS, _new_dec, "PGS" );
STREAM_REGISTER_DEC_SUB( SUB_FORMAT_DVD_GFX, _new_dec, "vobsub" );