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

#define DBGS	if(Debug[DBG_STREAM])
#define DBG 	if(Debug[DBG_SUB])
#define DBG2 	if(Debug[DBG_SUB] > 1)

typedef struct {
	STREAM_DEC_SUB base;
	AVCodecContext* avcontext;
} my_dec_sub;

static int _open( STREAM_DEC_SUB *dec, SUB_PROPERTIES *sub, void *ctx )
{
	my_dec_sub *self = (my_dec_sub*)dec;
	DBGS serprintf("ffsub_open\n");

	dec->subtitle = &dec->_subtitle;
	*dec->subtitle = *sub;
	
	dec->ctx = ctx;
	dec->is_open = 1;

	const AVCodec* myCodec = avcodec_find_decoder(AV_CODEC_ID_MOV_TEXT);
	self->avcontext = avcodec_alloc_context3(myCodec);
	avcodec_open2(self->avcontext, myCodec, NULL);
	DBGS serprintf("ffsub: Allocated avcontext %p\n", self->avcontext);
	
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
	my_dec_sub *self = (my_dec_sub*)dec;
	DBG {
		serprintf("ffsub decode\n", data, size);
		Dump(data, size);
	}

	VIDEO_FRAME *frame = *pframe;
	int   max = frame->size - 1;
	char *dst = frame->data[0];
	*dst = 0;
	frame->time = time;
	frame->duration = -1;

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
		av_packet_free(&avpkt);
		return 0;
	}

	DBG serprintf("Decoded subtitle, format %d, start %d, end %d, rects %d, pts %d\n",
			sub.format,
			sub.start_display_time,
			sub.end_display_time,
			sub.num_rects,
			sub.pts);

	for(int i=0; i<sub.num_rects;i++) {
		AVSubtitleRect *rect = sub.rects[i];
		DBG serprintf("... text is %s ass is %s type is %d\n", rect->text, rect->ass, rect->type);
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

static STREAM_DEC_SUB *_new_dec_text( void ) 
{ 
	my_dec_sub *self = (my_dec_sub*)amalloc(sizeof(my_dec_sub));
	if( !self )
		return NULL;
	memset( self, 0, sizeof( my_dec_sub ) );

	STREAM_DEC_SUB *dec = &self->base;

	static char name[] = "MOV_TEXT";
	dec->name    = name;
	dec->destroy = _destroy;
	dec->open    = _open;
	dec->close   = _close;
	dec->decode  = _decode;
	dec->flush   = _flush;
	
	return dec;
}

STREAM_REGISTER_DEC_SUB( SUB_FORMAT_MOV_TEXT, _new_dec_text, "MOV_TEXT" );
STREAM_REGISTER_DEC_SUB( SUB_FORMAT_TEXT, _new_dec_text, "TEXT" );
STREAM_REGISTER_DEC_SUB( SUB_FORMAT_SSA, _new_dec_text, "SSA" );
