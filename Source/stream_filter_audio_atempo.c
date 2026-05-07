/*
 * Copyright 2025 Courville Software
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
 * Audio Speed Control Filter using FFmpeg atempo
 *
 * This filter provides audio playback speed adjustment without pitch change
 * using FFmpeg's high-quality atempo filter. It replaces the AudioTrack
 * playback rate method for broader compatibility and consistent behavior.
 *
 * Features:
 * - Speed range: 0.5x to 2.0x
 * - Pitch preservation via atempo algorithm
 * - Works on all Android API levels (no PlaybackParams dependency)
 * - Handles PCM audio in various sample formats
 * - Dynamic filter chaining keeps each atempo instance within its supported range
 * - Runtime tempo updates (no graph rebuild on speed changes)
 *
 * Architecture:
 * - When atempo is active, timeline mapping is DISABLED
 * - Parser leaves timestamps in RST (real stream time) domain
 * - atempo physically changes audio duration to match playback speed
 * - AudioTrack plays at normal 1.0x rate
 *
 * Integration:
 * - Initialized in stream audio pipeline via stream_filter_audio_atempo_new()
 * - Speed controlled via audio_interface_get_audio_speed()
 * - Filter automatically enabled when speed != 1.0
 */

#include "global.h"
#include "stream_filter_audio.h"
#include "audio_interface.h"
#include "debug.h"
#include "astdlib.h"
#include "util.h"
#include "atime.h"

#ifdef CONFIG_FFMPEG_AUDIO
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/audio_fifo.h>

#define DBGA DBG_IF(Debug[DBG_AUD])
#define DBG DBG_IF(Debug[DBG_AUD])

// atempo filter constraints (FFmpeg limitation per filter instance)
#define ATEMPO_MIN 0.5f
#define ATEMPO_MAX 2.0f

// Practical speed limits supported by this implementation
#define SPEED_MIN 0.5f
#define SPEED_MAX 2.0f

struct ctx {
	AVFilterGraph *filter_graph;
	AVFilterContext *abuffer_ctx;
	AVFilterContext *aformat_in_ctx;
	AVFilterContext *atempo_ctx;
	AVFilterContext *aformat_out_ctx;
	AVFilterContext *abuffersink_ctx;
	AVFrame *in_frame;
	AVFrame *out_frame;
	AVAudioFifo *fifo;                  // FIFO for output buffering

	float current_speed;
	int channels;
	int sample_rate;
    enum AVSampleFormat format;
    uint8_t channel_layout[64];

    int enabled;                        // Filter enabled flag
    int filter_initialized;             // Filter graph ready flag

    uint8_t *output_buffer;             // Temporary buffer for filtered PCM
    int output_buffer_size;             // Size of temporary buffer in bytes

	int last_delay_ms;                  // Last reported delay (ms)
	int last_speed_change_ms;           // Timestamp of last speed change (ms)
	int delay_log_count;                // throttle noisy delay diagnostics
	int last_fifo_ms;                   // FIFO depth at last _delay() call (ms)
	int last_fifo_samples;              // FIFO depth at last _delay() call (samples)
	int last_input_samples;             // input samples from last _filter() call
	int last_target_samples;            // target output samples from last _filter() call
	int last_output_samples;            // actual output samples from last _filter() call
};

static int _flush(STREAM_FILTER_AUDIO *f);

static void atempo_reset_runtime_baseline(struct ctx *ctx, const char *reason)
{
	if (!ctx) {
		return;
	}
	if (ctx->fifo) {
		int fifo_samples = av_audio_fifo_size(ctx->fifo);
		int fifo_ms = (ctx->sample_rate > 0) ? (fifo_samples * 1000) / ctx->sample_rate : -1;
		DBGA serprintf("atempo: reset runtime baseline reason=%s fifo_samples=%d fifo_ms=%d last_delay=%d\n",
			reason ? reason : "unknown", fifo_samples, fifo_ms, ctx->last_delay_ms);
		av_audio_fifo_reset(ctx->fifo);
	}
	ctx->last_delay_ms = -1;
	ctx->last_speed_change_ms = 0;
	ctx->delay_log_count = 0;
	ctx->last_fifo_ms = -1;
	ctx->last_fifo_samples = -1;
	ctx->last_input_samples = -1;
	ctx->last_target_samples = -1;
	ctx->last_output_samples = -1;
}

