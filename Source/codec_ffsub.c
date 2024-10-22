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
		// TODO MARC codec not found with embedded ssa subs: need to add libssa
		DBGS serprintf("codec_ffsub: ffsub: Open ssa\n");
		myCodec = avcodec_find_decoder(AV_CODEC_ID_SSA);
	} else if (sub->format == SUB_FORMAT_ASS) {
		// TODO MARC codec not found with embedded ssa subs: need to add libssa
		DBGS serprintf("codec_ffsub: ffsub: Open ass\n");
		myCodec = avcodec_find_decoder(AV_CODEC_ID_SSA);
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

	if (!myCodec) {
		serprintf("codec_ffsub: codec not found\n");
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

static int _decode(STREAM_DEC_SUB *dec, UCHAR *data, int size, int time, VIDEO_FRAME **pframe)
{
	my_dec_sub *self = (my_dec_sub*)dec;
	DBG {
		serprintf("codec_ffsub: ffsub decode\n", data, size);
		Dump(data, size);
	}

	VIDEO_FRAME *frame = *pframe;
	int max = frame->size - 1;
	char *dst = frame->data[0];
	*dst = 0;
	frame->time = time;
	frame->duration = -1;

	DBGS serprintf("codec_ffsub: frame width=%d, height=%d, size=%d\n", frame->width, frame->height, frame->size);

	AVPacket *avpkt = av_packet_alloc();
	if (!avpkt) {
		serprintf("codec_ffsub: Failed to allocate AVPacket\n");
		return 1;
	}

	char *avdata = av_malloc(size);
	if (!avdata) {
		serprintf("codec_ffsub: Failed to allocate AVPacket data\n");
		av_packet_free(&avpkt);
		return 1;
	}

	memcpy(avdata, data, size);
	av_packet_from_data(avpkt, avdata, size);
	avpkt->pts = time;
	avpkt->dts = time;

	int got_frame;
	AVSubtitle sub;
	int ret = avcodec_decode_subtitle2(self->avcontext, &sub, &got_frame, avpkt);
	if (ret < 0) {
		serprintf("codec_ffsub: error decoding subtitle\n");
		av_packet_free(&avpkt);
		return 1;
	}

	if (!got_frame) {
		serprintf("codec_ffsub: no subtitle frame\n");
		av_packet_free(&avpkt);
		return 0;
	}

	DBGS serprintf("codec_ffsub: decoded subtitle, format %d, start %d, end %d, rects %d, pts %d\n",
					sub.format,
					sub.start_display_time,
					sub.end_display_time,
					sub.num_rects,
					sub.pts);

	// Calculate the bounding box for all rectangles
	int left = frame->width, top = frame->height, right = 0, bottom = 0;
	int has_bitmap = 0;

	// BGRA bitmap
	uint8_t *bgra_data[4] = {NULL};
	int bgra_linesize[4] = {0};
	int bb_width = 0, bb_height = 0;

	for (int i = 0; i < sub.num_rects; i++) {
		AVSubtitleRect *rect = sub.rects[i];
		if (rect->type == SUBTITLE_BITMAP) {
			has_bitmap = 1;
			left = MIN(left, rect->x);
			top = MIN(top, rect->y);
			right = MAX(right, rect->x + rect->w);
			bottom = MAX(bottom, rect->y + rect->h);
		}
	}

	if (has_bitmap) {
		// Allocate BGRA bitmap
		DBGS serprintf("codec_ffsub: Bounding box: left=%d, top=%d, right=%d, bottom=%d\n", left, top, right, bottom);
		bb_width = right - left;
		bb_height = bottom - top;

		// Check for valid dimensions
		if (bb_width <= 0 || bb_height <= 0) {
			serprintf("codec_ffsub: Invalid bitmap dimensions: %dx%d\n", bb_width, bb_height);
			av_packet_free(&avpkt);
			return 1;
		}

		// Add a minimum size check
		bb_width = MAX(bb_width, 1);
		bb_height = MAX(bb_height, 1);

		int ret = av_image_alloc(bgra_data, bgra_linesize, bb_width, bb_height, AV_PIX_FMT_BGRA, 32);
		if (ret < 0) {
			char error_buffer[AV_ERROR_MAX_STRING_SIZE] = {0};
			av_strerror(ret, error_buffer, AV_ERROR_MAX_STRING_SIZE);
			serprintf("codec_ffsub: Failed to allocate BGRA bitmap (%dx%d). Error: %s\n", bb_width, bb_height, error_buffer);
			av_packet_free(&avpkt);
			return 1;
		}
	}

	for (int i = 0; i < sub.num_rects; i++) {
		AVSubtitleRect *rect = sub.rects[i];
		DBGS serprintf("codec_ffsub: text is %s ass is %s type is %d\n", rect->text, rect->ass, rect->type);
		DBGS serprintf("codec_ffsub: unprocessed sub start %d, end %d, pts %d, duration %d, time %d\n", sub.start_display_time, sub.end_display_time, sub.pts, sub.end_display_time - sub.start_display_time, sub.pts + sub.start_display_time);
		if (rect->text != NULL) {
			DBGS serprintf("codec_ffsub: rect->text%s\n", rect->text);
			// Note that external srt are handled directly by Android and not by codec_ffsub
			frame->time = sub.pts + sub.start_display_time;
			frame->duration = sub.end_display_time - sub.start_display_time;
			// revert to data parsing to get start and end time if decoder fails to provide start_display_time and end_display_time
			if (sub.start_display_time == 0 && sub.end_display_time == 0) {
				int start, end;
				if(sscanf( data, "%d:%d,", &start, &end ) == 2) {
					frame->time = start;
					frame->duration = end - start;
				}
			}
			strnZcpy(dst, rect->text, max - 1);
		} else if (rect->ass != NULL) {
			// note that AV_CODEC_ID_TEXT codec outputs ass rect
			DBGS serprintf("codec_ffsub: rect->ass=%s\n", rect->ass);
			int start, end;
			char *pos = rect->ass;
			// surprisingly start and end are zero out of the ffmpeg decoder: try to infer it from ass txt and if it fails  parse the data
			// typical format is rect->ass="rect->ass=1,0,Default,,0,0,0,,4704:7998,- Kids?\N- Phil, would you get them?"
			// skip to 9th comma to extract start and end times
			for (int i = 0; i < 8 && pos != NULL; i++) {
				pos = strchr(pos, ',');
				if (pos) pos++;
			}
			if (pos != NULL && sscanf(pos, "%d:%d,", &start, &end) == 2) {
				frame->time = start;
				frame->duration = end - start;
			} else {
				// parsing error get back to text data parsing
				if (sub.start_display_time == 0 && sub.end_display_time == 0) {
					if(sscanf( data, "%d:%d,", &start, &end ) == 2) {
						frame->time = start;
						frame->duration = end - start;
					}
				}
			}
			// Continue skipping to the 9th comma to reach the text content
			if (pos != NULL) {
				pos = strchr(pos, ',');
				if (pos) pos++;
			}
			// Extract text zone and convert \N to \n
			if (pos != NULL) {
				strnZcpy(dst, pos, max - 1);
				while ((pos = strstr(dst, "\\N")) != NULL) {
					pos[0] = ' ';
					pos[1] = '\n';
				}
			}
		} else if (rect->type == SUBTITLE_BITMAP) {
			// Check if the bitmap rect is not empty and contains non-black pixels
			DBGS serprintf("codec_ffsub: blend bitmap rect\n");

			// perform the blending

			// Initialize SwsContext for this rectangle
			struct SwsContext *sws_ctx = sws_getContext(
				rect->w, rect->h, AV_PIX_FMT_PAL8,
				rect->w, rect->h, AV_PIX_FMT_BGRA,
				SWS_BILINEAR, NULL, NULL, NULL);

			if (!sws_ctx) {
				serprintf("codec_ffsub: Failed to create SwsContext\n");
				continue;
			}

			// Set up source data pointers and line sizes
			const uint8_t *src_data[4] = { rect->data[0], rect->data[1], NULL, NULL };
			int src_linesize[4] = { rect->linesize[0], 0, 0, 0 };

			// Set up destination data pointers and line sizes
			uint8_t *dst_data[4] = { bgra_data[0] + (rect->y - top) * bgra_linesize[0] + (rect->x - left) * 4, NULL, NULL, NULL };
			int dst_linesize[4] = { bgra_linesize[0], 0, 0, 0 };

			// Perform the conversion
			sws_scale(sws_ctx, src_data, src_linesize, 0, rect->h, dst_data, dst_linesize);

			// Free the SwsContext
			sws_freeContext(sws_ctx);
		}
	}

	if (has_bitmap) {
		frame->time = sub.pts + sub.start_display_time;
		frame->duration = sub.end_display_time - sub.start_display_time;
		if (sub.start_display_time == 0 && sub.end_display_time == -1) {
			// note that for PGS subtitles there is no start_display_time and end_display_time
			// so we have to calculate the duration from the avpkt->duration but it is always 0
			if (avpkt->duration > 0) {
				frame->duration = avpkt->duration;
			} else {
				// TODO MARC fixme
				frame->duration = 100000;
			}
		} else {
			frame->duration = sub.end_display_time - sub.start_display_time;
		}

		frame->window.x = left;
		frame->window.y = top;
		frame->window.width = bb_width;
		frame->window.height = bb_height;
		// Copy the BGRA bitmap to the VIDEO_FRAME
		uint8_t *frame_data = frame->data[0] + top * frame->linestep[0] + left * 4;
		for (int y = 0; y < bb_height; y++) {
			memcpy(frame_data + y * frame->linestep[0], bgra_data[0] + y * bgra_linesize[0], bb_width * 4);
		}
		// Free the BGRA bitmap
		av_freep(&bgra_data[0]);
	}

	DBGS serprintf("codec_ffsub: decoded sub start %d, end %d, pts %d, duration %d, time %d\n", sub.start_display_time, sub.end_display_time, sub.pts, frame->duration, frame->time);

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
	DBGS serprintf("codec_ffsub: ffsub_new\n");
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
//STREAM_REGISTER_DEC_SUB( SUB_FORMAT_SSA, _new_dec, "SSA" );
STREAM_REGISTER_DEC_SUB( SUB_FORMAT_PGS, _new_dec, "PGS" );
STREAM_REGISTER_DEC_SUB( SUB_FORMAT_DVD_GFX, _new_dec, "vobsub" );