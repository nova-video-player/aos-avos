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
#include "ac3_recode.h"

#ifdef CONFIG_FFMPEG_AUDIO
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/error.h>
#include <libswresample/swresample.h>

#define DBG if(0)

// AC3 encoding defaults
#define AC3_BITRATE_5POINT1 640000  // 640 kbps for multichannel content
#define AC3_BITRATE_STEREO   192000 // 192 kbps for 2.0 content
#define AC3_MAX_FRAMES_PER_OUTPUT 8 // keeps worst-case IEC batch below the 64 KiB SPDIF buffer
#define SURROUND_FOLD_GAIN 0.70710678  // -3 dB fold-down for back surrounds

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
	int eof;                      // Resampler and encoder tail queued
	int encoder_drained;
	int have_input;
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
	case 6:
		ret = av_channel_layout_from_mask(layout, AV_CH_LAYOUT_5POINT1);
		break;
	case 7:
		ret = av_channel_layout_from_mask(layout, AV_CH_LAYOUT_6POINT1);
		break;
	case 8:
		ret = av_channel_layout_from_mask(layout, AV_CH_LAYOUT_7POINT1);
		break;
	default:
		av_channel_layout_default(layout, MIN(channels, 8));
		ret = 0;
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

static int channel_index_from_layout(const AVChannelLayout *layout, enum AVChannel channel)
{
	if (!layout) {
		return -1;
	}

	for (unsigned int i = 0; i < layout->nb_channels; ++i) {
		if (av_channel_layout_channel_from_index(layout, i) == channel) {
			return (int)i;
		}
	}
	return -1;
}

static inline void add_gain(double *matrix, int out_channels, int in_channels,
			    int out_index, int in_index, double gain)
{
	if (!matrix || gain == 0.0 || out_index < 0 || in_index < 0) {
		return;
	}
	matrix[out_index * in_channels + in_index] += gain;
}

static void ensure_row_has_gain(double *matrix, int row, int in_channels)
{
	if (!matrix || row < 0) {
		return;
	}
	int has_gain = 0;
	for (int c = 0; c < in_channels; ++c) {
		if (matrix[row * in_channels + c] != 0.0) {
			has_gain = 1;
			break;
		}
	}
	if (!has_gain) {
		// Fallback: simply copy matching channel index to keep audio flowing
		matrix[row * in_channels + (row % in_channels)] = 1.0;
	}
}

