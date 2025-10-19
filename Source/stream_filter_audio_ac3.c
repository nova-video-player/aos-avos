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

/*
 * AC3 Encoding Audio Filter for Nova Video Player
 *
 * This filter provides universal passthrough support by encoding multichannel
 * decoded PCM audio to AC3 5.1. This is useful for devices that support AC3
 * passthrough but not the original codec (e.g., DTS, TrueHD, etc.).
 *
 * Features:
 * - Encodes PCM to AC3 5.1 at configurable bitrate (default: 640 kbps)
 * - Supports up to 6 channels (5.1 surround)
 * - Uses FFmpeg's AC3 encoder
 * - Automatic resampling to 48kHz (AC3 standard)
 *
 * Integration:
 * - Initialized in stream_video.c via stream_filter_audio_ac3_new()
 * - Applied to decoded PCM frames before passthrough
 * - Can be enabled/disabled via set_param()
 */

#include "global.h"
#include "stream_filter_audio.h"
#include "debug.h"
#include "astdlib.h"
#include "util.h"

#ifdef CONFIG_FFMPEG_AUDIO
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/error.h>
#include <libswresample/swresample.h>

#define DBG if(1)

// AC3 encoding defaults
#define AC3_SAMPLE_RATE 48000
#define AC3_BITRATE 640000  // 640 kbps for 5.1
#define AC3_FRAME_SIZE 1536  // Standard AC3 frame size

struct ctx {
	AVCodecContext *enc_ctx;      // FFmpeg encoder context
	SwrContext *swr_ctx;          // Resampler context
	AVFrame *frame;               // Input frame for encoder
	AVPacket *pkt;                // Output packet from encoder
	AVAudioFifo *fifo;            // FIFO for sample accumulation
	uint8_t *encode_buffer;       // Buffer for encoded AC3 data
	int encode_buffer_size;       // Size of encode buffer
	int encode_buffer_used;       // Bytes currently in buffer
	int encoded_samples;          // Number of PCM samples represented by encoded data
	int enabled;                  // Filter enabled flag
	int channels;                 // Number of input channels
	int sample_rate;              // Input sample rate
	int64_t pts;                  // Presentation timestamp
};

static void ctx_free(struct ctx *ctx)
{
	if (!ctx) {
		return;
	}

	if (ctx->swr_ctx) {
		swr_free(&ctx->swr_ctx);
	}
	if (ctx->frame) {
		av_frame_free(&ctx->frame);
	}
	if (ctx->pkt) {
		av_packet_free(&ctx->pkt);
	}
	if (ctx->fifo) {
		av_audio_fifo_free(ctx->fifo);
	}
	if (ctx->enc_ctx) {
		avcodec_free_context(&ctx->enc_ctx);
	}
	if (ctx->encode_buffer) {
		afree(ctx->encode_buffer);
	}

	afree(ctx);
}

static int init_channel_layout(AVChannelLayout *layout, int channels)
{
	if (!layout || channels <= 0) {
		return -1;
	}

	int ret = -1;

	switch (channels) {
	case 1:
		ret = av_channel_layout_from_mask(layout, AV_CH_LAYOUT_MONO);
		break;
	case 2:
		ret = av_channel_layout_from_mask(layout, AV_CH_LAYOUT_STEREO);
		break;
	case 3:
		ret = av_channel_layout_from_mask(layout, AV_CH_LAYOUT_SURROUND);
		break;
	case 4:
		ret = av_channel_layout_from_mask(layout, AV_CH_LAYOUT_QUAD);
		break;
	case 5:
		ret = av_channel_layout_from_mask(layout, AV_CH_LAYOUT_5POINT0);
		break;
	default:
		ret = av_channel_layout_from_mask(layout, AV_CH_LAYOUT_5POINT1);
		break;
	}

	if (ret < 0) {
		int fallback_channels = MIN(channels, 6);
		av_channel_layout_default(layout, fallback_channels);
		if (layout->nb_channels != fallback_channels) {
			serprintf("faac3: failed to init channel layout fallback (channels=%d)\n", fallback_channels);
			return -1;
		}
		serprintf("faac3: using default channel layout fallback (%d channels)\n", fallback_channels);
	}

	return 0;
}