static int atempo_update_speed(struct ctx *ctx, float speed)
{
	if (!ctx || !ctx->filter_graph || !ctx->atempo_ctx) {
		return -1;
	}

	char arg[32];
	char res[128];
	snprintf(arg, sizeof(arg), "%.6f", speed);

	int rc = avfilter_graph_send_command(ctx->filter_graph, "atempo0", "tempo",
		arg, res, sizeof(res), 0);
	if (rc < 0) {
		DBGA serprintf("atempo: runtime tempo update failed speed=%.3f rc=%d\n", speed, rc);
		return rc;
	}

	DBGA serprintf("atempo: runtime tempo update %.3f -> %.3f\n", ctx->current_speed, speed);
	DBGA serprintf("atempo_var: reason=speed_change prev_speed=%.3f new_speed=%.3f delay=%d fifo_samples=%d fifo_ms=%d last_in=%d last_target=%d last_out=%d\n",
		ctx->current_speed, speed, ctx->last_delay_ms,
		ctx->fifo ? av_audio_fifo_size(ctx->fifo) : -1,
		(ctx->fifo && ctx->sample_rate > 0) ? (av_audio_fifo_size(ctx->fifo) * 1000) / ctx->sample_rate : -1,
		ctx->last_input_samples, ctx->last_target_samples, ctx->last_output_samples);
	ctx->current_speed = speed;

	// Reusing the same FFmpeg atempo runtime state across tempo changes can
	// carry a stale FIFO backlog into the next steady state. Reset the wrapper
	// locally on every tempo change and let the normal cadence rebuild delay
	// from the new speed.
	atempo_reset_runtime_baseline(ctx, "speed_change");
	// Re-arm the post-speed-change window after the reset clears it.
	ctx->last_speed_change_ms = atime();
	return 0;
}

static void ctx_free(struct ctx *ctx)
{
	if (!ctx) {
		return;
	}

	if (ctx->filter_graph) {
		avfilter_graph_free(&ctx->filter_graph);
	}
	ctx->aformat_in_ctx = NULL;
	ctx->atempo_ctx = NULL;
	ctx->aformat_out_ctx = NULL;
	if (ctx->in_frame) {
		av_frame_free(&ctx->in_frame);
	}
	if (ctx->out_frame) {
		av_frame_free(&ctx->out_frame);
	}
    if (ctx->fifo) {
        av_audio_fifo_free(ctx->fifo);
    }
    if (ctx->output_buffer) {
        afree(ctx->output_buffer);
        ctx->output_buffer = NULL;
        ctx->output_buffer_size = 0;
    }

	afree(ctx);
}

static int get_sample_format_from_bits(int bits_per_sample)
{
	switch (bits_per_sample) {
	case 8:  return AV_SAMPLE_FMT_U8;
	case 16: return AV_SAMPLE_FMT_S16;
	case 24: return AV_SAMPLE_FMT_S32;
	case 32: return AV_SAMPLE_FMT_FLT;
	default: return AV_SAMPLE_FMT_NONE;
	}
}

static const char *sample_format_name(enum AVSampleFormat format)
{
	const char *name = av_get_sample_fmt_name(format);
	return name ? name : "unknown";
}

