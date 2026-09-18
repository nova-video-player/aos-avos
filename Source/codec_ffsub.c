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
#include <limits.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#define DBGS	if(Debug[DBG_STREAM])
#define DBG 	if(Debug[DBG_SUB])
#define DBG2 	if(Debug[DBG_SUB] > 1)

typedef struct {
	STREAM_DEC_SUB base;
	AVCodecContext* avcontext;
	uint8_t *bitmap; // decoder-owned bitmap, replaced only after successful allocation
} my_dec_sub;

static int _open( STREAM_DEC_SUB *dec, SUB_PROPERTIES *sub, void *ctx )
{
	my_dec_sub *self = (my_dec_sub*)dec;
	DBGS serprintf("codec_ffsub: ffsub_open format %d\n", sub->format);

	dec->subtitle = &dec->_subtitle;
	*dec->subtitle = *sub;

	dec->ctx = ctx;

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
	} else if (sub->format == SUB_FORMAT_WEBVTT) {
		DBGS serprintf("codec_ffsub: ffsub: Open webvtt\n");
		myCodec = avcodec_find_decoder(AV_CODEC_ID_WEBVTT);
	} else {
		DBGS serprintf("codec_ffsub: ffsub: Unknown subtitle format %d\n", sub->format);
		return 1;
	}

	if (!myCodec)
		return 1;
	self->avcontext = avcodec_alloc_context3(myCodec);
	if (!self->avcontext)
		return 1;
	// Reopening a track must install that track's codec initialization data
	// (notably mov_text configuration and DVD subtitle palettes).
	const uint8_t *extra = sub->extraDataSize2 > 0 ? sub->extraData2 : sub->extraData;
	int extra_size = sub->extraDataSize2 > 0 ? sub->extraDataSize2 : sub->extraDataSize;
	if (extra && extra_size > 0) {
		self->avcontext->extradata = av_mallocz((size_t)extra_size + AV_INPUT_BUFFER_PADDING_SIZE);
		if (!self->avcontext->extradata) {
			avcodec_free_context(&self->avcontext);
			return 1;
		}
		memcpy(self->avcontext->extradata, extra, extra_size);
		self->avcontext->extradata_size = extra_size;
	}
	self->avcontext->pkt_timebase = (AVRational){1, 1000};
	if (avcodec_open2(self->avcontext, myCodec, NULL) < 0) {
		avcodec_free_context(&self->avcontext);
		return 1;
	}
	dec->is_open = 1;
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
	
	av_freep(&self->bitmap);
	dec->is_open = 0;
 	return 0;
}