static int configure_downmix_matrix(struct ctx *ctx, const AVChannelLayout *in_layout)
{
	if (!ctx || !ctx->enc_ctx || !ctx->swr_ctx || !in_layout) {
		return 0;
	}

	const int out_channels = ctx->enc_ctx->ch_layout.nb_channels;
	const int in_channels = in_layout->nb_channels;

	// Only build a matrix when we truly need to fold channels (e.g. 7.1 -> 5.1)
	if (in_channels <= out_channels) {
		return 0;
	}

	double *matrix = acalloc(out_channels * in_channels, sizeof(double));
	if (!matrix) {
		serprintf("faac3: failed to allocate downmix matrix (%d x %d)\n", out_channels, in_channels);
		return AVERROR(ENOMEM);
	}

	// Map fronts directly
	add_gain(matrix, out_channels, in_channels,
		channel_index_from_layout(&ctx->enc_ctx->ch_layout, AV_CHAN_FRONT_LEFT),
		channel_index_from_layout(in_layout, AV_CHAN_FRONT_LEFT), 1.0);
	add_gain(matrix, out_channels, in_channels,
		channel_index_from_layout(&ctx->enc_ctx->ch_layout, AV_CHAN_FRONT_RIGHT),
		channel_index_from_layout(in_layout, AV_CHAN_FRONT_RIGHT), 1.0);
	add_gain(matrix, out_channels, in_channels,
		channel_index_from_layout(&ctx->enc_ctx->ch_layout, AV_CHAN_FRONT_CENTER),
		channel_index_from_layout(in_layout, AV_CHAN_FRONT_CENTER), 1.0);
	add_gain(matrix, out_channels, in_channels,
		channel_index_from_layout(&ctx->enc_ctx->ch_layout, AV_CHAN_LOW_FREQUENCY),
		channel_index_from_layout(in_layout, AV_CHAN_LOW_FREQUENCY), 1.0);

	// Derive surround left/right rows (fold back surrounds with -3 dB)
	int out_sl = channel_index_from_layout(&ctx->enc_ctx->ch_layout, AV_CHAN_SIDE_LEFT);
	if (out_sl < 0) {
		out_sl = channel_index_from_layout(&ctx->enc_ctx->ch_layout, AV_CHAN_BACK_LEFT);
	}
	int out_sr = channel_index_from_layout(&ctx->enc_ctx->ch_layout, AV_CHAN_SIDE_RIGHT);
	if (out_sr < 0) {
		out_sr = channel_index_from_layout(&ctx->enc_ctx->ch_layout, AV_CHAN_BACK_RIGHT);
	}

	int in_sl = channel_index_from_layout(in_layout, AV_CHAN_SIDE_LEFT);
	int in_sr = channel_index_from_layout(in_layout, AV_CHAN_SIDE_RIGHT);
	int in_bl = channel_index_from_layout(in_layout, AV_CHAN_BACK_LEFT);
	int in_br = channel_index_from_layout(in_layout, AV_CHAN_BACK_RIGHT);
	int in_bc = channel_index_from_layout(in_layout, AV_CHAN_BACK_CENTER);

	add_gain(matrix, out_channels, in_channels, out_sl, in_sl, in_sl >= 0 ? 1.0 : 0.0);
	add_gain(matrix, out_channels, in_channels, out_sr, in_sr, in_sr >= 0 ? 1.0 : 0.0);
	add_gain(matrix, out_channels, in_channels, out_sl, in_bl, SURROUND_FOLD_GAIN);
	add_gain(matrix, out_channels, in_channels, out_sr, in_br, SURROUND_FOLD_GAIN);

	// If we only have a single back center channel, split it equally between SL/SR
	if (in_bc >= 0) {
		add_gain(matrix, out_channels, in_channels, out_sl, in_bc, SURROUND_FOLD_GAIN);
		add_gain(matrix, out_channels, in_channels, out_sr, in_bc, SURROUND_FOLD_GAIN);
	}

	// Guarantee every output row has at least one contributor
	for (int row = 0; row < out_channels; ++row) {
		ensure_row_has_gain(matrix, row, in_channels);
	}

	int ret = swr_set_matrix(ctx->swr_ctx, matrix, in_channels);
	if (ret < 0) {
		char errbuf[64];
		av_strerror(ret, errbuf, sizeof(errbuf));
		serprintf("faac3: failed to set custom downmix matrix (%s)\n", errbuf);
	}

	if (ret >= 0) {
		int opt_ret = av_opt_set_double(ctx->swr_ctx, "rematrix_volume", 1.0, 0);
		if (opt_ret < 0) {
			serprintf("faac3: warning - failed to set rematrix_volume option (%d)\n", opt_ret);
		}
#ifdef AV_MATRIX_ENCODING_DPLIIX
		opt_ret = av_opt_set_int(ctx->swr_ctx, "matrix_encoding", in_channels >= 8 ? AV_MATRIX_ENCODING_DPLIIX : AV_MATRIX_ENCODING_DOLBY, 0);
#else
		opt_ret = av_opt_set_int(ctx->swr_ctx, "matrix_encoding", AV_MATRIX_ENCODING_DOLBY, 0);
#endif
		if (opt_ret < 0) {
			serprintf("faac3: warning - failed to set matrix_encoding option (%d)\n", opt_ret);
		}
		serprintf("faac3: configured custom %dch -> %dch downmix matrix for AC3 encoding\n",
			in_channels, out_channels);
	}

	afree(matrix);
	return ret;
}