static int rebuild_filter_graph(struct ctx *ctx, float speed)
{
	int ret;

	// Flush existing graph before destruction
	if (ctx->filter_graph && ctx->abuffer_ctx && ctx->abuffersink_ctx) {
		DBGA serprintf("atempo: flushing graph. speed=%.3f current=%.3f FIFO size before: %d\n",
			speed, ctx->current_speed, av_audio_fifo_size(ctx->fifo));
		// Push EOF to source
		int ret = av_buffersrc_add_frame(ctx->abuffer_ctx, NULL);
		if (ret < 0) {
			serprintf("atempo: warning: failed to flush source buffer: %s\n", av_err2str(ret));
		}

		// Drain remaining frames to FIFO
		int flushed_samples = 0;
		while (1) {
			ret = av_buffersink_get_frame(ctx->abuffersink_ctx, ctx->out_frame);
			if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
				break;
			}
			if (ret < 0) {
				serprintf("atempo: warning: error draining frame: %s\n", av_err2str(ret));
				break;
			}

			// Write to FIFO
			if (av_audio_fifo_space(ctx->fifo) < ctx->out_frame->nb_samples) {
				if (av_audio_fifo_realloc(ctx->fifo,
					av_audio_fifo_size(ctx->fifo) + ctx->out_frame->nb_samples) < 0) {
					serprintf("atempo: failed to realloc FIFO during flush\n");
					av_frame_unref(ctx->out_frame);
					break;
				}
			}

			av_audio_fifo_write(ctx->fifo, (void **)ctx->out_frame->data, ctx->out_frame->nb_samples);
			flushed_samples += ctx->out_frame->nb_samples;
			av_frame_unref(ctx->out_frame);
		}
		serprintf("atempo: flushed %d samples. FIFO size after: %d\n", flushed_samples, av_audio_fifo_size(ctx->fifo));
	}

	// Free existing graph
    if (ctx->filter_graph) {
        avfilter_graph_free(&ctx->filter_graph);
        ctx->abuffer_ctx = NULL;
        ctx->aformat_in_ctx = NULL;
        ctx->abuffersink_ctx = NULL;
        ctx->aformat_out_ctx = NULL;
    }
    ctx->atempo_ctx = NULL;

	// Do NOT reset FIFO here - we want to preserve buffered audio across speed changes!

    // Clamp speed to supported range
    if (speed < SPEED_MIN) speed = SPEED_MIN;
    if (speed > SPEED_MAX) speed = SPEED_MAX;

    // Note: We no longer bypass at 1.0x speed. The filter is always active when called.
    // Bypass logic is handled in stream_audio.c based on feature enablement and passthrough mode.

	// Create new filter graph
	ctx->filter_graph = avfilter_graph_alloc();
	if (!ctx->filter_graph) {
		serprintf("atempo: failed to allocate filter graph\n");
		return -1;
	}

	// Create abuffer source
	const AVFilter *abuffer = avfilter_get_by_name("abuffer");
	if (!abuffer) {
		serprintf("atempo: abuffer filter not found\n");
		avfilter_graph_free(&ctx->filter_graph);
		return -1;
	}

	ctx->abuffer_ctx = avfilter_graph_alloc_filter(ctx->filter_graph, abuffer, "src");
	if (!ctx->abuffer_ctx) {
		serprintf("atempo: failed to allocate abuffer\n");
		avfilter_graph_free(&ctx->filter_graph);
		return -1;
	}

	char args[512];
	snprintf(args, sizeof(args),
		"channel_layout=%s:sample_fmt=%s:time_base=1/%d:sample_rate=%d",
		ctx->channel_layout, sample_format_name(ctx->format),
		ctx->sample_rate, ctx->sample_rate);

	ret = avfilter_init_str(ctx->abuffer_ctx, args);
	if (ret < 0) {
		serprintf("atempo: failed to init abuffer: %s\n", av_err2str(ret));
		avfilter_graph_free(&ctx->filter_graph);
		return -1;
	}

	const AVFilter *aformat_filter = avfilter_get_by_name("aformat");
	if (!aformat_filter) {
		serprintf("atempo: aformat filter not found\n");
		avfilter_graph_free(&ctx->filter_graph);
		return -1;
	}

	ctx->aformat_in_ctx = avfilter_graph_alloc_filter(ctx->filter_graph, aformat_filter, "afmt_in");
	if (!ctx->aformat_in_ctx) {
		serprintf("atempo: failed to allocate input aformat filter\n");
		avfilter_graph_free(&ctx->filter_graph);
		return -1;
	}

	snprintf(args, sizeof(args), "sample_fmts=flt:sample_rates=%d:channel_layouts=%s",
		ctx->sample_rate, ctx->channel_layout);
	ret = avfilter_init_str(ctx->aformat_in_ctx, args);
	if (ret < 0) {
		serprintf("atempo: failed to init input aformat: %s\n", av_err2str(ret));
		avfilter_graph_free(&ctx->filter_graph);
		ctx->aformat_in_ctx = NULL;
		return -1;
	}

	const AVFilter *atempo_filter = avfilter_get_by_name("atempo");
	if (!atempo_filter) {
		serprintf("atempo: atempo filter not found\n");
		avfilter_graph_free(&ctx->filter_graph);
		return -1;
	}

	ctx->atempo_ctx = avfilter_graph_alloc_filter(ctx->filter_graph, atempo_filter, "atempo0");
	if (!ctx->atempo_ctx) {
		serprintf("atempo: failed to allocate atempo filter\n");
		avfilter_graph_free(&ctx->filter_graph);
		return -1;
	}

	snprintf(args, sizeof(args), "tempo=%f", speed);
	ret = avfilter_init_str(ctx->atempo_ctx, args);
	if (ret < 0) {
		serprintf("atempo: failed to init atempo with tempo=%f: %s\n", speed, av_err2str(ret));
		avfilter_graph_free(&ctx->filter_graph);
		ctx->aformat_in_ctx = NULL;
		ctx->atempo_ctx = NULL;
		return -1;
	}

	ctx->aformat_out_ctx = avfilter_graph_alloc_filter(ctx->filter_graph, aformat_filter, "afmt_out");
	if (!ctx->aformat_out_ctx) {
		serprintf("atempo: failed to allocate output aformat filter\n");
		avfilter_graph_free(&ctx->filter_graph);
		ctx->aformat_in_ctx = NULL;
		ctx->atempo_ctx = NULL;
		return -1;
	}

	snprintf(args, sizeof(args), "sample_fmts=%s:sample_rates=%d:channel_layouts=%s",
		sample_format_name(ctx->format), ctx->sample_rate, ctx->channel_layout);
	ret = avfilter_init_str(ctx->aformat_out_ctx, args);
	if (ret < 0) {
		serprintf("atempo: failed to init output aformat: %s\n", av_err2str(ret));
		avfilter_graph_free(&ctx->filter_graph);
		ctx->aformat_in_ctx = NULL;
		ctx->atempo_ctx = NULL;
		ctx->aformat_out_ctx = NULL;
		return -1;
	}

	ret = avfilter_link(ctx->abuffer_ctx, 0, ctx->aformat_in_ctx, 0);
	if (ret < 0) {
		serprintf("atempo: failed to link abuffer -> aformat_in: %s\n", av_err2str(ret));
		avfilter_graph_free(&ctx->filter_graph);
		ctx->aformat_in_ctx = NULL;
		ctx->atempo_ctx = NULL;
		ctx->aformat_out_ctx = NULL;
		return -1;
	}

	ret = avfilter_link(ctx->aformat_in_ctx, 0, ctx->atempo_ctx, 0);
	if (ret < 0) {
		serprintf("atempo: failed to link aformat_in -> atempo: %s\n", av_err2str(ret));
		avfilter_graph_free(&ctx->filter_graph);
		ctx->aformat_in_ctx = NULL;
		ctx->atempo_ctx = NULL;
		ctx->aformat_out_ctx = NULL;
		return -1;
	}

 	// Create abuffersink
	const AVFilter *abuffersink = avfilter_get_by_name("abuffersink");
	if (!abuffersink) {
		serprintf("atempo: abuffersink filter not found\n");
		avfilter_graph_free(&ctx->filter_graph);
		return -1;
	}

	ctx->abuffersink_ctx = avfilter_graph_alloc_filter(ctx->filter_graph, abuffersink, "sink");
	if (!ctx->abuffersink_ctx) {
		serprintf("atempo: failed to allocate abuffersink\n");
		avfilter_graph_free(&ctx->filter_graph);
		return -1;
	}

	ret = avfilter_init_str(ctx->abuffersink_ctx, NULL);
	if (ret < 0) {
		serprintf("atempo: failed to init abuffersink: %s\n", av_err2str(ret));
		avfilter_graph_free(&ctx->filter_graph);
		return -1;
	}

	ret = avfilter_link(ctx->atempo_ctx, 0, ctx->aformat_out_ctx, 0);
	if (ret < 0) {
		serprintf("atempo: failed to link atempo -> aformat_out: %s\n", av_err2str(ret));
		avfilter_graph_free(&ctx->filter_graph);
		return -1;
	}

	ret = avfilter_link(ctx->aformat_out_ctx, 0, ctx->abuffersink_ctx, 0);
	if (ret < 0) {
		serprintf("atempo: failed to link aformat_out -> abuffersink: %s\n", av_err2str(ret));
		avfilter_graph_free(&ctx->filter_graph);
		return -1;
	}

	// Configure graph
	ret = avfilter_graph_config(ctx->filter_graph, NULL);
	if (ret < 0) {
		serprintf("atempo: failed to configure filter graph: %s\n", av_err2str(ret));
		avfilter_graph_free(&ctx->filter_graph);
		return -1;
	}

	ctx->current_speed = speed;
	ctx->filter_initialized = 1;

	DBGA serprintf("atempo: filter graph configured for speed %.3f (internal=flt output=%s)\n",
		speed, sample_format_name(ctx->format));

	return 0;
}