static int _delete(STREAM_FILTER_AUDIO *f)
{
	serprintf("faac3: delete\n");
	if (f && f->priv) {
		ctx_free(f->priv);
		f->priv = NULL;
	}
	if (f) {
		afree(f);
	}
	return 0;
}

static int _open(STREAM_FILTER_AUDIO *f, AUDIO_PROPERTIES *audio)
{
	serprintf("faac3: open - channels=%d sampleRate=%d bitsPerSample=%d\n",
		audio->channels, audio->samplesPerSec, audio->bitsPerSample);

	// Allocate filter context
	struct ctx *ctx = acalloc(1, sizeof(struct ctx));
	if (!ctx) {
		serprintf("faac3: failed to allocate context\n");
		return -1;
	}

	f->priv = ctx;
	ctx->enabled = 0;  // Disabled by default, enabled via set_param
	ctx->channels = audio->channels;
	ctx->sample_rate = audio->samplesPerSec;
	ctx->pts = 0;

	// Find AC3 encoder
	const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AC3);
	if (!codec) {
		serprintf("faac3: AC3 encoder not found\n");
		goto error;
	}

	// Allocate encoder context
	ctx->enc_ctx = avcodec_alloc_context3(codec);
	if (!ctx->enc_ctx) {
		serprintf("faac3: failed to allocate encoder context\n");
		goto error;
	}

	// Configure encoder for 5.1 output
	ctx->enc_ctx->sample_rate = AC3_SAMPLE_RATE;
	ctx->enc_ctx->bit_rate = AC3_BITRATE;
	ctx->enc_ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;  // AC3 encoder uses planar float

	// Set encoder channel layout based on input channels
	if (init_channel_layout(&ctx->enc_ctx->ch_layout, audio->channels) < 0) {
		serprintf("faac3: failed to select encoder channel layout (%d channels)\n", audio->channels);
		goto error;
	}

	// Open encoder - if this fails, we might still be in passthrough mode
	// so we allow the filter to be created but keep it disabled
	if (avcodec_open2(ctx->enc_ctx, codec, NULL) < 0) {
		serprintf("faac3: WARNING - failed to open AC3 encoder (might be passthrough mode)\n");
		// Clean up encoder context but allow filter creation
		if (ctx->enc_ctx) {
			avcodec_free_context(&ctx->enc_ctx);
			ctx->enc_ctx = NULL;
		}
		// Skip resampler and FIFO initialization
		serprintf("faac3: filter created in passthrough-only mode\n");
		return 0;
	}

	// Allocate resampler if sample rate or format conversion needed
	AVChannelLayout in_ch_layout = { 0 };
	if (init_channel_layout(&in_ch_layout, audio->channels) < 0) {
		serprintf("faac3: failed to select input channel layout (%d channels)\n", audio->channels);
		goto error;
	}

	if (swr_alloc_set_opts2(&ctx->swr_ctx,
			&ctx->enc_ctx->ch_layout, ctx->enc_ctx->sample_fmt, ctx->enc_ctx->sample_rate,
			&in_ch_layout, AV_SAMPLE_FMT_S16, audio->samplesPerSec,
			0, NULL) < 0) {
		serprintf("faac3: failed to allocate resampler\n");
		av_channel_layout_uninit(&in_ch_layout);
		goto error;
	}
	av_channel_layout_uninit(&in_ch_layout);

	if (swr_init(ctx->swr_ctx) < 0) {
		serprintf("faac3: failed to initialize resampler\n");
		goto error;
	}

	// Allocate frame for encoder input
	ctx->frame = av_frame_alloc();
	if (!ctx->frame) {
		serprintf("faac3: failed to allocate frame\n");
		goto error;
	}
	ctx->frame->nb_samples = ctx->enc_ctx->frame_size;
	ctx->frame->format = ctx->enc_ctx->sample_fmt;
	av_channel_layout_copy(&ctx->frame->ch_layout, &ctx->enc_ctx->ch_layout);

	if (av_frame_get_buffer(ctx->frame, 0) < 0) {
		serprintf("faac3: failed to allocate frame buffer\n");
		goto error;
	}

	// Allocate packet for encoder output
	ctx->pkt = av_packet_alloc();
	if (!ctx->pkt) {
		serprintf("faac3: failed to allocate packet\n");
		goto error;
	}

	// Allocate encode buffer (max AC3 frame is ~2KB, allocate 64KB for safety)
	ctx->encode_buffer_size = 65536;
	ctx->encode_buffer = amalloc(ctx->encode_buffer_size);
	if (!ctx->encode_buffer) {
		serprintf("faac3: failed to allocate encode buffer\n");
		goto error;
	}
	ctx->encoded_samples = 0;
	ctx->encode_buffer_used = 0;

	ctx->fifo = av_audio_fifo_alloc(ctx->enc_ctx->sample_fmt, ctx->enc_ctx->ch_layout.nb_channels, ctx->enc_ctx->frame_size * 8);
	if (!ctx->fifo) {
		serprintf("faac3: failed to allocate audio fifo\n");
		goto error;
	}

	DBG serprintf("faac3: AC3 encoder initialized - %d channels @ %d Hz -> AC3 %d kbps\n",
		audio->channels, audio->samplesPerSec, AC3_BITRATE / 1000);
	return 0;