static int select_target_channels(int input_channels)
{
	if (input_channels <= 0) {
		return 2;
	}
	if (input_channels <= 2) {
		return 2;
	}
	if (input_channels >= 6) {
		return 6;
	}
	return input_channels;
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

	// Reset the published recode layout on entry so a partially-initialized open
	// (or a reinit) cannot inherit or leave stale layout state; it is republished
	// only just before this function returns success.
	libavos_set_ac3_recode_target_stereo(0);

	// Allocate filter context
	struct ctx *ctx = acalloc(1, sizeof(struct ctx));
	if (!ctx) {
		serprintf("faac3: failed to allocate context\n");
		return -1;
	}

	f->priv = ctx;
	ctx->enabled = 1;  // This filter is created only for AC3 recoding
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

	int input_channels = audio->channels > 0 ? audio->channels : 2;
	int target_channels = select_target_channels(input_channels);
	ctx->channels = input_channels;

	// Configure encoder for target output
	ctx->enc_ctx->sample_rate = AC3_RECODE_SAMPLE_RATE;
	ctx->enc_ctx->bit_rate = (target_channels <= 2) ? AC3_BITRATE_STEREO : AC3_BITRATE_5POINT1;
	ctx->enc_ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;  // AC3 encoder uses planar float

	// Set encoder channel layout based on target channels (max 6)
	if (init_channel_layout(&ctx->enc_ctx->ch_layout, target_channels) < 0) {
		serprintf("faac3: failed to select encoder channel layout (%d channels)\n", target_channels);
		goto error;
	}

	int bitrate_kbps = (int)(ctx->enc_ctx->bit_rate / 1000);
	serprintf("faac3: configuring AC3 encoder target=%dch (%s) bitrate=%d kbps (source=%dch)\n",
		target_channels, (target_channels <= 2) ? "2.0" : "multichannel",
		bitrate_kbps, input_channels);

	if (avcodec_open2(ctx->enc_ctx, codec, NULL) < 0) {
		serprintf("faac3: failed to open AC3 encoder\n");
		goto error;
	}

	// Allocate resampler if sample rate or format conversion needed
	AVChannelLayout in_ch_layout = { 0 };
	if (init_channel_layout(&in_ch_layout, input_channels) < 0) {
		serprintf("faac3: failed to select input channel layout (%d channels)\n", input_channels);
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

	if (configure_downmix_matrix(ctx, &in_ch_layout) < 0) {
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

	int init_bitrate_kbps = (int)(ctx->enc_ctx->bit_rate / 1000);
	DBG serprintf("faac3: AC3 encoder initialized - %dch input @ %d Hz -> %dch AC3 %d kbps\n",
		input_channels, audio->samplesPerSec, target_channels, init_bitrate_kbps);

	// Open fully succeeded: publish the recode output layout for the mode2 latency
	// policy. A multichannel target keeps app_latency; a stereo 2.0/192k target
	// uses pipeline_latency. Latched into the AudioTrack context at AC3-sink config.
	libavos_set_ac3_recode_target_stereo(target_channels <= 2);
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

// Errors must never leave the original PCM available to a compressed sink.
static int frame_error(AUDIO_FRAME *frame)
{
	frame->data = NULL;
	frame->size = 0;
	frame->fakeSize = 0;
	frame->error = 1;
	return -1;
}

static int queue_resampled(struct ctx *ctx, const uint8_t *input, int samples)
{
	const uint8_t *in_data[] = { input };
	uint8_t *out_data[AV_NUM_DATA_POINTERS] = { NULL };
	int capacity = av_rescale_rnd(
		swr_get_delay(ctx->swr_ctx, ctx->sample_rate) + samples,
		ctx->enc_ctx->sample_rate, ctx->sample_rate, AV_ROUND_UP);
	// A NULL-input conversion must be allowed to flush even with no reported delay.
	if (capacity < ctx->enc_ctx->frame_size)
		capacity = ctx->enc_ctx->frame_size;
	if (av_samples_alloc(out_data, NULL, ctx->enc_ctx->ch_layout.nb_channels,
		capacity, ctx->enc_ctx->sample_fmt, 0) < 0)
		return -1;
	int count = swr_convert(ctx->swr_ctx, out_data, capacity,
		input ? in_data : NULL, samples);
	if (count > 0 &&
	    (av_audio_fifo_realloc(ctx->fifo, av_audio_fifo_size(ctx->fifo) + count) < 0 ||
	     av_audio_fifo_write(ctx->fifo, (void **)out_data, count) != count))
		count = -1;
	av_freep(&out_data[0]);
	return count;
}

static int append_packet(struct ctx *ctx)
{
	if (!ctx->pkt->data || ctx->pkt->size <= 0 ||
	    ctx->pkt->size > ctx->encode_buffer_size - ctx->encode_buffer_used) {
		av_packet_unref(ctx->pkt);
		return -1;
	}
	memcpy(ctx->encode_buffer + ctx->encode_buffer_used, ctx->pkt->data, ctx->pkt->size);
	ctx->encode_buffer_used += ctx->pkt->size;
	ctx->encoded_samples += ctx->enc_ctx->frame_size;
	av_packet_unref(ctx->pkt);
	return 0;
}

static int encode_batch(struct ctx *ctx, AUDIO_FRAME *output)
{
	ctx->encode_buffer_used = 0;
	ctx->encoded_samples = 0;
	int frame_size = ctx->enc_ctx->frame_size;
	while (ctx->encoded_samples / frame_size < AC3_MAX_FRAMES_PER_OUTPUT) {
		int available = av_audio_fifo_size(ctx->fifo);
		if (available < frame_size && (!ctx->eof || !available))
			break;
		int count = available < frame_size ? available : frame_size;
		if (av_frame_make_writable(ctx->frame) < 0 ||
		    av_audio_fifo_read(ctx->fifo, (void **)ctx->frame->data, count) != count)
			return frame_error(output);
		// A receiver consumes complete 1536-sample frames, including EOF padding.
		if (count < frame_size && av_samples_set_silence(ctx->frame->data, count,
			frame_size - count, ctx->enc_ctx->ch_layout.nb_channels,
			ctx->enc_ctx->sample_fmt) < 0)
			return frame_error(output);
		ctx->frame->nb_samples = frame_size;
		ctx->frame->pts = ctx->pts;
		ctx->pts += frame_size;
		if (avcodec_send_frame(ctx->enc_ctx, ctx->frame) < 0)
			return frame_error(output);
		// The selected AC3 encoder produces exactly one packet per input frame.
		if (avcodec_receive_packet(ctx->enc_ctx, ctx->pkt) < 0 || append_packet(ctx) < 0)
			return frame_error(output);
	}
	if (ctx->eof && !av_audio_fifo_size(ctx->fifo) && !ctx->encoder_drained) {
		if (avcodec_send_frame(ctx->enc_ctx, NULL) < 0)
			return frame_error(output);
		// AC3 has no delayed packets. Its overlap was flushed with silence below.
		if (avcodec_receive_packet(ctx->enc_ctx, ctx->pkt) != AVERROR_EOF) {
			av_packet_unref(ctx->pkt);
			return frame_error(output);
		}
		ctx->encoder_drained = 1;
	}
	output->data = ctx->encode_buffer;
	output->size = ctx->encode_buffer_used;
	output->format = WAVE_FORMAT_AC3;
	output->channels = ctx->channels;
	output->samplesPerSec = ctx->enc_ctx->sample_rate;
	output->bits = 16;
	output->fakeSize = ctx->encoded_samples * ctx->channels * sizeof(int16_t);
	return 0;
}

static int _filter(STREAM_FILTER_AUDIO *f, AUDIO_FRAME *frame)
{
	if (!frame)
		return -1;
	if (!frame->size) {
		frame->fakeSize = 0;
		return 0;
	}
	if (!frame->data || frame->size < 0 || frame->format != WAVE_FORMAT_PCM ||
	    frame->bits != 16 || frame->channels <= 0 || frame->samplesPerSec <= 0)
		return frame_error(frame);

	struct ctx *ctx = f->priv;
	if (ctx && (ctx->channels != frame->channels || ctx->sample_rate != frame->samplesPerSec)) {
		ctx_free(ctx);
		f->priv = NULL;
		ctx = NULL;
	}
	if (!ctx) {
		AUDIO_PROPERTIES props = {0};
		props.channels = frame->channels;
		props.samplesPerSec = frame->samplesPerSec;
		props.bitsPerSample = frame->bits;
		if (_open(f, &props) < 0)
			return frame_error(frame);
		ctx = f->priv;
	}
	if (!ctx->enabled || ctx->eof || !ctx->enc_ctx || !ctx->swr_ctx || !ctx->fifo ||
	    frame->size % (ctx->channels * sizeof(int16_t)))
		return frame_error(frame);
	int samples = frame->size / (ctx->channels * sizeof(int16_t));
	if (queue_resampled(ctx, frame->data, samples) < 0)
		return frame_error(frame);
	ctx->have_input = 1;
	return encode_batch(ctx, frame);
}

static int _drain(STREAM_FILTER_AUDIO *f, AUDIO_FRAME *frame, int end)
{
	memset(frame, 0, sizeof(*frame));
	struct ctx *ctx = f->priv;
	if (!ctx)
		return 0;
	if (end && !ctx->eof) {
		int count;
		do {
			count = queue_resampled(ctx, NULL, 0);
			if (count < 0)
				return frame_error(frame);
		} while (count > 0);
		// The AC3 encoder retains initial_padding samples of overlap, even when
		// the FIFO ends on a full frame. Feed silence to release that final audio.
		int tail = ctx->have_input ? ctx->enc_ctx->initial_padding : 0;
		if (tail > 0) {
			if (tail > ctx->enc_ctx->frame_size || av_frame_make_writable(ctx->frame) < 0 ||
			    av_samples_set_silence(ctx->frame->data, 0, tail,
				ctx->enc_ctx->ch_layout.nb_channels, ctx->enc_ctx->sample_fmt) < 0 ||
			    av_audio_fifo_realloc(ctx->fifo, av_audio_fifo_size(ctx->fifo) + tail) < 0 ||
			    av_audio_fifo_write(ctx->fifo, (void **)ctx->frame->data, tail) != tail)
				return frame_error(frame);
		}
		ctx->eof = 1;
	}
	return encode_batch(ctx, frame);
}

static int _flush(STREAM_FILTER_AUDIO *f)
{
	// avcodec_flush_buffers() is a no-op for this encoder. Discard its overlap
	// and the resampler together, then reopen lazily from the next decoded PCM.
	ctx_free(f->priv);
	f->priv = NULL;
	return 0;
}

static int _set_param(STREAM_FILTER_AUDIO *f, void *params, void *night_on)
{
	if (!f || !f->priv) {
		DBG serprintf("faac3: set_param called with null parameters\n");
		return -1;
	}

	struct ctx *ctx = f->priv;

	// AC3 filter should always be enabled in AC3 recoding mode
	// The level/night_on parameters control the compress filter, not AC3 encoding
	// Only disable if encoder is not initialized (pure passthrough mode)
	if (!ctx->enc_ctx) {
		DBG serprintf("faac3: encoder not initialized, keeping disabled\n");
		return 0;
	}

	// Enable AC3 encoding if encoder is available
	if (!ctx->enabled) {
		ctx->enabled = 1;
		serprintf("faac3: AC3 encoding enabled for recoding mode\n");
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
	delay_samples += AC3_RECODE_FRAME_SAMPLES;

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
	f->drain     = _drain;

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