static int _open(STREAM_FILTER_AUDIO *f, AUDIO_PROPERTIES *audio)
{
	DBGA serprintf("atempo: open - channels=%d sampleRate=%d bitsPerSample=%d\n",
		audio->channels, audio->samplesPerSec, audio->bitsPerSample);

	struct ctx *ctx = acalloc(1, sizeof(struct ctx));
	if (!ctx) {
		serprintf("atempo: failed to allocate context\n");
		return -1;
	}

	f->priv = ctx;
	ctx->enabled = 0;
	ctx->filter_initialized = 0;
	ctx->channels = audio->channels;
	ctx->sample_rate = audio->samplesPerSec;
	ctx->current_speed = 1.0f;
	ctx->delay_log_count = 0;
	ctx->last_delay_ms = -1;
	ctx->last_fifo_ms = -1;
	ctx->last_fifo_samples = -1;
	ctx->last_input_samples = -1;
	ctx->last_target_samples = -1;
	ctx->last_output_samples = -1;

	// Determine sample format
	ctx->format = get_sample_format_from_bits(audio->bitsPerSample);
	if (ctx->format == AV_SAMPLE_FMT_NONE) {
		serprintf("atempo: unsupported bit depth %d\n", audio->bitsPerSample);
		goto error;
	}

	// Setup channel layout
	AVChannelLayout ch_layout = {0};
	av_channel_layout_default(&ch_layout, ctx->channels);
	int ret = av_channel_layout_describe(&ch_layout, (char *)ctx->channel_layout, sizeof(ctx->channel_layout));
	av_channel_layout_uninit(&ch_layout);
	if (ret < 0) {
		serprintf("atempo: failed to describe channel layout: %s\n", av_err2str(ret));
		goto error;
	}

	// Allocate frames
	ctx->in_frame = av_frame_alloc();
	ctx->out_frame = av_frame_alloc();
	if (!ctx->in_frame || !ctx->out_frame) {
		serprintf("atempo: failed to allocate frames\n");
		goto error;
	}

	ctx->in_frame->format = ctx->format;
	ctx->in_frame->sample_rate = ctx->sample_rate;
	av_channel_layout_default(&ctx->in_frame->ch_layout, ctx->channels);

	// Allocate FIFO for output buffering (handle variable output sizes)
	ctx->fifo = av_audio_fifo_alloc(ctx->format, ctx->channels, ctx->sample_rate * 2);
	if (!ctx->fifo) {
		serprintf("atempo: failed to allocate audio FIFO\n");
		goto error;
	}

	DBGA serprintf("atempo: initialized for %d channels, %d Hz, format %s\n",
		ctx->channels, ctx->sample_rate, sample_format_name(ctx->format));

	return 0;

error:
	ctx_free(ctx);
	f->priv = NULL;
	return -1;
}