error:
	ctx_free(ctx);
	f->priv = NULL;
	return -1;
}

static int _close(STREAM_FILTER_AUDIO *f)
{
	serprintf("faac3: close\n");
	return 0;
}

static int _filter(STREAM_FILTER_AUDIO *f, AUDIO_FRAME *frame)
{
	struct ctx *ctx = f->priv;

	// Safety checks
	if (!ctx || !frame || !frame->data || frame->size <= 0) {
		return 0;
	}

	serprintf("faac3: filter call fmt=%04X size=%d enabled=%d\n", frame->format, frame->size, ctx->enabled);

	// Only process if filter is enabled
	if (!ctx->enabled) {
		serprintf("faac3: filter disabled, bypassing\n");
		return 0;
	}

	// Convert input samples to encoder format
	const uint8_t *in_data[1] = { frame->data };
	int in_samples = frame->size / (ctx->channels * sizeof(int16_t));

	// Resample to encoder's format
	uint8_t *out_data[AV_NUM_DATA_POINTERS] = { NULL };
	int out_samples = av_rescale_rnd(
		swr_get_delay(ctx->swr_ctx, ctx->sample_rate) + in_samples,
		ctx->enc_ctx->sample_rate, ctx->sample_rate, AV_ROUND_UP);

	if (av_samples_alloc(out_data, NULL, ctx->enc_ctx->ch_layout.nb_channels,
			out_samples, ctx->enc_ctx->sample_fmt, 0) < 0) {
		serprintf("faac3: failed to allocate resampled buffer\n");
		return 0;  // Don't break audio chain, just skip encoding
	}

	out_samples = swr_convert(ctx->swr_ctx, out_data, out_samples, in_data, in_samples);
	if (out_samples < 0) {
		serprintf("faac3: resampling failed\n");
		av_freep(&out_data[0]);
		return 0;  // Don't break audio chain, just skip encoding
	}

	// Push converted samples into FIFO
	if (out_samples > 0) {
		if (av_audio_fifo_realloc(ctx->fifo, av_audio_fifo_size(ctx->fifo) + out_samples) < 0) {
			serprintf("faac3: failed to realloc fifo\n");
			av_freep(&out_data[0]);
			return 0;
		}
		av_audio_fifo_write(ctx->fifo, (void **)out_data, out_samples);
		serprintf("faac3: wrote %d samples to fifo (size=%d)\n", out_samples, av_audio_fifo_size(ctx->fifo));
	}

	av_freep(&out_data[0]);

	// Encode frames when we have enough samples
	while (av_audio_fifo_size(ctx->fifo) >= ctx->enc_ctx->frame_size) {
		int fifo_size = av_audio_fifo_size(ctx->fifo);
		serprintf("faac3: encoding frame from fifo (size=%d)\n", fifo_size);
		if (av_frame_make_writable(ctx->frame) < 0) {
			serprintf("faac3: failed to make frame writable\n");
			return 0;
		}

		if (av_audio_fifo_read(ctx->fifo, (void **)ctx->frame->data, ctx->enc_ctx->frame_size) < ctx->enc_ctx->frame_size) {
			serprintf("faac3: failed to read enough samples from fifo\n");
			return 0;
		}

		ctx->frame->nb_samples = ctx->enc_ctx->frame_size;
		ctx->frame->pts = ctx->pts;
		ctx->pts += ctx->enc_ctx->frame_size;

		int ret = avcodec_send_frame(ctx->enc_ctx, ctx->frame);
		if (ret < 0) {
			char errbuf[64];
			av_strerror(ret, errbuf, sizeof(errbuf));
			serprintf("faac3: ERROR - failed to send frame to encoder (%d:%s)\n", ret, errbuf);
			// Reset encoder on error
			avcodec_flush_buffers(ctx->enc_ctx);
			return 0;
		}

		while (avcodec_receive_packet(ctx->enc_ctx, ctx->pkt) == 0) {
			if (ctx->encode_buffer_used + ctx->pkt->size > ctx->encode_buffer_size) {
				serprintf("faac3: encode buffer overflow\n");
				av_packet_unref(ctx->pkt);
				return 0;
			}

			memcpy(ctx->encode_buffer + ctx->encode_buffer_used, ctx->pkt->data, ctx->pkt->size);
			ctx->encode_buffer_used += ctx->pkt->size;
			ctx->encoded_samples += ctx->enc_ctx->frame_size;
			serprintf("faac3: encoded AC3 packet size=%d (total=%d samples=%d)\n",
				ctx->pkt->size, ctx->encode_buffer_used, ctx->encoded_samples);

			av_packet_unref(ctx->pkt);
		}
	}

	// Replace frame data with encoded AC3
	if (ctx->encode_buffer_used > 0) {
		frame->data = ctx->encode_buffer;
		frame->size = ctx->encode_buffer_used;
		frame->format = WAVE_FORMAT_AC3;  // Mark as AC3
		frame->samplesPerSec = ctx->enc_ctx->sample_rate;
		frame->channels = 2; // IEC61937 encapsulation uses stereo container
		frame->bits = 16;
		if (ctx->encoded_samples > 0) {
			frame->fakeSize = ctx->encoded_samples *
			                  ctx->enc_ctx->ch_layout.nb_channels * sizeof(int16_t);
		} else {
			frame->fakeSize = 0;
		}
		serprintf("faac3: SUCCESS - produced AC3 frame size=%d fakeSize=%d format=0x%04X\n",
			frame->size, frame->fakeSize, frame->format);
		ctx->encode_buffer_used = 0;
		ctx->encoded_samples = 0;
	} else {
		// No encoded frame ready yet; drop PCM frame to avoid format mismatch
		// This causes ~32ms audio delay during initial buffering
		serprintf("faac3: buffering samples (fifo size=%d), dropping input frame\n",
			ctx->fifo ? av_audio_fifo_size(ctx->fifo) : 0);
		frame->size = 0;
		frame->fakeSize = 0;
	}

	return 0;
}