static int _decode(STREAM_DEC_SUB *dec, UCHAR *data, int size, int time, VIDEO_FRAME **pframe)
{
	// note that when ffmpeg sub decoding provides 0 rect for pgs it means that the previous subtitle is over

	my_dec_sub *self = (my_dec_sub*)dec;
	DBG {
		serprintf("codec_ffsub: ffsub decode\n", data, size);
		Dump(data, size);
	}

	VIDEO_FRAME *frame = *pframe;
	*pframe = NULL; // fragments and decode errors do not produce a display event
	if (!self->avcontext || !frame || size < 0 || (size && !data))
		return 1;
	int max = frame->size - 1;

	frame->time = time;
	frame->duration = -1;

	DBGS serprintf("codec_ffsub: frame width=%d, height=%d, size=%d\n", frame->width, frame->height, frame->size);

	AVPacket *avpkt = av_packet_alloc();
	if (!avpkt) {
		serprintf("codec_ffsub: Failed to allocate AVPacket\n");
		return 1;
	}

	if (av_new_packet(avpkt, size) < 0) {
		serprintf("codec_ffsub: Failed to allocate AVPacket data\n");
		av_packet_free(&avpkt);
		return 1;
	}

	memcpy(avpkt->data, data, size);
	DBGS serprintf("codec_ffsub: avpkt->pts=%d, avpkt->dts=%d overridden by time=%d\n", avpkt->pts, avpkt->dts, time);
	avpkt->pts = time >= 0 ? time : AV_NOPTS_VALUE;
	avpkt->dts = avpkt->pts;
	// Only plain TEXT packets receive Nova's private start:end prefix.
	// Strip it before FFmpeg creates an ASS rectangle; caption text itself
	// (e.g. "12:34 lunch" in mov_text/WebVTT) is never timing metadata.
	int private_start = -1, private_duration = -1;
	if (dec->_subtitle.format == SUB_FORMAT_TEXT) {
		int start, end, prefix = 0;
		if (sscanf((char *)avpkt->data, "%d:%d,%n", &start, &end, &prefix) == 2 &&
		    prefix > 0 && prefix <= avpkt->size && start >= 0 && end >= start) {
			private_start = start;
			private_duration = end - start;
			avpkt->data += prefix;
			avpkt->size -= prefix;
		}
	}

	int got_frame;
	AVSubtitle sub = {0};
	int ret = avcodec_decode_subtitle2(self->avcontext, &sub, &got_frame, avpkt);
	if (ret < 0) {
		serprintf("codec_ffsub: error decoding subtitle\n");
		avsubtitle_free(&sub);
		av_packet_free(&avpkt);
		return 1;
	}

	if (!got_frame) {
		DBGS serprintf("codec_ffsub: no subtitle frame\n");
		avsubtitle_free(&sub);
		av_packet_free(&avpkt);
		return 0;
	}

	DBGS serprintf("codec_ffsub: decoded subtitle, format %d, start %d, end %d, rects %d, pts %d\n",
					sub.format, // 0 is graphic
					sub.start_display_time,
					sub.end_display_time,
					sub.num_rects,
					sub.pts);

	// PGS can finish a display set in a later packet; its composition PTS is
	// authoritative. Packet timestamps are already in the stream TS domain.
	int64_t base = sub.pts != AV_NOPTS_VALUE ?
		av_rescale_q(sub.pts, AV_TIME_BASE_Q, (AVRational){1, 1000}) : time;
	int64_t start_time = base + RST_TO_TS_DELTA(sub.start_display_time, int64_t);
	if (start_time < -1 || start_time > INT_MAX) {
		avsubtitle_free(&sub);
		av_packet_free(&avpkt);
		return 1;
	}
	frame->time = private_start >= 0 ? private_start : (int)start_time;
	frame->duration = private_duration;
	if (private_start < 0 && sub.end_display_time != UINT32_MAX &&
	    sub.end_display_time >= sub.start_display_time) {
		int64_t duration = RST_TO_TS_DELTA(
			(int64_t)sub.end_display_time - sub.start_display_time, int64_t);
		frame->duration = duration <= INT_MAX ? (int)duration : -1;
	}

	// Calculate the bounding box for all rectangles
	int left = frame->width, top = frame->height, right = 0, bottom = 0;
	int has_bitmap = 0;

	// BGRA bitmap
	int bb_width = 0, bb_height = 0;

	for (int i = 0; i < sub.num_rects; i++) {
		AVSubtitleRect *rect = sub.rects[i];
		if (rect->type == SUBTITLE_BITMAP) {
			// A PGS display set without its cached object may decode to an
			// empty rectangle. Reject it before replacing the previous bitmap;
			// it is not the explicit zero-rectangle event that clears a cue.
			if (rect->w <= 0 || rect->h <= 0 || !rect->data[0] ||
			    !rect->data[1] || rect->linesize[0] < rect->w) {
				DBGS serprintf("codec_ffsub: incomplete bitmap rectangle\n");
				avsubtitle_free(&sub);
				av_packet_free(&avpkt);
				return 1;
			}
			has_bitmap = 1;
			left = MIN(left, rect->x);
			top = MIN(top, rect->y);
			right = MAX(right, rect->x + rect->w);
			bottom = MAX(bottom, rect->y + rect->h);
		}
	}

	if (sub.format == 0 && sub.num_rects == 0) {
		// this is to signal a change of subtitles with no rect (seen with pgs without duration timing)
		DBGS serprintf("codec_ffsub: no rect, reset bitmap\n");
		has_bitmap = 1;
		// create small empty bitmap
		left = 0; top = 0; right = 1, bottom = 1;
		frame->duration = 0; // cannot be -1 to get timed subtitle in java world but signal that this is a special end subtitle to SubtitleManager
	}

	if (has_bitmap) {
		// Allocate BGRA bitmap
		DBGS serprintf("codec_ffsub: Bounding box: left=%d, top=%d, right=%d, bottom=%d\n", left, top, right, bottom);
		bb_width = right - left;
		bb_height = bottom - top;

		// Check for valid dimensions
		if (bb_width <= 0 || bb_height <= 0) {
			serprintf("codec_ffsub: Invalid bitmap dimensions: %dx%d\n", bb_width, bb_height);
			avsubtitle_free(&sub);
			av_packet_free(&avpkt);
			return 1;
		}

		// Add a minimum size check
		bb_width = MAX(bb_width, 1);
		bb_height = MAX(bb_height, 1);

		uint8_t *bitmap_data[4] = {0};
		int bitmap_linesize[4] = {0};
		int ret = av_image_alloc(bitmap_data, bitmap_linesize, bb_width, bb_height, AV_PIX_FMT_BGRA, 32);
		if (ret < 0) {
			char error_buffer[AV_ERROR_MAX_STRING_SIZE] = {0};
			av_strerror(ret, error_buffer, AV_ERROR_MAX_STRING_SIZE);
			serprintf("codec_ffsub: Failed to allocate BGRA bitmap (%dx%d). Error: %s\n", bb_width, bb_height, error_buffer);
			avsubtitle_free(&sub);
			av_packet_free(&avpkt);
			return 1;
		}
		av_freep(&self->bitmap);
		self->bitmap = bitmap_data[0];
		memcpy(frame->data, bitmap_data, sizeof(frame->data));
		memcpy(frame->linestep, bitmap_linesize, sizeof(frame->linestep));
		memset(self->bitmap, 0, ret);
	}

	for (int i = 0; i < sub.num_rects; i++) {
		AVSubtitleRect *rect = sub.rects[i];
		DBGS serprintf("codec_ffsub: text is %s ass is %s type is %d\n", rect->text, rect->ass, rect->type);
		DBGS serprintf("codec_ffsub: unprocessed sub start %d, end %d, pts %d, duration %d, time %d\n", sub.start_display_time, sub.end_display_time, sub.pts, sub.end_display_time - sub.start_display_time, sub.pts + sub.start_display_time);
		if (rect->text || rect->ass) {
			if (!frame->data[0] || max < 1) {
				avsubtitle_free(&sub);
				av_packet_free(&avpkt);
				return 1;
			}
			const char *text = rect->text;
			if (!text) {
				text = rect->ass;
				int commas = !strncmp(text, "Dialogue:", 9) ? 9 : 8;
				for (int j = 0; j < commas && text; ++j) {
					text = strchr(text, ',');
					if (text) ++text;
				}
			}
			strnZcpy((char *)frame->data[0], text ? text : "", max);
			char *pos = (char *)frame->data[0];
			while ((pos = strstr(pos, "\\N"))) {
				pos[0] = ' ';
				pos[1] = '\n';
				pos += 2;
			}
		} else if (rect->type == SUBTITLE_BITMAP) {
			// Check if the bitmap rect is not empty and contains non-black pixels
			DBGS serprintf("codec_ffsub: blend bitmap rect\n");

			// perform the blending

			// Initialize SwsContext for this rectangle
			struct SwsContext *sws_ctx = sws_getContext( rect->w, rect->h, AV_PIX_FMT_PAL8, rect->w, rect->h,
														 AV_PIX_FMT_BGRA, SWS_BILINEAR, NULL, NULL, NULL );

			if( !sws_ctx ) {
				serprintf( "codec_ffsub: Failed to create SwsContext\n" );
				continue;
			}

			// Set up source data pointers and line sizes
			const uint8_t *src_data[4] = { rect->data[0], rect->data[1], NULL, NULL };
			int src_linesize[4] = { rect->linesize[0], 0, 0, 0 };
			if (self->base._subtitle.format == SUB_FORMAT_DVD_GFX && rect->nb_colors == 4) {
				uint32_t *palette = (uint32_t *)rect->data[1];
				int counts[4] = {0, 0, 0, 0};
					int dominant_index = -1;
					int fill_count = -1;
					int secondary_index = -1;
					int outline_count = 0x7FFFFFFF;

				// DVD subtitles can reuse the 4 palette slots differently across files.
				// Count actual bitmap index usage to normalize the dominant visible slot
				// and avoid relying on a fixed slot order.
				for (int y = 0; y < rect->h; y++) {
					const uint8_t *src_row = rect->data[0] + y * rect->linesize[0];
					for (int x = 0; x < rect->w; x++) {
						uint8_t idx = src_row[x] & 0x03;
						counts[idx]++;
					}
				}

				for (int c = 0; c < 4; c++) {
					uint32_t alpha = palette[c] & 0xFF000000;

					if (alpha == 0 || counts[c] == 0)
						continue;

					if (counts[c] > fill_count) {
						fill_count = counts[c];
							dominant_index = c;
						}
					}

				for (int c = 0; c < 4; c++) {
					uint32_t alpha = palette[c] & 0xFF000000;

						if (alpha == 0 || counts[c] == 0 || c == dominant_index)
							continue;

						if (counts[c] < outline_count) {
							outline_count = counts[c];
							secondary_index = c;
						}
					}

					if (dominant_index >= 0) {
						DBGS serprintf("codec_ffsub: dvd gfx normalize counts=[%d,%d,%d,%d] dominant_index=%d dominant_count=%d secondary_index=%d secondary_count=%d\n",
								counts[0], counts[1], counts[2], counts[3],
								dominant_index, fill_count, secondary_index,
								secondary_index >= 0 ? outline_count : -1);
						for (int c = 0; c < 4; c++) {
							uint32_t before = palette[c];
							uint32_t alpha = palette[c] & 0xFF000000;

							if (alpha == 0 || counts[c] == 0)
								continue;

							if (c == dominant_index) {
								palette[c] = alpha;
								DBGS serprintf("codec_ffsub: dvd gfx palette[%d] 0x%08X -> 0x%08X (dominant index -> black)\n",
										c, before, palette[c]);
							} else {
								palette[c] = alpha | 0x00FFFFFF;
								DBGS serprintf("codec_ffsub: dvd gfx palette[%d] 0x%08X -> 0x%08X (non-dominant visible index -> white)\n",
										c, before, palette[c]);
							}
						}
				}
			}
			// Set up destination data pointers and line sizes
			uint8_t *dst_data[4] = { frame->data[0] + (rect->y - top) * frame->linestep[0] + (rect->x - left) * 4, NULL, NULL, NULL };
			int dst_linesize[4] = { frame->linestep[0], 0, 0, 0 };

			// Perform the conversion
			sws_scale(sws_ctx, src_data, src_linesize, 0, rect->h, dst_data, dst_linesize);

			// Free the SwsContext
			sws_free_context(&sws_ctx);
		}
	}

	if (has_bitmap) {
		if (sub.num_rects && sub.end_display_time == UINT32_MAX) {
			// PGS ends with the next composition/clear, not a fixed timeout.
			// Keep the timed-message contract without overflowing Java's end time.
			frame->duration = INT_MAX - MAX(0, frame->time);
		}
		frame->window.x = left;
		frame->window.y = top;
		frame->window.width = bb_width;
		frame->window.height = bb_height;
		// Coordinates belong to the subtitle canvas, not the video surface.
		int canvas_width = self->avcontext->width;
		int canvas_height = self->avcontext->height;
		if (canvas_width <= 0) canvas_width = dec->_subtitle.format == SUB_FORMAT_PGS ? 1920 : 720;
		if (canvas_height <= 0) canvas_height = dec->_subtitle.format == SUB_FORMAT_PGS ? 1080 : 576;
		frame->width = MAX(canvas_width, right);
		frame->height = MAX(canvas_height, bottom);
		frame->colorspace = AV_IMAGE_BGRA_32;  // Set the colorspace to BGRA
		DBGS serprintf("codec_ffsub: decoded sub width=%d, height=%d, size=%d, window=%d,%d,%d,%d\n", frame->width, frame->height, frame->size, frame->window.x, frame->window.y, frame->window.width, frame->window.height);
	}

	DBGS serprintf("codec_ffsub: decoded sub start %d, end %d, pts %d, duration %d, time %d\n", sub.start_display_time, sub.end_display_time, sub.pts, frame->duration, frame->time);

	avsubtitle_free(&sub);
	av_packet_free(&avpkt);

	*pframe = frame;
	return 0;
}


static int _flush( STREAM_DEC_SUB *dec )
{
	my_dec_sub *self = (my_dec_sub *)dec;
	if (dec->_subtitle.format == SUB_FORMAT_PGS) {
		// This FFmpeg PGS decoder has no flush callback. Reopen to discard
		// palettes, objects and incomplete compositions from the old timeline.
		SUB_PROPERTIES sub = dec->_subtitle;
		void *ctx = dec->ctx;
		_close(dec);
		return _open(dec, &sub, ctx);
	}
	if (self->avcontext) avcodec_flush_buffers(self->avcontext);
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
STREAM_REGISTER_DEC_SUB( SUB_FORMAT_WEBVTT, _new_dec, "WEBVTT" );