static int _close(STREAM_FILTER_AUDIO *f)
{
	DBGA serprintf("atempo: close\n");
	struct ctx *ctx = f->priv;
	if (ctx) {
		if (ctx->filter_graph) {
			_flush(f);
			avfilter_graph_free(&ctx->filter_graph);
			ctx->abuffer_ctx = NULL;
			ctx->aformat_in_ctx = NULL;
			ctx->atempo_ctx = NULL;
			ctx->aformat_out_ctx = NULL;
			ctx->abuffersink_ctx = NULL;
		}
		if (ctx->fifo) {
			av_audio_fifo_reset(ctx->fifo);
		}
		ctx->filter_initialized = 0;
	}
	return 0;
}

static int atempo_reconfigure_format(struct ctx *ctx, int channels,
	int sample_rate, enum AVSampleFormat format)
{
	if (!ctx || channels <= 0 || sample_rate <= 0 || format == AV_SAMPLE_FMT_NONE) {
		return -1;
	}

	DBGA serprintf("atempo: reconfigure format %dch/%dHz/%s -> %dch/%dHz/%s\n",
		ctx->channels, ctx->sample_rate, sample_format_name(ctx->format),
		channels, sample_rate, sample_format_name(format));

	if (ctx->filter_graph) {
		avfilter_graph_free(&ctx->filter_graph);
		ctx->abuffer_ctx = NULL;
		ctx->aformat_in_ctx = NULL;
		ctx->atempo_ctx = NULL;
		ctx->aformat_out_ctx = NULL;
		ctx->abuffersink_ctx = NULL;
	}

	if (ctx->fifo) {
		av_audio_fifo_free(ctx->fifo);
		ctx->fifo = NULL;
	}
	if (ctx->output_buffer) {
		afree(ctx->output_buffer);
		ctx->output_buffer = NULL;
		ctx->output_buffer_size = 0;
	}

	ctx->channels = channels;
	ctx->sample_rate = sample_rate;
	ctx->format = format;
	ctx->filter_initialized = 0;
	ctx->current_speed = 1.0f;

	AVChannelLayout ch_layout = {0};
	av_channel_layout_default(&ch_layout, ctx->channels);
	int ret = av_channel_layout_describe(&ch_layout, (char *)ctx->channel_layout, sizeof(ctx->channel_layout));
	av_channel_layout_uninit(&ch_layout);
	if (ret < 0) {
		serprintf("atempo: failed to describe reconfigured channel layout: %s\n", av_err2str(ret));
		return -1;
	}

	if (ctx->in_frame) {
		av_channel_layout_uninit(&ctx->in_frame->ch_layout);
		ctx->in_frame->format = ctx->format;
		ctx->in_frame->sample_rate = ctx->sample_rate;
		av_channel_layout_default(&ctx->in_frame->ch_layout, ctx->channels);
	}

	ctx->fifo = av_audio_fifo_alloc(ctx->format, ctx->channels, ctx->sample_rate * 2);
	if (!ctx->fifo) {
		serprintf("atempo: failed to allocate reconfigured audio FIFO\n");
		return -1;
	}

	atempo_reset_runtime_baseline(ctx, "format_change");
	return 0;
}