static int _flush(STREAM_FILTER_AUDIO *f)
{
	struct ctx *ctx = f->priv;
	DBG serprintf("faac3: flush\n");

	if (ctx && ctx->enc_ctx) {
		// Flush and reset encoder to accept new frames
		avcodec_flush_buffers(ctx->enc_ctx);
	}

	if (ctx) {
		ctx->encode_buffer_used = 0;
		ctx->encoded_samples = 0;
		ctx->pts = 0;
		if (ctx->fifo) {
			av_audio_fifo_reset(ctx->fifo);
		}
	}

	return 0;
}

static int _set_param(STREAM_FILTER_AUDIO *f, void *params, void *night_on)
{
	if (!f || !f->priv || !params) {
		DBG serprintf("faac3: set_param called with null parameters\n");
		return -1;
	}

	int *enabled = params;
	struct ctx *ctx = f->priv;

	int new_enabled = (*enabled) ? 1 : 0;

	DBG serprintf("faac3: set_param called - enabled=%d\n", new_enabled);

	// Only allow enabling if encoder is initialized (not in passthrough-only mode)
	if (new_enabled && !ctx->enc_ctx) {
		serprintf("faac3: cannot enable - encoder not initialized (passthrough mode)\n");
		return 0;  // Success but keep disabled
	}

	serprintf("faac3: set_param -> %s\n", new_enabled ? "enabled" : "disabled");

	if (ctx->enabled != new_enabled) {
		ctx->enabled = new_enabled;
		DBG serprintf("faac3: filter %s\n", ctx->enabled ? "enabled" : "disabled");
		serprintf("faac3: runtime state %s\n", ctx->enabled ? "enabled" : "disabled");

		if (!ctx->enabled) {
			// Flush encoder when disabling
			_flush(f);
		}
	}

	return 0;
}

