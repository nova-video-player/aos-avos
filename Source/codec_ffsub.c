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
#include "sub_engine.h"

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
	DBGS serprintf("codec_ffsub: ffsub_open format %d\n", sub->format);

	dec->subtitle = &dec->_subtitle;
	*dec->subtitle = *sub;

	dec->ctx = ctx;
	dec->is_open = 1;

	const AVCodec* myCodec;
	if (sub->format == SUB_FORMAT_MOV_TEXT) {
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

	if (!myCodec) {
		serprintf("codec_ffsub: codec not found\n");
	}

	self->avcontext = avcodec_alloc_context3(myCodec);
	avcodec_open2(self->avcontext, myCodec, NULL);
	DBGS serprintf("codec_ffsub: ffsub: Allocated avcontext %p\n", self->avcontext);

	// --- NATIVE OPENGL UPGRADE ---
	// Safely initialize the hardware GFX track for PGS and DVD subtitles
	//extern SUB_ENGINE *g_sub_engine;
	STREAM *stream = (STREAM *)ctx;
	if (stream && stream->sub_engine && (sub->format == SUB_FORMAT_PGS || sub->format == SUB_FORMAT_DVD_GFX)) {
		int w = 1920;
		int h = 1080;
		STREAM *stream = (STREAM *)ctx;
		if (stream && stream->video) {
			if (stream->video->width > 0) w = stream->video->width;
			if (stream->video->height > 0) h = stream->video->height;
		}
		sub_engine_open_track((SUB_ENGINE*)stream->sub_engine, sub_fmt_from_format(sub->format), w, h, NULL, 0, NULL, 0, NULL);
		// GFX/bitmap track (PGS/VobSub), opened synchronously here off the codec's own open() call --
		// fed via sub_engine_feed_bitmap(), not the checkpointed _gen() token system, so there's no
		// long-lived job to pin a generation token to; see sub_engine_open_track()'s doc comment
		// in sub_engine.h and the matching internal-track call sites in stream_subtitle.c.
	}

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
	// note that when ffmpeg sub decoding provides 0 rect for pgs it means that the previous subtitle is over

	my_dec_sub *self = (my_dec_sub*)dec;
	DBG {
		serprintf("codec_ffsub: ffsub decode\n", data, size);
		Dump(data, size);
	}

	VIDEO_FRAME *frame = *pframe;
	int max = frame->size - 1;

	frame->time = time;
	frame->duration = -1;

    // Fix: Explicitly clear the buffer to prevent ghosting of old text
    // when a container sends an empty clear-screen packet.
    if (frame->data[0]) {
        frame->data[0][0] = '\0';
    }

	// mov_text/webvtt packets carry a 4-byte little-endian duration prepended by
	// stream_parser_ffmpeg.c's _get_subtitle_cdata() (same trick already used for
	// raw SSA/TEXT passthrough), because AVSubtitle.start_display_time/
	// end_display_time always come back 0 from these two ffmpeg decoders -- there
	// is no other source for the real per-cue duration. Strip it before handing
	// the payload to avcodec_decode_subtitle2().
	int packet_duration = -1;
	if ((self->base._subtitle.format == SUB_FORMAT_MOV_TEXT ||
	     self->base._subtitle.format == SUB_FORMAT_WEBVTT) &&
	    size >= (int)sizeof(int)) {
		packet_duration = *(int*)data;
		data += sizeof(int);
		size -= sizeof(int);
	}

	DBGS serprintf("codec_ffsub: frame width=%d, height=%d, size=%d\n", frame->width, frame->height, frame->size);

	AVPacket *avpkt = av_packet_alloc();
	if (!avpkt) {
		serprintf("codec_ffsub: Failed to allocate AVPacket\n");
		return 1;
	}

	// Fix: Allocate AV_INPUT_BUFFER_PADDING_SIZE to prevent the webvtt string
    // parser from reading uninitialized heap memory and outputting garbage text.
	char *avdata = av_malloc(size + AV_INPUT_BUFFER_PADDING_SIZE);
	if (!avdata) {
		serprintf("codec_ffsub: Failed to allocate AVPacket data\n");
		av_packet_free(&avpkt);
		return 1;
	}

	memcpy(avdata, data, size);
	memset(avdata + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
	av_packet_from_data(avpkt, avdata, size);
	DBGS serprintf("codec_ffsub: avpkt->pts=%d, avpkt->dts=%d overridden by time=%d\n", avpkt->pts, avpkt->dts, time);
	avpkt->pts = time;
	avpkt->dts = time;
	// Mirror mpv's sub/lavc_conv.c (mp_set_av_packet): hand the real packet
	// duration to ffmpeg via avpkt->duration BEFORE decode, not just read it
	// out afterwards. This is what lets avcodec_decode_subtitle2() populate
	// AVSubtitle.end_display_time correctly for mov_text/webvtt -- without
	// it, avpkt->duration is left unset (0) and end_display_time comes back 0.
	if (packet_duration >= 0) {
		avpkt->duration = packet_duration;
	}

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
					sub.format, // 0 is graphic
					sub.start_display_time,
					sub.end_display_time,
					sub.num_rects,
					sub.pts);

	// Calculate the bounding box for all rectangles
	int left = frame->width, top = frame->height, right = 0, bottom = 0;
	int has_bitmap = 0;

	// BGRA bitmap
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

	if (sub.format == 0 && sub.num_rects == 0) {
		// this is to signal a change of subtitles with no rect (seen with pgs without duration timing)
		DBGS serprintf("codec_ffsub: no rect, reset bitmap\n");
		has_bitmap = 1;
		// create small empty bitmap
		left = 0; top = 0; right = 1, bottom = 1;
		frame->time = time;
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

		int ret = av_image_alloc(frame->data, frame->linestep, bb_width, bb_height, AV_PIX_FMT_BGRA, 32);
		if (ret < 0) {
			char error_buffer[AV_ERROR_MAX_STRING_SIZE] = {0};
			av_strerror(ret, error_buffer, AV_ERROR_MAX_STRING_SIZE);
			serprintf("codec_ffsub: Failed to allocate BGRA bitmap (%dx%d). Error: %s\n", bb_width, bb_height, error_buffer);
			avsubtitle_free(&sub);
			av_packet_free(&avpkt);
			return 1;
		}
	}

	for (int i = 0; i < sub.num_rects; i++) {
		AVSubtitleRect *rect = sub.rects[i];
		DBGS serprintf("codec_ffsub: text is %s ass is %s type is %d\n", rect->text, rect->ass, rect->type);
		DBGS serprintf("codec_ffsub: unprocessed sub start %d, end %d, pts %d, duration %d, time %d\n", sub.start_display_time, sub.end_display_time, sub.pts, sub.end_display_time - sub.start_display_time, sub.pts + sub.start_display_time);
		if (rect->text != NULL) {
			char *dst = frame->data[0];
			*dst = 0;
			DBGS serprintf("codec_ffsub: rect->text%s\n", rect->text);
			// Note that external srt are handled directly by Android and not by codec_ffsub
			frame->time = sub.pts + sub.start_display_time;
			frame->duration = sub.end_display_time - sub.start_display_time;
			// sub.start_display_time/end_display_time are always 0 for mov_text/webvtt --
			// prefer the duration that was prepended to the packet by the demuxer.
			if (packet_duration >= 0) {
				frame->duration = packet_duration;
			} else if (sub.start_display_time == 0 && sub.end_display_time == 0) {
				// legacy fallback for callers that never had a duration prefix
				int start, end;
				if(sscanf( data, "%d:%d,", &start, &end ) == 2) {
					frame->time = start;
					frame->duration = end - start;
				}
			}
			strnZcpy(dst, rect->text, max - 1);
		} else if (rect->ass != NULL) {
			char *dst = frame->data[0];
			*dst = 0;
			// note that AV_CODEC_ID_TEXT codec outputs ass rect
			DBGS serprintf("codec_ffsub: rect->ass=%s\n", rect->ass);
			int start, end;
			char *pos = rect->ass;
			// surprisingly start and end are zero out of the ffmpeg decoder: try to infer it from ass txt and if it fails  parse the data
			// typical format for ffmpeg 7.1 is rect->ass="1,0,Default,,0,0,0,,4704:7998,- Kids?\N- Phil, would you get them?"
			// skip to 9th comma to extract start and end times
			// typical format for ffmpeg 4.4 is
			// rect->ass="Dialogue: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,,1217:2956,Ronflement léger"
			// but can be without timestamp information
			// rect->ass="Dialogue: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,,{\fs20}{\1c&HFFFFFF&}{\1a&H00&}there usually aren't\Na lot of taxis in this area,"
			int skipCommas; // number of commas to skip
			if (strncmp(pos, "Dialogue:", 9) == 0) { // match
				// ffmpeg 4.4 decoding format
				skipCommas = 9;
			} else {
				// ffmpeg 7.1 decoding format
				skipCommas = 8;
			}
			for (int i = 0; i < skipCommas && pos != NULL; i++) {
				pos = strchr(pos, ',');
				if (pos) pos++;
			}
			int found_timing = 0;
			if (packet_duration >= 0) {
				// Authoritative: duration prepended by the demuxer from the real
				// packet duration. frame->time was already set from `time` above
				// this loop (the pts passed into _decode()), which is correct --
				// only duration was ever missing. Do NOT set found_timing here:
				// that flag also triggers an extra comma-skip meant to consume an
				// inline "start:end," segment, which isn't present at `pos` in
				// this case -- pos already points at the text after skipCommas.
				frame->duration = packet_duration;
			} else if (pos != NULL && sscanf(pos, "%d:%d,", &start, &end) == 2) {
				frame->time = start;
				frame->duration = end - start;
				found_timing = 1;
			} else {
				// parsing error get back to text data parsing
				if (sub.start_display_time == 0 && sub.end_display_time == 0) {
					if(sscanf( data, "%d:%d,", &start, &end ) == 2) {
						frame->time = start;
						frame->duration = end - start;
					}
				}
			}
			// Continue skipping to the 9th comma to reach the text content only if an
			// inline "start:end," segment was actually found and consumed above --
			// not when duration came from the packet_duration prefix, since in that
			// case pos already points straight at the text.
			if (pos != NULL && found_timing) {
				pos = strchr(pos, ',');
				if (pos) pos++;
			}
			// Extract text zone
			if (pos != NULL)
				strnZcpy(dst, pos, max - 1);
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
		if (sub.pts > 0 && sub.start_display_time > 0) {
			frame->time = sub.pts + sub.start_display_time;
		}
		if (sub.num_rects != 0) {
			frame->duration = sub.end_display_time - sub.start_display_time;
			if( sub.start_display_time == 0 && sub.end_display_time == -1 ) {
				// note that for PGS subtitles there is no start_display_time and end_display_time
				// so we have to calculate the duration from the avpkt->duration but it is always 0
				if( avpkt->duration > 0 ) {
					frame->duration = avpkt->duration;
				} else {
					// Note: must fix a duration for PGS subtitles, real duration inferred from next 0 rect subtitle in the android domain
					frame->duration = 100000;
				}
			}
		}
		frame->window.x = left;
		frame->window.y = top;
		frame->window.width = bb_width;
		frame->window.height = bb_height;
		// Set frame resolution based on subtitle format if PGS or VobSub
		if (self->base._subtitle.format == SUB_FORMAT_PGS) {
			frame->width = MAX(1920, right); // safer but breaks AR
			frame->height = MAX(1080, bottom); // safer but breaks AR
		} else if (self->base._subtitle.format == SUB_FORMAT_DVD_GFX) {
			int base_width = 720;
			int base_height = 576;
			STREAM *stream = (STREAM *)self->base.ctx;
			if (stream && stream->video) {
				if (stream->video->width > 0)
					base_width = stream->video->width;
				if (stream->video->height > 0)
					base_height = stream->video->height;
			}
			frame->width = MAX(base_width, right);
			frame->height = MAX(base_height, bottom);
		}
		frame->colorspace = AV_IMAGE_BGRA_32;  // Set the colorspace to BGRA
		DBGS serprintf("codec_ffsub: decoded sub width=%d, height=%d, size=%d, window=%d,%d,%d,%d\n", frame->width, frame->height, frame->size, frame->window.x, frame->window.y, frame->window.width, frame->window.height);
	}

	DBGS serprintf("codec_ffsub: decoded sub start %d, end %d, pts %d, duration %d, time %d\n", sub.start_display_time, sub.end_display_time, sub.pts, frame->duration, frame->time);

	avsubtitle_free(&sub);
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
STREAM_REGISTER_DEC_SUB( SUB_FORMAT_PGS, _new_dec, "PGS" );
STREAM_REGISTER_DEC_SUB( SUB_FORMAT_DVD_GFX, _new_dec, "vobsub" );
STREAM_REGISTER_DEC_SUB( SUB_FORMAT_WEBVTT, _new_dec, "WEBVTT" );