static int _filter(STREAM_FILTER_AUDIO *f, AUDIO_FRAME *frame)
{
	struct ctx *ctx = f->priv;

	if (!ctx || !frame || !frame->data || frame->size <= 0) {
		return 0;
	}

	// Get current speed from audio interface
	float speed = audio_interface_is_audio_speed_enabled() ?
		audio_interface_get_audio_speed() : 1.0f;
	// Keep enabled state in sync with runtime audio speed selection.
	ctx->enabled = audio_interface_is_audio_speed_enabled() && audio_interface_is_using_atempo();
	if (speed < SPEED_MIN) {
		speed = SPEED_MIN;
	} else if (speed > SPEED_MAX) {
		speed = SPEED_MAX;
	}

	int frame_channels = frame->channels ? frame->channels : ctx->channels;
	int frame_sample_rate = frame->samplesPerSec ? frame->samplesPerSec : ctx->sample_rate;
	enum AVSampleFormat frame_format = frame->bits ? get_sample_format_from_bits(frame->bits) : ctx->format;
	if (frame_channels != ctx->channels ||
	    frame_sample_rate != ctx->sample_rate ||
	    frame_format != ctx->format) {
		if (atempo_reconfigure_format(ctx, frame_channels, frame_sample_rate, frame_format) < 0) {
			serprintf("atempo: failed to reconfigure for frame format %dch/%dHz/%s\n",
				frame_channels, frame_sample_rate, sample_format_name(frame_format));
			return -1;
		}
	}

	// Initialize filter graph on first call or update speed at runtime
	if (!ctx->filter_initialized || fabsf(ctx->current_speed - speed) > 0.001f) {
		if (!ctx->filter_initialized) {
			DBGA serprintf("atempo: initializing filter graph with speed %.3f (fifo=%d)\n",
				speed, ctx->fifo ? av_audio_fifo_size(ctx->fifo) : -1);
		}
		if (!ctx->filter_initialized) {
			if (rebuild_filter_graph(ctx, speed) < 0) {
				serprintf("atempo: failed to rebuild filter graph\n");
				return -1;
			}
				ctx->last_speed_change_ms = 0;
				ctx->last_delay_ms = -1;
				ctx->delay_log_count = 0;
			} else {
			DBGA serprintf("atempo: speed changed %.3f -> %.3f (fifo=%d)\n",
				ctx->current_speed, speed, ctx->fifo ? av_audio_fifo_size(ctx->fifo) : -1);
			if (atempo_update_speed(ctx, speed) < 0) {
				if (rebuild_filter_graph(ctx, speed) < 0) {
					serprintf("atempo: failed to rebuild filter graph\n");
					return -1;
				}
			}
				ctx->last_speed_change_ms = atime();
				ctx->delay_log_count = 0;
			}
		}

	int ret;
	int bytes_per_sample = av_get_bytes_per_sample(ctx->format) * ctx->channels;
	int fifo_before_push = ctx->fifo ? av_audio_fifo_size(ctx->fifo) : -1;
	int drained_frames = 0;
	int drained_samples = 0;

	// Setup input frame
	ctx->in_frame->nb_samples = frame->size / bytes_per_sample;
	ctx->in_frame->data[0] = frame->data;
	ctx->in_frame->linesize[0] = frame->size;

	// Push frame to filter
	ret = av_buffersrc_add_frame(ctx->abuffer_ctx, ctx->in_frame);
	if (ret < 0) {
		serprintf("atempo: failed to add frame to buffer: %s\n", av_err2str(ret));
		return -1;
	}

	// Pull all available filtered frames into FIFO
	while (1) {
		ret = av_buffersink_get_frame(ctx->abuffersink_ctx, ctx->out_frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
			break;
		}
		if (ret < 0) {
			serprintf("atempo: error getting frame: %s\n", av_err2str(ret));
			return -1;
		}

		// Write to FIFO
		if (av_audio_fifo_space(ctx->fifo) < ctx->out_frame->nb_samples) {
			if (av_audio_fifo_realloc(ctx->fifo,
				av_audio_fifo_size(ctx->fifo) + ctx->out_frame->nb_samples) < 0) {
				serprintf("atempo: failed to realloc FIFO\n");
				av_frame_unref(ctx->out_frame);
				return -1;
			}
		}

		av_audio_fifo_write(ctx->fifo, (void **)ctx->out_frame->data, ctx->out_frame->nb_samples);
		drained_frames++;
		drained_samples += ctx->out_frame->nb_samples;
		av_frame_unref(ctx->out_frame);
	}

    // Read from FIFO to fill output frame using the expected output duration
    int input_samples = ctx->in_frame->nb_samples;
    int available_samples = av_audio_fifo_size(ctx->fifo);
    int target_samples = input_samples;

    if (ctx->filter_initialized && fabsf(speed) > 0.0001f) {
        float expected = (float)input_samples / speed;
        target_samples = (int)(expected + 0.5f);
    }

    if (target_samples < 0) {
        target_samples = 0;
    }

    int samples_to_read = MIN(target_samples, available_samples);
	int fifo_after_push = available_samples;

	// Keep wrapper output bounded to the expected output duration for this input
	// chunk. FFmpeg can emit bursty output after runtime tempo changes, but
	// draining the whole FIFO here turns those bursts into audio-time jumps and
	// persistent A/V drift. Leave any extra samples buffered; the normal per-call
	// cadence will absorb them. The speed-change FIFO reset handles stale backlog.

    // If backlog grows significantly (e.g., during start-up), drain what we can
    if (available_samples > 0 && samples_to_read <= 0) {
        samples_to_read = available_samples;
    }

    if (samples_to_read > 0) {
        int bytes_needed = samples_to_read * bytes_per_sample;
        if (ctx->output_buffer_size < bytes_needed) {
            uint8_t *new_buffer = arealloc(ctx->output_buffer, bytes_needed);
            if (!new_buffer) {
                serprintf("atempo: failed to grow output buffer to %d bytes\n", bytes_needed);
                frame->size = 0;
                return -1;
            }
            ctx->output_buffer = new_buffer;
            ctx->output_buffer_size = bytes_needed;
        }

        void *data_ptrs[1] = { ctx->output_buffer };
        int read_samples = av_audio_fifo_read(ctx->fifo, data_ptrs, samples_to_read);
        if (read_samples > 0) {
            frame->data = ctx->output_buffer;
            frame->size = read_samples * bytes_per_sample;
        } else {
            frame->size = 0;
        }
    } else {
        // No output available yet (initial buffering)
        frame->size = 0;
    }

	int fifo_after_read = ctx->fifo ? av_audio_fifo_size(ctx->fifo) : -1;
	ctx->last_input_samples = input_samples;
	ctx->last_target_samples = target_samples;
	ctx->last_output_samples = samples_to_read;
	DBGA serprintf("atempo_flow: speed=%.3f in=%d target=%d drained_frames=%d drained_samples=%d fifo_before=%d fifo_after_push=%d out=%d fifo_after_read=%d\n",
		speed, input_samples, target_samples, drained_frames, drained_samples,
		fifo_before_push, fifo_after_push, samples_to_read, fifo_after_read);
	DBG serprintf("atempo: filter speed=%.3f in=%d samples out=%d samples fifo=%d\n",
		speed, ctx->in_frame->nb_samples, samples_to_read, fifo_after_read);

	return 0;
}