static int _delay(STREAM_FILTER_AUDIO *f)
{
	if (!f || !f->priv) {
		return 0;
	}

	struct ctx *ctx = f->priv;

	// When filter is disabled or encoder not initialized, no delay
	if (!ctx->enabled || !ctx->enc_ctx) {
		return 0;
	}

	// Verify encoder is fully initialized
	if (!ctx->enc_ctx->codec || ctx->enc_ctx->sample_rate <= 0) {
		return 0;
	}

	// Calculate total encoding pipeline delay in milliseconds
	int delay_samples = 0;

	// 1. FIFO buffer delay - samples waiting to be encoded
	if (ctx->fifo) {
		delay_samples += av_audio_fifo_size(ctx->fifo);
	}

	// 2. Encoder internal delay - AC3 encoder always has 1 frame delay (1536 samples)
	// This is the lookahead needed to produce the first encoded frame
	delay_samples += AC3_FRAME_SIZE;

	// 3. Resampler delay
	if (ctx->swr_ctx) {
		delay_samples += swr_get_delay(ctx->swr_ctx, ctx->enc_ctx->sample_rate);
	}

	// Convert samples to milliseconds at encoder sample rate (48kHz)
	int delay_ms = (delay_samples * 1000) / ctx->enc_ctx->sample_rate;

	return delay_ms;
}

STREAM_FILTER_AUDIO *stream_filter_audio_ac3_new(void)
{
	STREAM_FILTER_AUDIO *f = acalloc(1, sizeof(STREAM_FILTER_AUDIO));

	if (!f) {
		serprintf("faac3: failed to allocate filter structure\n");
		return NULL;
	}

	// Initialize function pointers for audio filter interface
	static char name[] = "AC3 Encoder";
	f->name      = name;
	f->delete    = _delete;
	f->open      = _open;
	f->close     = _close;
	f->filter    = _filter;
	f->flush     = _flush;
	f->set_param = _set_param;
	f->delay     = _delay;

	DBG serprintf("faac3: AC3 encoder filter created successfully\n");
	return f;
}

#else
// Stub implementation when FFmpeg is not available
STREAM_FILTER_AUDIO *stream_filter_audio_ac3_new(void)
{
	serprintf("faac3: AC3 encoder not available (FFmpeg disabled)\n");
	return NULL;
}
#endif