static int _flush(STREAM_FILTER_AUDIO *f)
{
	struct ctx *ctx = f->priv;
	DBGA serprintf("atempo: flush\n");

	if (ctx && ctx->fifo) {
		av_audio_fifo_reset(ctx->fifo);
	}

	if (ctx && ctx->filter_graph && ctx->abuffer_ctx && ctx->abuffersink_ctx) {
		// Flush filter graph
		(void)av_buffersrc_add_frame(ctx->abuffer_ctx, NULL);

		// Drain remaining frames
		AVFrame *frame = av_frame_alloc();
		if (frame) {
			while (av_buffersink_get_frame(ctx->abuffersink_ctx, frame) >= 0) {
				av_frame_unref(frame);
			}
			av_frame_free(&frame);
		}
	}

	return 0;
}

static int _delay(STREAM_FILTER_AUDIO *f)
{
	struct ctx *ctx = f->priv;

	if (!ctx) {
		return 0;
	}
	if (!ctx->enabled) {
		return 0;
	}

	// Total delay in milliseconds (in real-world time, not scaled by speed)
	int delay_ms = 0;
	int fifo_ms = 0;
	int atempo_internal_ms = 0;

	// 1. FIFO output buffer delay (samples already processed by atempo)
	if (ctx->fifo) {
		int fifo_samples = av_audio_fifo_size(ctx->fifo);
		// FIFO contains output samples (after speed change)
		// These represent real-world delay regardless of speed
		fifo_ms = (fifo_samples * 1000) / ctx->sample_rate;
		delay_ms += fifo_ms;
	}

	// 2. atempo filter internal delay (active even at 1.0x while filter is enabled)
	if (ctx->filter_initialized) {
		// atempo uses fragment size = sample_rate / 24 (rounded to power of 2)
		// Typical delay is 2-3 fragments due to WSOLA overlap-add algorithm
		int fragment_size = ctx->sample_rate / 24;
		// Round to nearest power of 2
		int power = 1;
		while (power < fragment_size) power <<= 1;
		fragment_size = power;

		// WSOLA delay: ~2.5 fragments (buffering + overlap region)
		// This is in INPUT domain, so scale by speed for real-world time
		int atempo_delay_samples = (fragment_size * 5) / 2;
		int atempo_delay_ms = (atempo_delay_samples * 1000) / ctx->sample_rate;

		// Scale by speed: at 1.5x, input delay is compressed to 2/3 real time
		atempo_internal_ms = (int)((float)atempo_delay_ms / ctx->current_speed);
		delay_ms += atempo_internal_ms;
	}

	// Limit downward delay jumps after a speed change (WSOLA needs time to stabilize).
	if (ctx->last_speed_change_ms > 0 && ctx->last_delay_ms >= 0) {
		int now_ms = atime();
		int elapsed_ms = now_ms - ctx->last_speed_change_ms;
		int min_stable_ms = atempo_internal_ms;
		if (fifo_ms > min_stable_ms) {
			min_stable_ms = fifo_ms;
		}
		if (min_stable_ms < 1) {
			min_stable_ms = 1;
		}
		if (elapsed_ms < min_stable_ms && delay_ms < ctx->last_delay_ms) {
			int max_drop = (ctx->last_delay_ms * elapsed_ms) / min_stable_ms;
			int floor = ctx->last_delay_ms - max_drop;
			if (delay_ms < floor) {
				delay_ms = floor;
			}
		}
		if (elapsed_ms >= min_stable_ms) {
			ctx->last_speed_change_ms = 0;
		}
	}

	{
		int emit_delay_log = 0;
		int delta_delay = (ctx->last_delay_ms >= 0) ? (delay_ms - ctx->last_delay_ms) : 0;
		int fifo_samples = ctx->fifo ? av_audio_fifo_size(ctx->fifo) : 0;
		int delta_fifo_ms = (ctx->last_fifo_ms >= 0) ? (fifo_ms - ctx->last_fifo_ms) : 0;
		int delta_fifo_samples = (ctx->last_fifo_samples >= 0) ? (fifo_samples - ctx->last_fifo_samples) : 0;
		int now_ms = atime();
		const char *reason = "steady";
		if (ctx->last_speed_change_ms > 0) {
			int elapsed_ms = now_ms - ctx->last_speed_change_ms;
			if (elapsed_ms >= 0 && elapsed_ms < 1500) {
				reason = "post_speed_change";
			}
		}
		if (Debug[DBG_AUD] > 2) {
			emit_delay_log = 1;
		} else if (Debug[DBG_AUD] > 1 && (ctx->delay_log_count % 100) == 0) {
			emit_delay_log = 1;
		}
		if (ABS(delta_delay) >= 16 || ABS(delta_fifo_ms) >= 16 || ABS(delta_fifo_samples) >= 512) {
			emit_delay_log = 1;
		}
		if (emit_delay_log) {
			serprintf("atempo: delay=%d ms (fifo_ms=%d, atempo_ms=%d, speed=%.2f)\n",
				delay_ms, fifo_ms, atempo_internal_ms, ctx->current_speed);
			serprintf("atempo_var: reason=%s speed=%.3f delay=%d->%d delta=%d fifo_ms=%d->%d delta=%d fifo_samples=%d->%d delta=%d in=%d target=%d out=%d\n",
				reason, ctx->current_speed, ctx->last_delay_ms, delay_ms, delta_delay,
				ctx->last_fifo_ms, fifo_ms, delta_fifo_ms,
				ctx->last_fifo_samples, fifo_samples, delta_fifo_samples,
				ctx->last_input_samples, ctx->last_target_samples, ctx->last_output_samples);
		}
		ctx->delay_log_count++;
		ctx->last_fifo_ms = fifo_ms;
		ctx->last_fifo_samples = fifo_samples;
	}

	ctx->last_delay_ms = delay_ms;
	return delay_ms;
}

static int _set_param(STREAM_FILTER_AUDIO *f, void *params, void *night_on)
{
	struct ctx *ctx = f->priv;

	if (!ctx) {
		return -1;
	}

	// Enable atempo filter when audio speed is enabled
	int should_enable = audio_interface_is_audio_speed_enabled();

	if (ctx->enabled != should_enable) {
		ctx->enabled = should_enable;
		DBGA serprintf("atempo: filter %s\n", should_enable ? "enabled" : "disabled");
	}

	return 0;
}

static int _delete(STREAM_FILTER_AUDIO *f)
{
	DBGA serprintf("atempo: delete\n");
	if (f && f->priv) {
		ctx_free(f->priv);
		f->priv = NULL;
	}
	if (f) {
		afree(f);
	}
	return 0;
}

STREAM_FILTER_AUDIO *stream_filter_audio_atempo_new(void)
{
	STREAM_FILTER_AUDIO *f = acalloc(1, sizeof(STREAM_FILTER_AUDIO));

	if (!f) {
		serprintf("atempo: failed to allocate filter structure\n");
		return NULL;
	}

	static char name[] = "atempo";
	f->name      = name;
	f->delete    = _delete;
	f->open      = _open;
	f->close     = _close;
	f->filter    = _filter;
	f->flush     = _flush;
	f->set_param = _set_param;
	f->delay     = _delay;

	DBGA serprintf("atempo: audio speed filter created\n");
	return f;
}

#else
// Stub implementation when FFmpeg is not available
STREAM_FILTER_AUDIO *stream_filter_audio_atempo_new(void)
{
	serprintf("atempo: filter not available (FFmpeg disabled)\n");
	return NULL;
}
#endif
